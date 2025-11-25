/*-------------------------------------------------------------------------
 *
 * batchscan.c
 *	  Support routines for index access methods' amgetbatch functions
 *	  (and for amgetbitmap functions that are implemented using batches).
 *
 * This module provides the index AM side of batch-based index scans: it
 * allocates batches (each a single allocation carrying the batch itself, its
 * matching items, and opaque areas for both AMs), caches released batches
 * for reuse to avoid palloc churn, and unlocks a batch's index page buffer
 * in a way that upholds the scan's TID recycling interlock rules.
 *
 * The table AM side, which consumes batches through the scan's batch ring
 * buffer, is in tableam_indexscan.c.  Index AMs free and unlock batches as
 * described in indexam.sgml.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/batchscan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/batchscan.h"
#include "access/tableam.h"
#include "storage/bufmgr.h"
#include "utils/memdebug.h"

static void batch_cache_mark_undefined(IndexScanDesc scan, IndexScanBatch batch);
static inline bool batch_cache_store(IndexScanDesc scan, IndexScanBatch batch);

/*
 * Return the size of the single allocation backing one of this scan's batches
 * for assertions/custom Valgrind batch instrumentation
 */
#if defined(USE_VALGRIND) || defined(USE_ASSERT_CHECKING)
static size_t
batch_alloc_size(IndexScanDesc scan)
{
	size_t		allocsz;

	Assert(scan->batch_base_offset > 0);

	allocsz = scan->batch_base_offset +
		MAXALIGN(offsetof(IndexScanBatchData, items) +
				 sizeof(BatchMatchingItem) * scan->maxitemsbatch);
	if (scan->xs_want_itup)
		allocsz += scan->batch_tuples_workspace;

	return allocsz;
}
#endif

/*
 * Make Valgrind treat a batch's entire allocation as undefined memory
 */
static void
batch_cache_mark_undefined(IndexScanDesc scan, IndexScanBatch batch)
{
#ifdef USE_VALGRIND
	char	   *tuples = batch->tuples;
	int		   *deadItems = batch->deadItems;

	VALGRIND_MAKE_MEM_UNDEFINED(index_scan_batch_base(scan, batch),
								batch_alloc_size(scan));
	if (deadItems)
		VALGRIND_MAKE_MEM_UNDEFINED(deadItems,
									sizeof(int) * scan->maxitemsbatch);

	/* preserve pointers to now-undefined tuples and deadItems buffers */
	batch->tuples = tuples;
	batch->deadItems = deadItems;
#endif
}

/*
 * Unlock batch's index page buffer lock
 *
 * Unlocks the given buffer in preparation for amgetbatch returning items
 * saved in that batch.  Performs extra steps required by amgetbatch callers
 * in passing.
 *
 * Only call here when a batch has one or more matching items to return using
 * amgetbatch (or for amgetbitmap to load into its bitmap of matching TIDs).
 * When an index page has no matches, it's always safe for index AMs to drop
 * both the lock and the pin for themselves.
 *
 * Note: It is convenient for index AMs that implement both amgetbatch and
 * amgetbitmap to consistently use the same batch management approach, since
 * that avoids introducing special cases to lower-level code.  We drop both
 * the lock and the pin on batch's page on behalf of amgetbitmap callers.
 *
 * For amgetbatch callers, when batchImmediateUnguard is set (plain MVCC
 * scans), we also release the pin here (the TID recycling interlock).  The
 * batch will be marked "unguarded", preventing the table AM from spuriously
 * calling amunguardbatch later on.
 *
 * Index AMs whose TID recycling interlock is not just a buffer pin, or whose
 * amunguardbatch does not simply release a pin, are not obligated to use this
 * function.  They can implement their own equivalent.  Such index AMs are also
 * free to use the batch LSN field themselves; their amkillitemsbatch routine
 * can use that LSN in the usual way, or in whatever way the AM deems necessary
 * (core code will not use it for any other purpose).
 */
