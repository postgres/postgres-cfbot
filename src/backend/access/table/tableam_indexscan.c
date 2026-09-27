/*-------------------------------------------------------------------------
 *
 * tableam_indexscan.c
 *	  Helpers for table AM index scan callbacks.
 *
 *
 * Table AMs can use these functions to implement xs_getnext_slot callbacks.
 * See access/tableam.h.  This includes the table AM side of batch-based index
 * scans.  The scan's batch ring buffer is managed by the table AM, since it
 * is closely tied to whatever mechanism the table AM uses to implement
 * prefetching of table blocks (typically a read stream).
 *
 * The ring buffer loads batches in index key space/index scan order.  This
 * allows the table AM to maintain an adequate prefetch distance: prefetching
 * is thereby able to request table blocks referenced by index pages that are
 * well ahead of the current scan position's index page.
 *
 * The tableam_index_* functions manage the batch ring buffer's lifecycle and
 * positional state, and help with certain aspects of resource management.
 * The table AM uses scanPos to return items from batches returned by
 * amgetbatch.  Table AMs that support I/O prefetching of table blocks during
 * index scans use prefetchPos to request table blocks well ahead of those
 * that are of immediate interest to scanPos.
 *
 * Batches are allocated and released on behalf of index AMs by the support
 * routines in batchscan.c.  Index AMs free and unlock batches as described
 * in indexam.sgml.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/table/tableam_indexscan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/batchscan.h"
#include "access/tableam_indexscan.h"
#include "common/int.h"
#include "lib/qunique.h"
#include "utils/builtins.h"

/* GUC storage */
bool		debug_disable_indexscan_prefetch = false;

static void release_and_unguard_batch(IndexScanDesc scan, IndexScanBatch batch,
									  bool allow_cache);
static int	batch_compare_int(const void *va, const void *vb);

/*
 * Reset ring buffer and related positional state used during an amgetbatch
 * index scan.
 *
 * Table AM caller should pass endscan=false, which makes us cache any freed
 * batches for reuse on rescan.  We release scan's markBatch here either way.
 */
