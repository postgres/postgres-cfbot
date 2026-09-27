/*-------------------------------------------------------------------------
 *
 * slog_flathash.c
 *	  Seqlock-protected flat open-addressing hash for sLog tuple tracking.
 *
 * Implements the flat hash table operations (probe, insert, remove) and
 * the apply handler that mutates the single hash copy.  The hash uses
 * linear probing with a power-of-2 capacity and tombstone markers.  A
 * per-partition seqlock (in SLogFlatPartition) provides retry-based
 * consistent reads; the writer lock serializes mutations.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/slog_flathash.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/slog_flathash.h"
#include "access/transam.h"
#include "common/hashfn.h"
#include "utils/dsa.h"

/*
 * Maximum probe distance before giving up.  With load factor < 0.7 and
 * power-of-2 sizing, typical probe chains are very short (< 5).
 * We cap at 128 to bound worst-case scan time.
 */
#define SLOG_FLAT_MAX_PROBE		128

/* ----------------------------------------------------------------
 * Size computation
 * ----------------------------------------------------------------
 */

/*
 * SLogFlatHashDataSize
 *		Size of one copy of the flat hash data structure.
 */
Size
SLogFlatHashDataSize(int capacity)
{
	return offsetof(SLogFlatHash, buckets) +
		(Size) capacity * sizeof(SLogFlatBucket);
}

/*
 * SLogFlatHashShmemSize
 *		Shared memory needed for one flat-hash copy (the seqlock guards a
 *		single copy in place; the sequence counter and writer lock live in
 *		the SLogFlatPartition struct, not here).
 */
Size
SLogFlatHashShmemSize(int capacity, int max_backends)
{
	(void) max_backends;		/* retained for call-site compatibility */
	return MAXALIGN(SLogFlatHashDataSize(capacity));
}

/*
 * SLogFlatHashPartitionedShmemSize
 *		Total shared memory needed for all partitions' flat-hash copies.
 *
 * Each partition gets capacity/N buckets in one copy.  The per-partition
 * seqlock counter and writer lock are embedded in SLogFlatPartition in the
 * SLogSharedState, so only the hash copies are sized here.
 */
Size
SLogFlatHashPartitionedShmemSize(int total_capacity, int max_backends)
{
	int			per_partition_capacity;
	Size		per_partition_size;
	Size		total;

	per_partition_capacity = total_capacity / SLogNumPartitions;
	if (per_partition_capacity < 64)
		per_partition_capacity = 64;

	per_partition_size = SLogFlatHashShmemSize(per_partition_capacity,
											   max_backends);
	total = (Size) SLogNumPartitions * MAXALIGN(per_partition_size);

	return total;
}

/* ----------------------------------------------------------------
 * Initialization
 * ----------------------------------------------------------------
 */

/*
 * SLogFlatHashInit
 *		Initialize a flat hash data block (one copy).
 *
 * Sets all buckets to EMPTY state.
 */
void
SLogFlatHashInit(void *data, int capacity)
{
	SLogFlatHash *ht = (SLogFlatHash *) data;
	int			i;

	ht->capacity = capacity;
	ht->num_entries = 0;
	ht->num_tombstones = 0;
	ht->padding = 0;

	for (i = 0; i < capacity; i++)
	{
		ht->buckets[i].hash_val = SLOG_FLAT_EMPTY;
		memset(&ht->buckets[i].key, 0, sizeof(SLogTupleKey));
		ht->buckets[i].padding = 0;
		ht->buckets[i].entry.nops = 0;
		memset(ht->buckets[i].entry.ops, 0,
			   sizeof(SLogTupleOp) * SLOG_MAX_TUPLE_OPS);
	}
}

/* ----------------------------------------------------------------
 * Hash function
 * ----------------------------------------------------------------
 */

/*
 * SLogFlatHashComputeHash
 *		Compute a 32-bit hash for an SLogTupleKey.
 *
 * The key must have been zeroed before population (for padding bytes).
 * Returns a non-zero, non-TOMBSTONE value (adjusts if hash_bytes returns
 * 0 or UINT32_MAX).
 */
