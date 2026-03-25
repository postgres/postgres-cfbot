/*-------------------------------------------------------------------------
 *
 * indexbatch.c
 *	  Batch-based index scan infrastructure for the amgetbatch interface.
 *
 * This module provides the core infrastructure for batch-based index scans,
 * which allow index AMs to return multiple matching TIDs per page in a single
 * call.  The batch ring buffer is owned by the table AM.
 *
 * The ring buffer loads batches in index key space/index scan order.
 *
 * Most functions here are table AM utilities (tableam_util_*), called by
 * table AMs during amgetbatch index scans.  These manage the batch ring
 * buffer's lifecycle and positional state, and help with certain aspects of
 * resource management.  The table AM uses scanPos to return items from
 * batches returned by amgetbatch.
 *
 * There are also some index AM utilities (indexam_util_*), called by index
 * AMs that implement the amgetbatch interface, to help manage resources like
 * memory, locks, and buffer pins.  Index AMs free and unlock batches as
 * described in indexam.sgml.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/indexbatch.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/indexbatch.h"
#include "access/tableam.h"
#include "common/int.h"
#include "lib/qunique.h"
#include "utils/memdebug.h"

static void batch_cache_mark_undefined(IndexScanDesc scan, IndexScanBatch batch);
static void release_and_unguard_batch(IndexScanDesc scan, IndexScanBatch batch,
									  bool allow_cache);
static inline bool batch_cache_store(IndexScanDesc scan, IndexScanBatch batch);
static int	batch_compare_int(const void *va, const void *vb);

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
	char	   *currTuples = batch->currTuples;
	int		   *deadItems = batch->deadItems;

	VALGRIND_MAKE_MEM_UNDEFINED(index_scan_batch_base(scan, batch),
								batch_alloc_size(scan));
	if (deadItems)
		VALGRIND_MAKE_MEM_UNDEFINED(deadItems,
									sizeof(int) * scan->maxitemsbatch);

	/* preserve pointers to now-undefined currTuples and deadItems buffers */
	batch->currTuples = currTuples;
	batch->deadItems = deadItems;
#endif
}

/*
 * Reset ring buffer and related positional state used during an amgetbatch
 * index scan.
 *
 * Table AM caller should pass endscan=false, which makes us cache any freed
 * batches for reuse on rescan.  We release scan's markBatch here either way.
 */
void
tableam_util_batchscan_reset(IndexScanDesc scan, bool endscan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	IndexScanBatch markBatch = batchringbuf->markBatch;
	bool		markBatchFreed = false;

	batchringbuf->scanPos.valid = false;
	batchringbuf->markPos.valid = false;

	for (uint8 i = batchringbuf->headBatch; i != batchringbuf->nextBatch; i++)
	{
		IndexScanBatch batch = index_scan_batch(scan, i);

		if (batch == markBatch)
			markBatchFreed = true;

		release_and_unguard_batch(scan, batch, !endscan);
	}

	if (!markBatchFreed && unlikely(markBatch))
		release_and_unguard_batch(scan, markBatch, !endscan);

	batchringbuf->headBatch = 0;
	batchringbuf->nextBatch = 0;
	batchringbuf->markBatch = NULL;
}

/*
 * Free resources at end of a batch index scan.
 *
 * Called by table AM when an index scan is ending, right before the owning
 * scan descriptor goes away.  Cleans up all batch related resources.
 */
void
tableam_util_batchscan_end(IndexScanDesc scan)
{
	/* Free all remaining loaded batches (even markBatch), bypassing cache */
	tableam_util_batchscan_reset(scan, true);

	for (int i = 0; i < INDEX_SCAN_CACHE_BATCHES; i++)
	{
		IndexScanBatch cached = scan->batchcache[i];

		if (cached == NULL)
			continue;

		if (cached->deadItems)
			pfree(cached->deadItems);
		pfree(index_scan_batch_base(scan, cached));
	}
}

