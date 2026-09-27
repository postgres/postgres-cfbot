/*-------------------------------------------------------------------------
 *
 * slog_tuple.c
 *	  Tuple sLog -- optional per-tuple UNDO tracking extension
 *
 * The tuple sLog is an OPTIONAL extension of the sLog subsystem (slog.c).
 * It provides bounded-recovery-time uncommitted-writer tracking for in-place
 * MVCC table access methods: a flat, partitioned, seqlock-guarded hash of
 * in-flight per-tuple operations, keyed by (relid, tid).  Because the
 * uncommitted writers of every hot tuple are recorded in shared memory,
 * recovery and cross-backend visibility need not scan an unbounded UNDO
 * chain to decide whether a given physical tuple version is visible; the
 * hash answers "is there an in-flight/aborted writer of this tuple?" in
 * O(1), which is what bounds recovery and read cost.
 *
 * The flat hash and its shared-memory partitions are carved from the same
 * shared segment as the transaction sLog (see slog.c's SLogShmemInit); the
 * two facilities share only that segment and its initialization, never each
 * other's data structures.
 *
 * WAL-free: entries are transient, removed at commit/abort.
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

#include <unistd.h>
#ifdef WIN32
#include <windows.h>
#endif

#include "access/slog.h"
#include "access/slog_flathash.h"
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
#include "utils/dsa.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"


/*
 * slog_num_cpus - portable replacement for sysconf(_SC_NPROCESSORS_ONLN).
 *
 * Returns the number of online CPUs (or a sane positive fallback if the
 * platform refuses to answer).  Used to auto-size the sLog flat-hash
 * partition count when the GUC is left at zero.
 */
static int
slog_num_cpus(void)
{
#ifdef WIN32
	SYSTEM_INFO si;

	GetSystemInfo(&si);
	if (si.dwNumberOfProcessors > 0)
		return (int) si.dwNumberOfProcessors;
	return 4;
#else
	long		n = sysconf(_SC_NPROCESSORS_ONLN);

	if (n <= 0)
		return 4;
	return (int) n;
#endif
}


/*
 * Maximum number of abort ops applied to a partition between seqlock
 * begin/end cycles in SLogTupleMarkAborted / SLogTupleCleanupRetained.
 *
 * The seqlock keeps its counter odd for the whole write cycle, so any
 * concurrent wait-free reader spins until the counter turns even.  A very
 * large rollback (hundreds of thousands of tuples applied in one cycle)
 * would pin readers spinning for the entire batch.  Ending and re-beginning
 * the seqlock every SLOG_ABORT_PUBLISH_BATCH ops bounds that odd-hold
 * window; the partition writer_lock is held across the whole function, so
 * dropping to even at a batch boundary leaves the partition consistent for
 * any reader that observes it.
 */
#define SLOG_ABORT_PUBLISH_BATCH	24


/*
 * Wall-clock sampling period for the self-clocking throttles in
 * SLogTupleInsert() and SLogTupleMaybeCleanupRetained().  GetCurrentTimestamp()
 * is a clock_gettime() syscall that dominates the per-tuple hot path, so we
 * read it once every SLOG_INSERT_CLOCK_PERIOD calls and reuse the cached
 * horizon in between.  Must be a power of two (callers mask with PERIOD - 1).
 */
#define SLOG_INSERT_CLOCK_PERIOD	256


/*
 * SLogTupleNumEntries
 *		Calculate the number of hash entries for the tuple sLog.
 *
 * Auto-sizing formula: MaxBackends * 1024, clamped to [4096, 4194304].
 */
int
SLogTupleNumEntries(void)
{
	int			n = MaxBackends * SLOG_TUPLE_PER_BACKEND_SLOTS;

	n = Max(n, SLOG_TUPLE_MIN_ENTRIES);
	n = Min(n, SLOG_TUPLE_MAX_ENTRIES);
	return n;
}



/*
 * GUC: slog_num_partitions; number of flat hash partitions.
 * 0 = auto (heuristic: 4 x NumCPUs, clamped [16..256], power of 2).
 * Set at postmaster startup, immutable thereafter.
 */
int			slog_num_partitions = 0;

/* Runtime partition count (set once during SLogShmemInit, read everywhere) */
int			SLogNumPartitions = SLOG_FLAT_DEFAULT_PARTITIONS;

/*
 * Compute the effective partition count from the GUC value.
 * Called once during SLogShmemInit.
 */
static int
SLogComputeNumPartitions(void)
{
	int			n;

	if (slog_num_partitions > 0)
	{
		/* Explicit GUC value; clamp and round to power of 2 */
		n = slog_num_partitions;
	}
	else
	{
		/* Auto-size: 4 x number of CPUs */
		int			ncpus = slog_num_cpus();

		n = ncpus * 4;
	}

	/* Clamp */
	n = Max(n, SLOG_FLAT_MIN_PARTITIONS);
	n = Min(n, SLOG_FLAT_MAX_PARTITIONS);

	/* Round up to next power of 2 (for fast modulo via bitmask) */
	{
		int			p = 1;

		while (p < n)
			p <<= 1;
		n = p;
	}

	return n;
}

/*
 * Flat hash capacity: round up SLogTupleNumEntries to next power of 2,
 * then divide by 0.7 (max load factor) to ensure probe chains stay short.
 */
static inline int
SLogFlatHashCapacity(void)
{
	int			n = SLogTupleNumEntries();
	int			cap;

	/* Target: num_entries / capacity <= 0.7, so capacity >= n / 0.7 */
	cap = (int) ((double) n / 0.7) + 1;

	/* Round up to next power of 2 */
	cap--;
	cap |= cap >> 1;
	cap |= cap >> 2;
	cap |= cap >> 4;
	cap |= cap >> 8;
	cap |= cap >> 16;
	cap++;

	/* Clamp to reasonable bounds */
	if (cap < 2048)
		cap = 2048;
	if (cap > 2 * 1048576)
		cap = 2 * 1048576;

	return cap;
}

/* ----------------------------------------------------------------
 * Static variables (tuple sLog)
 * ----------------------------------------------------------------
 */

/* Pointer to ShmemAlloc'd flat-hash region, set by ShmemRequestStruct framework */
static char *SLogFlatHashBlock = NULL;	/* single allocation for all partition
										 * flat-hash copies */

/* Rate-limiting for sLog overflow warnings (per-backend) */
static int	slog_overflow_warning_count = 0;
static TimestampTz slog_overflow_last_warning = 0;


/* ----------------------------------------------------------------
 * Backend-private tracking for tuple sLog cleanup
 * ----------------------------------------------------------------
 */
typedef struct SLogTrackedKey
{
	SLogTupleKey key;
	TransactionId xid;
	TransactionId subxid;
	bool		local_only;		/* no shared hash entry (INSERT-only) */
	SLogOpType	op_type;		/* DML type (for commit retention decisions) */

	struct SLogTrackedKey *next;
} SLogTrackedKey;

static SLogTrackedKey *slog_tracked_keys = NULL;
static bool slog_has_shared_entries = false;	/* any non-local_only entries? */

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
	uint64		encoded_tid;	/* SLOG_ENCODE_TID(blkno, offnum) */
} SLogInsertTidKey;

typedef struct SLogInsertTidEntry
{
	SLogInsertTidKey key;		/* (relid, encoded_tid) */
	char		status;			/* required by simplehash */
} SLogInsertTidEntry;

static inline uint32
slog_insert_tid_hash(Oid relid, uint64 encoded_tid)
{
	uint64		h = hash_combine64((uint64) hash_uint32((uint32) relid),
								   murmurhash64(encoded_tid));

	return (uint32) (h ^ (h >> 32));
}

#define SH_PREFIX sloginsert
#define SH_ELEMENT_TYPE SLogInsertTidEntry
#define SH_KEY_TYPE SLogInsertTidKey
#define SH_KEY key
#define SH_HASH_KEY(tb, key) slog_insert_tid_hash((key).relid, (key).encoded_tid)
#define SH_EQUAL(tb, a, b) ((a).relid == (b).relid && (a).encoded_tid == (b).encoded_tid)
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

/* ----------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------
 */

/*
 * Partition accessor helpers for the tuple sLog.
 *
 * These inline functions encapsulate the key->partition routing so that
 * callers don't need to repeat the pattern.
 */
static inline SLogFlatPartition *
SLogGetPartition(const SLogTupleKey *key)
{
	int			part = SLogFlatHashPartitionIndex(key);

	return &SLogState->tuple_partitions[part];
}

static inline SLogFlatPartition *
SLogGetPartitionByIndex(int part)
{
	Assert(part >= 0 && part < SLogNumPartitions);
	return &SLogState->tuple_partitions[part];
}

/*
 * Convenience macros for partition-routed locking.
 *
 * Most functions have a local `key` variable of type SLogTupleKey and need
 * to route to the correct partition.  These macros minimize the diff from
 * the old single-lock code.  The `fp__` variable is defined in-scope by
 * SLOG_PART_READ_BEGIN / SLOG_PART_WRITE_BEGIN.
 */
#define SLOG_PART_WRITER_LOCK(key_ptr) \
	(&SLogGetPartition(key_ptr)->writer_lock.lock)

/*
 * SLogPartApplyOne
 *		Single-op write helper: acquire writer_lock, one seqlock cycle, one
 *		apply, release.  Covers the many write sites that apply exactly one
 *		SLogFlatOp to fp->hash.
 */
static inline void
SLogPartApplyOne(SLogFlatPartition *fp, const SLogFlatOp *op)
{
	LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);
	SLogFlatHashApply(fp->hash, op, sizeof(*op));
	LWLockRelease(&fp->writer_lock.lock);
}

/* ----------------------------------------------------------------
 * Tuple sLog shared-memory sizing and initialization
 *
 * Called from slog.c's SLogShmemSize/Request/Init so the tuple flat-hash
 * partitions share the sLog's shared segment.  Keeping the tuple-specific
 * shmem code here (rather than in slog.c) keeps slog.c free of any
 * flat-hash/partition knowledge.
 * ----------------------------------------------------------------
 */

/*
 * SLogTupleShmemSize
 *		Additional shared memory needed for the tuple flat-hash partitions.
 */
Size
SLogTupleShmemSize(void)
{
	return SLogFlatHashPartitionedShmemSize(SLogFlatHashCapacity(), MaxBackends);
}

/*
 * SLogTupleShmemRequest
 *		Register the tuple flat-hash partition block.
 */