uint32
SLogFlatHashComputeHash(const SLogTupleKey *key)
{
	uint32		h;

	h = hash_bytes((const unsigned char *) key, sizeof(SLogTupleKey));

	/* Ensure we never produce EMPTY or TOMBSTONE values */
	if (h == SLOG_FLAT_EMPTY)
		h = 1;
	else if (h == SLOG_FLAT_TOMBSTONE)
		h = SLOG_FLAT_TOMBSTONE - 1;

	return h;
}

/* ----------------------------------------------------------------
 * Probe operations
 * ----------------------------------------------------------------
 */

/*
 * SLogFlatHashProbe
 *		Look up a key in the flat hash. Returns bucket pointer if found,
 *		NULL if not present.
 *
 * Linear probing: start at hash_val % capacity, walk forward skipping
 * tombstones, stop at EMPTY (not found) or matching key (found).
 */
SLogFlatBucket *
SLogFlatHashProbe(const SLogFlatHash *ht, const SLogTupleKey *key)
{
	uint32		h;
	uint32		idx;
	int			probe;

	/*
	 * Fast path: an empty hash cannot contain the key, so skip the key hash
	 * computation and bucket walk entirely.  This lookup sits on the calling
	 * AM's per-tuple visibility/conflict hot path and the sLog
	 * (uncommitted-writer tracking) is empty for the vast majority of lookups
	 * in a committed-data OLTP workload (measured ~4%% of AM CPU under a
	 * TPROC-C-style workload before this guard).  Callers read the hash under
	 * the partition seqlock, so a concurrent writer's insert (which bumps
	 * num_entries) is caught by the seqlock retry.
	 */
	if (ht->num_entries == 0)
		return NULL;

	h = SLogFlatHashComputeHash(key);
	idx = h & (uint32) (ht->capacity - 1);

	for (probe = 0; probe < SLOG_FLAT_MAX_PROBE; probe++)
	{
		const SLogFlatBucket *bucket = &ht->buckets[idx];

		if (bucket->hash_val == SLOG_FLAT_EMPTY)
			return NULL;		/* definitive miss */

		if (bucket->hash_val != SLOG_FLAT_TOMBSTONE &&
			bucket->hash_val == h &&
			memcmp(&bucket->key, key, sizeof(SLogTupleKey)) == 0)
		{
			return (SLogFlatBucket *) bucket;	/* found */
		}

		idx = (idx + 1) & (uint32) (ht->capacity - 1);
	}

	return NULL;				/* probe limit exceeded */
}

/*
 * SLogFlatHashHasOpForXid
 *		Return true iff the entry for key holds an in-use op for xid.
 *
 * Used by SLogTupleInsert to confirm an op was actually stored, rather than
 * silently dropped because the per-TID ops array was full.  Probing the
 * bucket alone is insufficient on a hot row, where the bucket pre-exists with
 * other markers.
 */
bool
SLogFlatHashHasOpForXid(const SLogFlatHash *ht, const SLogTupleKey *key,
						TransactionId xid)
{
	const SLogFlatBucket *bucket = SLogFlatHashProbe(ht, key);
	const SLogTupleEntry *entry;
	int			i;

	if (bucket == NULL)
		return false;

	entry = &bucket->entry;
	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (entry->ops[i].in_use &&
			TransactionIdEquals(entry->ops[i].xid, xid))
			return true;
	}
	return false;
}

/*
 * SLogFlatHashProbeForInsert
 *		Find a slot for inserting a key. Returns the bucket to use.
 *
 * If the key already exists, returns that bucket (for update-in-place).
 * Otherwise returns the first EMPTY or TOMBSTONE slot encountered.
 * Returns NULL if probe limit exceeded without finding a slot.
 */
SLogFlatBucket *
SLogFlatHashProbeForInsert(SLogFlatHash *ht, const SLogTupleKey *key,
						   uint32 hash_val)
{
	uint32		idx;
	int			probe;
	SLogFlatBucket *first_free = NULL;

	idx = hash_val & (uint32) (ht->capacity - 1);

	for (probe = 0; probe < SLOG_FLAT_MAX_PROBE; probe++)
	{
		SLogFlatBucket *bucket = &ht->buckets[idx];

		if (bucket->hash_val == SLOG_FLAT_EMPTY)
		{
			/* Definitive miss; use first_free if we found one, else this */
			return first_free ? first_free : bucket;
		}

		if (bucket->hash_val == SLOG_FLAT_TOMBSTONE)
		{
			/* Remember first tombstone for potential reuse */
			if (first_free == NULL)
				first_free = bucket;
		}
		else if (bucket->hash_val == hash_val &&
				 memcmp(&bucket->key, key, sizeof(SLogTupleKey)) == 0)
		{
			/* Key already exists */
			return bucket;
		}

		idx = (idx + 1) & (uint32) (ht->capacity - 1);
	}

	/* Probe limit exceeded; return first_free if available */
	return first_free;
}

