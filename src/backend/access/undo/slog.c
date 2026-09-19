/*-------------------------------------------------------------------------
 *
 * slog.c
 *	  Secondary Log (sLog) -- transaction Aborted Transaction Map (ATM)
 *
 * The sLog tracks aborted transactions in shared memory for the UNDO
 * subsystem's Constant-Time Recovery.
 *
 * Transaction sLog (adaptive radix tree):
 *   - A shared-memory adaptive radix tree (radixtree.h, RT_SHMEM) keyed by
 *     (xid, reloid) packed into a uint64 stores full abort metadata.  With
 *     xid in the high bits, ordered iteration groups all entries for a
 *     given xid contiguously, enabling efficient per-xid range operations.
 *   - The tree lives in the sLog's DSA area and grows on demand.
 *   - The radix tree is protected by a single LWLock (sLog modifications
 *     only occur on transaction abort, an uncommon path).
 *
 * An optional per-tuple flat-hash extension (see the tuple sLog, added by
 * the consumer that needs bounded-recovery uncommitted-writer tracking)
 * shares this file's shared-memory segment and initialization but is not
 * required by the UNDO core.
 *
 * Locking: Transaction sLog uses LWTRANCHE_SLOG.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/slog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#ifdef WIN32
#include <windows.h>
#endif

#include "access/slog.h"
#include "access/slog_internal.h"
#include "access/transam.h"
#include "access/xact.h"
#include "common/hashfn.h"
#include "nodes/lockoptions.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/off.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/subsystems.h"
#include "utils/dsa.h"
#include "utils/freepage.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

/* ----------------------------------------------------------------
 * Adaptive radix tree instantiation for the transaction sLog (ATM)
 *
 * The Aborted Transaction Map is keyed by a single uint64 formed from
 * (xid, reloid):  key = (xid << 32) | reloid.  Both are uint32 and the
 * two halves pack exactly.  Because xid occupies the high 32 bits, the
 * radix tree's ordered iteration groups all entries for one xid
 * contiguously (xid-major, reloid-minor ascending) -- the same ordering
 * property the old skip-list provided, which the per-xid range
 * operations below rely on.
 *
 * The tree lives in the sLog's DSA area so it grows on demand rather
 * than reserving a fixed shared-memory pool.  All access is serialized externally by the
 * existing txn_lock (LWLock): SET/DELETE under LW_EXCLUSIVE, FIND and
 * iteration under LW_SHARED.  We do not use the radix tree's own
 * internal lock (RT_LOCK_*), since txn_lock already provides the needed
 * serialization.
 * ----------------------------------------------------------------
 */

/*
 * slog_atm_value_t - value stored in the ATM radix tree.
 *
 * The key (xid, reloid) is implicit in the tree path, so only the data
 * fields are stored here.
 */
typedef struct slog_atm_value_t
{
	XLogRecPtr	last_batch_lsn; /* LSN of last UNDO batch for this xid */
	XLogRecPtr	stop_lsn;		/* chain walk boundary (see SLogTxnEntry) */
	XLogRecPtr	first_batch_lsn;	/* oldest batch of the chain */
	Oid			dboid;
	TimestampTz abort_time;
	bool		revert_complete;
	bool		fileops_owned;
} slog_atm_value_t;

#define RT_PREFIX slog_atm
#define RT_SCOPE static inline
#define RT_DECLARE
#define RT_DEFINE
#define RT_VALUE_TYPE slog_atm_value_t
#define RT_SHMEM
#define RT_USE_DELETE
#include "lib/radixtree.h"

/* ATM key encoding: xid in the high 32 bits, reloid in the low 32 bits. */
static inline uint64
slog_atm_key(TransactionId xid, Oid reloid)
{
	return ((uint64) xid << 32) | (uint64) reloid;
}

static inline TransactionId
slog_atm_key_xid(uint64 key)
{
	return (TransactionId) (key >> 32);
}

