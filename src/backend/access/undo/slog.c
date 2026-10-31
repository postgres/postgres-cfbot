/*-------------------------------------------------------------------------
 *
 * slog.c
 *	  Secondary Log (sLog) -- the transaction sLog, which holds the
 *	  Aborted Transaction Map (ATM)
 *
 * The sLog is the UNDO subsystem's shared state that is not WAL.  It has
 * two parts:
 *
 *   - The transaction sLog (this file): the aborted transactions whose UNDO
 *     has not been applied yet (the ATM, atm.c), which the logical revert
 *     worker reverts and which checkpoints and redo carry across a crash
 *     (RM_ATM_ID records).
 *   - The tuple sLog (slog_tuple.c): per-tuple state of UNDO-based table
 *     AMs -- who holds row locks on their tuples (they record no locker in
 *     the tuple), and which tuples this transaction inserted.
 *
 * Transaction sLog (adaptive radix tree):
 *   - A shared-memory adaptive radix tree (radixtree.h, RT_SHMEM) keyed by
 *     (xid, reloid) packed into a uint64 stores full abort metadata.  With
 *     xid in the high bits, ordered iteration groups all entries for a
 *     given xid contiguously, enabling efficient per-xid range operations.
 *   - The tree lives in a DSA area of its own, created by the first
 *     transaction that writes UNDO (SLogTxnEnsureCreated), and grows on
 *     demand.  Nothing but the lock and the tree's handle is
 *     allocated at startup.  Until it exists, the ATM is empty: readers say
 *     so without creating it.
 *   - The radix tree is protected by a single LWLock (sLog modifications
 *     only occur on transaction abort, an uncommon path).
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
 * Maximum size of the ATM's DSA area.  The shared-state struct is defined in
 * access/slog_internal.h.
 */
#define SLOG_DSA_MAX_SIZE_MB	256 /* default max: 256 MB */

/* GUC: maximum sLog DSA area size (in MB) */
int			slog_dsa_max_size_mb = SLOG_DSA_MAX_SIZE_MB;

/* ----------------------------------------------------------------
 * Static variables
 * ----------------------------------------------------------------
 */
SLogSharedState *SLogState = NULL;

/* Per-backend attachment of the ATM's DSA area (lazy, via slog_atm_tree) */
static dsa_area *slog_dsa_handle = NULL;

/* Per-backend attached ATM radix tree (lazy, via slog_atm_tree) */
static slog_atm_radix_tree * slog_atm_tree_local = NULL;


/*
 * slog_atm_tree
 *		Return this backend's attached ATM radix tree, attaching lazily.
 *
 * Returns NULL if the tree was never created and create is false; readers
 * then treat the ATM as empty.  With create, the first caller in the cluster
 * creates the DSA area and the tree, which raises ERROR on failure: callers
 * that must not fail (an aborting transaction) pass false, and their
 * transaction created the tree when it first wrote UNDO
 * (SLogTxnEnsureCreated).  Once created the area is pinned and lasts until
 * shared memory is reinitialized.  The attached wrapper is cached for the
 * life of the backend.  The caller must hold txn_lock across any use of the
 * returned tree, and must not hold it here.
 */