void
SLogTupleShmemRequest(void)
{
	/* Compute partition count early so shmem sizing is correct */
	SLogNumPartitions = SLogComputeNumPartitions();

	ShmemRequestStruct(.name = "sLog Flat Hash Partitions",
					   .size = SLogFlatHashPartitionedShmemSize(
																SLogFlatHashCapacity(), MaxBackends),
					   .ptr = (void **) &SLogFlatHashBlock,
		);
}

/*
 * SLogTupleShmemInit
 *		Allocate and initialize the tuple flat-hash partitions.
 *
 * Invoked from SLogShmemInit() after the shared state struct exists.  Runs
 * unconditionally (whether or not an AM has opted in) so the two sLog files
 * share one segment with no init-ordering dependency; an idle flat hash
 * costs only its shared-memory footprint.
 */
void
SLogTupleShmemInit(void)
{
	int			total_capacity;
	int			per_part_cap;
	Size		per_part_shmem_size;
	char	   *block_ptr;
	int			part;

	/* ---- Compute and set partition count (once, globally) ---- */
	SLogNumPartitions = SLogComputeNumPartitions();
	SLogState->num_partitions = SLogNumPartitions;

	/* Allocate partition array in shared memory (after SLogState) */
	SLogState->tuple_partitions = (SLogFlatPartition *)
		ShmemAlloc(sizeof(SLogFlatPartition) * SLogNumPartitions);
	memset(SLogState->tuple_partitions, 0,
		   sizeof(SLogFlatPartition) * SLogNumPartitions);

	ereport(DEBUG1,
			(errmsg("sLog: %d flat hash partitions (slog_num_partitions=%d, CPUs=%d)",
					SLogNumPartitions, slog_num_partitions,
					slog_num_cpus())));

	/* ---- Initialize partitioned seqlock flat hashes ---- */
	total_capacity = SLogFlatHashCapacity();
	per_part_cap = total_capacity / SLogNumPartitions;
	if (per_part_cap < 64)
		per_part_cap = 64;

	per_part_shmem_size = MAXALIGN(SLogFlatHashShmemSize(per_part_cap,
														 MaxBackends));
	block_ptr = SLogFlatHashBlock;

	for (part = 0; part < SLogNumPartitions; part++)
	{
		SLogFlatPartition *fp = &SLogState->tuple_partitions[part];

		/* Initialize per-partition writer lock */
		LWLockInitialize(&fp->writer_lock.lock, LWTRANCHE_SLOG);

		/* Single flat-hash copy carved from the shared block */
		fp->hash = (SLogFlatHash *) block_ptr;
		SLogFlatHashInit(fp->hash, per_part_cap);

		block_ptr += per_part_shmem_size;
	}
}

/* ================================================================
 * Tuple sLog functions
 * ================================================================
 */

/* ----------------------------------------------------------------
 * Emergency eviction
 * ----------------------------------------------------------------
 */

/*
 * SLogTupleEvictCommitted -- evict entries for committed transactions.
 *
 * Called when the sLog flat hash is full.  Scans each partition under its
 * writer lock to collect evictable keys, then applies REMOVE_ENTRY ops.
 * (This is a cold path, run only when a partition is full; holding the
 * writer lock across the scan is simpler than a seqlock retry loop and the
 * per-op TransactionIdIsInProgress/DidCommit probes must not run repeatedly.)
 *
 * Returns the number of entries evicted.
 */
static int
SLogTupleEvictCommitted(void)
{
	SLogTupleKey *keys_to_evict;
	int			nkeys = 0;
	int			max_evict = 1024;
	int			part;

	keys_to_evict = (SLogTupleKey *)
		palloc(sizeof(SLogTupleKey) * max_evict);

	/* First pass: scan each partition under its writer lock to collect keys */
	for (part = 0; part < SLogNumPartitions && nkeys < max_evict; part++)
	{
		SLogFlatPartition *fp = SLogGetPartitionByIndex(part);
		SLogFlatHashScanState scan;
		const SLogFlatBucket *bucket;

		LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);

		SLogFlatHashScanInit(&scan);
		while ((bucket = SLogFlatHashScanNext(fp->hash, &scan)) != NULL)
		{
			const SLogTupleEntry *entry = &bucket->entry;
			bool		all_committed = true;
			bool		has_any_op = false;
			int			i;

			for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
			{
				if (!entry->ops[i].in_use)
					continue;
				has_any_op = true;

				if (TransactionIdIsInProgress(entry->ops[i].xid) ||
					!TransactionIdDidCommit(entry->ops[i].xid))
				{
					all_committed = false;
					break;
				}
			}

			if (has_any_op && all_committed)
			{
				keys_to_evict[nkeys++] = bucket->key;
				if (nkeys >= max_evict)
					break;
			}
		}

		LWLockRelease(&fp->writer_lock.lock);
	}

	/* Second pass: apply removals grouped by partition */
	if (nkeys > 0)
	{
		int			i;

		for (part = 0; part < SLogNumPartitions; part++)
		{
			SLogFlatPartition *fp = SLogGetPartitionByIndex(part);
			bool		has_keys_for_part = false;

			/* Check if any keys belong to this partition */
			for (i = 0; i < nkeys; i++)
			{
				if (SLogFlatHashPartitionIndex(&keys_to_evict[i]) == part)
				{
					has_keys_for_part = true;
					break;
				}
			}
			if (!has_keys_for_part)
				continue;

			LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);

			for (i = 0; i < nkeys; i++)
			{
				SLogFlatOp	flat_op;

				if (SLogFlatHashPartitionIndex(&keys_to_evict[i]) != part)
					continue;

				memset(&flat_op, 0, sizeof(flat_op));
				flat_op.kind = SLOG_FLAT_OP_REMOVE_ENTRY;
				flat_op.key = keys_to_evict[i];
				SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));
			}
			LWLockRelease(&fp->writer_lock.lock);
		}
	}

	pfree(keys_to_evict);

	return nkeys;
}

/* ----------------------------------------------------------------
 * Core Tuple sLog API
 * ----------------------------------------------------------------
 */

/*
 * SLogTupleInsert
 *		Record a tuple operation in the sLog.
 *
 * Inserts into the seqlock-protected flat hash (wait-free reads).
 * Performs overflow handling before failing.
 *
 * Also adds the key to the backend-private tracking list for cleanup.
 */
bool
SLogTupleInsert(Oid relid, ItemPointer tid, TransactionId xid,
				SLogOpType op_type, TransactionId subxid,
				CommandId cid, TimestampTz commit_ts,
				uint32 spec_token, LockTupleMode lock_mode)
{
	SLogTupleKey key;
	SLogFlatOp	flat_op;
	SLogFlatPartition *fp;
	int			entries_before;
	int			entries_after;

	Assert(SLogState != NULL);
	Assert(TransactionIdIsValid(xid));
	Assert(ItemPointerIsValid(tid));

	/* Zero for deterministic hashing (ItemPointerData is 6 bytes) */
	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	fp = SLogGetPartition(&key);

	/*
	 * Retained UPDATE markers are never evicted by the on-overflow path --
	 * SLogTupleEvictCommitted() leaves any op whose xid is in-progress or not
	 * yet committed in CLOG.  They are cleaned up by
	 * SLogTupleCleanupRetained(), driven by the UNDO background worker or,
	 * when it is disabled, by the access method calling
	 * SLogTupleMaybeCleanupRetained() outside any buffer-locked section. This
	 * function never triggers that sweep itself (see note below).
	 *
	 * Per-TID ops array overflow (SLOG_MAX_TUPLE_OPS=32 slots all full of
	 * retained entries on hot rows) is handled by flat_hash_apply_insert()
	 * which reclaims the oldest retained entry when no free slot exists.
	 */

	{
		/* Build the flat hash op */
		memset(&flat_op, 0, sizeof(flat_op));
		flat_op.kind = SLOG_FLAT_OP_INSERT;
		flat_op.key = key;
		flat_op.xid = xid;
		flat_op.subxid = subxid;

		/*
		 * The xid reclaim horizon is computed LAZILY, not on every insert. It
		 * is only consumed by flat_hash_apply_insert to reclaim a slot when a
		 * hot row's per-TID ops array is completely full; the common path
		 * (free slot, same-xid overwrite, or a slot freed by coalescing)
		 * never reads it.  Computing it here would run a ProcArrayLock-shared
		 * xid-horizon scan on every CAS-update -- a measured hot-path cost --
		 * for a value used only in the rare full-array case.
		 *
		 * So the fast path passes InvalidTransactionId, which now fail-safe
		 * DISABLES reclamation (see flat_hash_apply_insert): an invalid
		 * horizon frees NOTHING.  This preserves the original safety
		 * invariant -- a stale or absent horizon must never cause a lost
		 * update -- in the most conservative direction possible.  If the op
		 * is then dropped because the array was genuinely full, the caller
		 * (below) recomputes the real, authoritative horizon and retries;
		 * reclamation is thus attempted with a fresh horizon exactly when,
		 * and only when, it is actually needed.
		 */
		flat_op.reclaim_xid_horizon = InvalidTransactionId;
		flat_op.tuple_op.xid = xid;
		flat_op.tuple_op.subxid = subxid;
		flat_op.tuple_op.op_type = op_type;
		flat_op.tuple_op.cid = cid;
		flat_op.tuple_op.commit_ts = commit_ts;
		flat_op.tuple_op.spec_token = spec_token;
		flat_op.tuple_op.lock_mode = lock_mode;
		flat_op.tuple_op.in_use = true;
	}

	/* Apply to flat hash via seqlock writer path (partition-local) */
	LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);

	/* Check capacity before insert (writer lock held: fp->hash is stable) */
	entries_before = fp->hash->num_entries;
	SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));

	/* Check if insert succeeded */
	entries_after = fp->hash->num_entries;

	LWLockRelease(&fp->writer_lock.lock);

	/*
	 * Verify the op was actually stored.  We must probe for THIS xid's op,
	 * not merely the bucket: on a hot row the bucket pre-exists (it holds
	 * other TIDs' / xids' markers), so num_entries is unchanged and the
	 * bucket is present even when flat_hash_apply_insert silently dropped our
	 * op because the per-TID ops array was full with nothing reclaimable.
	 * Testing only bucket presence (the old SLogFlatHashProbe != NULL)
	 * reports success for a dropped op, so no marker exists to stamp at
	 * PRE_COMMIT and the next concurrent writer clobbers this update -- a
	 * lost update.  Probe the ops array for our xid instead.
	 */
	if (entries_after == entries_before)
	{
		bool		op_stored;

		LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);
		op_stored = SLogFlatHashHasOpForXid(fp->hash, &key, xid);
		LWLockRelease(&fp->writer_lock.lock);

		if (!op_stored)
		{
			/*
			 * The op was dropped: the per-TID ops array was full and the fast
			 * path passed an invalid horizon, which disables reclamation.  On
			 * a hot row the array is full of THIS relation's own
			 * below-horizon UPDATE markers, which the reclaim path can free
			 * WITHOUT the cross-partition SLogTupleEvictCommitted() sweep. So
			 * recompute the real, authoritative horizon now (the
			 * ProcArrayLock scan we skipped on the fast path) and retry:
			 * flat_hash_apply_insert can now reclaim a settled marker and
			 * store our op in place.
			 */
			TransactionId horizon = GetOldestNonRemovableTransactionId(NULL);

			flat_op.reclaim_xid_horizon = horizon;

			LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);
			SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));
			op_stored = SLogFlatHashHasOpForXid(fp->hash, &key, xid);
			LWLockRelease(&fp->writer_lock.lock);
		}

		if (!op_stored)
		{
			/* Table or per-TID array was full; try eviction */
			int			evicted = SLogTupleEvictCommitted();

			if (evicted > 0)
			{
				/* Retry */
				LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);
				SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));
				op_stored = SLogFlatHashHasOpForXid(fp->hash, &key, xid);
				LWLockRelease(&fp->writer_lock.lock);
			}

			if (!op_stored)
			{
				slog_overflow_warning_count++;
				{
					TimestampTz now = GetCurrentTimestamp();

					if (slog_overflow_warning_count == 1 ||
						TimestampDifferenceExceeds(slog_overflow_last_warning,
												   now, 1000))
					{
						elog(WARNING, "sLog tuple hash partition full "
							 "(%d entries); %d overflow(s) this transaction "
							 "on rel %u (visibility relies on UNCOMMITTED "
							 "flag + UNDO replay)",
							 SLogTupleNumEntries() / SLogNumPartitions,
							 slog_overflow_warning_count, relid);
						slog_overflow_last_warning = now;
					}
				}

				SLogTupleTrackLocalOnly(relid, tid, xid, subxid);
				return false;
			}
		}
	}

	SLogTupleTrackKey(key, xid, subxid, op_type);
	return true;
}

