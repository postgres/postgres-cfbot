/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  routines for mapping BufferTags to buffer indexes.
 *
 * The shared buffer mapping table is a flat, index-linked hash table (an
 * open-chaining replacement for the former dynahash-based table).  It is made
 * of two shared-memory arrays:
 *
 *	  buckets[num_buckets] - one chain head per hash bucket
 *	  entries[NBuffers]     - one entry per buffer, indexed by buf_id
 *
 * Each buffer slot i permanently owns entry slot i, so no freelist is needed:
 * bufmgr always removes a buffer's old mapping (BufTableUnlink, called from
 * InvalidateVictimBuffer) before inserting a new tag for that same buf_id (see
 * GetVictimBuffer / BufferAlloc in bufmgr.c).  Empty entry slots are marked by
 * bucket == BUF_TABLE_CHAIN_END; chains are linked by int index and terminated
 * by BUF_TABLE_CHAIN_END.
 *
 * num_buckets is a power of two and a multiple of NUM_BUFFER_PARTITIONS, so the
 * bucket index (hashcode % num_buckets) shares its low bits with the partition
 * index (hashcode % NUM_BUFFER_PARTITIONS).  Every tag that maps to a given
 * bucket therefore maps to a single partition, and the caller's BufMappingLock
 * fully serializes each chain -- the same guarantee the dynahash table relied
 * on.
 *
 * Prepare/insert/unlink require the caller to hold exclusive BufMappingLock
 * for the tag's partition for the whole prepare + mutate sequence:
 * BufTableScanResult.link is a pointer into the shared chain and is invalid
 * after that lock is released.  The actual pointer swing (BufTableInsert /
 * BufTableUnlink) is done while also holding the buffer header spinlock, so a
 * lock-free lookup cannot pin a buffer whose identity is still being changed.
 *
 * Lookup is called without a lock and takes a shared partition lock only if it
 * hits a stale node during the concurrent scan.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_table.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

#define BUF_TABLE_CHAIN_END  (-1)

/* bucket for buffer lookup hashtable */
typedef struct
{
	int			head;			/* head of hash chain, or BUF_TABLE_CHAIN_END */
} BufferLookupBucket;

/* entry for buffer lookup hashtable */
typedef struct
{
	uint32		hashcode;
	BufferTag	tag;			/* Tag of a disk page */
	int			next;			/* next entry in hash chain */
	uint32		bucket;
} BufferLookupEnt;

/* bucket and entry arrays for buffer lookup hashtable (in shared memory) */
static BufferLookupBucket *buckets;
static BufferLookupEnt *entries;

/* number of hash buckets; power of two and multiple of NUM_BUFFER_PARTITIONS */
static int	num_buckets;

static void BufTableShmemRequest(void *arg);
static void BufTableShmemInit(void *arg);
static void BufTableShmemAttach(void *arg);

const ShmemCallbacks BufTableShmemCallbacks = {
	.request_fn = BufTableShmemRequest,
	.init_fn = BufTableShmemInit,
	.attach_fn = BufTableShmemAttach,
};

/*
 * Number of hash buckets for the current NBuffers.
 *
 * Must be a power of two (so hashcode % num_buckets == hashcode & (num_buckets
 * - 1)) and a multiple of NUM_BUFFER_PARTITIONS, so that every tag in a bucket
 * maps to a single buffer partition (see file header).
 */
static inline int
BufTableNumBuckets(void)
{
	return Max(NUM_BUFFER_PARTITIONS, pg_nextpower2_32(NBuffers + NBuffers / 2));
}

/*
 * Register shared memory arrays for mapping buffers.
 */
void
BufTableShmemRequest(void *arg)
{
	num_buckets = BufTableNumBuckets();
	Assert(num_buckets % NUM_BUFFER_PARTITIONS == 0);

	ShmemRequestStruct(.name = "Shared Buffer Lookup Buckets",
					   .size = (Size) num_buckets * sizeof(BufferLookupBucket),
					   .ptr = (void **) &buckets,
		);

	ShmemRequestStruct(.name = "Shared Buffer Lookup Entries",
					   .size = (Size) NBuffers * sizeof(BufferLookupEnt),
					   .ptr = (void **) &entries,
		);
}

