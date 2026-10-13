/*-------------------------------------------------------------------------
 *
 * slog_tuple.c
 *	  Tuple sLog -- per-tuple state of UNDO-based table access methods
 *
 * An UNDO-based table AM updates rows in place and judges a version by the
 * writer xids in its header and CLOG, as heap does, so it needs no shared
 * per-tuple record of who wrote what.  It keeps two other things here, keyed
 * by tuple (relation OID, relation storage, TID), without reference to any
 * AM's page format:
 *
 * Row locks (SLogTupleLock*).  Such an AM records no row locker in the tuple
 * (no locker xmax, no MultiXact), so the holder and strength of each row
 * lock live here.  Nothing is allocated at startup.  The first row-lock call
 * in the cluster creates, through the DSM registry, a directory with one
 * slot per PGPROC (so also one per prepared transaction's dummy proc).  A
 * slot keeps the first few TIDs its transaction locks in records of its own,
 * and any more in one hash table per relation storage, in a DSA that is
 * created by the first such lock and grows as far as needed.  So a
 * transaction that locks a few rows touches neither the shared allocator
 * nor any LWLock of the store: other backends read its records under a
 * change counter, and take the slot's LWLock (shared; the owner changes a
 * table under it exclusive) only when the slot has tables.  Only the owner
 * writes its slot and tables, and it reads them without the lock.  A table
 * is one allocation, and a slot keeps its small emptied tables for its next
 * transactions, so a transaction that locks a few hundred rows usually
 * makes no allocation either.
 *
 * A conflict probe does not visit every holder: a shared index, created
 * with the directory, maps a hash of each locked TID to the set of slots
 * that may hold a lock on it, and the probe visits only those.
 *
 * A TID's entries are in one place: its record, or its table.  In a table
 * the key is the TID, with a small "depth" in the high bits:
 * (depth << 48) | (block << 16) | offset; a record keeps the same entries
 * in an array.  A TID usually has one entry, at
 * depth 0.  Its value is the lock strength and the xid of the subtransaction
 * that took it, which is what a conflicting waiter sleeps on, so that
 * ROLLBACK TO wakes it, as with heap's xmax.  A transaction keeps one entry
 * per subtransaction level only while the inner lock is stronger: after
 * SAVEPOINT s; FOR UPDATE; ROLLBACK TO s the outer FOR KEY SHARE must still
 * be there.  Strengths strictly increase with depth, so a TID has at most
 * four entries.
 *
 * Nothing is done at ROLLBACK TO or at subtransaction commit: an entry whose
 * subtransaction aborted is dead because its xid is neither current (for the
 * owner) nor in progress (for everyone else).  The AM calls
 * SLogTupleLockReleaseAll at top-level commit or abort, which empties the
 * slot and frees or empties its tables.  SLogTupleLockAtPrepare moves them to the
 * prepared transaction's dummy-proc slot and SLogTupleLockReleasePrepared
 * frees them at COMMIT/ROLLBACK PREPARED; the AM logs the live locks in its
 * own two-phase records and restores them at the end of recovery
 * (SLogTupleLockRestore).
 *
 * Inserted tuples (SLogTupleTrackLocalOnly and friends), backend-locally:
 * the tuples this transaction inserted, so that commit can clear their
 * "uncommitted" hint and so that savepoint rollback can say which of them
 * were rolled back (SLogTupleCollectTrackedKeys).  Nothing is allocated
 * until a transaction inserts a row, and all of it goes at transaction end.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/slog_tuple.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/slog.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include "miscadmin.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/off.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/dsa.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

static void slog_tl_forget_storage(Oid relid, RelFileNumber relnumber);

/* ----------------------------------------------------------------
 * Backend-private tracking of inserted tuples
 * ----------------------------------------------------------------
 */
typedef struct SLogTrackedKey
{
	SLogTupleKey key;
	RelFileNumber relnumber;	/* the relation's storage the row went to */
	TransactionId xid;
	TransactionId subxid;
	SLogOpType	op_type;		/* INSERT, or ABORTED once its savepoint is */

	struct SLogTrackedKey *next;
} SLogTrackedKey;

static SLogTrackedKey *slog_tracked_keys = NULL;

/* ----------------------------------------------------------------
 * TID encoding for backend-local INSERT tracking
 *
 * Each (blkno, offnum) pair maps to a dense 64-bit key used by the
 * backend-local INSERT-tracking hash (see below):
 *
 *   blkno * MaxOffsetNumber + (offnum - 1)
 *
 * Losslessness invariant: this key is in-memory and per-backend only --
 * never written to WAL or disk -- so the divisor need only be strictly
 * greater than any real offnum a tracked page can hold.  MaxOffsetNumber
 * is the maximum number of line pointers any page can hold and is an
 * AM-agnostic upper bound: offnum <= MaxOffsetNumber always holds, so the
 * divisor never aliases two pages' TIDs onto the same key.  A check at each
 * encode site enforces this; an out-of-range offset is a corruption bug,
 * not a recoverable condition, so it raises ERROR even in production builds.
 * ----------------------------------------------------------------
 */
#define SLOG_ENCODE_TID(blkno, offnum) \
	((uint64) (blkno) * MaxOffsetNumber + (uint64) ((offnum) - 1))
#define SLOG_DECODE_BLKNO(encoded) \
	((BlockNumber) ((encoded) / MaxOffsetNumber))
#define SLOG_DECODE_OFFNUM(encoded) \
	((OffsetNumber) ((encoded) % MaxOffsetNumber + 1))

/* ----------------------------------------------------------------
 * Backend-local INSERT tracking (simplehash)
 *
 * Top-level local-only INSERTs are recorded in a single backend-local
 * open-addressing hash keyed by (relid, encoded_tid).  This replaces the
 * former per-relid linked-list-of-sparsemaps, which forced an O(n) chunk
 * scan on every per-tuple visibility probe.  The hash lives in
 * TopTransactionContext and is destroyed per transaction.  No locks: it is
 * strictly backend-private.
 * ----------------------------------------------------------------
 */
typedef struct SLogInsertTidKey
{
	Oid			relid;
	RelFileNumber relnumber;	/* the relation's storage the row went to */
	uint64		encoded_tid;	/* SLOG_ENCODE_TID(blkno, offnum) */
} SLogInsertTidKey;

typedef struct SLogInsertTidEntry
{
	SLogInsertTidKey key;		/* (relid, relnumber, encoded_tid) */
	char		status;			/* required by simplehash */
} SLogInsertTidEntry;

static inline uint32
slog_insert_tid_hash(const SLogInsertTidKey *key)
{
	uint64		h = hash_combine64((uint64) hash_uint32((uint32) key->relid),
								   murmurhash64(key->encoded_tid));

	h = hash_combine64(h, (uint64) hash_uint32((uint32) key->relnumber));
	return (uint32) (h ^ (h >> 32));
}

#define SH_PREFIX sloginsert
#define SH_ELEMENT_TYPE SLogInsertTidEntry
#define SH_KEY_TYPE SLogInsertTidKey
#define SH_KEY key
#define SH_HASH_KEY(tb, key) slog_insert_tid_hash(&(key))
#define SH_EQUAL(tb, a, b) ((a).relid == (b).relid && \
							(a).relnumber == (b).relnumber && \
							(a).encoded_tid == (b).encoded_tid)
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

static sloginsert_hash * slog_insert_tids = NULL;

/*
 * Lazily create the backend-local INSERT hash in TopTransactionContext.
 * Returns the (now non-NULL) table.
 */