/*
 * Set a mark from scanPos position
 *
 * Called from the table AM's index_scan_markpos callback.  Saves the current
 * scan position and associated batch so that the scan can be restored to this
 * point later, via tableam_util_batchscan_restore_pos.  The marked batch is
 * retained and not freed until a new mark is set or the scan ends (or until
 * the mark is restored).
 */
void
tableam_util_batchscan_mark_pos(IndexScanDesc scan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	BatchRingItemPos *markPos PG_USED_FOR_ASSERTS_ONLY = &batchringbuf->markPos;
	IndexScanBatch scanBatch = index_scan_batch(scan, scanPos->batch);
	IndexScanBatch markBatch = batchringbuf->markBatch;

	Assert(scan->indexRelation->rd_indam->amcanmarkpos);
	Assert(scan->MVCCScan);
	Assert(scan->xs_table_opaque);
	Assert(scan->parallel_scan == NULL);
	Assert(batchringbuf->headBatch == scanPos->batch);	/* see below */

	/*
	 * A mark must point at a real matching item.  We require the core
	 * executor to only take a mark just after a successful tuple fetch.
	 */
	Assert(scanPos->valid);
	Assert(scanPos->item >= scanBatch->firstItem &&
		   scanPos->item <= scanBatch->lastItem);

	/* Free the previous mark batch? */
	if (!markBatch || markBatch == scanBatch)
	{
		/* No older markBatch that needs to be freed now */
	}
	else
	{
		/*
		 * Have a markBatch that isn't in batchringbuf; it was saved when
		 * tableam_util_release_batch was asked to release it earlier on.
		 *
		 * Note: this assumes that "batchringbuf->headBatch == scanPos->batch"
		 * is invariant.  In other words, it assumes that table AMs always
		 * remove an obsolescent scanBatch from the ring buffer at the point
		 * where they step off its underlying batch.
		 */
		Assert(!markBatch->isGuarded);
		Assert(!index_scan_batch_loaded(scan, markPos->batch) ||
			   index_scan_batch(scan, markPos->batch) != markBatch);

		release_and_unguard_batch(scan, markBatch, true);
	}

	batchringbuf->markPos = *scanPos;
	batchringbuf->markBatch = scanBatch;
}

/*
 * Restore scanPos to the previously saved markPos position.
 *
 * Called from the table AM's index_scan_restrpos callback.  Restores the
 * scan to a position saved using tableam_util_batchscan_mark_pos earlier.
 * The scan's markPos becomes its scanPos.  The marked batch is restored as
 * the current scanBatch when needed.
 *
 * We just discard all batches (other than markBatch/restored scanBatch),
 * except when markBatch is already the scan's current scanBatch.
 *
 * Note: This relies on the assumption that we already have a valid scanPos.
 * Table AMs must never call tableam_util_batchscan_reset between taking a
 * mark and restoring it, since resetting invalidates scanPos (and releases
 * the scan's markBatch).
 */
