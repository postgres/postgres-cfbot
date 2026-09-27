/*-------------------------------------------------------------------------
 *
 * slog_flathash.h
 *	  LWLock-protected flat open-addressing hash for sLog tuple tracking.
 *
 * This provides read access to sLog tuple entries under the per-partition
 * writer_lock LWLock.  The flat hash uses open addressing with linear probing
 * and tombstone markers for deletions.
 *
 * Architecture: A SINGLE copy of the hash lives in shared memory, guarded
 * by a per-partition writer_lock LWLock (SLogFlatPartition.writer_lock).
 * Writers take the lock LW_EXCLUSIVE and mutate the single copy in place;
 * readers take it LW_SHARED for the duration of a read, copy data into local
 * variables, and release.  Reader/writer exclusion is entirely by the LWLock
 * (this replaces an earlier seqlock reader/writer scheme; the measured
 * read:write ratio was not read-dominated enough for the seqlock to pay off).
 *
 * Scan semantics: SLogFlatHashScanInit/ScanNext iterate all occupied
 * buckets linearly.  Read-side scans must run inside SLOG_SEQ_READ_BEGIN/END
 * (which take writer_lock LW_SHARED) and may only copy data into locals; a
 * writer holds writer_lock LW_EXCLUSIVE and mutates fp->hash directly.  For
 * write operations that need global scans (eviction, xid removal), the
 * pattern is: scan fp->hash under the writer lock (stable, no reader can tear
 * it), collect keys, then apply write ops.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/slog_flathash.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLOG_FLATHASH_H
#define SLOG_FLATHASH_H

#include "access/slog.h"
#include "port/atomics.h"
#include "storage/lwlock.h"
#include "storage/s_lock.h"

/*
 * Bucket states encoded via hash_val:
 *   0              = empty (never used)
 *   UINT32_MAX     = tombstone (deleted)
 *   anything else  = occupied with that hash value
 */
#define SLOG_FLAT_EMPTY		0
#define SLOG_FLAT_TOMBSTONE	UINT32_MAX

/*
 * SLogFlatBucket - One bucket in the flat open-addressing hash table.
 *
 * Layout: hash_val marks the state, key is the lookup key, entry contains
 * the full SLogTupleEntry data (ops array etc).
 */
typedef struct SLogFlatBucket
{
	uint32		hash_val;		/* 0=empty, TOMBSTONE=deleted, else hash */
	SLogTupleKey key;			/* (relid, tid) */
	uint16		padding;		/* alignment padding */
	SLogTupleEntry entry;		/* nops + ops[SLOG_MAX_TUPLE_OPS] */
} SLogFlatBucket;

/*
 * SLogFlatHash - The flat hash table header, followed by buckets[].
 *
 * A single copy lives in shared memory, guarded by the partition writer_lock.
 */
typedef struct SLogFlatHash
{
	int32		capacity;		/* number of buckets (power of 2) */
	int32		num_entries;	/* current live entries */
	int32		num_tombstones; /* tombstone count (for load factor) */
	int32		padding;
	SLogFlatBucket buckets[FLEXIBLE_ARRAY_MEMBER];
} SLogFlatHash;

/*
 * Operation kinds for the flat-hash apply dispatcher (SLogFlatHashApply).
 */
typedef enum SLogFlatOpKind
{
	SLOG_FLAT_OP_INSERT,		/* Insert/update a single op slot */
	SLOG_FLAT_OP_REMOVE_XID,	/* Remove all ops for xid from entry */
	SLOG_FLAT_OP_REMOVE_ENTRY,	/* Remove entire entry (tombstone it) */
	SLOG_FLAT_OP_MARK_ABORTED,	/* Mark all ops for xid as ABORTED */
	SLOG_FLAT_OP_UPDATE_OP,		/* Update a specific op slot in-place */
	SLOG_FLAT_OP_COMMIT_XID,	/* Handle commit retention for an entry */
	SLOG_FLAT_OP_CLEANUP_RETAINED,	/* Remove old retained entries */
	SLOG_FLAT_OP_CREATE_ABORTED,	/* Create a new ABORTED entry (for
									 * local-only) */
} SLogFlatOpKind;

/*
 * SLogFlatOp - A single operation to be applied to the flat hash.
 *
 * Passed to SLogFlatHashApply(), which mutates the single flat-hash copy.
 */
typedef struct SLogFlatOp
{
	SLogFlatOpKind kind;
	SLogTupleKey key;			/* which entry */
	TransactionId xid;			/* target xid */
	TransactionId subxid;		/* for subxid operations */
	TransactionId reclaim_xid_horizon;	/* INSERT: oldest active snapshot
										 * xmin; a committed UPDATE marker may
										 * be reclaimed only if its xid
										 * precedes this (visible to all
										 * snapshots) */
	SLogTupleOp tuple_op;		/* the op to insert/update (for INSERT) */
} SLogFlatOp;

/* ----------------------------------------------------------------
 * Partitioned flat hash: 32-way sharding to reduce writer lock contention.
 *
 * Each partition has its own single flat-hash copy guarded by a writer_lock
 * LWLock (LW_SHARED reads, LW_EXCLUSIVE writes) and its own writer lock.  Key
 * routing:
 * hash(key) % NUM_PARTITIONS.  This reduces writer lock contention
 * proportionally to the number of partitions.
 *
 * The partition count is determined at startup by the slog_num_partitions
 * GUC (default: 0 = auto-size based on CPU count).  The heuristic targets
 * 4x the number of CPUs, clamped to [16, 256], rounded to next power of 2.
 * This ensures that at peak concurrency each CPU core has ~4 partitions to
 * spread writes across, minimizing writer lock wait time.
 * ----------------------------------------------------------------
 */