/*
 * SLogTupleInsertRecovery
 *		Record a tuple operation during WAL replay (recovery-safe).
 *
 * This is a simplified variant of SLogTupleInsert() designed for use during
 * WAL redo on hot standbys.  Key differences:
 *   - Does NOT call SLogTupleTrackKey() (no backend-local tracking needed)
 *   - Returns false silently if the hash is full (instead of ERROR/PANIC)
 *   - No retries with pg_usleep (would delay WAL replay)
 *
 * Used by an AM's WAL redo path to register UNCOMMITTED tuples in the
 * per-tuple sLog so that the AM's visibility check can correctly determine
 * visibility on standbys (where the sLog is otherwise never populated).
 */
bool
SLogTupleInsertRecovery(Oid relid, ItemPointer tid, TransactionId xid,
						SLogOpType op_type)
{
	SLogTupleKey key;
	SLogFlatOp	clear_op;
	SLogFlatOp	flat_op;

	if (SLogState == NULL)
		return false;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	/*
	 * Clear any pre-existing entry at this exact TID before registering the
	 * new op.  A physical INSERT always targets a free line pointer, so any
	 * sLog entry already present at this TID belongs to a dead prior occupant
	 * (deleted and VACUUM-recycled).  On the primary the prior occupant's
	 * sLog entry is removed by its commit/abort xact callback, but the
	 * standby redo path only ever inserts -- it never removes -- so without
	 * this the stale entry survives.  flat_hash_apply_insert() only
	 * overwrites a same-xid op, so a stale op from the prior occupant's
	 * (different) xid would persist and make the AM's visibility check trip
	 * on TransactionIdDidAbort()/IsInProgress() for that dead xid, wrongly
	 * hiding the freshly inserted live tuple.
	 *
	 * Tombstoning the entry is safe and self-healing:
	 * flat_hash_apply_insert() reuses tombstoned buckets, so the immediately
	 * following INSERT recreates a clean entry holding only this insert's op.
	 */
	memset(&clear_op, 0, sizeof(clear_op));
	clear_op.kind = SLOG_FLAT_OP_REMOVE_ENTRY;
	clear_op.key = key;

	/* Apply to flat hash */
	memset(&flat_op, 0, sizeof(flat_op));
	flat_op.kind = SLOG_FLAT_OP_INSERT;
	flat_op.key = key;
	flat_op.xid = xid;
	flat_op.tuple_op.xid = xid;
	flat_op.tuple_op.subxid = InvalidTransactionId;
	flat_op.tuple_op.op_type = op_type;
	flat_op.tuple_op.cid = InvalidCommandId;
	flat_op.tuple_op.commit_ts = 0;
	flat_op.tuple_op.spec_token = 0;
	flat_op.tuple_op.in_use = true;

	LWLockAcquire(SLOG_PART_WRITER_LOCK(&key), LW_EXCLUSIVE);
	SLogFlatHashApply(SLogGetPartition(&key)->hash, &clear_op, sizeof(clear_op));
	SLogFlatHashApply(SLogGetPartition(&key)->hash, &flat_op, sizeof(flat_op));
	LWLockRelease(SLOG_PART_WRITER_LOCK(&key));

	return true;
}

/*
 * Restore a prepared row lock without replacing other holders at this TID.
 * Unlike physical INSERT replay, the tuple has not acquired a new occupant.
 * Recovery must fail rather than silently lose a lock if the sLog is full.
 */
void
SLogTupleRestoreLock(Oid relid, ItemPointer tid, TransactionId xid,
					 LockTupleMode mode)
{
	SLogFlatOp op = {0};
	SLogFlatPartition *fp;
	bool		stored;

	op.kind = SLOG_FLAT_OP_INSERT;
	op.key.relid = relid;
	ItemPointerCopy(tid, &op.key.tid);
	op.xid = xid;
	op.tuple_op.xid = xid;
	op.tuple_op.op_type = mode <= LockTupleShare ?
		SLOG_OP_LOCK_SHARE : SLOG_OP_LOCK_EXCL;
	op.tuple_op.cid = InvalidCommandId;
	op.tuple_op.lock_mode = mode;
	op.tuple_op.in_use = true;
	fp = SLogGetPartition(&op.key);

	LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);
	SLogFlatHashApply(fp->hash, &op, sizeof(op));
	stored = SLogFlatHashHasOpForXid(fp->hash, &op.key, xid);
	LWLockRelease(&fp->writer_lock.lock);

	if (!stored)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not restore prepared row lock in sLog")));
}

/*
 * SLogTupleLookup
 *		Look up a tuple's sLog entry (copy semantics).
 *
 * Returns true if found, copying the full entry into *entry_out.
 * WAIT-FREE: uses the seqlock read-side retry loop.
 */
bool
SLogTupleLookup(Oid relid, ItemPointer tid, SLogTupleEntry *entry_out)
{
	SLogTupleKey key;
	SLogFlatPartition *fp;
	bool		found;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	fp = SLogGetPartition(&key);

	SLOG_SEQ_READ_BEGIN(fp)
	{
		const SLogFlatBucket *bucket = SLogFlatHashProbe(fp->hash, &key);

		found = (bucket != NULL);
		if (found && entry_out)
			memcpy(entry_out, &bucket->entry, sizeof(SLogTupleEntry));
	}
	SLOG_SEQ_READ_END(fp);

	return found;
}

/*
 * SLogTupleLookupFiltered
 *		Find sLog entries for a TID, optionally filtered by xid.
 *
 * WAIT-FREE: uses the seqlock read-side.  If xid_filter is valid, returns
 * only ops for that xid.  If InvalidTransactionId, returns all active
 * ops for this TID.
 *
 * Returns the number of ops written to ops_out.
 */
int
SLogTupleLookupFiltered(Oid relid, ItemPointer tid,
						TransactionId xid_filter,
						SLogTupleOp *ops_out, int max_ops)
{
	SLogTupleKey key;
	SLogFlatPartition *fp;
	int			nfound = 0;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	fp = SLogGetPartition(&key);

	SLOG_SEQ_READ_BEGIN(fp)
	{
		const SLogFlatBucket *bucket = SLogFlatHashProbe(fp->hash, &key);

		if (bucket != NULL)
		{
			const SLogTupleEntry *entry = &bucket->entry;
			int			i;

			for (i = 0; i < SLOG_MAX_TUPLE_OPS && nfound < max_ops; i++)
			{
				if (!entry->ops[i].in_use)
					continue;

				if (TransactionIdIsValid(xid_filter) &&
					!TransactionIdEquals(entry->ops[i].xid, xid_filter))
					continue;

				memcpy(&ops_out[nfound], &entry->ops[i], sizeof(SLogTupleOp));
				nfound++;
			}
		}
	}
	SLOG_SEQ_READ_END(fp);

	return nfound;
}

/*
 * SLogTupleRemove
 *		Remove operations for a specific xid from a tuple entry.
 *
 * Uses the seqlock writer path with external (writer_lock) serialization.
 */
void
SLogTupleRemove(Oid relid, ItemPointer tid, TransactionId xid)
{
	SLogTupleKey key;
	SLogFlatOp	op;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	memset(&op, 0, sizeof(op));
	op.kind = SLOG_FLAT_OP_REMOVE_XID;
	op.key = key;
	op.xid = xid;

	SLogPartApplyOne(SLogGetPartition(&key), &op);
}

/*
 * Width of the touched-partition bitmap, in uint64 words.  Sized to cover
 * SLOG_FLAT_MAX_PARTITIONS so the bitmap is a fixed on-stack array regardless
 * of the runtime partition count.
 */
#define SLOG_PART_BITMAP_WORDS	((SLOG_FLAT_MAX_PARTITIONS + 63) / 64)

/*
 * SLogCollectTrackedPartitions
 *		Build a bitmap of the partitions touched by xid's shared tracked keys.
 *
 * Walks slog_tracked_keys once, setting one bit per distinct partition that
 * holds a non-local_only entry for xid.  Callers then iterate only the set
 * partitions instead of sweeping all SLogNumPartitions, turning the batch
 * apply from O(num_partitions * tracked_keys) into
 * O(touched_partitions * tracked_keys) -- optimal for the common single-row,
 * single-partition transaction.
 *
 * Returns the number of distinct partitions marked.
 */