/*
 * Initialize the shared buffer lookup table.  Called once during shared-memory
 * initialization (in the postmaster, or in a standalone backend).
 *
 * Shared memory is zeroed, but zero is a valid buf_id and block 0 is a valid
 * block number, so we must explicitly mark every bucket empty
 * (BUF_TABLE_CHAIN_END) and every entry empty.
 */
void
BufTableShmemInit(void *arg)
{
	num_buckets = BufTableNumBuckets();

	for (int i = 0; i < num_buckets; i++)
		buckets[i].head = BUF_TABLE_CHAIN_END;

	for (int i = 0; i < NBuffers; i++)
	{
		entries[i].next = BUF_TABLE_CHAIN_END;
		entries[i].bucket = BUF_TABLE_CHAIN_END;
	}
}

/*
 * Per-backend attach.  The buckets/entries pointers are restored by the shmem
 * framework, but num_buckets is a process-local scalar that must be recomputed
 * in each backend.  Forked children inherit it, but EXEC_BACKEND children run
 * only the attach callback, so set it here too.
 */
void
BufTableShmemAttach(void *arg)
{
	num_buckets = BufTableNumBuckets();
}

/*
 * BufTableHashCode
 *		Compute the hash code associated with a BufferTag
 *
 * This must be passed to the lookup/insert/delete routines along with the
 * tag.  We do it like this because the callers need to know the hash code
 * in order to determine which buffer partition to lock, and we don't want
 * to do the hash computation twice (hash_any is a bit slow).
 */
uint32
BufTableHashCode(BufferTag *tagPtr)
{
	return tag_hash(tagPtr, sizeof(BufferTag));
}

/*
 * BufTableScan
 *		Walk one hash chain.  Does not modify the table.
 *
 * Invariants:
 *  - entries[id] is associated with buffer id.
 *  - a chain must end with a link to BUF_TABLE_CHAIN_END
 *  - chains are sorted by hash code, i.e.
 *    entries[entries[id].next].hashcode >= entries[id].hashcode.
 *  - entries[buckets[bucket].head].bucket == bucket
 *  - entries[entries[id].next].bucket == entries[id].bucket
 *
 * On a hit, result->found is the buf_id and result->link points at the
 * predecessor's next pointer.  On a miss, found is BUF_TABLE_CHAIN_END and
 * link is the splice point (first node with a greater hash, or the tail).
 *
 * A node whose bucket field does not match is a leftover pointer to a
 * recycled slot: result->link is left NULL so lookup can retry under a
 * shared partition lock.  Prepare insert/delete treat that as corruption.
 */
static pg_always_inline void
BufTableScan(BufferTag *tagPtr, uint32 hashcode,
			 BufTableScanResult *result)
{
	int			bucket = hashcode & (num_buckets - 1);
	int		   *link;
	int			id;

	result->link = NULL;
	result->bucket = bucket;
	result->found = BUF_TABLE_CHAIN_END;

	for (link = &buckets[bucket].head;
		 (id = *link) != BUF_TABLE_CHAIN_END;
		 link = &entries[id].next)
	{
		pg_read_barrier();
		if (entries[id].bucket != bucket)
			return;
		if (entries[id].hashcode > hashcode)
			break;
		if (entries[id].hashcode < hashcode)
			continue;
		if (BufferTagsEqual(&entries[id].tag, tagPtr))
		{
			result->link = link;
			result->found = id;
			return;
		}
	}
	result->link = link;
}