/* ----------------------------------------------------------------
 * Apply handler (single-copy, under the seqlock writer)
 * ----------------------------------------------------------------
 */

/*
 * flat_hash_apply_insert
 *		Apply an INSERT operation: find/create entry, add op to slot.
 */
static void
flat_hash_apply_insert(SLogFlatHash *ht, const SLogFlatOp *op)
{
	uint32		h;
	SLogFlatBucket *bucket;
	SLogTupleEntry *entry;
	int			i;
	bool		existing;

	h = SLogFlatHashComputeHash(&op->key);
	bucket = SLogFlatHashProbeForInsert(ht, &op->key, h);

	if (bucket == NULL)
	{
		elog(WARNING, "SLOG_LOST_OP table_full relid=%u xid=%u op=%d",
			 op->key.relid, op->tuple_op.xid, (int) op->tuple_op.op_type);
		return;					/* table full, operation lost */
	}

	/* Determine if this is an existing entry */
	existing = (bucket->hash_val != SLOG_FLAT_EMPTY &&
				bucket->hash_val != SLOG_FLAT_TOMBSTONE);

	if (!existing)
	{
		/* New entry */
		if (bucket->hash_val == SLOG_FLAT_TOMBSTONE)
			ht->num_tombstones--;

		bucket->hash_val = h;
		memcpy(&bucket->key, &op->key, sizeof(SLogTupleKey));
		bucket->entry.nops = 0;
		memset(&bucket->entry.key, 0, sizeof(SLogTupleKey));
		memcpy(&bucket->entry.key, &op->key, sizeof(SLogTupleKey));
		memset(bucket->entry.ops, 0, sizeof(bucket->entry.ops));
		ht->num_entries++;
	}

	entry = &bucket->entry;

	/* Check if this xid already has an op (overwrite) */
	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (entry->ops[i].in_use &&
			entry->ops[i].xid == op->tuple_op.xid)
		{
			/* Overwrite existing op for same xid */
			memcpy(&entry->ops[i], &op->tuple_op, sizeof(SLogTupleOp));
			return;
		}
	}

	/* Find a free slot */
	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (!entry->ops[i].in_use)
		{
			memcpy(&entry->ops[i], &op->tuple_op, sizeof(SLogTupleOp));
			entry->nops++;
			return;
		}
	}

	/*
	 * No free slot; reclaim the oldest horizon-eligible retained UPDATE
	 * marker.
	 *
	 * Hot rows (e.g. a TPC-C district) accumulate one retained UPDATE marker
	 * per committed transaction.  Committed UPDATE markers are removed at
	 * commit (flat_hash_apply_commit_xid), so in steady state the array only
	 * fills with in-progress writers or stragglers; this path drains the
	 * latter.
	 */
	{
		int			oldest_idx = -1;
		TransactionId reclaim_xid_horizon = op->reclaim_xid_horizon;

		/*
		 * Take the first horizon-eligible UPDATE marker.  A marker is
		 * reclaimable once its xid precedes the oldest active snapshot's xmin
		 * (reclaim_xid_horizon): at that point the xid's outcome is settled
		 * and visible to (or irrelevant to) every live snapshot, so no
		 * residual consumer can still need it (the write-write conflict probe
		 * and MVCC read are served from the durable UNDO log chain).  An
		 * in-progress xid can never precede this horizon, so this test alone
		 * never frees a marker a concurrent reader or writer still needs.
		 * Every marker that passes the horizon predicate is equally
		 * reclaimable, so we take the first one found.
		 *
		 * An INVALID reclaim_xid_horizon disables reclamation entirely: every
		 * marker is treated as non-reclaimable and we fall through to
		 * oldest_idx == -1.  This is the fail-safe direction and is exactly
		 * what the caller relies on -- SLogTupleInsert passes
		 * InvalidTransactionId on the fast path (skipping the ProcArrayLock
		 * horizon scan) and only supplies a real, freshly-computed horizon on
		 * the full-array retry.  A missing horizon must never make markers
		 * look reclaimable, or a live reader's / in-progress writer's op
		 * could be freed (lost update / MVCC corruption).
		 *
		 * The xid horizon is the authoritative, self-healing reclaim gate: a
		 * below-horizon marker is reclaimable whether or not it was ever
		 * committed in CLOG.
		 */
		for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
		{
			if (!entry->ops[i].in_use)
				continue;
			if (entry->ops[i].op_type != SLOG_OP_UPDATE)
				continue;
			if (!TransactionIdIsValid(entry->ops[i].xid))
				continue;
			if (!TransactionIdIsValid(reclaim_xid_horizon) ||
				!TransactionIdPrecedes(entry->ops[i].xid, reclaim_xid_horizon))
				continue;		/* invalid horizon disables reclaim
								 * (fail-safe); else still visible-relevant to
								 * an active snapshot, or in-progress */
			oldest_idx = i;
			break;
		}

		if (oldest_idx >= 0)
		{
			/* Safe to reclaim; no active snapshot needs this entry */
			entry->ops[oldest_idx].in_use = false;
			entry->nops--;

			/* Now insert the new op in the freed slot */
			memcpy(&entry->ops[oldest_idx], &op->tuple_op, sizeof(SLogTupleOp));
			entry->nops++;
			return;
		}
	}

	/*
	 * No reclaimable slot: every one of the SLOG_MAX_TUPLE_OPS slots holds an
	 * UPDATE whose xid is still at/above the reclaim horizon (in-progress or
	 * very recently committed) or a non-UPDATE marker.  With the xid-horizon
	 * reclaim above this requires SLOG_MAX_TUPLE_OPS concurrent unsettled
	 * writers on a single TID, which is not reachable under normal load.  The
	 * caller (SLogTupleInsert) detects the drop via SLogFlatHashHasOpForXid
	 * and falls back to local-only tracking + UNDO replay, so this is safe
	 * but worth surfacing.
	 */
	elog(WARNING, "SLOG_LOST_OP ops_full relid=%u xid=%u op=%d nops=%d horizon=%u",
		 op->key.relid, op->tuple_op.xid, (int) op->tuple_op.op_type,
		 entry->nops, op->reclaim_xid_horizon);
}