static inline Oid
slog_atm_key_reloid(uint64 key)
{
	return (Oid) (key & 0xFFFFFFFFu);
}

/*
 * Initial size for the sLog DSA area (backs the aborted-txn radix tree).
 * Grows dynamically as needed up to slog_dsa_max_size_mb.  SLOG_DSA_INIT_SIZE
 * and the shared-state struct are defined in access/slog_internal.h, shared
 * with the optional tuple sLog (slog_tuple.c).
 */
#define SLOG_DSA_MAX_SIZE_MB	256 /* default max: 256 MB */

/* GUC: maximum sLog DSA area size (in MB) */
int			slog_dsa_max_size_mb = SLOG_DSA_MAX_SIZE_MB;

/* ----------------------------------------------------------------
 * Static variables
 * ----------------------------------------------------------------
 */
SLogSharedState *SLogState = NULL;

/* Per-backend DSA attachment (lazy, via SLogEnsureDsaAttached) */
static dsa_area *slog_dsa_handle = NULL;

/* Per-backend attached ATM radix tree (lazy, via slog_atm_tree) */
static slog_atm_radix_tree * slog_atm_tree_local = NULL;


/*
 * slog_atm_tree
 *		Return this backend's attached ATM radix tree, attaching lazily.
 *
 * Ensures the sLog DSA area is attached, then attaches to the shared
 * radix tree via its handle.  The attached wrapper is cached for the
 * life of the backend (the control block lives in the pinned DSA area).
 * The caller must hold txn_lock across any use of the returned tree.
 */