/*
 * BufTableLookup
 *		Lookup the given BufferTag; return buffer ID, or -1 if not found
 *
 * Fast path attempt without a lock; it might fail if a node is reused while
 * we hold a reference to it.  In that case a shared lock is acquired and the
 * scan is repeated.
 *
 * Concurrency:
 *   Conflicts with deletion on the same bucket; concurrent insertions
 *   linearize with lookup.  On a stale node it acquires LW_SHARED so the
 *   caller need not hold a lock for as long as deletions hold LW_EXCLUSIVE.
 */
int
BufTableLookup(BufferTag *tagPtr, uint32 hashcode)
{
	BufTableScanResult r;
	LWLock	   *lock = NULL;

	for (;;)
	{
		BufTableScan(tagPtr, hashcode, &r);
		if (r.link != NULL)
		{
			if (lock)
				LWLockRelease(lock);
			return r.found;
		}

		/* Concurrent reuse of a node we were following. */
		if (lock)
			elog(ERROR, "shared buffer hash table corrupted");

		lock = BufMappingPartitionLock(hashcode);
		LWLockAcquire(lock, LW_SHARED);
	}
}

/*
 * BufTablePrepareInsert
 *		Determine the splice point for a given tag without modifying the table.
 *
 * The subsequent BufTableInsert must run before the exclusive partition lock
 * is released, typically while also holding the victim's buffer header lock.
 *
 * Concurrency:
 *   Conflicts with same-tag insertion and with deletions on the same bucket.
 *
 * Returns the existing buf_id on collision, or -1 if the tag is absent.
 */
int
BufTablePrepareInsert(BufferTag *tagPtr, uint32 hashcode,
					  BufTableScanResult *result)
{
	BufTableScan(tagPtr, hashcode, result);
	if (result->link == NULL)
		elog(ERROR, "shared buffer hash table corrupted");
	return result->found;
}

/*
 * BufTableInsert
 *		Splice buf_id into the chain at the prepared location.
 *
 * Must be called only after a miss from BufTablePrepareInsert (found < 0).
 * Writes the entry fields, then publishes *link so lock-free lookup can see
 * the node.  Caller should hold the buffer header spinlock so PinBuffer waits
 * until BufferDesc.tag / BM_TAG_VALID are installed.
 */
void
BufTableInsert(BufTableScanResult *result, BufferTag *tagPtr,
			   uint32 hashcode, int buf_id)
{
	Assert(buf_id >= 0 && buf_id < NBuffers);
	Assert(result->link != NULL);
	Assert(result->found == BUF_TABLE_CHAIN_END);
	Assert(entries[buf_id].bucket == (uint32) BUF_TABLE_CHAIN_END);

	entries[buf_id].tag = *tagPtr;
	entries[buf_id].hashcode = hashcode;
	entries[buf_id].next = *result->link;
	entries[buf_id].bucket = result->bucket;
	pg_write_barrier();
	*result->link = buf_id;
}

/*
 * BufTablePrepareDelete
 *		Find the predecessor link for an existing entry without unlinking it.
 *
 * The subsequent BufTableUnlink must run before the exclusive partition lock
 * is released, typically while also holding the buffer header spinlock.
 *
 * Returns the matching buf_id, or -1 if the tag is not in the table.
 */
int
BufTablePrepareDelete(BufferTag *tagPtr, uint32 hashcode,
					  BufTableScanResult *result)
{
	BufTableScan(tagPtr, hashcode, result);
	if (result->link == NULL)
		elog(ERROR, "shared buffer hash table corrupted");
	return result->found;
}

/*
 * BufTableUnlink
 *		Remove the prepared entry from its hash chain.
 *
 * Concurrency:
 *		Conflicts with deletion or insertion on the same bucket.
 */
void
BufTableUnlink(BufTableScanResult *result)
{
	int			id = result->found;

	Assert(result->link != NULL);
	Assert(id >= 0 && id < NBuffers);

	*result->link = entries[id].next;
	pg_write_barrier();
	entries[id].next = BUF_TABLE_CHAIN_END;
	entries[id].bucket = BUF_TABLE_CHAIN_END;
}