static int
SLogCollectTrackedPartitions(TransactionId xid,
							 uint64 *bitmap)
{
	SLogTrackedKey *tk;
	int			nparts = 0;

	memset(bitmap, 0, sizeof(uint64) * SLOG_PART_BITMAP_WORDS);

	for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
	{
		int			part;
		uint64		mask;

		if (!TransactionIdEquals(tk->xid, xid) || tk->local_only)
			continue;

		part = SLogFlatHashPartitionIndex(&tk->key);
		mask = UINT64CONST(1) << (part & 63);
		if (!(bitmap[part >> 6] & mask))
		{
			bitmap[part >> 6] |= mask;
			nparts++;
		}
	}

	return nparts;
}

/*
 * SLogTupleRemoveByXid
 *		Remove all tuple sLog entries for a transaction.
 *
 * Uses the backend-local tracking list.  Applies REMOVE_XID ops to the
 * flat hash in a single writer batch.
 */
void
SLogTupleRemoveByXid(TransactionId xid)
{
	SLogTrackedKey *tk;
	uint64		touched[SLOG_PART_BITMAP_WORDS] = {0};
	int			part;

	if (SLogState == NULL)
		return;

	/*
	 * Collect the partitions xid actually touches in one pass.  Walking only
	 * those avoids an O(num_partitions * tracked_keys) sweep over every
	 * partition for what is usually a single-row transaction.
	 */
	if (SLogCollectTrackedPartitions(xid, touched) == 0)
		return;

	/* Batch apply REMOVE_XID ops grouped by partition */
	for (part = 0; part < SLogNumPartitions; part++)
	{
		SLogFlatPartition *fp;

		if (!(touched[part >> 6] & (UINT64CONST(1) << (part & 63))))
			continue;

		fp = SLogGetPartitionByIndex(part);

		LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);

		for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
		{
			SLogFlatOp	flat_op;

			if (!TransactionIdEquals(tk->xid, xid) || tk->local_only)
				continue;
			if (SLogFlatHashPartitionIndex(&tk->key) != part)
				continue;

			memset(&flat_op, 0, sizeof(flat_op));
			flat_op.kind = SLOG_FLAT_OP_REMOVE_XID;
			flat_op.key = tk->key;
			flat_op.xid = xid;
			SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));
		}
		LWLockRelease(&fp->writer_lock.lock);
	}
}

/*
 * SLogTupleCommitByXid
 *		Handle commit for tuple sLog: remove ALL of the committing xid's ops
 *		(INSERT/DELETE/LOCK and UPDATE alike).
 *
 * Committed-UPDATE markers are NOT retained on the flat hash.
 * Snapshot-isolation readers reconstruct the visible version by walking the
 * durable UNDO log chain (via the AM's version-reconstruction walk), so at
 * commit every op of the xid is removed -- identical to INSERT/DELETE/LOCK --
 * which also drains bucket table_full pressure.
 *
 * Uses the backend-local tracking list.  Applies COMMIT_XID ops to the
 * flat hash in batch.
 */
void
SLogTupleCommitByXid(TransactionId xid)
{
	SLogTrackedKey *tk;
	uint64		touched[SLOG_PART_BITMAP_WORDS] = {0};

	if (SLogState == NULL)
		return;

	/* Fast path: INSERT-only transactions never touch the shared hash */
	if (!slog_has_shared_entries)
		return;

	/*
	 * Collect the partitions xid touches in one pass; the COMMIT_XID batch
	 * below iterates only those rather than sweeping all SLogNumPartitions.
	 */
	if (SLogCollectTrackedPartitions(xid, touched) == 0)
		return;

	/*
	 * Committed-UPDATE cross-backend before-images are not published to
	 * shared DSA, and committed-UPDATE markers are not retained on the flat
	 * hash.  Snapshot-isolation readers reconstruct the visible version by
	 * walking the durable UNDO log chain (via the AM's version-reconstruction
	 * walk); the write-write conflict probe reads the head verptr on the
	 * on-page tuple and resolves it in the same chain.
	 *
	 * flat_hash_apply_commit_xid therefore removes every op of xid at commit
	 * -- identical to INSERT/DELETE/LOCK -- which drains bucket table_full.
	 */

	/* Batch apply COMMIT_XID ops grouped by partition */
	{
		int			part;

		for (part = 0; part < SLogNumPartitions; part++)
		{
			SLogFlatPartition *fp;

			if (!(touched[part >> 6] & (UINT64CONST(1) << (part & 63))))
				continue;

			fp = SLogGetPartitionByIndex(part);

			LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);

			for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
			{
				SLogFlatOp	flat_op;

				if (!TransactionIdEquals(tk->xid, xid) || tk->local_only)
					continue;
				if (SLogFlatHashPartitionIndex(&tk->key) != part)
					continue;

				memset(&flat_op, 0, sizeof(flat_op));
				flat_op.kind = SLOG_FLAT_OP_COMMIT_XID;
				flat_op.key = tk->key;
				flat_op.xid = xid;
				SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));
			}
			LWLockRelease(&fp->writer_lock.lock);
		}
	}
}

/*
 * SLogTupleRemoveByXidSingle
 *		Remove the sLog entry for a single tuple identified by (relid, tid, xid).
 *
 * Used by an AM's two-phase postcommit callback to clean up sLog entries
 * one at a time (since the local tracking list is unavailable in the
 * resolving backend).
 */
void
SLogTupleRemoveByXidSingle(Oid relid, ItemPointer tid, TransactionId xid)
{
	SLogTupleKey key;
	SLogFlatOp	flat_op;

	if (SLogState == NULL)
		return;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	/* Apply to flat hash */
	memset(&flat_op, 0, sizeof(flat_op));
	flat_op.kind = SLOG_FLAT_OP_REMOVE_XID;
	flat_op.key = key;
	flat_op.xid = xid;

	SLogPartApplyOne(SLogGetPartition(&key), &flat_op);
}

/*
 * SLogTupleMarkAbortedSingle
 *		Mark the sLog entry for a single tuple as ABORTED.
 *
 * Used by an AM's two-phase postabort callback to mark sLog entries
 * one at a time (since the local tracking list is unavailable in the
 * resolving backend).  Only operates on tuples that already have a shared
 * sLog entry (DELETE/UPDATE operations).
 */
void
SLogTupleMarkAbortedSingle(Oid relid, ItemPointer tid, TransactionId xid)
{
	SLogTupleKey key;
	SLogFlatOp	flat_op;

	if (SLogState == NULL)
		return;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	/* Apply to flat hash */
	memset(&flat_op, 0, sizeof(flat_op));
	flat_op.kind = SLOG_FLAT_OP_MARK_ABORTED;
	flat_op.key = key;
	flat_op.xid = xid;

	SLogPartApplyOne(SLogGetPartition(&key), &flat_op);
}

/*
 * SLogTupleRemoveBySubXid
 *		Handle subtransaction abort for tuple sLog.
 *
 * For entries that have a shared sLog entry, marks matching ops as ABORTED.
 * For entries that only have local tracking (from SLogTupleTrackLocalOnly,
 * used by INSERT), creates a shared ABORTED entry so visibility code can
 * find it.
 */
void
SLogTupleRemoveBySubXid(TransactionId xid, TransactionId subxid)
{
	SLogTrackedKey *tk;

	if (SLogState == NULL)
		return;

	for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
	{
		SLogFlatOp	flat_op;

		if (!TransactionIdEquals(tk->xid, xid))
			continue;
		if (tk->subxid != subxid)
			continue;

		memset(&flat_op, 0, sizeof(flat_op));

		if (tk->local_only)
		{
			/*
			 * INSERT elision: no shared entry exists yet.  Create one with
			 * SLOG_OP_ABORTED so visibility code can find it.
			 */
			flat_op.kind = SLOG_FLAT_OP_CREATE_ABORTED;
			flat_op.key = tk->key;
			flat_op.xid = xid;
			flat_op.subxid = subxid;
		}
		else
		{
			/* Shared entry exists -- mark matching ops ABORTED */
			flat_op.kind = SLOG_FLAT_OP_MARK_ABORTED;
			flat_op.key = tk->key;
			flat_op.xid = xid;
		}

		LWLockAcquire(SLOG_PART_WRITER_LOCK(&tk->key), LW_EXCLUSIVE);
		SLogFlatHashApply(SLogGetPartition(&tk->key)->hash, &flat_op,
						  sizeof(flat_op));
		LWLockRelease(SLOG_PART_WRITER_LOCK(&tk->key));

		/*
		 * Mark the backend-local tracked key as aborted so the AM's
		 * commit-time flag clearing skips it.  Without this, a local-only
		 * INSERT tracked key still reads as a live INSERT at top-level
		 * commit, and the AM's uncommitted-flag clearing would clear the
		 * tuple's UNCOMMITTED flag and stamp a commit marker -- resurrecting
		 * a tuple that the savepoint rollback was supposed to discard.
		 */
		tk->op_type = SLOG_OP_ABORTED;
	}
}

/*
 * SLogTupleUpdateSubXid
 *		Re-parent ops on subtransaction commit.
 *
 * When a subtransaction commits, its entries' subxid is updated to the
 * parent's subxid so they survive subtransaction commit but are cleaned
 * up at top-level commit.
 */
void
SLogTupleUpdateSubXid(TransactionId xid,
					  TransactionId old_subxid,
					  TransactionId new_subxid)
{
	SLogTrackedKey *tk;

	if (SLogState == NULL)
		return;

	for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
	{
		if (!TransactionIdEquals(tk->xid, xid))
			continue;
		if (tk->subxid != old_subxid)
			continue;

		/* Re-parent the local entry */
		tk->subxid = new_subxid;

		/* Also re-parent in the shared sLog if an entry exists */
		if (!tk->local_only)
		{
			SLogFlatOp	flat_op;

			memset(&flat_op, 0, sizeof(flat_op));
			flat_op.kind = SLOG_FLAT_OP_UPDATE_OP;
			flat_op.key = tk->key;
			flat_op.xid = xid;
			flat_op.subxid = new_subxid;
			flat_op.tuple_op.subxid = old_subxid;

			LWLockAcquire(SLOG_PART_WRITER_LOCK(&tk->key), LW_EXCLUSIVE);
			SLogFlatHashApply(SLogGetPartition(&tk->key)->hash, &flat_op,
							  sizeof(flat_op));
			LWLockRelease(SLOG_PART_WRITER_LOCK(&tk->key));
		}
	}
}