static slog_atm_radix_tree *
slog_atm_tree(void)
{
	MemoryContext oldcxt;

	if (slog_atm_tree_local != NULL)
		return slog_atm_tree_local;

	SLogEnsureDsaAttached();
	if (slog_dsa_handle == NULL)
		elog(PANIC, "sLog: DSA not attached for ATM radix tree");

	/*
	 * Attach in TopMemoryContext so the wrapper persists across transactions,
	 * mirroring the DSA attach above.
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	slog_atm_tree_local = slog_atm_attach(slog_dsa_handle,
										  SLogState->atm_handle);
	MemoryContextSwitchTo(oldcxt);

	return slog_atm_tree_local;
}

/* ----------------------------------------------------------------
 * Shared memory sizing and initialization
 * ----------------------------------------------------------------
 */

/*
 * SLogShmemSize
 *		Calculate shared memory needed for the sLog.
 *
 * Note: The DSA initial region is embedded in SLogSharedState (dsa_space[]),
 * so sizeof(SLogSharedState) already includes SLOG_DSA_INIT_SIZE.  The ATM
 * radix tree is allocated from that DSA area on demand, so it needs no fixed
 * reservation here.
 */
Size
SLogShmemSize(void)
{
	return add_size(MAXALIGN(sizeof(SLogSharedState)), SLogTupleShmemSize());
}

/*
 * SLogShmemRequest
 *		Register shared memory needs for the sLog.
 *
 * Registers the shared state struct (which embeds the DSA initial region
 * backing the aborted-transaction radix tree).
 */
void
SLogShmemRequest(void)
{
	/* Register the shared state structure */
	ShmemRequestStruct(.name = "Secondary Log State",
					   .size = sizeof(SLogSharedState),
					   .ptr = (void **) &SLogState,
		);

	/* Optional tuple sLog: register its flat-hash partition block. */
	SLogTupleShmemRequest();
}

/*
 * SLogShmemInit
 *		Initialize sLog shared memory contents.
 *
 * Called during the init_fn phase.  The framework has already allocated
 * SLogState.  We initialize the transaction ATM radix tree and its DSA area.
 */
void
SLogShmemInit(void)
{
	/* ATM radix tree is created below, after the DSA area exists. */
	SLogState->atm_handle = InvalidDsaPointer;

	/* ---- Initialize locks ---- */
	LWLockInitialize(&SLogState->txn_lock.lock, LWTRANCHE_SLOG);

	/* ---- Optional tuple sLog: allocate + init the flat-hash partitions ---- */
	SLogTupleShmemInit();

	/* ---- Initialize DSA area (backs the aborted-txn radix tree) ---- */
	SLogState->dsa_area = dsa_create_in_place(SLogState->dsa_space,
											  SLOG_DSA_INIT_SIZE,
											  LWTRANCHE_SLOG,
											  0);
	dsa_pin(SLogState->dsa_area);
	dsa_set_size_limit(SLogState->dsa_area,
					   (Size) slog_dsa_max_size_mb * 1024 * 1024);

	/*
	 * Create the ATM radix tree in the DSA area and record its handle.  The
	 * transient tree wrapper returned here is discarded when init's memory
	 * context is reset; the control block lives in the pinned DSA area, and
	 * each backend re-attaches lazily via slog_atm_tree().  We reuse
	 * LWTRANCHE_SLOG for the tree's internal lock (unused -- txn_lock
	 * serializes all access -- but a valid tranche is required).
	 */
	{
		slog_atm_radix_tree *tree;

		tree = slog_atm_create(SLogState->dsa_area, LWTRANCHE_SLOG);
		SLogState->atm_handle = slog_atm_get_handle(tree);
	}

	dsa_detach(SLogState->dsa_area);
	SLogState->dsa_area = NULL; /* backends re-attach lazily */
}

/*
 * SLogShmemRequest_cb / SLogShmemInit_cb
 *		ShmemCallbacks adapters for SLogShmemRequest()/SLogShmemInit().
 *
 * The sLog is registered as its own PG_SHMEM_SUBSYSTEM entry (see
 * storage/subsystemlist.h) rather than being sized/initialized from within
 * the generic UNDO subsystem's callback, so the UNDO core has no
 * compile-time or link-time dependency on any consumer.
 *
 * No .attach_fn is provided: SLogShmemInit() has no found-guard, so the
 * framework re-attaches EXEC_BACKEND children to the already-initialized
 * shared struct via the ShmemRequestStruct(.ptr=&SLogState) registration
 * in SLogShmemRequest().
 */
static void
SLogShmemRequest_cb(void *arg)
{
	SLogShmemRequest();
}

static void
SLogShmemInit_cb(void *arg)
{
	SLogShmemInit();
}

const ShmemCallbacks SLogShmemCallbacks = {
	.request_fn = SLogShmemRequest_cb,
	.init_fn = SLogShmemInit_cb,
};

/* ----------------------------------------------------------------
 * DSA lifecycle for the shared sLog DSA area
 * ----------------------------------------------------------------
 */

/*
 * SLogEnsureDsaAttached
 *		Lazy per-backend DSA attachment.
 *
 * Must be called before any DSA alloc/free/get_address operations.
 * Safe to call multiple times (no-op after first attach).
 */
void
SLogEnsureDsaAttached(void)
{
	MemoryContext oldcxt;

	if (slog_dsa_handle != NULL)
		return;					/* already attached */

	if (SLogState == NULL)
		return;					/* sLog not yet initialized */

	/*
	 * Allocate in TopMemoryContext so the dsa_area struct persists across
	 * transactions.  dsa_attach_in_place internally palloc's, so if we're in
	 * a transaction context, the handle would become a dangling pointer after
	 * transaction end.
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	slog_dsa_handle = dsa_attach_in_place(SLogState->dsa_space, NULL);
	dsa_pin_mapping(slog_dsa_handle);
	MemoryContextSwitchTo(oldcxt);
}

/* ================================================================
 * Transaction sLog functions
 * ================================================================
 */

/*
 * Free DSA space one new ATM entry may need: a fresh superblock (16 pages,
 * DSA_PAGES_PER_SUPERBLOCK in dsa.c) for each size class the insert allocates
 * from -- the node4s of a new path, one grown node, the leaf, and the span
 * descriptors -- plus one for slack.
 */
#define SLOG_ATM_INSERT_RESERVE (5 * 16 * FPM_PAGE_SIZE)

/*
 * SLogTxnInsert
 *		Insert an aborted transaction entry into the sLog.
 *
 * Creates an entry in the ATM radix tree, which grows on demand from the DSA
 * area up to slog_dsa_max_size_mb.  The tree's allocations raise ERROR when
 * the area is full, and an ERROR midway through an insert leaves the tree
 * corrupt.  With check_space, the caller is aborting a transaction and must
 * not fail: first make sure the area has room for the insert, and return
 * false if it does not.  Only this tree allocates from the area, and txn_lock
 * serializes it, so the room cannot be taken in between.  Without
 * check_space (redo, and restoring entries at startup) the insert raises
 * ERROR when the area is full, failing startup rather than dropping a
 * rollback.
 */
bool
SLogTxnInsert(TransactionId xid, Oid reloid, Oid dboid,
			  XLogRecPtr last_batch_lsn, XLogRecPtr stop_lsn,
			  XLogRecPtr first_batch_lsn, bool fileops_owned,
			  bool check_space)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	uint64		key = slog_atm_key(xid, reloid);
	slog_atm_value_t value;
	slog_atm_value_t *found;

	value.last_batch_lsn = last_batch_lsn;
	value.stop_lsn = stop_lsn;
	value.first_batch_lsn = first_batch_lsn;
	value.fileops_owned = fileops_owned;
	value.dboid = dboid;
	value.abort_time = GetCurrentTimestamp();
	value.revert_complete = false;

	LWLockAcquire(&SLogState->txn_lock.lock, LW_EXCLUSIVE);

	/*
	 * Keep the original chain and progress on duplicate insertion, but take
	 * the latest FILEOPS ownership.  Recovery may supersede an online insert
	 * made before an abort record that never reached WAL, or mark the
	 * inverses complete after applying them itself.
	 */
	found = slog_atm_find(tree, key);
	if (found != NULL)
		found->fileops_owned = fileops_owned;
	else
	{
		if (check_space)
		{
			dsa_pointer reserve;

			/*
			 * Freeing a large allocation returns its pages, contiguous, to
			 * the free page manager, where the insert's new superblocks find
			 * them.
			 */
			reserve = dsa_allocate_extended(slog_dsa_handle,
											SLOG_ATM_INSERT_RESERVE,
											DSA_ALLOC_NO_OOM);
			if (!DsaPointerIsValid(reserve))
			{
				LWLockRelease(&SLogState->txn_lock.lock);
				return false;
			}
			dsa_free(slog_dsa_handle, reserve);
		}
		(void) slog_atm_set(tree, key, &value);
	}

	LWLockRelease(&SLogState->txn_lock.lock);
	return true;
}

/*
 * SLogTxnLookup
 *		Look up a specific (xid, reloid) entry.
 *
 * Returns true if found, copying the entry into *entry_out.
 */
bool
SLogTxnLookup(TransactionId xid, Oid reloid, SLogTxnEntry *entry_out)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	uint64		key = slog_atm_key(xid, reloid);
	slog_atm_value_t *found;

	LWLockAcquire(&SLogState->txn_lock.lock, LW_SHARED);

	found = slog_atm_find(tree, key);

	if (found != NULL && entry_out != NULL)
	{
		entry_out->xid = xid;
		entry_out->reloid = reloid;
		entry_out->last_batch_lsn = found->last_batch_lsn;
		entry_out->stop_lsn = found->stop_lsn;
		entry_out->first_batch_lsn = found->first_batch_lsn;
		entry_out->fileops_owned = found->fileops_owned;
		entry_out->dboid = found->dboid;
		entry_out->abort_time = found->abort_time;
		entry_out->revert_complete = found->revert_complete;
	}

	LWLockRelease(&SLogState->txn_lock.lock);

	return (found != NULL);
}