void
batchscan_unlock(IndexScanDesc scan, IndexScanBatch batch, Buffer buf)
{
	/* batch must have one or more matching items returned by index AM */
	Assert(batch->firstItem >= 0 && batch->firstItem <= batch->lastItem);

	if (scan->usebatchring)
	{
		/* amgetbatch (not amgetbitmap) caller */
		Assert(scan->heapRelation != NULL);

		/*
		 * Have to set batch->lsn so that amkillitemsbatch callback has a way
		 * to detect when concurrent table TID recycling by VACUUM might have
		 * taken place.  It'll only be safe for amkillitemsbatch to set index
		 * tuple LP_DEAD bits when the page LSN hasn't advanced between then
		 * and now.
		 */
		batch->lsn = BufferGetLSNAtomic(buf);

		/*
		 * Drop the pin here during scans that don't require an explicit TID
		 * recycling interlock (a pin will block cleanup lock acquisition by
		 * index vacuuming)
		 */
		if (scan->batchImmediateUnguard)
		{
			/* drop both the lock and the pin */
			UnlockReleaseBuffer(buf);
			Assert(!batch->isGuarded);	/* won't call amunguardbatch */
		}
		else
		{
			/*
			 * just drop the lock; index AM's amunguardbatch callback will be
			 * called to drop the pin later on, when the table AM determines
			 * that it is safe to do so
			 */
			UnlockBuffer(buf);
			batch->isGuarded = true;
		}
	}
	else
	{
		/* amgetbitmap (not amgetbatch) caller */
		Assert(scan->heapRelation == NULL);

		/*
		 * drop both the lock and the pin (amunguardbatch is never called
		 * during bitmap index scans)
		 */
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Allocate a new batch
 *
 * Used by index AMs that support amgetbatch interface (both during amgetbatch
 * and amgetbitmap scans).
 *
 * Returns IndexScanBatch with space to fit scan->maxitemsbatch-many
 * BatchMatchingItem entries.  This will either be a newly allocated batch, or
 * a batch recycled from the cache managed by batchscan_release.  See
 * comments above batchscan_release.
 *
 * Housekeeping fields (buf, knownEndBackward/Forward, firstItem, lastItem,
 * numDead, deadItems, tuples) are initialized here.  The table AM's
 * batch_init callback is invoked here to initialize the table AM opaque area.
 * The index AM caller is responsible for filling in its per-batch opaque
 * fields and the matching items[] array.
 *
 * Once the batch has the required matching items, caller should generally
 * pass it to batchscan_unlock, ahead of it being returned through
 * index AM's amgetbatch routine.  If it turns out that the batch won't need
 * to be returned like this (e.g., due to the scan having no more matches),
 * caller should pass its empty/unused batch to batchscan_release.
 */
IndexScanBatch
batchscan_alloc(IndexScanDesc scan)
{
	IndexScanBatch batch = NULL;

	/* Index AM must have set its opaque space to something already */
	Assert(scan->indexRelation->rd_indam->amgetbatch != NULL);
	Assert(scan->batch_index_opaque_static > 0);

	/* First look for an existing batch from the cache */
	if (scan->usebatchring)
	{
		for (int i = 0; i < INDEX_SCAN_CACHE_BATCHES; i++)
		{
			if (scan->batchcache[i] != NULL)
			{
				/* Return cached unreferenced batch */
				batch = scan->batchcache[i];
				scan->batchcache[i] = NULL;
				break;
			}
		}
	}
	else if (scan->batchcache[0] != NULL)
	{
		/*
		 * Reuse cached batch from prior amgetbitmap iteration.  This path is
		 * hit on every amgetbitmap call here after the scan's first.
		 */
		batch = scan->batchcache[0];
		scan->batchcache[0] = NULL;
	}

	if (!batch)
	{
		size_t		opaque_areas_prefix_sz,
					base_sz,
					batch_tuples_workspace,
					allocsz;
		char	   *raw_batch_alloc;

		if (scan->batch_base_offset == 0)
		{
			/* We lazily compute batch_base_offset on scan's first call */
			size_t		table_area = 0;

			if (scan->usebatchring)
			{
				/*
				 * Handle table AM's dynamically-sized area.  It isn't used
				 * during batch-based bitmap scans...
				 */
				table_area = MAXALIGN(scan->batch_table_opaque_size);
			}

			/* ...though we always need an index AM area */
			scan->batch_base_offset = table_area +
				scan->batch_index_opaque_static;
		}

		/* Subtotal #1: the size of all AM opaque areas */
		opaque_areas_prefix_sz = scan->batch_base_offset;
		Assert(opaque_areas_prefix_sz == MAXALIGN(opaque_areas_prefix_sz));

		/* Subtotal #2: IndexScanBatchData and its items[maxitemsbatch] */
		base_sz = MAXALIGN(offsetof(IndexScanBatchData, items) +
						   sizeof(BatchMatchingItem) * scan->maxitemsbatch);

		/*
		 * Subtotal #3: the tuples workspace that comes after items[], where
		 * the index AM stores index tuples during index-only scans
		 */
		batch_tuples_workspace = 0;
		if (scan->xs_want_itup)
		{
			batch_tuples_workspace = scan->batch_tuples_workspace;
			pg_assume(batch_tuples_workspace > 0);
			Assert(batch_tuples_workspace == MAXALIGN(batch_tuples_workspace));
		}

		/* Total batch allocation size is the sum of our three subtotals */
		allocsz = opaque_areas_prefix_sz + base_sz + batch_tuples_workspace;
		Assert(allocsz == batch_alloc_size(scan));
		raw_batch_alloc = palloc(allocsz);
		batch = (IndexScanBatch) (raw_batch_alloc + opaque_areas_prefix_sz);
		Assert(index_scan_batch_base(scan, batch) == raw_batch_alloc);

		/* tuples (if any) is directly after items[] */
		batch->tuples = NULL;
		if (batch_tuples_workspace)
			batch->tuples = (char *) batch + base_sz;
		batch->deadItems = NULL;
	}

	Assert(scan->batch_base_offset > 0);

	/*
	 * Let the table AM initialize its per-batch opaque area iff it requested
	 * one (which can't happen during batch-based bitmap index scans)
	 */
	if (scan->usebatchring && scan->batch_table_opaque_size > 0)
		table_index_scan_batch_init(scan, batch);

	/* initialize shared batch fields */
	batch->dir = NoMovementScanDirection;
	batch->knownEndBackward = false;
	batch->knownEndForward = false;
	batch->isGuarded = false;

	/* "firstItem <= lastItem" tests will fail at first (defensive) */
	batch->firstItem = 0;
	batch->lastItem = -1;

	/*
	 * deadItems[] might already be allocated iff this is a recycled batch.
	 * Either way, it starts out with zero valid killable items.
	 */
	batch->numDead = 0;

	return batch;
}

/*
 * Release allocated batch
 *
 * This function is called by index AMs to release a batch allocated by
 * batchscan_alloc.  Batches are cached here for reuse to reduce
 * palloc/pfree overhead.
 *
 * It's safe to release a batch immediately when it was used to read a page
 * that returned no matches to the scan.  Batches actually returned by index
 * AM's amgetbatch routine (i.e. batches for pages with one or more matches)
 * must be released by tableam_index_release_batch, which calls here after the
 * index AM's amkillitemsbatch routine (if any).  Index AMs that use batches
 * should call here to release a batch from their amgetbatch or amgetbitmap
 * routines.
 *
 * The rules for batch ownership differ slightly for amgetbitmap scans; see
 * the amgetbitmap documentation in doc/src/sgml/indexam.sgml for details.
 */
void
batchscan_release(IndexScanDesc scan, IndexScanBatch batch)
{
	if (!scan->usebatchring)
	{
		/*
		 * amgetbitmap scan caller.
		 *
		 * amgetbitmap routines are required to allocate no more than one
		 * batch at a time, so we'll always have a free slot.
		 */
		Assert(scan->batchcache[0] == NULL);
		Assert(scan->heapRelation == NULL);
		Assert(batch->deadItems == NULL);
		Assert(batch->tuples == NULL);

		batch_cache_mark_undefined(scan, batch);
		scan->batchcache[0] = batch;
		return;
	}

	/* amgetbatch scan caller */
	Assert(scan->heapRelation != NULL);

	/*
	 * Try to store caller's batch in this amgetbatch scan's cache of
	 * previously released batches first
	 */
	if (batch_cache_store(scan, batch))
		return;

	/* Cache full; just free the caller's batch */
	if (batch->deadItems)
		pfree(batch->deadItems);
	pfree(index_scan_batch_base(scan, batch));
}

/*
 * Try to store a batch in the scan's batch cache.
 *
 * Returns true if a free slot was found, false if the cache is full.
 */
static inline bool
batch_cache_store(IndexScanDesc scan, IndexScanBatch batch)
{
	for (int i = 0; i < INDEX_SCAN_CACHE_BATCHES; i++)
	{
		if (scan->batchcache[i] == NULL)
		{
			batch_cache_mark_undefined(scan, batch);
			scan->batchcache[i] = batch;
			return true;
		}
	}

	return false;
}