/*
 * SLogTupleMarkAborted
 *		Mark all ops for a transaction as SLOG_OP_ABORTED.
 *
 * Called at transaction abort.  Entries remain so visibility checks can
 * distinguish "committed (no entry)" from "aborted (UNDO pending)".
 *
 * For local-only entries (INSERT elision), we CREATE a shared ABORTED
 * entry at abort time.  This is safe because:
 *   (a) Abort is uncommon (vast majority of transactions commit)
 *   (b) The UNDO worker removes both the sLog entry and the page tuple,
 *       bounding the lifetime of these entries
 *   (c) If the hash is full, we log a warning; the UNDO worker will
 *       eventually remove the tuple physically, resolving the anomaly
 */
void
SLogTupleMarkAborted(TransactionId xid)
{
	SLogTrackedKey *tk;
	int			part;
	int			ops_since_publish;

	if (SLogState == NULL)
		return;

	/*
	 * Process all entries grouped by partition.  For each partition, acquire
	 * its writer lock once, apply all relevant ops, then release.
	 */
	for (part = 0; part < SLogNumPartitions; part++)
	{
		SLogFlatPartition *fp = SLogGetPartitionByIndex(part);
		bool		has_entries = false;

		/* Quick check: any local-only INSERT entries at all? */
		if (slog_insert_tids != NULL && slog_insert_tids->members > 0)
			has_entries = true; /* conservative; filtered per-entry below */

		/* Check linked-list entries */
		if (!has_entries)
		{
			for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
			{
				if (!TransactionIdEquals(tk->xid, xid))
					continue;
				if (SLogFlatHashPartitionIndex(&tk->key) == part)
				{
					has_entries = true;
					break;
				}
			}
		}

		if (!has_entries &&
			(slog_insert_tids == NULL || slog_insert_tids->members == 0))
			continue;

		LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);

		/*
		 * Bound the seqlock odd-hold window on a very large rollback
		 * (hundreds of thousands of tuples).  While seq is odd every
		 * wait-free reader of this partition spins; ending and re-beginning
		 * the seqlock every SLOG_ABORT_PUBLISH_BATCH ops lets those readers
		 * make progress.  Correctness across the boundary: each tuple's abort
		 * visibility is resolved independently and each seq cycle leaves
		 * fp->hash consistent, so a reader that observes the even counter at
		 * a boundary sees a valid partial state.
		 */
		ops_since_publish = 0;

		/* Process backend-local INSERT-hash entries for this partition */
		if (slog_insert_tids != NULL)
		{
			sloginsert_iterator it;
			SLogInsertTidEntry *ie;

			sloginsert_start_iterate(slog_insert_tids, &it);
			while ((ie = sloginsert_iterate(slog_insert_tids, &it)) != NULL)
			{
				SLogFlatOp	flat_op;
				SLogTupleKey smkey;

				memset(&smkey, 0, sizeof(smkey));
				smkey.relid = ie->key.relid;
				ItemPointerSet(&smkey.tid,
							   SLOG_DECODE_BLKNO(ie->key.encoded_tid),
							   SLOG_DECODE_OFFNUM(ie->key.encoded_tid));

				/* Only process if this key belongs to current partition */
				if (SLogFlatHashPartitionIndex(&smkey) == part)
				{
					memset(&flat_op, 0, sizeof(flat_op));
					flat_op.kind = SLOG_FLAT_OP_CREATE_ABORTED;
					flat_op.key = smkey;
					flat_op.xid = xid;
					flat_op.subxid = InvalidTransactionId;
					SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));

					if (++ops_since_publish >= SLOG_ABORT_PUBLISH_BATCH)
					{
						ops_since_publish = 0;
					}
				}
			}
		}

		/* Process linked-list entries for this partition */
		for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
		{
			SLogFlatOp	flat_op;

			if (!TransactionIdEquals(tk->xid, xid))
				continue;
			if (SLogFlatHashPartitionIndex(&tk->key) != part)
				continue;

			memset(&flat_op, 0, sizeof(flat_op));
			if (tk->local_only)
			{
				flat_op.kind = SLOG_FLAT_OP_CREATE_ABORTED;
				flat_op.key = tk->key;
				flat_op.xid = xid;
				flat_op.subxid = tk->subxid;
			}
			else
			{
				flat_op.kind = SLOG_FLAT_OP_MARK_ABORTED;
				flat_op.key = tk->key;
				flat_op.xid = xid;
			}

			SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));

			if (++ops_since_publish >= SLOG_ABORT_PUBLISH_BATCH)
			{
				ops_since_publish = 0;
			}
		}

		LWLockRelease(&fp->writer_lock.lock);
	}
}

/*
 * SLogTupleRemoveByXidGlobal
 *		Remove ALL ops for a transaction by scanning the shared flat hash.
 *
 * Unlike SLogTupleRemoveByXid, this does not use the backend-local tracking
 * list (which doesn't exist in the UNDO worker process).  Used by the UNDO
 * worker to clean up ABORTED entries after UNDO has been applied.
 */
void
SLogTupleRemoveByXidGlobal(TransactionId xid)
{
	SLogTupleKey *collected_keys;
	int			max_keys;
	int			part;

	if (SLogState == NULL)
		return;

	max_keys = SLogTupleNumEntries();
	if (max_keys <= 0)
		return;

	collected_keys = (SLogTupleKey *)
		palloc(sizeof(SLogTupleKey) * max_keys);

	/*
	 * Process each partition while holding its exclusive writer lock.  The
	 * writer lock serializes all writers for the partition, so the single
	 * copy is stable while we scan it.  We apply REMOVE_XID inside one
	 * seqlock cycle.
	 */
	for (part = 0; part < SLogNumPartitions; part++)
	{
		SLogFlatPartition *fp = SLogGetPartitionByIndex(part);
		SLogFlatHashScanState scan;
		const SLogFlatBucket *bucket;
		int			nkeys = 0;
		int			i;

		LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);

		/* Scan the single copy; stable because we hold the writer lock. */
		SLogFlatHashScanInit(&scan);
		while ((bucket = SLogFlatHashScanNext(fp->hash, &scan)) != NULL)
		{
			const SLogTupleEntry *entry = &bucket->entry;

			for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
			{
				if (entry->ops[i].in_use &&
					TransactionIdEquals(entry->ops[i].xid, xid))
				{
					if (nkeys < max_keys)
						collected_keys[nkeys++] = bucket->key;
					break;
				}
			}
		}

		/* Drop the slots in one seq cycle. */
		if (nkeys > 0)
		{
			for (i = 0; i < nkeys; i++)
			{
				SLogFlatOp	flat_op;

				memset(&flat_op, 0, sizeof(flat_op));
				flat_op.kind = SLOG_FLAT_OP_REMOVE_XID;
				flat_op.key = collected_keys[i];
				flat_op.xid = xid;
				SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));
			}
		}

		LWLockRelease(&fp->writer_lock.lock);
	}

	pfree(collected_keys);
}

/*
 * SLogTupleIterateByTid
 *		Call a callback for each active operation on a tuple.
 *
 * WAIT-FREE: uses the seqlock read-side.  The in-use ops are copied into a
 * local array under the seqlock retry loop; the callback then runs after a
 * consistent read, so it may have side effects and receives pointers into
 * the local copy (valid for the duration of this call).
 */
void
SLogTupleIterateByTid(Oid relid, ItemPointer tid,
					  SLogTupleIterCallback callback, void *arg)
{
	SLogTupleKey key;
	SLogFlatPartition *fp;
	SLogTupleOp ops[SLOG_MAX_TUPLE_OPS];
	int			nops = 0;
	int			i;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	fp = SLogGetPartition(&key);

	SLOG_SEQ_READ_BEGIN(fp)
	{
		const SLogFlatBucket *bucket = SLogFlatHashProbe(fp->hash, &key);

		if (bucket != NULL)
		{
			const SLogTupleEntry *entry = &bucket->entry;

			for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
				if (entry->ops[i].in_use)
					ops[nops++] = entry->ops[i];
		}
	}
	SLOG_SEQ_READ_END(fp);

	/* Consistent read complete; run the (side-effecting) callback. */
	for (i = 0; i < nops; i++)
		if (!callback(&ops[i], arg))
			break;
}

/* ----------------------------------------------------------------
 * Convenience wrappers
 * ----------------------------------------------------------------
 */

/*
 * SLogTupleHasEntry -- quick probe: does ANY active entry exist for this TID?
 * WAIT-FREE: uses the seqlock read-side.
 */
bool
SLogTupleHasEntry(Oid relid, ItemPointer tid)
{
	SLogTupleKey key;
	SLogFlatPartition *fp;
	bool		has_entry = false;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	fp = SLogGetPartition(&key);

	SLOG_SEQ_READ_BEGIN(fp)
	{
		const SLogFlatBucket *bucket = SLogFlatHashProbe(fp->hash, &key);

		has_entry = (bucket != NULL && bucket->entry.nops > 0);
	}
	SLOG_SEQ_READ_END(fp);

	return has_entry;
}

/*
 * SLogTupleIsInsertedByMe -- check if current transaction inserted this tuple.
 *
 * Checks both the shared hash (normal case) and the backend-local tracking
 * list (for local-only INSERTs that have no shared entry).
 *
 * Uses the top-level XID because SLogTupleTrackLocalOnly() always stores
 * GetTopTransactionId().  This ensures correct results even when called
 * from within a subtransaction (savepoint).
 */
bool
SLogTupleIsInsertedByMe(Oid relid, ItemPointer tid)
{
	SLogTupleOp op;
	int			nfound;
	SLogTrackedKey *tk;
	TransactionId myxid = GetTopTransactionIdIfAny();

	if (!TransactionIdIsValid(myxid))
		return false;

	/*
	 * Special-marker TIDs (SpecTokenOffsetNumber 0xFFFE for speculative
	 * insertions, MovedPartitionsOffsetNumber 0xFFFD for cross-partition
	 * moves) can appear in a tuple's on-page t_ctid.  All sLog entries are
	 * keyed by an on-page (block, offnum) with offnum <= MaxOffsetNumber, so
	 * this backend cannot have "inserted" the tuple under such a key. Bail
	 * out early to avoid tripping the encode-guard elog(ERROR) below.
	 */
	if (ItemPointerGetOffsetNumber(tid) > MaxOffsetNumber)
		return false;

	/* Check shared hash first (normal case) */
	nfound = SLogTupleLookupFiltered(relid, tid, myxid, &op, 1);
	if (nfound > 0 && op.op_type == SLOG_OP_INSERT)
		return true;

	/*
	 * Check the backend-local INSERT hash (top-level local-only INSERTs).
	 */
	{
		BlockNumber blkno = ItemPointerGetBlockNumber(tid);
		OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
		SLogInsertTidKey ikey;

		if (offnum > MaxOffsetNumber)
			elog(ERROR, "offset %u exceeds page item limit %d in sLog TID encoding",
				 offnum, MaxOffsetNumber);
		ikey.relid = relid;
		ikey.encoded_tid = SLOG_ENCODE_TID(blkno, offnum);
		if (slog_insert_tids != NULL &&
			sloginsert_lookup(slog_insert_tids, ikey) != NULL)
			return true;
	}

	/*
	 * Fall back to backend-local tracking list.  This handles the overflow
	 * case where SLogTupleInsert returned false (hash full) but the INSERT
	 * was tracked locally via SLogTupleTrackLocalOnly or SLogTupleTrackKey.
	 * Also handles subtransaction local-only entries (which still use the
	 * linked list).
	 */
	for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
	{
		if (!TransactionIdEquals(tk->xid, myxid))
			continue;
		if (tk->key.relid != relid)
			continue;
		if (ItemPointerEquals(&tk->key.tid, tid))
			return true;
	}

	return false;
}