void
tableam_util_batchscan_restore_pos(IndexScanDesc scan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	BatchRingItemPos *markPos = &batchringbuf->markPos;
	IndexScanBatch markBatch = batchringbuf->markBatch;
	IndexScanBatch scanBatch = index_scan_batch(scan, scanPos->batch);

	Assert(scan->indexRelation->rd_indam->amcanmarkpos);
	Assert(scan->MVCCScan);
	Assert(scan->xs_table_opaque);
	Assert(scan->parallel_scan == NULL);

	/*
	 * The core executor must only ask us to restore a mark when it already
	 * had us take one on its behalf at some point during the ongoing scan
	 */
	Assert(markPos->valid);
	Assert(markPos->item >= markBatch->firstItem &&
		   markPos->item <= markBatch->lastItem);

	if (scanBatch == markBatch)
	{
		/* markBatch is already scanBatch; needn't change batchringbuf */
		Assert(scanPos->batch == markPos->batch);

		scanPos->item = markPos->item;
		return;
	}

	/*
	 * A batch is always unguarded by the time the scan moves on to a later
	 * batch, so markBatch (now behind scanBatch) cannot still be guarded.
	 * (Marks only come from nodeMergejoin.c, whose scans never change
	 * direction, so the scan can only have stepped off markBatch by first
	 * consuming all of its items -- and index-only scans drop the guard no
	 * later than the point where a batch's final item is returned.)
	 */
	Assert(!markBatch->isGuarded);

	/*
	 * markBatch is behind scanBatch, and so must not be saved in ring buffer
	 * anymore.  We have to deal with restoring the mark the hard way: by
	 * invalidating all other loaded batches.  This is similar to the case
	 * where the scan direction changes and the scan actually crosses
	 * batch/index page boundaries (see tableam_util_scanbatch_dirchange).
	 *
	 * First, free all batches that are still in the ring buffer.
	 */
	for (uint8 i = batchringbuf->headBatch; i != batchringbuf->nextBatch; i++)
	{
		IndexScanBatch batch = index_scan_batch(scan, i);

		Assert(batch != markBatch);

		tableam_util_release_batch(scan, batch);
	}

	/*
	 * Next "append" standalone markBatch, which will become scanBatch
	 * (scanBatch is always the ring buffer's headBatch)
	 */
	markPos->batch = 0;
	batchringbuf->scanPos = *markPos;
	batchringbuf->nextBatch = batchringbuf->headBatch = markPos->batch;
	index_scan_batch_append(scan, markBatch);
	Assert(index_scan_batch(scan, batchringbuf->scanPos.batch) == markBatch);

	/*
	 * Finally, call amposreset to let index AM know to invalidate any private
	 * state that independently tracks the scan's progress
	 */
	if (scan->indexRelation->rd_indam->amposreset)
		scan->indexRelation->rd_indam->amposreset(scan, markBatch);

	/*
	 * Note: markBatch.deadItems[] might already contain dead items, and might
	 * yet have more dead items saved.  tableam_util_release_batch is prepared
	 * for that.
	 */
}

/*
 * Handle cross-batch change in scan direction
 *
 * Called by table AM when its scan changes direction in a way that
 * necessitates backing the scan up to an index page originally associated
 * with a now-freed batch.
 *
 * When we return, batchringbuf will only contain one batch (the current
 * headBatch/scanBatch) and will look as if the new scan direction had been
 * used from the start.  Caller can then safely pass this batch to amgetbatch
 * to determine which batch comes next in the new scan direction.  This
 * approach isn't particularly efficient, but it works well enough for what
 * ought to be a relatively rare occurrence.
 */
void
tableam_util_scanbatch_dirchange(IndexScanDesc scan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	IndexScanBatch scanBatch;

	Assert(scan->indexRelation->rd_indam->amcanbackward);
	Assert(scan->MVCCScan);
	Assert(scan->parallel_scan == NULL);

	/*
	 * Release batches starting from the current "final" batch, working
	 * backwards until the current head batch (which is also the current
	 * scanBatch) is the only batch hasn't been freed
	 */
	while (index_scan_batch_count(scan) > 1)
	{
		uint8		finalidx = batchringbuf->nextBatch - 1;
		IndexScanBatch final = index_scan_batch(scan, finalidx);

		Assert(finalidx != batchringbuf->scanPos.batch);

		tableam_util_release_batch(scan, final);
		batchringbuf->nextBatch--;
	}

	/* scanBatch is now the only batch still loaded */
	Assert(batchringbuf->headBatch == batchringbuf->scanPos.batch);
	scanBatch = index_scan_batch(scan, batchringbuf->headBatch);

	/*
	 * Flip scanBatch's scan direction to reflect the reversal.  Also reset
	 * any index AM state that independently tracks scan progress.
	 */
	scanBatch->dir = -scanBatch->dir;
	if (scan->indexRelation->rd_indam->amposreset)
		scan->indexRelation->rd_indam->amposreset(scan, scanBatch);
}