/*
 * SLogTxnLookupByXid
 *		Find the UNDO chain for a given xid (any reloid).
 *
 * The radix tree has no seek-to-key API, so we iterate its entries in
 * ascending key order (xid-major) and return the first one whose xid
 * matches.  Because keys are xid-major we can stop as soon as we pass
 * the target xid.  The ATM only holds un-reverted aborted transactions,
 * so it is small and a full scan is acceptable on this cold path.
 */
bool
SLogTxnLookupByXid(TransactionId xid, XLogRecPtr *lsn_out)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	bool		result = false;

	LWLockAcquire(&SLogState->txn_lock.lock, LW_SHARED);

	iter = slog_atm_begin_iterate(tree);
	while ((val = slog_atm_iterate_next(iter, &k)) != NULL)
	{
		TransactionId k_xid = slog_atm_key_xid(k);

		if (k_xid < xid)
			continue;
		if (k_xid > xid)
			break;				/* passed the target range */

		if (lsn_out)
			*lsn_out = val->last_batch_lsn;
		result = true;
		break;
	}
	slog_atm_end_iterate(iter);

	LWLockRelease(&SLogState->txn_lock.lock);
	return result;
}

/*
 * SLogTxnRemove
 *		Remove a specific (xid, reloid) entry.
 */