/*
 * SLogTupleIsDeletedByMe -- check if current transaction deleted this tuple.
 */
bool
SLogTupleIsDeletedByMe(Oid relid, ItemPointer tid)
{
	SLogTupleOp op;
	int			nfound;
	TransactionId myxid = GetCurrentTransactionIdIfAny();

	if (!TransactionIdIsValid(myxid))
		return false;

	nfound = SLogTupleLookupFiltered(relid, tid, myxid, &op, 1);
	return (nfound > 0 &&
			(op.op_type == SLOG_OP_DELETE ||
			 op.op_type == SLOG_OP_UPDATE));
}

/*
 * SLogTupleGetDirtyXid -- for SNAPSHOT_DIRTY, get the xid of the in-progress
 * transaction operating on this tuple.
 *
 * Returns the xid of the first in-progress INSERT or DELETE/UPDATE entry
 * found (excluding our own), or InvalidTransactionId if none.
 *
 * WAIT-FREE: uses the seqlock read-side.  The ops are snapshotted into a
 * local array inside the retry loop; the (LWLock-taking) transaction-status
 * checks then run after a consistent read, never inside the retry loop.
 */
TransactionId
SLogTupleGetDirtyXid(Oid relid, ItemPointer tid, bool *is_insert)
{
	SLogTupleKey key;
	SLogFlatPartition *fp;
	SLogTupleOp ops[SLOG_MAX_TUPLE_OPS];
	int			nops = 0;
	TransactionId result = InvalidTransactionId;
	int			i;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	fp = SLogGetPartition(&key);

	SLOG_SEQ_READ_BEGIN(fp)
	{
		const SLogFlatBucket *bucket = SLogFlatHashProbe(fp->hash, &key);

		if (bucket != NULL)
		{
			const SLogTupleEntry *entry = &bucket->entry;

			for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
				if (entry->ops[i].in_use)
					ops[nops++] = entry->ops[i];
		}
	}
	SLOG_SEQ_READ_END(fp);

	/* Consistent snapshot taken; resolve status outside the shared lock. */
	for (i = 0; i < nops; i++)
	{
		TransactionId xid = ops[i].xid;
		SLogOpType	op = ops[i].op_type;

		if (TransactionIdIsCurrentTransactionId(xid))
			continue;
		if (!TransactionIdIsInProgress(xid))
			continue;

		if (is_insert)
			*is_insert = (op == SLOG_OP_INSERT);
		result = xid;
		break;
	}

	return result;
}

/*
 * SLogTupleGetDirtyWriterXid -- like SLogTupleGetDirtyXid, but returns only
 * the xid of an in-progress *writer* (INSERT/UPDATE/DELETE), ignoring
 * lock-only markers (LOCK_SHARE/LOCK_EXCL) and aborted markers.
 *
 * Used at the write-wait decision in the UPDATE/DELETE paths.  An updater
 * already holds the heavyweight LOCKTAG_TUPLE lock (LockTupleNoKeyExclusive ->
 * ExclusiveLock), which serializes against conflicting lockers via the
 * standard lock matrix: a key-share locker (AccessShareLock) is compatible and
 * does not block, while a share/exclusive locker conflicts and blocks the
 * updater on the lock manager.  XactLockTableWait must therefore fire only for
 * an actual in-progress writer -- never for a pure locker.  Waiting on a
 * locker's xid here while that locker is queued behind us for the same tuple
 * ExclusiveLock manufactures a deadlock cycle that heap avoids via
 * HEAP_XMAX_IS_LOCKED_ONLY.
 *
 * WAIT-FREE: uses the seqlock read-side, identical to SLogTupleGetDirtyXid.
 */
TransactionId
SLogTupleGetDirtyWriterXid(Oid relid, ItemPointer tid, bool *is_insert)
{
	SLogTupleKey key;
	SLogFlatPartition *fp;
	SLogTupleOp ops[SLOG_MAX_TUPLE_OPS];
	int			nops = 0;
	TransactionId result = InvalidTransactionId;
	int			i;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	fp = SLogGetPartition(&key);

	SLOG_SEQ_READ_BEGIN(fp)
	{
		const SLogFlatBucket *bucket = SLogFlatHashProbe(fp->hash, &key);

		if (bucket != NULL)
		{
			const SLogTupleEntry *entry = &bucket->entry;

			for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
				if (entry->ops[i].in_use)
					ops[nops++] = entry->ops[i];
		}
	}
	SLOG_SEQ_READ_END(fp);

	/* Consistent snapshot taken; resolve status outside the shared lock. */
	for (i = 0; i < nops; i++)
	{
		TransactionId xid;
		SLogOpType	op = ops[i].op_type;

		/* Only real writers block another writer. */
		if (op != SLOG_OP_INSERT &&
			op != SLOG_OP_UPDATE &&
			op != SLOG_OP_DELETE)
			continue;

		xid = ops[i].xid;

		if (TransactionIdIsCurrentTransactionId(xid))
			continue;
		if (!TransactionIdIsInProgress(xid))
			continue;

		if (is_insert)
			*is_insert = (op == SLOG_OP_INSERT);
		result = xid;
		break;
	}

	return result;
}

/*
 * slog_tuplock_to_lockmode -- map a LockTupleMode to its heavyweight LOCKMODE.
 *
 * The four tuple-lock strengths MUST map to four distinct LOCKMODEs so the
 * real conflict matrix (DoLockModesConflict) can distinguish a compatible
 * KeyShare FK locker from a conflicting Share/Exclusive locker.
 */
static LOCKMODE
slog_tuplock_to_lockmode(LockTupleMode mode)
{
	switch (mode)
	{
		case LockTupleKeyShare:
			return AccessShareLock;
		case LockTupleShare:
			return RowShareLock;
		case LockTupleNoKeyExclusive:
			return ExclusiveLock;
		case LockTupleExclusive:
			return AccessExclusiveLock;
	}
	elog(ERROR, "invalid tuple lock mode: %d", (int) mode);
	return NoLock;				/* keep compiler quiet */
}

/*
 * SLogTupleGetWriteConflictXid -- find an in-progress transaction whose marker
 * conflicts with a writer (UPDATE/DELETE) acquiring tuple lock my_mode.
 *
 * Unlike SLogTupleGetDirtyWriterXid (which only ever reports writers and
 * silently ignores lock-only markers), this also reports a *locker* whose
 * recorded LockTupleMode conflicts with my_mode under the standard heavyweight
 * matrix.  That is required for correctness: a SELECT ... FOR UPDATE locker
 * leaves only a LOCK_EXCL marker (no on-page writer state), and an updater that
 * consults a writer-only probe sails past it and clobbers the row the locker is
 * protecting.  The four-way mapping keeps a KeyShare FK locker (AccessShareLock)
 * compatible with a NoKeyExclusive UPDATE (ExclusiveLock) while making FOR SHARE
 * (RowShareLock) and FOR UPDATE (AccessExclusiveLock) correctly block it.
 *
 * Writers take priority over lockers in the returned xid so the caller's
 * is_insert handling (TM_Invisible for an in-progress INSERT) is preserved; a
 * conflicting locker is returned only when no in-progress writer exists.
 *
 * Returns the conflicting xid, or InvalidTransactionId if none.  *is_insert is
 * set true only when the returned xid is an in-progress INSERT writer.
 *
 * WAIT-FREE: uses the seqlock read-side, identical to SLogTupleGetDirtyWriterXid.
 */
TransactionId
SLogTupleGetWriteConflictXid(Oid relid, ItemPointer tid,
							 LockTupleMode my_mode, bool *is_insert)
{
	SLogTupleKey key;
	SLogFlatPartition *fp;
	SLogTupleOp ops[SLOG_MAX_TUPLE_OPS];
	int			nops = 0;
	TransactionId writer_xid = InvalidTransactionId;
	bool		writer_is_insert = false;
	TransactionId locker_xid = InvalidTransactionId;
	LOCKMODE	my_lockmode = slog_tuplock_to_lockmode(my_mode);
	int			i;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);

	fp = SLogGetPartition(&key);

	SLOG_SEQ_READ_BEGIN(fp)
	{
		const SLogFlatBucket *bucket = SLogFlatHashProbe(fp->hash, &key);

		if (bucket != NULL)
		{
			const SLogTupleEntry *entry = &bucket->entry;

			for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
				if (entry->ops[i].in_use)
					ops[nops++] = entry->ops[i];
		}
	}
	SLOG_SEQ_READ_END(fp);

	/* Consistent snapshot taken; resolve status outside the shared lock. */
	for (i = 0; i < nops; i++)
	{
		TransactionId xid;
		SLogOpType	op = ops[i].op_type;

		xid = ops[i].xid;

		if (TransactionIdIsCurrentTransactionId(xid))
			continue;
		if (!TransactionIdIsInProgress(xid))
			continue;

		if (op == SLOG_OP_INSERT ||
			op == SLOG_OP_UPDATE ||
			op == SLOG_OP_DELETE)
		{
			/* A real writer: highest priority, stop scanning. */
			writer_xid = xid;
			writer_is_insert = (op == SLOG_OP_INSERT);
			break;
		}

		if (op == SLOG_OP_LOCK_SHARE || op == SLOG_OP_LOCK_EXCL)
		{
			LOCKMODE	locker_lockmode =
				slog_tuplock_to_lockmode(ops[i].lock_mode);

			if (DoLockModesConflict(my_lockmode, locker_lockmode))
				locker_xid = xid;	/* candidate; keep seeking a writer */
		}
	}

	if (TransactionIdIsValid(writer_xid))
	{
		if (is_insert)
			*is_insert = writer_is_insert;
		return writer_xid;
	}

	if (is_insert)
		*is_insert = false;
	return locker_xid;
}