/*
 * Record that scanPos item is dead
 *
 * Records an offset to the current scanBatch/scanPos item, saving it in
 * scanBatch's deadItems array.  The items' index tuples will later be
 * marked LP_DEAD when current scanBatch is freed.
 */
void
tableam_util_scanpos_killitem(IndexScanDesc scan)
{
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	IndexScanBatch scanBatch = index_scan_batch(scan, scanPos->batch);

	if (scanBatch->deadItems == NULL)
		scanBatch->deadItems = palloc_array(int, scan->maxitemsbatch);
	if (scanBatch->numDead < scan->maxitemsbatch)
		scanBatch->deadItems[scanBatch->numDead++] = scanPos->item;
}

/*
 * Release resources associated with a batch
 *
 * Called by table AM's amgetbatch index scan implementation when it is
 * finished with a batch and wishes to release its resources.
 *
 * Calling here when 'batch' is also batchringbuf.markBatch is a no-op.  Table
 * AM callers generally won't need to worry about this because it is handled
 * as a special case by the functions in this module (besides, the scan can
 * only have one markBatch at a time).
 *
 * We call amunguardbatch to drop the TID recycling interlock (e.g. buffer
 * pin) when it hasn't been dropped yet.  For plain MVCC scans (where
 * batchImmediateUnguard is set), the interlock was already dropped eagerly
 * in indexam_util_unlock_batch, so we skip the amunguardbatch call here.
 * Index-only scans must delay dropping the interlock until visibility is
 * resolved for all items in the batch, so amunguardbatch may still need to
 * act here.  For non-MVCC snapshot scans, the interlock is always held
 * until amunguardbatch drops it here -- this is the only place willing to
 * unguard a non-MVCC scan's batch.
 *
 * When the batch has dead items (numDead > 0) and the index AM provides an
 * amkillitemsbatch callback, we call it to set LP_DEAD bits in the index
 * page.  This is the natural place to kill index items because it's the
 * point when we know for sure that no further table accesses will take
 * place for that batch's items.
 */
void
tableam_util_release_batch(IndexScanDesc scan, IndexScanBatch batch)
{
	/* don't free caller's batch if it is scan's current markBatch */
	if (batch == scan->batchringbuf.markBatch)
		return;

	/* Pass through to implementation function, with allow_cache=true */
	release_and_unguard_batch(scan, batch, true);
}

/*
 * Free a batch, optionally caching it for reuse.
 *
 * When allow_cache is true, we try to store the batch in the scan's batch
 * cache for later reuse.  When allow_cache is false (typically because the
 * scan is shutting down), we pfree the caller's batch unconditionally.
 */
static void
release_and_unguard_batch(IndexScanDesc scan, IndexScanBatch batch,
						  bool allow_cache)
{
	Assert(!(scan->batchImmediateUnguard && batch->isGuarded));
	Assert(batch->isGuarded || scan->MVCCScan);

	/* Drop TID recycling interlock via amunguardbatch as needed */
	if (!scan->batchImmediateUnguard && batch->isGuarded)
		tableam_util_unguard_batch(scan, batch);

	/*
	 * Let the index AM set LP_DEAD bits in the index page, if applicable.
	 *
	 * batch.deadItems[] is now in whatever order the scan returned items in.
	 * We might have even saved the same item/TID twice.
	 *
	 * Sort and unique-ify deadItems[].  That way the index AM can safely
	 * assume that items will always be in their original index page order.
	 */
	Assert(!scan->xactStartedInRecovery || batch->numDead == 0);
	if (batch->numDead > 0 &&
		scan->indexRelation->rd_indam->amkillitemsbatch != NULL)
	{
		if (batch->numDead > 1)
		{
			qsort(batch->deadItems, batch->numDead, sizeof(int),
				  batch_compare_int);
			batch->numDead = qunique(batch->deadItems, batch->numDead,
									 sizeof(int), batch_compare_int);
		}

		scan->indexRelation->rd_indam->amkillitemsbatch(scan, batch);
	}

	/*
	 * Try to store caller's batch in this amgetbatch scan's cache of
	 * previously released batches first (when caller requests it)
	 */
	if (allow_cache && batch_cache_store(scan, batch))
		return;

	/* just pfree the caller's batch (plus batch's deadItems, if any) */
	if (batch->deadItems)
		pfree(batch->deadItems);
	pfree(index_scan_batch_base(scan, batch));
}