/*
 * flat_hash_apply_remove_xid
 *		Remove all ops for a given xid from an entry.
 *		Remove the entry entirely if nops reaches 0.
 */
static void
flat_hash_apply_remove_xid(SLogFlatHash *ht, const SLogFlatOp *op)
{
	SLogFlatBucket *bucket;
	SLogTupleEntry *entry;
	int			i;

	bucket = SLogFlatHashProbe(ht, &op->key);
	if (bucket == NULL)
		return;

	entry = &bucket->entry;

	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (entry->ops[i].in_use &&
			entry->ops[i].xid == op->xid)
		{
			entry->ops[i].in_use = false;
			entry->nops--;
		}
	}

	if (entry->nops == 0)
	{
		bucket->hash_val = SLOG_FLAT_TOMBSTONE;
		ht->num_entries--;
		ht->num_tombstones++;
	}
}

/*
 * flat_hash_apply_remove_entry
 *		Remove an entire entry (tombstone it).
 */
static void
flat_hash_apply_remove_entry(SLogFlatHash *ht, const SLogFlatOp *op)
{
	SLogFlatBucket *bucket;

	bucket = SLogFlatHashProbe(ht, &op->key);
	if (bucket == NULL)
		return;

	bucket->hash_val = SLOG_FLAT_TOMBSTONE;
	ht->num_entries--;
	ht->num_tombstones++;
}

/*
 * flat_hash_apply_mark_aborted
 *		Mark all ops for a given xid as SLOG_OP_ABORTED.
 */
static void
flat_hash_apply_mark_aborted(SLogFlatHash *ht, const SLogFlatOp *op)
{
	SLogFlatBucket *bucket;
	SLogTupleEntry *entry;
	int			i;

	bucket = SLogFlatHashProbe(ht, &op->key);
	if (bucket == NULL)
		return;

	entry = &bucket->entry;

	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (entry->ops[i].in_use &&
			entry->ops[i].xid == op->xid)
		{
			entry->ops[i].op_type = SLOG_OP_ABORTED;
		}
	}
}