/*
 * SLogTupleHasLockConflict -- check if any active lock entries on this TID
 * conflict with the requested lock mode.
 */
bool
SLogTupleHasLockConflict(Oid relid, ItemPointer tid,
						 TransactionId my_xid,
						 SLogOpType requested_lock)
{
	SLogTupleOp ops[SLOG_MAX_TUPLE_OPS];
	int			nfound;
	int			i;

	nfound = SLogTupleLookupFiltered(relid, tid, InvalidTransactionId,
									 ops, SLOG_MAX_TUPLE_OPS);

	for (i = 0; i < nfound; i++)
	{
		if (TransactionIdEquals(ops[i].xid, my_xid))
			continue;
		if (!TransactionIdIsInProgress(ops[i].xid))
			continue;

		/* Only lock/mutating entries can conflict */
		if (ops[i].op_type != SLOG_OP_LOCK_SHARE &&
			ops[i].op_type != SLOG_OP_LOCK_EXCL &&
			ops[i].op_type != SLOG_OP_DELETE &&
			ops[i].op_type != SLOG_OP_UPDATE)
			continue;

		/*
		 * Lock compatibility matrix: SHARE vs SHARE: compatible SHARE vs
		 * EXCL/DELETE/UPDATE: conflict EXCL vs anything: conflict
		 */
		if (requested_lock == SLOG_OP_LOCK_SHARE)
		{
			if (ops[i].op_type == SLOG_OP_LOCK_EXCL ||
				ops[i].op_type == SLOG_OP_DELETE ||
				ops[i].op_type == SLOG_OP_UPDATE)
				return true;
		}
		else if (requested_lock == SLOG_OP_LOCK_EXCL)
		{
			return true;
		}
	}

	return false;
}

/*
 * SLogTupleGetLockConflictXid -- like SLogTupleHasLockConflict, but also
 * returns the xid of the *conflicting* transaction (the one whose marker
 * actually conflicts with the requested lock).
 *
 * The waiter must XactLockTableWait on the conflicting xid specifically.
 * Waiting on the first in-progress xid found on the TID (as a broad
 * SLogTupleGetDirtyXid probe would return) can pick a compatible peer -- e.g.
 * another KeyShare locker -- which is not blocking us and may itself be queued
 * behind us, manufacturing a spurious mutual-wait deadlock cycle.
 *
 * my_mode is the precise LockTupleMode being acquired.  Conflicts are decided
 * with the real heavyweight matrix (DoLockModesConflict) over the four
 * distinct tuple-lock strengths, exactly as SLogTupleGetWriteConflictXid does.
 * The coarse two-value SLOG_OP_LOCK_SHARE/EXCL classification is not
 * sufficient here: it made every in-progress UPDATE/DELETE marker conflict
 * with a SHARE request, so an FK check's KeyShare probe (AccessShareLock)
 * blocked behind a NoKeyExclusive UPDATE (ExclusiveLock) that heap treats as
 * COMPATIBLE.  Two concurrent FK inserts referencing the same parent row then
 * waited on each other's xid with no lock cycle for the deadlock detector to
 * break, hanging the backend forever (seen in the fk-deadlock isolation test).
 *
 * Returns true and sets *xid_out if a conflicting in-progress transaction
 * exists; returns false otherwise (xid_out is set to InvalidTransactionId).
 */
bool
SLogTupleGetLockConflictXid(Oid relid, ItemPointer tid,
							TransactionId my_xid,
							LockTupleMode my_mode,
							TransactionId *xid_out)
{
	SLogTupleOp ops[SLOG_MAX_TUPLE_OPS];
	LOCKMODE	my_lockmode = slog_tuplock_to_lockmode(my_mode);
	int			nfound;
	int			i;

	*xid_out = InvalidTransactionId;

	nfound = SLogTupleLookupFiltered(relid, tid, InvalidTransactionId,
									 ops, SLOG_MAX_TUPLE_OPS);

	for (i = 0; i < nfound; i++)
	{
		if (TransactionIdEquals(ops[i].xid, my_xid))
			continue;
		if (!TransactionIdIsInProgress(ops[i].xid))
			continue;

		switch (ops[i].op_type)
		{
			case SLOG_OP_LOCK_SHARE:
			case SLOG_OP_LOCK_EXCL:

				/*
				 * A locker records its precise LockTupleMode, so compare the
				 * two strengths directly.
				 */
				if (DoLockModesConflict(my_lockmode,
										slog_tuplock_to_lockmode(ops[i].lock_mode)))
				{
					*xid_out = ops[i].xid;
					return true;
				}
				break;

			case SLOG_OP_UPDATE:
			case SLOG_OP_DELETE:

				/*
				 * A writer holds at least NoKeyExclusive (ExclusiveLock); a
				 * DELETE holds Exclusive (AccessExclusiveLock).  KeyShare
				 * remains compatible with a NoKeyExclusive UPDATE, which is
				 * what lets concurrent FK checks proceed.
				 */
				if (DoLockModesConflict(my_lockmode,
										ops[i].op_type == SLOG_OP_DELETE ?
										AccessExclusiveLock : ExclusiveLock))
				{
					*xid_out = ops[i].xid;
					return true;
				}
				break;

			default:
				/* INSERT / ABORTED markers never block a lock request. */
				break;
		}
	}

	return false;
}

/*
 * SLogTupleHasAbortedEntry -- check if any aborted sLog op exists for a TID.
 */
bool
SLogTupleHasAbortedEntry(Oid relid, ItemPointer tid)
{
	SLogTupleOp ops[SLOG_MAX_TUPLE_OPS];
	int			nfound;
	int			i;

	nfound = SLogTupleLookupFiltered(relid, tid, InvalidTransactionId,
									 ops, SLOG_MAX_TUPLE_OPS);

	for (i = 0; i < nfound; i++)
	{
		/* Explicitly marked ABORTED */
		if (ops[i].op_type == SLOG_OP_ABORTED)
			return true;

		/* Skip our own transaction's entries for CLOG fallback */
		if (TransactionIdIsCurrentTransactionId(ops[i].xid))
			continue;

		/* CLOG fallback: completed but did not commit => aborted */
		if (!TransactionIdIsInProgress(ops[i].xid) &&
			TransactionIdDidAbort(ops[i].xid))
			return true;
	}

	return false;
}

/* ----------------------------------------------------------------
 * Backend-private tracking for tuple sLog cleanup
 * ----------------------------------------------------------------
 */

/*
 * SLogTupleTrackKey
 *		Remember a tuple key for cleanup at commit/abort.
 *
 * Allocated in TopTransactionContext so it's automatically freed
 * when the transaction ends.
 */
void
SLogTupleTrackKey(SLogTupleKey key, TransactionId xid, TransactionId subxid,
				  SLogOpType op_type)
{
	MemoryContext oldcxt;
	SLogTrackedKey *tk;

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);

	tk = (SLogTrackedKey *) palloc(sizeof(SLogTrackedKey));
	memcpy(&tk->key, &key, sizeof(SLogTupleKey));
	tk->xid = xid;
	tk->subxid = subxid;
	tk->local_only = false;
	slog_has_shared_entries = true;
	tk->op_type = op_type;
	tk->next = slog_tracked_keys;
	slog_tracked_keys = tk;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * SLogTupleTrackLocalOnly
 *		Lightweight local-only tracking (INSERTs only).
 *
 * Records (relid, tid, xid, subxid) in the per-backend local list WITHOUT
 * creating a shared sLog entry.  On subtransaction abort,
 * SLogTupleRemoveBySubXid will create a shared ABORTED entry for visibility.
 *
 * OOM optimization: When not inside a subtransaction, records the TID in a
 * backend-local open-addressing hash (simplehash) keyed by (relid,
 * encoded_tid) instead of a 136-byte linked-list node, giving O(1) insert
 * and probe and bounded per-backend memory under bulk INSERT.  Subtransaction
 * entries still use the linked list because subtxn abort needs per-entry
 * subxid filtering.
 */