void
SLogTxnRemove(TransactionId xid, Oid reloid)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	uint64		key = slog_atm_key(xid, reloid);

	LWLockAcquire(&SLogState->txn_lock.lock, LW_EXCLUSIVE);

	(void) slog_atm_delete(tree, key);

	LWLockRelease(&SLogState->txn_lock.lock);
}

/*
 * SLogTxnRemoveByXid
 *		Remove all sLog entries for a given transaction ID.
 *
 * Collects all keys for this xid into a local array during an ascending
 * iteration (keys are contiguous thanks to the xid-major ordering), then
 * deletes them after ending the iteration -- we do not delete while
 * iterating.  The ATM is small, so the full scan is acceptable.
 */
void
SLogTxnRemoveByXid(TransactionId xid)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	uint64	   *to_remove;
	int			nremove = 0;
	int			max_remove = 64;
	int			i;

	to_remove = (uint64 *) palloc(max_remove * sizeof(uint64));

	LWLockAcquire(&SLogState->txn_lock.lock, LW_EXCLUSIVE);

	/* Collect all keys with matching xid */
	iter = slog_atm_begin_iterate(tree);
	while ((val = slog_atm_iterate_next(iter, &k)) != NULL)
	{
		TransactionId k_xid = slog_atm_key_xid(k);

		(void) val;
		if (k_xid < xid)
			continue;
		if (k_xid > xid)
			break;

		if (nremove >= max_remove)
		{
			max_remove *= 2;
			to_remove = (uint64 *) repalloc(to_remove,
											max_remove * sizeof(uint64));
		}
		to_remove[nremove++] = k;
	}
	slog_atm_end_iterate(iter);

	/* Remove collected keys */
	for (i = 0; i < nremove; i++)
		(void) slog_atm_delete(tree, to_remove[i]);

	LWLockRelease(&SLogState->txn_lock.lock);

	pfree(to_remove);
}

/*
 * SLogTxnMarkReverted
 *		Mark all entries for a given xid as revert_complete.
 *
 * Iterates the tree in ascending key order and rewrites the value of each
 * entry for this xid with revert_complete = true.  Keys for one xid are
 * contiguous, so we stop once we pass the target xid.  We collect the
 * matching (key, value) pairs first and RT_SET them after ending the
 * iteration, so the tree is never mutated while an iterator is live.
 */