/*
 * flat_hash_apply_update_op
 *		Update a specific op slot in place (e.g. re-parent a subxid).
 */
static void
flat_hash_apply_update_op(SLogFlatHash *ht, const SLogFlatOp *op)
{
	SLogFlatBucket *bucket;
	SLogTupleEntry *entry;
	int			i;

	bucket = SLogFlatHashProbe(ht, &op->key);
	if (bucket == NULL)
		return;

	entry = &bucket->entry;

	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (entry->ops[i].in_use &&
			entry->ops[i].xid == op->xid)
		{
			/*
			 * Apply selective updates from the op.  We use the tuple_op
			 * fields as the source of truth for what to update.
			 */
			if (op->subxid != InvalidTransactionId)
			{
				/* Re-parent subxid */
				if (entry->ops[i].subxid == op->tuple_op.subxid)
					entry->ops[i].subxid = op->subxid;
			}
			break;
		}
	}
}

/*
 * flat_hash_apply_commit_xid
 *		Remove every op belonging to xid.
 *
 * Committed UPDATE markers are no longer retained here.
 * The write-write conflict probe reads the head verptr on the tuple
 * and resolves it in the durable UNDO log (via the AM's version-reconstruction
 * walk), and no shared before-image is kept.  With no consumer
 * for a retained marker, an UPDATE at commit is treated like INSERT/DELETE/LOCK:
 * removed.  This drains bucket table_full pressure.
 */
static void
flat_hash_apply_commit_xid(SLogFlatHash *ht, const SLogFlatOp *op)
{
	SLogFlatBucket *bucket;
	SLogTupleEntry *entry;
	int			i;

	bucket = SLogFlatHashProbe(ht, &op->key);
	if (bucket == NULL)
		return;

	entry = &bucket->entry;

	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (!entry->ops[i].in_use)
			continue;
		if (entry->ops[i].xid != op->xid)
			continue;

		entry->ops[i].in_use = false;
		entry->nops--;
	}

	if (entry->nops == 0)
	{
		bucket->hash_val = SLOG_FLAT_TOMBSTONE;
		ht->num_entries--;
		ht->num_tombstones++;
	}
}

/*
 * flat_hash_apply_cleanup_retained
 *		Remove retained committed UPDATE markers whose committing xid
 *		precedes the reclaim xid horizon (op->reclaim_xid_horizon).
 *
 * Gates on the xid horizon -- NOT an HLC threshold -- so a marker is
 * reclaimed only when its committing xid precedes every live snapshot.
 * The write-write conflict probe and MVCC read use the durable per-backend
 * UNDO log chain, not this hash, so retained UPDATE markers here are dead
 * bookkeeping; this sweep still applies for any leftover
 * entries not removed at commit.
 */
static void
flat_hash_apply_cleanup_retained(SLogFlatHash *ht, const SLogFlatOp *op)
{
	SLogFlatBucket *bucket;
	SLogTupleEntry *entry;
	int			i;

	bucket = SLogFlatHashProbe(ht, &op->key);
	if (bucket == NULL)
		return;

	entry = &bucket->entry;

	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (!entry->ops[i].in_use)
			continue;
		if (entry->ops[i].op_type != SLOG_OP_UPDATE)
			continue;
		if (!TransactionIdIsValid(entry->ops[i].xid))
			continue;
		if (TransactionIdIsValid(op->reclaim_xid_horizon) &&
			!TransactionIdPrecedes(entry->ops[i].xid,
								   op->reclaim_xid_horizon))
			continue;

		/*
		 * Expired retained entry.  Gated on the xid horizon alone -- a marker
		 * committed in CLOG or an aborted/in-flight one is drained the moment
		 * its xid precedes the horizon, so no slot is pinned forever.  Must
		 * match the read-side eligibility scan in SLogTupleCleanupRetained.
		 */
		entry->ops[i].in_use = false;
		entry->nops--;
	}

	if (entry->nops == 0)
	{
		bucket->hash_val = SLOG_FLAT_TOMBSTONE;
		ht->num_entries--;
		ht->num_tombstones++;
	}
}

/*
 * flat_hash_apply_create_aborted
 *		Create a new entry with an ABORTED op (for local-only INSERT abort).
 */