void
SLogTupleTrackLocalOnly(Oid relid, ItemPointer tid,
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
		ItemPointerCopy(tid, &key.tid);

		oldcxt = MemoryContextSwitchTo(TopTransactionContext);

		tk = (SLogTrackedKey *) palloc(sizeof(SLogTrackedKey));
		memcpy(&tk->key, &key, sizeof(SLogTupleKey));
		tk->xid = xid;
		tk->subxid = subxid;
		tk->local_only = true;
		tk->op_type = SLOG_OP_INSERT;
		tk->next = slog_tracked_keys;
		slog_tracked_keys = tk;

		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * SLogTupleUntrackLocalOnly
 *		Remove local-only INSERT tracking for (relid, tid).
 *
 * Counterpart to SLogTupleTrackLocalOnly.  Used when a freshly inserted tuple
 * must NOT be stamped with the inserting transaction's commit marker at commit
 * time -- e.g. VACUUM FULL / CLUSTER copies a recently-dead tombstone into the
 * new relation and rewrites it with its ORIGINAL delete timestamp.  Leaving the
 * INSERT tracked would let the AM's commit-time stamping clobber the tuple's
 * commit metadata with the rewrite transaction's commit marker, resurrecting
 * the deleted row for any reader whose snapshot predates that commit.
 *
 * Clears the backend-local INSERT-hash entry on the top-level fast path and
 * also drops any matching linked-list node (the savepoint/fallback path).
 */
void
SLogTupleUntrackLocalOnly(Oid relid, ItemPointer tid)
{
	BlockNumber blkno = ItemPointerGetBlockNumber(tid);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
	SLogTrackedKey **link;

	if (offnum > MaxOffsetNumber)
		elog(ERROR, "offset %u exceeds page item limit %d in sLog TID encoding",
			 offnum, MaxOffsetNumber);

	/* Fast path: drop the backend-local INSERT-hash entry if present. */
	if (slog_insert_tids != NULL)
	{
		SLogInsertTidKey ikey;

		ikey.relid = relid;
		ikey.encoded_tid = SLOG_ENCODE_TID(blkno, offnum);
		(void) sloginsert_delete(slog_insert_tids, ikey);
	}

	/* Fallback/subtxn path: unlink any matching local-only INSERT node. */
	link = &slog_tracked_keys;
	while (*link != NULL)
	{
		SLogTrackedKey *tk = *link;

		if (tk->local_only &&
			tk->key.relid == relid &&
			ItemPointerGetBlockNumber(&tk->key.tid) == blkno &&
			ItemPointerGetOffsetNumber(&tk->key.tid) == offnum)
		{
			*link = tk->next;
			pfree(tk);
			continue;
		}
		link = &tk->next;
	}
}

/*
 * SLogTupleCleanupRetained
 *		Free retained sLog entries that are no longer visible to any
 *		active snapshot.
 *
 * Committed UPDATE markers are not retained (commit_xid
 * apply removes them), so under steady-state this routine primarily
 * cleans up other retained state.  Reclamation gates on the xid horizon
 * (GetOldestNonRemovableTransactionId) so a marker is reclaimable only
 * once its committing xid precedes the oldest active snapshot's xmin.
 *
 * Scans the flat hash under read-side, then applies CLEANUP_RETAINED ops.
 * Walks every partition taking each writer lock LW_EXCLUSIVE, so callers MUST
 * NOT hold a buffer content lock or other page-level critical section across
 * this call.  Driven by the UNDO background worker, or by
 * SLogTupleMaybeCleanupRetained() when the worker is disabled.  The
 * reclamation decision is the xid horizon (GetOldestNonRemovableTransactionId)
 * computed below.
 */
void
SLogTupleCleanupRetained(void)
{
	SLogTupleKey *collected_keys;
	int			max_keys = 256;
	int			part;
	int			i;
	TransactionId reclaim_xid_horizon;

	if (SLogState == NULL)
		return;

	/*
	 * Compute the xid horizon once for this pass.  The read-side eligibility
	 * scan and the CLEANUP_RETAINED apply gate on the same horizon so the set
	 * of slots reclaimed here matches the set the apply drops.
	 */
	reclaim_xid_horizon = GetOldestNonRemovableTransactionId(NULL);
	if (!TransactionIdIsValid(reclaim_xid_horizon))
		return;

	collected_keys = (SLogTupleKey *)
		palloc(max_keys * sizeof(SLogTupleKey));

	/*
	 * Process each partition independently while holding its exclusive writer
	 * lock.  The writer lock serializes all writers (other cleanup runs and
	 * forward-path inserts) for the partition, so the single copy is stable
	 * to scan.  We apply the CLEANUP_RETAINED ops inside seqlock cycles.
	 */
	for (part = 0; part < SLogNumPartitions; part++)
	{
		SLogFlatPartition *fp = SLogGetPartitionByIndex(part);
		SLogFlatHashScanState scan;
		const SLogFlatBucket *bucket;
		int			nkeys = 0;

		LWLockAcquire(&fp->writer_lock.lock, LW_EXCLUSIVE);

		/*
		 * Scan the single copy to collect expired keys.  Stable because we
		 * hold the writer lock.
		 */
		SLogFlatHashScanInit(&scan);
		while ((bucket = SLogFlatHashScanNext(fp->hash, &scan)) != NULL)
		{
			const SLogTupleEntry *entry = &bucket->entry;
			bool		has_expired = false;

			for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
			{
				if (!entry->ops[i].in_use)
					continue;
				if (entry->ops[i].op_type != SLOG_OP_UPDATE)
					continue;
				if (!TransactionIdIsValid(entry->ops[i].xid))
					continue;

				/*
				 * Reclaim once the xid precedes the oldest active snapshot's
				 * xmin (xid authority -- see the function header).  This is
				 * the authoritative, self-healing gate: a below-horizon
				 * marker is reclaimable whether or not it was ever committed
				 * in CLOG, and an in-progress xid can never precede the
				 * horizon.
				 */
				if (!TransactionIdPrecedes(entry->ops[i].xid,
										   reclaim_xid_horizon))
					continue;

				if (!has_expired)
				{
					if (nkeys >= max_keys)
					{
						max_keys *= 2;
						collected_keys = (SLogTupleKey *)
							repalloc(collected_keys,
									 max_keys * sizeof(SLogTupleKey));
					}
					collected_keys[nkeys++] = bucket->key;
					has_expired = true;
				}
			}
		}

		/* Null the now-dangling pointers and drop the slots. */
		if (nkeys > 0)
		{
			int			ops_since_publish = 0;

			for (i = 0; i < nkeys; i++)
			{
				SLogFlatOp	flat_op;

				memset(&flat_op, 0, sizeof(flat_op));
				flat_op.kind = SLOG_FLAT_OP_CLEANUP_RETAINED;
				flat_op.key = collected_keys[i];
				flat_op.reclaim_xid_horizon = reclaim_xid_horizon;
				SLogFlatHashApply(fp->hash, &flat_op, sizeof(flat_op));

				/*
				 * Bound the seqlock odd-hold window so wait-free readers of
				 * this partition make progress.  On a hot row many retained
				 * UPDATE markers expire at once, so nkeys can be large; a
				 * single uninterrupted odd window would spin every reader for
				 * its whole duration.  Each key's reclamation is independent
				 * and each seq cycle leaves fp->hash consistent, so ending
				 * and re-beginning at a batch boundary is safe -- the
				 * partition LWLock still serializes us against other
				 * before-image freers.
				 */
				if (++ops_since_publish >= SLOG_ABORT_PUBLISH_BATCH)
				{
					ops_since_publish = 0;
				}
			}
		}

		LWLockRelease(&fp->writer_lock.lock);
	}

	pfree(collected_keys);
}

/*
 * SLogTupleMaybeCleanupRetained
 *		Throttled, self-clocking entry point for retained-entry cleanup.
 *
 * Intended for access methods to call from their DML paths when the UNDO
 * background worker is disabled (max_logical_revert_workers = 0), so the DSA
 * before-image area does not fill up under sustained high-TPS load.  Unlike
 * SLogTupleCleanupRetained(), the heavy global sweep here runs at most once
 * every 5 seconds per backend; the common call is a counter increment that
 * returns immediately.
 *
 * CRITICAL: the caller MUST NOT hold any buffer content lock or other
 * page-level critical section across this call.  When the throttle fires this
 * walks every sLog partition taking each writer lock LW_EXCLUSIVE; running it
 * under a buffer lock would serialize all writers to a hot page behind the
 * sweep (the c>=2 hot-row UPDATE convoy this throttle was introduced to avoid).
 */
void
SLogTupleMaybeCleanupRetained(void)
{
	static TimestampTz slog_last_cleanup = 0;
	static uint32 slog_cleanup_clock = 0;
	TimestampTz now_ts;

	if (SLogState == NULL)
		return;

	/*
	 * Sample the wall clock only once every SLOG_INSERT_CLOCK_PERIOD calls.
	 * The throttle below already bounds the heavy sweep to once per 5s; this
	 * just keeps the common-case cost down to a counter increment so the
	 * post-unlock call on every UPDATE does not add a clock_gettime() syscall
	 * to the hot path (matters for the >=HEAP perf target).
	 */
	if ((slog_cleanup_clock++ & (SLOG_INSERT_CLOCK_PERIOD - 1)) != 0)
		return;

	now_ts = GetCurrentTimestamp();

	if (now_ts - slog_last_cleanup > 5000000)	/* 5 seconds */
	{
		slog_last_cleanup = now_ts;
		SLogTupleCleanupRetained();
	}
}

/*
 * SLogTupleAnyTracked
 *		True iff the current backend has tracked any tuple key (INSERT,
 *		UPDATE, or DELETE) for the current transaction -- i.e. this backend
 *		touched at least one tracked tuple since the last
 *		SLogTupleResetTracking().
 *
 * Used by the AM's xact callback to decide whether a transaction that
 * touched no tracked tuples should skip writing a durable commit-map entry.
 */
bool
SLogTupleAnyTracked(void)
{
	return slog_tracked_keys != NULL ||
		(slog_insert_tids != NULL && slog_insert_tids->members > 0);
}

/*
 * SLogTupleResetTracking
 *		Clear the backend-private tracking list and reset overflow state.
 *
 * Also frees the backend-local INSERT hash.  The table lives in
 * TopTransactionContext, so it would be freed at transaction end anyway.
 * Explicit cleanup here allows earlier memory reclaim and makes the state
 * consistent for any subsequent operations within the same backend lifetime.
 */
void
SLogTupleResetTracking(void)
{
	/* Destroy the backend-local INSERT hash */
	if (slog_insert_tids != NULL)
	{
		sloginsert_destroy(slog_insert_tids);
		slog_insert_tids = NULL;
	}

	slog_tracked_keys = NULL;
	slog_has_shared_entries = false;
	slog_overflow_warning_count = 0;
	slog_overflow_last_warning = 0;
}

/*
 * SLogTupleIterateTrackedKeys
 *		Iterate over tracked keys for a given xid.
 *
 * Calls the callback for each tracked key matching the given xid.
 * If the callback returns false, iteration stops early.
 * Used by AM-specific pre-commit callbacks that need to touch pages.
 *
 * Iterates both hash-based INSERT entries (top-level) and
 * linked-list entries.
 */
void
SLogTupleIterateTrackedKeys(TransactionId xid,
							SLogTrackedKeyCallback callback,
							void *arg)
{
	SLogTrackedKey *tk;

	/* Iterate backend-local INSERT-hash entries (top-level local-only) */
	if (slog_insert_tids != NULL)
	{
		sloginsert_iterator it;
		SLogInsertTidEntry *ie;

		sloginsert_start_iterate(slog_insert_tids, &it);
		while ((ie = sloginsert_iterate(slog_insert_tids, &it)) != NULL)
		{
			SLogTupleKey key;

			memset(&key, 0, sizeof(key));
			key.relid = ie->key.relid;
			ItemPointerSet(&key.tid,
						   SLOG_DECODE_BLKNO(ie->key.encoded_tid),
						   SLOG_DECODE_OFFNUM(ie->key.encoded_tid));

			if (!callback(&key, xid, InvalidTransactionId, true, arg))
				return;
		}
	}

	/* Iterate linked-list entries */
	for (tk = slog_tracked_keys; tk != NULL; tk = tk->next)
	{
		if (!TransactionIdEquals(tk->xid, xid))
			continue;

		if (!callback(&tk->key, tk->xid, tk->subxid, tk->local_only, arg))
			break;
	}
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
			ItemPointerSet(&arr[count].key.tid,
						   SLOG_DECODE_BLKNO(ie->key.encoded_tid),
						   SLOG_DECODE_OFFNUM(ie->key.encoded_tid));
			arr[count].xid = xid;
			arr[count].subxid = InvalidTransactionId;
			arr[count].local_only = true;
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
		arr[count].xid = tk->xid;
		arr[count].subxid = tk->subxid;
		arr[count].local_only = tk->local_only;
		arr[count].op_type = tk->op_type;
		count++;
	}

	*out_keys = arr;
	return count;
}