void
SLogTxnMarkReverted(TransactionId xid)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	uint64	   *keys;
	slog_atm_value_t *vals;
	int			n = 0;
	int			max_n = 64;
	int			i;

	keys = (uint64 *) palloc(max_n * sizeof(uint64));
	vals = (slog_atm_value_t *) palloc(max_n * sizeof(slog_atm_value_t));

	LWLockAcquire(&SLogState->txn_lock.lock, LW_EXCLUSIVE);

	/*
	 * Collect matching entries first, then RT_SET them after ending the
	 * iteration, to avoid mutating the tree while an iterator is live.
	 */
	iter = slog_atm_begin_iterate(tree);
	while ((val = slog_atm_iterate_next(iter, &k)) != NULL)
	{
		TransactionId k_xid = slog_atm_key_xid(k);

		if (k_xid < xid)
			continue;
		if (k_xid > xid)
			break;

		if (n >= max_n)
		{
			max_n *= 2;
			keys = (uint64 *) repalloc(keys, max_n * sizeof(uint64));
			vals = (slog_atm_value_t *) repalloc(vals,
												 max_n * sizeof(slog_atm_value_t));
		}
		keys[n] = k;
		vals[n] = *val;
		vals[n].revert_complete = true;
		n++;
	}
	slog_atm_end_iterate(iter);

	for (i = 0; i < n; i++)
		(void) slog_atm_set(tree, keys[i], &vals[i]);

	LWLockRelease(&SLogState->txn_lock.lock);

	pfree(keys);
	pfree(vals);
}

/*
 * SLogTxnGetNextUnreverted
 *		Find an entry of database dboid that hasn't been reverted yet.
 *
 * Iterates the tree in ascending key order (xid-major), returning the
 * first entry of dboid after after_xid with revert_complete == false.
 * Pass InvalidTransactionId to start from the beginning.
 */
bool
SLogTxnGetNextUnreverted(Oid dboid, TransactionId after_xid,
						 TransactionId *xid_out,
						 XLogRecPtr *lsn_out, XLogRecPtr *stop_lsn_out)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	bool		result = false;

	LWLockAcquire(&SLogState->txn_lock.lock, LW_SHARED);

	iter = slog_atm_begin_iterate(tree);
	while ((val = slog_atm_iterate_next(iter, &k)) != NULL)
	{
		if (slog_atm_key_xid(k) <= after_xid || val->dboid != dboid)
			continue;
		if (!val->revert_complete)
		{
			*xid_out = slog_atm_key_xid(k);
			*lsn_out = val->last_batch_lsn;
			*stop_lsn_out = val->stop_lsn;
			result = true;
			break;
		}
	}
	slog_atm_end_iterate(iter);

	LWLockRelease(&SLogState->txn_lock.lock);
	return result;
}

/*
 * SLogTxnCollectUnrevertedDatabases
 *		Collect the distinct database OIDs that have at least one unreverted
 *		ATM entry.
 *
 * Fills dboids[] (caller-provided, capacity max_dboids) with the distinct
 * OIDs and returns the count.  Used by the logical revert launcher so it
 * spawns a per-database worker only for databases that actually have
 * aborted-transaction UNDO to apply, rather than for every database in the
 * cluster.  The set of databases with unreverted entries is tiny, so a
 * linear dedup against the collected prefix is adequate.
 */
int
SLogTxnCollectUnrevertedDatabases(Oid *dboids, int max_dboids)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	int			n = 0;

	LWLockAcquire(&SLogState->txn_lock.lock, LW_SHARED);

	iter = slog_atm_begin_iterate(tree);
	while ((val = slog_atm_iterate_next(iter, &k)) != NULL)
	{
		int			i;
		bool		seen = false;

		if (val->revert_complete)
			continue;

		for (i = 0; i < n; i++)
		{
			if (dboids[i] == val->dboid)
			{
				seen = true;
				break;
			}
		}
		if (!seen && n < max_dboids)
			dboids[n++] = val->dboid;
	}
	slog_atm_end_iterate(iter);

	LWLockRelease(&SLogState->txn_lock.lock);
	return n;
}

/*
 * SLogRecoveryFinalize
 *		Count entries after recovery for logging.
 */