void
tableam_index_batchscan_reset(IndexScanDesc scan, bool endscan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	IndexScanBatch markBatch = batchringbuf->markBatch;
	bool		markBatchFreed = false;

	batchringbuf->scanPos.valid = false;
	batchringbuf->prefetchPos.valid = false;
	batchringbuf->markPos.valid = false;

	for (uint8 i = batchringbuf->headBatch; i != batchringbuf->nextBatch; i++)
	{
		IndexScanBatch batch = tableam_index_batch(scan, i);

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
tableam_index_batchscan_end(IndexScanDesc scan)
{
	/* Free all remaining loaded batches (even markBatch), bypassing cache */
	tableam_index_batchscan_reset(scan, true);

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
 * point later, via tableam_index_batchscan_restore_pos.  The marked batch is
 * retained and not freed until a new mark is set or the scan ends (or until
 * the mark is restored).
 */
void
tableam_index_batchscan_mark_pos(IndexScanDesc scan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	BatchRingItemPos *markPos PG_USED_FOR_ASSERTS_ONLY = &batchringbuf->markPos;
	IndexScanBatch scanBatch = tableam_index_batch(scan, scanPos->batch);
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
		 * tableam_index_release_batch was asked to release it earlier on.
		 *
		 * Note: this assumes that "batchringbuf->headBatch == scanPos->batch"
		 * is invariant.  In other words, it assumes that table AMs always
		 * remove an obsolescent scanBatch from the ring buffer at the point
		 * where they step off its underlying batch.
		 */
		Assert(!markBatch->isGuarded);
		Assert(!tableam_index_batch_loaded(scan, markPos->batch) ||
			   tableam_index_batch(scan, markPos->batch) != markBatch);

		release_and_unguard_batch(scan, markBatch, true);
	}

	batchringbuf->markPos = *scanPos;
	batchringbuf->markBatch = scanBatch;
}

/*
 * Restore scanPos to the previously saved markPos position.
 *
 * Called from the table AM's index_scan_restrpos callback.  Restores the
 * scan to a position saved using tableam_index_batchscan_mark_pos earlier.
 * The scan's markPos becomes its scanPos.  The marked batch is restored as
 * the current scanBatch when needed.
 *
 * We just discard all batches (other than markBatch/restored scanBatch),
 * except when markBatch is already the scan's current scanBatch.  We always
 * invalidate prefetchPos.  The table AM's prefetching state (e.g., its read
 * stream) is reset by the caller (which calls this function as it resets that
 * state).  This approach keeps things simple for table AMs: most code that
 * deals with batches is thereby able to assume that the common case where
 * scan direction never changes is the only case.
 *
 * Note: This relies on the assumption that we already have a valid scanPos.
 * Table AMs must never call tableam_index_batchscan_reset between taking a
 * mark and restoring it, since resetting invalidates scanPos (and releases
 * the scan's markBatch).
 */
void
tableam_index_batchscan_restore_pos(IndexScanDesc scan)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	BatchRingItemPos *markPos = &batchringbuf->markPos;
	IndexScanBatch markBatch = batchringbuf->markBatch;
	IndexScanBatch scanBatch = tableam_index_batch(scan, scanPos->batch);

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

	/*
	 * Restoring a mark always requires stopping prefetching.  This is similar
	 * to the handling table AMs implement to deal with a tuple-level change
	 * in the scan's direction.
	 */
	batchringbuf->prefetchPos.valid = false;

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
	 * batch/index page boundaries (see tableam_index_scanbatch_dirchange).
	 *
	 * First, free all batches that are still in the ring buffer.
	 */
	for (uint8 i = batchringbuf->headBatch; i != batchringbuf->nextBatch; i++)
	{
		IndexScanBatch batch = tableam_index_batch(scan, i);

		Assert(batch != markBatch);

		tableam_index_release_batch(scan, batch);
	}

	/*
	 * Next "append" standalone markBatch, which will become scanBatch
	 * (scanBatch is always the ring buffer's headBatch)
	 */
	markPos->batch = 0;
	batchringbuf->scanPos = *markPos;
	batchringbuf->nextBatch = batchringbuf->headBatch = markPos->batch;
	tableam_index_batch_append(scan, markBatch);
	Assert(tableam_index_batch(scan, batchringbuf->scanPos.batch) == markBatch);

	/*
	 * Finally, call amposreset to let index AM know to invalidate any private
	 * state that independently tracks the scan's progress
	 */
	if (scan->indexRelation->rd_indam->amposreset)
		scan->indexRelation->rd_indam->amposreset(scan, markBatch);

	/*
	 * Note: markBatch.deadItems[] might already contain dead items, and might
	 * yet have more dead items saved.  tableam_index_release_batch is
	 * prepared for that.
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
tableam_index_scanbatch_dirchange(IndexScanDesc scan)
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
	while (tableam_index_batch_count(scan) > 1)
	{
		uint8		finalidx = batchringbuf->nextBatch - 1;
		IndexScanBatch final = tableam_index_batch(scan, finalidx);

		Assert(finalidx != batchringbuf->scanPos.batch);

		tableam_index_release_batch(scan, final);
		batchringbuf->nextBatch--;
	}

	/* scanBatch is now the only batch still loaded */
	Assert(batchringbuf->headBatch == batchringbuf->scanPos.batch);
	scanBatch = tableam_index_batch(scan, batchringbuf->headBatch);

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
tableam_index_scanpos_killitem(IndexScanDesc scan)
{
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	IndexScanBatch scanBatch = tableam_index_batch(scan, scanPos->batch);

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
 * in batchscan_unlock, so we skip the amunguardbatch call here.
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
tableam_index_release_batch(IndexScanDesc scan, IndexScanBatch batch)
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
		tableam_index_unguard_batch(scan, batch);

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

	if (allow_cache)
	{
		/* Return the batch to the index AM side, which caches it for reuse */
		batchscan_release(scan, batch);
		return;
	}

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
tableam_index_unguard_batch(IndexScanDesc scan, IndexScanBatch batch)
{
	/* Should be called exactly once iff !batchImmediateUnguard */
	Assert(!scan->batchImmediateUnguard);
	Assert(batch->isGuarded);

	scan->indexRelation->rd_indam->amunguardbatch(scan, batch);

	batch->isGuarded = false;
}

/*
 * Copy all name columns stored as cstrings back into NAMEDATALEN bytes of
 * xs_name_cstring_buf.
 *
 * Called by tableam_index_fill_ios_slot (kept out of line, since it's needed
 * only by index-only scans of indexes on "name" columns).
 */
pg_attribute_cold void
tableam_index_fill_ios_names(IndexScanDesc scan, TupleTableSlot *slot)
{
	for (int idx = 0; idx < scan->xs_name_cstring_count; idx++)
	{
		int			attnum = scan->xs_name_cstring_attnums[idx];
		Name		name;

		/* skip null Datums */
		if (slot->tts_isnull[attnum])
			continue;

		/* use namestrcpy to zero-pad all trailing bytes */
		name = (Name) (scan->xs_name_cstring_buf + idx * NAMEDATALEN);
		namestrcpy(name, DatumGetCString(slot->tts_values[attnum]));
		slot->tts_values[attnum] = NameGetDatum(name);
	}
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