static slog_atm_radix_tree *
slog_atm_tree(bool create)
{
	MemoryContext oldcxt;
	bool		created = false;

	if (slog_atm_tree_local != NULL)
		return slog_atm_tree_local;

	LWLockAcquire(&SLogState->txn_lock.lock, create ? LW_EXCLUSIVE : LW_SHARED);
	if (!DsaPointerIsValid(SLogState->atm_handle) && !create)
	{
		LWLockRelease(&SLogState->txn_lock.lock);
		return NULL;
	}

	/*
	 * Keep the attachment and the wrapper across transactions.  The area is
	 * remembered only once its mapping is pinned, so that an ERROR in between
	 * cannot leave a dangling pointer behind.  The smallest first segment
	 * will do: an entry is a few dozen bytes, and the area grows as needed.
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	if (slog_dsa_handle == NULL)
	{
		dsa_area   *area;

		if (SLogState->area != DSA_HANDLE_INVALID)
		{
			area = dsa_attach(SLogState->area);
			dsa_pin_mapping(area);
		}
		else
		{
			area = dsa_create_ext(LWTRANCHE_SLOG, DSA_MIN_SEGMENT_SIZE,
								  DSA_MAX_SEGMENT_SIZE);
			dsa_pin_mapping(area);
			dsa_pin(area);
			dsa_set_size_limit(area,
							   (Size) slog_dsa_max_size_mb * 1024 * 1024);
			SLogState->area = dsa_get_handle(area);
		}
		slog_dsa_handle = area;
	}
	if (!DsaPointerIsValid(SLogState->atm_handle))
	{
		slog_atm_radix_tree *tree;

		tree = slog_atm_create(slog_dsa_handle, LWTRANCHE_SLOG);
		SLogState->atm_handle = slog_atm_get_handle(tree);
		slog_atm_tree_local = tree;
		created = true;
	}
	else
		slog_atm_tree_local = slog_atm_attach(slog_dsa_handle,
											  SLogState->atm_handle);
	MemoryContextSwitchTo(oldcxt);
	LWLockRelease(&SLogState->txn_lock.lock);

	if (created)
		elog(DEBUG1, "created the aborted transaction map");

	return slog_atm_tree_local;
}

/*
 * SLogTxnEnsureCreated
 *		Create the ATM now, if nobody has yet, so that this transaction can
 *		abort into it without allocating.
 *
 * Called when a transaction first writes UNDO, and by ROLLBACK PREPARED,
 * outside any critical section: a failure here is an ordinary ERROR, before
 * anything that would need rolling back exists.
 */
void
SLogTxnEnsureCreated(void)
{
	(void) slog_atm_tree(true);
}

/* ----------------------------------------------------------------
 * Shared memory sizing and initialization
 * ----------------------------------------------------------------
 */

/*
 * SLogShmemSize
 *		Calculate shared memory needed for the sLog.
 *
 * Only the lock and the tree's handle: the tree and its DSA area are
 * created on first use (slog_atm_tree).
 */
Size
SLogShmemSize(void)
{
	return MAXALIGN(sizeof(SLogSharedState));
}

/*
 * SLogShmemRequest
 *		Register shared memory needs for the sLog.
 *
 * Registers the shared state struct.
 */
void
SLogShmemRequest(void)
{
	/* Register the shared state structure */
	ShmemRequestStruct(.name = "Secondary Log State",
					   .size = sizeof(SLogSharedState),
					   .ptr = (void **) &SLogState,
		);
}

/*
 * SLogShmemInit
 *		Initialize sLog shared memory contents.
 *
 * Called during the init_fn phase.  The framework has already allocated
 * SLogState.  The ATM's DSA area and tree do not exist yet.
 */
void
SLogShmemInit(void)
{
	LWLockInitialize(&SLogState->txn_lock.lock, LWTRANCHE_SLOG);
	SLogState->area = DSA_HANDLE_INVALID;
	SLogState->atm_handle = InvalidDsaPointer;
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
	slog_atm_radix_tree *tree = slog_atm_tree(!check_space);
	uint64		key = slog_atm_key(xid, reloid);
	slog_atm_value_t value;
	slog_atm_value_t *found;

	/* Aborting, and the ATM was never created: report it as full. */
	if (tree == NULL)
		return false;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	uint64		key = slog_atm_key(xid, reloid);
	slog_atm_value_t *found;

	/* never created: the ATM is empty */
	if (tree == NULL)
		return false;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	bool		result = false;

	/* never created: the ATM is empty */
	if (tree == NULL)
		return false;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	uint64		key = slog_atm_key(xid, reloid);

	/* never created: the ATM is empty */
	if (tree == NULL)
		return;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	uint64	   *to_remove;
	int			nremove = 0;
	int			max_remove = 64;
	int			i;

	/* never created: the ATM is empty */
	if (tree == NULL)
		return;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	uint64	   *keys;
	slog_atm_value_t *vals;
	int			n = 0;
	int			max_n = 64;
	int			i;

	/* never created: the ATM is empty */
	if (tree == NULL)
		return;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	bool		result = false;

	/* never created: the ATM is empty */
	if (tree == NULL)
		return false;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	int			n = 0;

	/* never created: the ATM is empty */
	if (tree == NULL)
		return 0;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	int			total = 0;
	int			unreverted = 0;

	/* never created: the ATM is empty */
	if (tree == NULL)
	{
		if (total_out)
			*total_out = 0;
		if (unreverted_out)
			*unreverted_out = 0;
		return;
	}

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	XLogRecPtr	oldest = InvalidXLogRecPtr;

	/* never created: the ATM is empty */
	if (tree == NULL)
		return InvalidXLogRecPtr;

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
	slog_atm_radix_tree *tree = slog_atm_tree(false);
	slog_atm_iter *iter;
	slog_atm_value_t *val;
	uint64		k;
	SLogTxnEntry *arr = NULL;
	int			count = 0;
	int			capacity = 64;

	*entries_out = NULL;

	/* never created: the ATM is empty */
	if (tree == NULL)
		return 0;

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