static inline sloginsert_hash *
slog_insert_tids_ensure(void)
{
	if (slog_insert_tids == NULL)
		slog_insert_tids = sloginsert_create(TopTransactionContext, 256, NULL);
	return slog_insert_tids;
}

/*
 * SLogTupleTrackLocalOnly
 *		Lightweight local-only tracking (INSERTs only).
 *
 * Records (relid, relnumber, tid, xid, subxid) in backend-local memory.
 *
 * Outside a subtransaction the TID goes into a backend-local open-addressing
 * hash (simplehash) keyed by (relid, relnumber, encoded_tid), which keeps
 * bulk INSERT at a few bytes per row.  Inside one, it goes on a linked list,
 * as subtransaction abort needs per-entry subxid filtering.
 */
void
SLogTupleTrackLocalOnly(Oid relid, RelFileNumber relnumber, ItemPointer tid,
						TransactionId xid, TransactionId subxid)
{
	MemoryContext oldcxt;

	/*
	 * Fast path: top-level transaction with no savepoint -> use the hash. The
	 * subxid is InvalidTransactionId in this case (top-level INSERTs always
	 * pass the top xid as both xid and subxid=Invalid).
	 */
	if (!IsSubTransaction())
	{
		BlockNumber blkno = ItemPointerGetBlockNumber(tid);
		OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
		SLogInsertTidKey ikey;
		bool		found;

		if (offnum > MaxOffsetNumber)
			elog(ERROR, "offset %u exceeds page item limit %d in sLog TID encoding",
				 offnum, MaxOffsetNumber);
		ikey.relid = relid;
		ikey.relnumber = relnumber;
		ikey.encoded_tid = SLOG_ENCODE_TID(blkno, offnum);

		/*
		 * Record the TID in the backend-local hash.  Insert is amortized
		 * O(1); the table and its entries live in TopTransactionContext and
		 * are destroyed at transaction end.
		 */
		(void) sloginsert_insert(slog_insert_tids_ensure(), ikey, &found);
		return;
	}

	{
		SLogTrackedKey *tk;
		SLogTupleKey key;

		memset(&key, 0, sizeof(key));
		key.relid = relid;
		key.relnumber = relnumber;
		ItemPointerCopy(tid, &key.tid);

		oldcxt = MemoryContextSwitchTo(TopTransactionContext);

		tk = (SLogTrackedKey *) palloc(sizeof(SLogTrackedKey));
		memcpy(&tk->key, &key, sizeof(SLogTupleKey));
		tk->relnumber = relnumber;
		tk->xid = xid;
		tk->subxid = subxid;
		tk->op_type = SLOG_OP_INSERT;
		tk->next = slog_tracked_keys;
		slog_tracked_keys = tk;

		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * SLogTupleRemoveBySubXid
 *		Mark the inserts of a rolled-back subtransaction as rolled back.
 *
 * Its tuples are already invisible to everyone (their xmin is the aborted
 * subtransaction's xid); the mark tells commit not to clear their
 * uncommitted hint and lets the AM discard them.
 */
void
SLogTupleRemoveBySubXid(TransactionId xid, TransactionId subxid)
{
	SLogTrackedKey *tk;

	for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
	{
		if (TransactionIdEquals(tk->xid, xid) && tk->subxid == subxid)
			tk->op_type = SLOG_OP_ABORTED;
	}
}

/*
 * SLogTupleUpdateSubXid
 *		Re-parent a committed subtransaction's inserts to its parent, so a
 *		later rollback of the parent finds them.
 */
void
SLogTupleUpdateSubXid(TransactionId xid,
					  TransactionId old_subxid,
					  TransactionId new_subxid)
{
	SLogTrackedKey *tk;

	for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
	{
		if (TransactionIdEquals(tk->xid, xid) && tk->subxid == old_subxid)
			tk->subxid = new_subxid;
	}
}

/*
 * SLogTupleResetTracking
 *		Forget this transaction's tracked inserts (at transaction end).
 *
 * Everything lives in TopTransactionContext, so only the pointers need
 * resetting.
 */
void
SLogTupleResetTracking(void)
{
	slog_insert_tids = NULL;
	slog_tracked_keys = NULL;
}

/*
 * SLogTupleCollectTrackedKeys
 *		Collect all tracked keys for a given xid into a palloc'd array.
 *
 * Returns the number of collected keys.  The caller can then sort the
 * array for batch processing (e.g. by relid/blockno to amortize buffer
 * I/O at commit time).
 *
 * The returned array is allocated in the current memory context; the caller
 * is responsible for pfree().
 */
int
SLogTupleCollectTrackedKeys(TransactionId xid, SLogTrackedKeyInfo **out_keys)
{
	SLogTrackedKey *tk;
	int			count = 0;
	int			capacity = 64;
	SLogTrackedKeyInfo *arr;

	/*
	 * A single very large transaction can touch tens of millions of tracked
	 * keys; at 48 bytes each the array crosses MaxAllocSize (1 GB) past ~22M
	 * entries.  Use the huge-allocation API so bulk loads do not fail at
	 * commit.  This array is transient and pfree'd by the caller.
	 */
	arr = (SLogTrackedKeyInfo *) MemoryContextAllocHuge(CurrentMemoryContext,
														sizeof(SLogTrackedKeyInfo) * (Size) capacity);

	/* Collect backend-local INSERT-hash entries (top-level local-only) */
	if (slog_insert_tids != NULL)
	{
		sloginsert_iterator it;
		SLogInsertTidEntry *ie;

		sloginsert_start_iterate(slog_insert_tids, &it);
		while ((ie = sloginsert_iterate(slog_insert_tids, &it)) != NULL)
		{
			if (count >= capacity)
			{
				capacity *= 2;
				arr = (SLogTrackedKeyInfo *)
					repalloc_huge(arr, sizeof(SLogTrackedKeyInfo) * (Size) capacity);
			}

			memset(&arr[count].key, 0, sizeof(SLogTupleKey));
			arr[count].key.relid = ie->key.relid;
			arr[count].key.relnumber = ie->key.relnumber;
			ItemPointerSet(&arr[count].key.tid,
						   SLOG_DECODE_BLKNO(ie->key.encoded_tid),
						   SLOG_DECODE_OFFNUM(ie->key.encoded_tid));
			arr[count].relnumber = ie->key.relnumber;
			arr[count].xid = xid;
			arr[count].subxid = InvalidTransactionId;
			arr[count].op_type = SLOG_OP_INSERT;
			count++;
		}
	}

	/* Collect linked-list entries */
	for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
	{
		if (!TransactionIdEquals(tk->xid, xid))
			continue;

		if (count >= capacity)
		{
			capacity *= 2;
			arr = (SLogTrackedKeyInfo *)
				repalloc_huge(arr, sizeof(SLogTrackedKeyInfo) * (Size) capacity);
		}

		arr[count].key = tk->key;
		arr[count].relnumber = tk->relnumber;
		arr[count].xid = tk->xid;
		arr[count].subxid = tk->subxid;
		arr[count].op_type = tk->op_type;
		count++;
	}

	*out_keys = arr;
	return count;
}

/*
 * Forget an irreversibly truncated storage incarnation.  The caller holds
 * AccessExclusiveLock and has already truncated the file: no old tuple can
 * survive, even when TRUNCATE reused the same relfilenumber, and its TIDs
 * will be reused.  Forget the tracked inserts in it, lest commit stamp the
 * new occupants, and our row locks on it.  (No other transaction can hold
 * any.)
 */
void
SLogTupleForgetStorage(Oid relid, RelFileNumber relnumber)
{
	SLogTrackedKey **link = &slog_tracked_keys;

	slog_tl_forget_storage(relid, relnumber);

	while (*link != NULL)
	{
		SLogTrackedKey *tk = *link;

		if (tk->key.relid == relid && tk->key.relnumber == relnumber)
		{
			*link = tk->next;
			pfree(tk);
		}
		else
			link = &tk->next;
	}
	if (slog_insert_tids != NULL)
	{
		sloginsert_iterator it;
		SLogInsertTidEntry *ie;

		sloginsert_start_iterate(slog_insert_tids, &it);
		while ((ie = sloginsert_iterate(slog_insert_tids, &it)) != NULL)
		{
			if (ie->key.relid == relid && ie->key.relnumber == relnumber)
				sloginsert_delete(slog_insert_tids, ie->key);
		}
	}
}


/* ----------------------------------------------------------------
 * Row locks (see the file header)
 * ----------------------------------------------------------------
 */
/* one entry per strength at most; strengths strictly increase with depth */
#define SLOG_TL_MAX_DEPTH	4

#define SLOG_TL_VALUE(xid, mode)	(((uint64) (xid) << 8) | (uint64) (mode))
#define SLOG_TL_XID(v)				((TransactionId) ((v) >> 8))
#define SLOG_TL_MODE(v)				((LockTupleMode) ((v) & 0xFF))

/*
 * An entry of a lock table: key slog_tl_key(tid, depth), and the lock's
 * value, 0 once the level is gone.  Key 0 (offset 0, never a TID's) marks a
 * free entry.
 */
typedef struct SLogTupleLockEntry
{
	uint64		key;
	uint64		val;
} SLogTupleLockEntry;

/*
 * A relation storage a slot holds row locks in, beyond its inline records:
 * an open-addressing table of 'size' entries (a power of two), 'used' of
 * them with a key.  Entries past the slot's nrels are not in use; they keep
 * their (empty) tables for the slot's next transactions.
 */
typedef struct SLogTupleLockRel
{
	Oid			relid;
	RelFileNumber relnumber;
	dsa_pointer table;			/* SLogTupleLockEntry[size], or invalid */
	uint64		size;
	uint64		used;
} SLogTupleLockRel;

/* a table is grown before it is more than 3/4 full */
#define SLOG_TL_MIN_ENTRIES		16

/*
 * At the end of its transaction a slot keeps its rels array if it has at
 * most SLOG_TL_KEEP_RELS entries, and in it the tables of at most
 * SLOG_TL_KEEP_ENTRIES entries, emptied, for its next transactions (4 kB
 * each); bigger ones are freed, so that one huge transaction does not keep
 * its memory.  What a slot keeps stays with it for the next backend that
 * uses its PGPROC (at most 4 x 4 kB of tables and a 4-entry array a slot).
 *
 * ponytail: a transaction that locks more than 192 rows of a relation past
 * its inline records allocates its table again each time; raise the limit
 * if that shows in profiles.
 */
#define SLOG_TL_KEEP_RELS		4
#define SLOG_TL_KEEP_ENTRIES	256

/*
 * A row lock kept in its holder's slot: one TID's entries, outermost first,
 * as a table keeps them at depths 0 .. nlev - 1.  An invalid relid marks a
 * free record.
 */
typedef struct SLogTupleLockInline
{
	Oid			relid;
	RelFileNumber relnumber;
	BlockNumber blkno;
	OffsetNumber offnum;
	uint16		nlev;
	uint64		lev[SLOG_TL_MAX_DEPTH];
} SLogTupleLockInline;

/*
 * TIDs a slot keeps inline; the transaction's further TIDs go to its tables.
 *
 * ponytail: fixed; enough for transactions that lock a few rows, which then
 * never touch the shared DSA or a slot LWLock.  Raise it if lock counts per
 * transaction show the tables on a hot path.
 */
#define SLOG_TL_NINLINE		8

typedef struct SLogTupleLockSlot
{
	LWLock		lock;			/* protects rels and the tables */
	pg_atomic_uint32 changecount;	/* odd while inline/ninline/nrels change */
	TransactionId xid;			/* top-level holder, or invalid */
	int			ninline;		/* inline records in use (or freed) */
	bool		indexed;		/* may have bits in the holder index */
	int			nrels;			/* rels in use */
	int			maxrels;
	dsa_pointer rels;			/* SLogTupleLockRel[maxrels] */
	SLogTupleLockInline inl[SLOG_TL_NINLINE];
} SLogTupleLockSlot;

/*
 * The holder index: bucket b is a bitset of slots, bit i set while slot i
 * may hold a lock on a TID whose hash (slog_tl_bucket) is b.  A locker sets
 * its bit under the buffer lock of the tuple before it records the lock,
 * and a probe reads the bucket under that buffer lock, so a probe cannot
 * miss a lock on its tuple.  A bit outlives the locks it stands for (until
 * the holder's transaction ends), and buckets are shared by many TIDs:
 * the probe then visits a slot for nothing, which costs time, not
 * correctness.  A backend notes the buckets it set (slog_tl_listed), so
 * that the end of its transaction clears only those; the slot of a prepared
 * transaction is cleared by a scan of every bucket.
 *
 * A lock bit in the tuple header cannot serve as the filter: an UPDATE
 * compatible with a KEY SHARE lock may write a new image without it.
 *
 * ponytail: a fixed bucket count, 512 bytes per slot; a probe visits about
 * (live locked TIDs / 4096) slots for nothing.  Size it from max_connections
 * if more concurrent row locks make that show in profiles.
 */
#define SLOG_TL_INDEX_NBUCKETS	4096
#define SLOG_TL_NWORDS(nslots)	(((nslots) + 63) / 64)

typedef struct SLogTupleLockShared
{
	int			tranche_id;
	int			nslots;
	int			nwords;			/* index words per bucket */
	pg_atomic_uint64 index[FLEXIBLE_ARRAY_MEMBER];	/* [bucket][word] */
	/* followed, cache-line aligned, by nslots padded SLogTupleLockSlots */
} SLogTupleLockShared;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
StaticAssertDecl(offsetof(SLogTupleLockShared, index) % 8 == 0,
				 "SLogTupleLockShared.index must be eight-byte aligned");
#endif

#define SLOG_TL_SLOT_SIZE \
	TYPEALIGN(PG_CACHE_LINE_SIZE, sizeof(SLogTupleLockSlot))
#define SLOG_TL_SLOTS_OFFSET(nslots) \
	TYPEALIGN(PG_CACHE_LINE_SIZE, \
			  offsetof(SLogTupleLockShared, index) + \
			  sizeof(pg_atomic_uint64) * SLOG_TL_INDEX_NBUCKETS * \
			  SLOG_TL_NWORDS(nslots))

static void slog_tl_free_slot(int slotno, bool mine);

static SLogTupleLockShared *slog_tl_dir = NULL;
static dsa_area *slog_tl_area = NULL;

/* Does this backend's slot hold anything of its current transaction? */
static bool slog_tl_used = false;

/* The index buckets in which this backend set its slot's bit */
static uint16 *slog_tl_listed = NULL;
static int	slog_tl_nlisted = 0;

static const LOCKMODE slog_tl_lockmode[] = {
	[LockTupleKeyShare] = AccessShareLock,
	[LockTupleShare] = RowShareLock,
	[LockTupleNoKeyExclusive] = ExclusiveLock,
	[LockTupleExclusive] = AccessExclusiveLock,
};

static inline uint64
slog_tl_key(ItemPointer tid, int depth)
{
	return ((uint64) depth << 48) |
		((uint64) ItemPointerGetBlockNumber(tid) << 16) |
		ItemPointerGetOffsetNumber(tid);
}

/* The holder-index bucket of a TID */
static inline uint32
slog_tl_bucket(Oid relid, RelFileNumber relnumber, ItemPointer tid)
{
	uint64		h;

	h = hash_combine64(murmurhash64(((uint64) relid << 32) | relnumber),
					   murmurhash64(((uint64) ItemPointerGetBlockNumber(tid) << 16) |
									ItemPointerGetOffsetNumber(tid)));
	return (uint32) h & (SLOG_TL_INDEX_NBUCKETS - 1);
}

static inline pg_atomic_uint64 *
slog_tl_index_word(SLogTupleLockShared *dir, uint32 bucket, int slotno)
{
	return &dir->index[(Size) bucket * dir->nwords + slotno / 64];
}

static inline uint64
slog_tl_index_bit(int slotno)
{
	return UINT64CONST(1) << (slotno % 64);
}

static inline SLogTupleLockSlot *
slog_tl_slot(SLogTupleLockShared *dir, int slotno)
{
	return (SLogTupleLockSlot *) ((char *) dir +
								  SLOG_TL_SLOTS_OFFSET(dir->nslots) +
								  (Size) slotno * SLOG_TL_SLOT_SIZE);
}

/*
 * A slot's inline records and table count change between these two (the
 * slot has one writer at a time: its owner, or for a prepared transaction's
 * dummy slot whoever prepares, restores or finishes it).  Readers retry
 * while the count is odd or has moved, as for PgBackendStatus.
 */
static inline void
slog_tl_change_begin(SLogTupleLockSlot *slot)
{
	pg_atomic_fetch_add_u32(&slot->changecount, 1);
}

static inline void
slog_tl_change_end(SLogTupleLockSlot *slot)
{
	pg_atomic_fetch_add_u32(&slot->changecount, 1);
}

static void
slog_tl_init_dir(void *ptr, void *arg)
{
	SLogTupleLockShared *dir = ptr;

	dir->tranche_id = LWLockNewTrancheId("SLogTupleLock");
	dir->nslots = *(int *) arg;
	dir->nwords = SLOG_TL_NWORDS(dir->nslots);
	for (int w = 0; w < SLOG_TL_INDEX_NBUCKETS * dir->nwords; w++)
		pg_atomic_init_u64(&dir->index[w], 0);
	for (int i = 0; i < dir->nslots; i++)
	{
		SLogTupleLockSlot *slot = slog_tl_slot(dir, i);

		slot->indexed = false;
		LWLockInitialize(&slot->lock, dir->tranche_id);
		pg_atomic_init_u32(&slot->changecount, 0);
		slot->xid = InvalidTransactionId;
		slot->ninline = 0;
		slot->nrels = 0;
		slot->maxrels = 0;
		slot->rels = InvalidDsaPointer;
	}
}

static SLogTupleLockShared *
slog_tl_directory(void)
{
	if (slog_tl_dir == NULL)
	{
		/* every PGPROC, including prepared transactions' dummy procs */
		int			nslots = FIRST_PREPARED_XACT_PROC_NUMBER + max_prepared_xacts;
		bool		found;
		Size		size;

		size = add_size(SLOG_TL_SLOTS_OFFSET(nslots),
						mul_size(SLOG_TL_SLOT_SIZE, nslots));
		slog_tl_dir = GetNamedDSMSegment("slog_tuple_locks", size,
										 slog_tl_init_dir, &found, &nslots);
	}
	return slog_tl_dir;
}

static dsa_area *
slog_tl_dsa(void)
{
	if (slog_tl_area == NULL)
	{
		bool		found;

		slog_tl_area = GetNamedDSA("slog_tuple_lock_tables", &found);
	}
	return slog_tl_area;
}

/*
 * Mark slot slotno as possibly holding a lock on a TID of the given bucket
 * (see the holder index).  The slot's writer calls it before it records the
 * lock.  In this backend's own slot, the bit is set only here, and noted.
 */
static void
slog_tl_index_set(SLogTupleLockShared *dir, int slotno, uint32 bucket)
{
	pg_atomic_uint64 *word = slog_tl_index_word(dir, bucket, slotno);
	uint64		bit = slog_tl_index_bit(slotno);

	if (pg_atomic_read_u64(word) & bit)
		return;
	if (slotno == MyProcNumber)
	{
		Assert(slog_tl_nlisted < SLOG_TL_INDEX_NBUCKETS);
		slog_tl_listed[slog_tl_nlisted++] = bucket;
	}
	slog_tl_slot(dir, slotno)->indexed = true;
	pg_atomic_fetch_or_u64(word, bit);
}

/* Clear this backend's slot from the buckets it noted. */
static void
slog_tl_index_clear_mine(SLogTupleLockShared *dir)
{
	uint64		bit = slog_tl_index_bit(MyProcNumber);

	for (int i = 0; i < slog_tl_nlisted; i++)
		pg_atomic_fetch_and_u64(slog_tl_index_word(dir, slog_tl_listed[i],
												   MyProcNumber), ~bit);
	slog_tl_nlisted = 0;
}

/* Clear slot slotno from every bucket. */
static void
slog_tl_index_clear_all(SLogTupleLockShared *dir, int slotno)
{
	uint64		bit = slog_tl_index_bit(slotno);

	for (uint32 b = 0; b < SLOG_TL_INDEX_NBUCKETS; b++)
	{
		pg_atomic_uint64 *word = slog_tl_index_word(dir, b, slotno);

		if (pg_atomic_read_u64(word) & bit)
			pg_atomic_fetch_and_u64(word, ~bit);
	}
}

/*
 * The inline record of a TID in a slot, or -1.  For the slot's writer, which
 * needs no protection against itself.
 */
static int
slog_tl_find_inline(SLogTupleLockSlot *slot, Oid relid,
					RelFileNumber relnumber, ItemPointer tid)
{
	BlockNumber blkno = ItemPointerGetBlockNumber(tid);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);

	for (int r = 0; r < slot->ninline; r++)
	{
		SLogTupleLockInline *in = &slot->inl[r];

		if (in->relid == relid && in->relnumber == relnumber &&
			in->blkno == blkno && in->offnum == offnum)
			return r;
	}
	return -1;
}

/* A free inline record of a slot, or -1 */
static int
slog_tl_free_inline(SLogTupleLockSlot *slot)
{
	for (int r = 0; r < slot->ninline; r++)
		if (!OidIsValid(slot->inl[r].relid))
			return r;
	return slot->ninline < SLOG_TL_NINLINE ? slot->ninline : -1;
}

/* Write a TID's entries to inline record r of a slot. */
static void
slog_tl_write_inline(SLogTupleLockSlot *slot, int r, Oid relid,
					 RelFileNumber relnumber, ItemPointer tid,
					 int nlev, const uint64 *lev)
{
	SLogTupleLockInline *in = &slot->inl[r];

	Assert(nlev > 0 && nlev <= SLOG_TL_MAX_DEPTH);
	slog_tl_change_begin(slot);
	in->relid = relid;
	in->relnumber = relnumber;
	in->blkno = ItemPointerGetBlockNumber(tid);
	in->offnum = ItemPointerGetOffsetNumber(tid);
	in->nlev = nlev;
	memcpy(in->lev, lev, sizeof(uint64) * nlev);
	if (r >= slot->ninline)
		slot->ninline = r + 1;
	slog_tl_change_end(slot);
}

/*
 * A probe's read of another slot: the entries of the TID's inline record
 * that conflict with lockmode go to cand, and *has_tables says whether the
 * TID may be in one of the slot's tables instead.  The caller holds the
 * TID's buffer lock, under which the owner wrote the record, so the record
 * itself cannot change; the counter guards against the owner's concurrent
 * changes to its other records (another TID, or the end of its
 * transaction).
 */
static int
slog_tl_read_inline(SLogTupleLockSlot *slot, Oid relid,
					RelFileNumber relnumber, ItemPointer tid,
					LOCKMODE lockmode, uint64 *cand, bool *has_tables)
{
	volatile SLogTupleLockSlot *vslot = slot;
	BlockNumber blkno = ItemPointerGetBlockNumber(tid);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
	SpinDelayStatus delay;
	int			n;

	init_local_spin_delay(&delay);
	for (;;)
	{
		uint32		before = pg_atomic_read_u32(&slot->changecount);
		int			ninline;
		bool		found = false;

		if (before & 1)
		{
			perform_spin_delay(&delay);
			continue;
		}
		pg_read_barrier();

		n = 0;
		ninline = Min(vslot->ninline, SLOG_TL_NINLINE);
		for (int r = 0; r < ninline; r++)
		{
			volatile SLogTupleLockInline *in = &vslot->inl[r];
			int			nlev;

			if (in->relid != relid || in->relnumber != relnumber ||
				in->blkno != blkno || in->offnum != offnum)
				continue;
			nlev = Min(in->nlev, SLOG_TL_MAX_DEPTH);
			for (int d = 0; d < nlev; d++)
			{
				uint64		v = in->lev[d];

				/* (a torn read is retried below; just stay in bounds) */
				if (SLOG_TL_MODE(v) <= LockTupleExclusive &&
					DoLockModesConflict(lockmode,
										slog_tl_lockmode[SLOG_TL_MODE(v)]))
					cand[n++] = v;
			}
			found = true;
			break;
		}
		/* a TID's entries are in its record or in a table, not both */
		*has_tables = !found && vslot->nrels > 0;

		pg_read_barrier();
		if (pg_atomic_read_u32(&slot->changecount) == before)
		{
			finish_spin_delay(&delay);
			return n;
		}
		perform_spin_delay(&delay);
	}
}

/*
 * Every shared allocation of the row-lock store, zeroed.  Test-only
 * injection points fail it as the DSA running out would:
 * slog-tuple-lock-oom any of them, slog-tuple-lock-table-oom only a lock
 * table's, so that a test can fail the second allocation of a step.
 */
static dsa_pointer
slog_tl_alloc(Size size, bool table)
{
	if (IS_INJECTION_POINT_ATTACHED("slog-tuple-lock-oom") ||
		(table && IS_INJECTION_POINT_ATTACHED("slog-tuple-lock-table-oom")))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Injected failure of a row-lock store allocation.")));
	return dsa_allocate_extended(slog_tl_dsa(), size,
								 DSA_ALLOC_ZERO | DSA_ALLOC_HUGE);
}

/* A slot's rels array, or NULL (for its writer, or under its lock) */
static inline SLogTupleLockRel *
slog_tl_rels(SLogTupleLockSlot *slot)
{
	if (!DsaPointerIsValid(slot->rels))
		return NULL;
	return (SLogTupleLockRel *) dsa_get_address(slog_tl_dsa(), slot->rels);
}

/* The rels entry of a relation storage in a slot, or NULL */
static SLogTupleLockRel *
slog_tl_find_rel(SLogTupleLockSlot *slot, Oid relid, RelFileNumber relnumber)
{
	SLogTupleLockRel *rels;

	if (slot->nrels == 0)
		return NULL;
	rels = slog_tl_rels(slot);
	for (int i = 0; i < slot->nrels; i++)
		if (rels[i].relid == relid && rels[i].relnumber == relnumber)
			return &rels[i];
	return NULL;
}

static inline SLogTupleLockEntry *
slog_tl_table(SLogTupleLockRel *rel)
{
	return (SLogTupleLockEntry *) dsa_get_address(slog_tl_dsa(), rel->table);
}

/*
 * The entry of key in a table of size entries, or the free entry where it
 * would go.  A table is never more than 3/4 full, so there is one.
 */
static inline SLogTupleLockEntry *
slog_tl_entry(SLogTupleLockEntry *tab, uint64 size, uint64 key)
{
	uint64		i = murmurhash64(key) & (size - 1);

	while (tab[i].key != key && tab[i].key != 0)
		i = (i + 1) & (size - 1);
	return &tab[i];
}

/* The value of key in a rel's table, or 0 */
static inline uint64
slog_tl_lookup(SLogTupleLockRel *rel, uint64 key)
{
	SLogTupleLockEntry *e = slog_tl_entry(slog_tl_table(rel), rel->size, key);

	return e->key == key ? e->val : 0;
}

/*
 * The rels entry of a relation storage in slot slotno, made if there is
 * none.  For the slot's writer.  The allocations come before any change and
 * each is the slot's as soon as it is made: a bigger rels array replaces
 * the old one at once (holding the same tables), and a new table goes into
 * the first entry past the ones in use, where the slot keeps the empty
 * tables of its earlier transactions and where its release finds it.  So
 * running out of memory leaves nothing unowned, and changes nothing a probe
 * can see.
 */
static SLogTupleLockRel *
slog_tl_add_rel(int slotno, Oid relid, RelFileNumber relnumber)
{
	SLogTupleLockSlot *slot = slog_tl_slot(slog_tl_dir, slotno);
	SLogTupleLockRel *rels;
	SLogTupleLockRel *rel;

	rel = slog_tl_find_rel(slot, relid, relnumber);
	if (rel != NULL)
		return rel;

	rels = slog_tl_rels(slot);
	if (slot->nrels == slot->maxrels)
	{
		int			newmax = Max(SLOG_TL_KEEP_RELS, slot->maxrels * 2);
		dsa_pointer newrels;
		dsa_pointer oldrels = slot->rels;

		newrels = slog_tl_alloc(sizeof(SLogTupleLockRel) * newmax, false);
		if (slot->maxrels > 0)
			memcpy(dsa_get_address(slog_tl_dsa(), newrels), rels,
				   sizeof(SLogTupleLockRel) * slot->maxrels);
		LWLockAcquire(&slot->lock, LW_EXCLUSIVE);
		slot->rels = newrels;
		slot->maxrels = newmax;
		LWLockRelease(&slot->lock);
		if (DsaPointerIsValid(oldrels))
			dsa_free(slog_tl_dsa(), oldrels);
		rels = slog_tl_rels(slot);
	}

	rel = &rels[slot->nrels];
	if (!DsaPointerIsValid(rel->table))
	{
		rel->table = slog_tl_alloc(sizeof(SLogTupleLockEntry) *
								   SLOG_TL_MIN_ENTRIES, true);
		rel->size = SLOG_TL_MIN_ENTRIES;
		rel->used = 0;
	}
	Assert(rel->used == 0);
	rel->relid = relid;
	rel->relnumber = relnumber;

	LWLockAcquire(&slot->lock, LW_EXCLUSIVE);
	slog_tl_change_begin(slot);
	slot->nrels++;
	slog_tl_change_end(slot);
	LWLockRelease(&slot->lock);
	return rel;
}

/*
 * Give a rel's table room for need keys: a bigger table, filled before it
 * replaces the old one.  For the slot's writer; the one allocation is the
 * only step that can fail, and it changes nothing.
 */
static void
slog_tl_grow_table(SLogTupleLockSlot *slot, SLogTupleLockRel *rel, uint64 need)
{
	SLogTupleLockEntry *old = slog_tl_table(rel);
	SLogTupleLockEntry *tab;
	dsa_pointer oldtable = rel->table;
	dsa_pointer newtable;
	uint64		size = rel->size * 2;
	uint64		used = 0;

	while (need > size / 4 * 3)
		size *= 2;
	newtable = slog_tl_alloc(sizeof(SLogTupleLockEntry) * size, true);
	tab = (SLogTupleLockEntry *) dsa_get_address(slog_tl_dsa(), newtable);
	for (uint64 i = 0; i < rel->size; i++)
	{
		if (old[i].key != 0 && old[i].val != 0)
		{
			*slog_tl_entry(tab, size, old[i].key) = old[i];
			used++;
		}
	}
	LWLockAcquire(&slot->lock, LW_EXCLUSIVE);
	rel->table = newtable;
	rel->size = size;
	rel->used = used;
	LWLockRelease(&slot->lock);
	dsa_free(slog_tl_dsa(), oldtable);
}

/*
 * Write a TID's entries, depths 0 .. n - 1, to the table of its relation
 * storage in slot slotno, and drop its entries at depths n .. oldn - 1.
 * For the slot's writer.  Whatever can fail comes first and changes
 * nothing a probe can see; then the writes, which cannot fail, under the
 * slot's lock.
 */
static void
slog_tl_table_write(int slotno, Oid relid, RelFileNumber relnumber,
					ItemPointer tid, int n, const uint64 *lev, int oldn)
{
	SLogTupleLockSlot *slot = slog_tl_slot(slog_tl_dir, slotno);
	SLogTupleLockRel *rel = slog_tl_add_rel(slotno, relid, relnumber);
	SLogTupleLockEntry *tab;

	if (rel->used + n > rel->size / 4 * 3)
		slog_tl_grow_table(slot, rel, rel->used + n);
	tab = slog_tl_table(rel);

	LWLockAcquire(&slot->lock, LW_EXCLUSIVE);
	for (int d = 0; d < n; d++)
	{
		uint64		key = slog_tl_key(tid, d);
		SLogTupleLockEntry *e = slog_tl_entry(tab, rel->size, key);

		if (e->key == 0)
		{
			e->key = key;
			rel->used++;
		}
		e->val = lev[d];
	}
	for (int d = n; d < oldn; d++)
	{
		SLogTupleLockEntry *e = slog_tl_entry(tab, rel->size,
											  slog_tl_key(tid, d));

		Assert(e->key == slog_tl_key(tid, d));
		e->val = 0;
	}
	LWLockRelease(&slot->lock);
}

/*
 * Empty a slot.  With mine, the slot is this backend's own, and it keeps a
 * small rels array and its small tables, emptied, for the backend's next
 * transactions; otherwise everything is freed.  This allocates nothing, as
 * transaction abort needs.
 */
static void
slog_tl_free_slot(int slotno, bool mine)
{
	SLogTupleLockShared *dir = slog_tl_directory();
	SLogTupleLockSlot *slot = slog_tl_slot(dir, slotno);

	if (DsaPointerIsValid(slot->rels))
	{
		SLogTupleLockRel *rels = slog_tl_rels(slot);
		bool		keep = mine && slot->maxrels <= SLOG_TL_KEEP_RELS;
		int			nrels = slot->nrels;

		LWLockAcquire(&slot->lock, LW_EXCLUSIVE);
		slog_tl_change_begin(slot);
		slot->nrels = 0;
		slog_tl_change_end(slot);
		for (int i = 0; i < slot->maxrels; i++)
		{
			SLogTupleLockRel *rel = &rels[i];

			if (!DsaPointerIsValid(rel->table))
				continue;
			if (keep && rel->size <= SLOG_TL_KEEP_ENTRIES)
			{
				/* (those past the ones in use are empty already) */
				if (i < nrels)
					memset(slog_tl_table(rel), 0,
						   sizeof(SLogTupleLockEntry) * rel->size);
				rel->used = 0;
				continue;
			}
			dsa_free(slog_tl_dsa(), rel->table);
			rel->table = InvalidDsaPointer;
			rel->size = 0;
			rel->used = 0;
		}
		if (!keep)
		{
			dsa_free(slog_tl_dsa(), slot->rels);
			slot->rels = InvalidDsaPointer;
			slot->maxrels = 0;
		}
		LWLockRelease(&slot->lock);
	}
	if (slot->ninline > 0)
	{
		slog_tl_change_begin(slot);
		slot->ninline = 0;
		slog_tl_change_end(slot);
	}
	slot->xid = InvalidTransactionId;

	/*
	 * This backend's noted buckets are all its transaction set; anything else
	 * (a prepared transaction's slot, or one a backend left) needs a scan.
	 */
	if (slotno == MyProcNumber && slog_tl_nlisted > 0)
		slog_tl_index_clear_mine(dir);
	else if (slot->indexed)
		slog_tl_index_clear_all(dir, slotno);
	slot->indexed = false;
}

/* The first row lock of this backend's transaction: take its slot. */
static SLogTupleLockSlot *
slog_tl_my_slot(void)
{
	SLogTupleLockShared *dir = slog_tl_directory();
	SLogTupleLockSlot *slot = slog_tl_slot(dir, MyProcNumber);

	if (!slog_tl_used)
	{
		if (slog_tl_listed == NULL)
			slog_tl_listed = MemoryContextAlloc(TopMemoryContext,
												sizeof(uint16) * SLOG_TL_INDEX_NBUCKETS);

		/*
		 * A backend that exited without cleaning up may have left it.  (The
		 * empty tables kept for reuse stay.)
		 */
		if (slot->ninline > 0 || slot->nrels > 0 || slot->indexed)
			slog_tl_free_slot(MyProcNumber, false);
		slot->xid = GetTopTransactionId();
		slog_tl_used = true;
	}
	return slot;
}

/*
 * SLogTupleLockAcquire -- record that the current subtransaction holds a row
 * lock of the given strength on a tuple.
 *
 * Call it under the tuple's buffer lock, before changing the page: running
 * out of shared memory ERRORs here, with nothing changed.
 *
 * A TID's entries live in one place in the slot: in an inline record while
 * there is one free when the TID is first locked, else in the table of its
 * relation storage.
 */
void
SLogTupleLockAcquire(Oid relid, RelFileNumber relnumber, ItemPointer tid,
					 LockTupleMode mode)
{
	TransactionId cur = GetCurrentTransactionId();
	SLogTupleLockSlot *slot = slog_tl_my_slot();
	SLogTupleLockRel *rel = NULL;
	uint64		old[SLOG_TL_MAX_DEPTH];
	uint64		stack[SLOG_TL_MAX_DEPTH];
	int			n = 0;
	int			oldn = 0;
	int			r;

	r = slog_tl_find_inline(slot, relid, relnumber, tid);
	if (r >= 0)
	{
		oldn = slot->inl[r].nlev;
		memcpy(old, slot->inl[r].lev, sizeof(uint64) * oldn);
	}
	else if ((rel = slog_tl_find_rel(slot, relid, relnumber)) != NULL)
	{
		for (int d = 0; d < SLOG_TL_MAX_DEPTH; d++)
		{
			uint64		v = slog_tl_lookup(rel, slog_tl_key(tid, d));

			if (v == 0)
				break;
			old[oldn++] = v;
		}
	}

	/*
	 * Every live entry (one whose subtransaction is still current) lives at
	 * least as long as the current subtransaction: it was taken by this one,
	 * by an enclosing one, or by an inner one already committed into an
	 * enclosing one.  So a live entry at least as strong covers the request,
	 * and the new lock replaces the current subtransaction's weaker ones.
	 * Entries of aborted subtransactions are dropped.
	 */
	for (int d = 0; d < oldn; d++)
	{
		if (!TransactionIdIsCurrentTransactionId(SLOG_TL_XID(old[d])))
			continue;
		if (SLOG_TL_MODE(old[d]) >= mode)
			return;
		if (TransactionIdEquals(SLOG_TL_XID(old[d]), cur))
			continue;
		stack[n++] = old[d];
	}
	Assert(n < SLOG_TL_MAX_DEPTH);
	stack[n++] = SLOG_TL_VALUE(cur, mode);

	slog_tl_index_set(slog_tl_dir, MyProcNumber,
					  slog_tl_bucket(relid, relnumber, tid));
	if (r < 0 && oldn == 0)
		r = slog_tl_free_inline(slot);
	if (r >= 0)
		slog_tl_write_inline(slot, r, relid, relnumber, tid, n, stack);
	else
		slog_tl_table_write(MyProcNumber, relid, relnumber, tid, n, stack,
							oldn);
}

/*
 * SLogTupleLockHeldByMe -- does the current transaction (any live
 * subtransaction) hold a row lock on the tuple?  If so, *mode (when not
 * NULL) is the strongest one.
 */
bool
SLogTupleLockHeldByMe(Oid relid, RelFileNumber relnumber, ItemPointer tid,
					  LockTupleMode *mode)
{
	SLogTupleLockSlot *slot;
	SLogTupleLockRel *rel;
	int			best = -1;
	int			r;

	if (!slog_tl_used)
		return false;
	slot = slog_tl_slot(slog_tl_dir, MyProcNumber);
	r = slog_tl_find_inline(slot, relid, relnumber, tid);
	if (r >= 0)
	{
		for (int d = 0; d < slot->inl[r].nlev; d++)
		{
			uint64		v = slot->inl[r].lev[d];

			if (TransactionIdIsCurrentTransactionId(SLOG_TL_XID(v)))
				best = Max(best, (int) SLOG_TL_MODE(v));
		}
	}
	else if ((rel = slog_tl_find_rel(slot, relid, relnumber)) != NULL)
	{
		for (int d = 0; d < SLOG_TL_MAX_DEPTH; d++)
		{
			uint64		v = slog_tl_lookup(rel, slog_tl_key(tid, d));

			if (v == 0)
				break;
			if (TransactionIdIsCurrentTransactionId(SLOG_TL_XID(v)))
				best = Max(best, (int) SLOG_TL_MODE(v));
		}
	}
	if (best < 0)
		return false;
	if (mode)
		*mode = (LockTupleMode) best;
	return true;
}

/*
 * One holder slot of SLogTupleLockConflictXid: the xid to wait for, or
 * InvalidTransactionId.
 */
static TransactionId
slog_tl_probe_slot(SLogTupleLockShared *dir, int i, Oid relid,
				   RelFileNumber relnumber, ItemPointer tid, LOCKMODE lockmode)
{
	SLogTupleLockSlot *slot;
	uint64		cand[SLOG_TL_MAX_DEPTH];
	int			n;
	bool		has_tables;

	slot = slog_tl_slot(dir, i);
	n = slog_tl_read_inline(slot, relid, relnumber, tid, lockmode, cand,
							&has_tables);
	if (n == 0 && has_tables)
	{
		SLogTupleLockRel *rel;

		(void) slog_tl_dsa();	/* attach before taking the lock */
		LWLockAcquire(&slot->lock, LW_SHARED);
		rel = slog_tl_find_rel(slot, relid, relnumber);
		for (int d = 0; rel != NULL && d < SLOG_TL_MAX_DEPTH; d++)
		{
			uint64		v = slog_tl_lookup(rel, slog_tl_key(tid, d));

			if (v == 0)
				break;
			if (DoLockModesConflict(lockmode,
									slog_tl_lockmode[SLOG_TL_MODE(v)]))
				cand[n++] = v;
		}
		LWLockRelease(&slot->lock);
	}

	/* (a parallel worker's leader holds our own locks) */
	for (int c = 0; c < n; c++)
		if (!TransactionIdIsCurrentTransactionId(SLOG_TL_XID(cand[c])) &&
			TransactionIdIsInProgress(SLOG_TL_XID(cand[c])))
			return SLOG_TL_XID(cand[c]);
	return InvalidTransactionId;
}

/*
 * SLogTupleLockConflictXid -- find another transaction's row lock on the tuple
 * that conflicts with mode.  Returns the xid to wait for (the locking
 * subtransaction's), or InvalidTransactionId.  Call it under the tuple's
 * buffer lock.
 *
 * Within one holder the outermost conflicting lock is returned, since it
 * lives longest.
 *
 * Only the slots in the TID's bucket of the holder index are visited, in
 * slot order, and the bucket is read again after each visit for the slots
 * after it: see SLogTupleLockAtPrepare.
 */
TransactionId
SLogTupleLockConflictXid(Oid relid, RelFileNumber relnumber, ItemPointer tid,
						 LockTupleMode mode)
{
	SLogTupleLockShared *dir = slog_tl_directory();
	uint32		bucket = slog_tl_bucket(relid, relnumber, tid);
	LOCKMODE	lockmode = slog_tl_lockmode[mode];

	for (int w = 0; w < dir->nwords; w++)
	{
		pg_atomic_uint64 *word = slog_tl_index_word(dir, bucket, w * 64);
		uint64		bits = pg_atomic_read_u64(word);

		while (bits != 0)
		{
			int			j = pg_rightmost_one_pos64(bits);
			int			i = w * 64 + j;
			TransactionId xwait;

			pg_read_barrier();
			if (i != MyProcNumber)
			{
				xwait = slog_tl_probe_slot(dir, i, relid, relnumber, tid,
										   lockmode);
				if (TransactionIdIsValid(xwait))
					return xwait;
			}
			pg_read_barrier();
			/* the slots after i, read again */
			bits = pg_atomic_read_u64(word) & ~((UINT64CONST(2) << j) - 1);
		}
	}
	return InvalidTransactionId;
}


/* Top-level commit or abort: release every row lock of this backend. */
void
SLogTupleLockReleaseAll(void)
{
	if (!slog_tl_used)
		return;
	slog_tl_free_slot(MyProcNumber, true);
	slog_tl_used = false;
}

/* Row locks of SLogTupleForgetStorage */
static void
slog_tl_forget_storage(Oid relid, RelFileNumber relnumber)
{
	SLogTupleLockSlot *slot;
	SLogTupleLockRel *rel;
	bool		changing = false;

	if (!slog_tl_used)
		return;
	slot = slog_tl_slot(slog_tl_dir, MyProcNumber);
	for (int r = 0; r < slot->ninline; r++)
	{
		if (slot->inl[r].relid != relid || slot->inl[r].relnumber != relnumber)
			continue;
		if (!changing)
			slog_tl_change_begin(slot);
		changing = true;
		slot->inl[r].relid = InvalidOid;
	}
	if (changing)
		slog_tl_change_end(slot);

	/*
	 * Empty its table and move it past the ones in use, where the slot's
	 * release finds it.
	 */
	rel = slog_tl_find_rel(slot, relid, relnumber);
	if (rel != NULL)
	{
		SLogTupleLockRel *last = &slog_tl_rels(slot)[slot->nrels - 1];
		SLogTupleLockRel gone;

		LWLockAcquire(&slot->lock, LW_EXCLUSIVE);
		memset(slog_tl_table(rel), 0, sizeof(SLogTupleLockEntry) * rel->size);
		gone = *rel;
		gone.used = 0;
		*rel = *last;
		*last = gone;
		slog_tl_change_begin(slot);
		slot->nrels--;
		slog_tl_change_end(slot);
		LWLockRelease(&slot->lock);
	}
}

/*
 * Call cb for each live row lock of the current transaction, with the
 * relation storage it is in.  A TID may be reported more than once (one per
 * live subtransaction level); the strongest wins.
 */
void
SLogTupleLockForEachMine(SLogTupleLockCallback cb, void *arg)
{
	SLogTupleLockSlot *slot;
	SLogTupleLockRel *rels;

	if (!slog_tl_used)
		return;
	slot = slog_tl_slot(slog_tl_dir, MyProcNumber);
	for (int r = 0; r < slot->ninline; r++)
	{
		SLogTupleLockInline *in = &slot->inl[r];
		ItemPointerData tid;

		if (!OidIsValid(in->relid))
			continue;
		ItemPointerSet(&tid, in->blkno, in->offnum);
		for (int d = 0; d < in->nlev; d++)
			if (TransactionIdIsCurrentTransactionId(SLOG_TL_XID(in->lev[d])))
				cb(in->relid, in->relnumber, &tid, SLOG_TL_MODE(in->lev[d]),
				   arg);
	}

	rels = slog_tl_rels(slot);
	for (int i = 0; i < slot->nrels; i++)
	{
		SLogTupleLockEntry *tab = slog_tl_table(&rels[i]);

		for (uint64 k = 0; k < rels[i].size; k++)
		{
			ItemPointerData tid;

			if (tab[k].key == 0 || tab[k].val == 0 ||
				!TransactionIdIsCurrentTransactionId(SLOG_TL_XID(tab[k].val)))
				continue;
			ItemPointerSet(&tid,
						   (BlockNumber) ((tab[k].key >> 16) & 0xFFFFFFFF),
						   (OffsetNumber) (tab[k].key & 0xFFFF));
			cb(rels[i].relid, rels[i].relnumber, &tid,
			   SLOG_TL_MODE(tab[k].val), arg);
		}
	}
}

/*
 * PREPARE: hand this backend's row locks to the prepared transaction, whose
 * dummy proc's slot keeps them until COMMIT/ROLLBACK PREPARED.  Called after
 * the dummy proc took over the xid.  any_live says whether a live lock was
 * written to the AM's two-phase state; without one, COMMIT PREPARED would
 * not call back to free the slot, so the (dead) locks are dropped now.
 */
void
SLogTupleLockAtPrepare(FullTransactionId fxid, bool any_live)
{
	SLogTupleLockShared *dir;
	SLogTupleLockSlot *src;
	SLogTupleLockSlot *dst;
	int			dstno;
	uint64		dstbit;

	if (!slog_tl_used)
		return;
	if (!any_live)
	{
		SLogTupleLockReleaseAll();
		return;
	}

	dir = slog_tl_dir;
	src = slog_tl_slot(dir, MyProcNumber);
	dstno = TwoPhaseGetDummyProcNumber(fxid, false);
	dst = slog_tl_slot(dir, dstno);
	Assert(dstno > MyProcNumber);

	/* The dummy proc is ours now: whatever its slot holds is stale. */
	if (dst->ninline > 0 || dst->maxrels > 0 || dst->indexed)
		slog_tl_free_slot(dstno, false);

	/*
	 * A probe visits the slots of a bucket in slot order, reading the bucket
	 * again after each, and ours comes first.  So filling the dummy slot
	 * (index bits, then contents) before emptying ours (contents, then index
	 * bits) keeps the locks in flight visible to it: if it finds ours empty,
	 * it reads the bucket after that and finds the dummy's bit.  The slot
	 * locks keep table probes out meanwhile.  The tables go with the rels
	 * array, including the empty ones kept for reuse.
	 */
	LWLockAcquire(&src->lock, LW_EXCLUSIVE);
	LWLockAcquire(&dst->lock, LW_EXCLUSIVE);
	dst->indexed = true;
	dstbit = slog_tl_index_bit(dstno);
	for (int i = 0; i < slog_tl_nlisted; i++)
		pg_atomic_fetch_or_u64(slog_tl_index_word(dir, slog_tl_listed[i],
												  dstno), dstbit);
	slog_tl_change_begin(dst);
	dst->xid = XidFromFullTransactionId(fxid);
	dst->ninline = src->ninline;
	memcpy(dst->inl, src->inl, sizeof(SLogTupleLockInline) * src->ninline);
	dst->nrels = src->nrels;
	dst->maxrels = src->maxrels;
	dst->rels = src->rels;
	slog_tl_change_end(dst);
	slog_tl_change_begin(src);
	src->xid = InvalidTransactionId;
	src->ninline = 0;
	src->nrels = 0;
	src->maxrels = 0;
	src->rels = InvalidDsaPointer;
	slog_tl_change_end(src);
	slog_tl_index_clear_mine(dir);
	src->indexed = false;
	LWLockRelease(&dst->lock);
	LWLockRelease(&src->lock);
	slog_tl_used = false;
}

/*
 * End of recovery: restore one row lock of a prepared transaction from the
 * AM's two-phase state.  The waiter sleeps on the top-level xid.
 */
void
SLogTupleLockRestore(FullTransactionId fxid, Oid relid, RelFileNumber relnumber,
					 ItemPointer tid, LockTupleMode mode)
{
	TransactionId xid = XidFromFullTransactionId(fxid);
	int			slotno = TwoPhaseGetDummyProcNumber(fxid, false);
	SLogTupleLockShared *dir = slog_tl_directory();
	SLogTupleLockSlot *slot = slog_tl_slot(dir, slotno);
	SLogTupleLockRel *rel;
	uint64		v = 0;
	uint64		val = SLOG_TL_VALUE(xid, mode);
	int			r;

	slot->xid = xid;
	slog_tl_index_set(dir, slotno, slog_tl_bucket(relid, relnumber, tid));
	r = slog_tl_find_inline(slot, relid, relnumber, tid);
	if (r >= 0)
	{
		if (SLOG_TL_MODE(slot->inl[r].lev[0]) < mode)
			slog_tl_write_inline(slot, r, relid, relnumber, tid, 1, &val);
		return;
	}

	rel = slot->nrels > 0 ? slog_tl_find_rel(slot, relid, relnumber) : NULL;
	if (rel != NULL)
		v = slog_tl_lookup(rel, slog_tl_key(tid, 0));
	if (v == 0 && (r = slog_tl_free_inline(slot)) >= 0)
		slog_tl_write_inline(slot, r, relid, relnumber, tid, 1, &val);
	else if (v == 0 || SLOG_TL_MODE(v) < mode)
		slog_tl_table_write(slotno, relid, relnumber, tid, 1, &val, 0);
}

/*
 * COMMIT/ROLLBACK PREPARED: release the prepared transaction's row locks.
 * Called with TwoPhaseStateLock held, once per row lock the AM wrote to its
 * two-phase state; the first call frees them all.
 */
void
SLogTupleLockReleasePrepared(FullTransactionId fxid)
{
	int			slotno = TwoPhaseGetDummyProcNumber(fxid, true);
	SLogTupleLockShared *dir = slog_tl_directory();

	if (TransactionIdEquals(slog_tl_slot(dir, slotno)->xid,
							XidFromFullTransactionId(fxid)))
		slog_tl_free_slot(slotno, false);
}
