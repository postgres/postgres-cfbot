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
 * bufmgr always removes a buffer's old mapping (BufTableDelete, called from
 * InvalidateVictimBuffer) before inserting a new tag for that same buf_id (see
 * GetVictimBuffer / BufferAlloc in bufmgr.c).  Empty entry slots are marked by
 * tag.blockNum == P_NEW; chains are linked by int index and terminated by
 * BUF_TABLE_CHAIN_END.
 *
 * num_buckets is a power of two and a multiple of NUM_BUFFER_PARTITIONS, so the
 * bucket index (hashcode % num_buckets) shares its low bits with the partition
 * index (hashcode % NUM_BUFFER_PARTITIONS).  Every tag that maps to a given
 * bucket therefore maps to a single partition, and the caller's BufMappingLock
 * fully serializes each chain -- the same guarantee the dynahash table relied
 * on.
 *
 * Note: the routines in this file do no locking of their own.  The caller
 * must hold a suitable lock on the appropriate BufMappingLock, as specified
 * in the comments.  We can't do the locking inside these functions because
 * in most cases the caller needs to adjust the buffer header contents
 * before the lock is released (see notes in README).
 *
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
	BufferTag	tag;			/* Tag of a disk page, or P_NEW if empty */
	int			next;			/* next entry in hash chain */
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
	return Max(NUM_BUFFER_PARTITIONS, pg_nextpower2_32(1.5 * NBuffers));
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
 * (BUF_TABLE_CHAIN_END) and every entry empty (tag.blockNum == P_NEW).
 */
void
BufTableShmemInit(void *arg)
{
	num_buckets = BufTableNumBuckets();

	for (int i = 0; i < num_buckets; i++)
		buckets[i].head = BUF_TABLE_CHAIN_END;

	for (int i = 0; i < NBuffers; i++)
	{
		entries[i].tag.blockNum = P_NEW;
		entries[i].next = BUF_TABLE_CHAIN_END;
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
 * BufTableLookup
 *		Lookup the given BufferTag; return buffer ID, or -1 if not found
 *
 * Caller must hold at least share lock on BufMappingLock for tag's partition
 */
int
BufTableLookup(BufferTag *tagPtr, uint32 hashcode)
{
	int			id = buckets[hashcode % num_buckets].head;

	while (id != BUF_TABLE_CHAIN_END)
	{
		if (BufferTagsEqual(&entries[id].tag, tagPtr))
			return id;
		id = entries[id].next;
	}
	return -1;
}

/*
 * BufTableInsert
 *		Insert a hashtable entry for given tag and buffer ID,
 *		unless an entry already exists for that tag
 *
 * Returns -1 on successful insertion.  If a conflicting entry exists
 * already, returns the buffer ID in that entry.
 *
 * Caller must hold exclusive lock on BufMappingLock for tag's partition
 */
int
BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id)
{
	int			bucket_id = hashcode % num_buckets;
	int			head = buckets[bucket_id].head;
	int			id = head;

	Assert(buf_id >= 0 && buf_id < NBuffers);
	Assert(tagPtr->blockNum != P_NEW);	/* invalid tag */

	/* If the tag is already in the chain, surface the existing buf_id. */
	while (id != BUF_TABLE_CHAIN_END)
	{
		if (BufferTagsEqual(&entries[id].tag, tagPtr))
			return id;
		id = entries[id].next;
	}

	/*
	 * Not present.  entry[buf_id] must be empty: bufmgr always deletes a
	 * buffer's old mapping before inserting a new tag for that buf_id.
	 */
	Assert(entries[buf_id].tag.blockNum == P_NEW);

	/*
	 * Link entry[buf_id] at the chain head, keeping the prior head as its
	 * successor.  (Use the saved `head`, not `id`, which the loop above has
	 * advanced to BUF_TABLE_CHAIN_END.)
	 */
	entries[buf_id].tag = *tagPtr;
	entries[buf_id].next = head;
	buckets[bucket_id].head = buf_id;

	return -1;
}

/*
 * BufTableDelete
 *		Delete the hashtable entry for given tag (which must exist)
 *
 * Caller must hold exclusive lock on BufMappingLock for tag's partition
 */
void
BufTableDelete(BufferTag *tagPtr, uint32 hashcode)
{
	int			bucket_id = hashcode % num_buckets;
	int			prev = BUF_TABLE_CHAIN_END;
	int			id = buckets[bucket_id].head;

	while (id != BUF_TABLE_CHAIN_END)
	{
		if (BufferTagsEqual(&entries[id].tag, tagPtr))
		{
			/* unlink from the chain */
			if (prev == BUF_TABLE_CHAIN_END)
				buckets[bucket_id].head = entries[id].next;
			else
				entries[prev].next = entries[id].next;
			/* mark the entry empty */
			entries[id].tag.blockNum = P_NEW;
			entries[id].next = BUF_TABLE_CHAIN_END;
			return;
		}
		prev = id;
		id = entries[id].next;
	}

	/*
	 * Entry not in table.  Callers never double-delete (deletion is gated by
	 * BM_TAG_VALID on the buffer header), so this indicates corruption.
	 */
	Assert(false);
	elog(ERROR, "shared buffer hash table corrupted");
}