void
SLogRecoveryFinalize(int *total_out, int *unreverted_out)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	int			total = 0;
	int			unreverted = 0;

	LWLockAcquire(&SLogState->txn_lock.lock, LW_SHARED);

	iter = slog_atm_begin_iterate(tree);
	while ((val = slog_atm_iterate_next(iter, &k)) != NULL)
	{
		total++;
		if (!val->revert_complete)
			unreverted++;
	}
	slog_atm_end_iterate(iter);

	LWLockRelease(&SLogState->txn_lock.lock);

	if (total_out)
		*total_out = total;
	if (unreverted_out)
		*unreverted_out = unreverted;
}

/*
 * SLogTxnGetOldestUnrevertedLSN
 *		Return the oldest UNDO WAL position any unreverted entry still needs:
 *		the minimum over entries of their chain's first batch LSN.
 *
 * Used by the WAL retention logic to prevent recycling WAL segments that
 * still contain UNDO batches needed by the logical revert worker.
 * Returns InvalidXLogRecPtr if no unreverted entries exist.
 */
XLogRecPtr
SLogTxnGetOldestUnrevertedLSN(void)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	XLogRecPtr	oldest = InvalidXLogRecPtr;

	LWLockAcquire(&SLogState->txn_lock.lock, LW_SHARED);

	iter = slog_atm_begin_iterate(tree);
	while ((val = slog_atm_iterate_next(iter, &k)) != NULL)
	{
		/*
		 * The revert worker reads the whole chain, back to its oldest batch,
		 * so that is what must stay; the head alone would let checkpoints
		 * recycle the older batches first.  An entry without a known first
		 * batch falls back to its head.
		 */
		XLogRecPtr	lsn = XLogRecPtrIsValid(val->first_batch_lsn) ?
			val->first_batch_lsn : val->last_batch_lsn;

		if (!val->revert_complete && XLogRecPtrIsValid(lsn))
		{
			if (!XLogRecPtrIsValid(oldest) || lsn < oldest)
				oldest = lsn;
		}
	}
	slog_atm_end_iterate(iter);

	LWLockRelease(&SLogState->txn_lock.lock);
	return oldest;
}

/*
 * SLogTxnSnapshotForCheckpoint
 *		Copy every ATM entry into a palloc'd array for durable checkpointing.
 *
 * Returns the number of entries and stores a palloc'd array of SLogTxnEntry
 * in *entries_out (NULL if there are none).  The caller owns the array and
 * must pfree it.  Runs outside any critical section (CheckPointATM), so
 * palloc is safe.
 */
int
SLogTxnSnapshotForCheckpoint(SLogTxnEntry **entries_out)
{
	slog_atm_radix_tree *tree = slog_atm_tree();
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	SLogTxnEntry *arr = NULL;
	int			count = 0;
	int			capacity = 64;

	*entries_out = NULL;

	arr = (SLogTxnEntry *) palloc(sizeof(SLogTxnEntry) * capacity);

	LWLockAcquire(&SLogState->txn_lock.lock, LW_SHARED);

	iter = slog_atm_begin_iterate(tree);
	while ((val = slog_atm_iterate_next(iter, &k)) != NULL)
	{
		if (count >= capacity)
		{
			capacity *= 2;
			arr = (SLogTxnEntry *) repalloc(arr,
											sizeof(SLogTxnEntry) * capacity);
		}

		arr[count].xid = slog_atm_key_xid(k);
		arr[count].reloid = slog_atm_key_reloid(k);
		arr[count].last_batch_lsn = val->last_batch_lsn;
		arr[count].stop_lsn = val->stop_lsn;
		arr[count].first_batch_lsn = val->first_batch_lsn;
		arr[count].fileops_owned = val->fileops_owned;
		arr[count].dboid = val->dboid;
		arr[count].abort_time = val->abort_time;
		arr[count].revert_complete = val->revert_complete;
		count++;
	}
	slog_atm_end_iterate(iter);

	LWLockRelease(&SLogState->txn_lock.lock);

	if (count == 0)
	{
		pfree(arr);
		return 0;
	}

	*entries_out = arr;
	return count;
}