/*
 * Drop the batch's TID recycling interlock via amunguardbatch
 *
 * Called by the table AM when it's safe to drop whatever interlock the index
 * AM holds to prevent unsafe concurrent TID recycling by VACUUM (typically a
 * buffer pin on the batch's index page in batch's opaque area).
 */
void
tableam_util_unguard_batch(IndexScanDesc scan, IndexScanBatch batch)
{
	/* Should be called exactly once iff !batchImmediateUnguard */
	Assert(!scan->batchImmediateUnguard);
	Assert(batch->isGuarded);

	scan->indexRelation->rd_indam->amunguardbatch(scan, batch);

	batch->isGuarded = false;
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
indexam_util_unlock_batch(IndexScanDesc scan, IndexScanBatch batch, Buffer buf)
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
			batch->isGuarded = false;	/* won't call amunguardbatch */
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
 * a batch recycled from the cache managed by indexam_util_release_batch.  See
 * comments above indexam_util_release_batch.
 *
 * Housekeeping fields (buf, knownEndBackward/Forward, firstItem, lastItem,
 * numDead, deadItems, currTuples) are initialized here.  The table AM's
 * batch_init callback is invoked here to initialize the table AM opaque area.
 * The index AM caller is responsible for filling in its per-batch opaque
 * fields and the matching items[] array.
 *
 * Once the batch has the required matching items, caller should generally
 * pass it to indexam_util_unlock_batch, ahead of it being returned through
 * index AM's amgetbatch routine.  If it turns out that the batch won't need
 * to be returned like this (e.g., due to the scan having no more matches),
 * caller should pass its empty/unused batch to indexam_util_release_batch.
 */
IndexScanBatch
indexam_util_alloc_batch(IndexScanDesc scan)
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
		 * Subtotal #3: the currTuples workspace that comes after items[],
		 * where the index AM stores index tuples during index-only scans
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

		/* currTuples (if any) is directly after items[] */
		batch->currTuples = NULL;
		if (batch_tuples_workspace)
			batch->currTuples = (char *) batch + base_sz;
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
 * indexam_util_alloc_batch.  Batches are cached here for reuse to reduce
 * palloc/pfree overhead.
 *
 * It's safe to release a batch immediately when it was used to read a page
 * that returned no matches to the scan.  Batches actually returned by index
 * AM's amgetbatch routine (i.e. batches for pages with one or more matches)
 * must be released by tableam_util_release_batch, which calls here after the
 * index AM's amkillitemsbatch routine (if any).  Index AMs that use batches
 * should call here to release a batch from their amgetbatch or amgetbitmap
 * routines.
 *
 * The rules for batch ownership differ slightly for amgetbitmap scans; see
 * the amgetbitmap documentation in doc/src/sgml/indexam.sgml for details.
 */
void
indexam_util_release_batch(IndexScanDesc scan, IndexScanBatch batch)
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
		Assert(batch->currTuples == NULL);

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

/*
 * qsort comparison function for int arrays
 */
static int
batch_compare_int(const void *va, const void *vb)
{
	int			a = *((const int *) va);
	int			b = *((const int *) vb);

	return pg_cmp_s32(a, b);
}