static void
flat_hash_apply_create_aborted(SLogFlatHash *ht, const SLogFlatOp *op)
{
	uint32		h;
	SLogFlatBucket *bucket;
	SLogTupleEntry *entry;
	int			i;
	bool		existing;

	h = SLogFlatHashComputeHash(&op->key);
	bucket = SLogFlatHashProbeForInsert(ht, &op->key, h);

	if (bucket == NULL)
		return;					/* table full */

	existing = (bucket->hash_val != SLOG_FLAT_EMPTY &&
				bucket->hash_val != SLOG_FLAT_TOMBSTONE);

	if (!existing)
	{
		if (bucket->hash_val == SLOG_FLAT_TOMBSTONE)
			ht->num_tombstones--;

		bucket->hash_val = h;
		memcpy(&bucket->key, &op->key, sizeof(SLogTupleKey));
		bucket->entry.nops = 0;
		memset(&bucket->entry.key, 0, sizeof(SLogTupleKey));
		memcpy(&bucket->entry.key, &op->key, sizeof(SLogTupleKey));
		memset(bucket->entry.ops, 0, sizeof(bucket->entry.ops));
		ht->num_entries++;
	}

	entry = &bucket->entry;

	/* Find a free slot for the ABORTED entry */
	for (i = 0; i < SLOG_MAX_TUPLE_OPS; i++)
	{
		if (!entry->ops[i].in_use)
		{
			entry->ops[i].xid = op->xid;
			entry->ops[i].subxid = op->subxid;
			entry->ops[i].op_type = SLOG_OP_ABORTED;
			entry->ops[i].in_use = true;
			entry->ops[i].commit_ts = 0;
			entry->ops[i].spec_token = 0;
			entry->ops[i].cid = InvalidCommandId;
			entry->nops++;
			return;
		}
	}
	/* No free slot; operation lost */
}

/*
 * SLogFlatHashApply
 *		Apply one op to the flat hash. Dispatches to operation-specific handlers.
 */
void
SLogFlatHashApply(void *data, const void *operation, Size op_size)
{
	SLogFlatHash *ht = (SLogFlatHash *) data;
	const SLogFlatOp *op = (const SLogFlatOp *) operation;

	Assert(op_size == sizeof(SLogFlatOp));

	switch (op->kind)
	{
		case SLOG_FLAT_OP_INSERT:
			flat_hash_apply_insert(ht, op);
			break;
		case SLOG_FLAT_OP_REMOVE_XID:
			flat_hash_apply_remove_xid(ht, op);
			break;
		case SLOG_FLAT_OP_REMOVE_ENTRY:
			flat_hash_apply_remove_entry(ht, op);
			break;
		case SLOG_FLAT_OP_MARK_ABORTED:
			flat_hash_apply_mark_aborted(ht, op);
			break;
		case SLOG_FLAT_OP_UPDATE_OP:
			flat_hash_apply_update_op(ht, op);
			break;
		case SLOG_FLAT_OP_COMMIT_XID:
			flat_hash_apply_commit_xid(ht, op);
			break;
		case SLOG_FLAT_OP_CLEANUP_RETAINED:
			flat_hash_apply_cleanup_retained(ht, op);
			break;
		case SLOG_FLAT_OP_CREATE_ABORTED:
			flat_hash_apply_create_aborted(ht, op);
			break;
	}
}

/* ----------------------------------------------------------------
 * Scan API
 * ----------------------------------------------------------------
 */

/*
 * SLogFlatHashScanInit
 *		Initialize a sequential scan over the flat hash.
 */
void
SLogFlatHashScanInit(SLogFlatHashScanState *state)
{
	state->current_index = 0;
}

/*
 * SLogFlatHashScanNext
 *		Return the next occupied bucket, or NULL when the scan is complete.
 *
 * Skips EMPTY and TOMBSTONE slots.  The returned pointer is valid only
 * within the current seqlock read section (or under the writer lock).
 */
const SLogFlatBucket *
SLogFlatHashScanNext(const SLogFlatHash *ht, SLogFlatHashScanState *state)
{
	while (state->current_index < ht->capacity)
	{
		const SLogFlatBucket *bucket = &ht->buckets[state->current_index];

		state->current_index++;

		if (bucket->hash_val != SLOG_FLAT_EMPTY &&
			bucket->hash_val != SLOG_FLAT_TOMBSTONE)
		{
			return bucket;
		}
	}

	return NULL;
}