/* Default partition count used only before SLogShmemInit sets the real value */
#define SLOG_FLAT_DEFAULT_PARTITIONS	32
#define SLOG_FLAT_MIN_PARTITIONS		16
#define SLOG_FLAT_MAX_PARTITIONS		256

/*
 * SLogFlatPartition - Per-partition state.
 *
 * Each partition owns a slice of the total flat hash capacity.  Reads take
 * writer_lock LW_SHARED; writes take it LW_EXCLUSIVE and
 * mutate the single copy (*hash) in place.
 */
typedef struct SLogFlatPartition
{
	SLogFlatHash *hash;			/* single flat-hash copy (in shmem) */
	LWLockPadded writer_lock;	/* per-partition writer serialization */
} SLogFlatPartition;

/*
 * Read-side locking helpers -- "copy into locals, then act after".
 *
 * Usage:
 *		SLOG_SEQ_READ_BEGIN(fp)
 *		{
 *			... probe fp->hash and memcpy/read into LOCAL variables ONLY ...
 *		}
 *		SLOG_SEQ_READ_END(fp);
 *		... now act on the locals ...
 *
 * Reads take the partition writer_lock in LW_SHARED for the duration of the
 * body, so the writer (which holds it LW_EXCLUSIVE) cannot mutate fp->hash
 * underneath the reader; the body therefore runs exactly once.  It must
 * only write caller-provided output locals and must NOT return/goto/elog out
 * of the block, or the shared lock would leak.
 */
#define SLOG_SEQ_READ_BEGIN(fp) \
	do { \
		LWLockAcquire(&(fp)->writer_lock.lock, LW_SHARED);

#define SLOG_SEQ_READ_END(fp) \
		LWLockRelease(&(fp)->writer_lock.lock); \
	} while (0)

/*
 * Compute the shared memory size needed for the flat hash data
 * (the single copy guarded by the writer_lock).
 */
extern Size SLogFlatHashDataSize(int capacity);

/*
 * Compute the total shared memory needed for one partition's flat hash
 * (the single copy; the writer_lock is embedded in
 * SLogFlatPartition).
 */
extern Size SLogFlatHashShmemSize(int capacity, int max_backends);

/*
 * Compute the total shared memory needed for all partitions.
 */
extern Size SLogFlatHashPartitionedShmemSize(int total_capacity,
											 int max_backends);

/*
 * Initialize the flat hash in a pre-allocated data block.
 * Called during SLogShmemInit to set up the single copy.
 */
extern void SLogFlatHashInit(void *data, int capacity);

/*
 * Apply one SLogFlatOp to the (single) flat-hash copy.  Called by writers
 * holding the partition writer_lock LW_EXCLUSIVE.
 */
extern void SLogFlatHashApply(void *data, const void *operation, Size op_size);

/*
 * Hash computation for SLogTupleKey.
 */
extern uint32 SLogFlatHashComputeHash(const SLogTupleKey *key);

/*
 * Runtime partition count; set during SLogShmemInit() based on
 * the slog_num_partitions GUC.  Declared in slog.c.
 */
extern int	SLogNumPartitions;

/*
 * Compute which partition a key belongs to.
 */
static inline int
SLogFlatHashPartitionIndex(const SLogTupleKey *key)
{
	return (int) (SLogFlatHashComputeHash(key) % (uint32) SLogNumPartitions);
}

/*
 * Probe the flat hash for a key. Returns pointer to the bucket if found,
 * NULL if not found. Only valid during a read-side or write-side critical
 * section.
 */
extern SLogFlatBucket *SLogFlatHashProbe(const SLogFlatHash *ht,
										 const SLogTupleKey *key);

/*
 * Return true iff the entry for key holds an in-use op for xid.  Detects a
 * silently-dropped op (per-TID array full) where the bucket itself exists.
 * Only valid during a read-side or write-side critical section.
 */
extern bool SLogFlatHashHasOpForXid(const SLogFlatHash *ht,
									const SLogTupleKey *key,
									TransactionId xid);

/*
 * Find a bucket for insertion (first empty or tombstone slot on probe chain).
 * Returns NULL if the table is full (all slots on probe chain occupied).
 * Only valid during write-side critical section.
 */
extern SLogFlatBucket *SLogFlatHashProbeForInsert(SLogFlatHash *ht,
												  const SLogTupleKey *key,
												  uint32 hash_val);

/*
 * Scan API for iterating all occupied buckets.
 *
 * Used by global-scan operations (eviction, xid removal, cleanup) that
 * need to visit every entry.  The scan iterates linearly over the bucket
 * array, skipping EMPTY and TOMBSTONE slots.
 *
 * Usage pattern:
 *   SLogFlatHashScanState state;
 *   const SLogFlatBucket *bucket;
 *
 *   SLogFlatHashScanInit(&state);
 *   while ((bucket = SLogFlatHashScanNext(ht, &state)) != NULL)
 *   {
 *       // process bucket->entry
 *   }
 *
 * The scan must be performed inside a read-side critical section
 * (SLOG_SEQ_READ_BEGIN/END, which take writer_lock LW_SHARED, read into
 * locals only) or by a writer holding writer_lock LW_EXCLUSIVE.  For write
 * operations, scan fp->hash under the writer lock
 * (stable) to collect keys, then apply write ops.
 */
typedef struct SLogFlatHashScanState
{
	int32		current_index;
} SLogFlatHashScanState;

extern void SLogFlatHashScanInit(SLogFlatHashScanState *state);
extern const SLogFlatBucket *SLogFlatHashScanNext(const SLogFlatHash *ht,
												  SLogFlatHashScanState *state);

#endif							/* SLOG_FLATHASH_H */
