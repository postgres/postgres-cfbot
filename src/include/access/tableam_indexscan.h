/*-------------------------------------------------------------------------
 *
 * tableam_indexscan.h
 *	  Helpers for table AM index scan callbacks.
 *
 * Table AMs can use these functions to implement xs_getnext_slot callbacks.
 * See access/tableam.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tableam_indexscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLEAM_INDEXSCAN_H
#define TABLEAM_INDEXSCAN_H

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "executor/tuptable.h"
#include "pgstat.h"
#include "utils/rel.h"

extern PGDLLIMPORT bool debug_disable_indexscan_prefetch;

extern void tableam_index_batchscan_reset(IndexScanDesc scan, bool endscan);
extern void tableam_index_batchscan_end(IndexScanDesc scan);
extern void tableam_index_batchscan_mark_pos(IndexScanDesc scan);
extern void tableam_index_batchscan_restore_pos(IndexScanDesc scan);
extern void tableam_index_scanbatch_dirchange(IndexScanDesc scan);
extern void tableam_index_scanpos_killitem(IndexScanDesc scan);
extern void tableam_index_release_batch(IndexScanDesc scan, IndexScanBatch batch);
extern void tableam_index_unguard_batch(IndexScanDesc scan, IndexScanBatch batch);
extern pg_attribute_cold void tableam_index_fill_ios_names(IndexScanDesc scan,
														   TupleTableSlot *slot);


/*
 * Fetch the next matching TID for the scan (or the first) through the
 * amgettuple interface.
 *
 * Returns true when a TID was found (the index AM will have saved it in
 * scan->xs_heaptid), or false when we're out of index entries.
 */
static inline bool
tableam_index_getnext_tid(IndexScanDesc scan, ScanDirection direction)
{
	bool		found;

	found = scan->indexRelation->rd_indam->amgettuple(scan, direction);

	/* Reset kill flag immediately for safety */
	scan->kill_prior_tuple = false;
	scan->xs_heap_continue = false;

	/* If we're out of index entries, we're done */
	if (!found)
		return false;

	Assert(ItemPointerIsValid(&scan->xs_heaptid));

	pgstat_count_index_tuples(scan->indexRelation, 1);

	return true;
}

/*
 * Fill an index-only scan's result slot from the data the index AM returned.
 *
 * The data is provided in either HeapTuple (xs_hitup) or IndexTuple (xs_itup)
 * format.  An index AM may fill both, in which case the heap format is used,
 * since it's a bit cheaper to fill a slot from.
 *
 * Called by table AMs from their xs_getnext_slot callbacks.
 */
static inline void
tableam_index_fill_ios_slot(IndexScanDesc scan, TupleTableSlot *slot)
{
	ExecClearTuple(slot);

	/*
	 * We must deform the tuple using the tupdesc the index AM formed it with
	 * (xs_hitupdesc or xs_itupdesc), not the slot's tupdesc.  The datums
	 * returned by the index AM must be binary compatible, but the descriptors
	 * may align each column differently in certain rare cases. (Actually,
	 * btree's "name" opclass stores cstring tuples that _aren't_ even binary
	 * compatible, in the strictest sense.  tableam_index_fill_ios_names
	 * handles that.)
	 */
	if (scan->xs_hitup)
	{
		Assert(slot->tts_tupleDescriptor->natts == scan->xs_hitupdesc->natts);

		heap_deform_tuple(scan->xs_hitup, scan->xs_hitupdesc,
						  slot->tts_values, slot->tts_isnull);
	}
	else if (scan->xs_itup)
	{
		Assert(slot->tts_tupleDescriptor->natts == scan->xs_itupdesc->natts);

		index_deform_tuple(scan->xs_itup, scan->xs_itupdesc,
						   slot->tts_values, slot->tts_isnull);

		/*
		 * Copy all name columns stored as cstrings back into NAMEDATALEN
		 * bytes of xs_name_cstring_buf.  We mark this branch as unlikely as
		 * generally "name" is used only for the system catalogs and this
		 * would have to be a user query running on those or some other user
		 * table with an index on a name column.
		 */
		if (unlikely(scan->xs_name_cstring_attnums != NULL))
			tableam_index_fill_ios_names(scan, slot);
	}
	else
		elog(ERROR, "no data returned for index-only scan");

	ExecStoreVirtualTuple(slot);
}

/* ----------------------------------------------------------------------------
 * Elementary batch ring buffer operations
 * ----------------------------------------------------------------------------
 */

StaticAssertDecl(INDEX_SCAN_MAX_BATCHES <= PG_INT8_MAX + 1,
				 "tableam_index_batch_loaded relies on int8 ring buffer arithmetic");
StaticAssertDecl((INDEX_SCAN_MAX_BATCHES & (INDEX_SCAN_MAX_BATCHES - 1)) == 0,
				 "INDEX_SCAN_MAX_BATCHES must be a power of 2");

/*
 * Sets up the batch ring buffer structure for use by an index scan.
 *
 * Called from table AM's index_scan_begin callback during amgetbatch scans.
 */
static inline void
tableam_index_batchscan_init(IndexScanDesc scan)
{
	Assert(scan->indexRelation->rd_indam->amgetbatch != NULL);

	scan->batchringbuf.scanPos.valid = false;
	scan->batchringbuf.prefetchPos.valid = false;
	scan->batchringbuf.markPos.valid = false;

	scan->batchringbuf.markBatch = NULL;
	scan->batchringbuf.headBatch = 0;
	scan->batchringbuf.nextBatch = 0;

	scan->usebatchring = true;
}

/*
 * How many batches are currently loaded in the ring buffer?
 */
static inline uint8
tableam_index_batch_count(IndexScanDesc scan)
{
	return (uint8) (scan->batchringbuf.nextBatch -
					scan->batchringbuf.headBatch);
}

/*
 * Do we already have a batch loaded at 'idx' offset in scan's ring buffer?
 *
 * NOTE: a stale batch idx can alias a currently-loaded range due to
 * wraparound, producing a false positive.  False negatives are not possible.
 */
static inline bool
tableam_index_batch_loaded(IndexScanDesc scan, uint8 idx)
{
	return (int8) (idx - scan->batchringbuf.headBatch) >= 0 &&
		(int8) (idx - scan->batchringbuf.nextBatch) < 0;
}

/*
 * Have we loaded the maximum number of batches?
 */
static inline bool
tableam_index_batch_full(IndexScanDesc scan)
{
	return tableam_index_batch_count(scan) == INDEX_SCAN_MAX_BATCHES;
}

/*
 * Return batch for the provided index.
 */
static inline IndexScanBatch
tableam_index_batch(IndexScanDesc scan, uint8 idx)
{
	Assert(tableam_index_batch_loaded(scan, idx));

	return scan->batchbuf[idx & (INDEX_SCAN_MAX_BATCHES - 1)];
}

/*
 * Append given batch to scan's batch ring buffer.
 */
static inline void
tableam_index_batch_append(IndexScanDesc scan, IndexScanBatch batch)
{
	BatchRingBuffer *ringbuf = &scan->batchringbuf;
	uint8		nextBatch = ringbuf->nextBatch;

	Assert(!tableam_index_batch_full(scan));

	scan->batchbuf[nextBatch & (INDEX_SCAN_MAX_BATCHES - 1)] = batch;
	ringbuf->nextBatch++;
}

/* ----------------------------------------------------------------------------
 * Elementary batch position operations
 * ----------------------------------------------------------------------------
 */

/*
 * Compare two batch ring positions in the given scan direction.
 *
 * Returns negative if pos1 is behind pos2, 0 if equal, positive if pos1 is
 * ahead of pos2.
 */
static inline int
tableam_index_pos_cmp(BatchRingItemPos *pos1, BatchRingItemPos *pos2,
					  ScanDirection direction)
{
	int8		batchdiff;

	Assert(pos1->valid && pos2->valid);

	batchdiff = (int8) (pos1->batch - pos2->batch);

	Assert(batchdiff > -INDEX_SCAN_MAX_BATCHES &&
		   batchdiff < INDEX_SCAN_MAX_BATCHES);

	if (batchdiff != 0)
	{
		/* Resolve comparison using differing batch offsets */
		return batchdiff;
	}

	/*
	 * Resolve comparison using items[]-wise indexes from caller's positions,
	 * since both positions point to the same ring buffer batch
	 */
	if (ScanDirectionIsForward(direction))
		return pos1->item - pos2->item;
	else
		return pos2->item - pos1->item;
}

/*
 * Advance position to its next item in the batch.
 *
 * Advance to the next item within the provided batch (or to the previous item,
 * when scanning backwards).
 *
 * Returns true if the position could be advanced.  Returns false when there
 * are no more items from the batch remaining in the given scan direction.
 */
static inline bool
tableam_index_pos_advance(ScanDirection direction,
						  IndexScanBatch batch, BatchRingItemPos *pos)
{
	/*
	 * On entry, pos->item must be valid, and must actually point to a valid
	 * item for this batch.  There is exactly one exception: pos->item may
	 * initially sit one step outside the batch when caller just flipped its
	 * scan direction.  pos->item will point to a valid item once we return
	 * (we _must_ return true when passed a just-stepped-off-batch position).
	 *
	 * This precondition ensures that callers actually step to the next batch
	 * when indicated (or flip the scan direction instead, which can happen
	 * right after a cursor tries to step off the final batch in the given
	 * scan direction).  Table AMs must avoid ambiguous positional states.
	 */
	Assert(pos->valid);

	if (ScanDirectionIsForward(direction))
	{
		/* Precondition: valid-or-just-before-start item position */
		Assert(pos->item >= batch->firstItem - 1);
		Assert(pos->item <= batch->lastItem);

		if (++pos->item > batch->lastItem)
			return false;
	}
	else						/* ScanDirectionIsBackward */
	{
		/* Precondition: valid-or-just-past-end item position */
		Assert(pos->item >= batch->firstItem);
		Assert(pos->item <= batch->lastItem + 1);

		if (--pos->item < batch->firstItem)
			return false;
	}

	/* Advanced within batch */
	return true;
}

/*
 * Position pos at the start of newBatch (in the given scan direction).
 *
 * When we're called, pos should point to a batch that caller just finished
 * consuming from (or be invalid, when no batch has been loaded for caller's
 * scan yet).  When we return, pos will point to newBatch, the next batch from
 * the ring buffer.  We'll have also set pos's item offset to newBatch's
 * initial item in the given direction (the first item when scanning forwards,
 * the last item when scanning backwards).
 *
 * newBatch doesn't have to be (and often isn't) the most recently appended
 * batch in the scan's ring buffer.  It is merely the next batch in line to be
 * consumed from the point of view of our caller.
 */
static inline void
tableam_index_pos_startbatch(ScanDirection direction,
							 IndexScanBatch newBatch, BatchRingItemPos *pos)
{
	Assert(newBatch->dir == direction);
	Assert(newBatch->firstItem >= 0 && newBatch->firstItem <= newBatch->lastItem);

	/* Increment batch (might wrap), or initialize it to zero */
	if (pos->valid)
		pos->batch++;
	else
		pos->batch = 0;

	pos->valid = true;

	if (ScanDirectionIsForward(direction))
		pos->item = newBatch->firstItem;
	else
		pos->item = newBatch->lastItem;
}

/* ----------------------------------------------------------------------------
 * scanPos/prefetchPos advancement functions
 * ----------------------------------------------------------------------------
 */

/*
 * Try to advance the scan's scanPos to the next matching item from the
 * scan's existing scanBatch, moving in the given scan direction.
 *
 * Sets *scanBatch to the ring buffer's existing scanBatch, or to NULL when no
 * batch has been loaded yet (the first call here for the entire scan).
 *
 * Returns true when scanPos was advanced, in which case the scan should
 * process the item that scanPos now points to.  Returns false when there are
 * no more matching items remaining in scanBatch (or when no scanBatch has
 * been loaded yet).  Caller responds to a false return by passing *scanBatch
 * to tableam_index_fetch_next_batch as its priorBatch argument, advancing the
 * scan to its next batch.
 */
static pg_always_inline bool
tableam_index_scanpos_advance(IndexScanDesc scan, ScanDirection direction,
							  IndexScanBatch *scanBatch, BatchRingItemPos *scanPos)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;

	if (!scanPos->valid)
	{
		/* First call here for the entire scan */
		Assert(tableam_index_batch_count(scan) == 0);

		*scanBatch = NULL;
		return false;
	}

	/*
	 * scanPos is valid, so scanBatch must already be loaded in batch ring
	 * buffer.  We rely on that here (can't do this with prefetchBatch).
	 */
	pg_assume(batchringbuf->headBatch == scanPos->batch);

	*scanBatch = tableam_index_batch(scan, scanPos->batch);

	return tableam_index_pos_advance(direction, *scanBatch, scanPos);
}

/*
 * Fetch the next batch of matching items for the scan (or the first).
 *
 * Called when caller's current scanBatch/prefetchBatch (passed to us as
 * priorBatch) has no more matching items in the given scan direction.  Caller
 * passes a NULL priorBatch on the first call here for the scan.
 *
 * Returns the next batch to be processed by caller in the given scan
 * direction, or NULL when there are no more matches in that direction.
 * Returned batch will have already been appended to the scan's ring buffer
 * (though not necessarily during this call).
 *
 * We don't free any batches here; that is a separate step performed by
 * tableam_index_scanpos_nextbatch.  Caller also needs to advance their
 * scanPos/prefetchPos position to the start of the returned batch.
 */
static pg_always_inline IndexScanBatch
tableam_index_fetch_next_batch(IndexScanDesc scan, ScanDirection direction,
							   IndexScanBatch priorBatch, BatchRingItemPos *pos)
{
	IndexScanBatch batch = NULL;
	BatchRingBuffer *batchringbuf PG_USED_FOR_ASSERTS_ONLY = &scan->batchringbuf;

	Assert(scan->usebatchring);

	if (!priorBatch)
	{
		/* First call for the scan */
		Assert(pos == &batchringbuf->scanPos);
	}
	else if (unlikely(priorBatch->dir != direction))
	{
		/*
		 * We detected a change in scan direction across batches.  Prepare
		 * scan's batchringbuf state for us to get the next batch for the
		 * opposite scan direction to the one used when priorBatch was
		 * returned by amgetbatch.
		 */
		tableam_index_scanbatch_dirchange(scan);

		/* priorBatch is now batchringbuf's only batch */
		Assert(pos->batch == batchringbuf->headBatch);
		Assert(tableam_index_batch_count(scan) == 1);
	}
	else if (tableam_index_batch_loaded(scan, pos->batch + 1))
	{
		/* Next batch already loaded for us */
		batch = tableam_index_batch(scan, pos->batch + 1);

		Assert(priorBatch->dir == direction);
		Assert(batch->dir == direction);
		Assert(batch->firstItem >= 0 && batch->firstItem <= batch->lastItem);
		return batch;
	}

	/*
	 * Assert preconditions for calling amgetbatch.
	 *
	 * priorBatch had better be for the last valid batch currently in the ring
	 * buffer (batches must stay in scan order).  If it isn't then we should
	 * have already returned some existing loaded batch earlier.
	 */
	Assert(!tableam_index_batch_full(scan));
	Assert(!priorBatch ||
		   (tableam_index_batch_count(scan) > 0 && priorBatch->dir == direction &&
			tableam_index_batch(scan, batchringbuf->nextBatch - 1) == priorBatch));

	/*
	 * Before we call amgetbatch again, check if priorBatch is already known
	 * to be the last batch with matching items in this scan direction
	 */
	if (priorBatch &&
		(ScanDirectionIsForward(direction) ?
		 priorBatch->knownEndForward :
		 priorBatch->knownEndBackward))
		return NULL;

	batch = scan->indexRelation->rd_indam->amgetbatch(scan, priorBatch,
													  direction);
	if (batch)
	{
		/* We got the batch from the index AM */
		Assert(batch->dir == direction);
		Assert(batch->firstItem >= 0 && batch->firstItem <= batch->lastItem);

		pgstat_count_index_tuples(scan->indexRelation,
								  (batch->lastItem - batch->firstItem) + 1);

		/* Append batch to the end of ring buffer/write it to buffer index */
		tableam_index_batch_append(scan, batch);

		/*
		 * We don't set knownEndForward/knownEndBackward to true (whichever is
		 * used when moving in the opposite direction) here when this is the
		 * scan's first returned batch.  The index AM will generally do so for
		 * us (though we don't rely on that for correctness).
		 */
	}
	else
	{
		/* amgetbatch returned NULL */
		if (priorBatch)
		{
			/*
			 * There are no further matches to be found in the current scan
			 * direction, following priorBatch.  Remember that priorBatch is
			 * the last batch with matching items.
			 */
			if (ScanDirectionIsForward(direction))
				priorBatch->knownEndForward = true;
			else
				priorBatch->knownEndBackward = true;
		}
	}

	return batch;
}

/*
 * Position scanPos at the start of newScanBatch (in the given scan
 * direction), and remove the scan's old scanBatch from the ring buffer.
 *
 * Called after tableam_index_fetch_next_batch returns newScanBatch, the next
 * batch that scanPos will consume matching items from.  We release the
 * now-obsolescent old scanBatch (the ring buffer's head batch), freeing up
 * its ring buffer slot.  (When newScanBatch is the scan's first batch, there
 * is no old scanBatch for us to release.)
 *
 * Return value indicates if a previously occupied ring buffer slot was freed.
 * A table AM that paused its prefetch mechanism because the ring buffer was
 * full (see tableam_index_prefetchpos_advance) can resume it when we return
 * true (to indicate to caller that there's now space to store another batch).
 */
static pg_always_inline bool
tableam_index_scanpos_nextbatch(IndexScanDesc scan, ScanDirection direction,
								IndexScanBatch newScanBatch)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &batchringbuf->scanPos;
	BatchRingItemPos *prefetchPos = &batchringbuf->prefetchPos;
	bool		releaseOldHeadBatch = scanPos->valid;
	IndexScanBatch headBatch;

	/* Position scanPos to the start of new scanBatch */
	tableam_index_pos_startbatch(direction, newScanBatch, scanPos);
	Assert(tableam_index_batch(scan, scanPos->batch) == newScanBatch);

	if (!releaseOldHeadBatch)
	{
		/* newScanBatch is the scan's first and only batch */
		Assert(batchringbuf->headBatch == scanPos->batch);
		return false;
	}

	headBatch = tableam_index_batch(scan, batchringbuf->headBatch);

	Assert(headBatch != newScanBatch);
	Assert(batchringbuf->headBatch != scanPos->batch);

	/* free obsolescent head batch (unless it is scan's markBatch) */
	tableam_index_release_batch(scan, headBatch);

	/*
	 * If we're about to release the batch that prefetchPos currently points
	 * to, just invalidate prefetchPos.  This keeps prefetchPos from ever
	 * falling behind scanPos at the batch granularity, which
	 * tableam_index_prefetchpos_catchup relies on.
	 */
	if (prefetchPos->valid &&
		prefetchPos->batch == batchringbuf->headBatch)
		prefetchPos->valid = false;

	/* Remove the batch from the ring buffer (even if it's markBatch) */
	batchringbuf->headBatch++;

	/* Postconditions for having freed up a ring buffer slot */
	Assert(!prefetchPos->valid ||
		   tableam_index_batch_loaded(scan, prefetchPos->batch));
	Assert(!tableam_index_batch_full(scan));
	Assert(batchringbuf->headBatch == scanPos->batch);

	return true;
}

/*
 * Handle initialization of the scan's prefetchPos, when prefetchPos isn't
 * yet valid (also handles the prefetchPos < scanPos edge case).
 *
 * Called at the start of each table AM prefetch callback call.  Returns true
 * after setting prefetchPos to the scan's current scanPos.  That's a special
 * case: the prefetch callback should process the very item that the scan is
 * on directly (e.g., by returning that item's table block to its read
 * stream), rather than reading ahead of the scan.  Returns false when
 * prefetchPos is ahead of (or equal to) scanPos, in which case the prefetch
 * callback picks up from where its last call left off.
 */
static inline bool
tableam_index_prefetchpos_catchup(IndexScanDesc scan, ScanDirection direction)
{
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos = &batchringbuf->scanPos;
	BatchRingItemPos *prefetchPos = &batchringbuf->prefetchPos;

	/*
	 * scanPos must always be valid when prefetching takes place.  There has
	 * to be at least one batch, loaded as the scan's scanBatch.
	 */
	Assert(tableam_index_batch_count(scan) > 0);
	Assert(scanPos->valid && tableam_index_batch_loaded(scan, scanPos->batch));

	/*
	 * prefetchPos can "fall behind" scanPos at the item granularity: the
	 * prefetch callback only runs on demand, so scanPos can overtake
	 * prefetchPos whenever the scan consumes items without the callback being
	 * called (e.g., runs of adjacent matching items whose TIDs all point to
	 * the same table block).  We handle that case using exactly the same
	 * steps as initialization.
	 *
	 * prefetchPos can never fall behind scanPos at the batch granularity,
	 * since tableam_index_scanpos_nextbatch invalidates prefetchPos before
	 * releasing the batch that prefetchPos points to.  There is therefore no
	 * danger of prefetchPos.batch falling so far behind scanPos.batch that it
	 * wraps around (and appears to be ahead of scanPos instead of behind it).
	 */
	if (unlikely(!prefetchPos->valid ||
				 tableam_index_pos_cmp(scanPos, prefetchPos, direction) > 0))
	{
		*prefetchPos = *scanPos;
		return true;
	}

	/* Picking up prefetching from where the last callback call left off */
	Assert(tableam_index_pos_cmp(scanPos, prefetchPos, direction) <= 0);
	return false;
}

/*
 * Result of a tableam_index_prefetchpos_advance call
 */
typedef enum BatchPosAdvanceResult
{
	BATCH_POS_ADVANCED,			/* advanced to next item in current batch */
	BATCH_POS_BATCH_ADVANCED,	/* advanced to first item of new batch */
	BATCH_POS_DONE,				/* no further matching items in direction */
	BATCH_POS_RING_FULL,		/* couldn't advance; ring buffer full */
} BatchPosAdvanceResult;

/*
 * Advance the scan's prefetchPos to the next item that the table AM's
 * prefetch callback should consider reading ahead, moving in the given scan
 * direction.
 *
 * On entry, *prefetchBatch must be the batch that prefetchPos points to.
 * Advances prefetchPos to the next item within *prefetchBatch when possible
 * (returns BATCH_POS_ADVANCED).  Otherwise tries to advance to the scan's
 * next batch, setting *prefetchBatch to the new batch and positioning
 * prefetchPos at its first item in the scan direction (returns
 * BATCH_POS_BATCH_ADVANCED).  Callers must use the returned result (never
 * compare *prefetchBatch against its earlier value) to detect this case;
 * batch recycling can reuse the memory of a recently released batch.
 *
 * Returns BATCH_POS_DONE when there are no further matching items in the
 * given scan direction (*prefetchBatch is set to NULL).
 *
 * Returns BATCH_POS_RING_FULL when the next batch couldn't be loaded because
 * all available ring buffer batch slots are currently in use (prefetchPos
 * and *prefetchBatch are left unchanged).  Caller responds by momentarily
 * pausing its read-ahead mechanism; it can be resumed once
 * tableam_index_scanpos_nextbatch reports that the scan freed up a slot
 * (which'll happen only after scanPos has consumed all remaining items from
 * the scan's current scanBatch).
 *
 * When caller passes throttle=true we likewise decline to advance to the next
 * batch and return BATCH_POS_RING_FULL instead.  Caller uses this to cap how
 * many batches a single read-ahead callback invocation can advance by.
 * Advancing within the current batch (BATCH_POS_ADVANCED) ignores throttle,
 * so throttling only takes effect at a batch boundary.
 */
static inline BatchPosAdvanceResult
tableam_index_prefetchpos_advance(IndexScanDesc scan, ScanDirection direction,
								  IndexScanBatch *prefetchBatch,
								  BatchRingItemPos *prefetchPos,
								  bool throttle)
{
	if (tableam_index_pos_advance(direction, *prefetchBatch, prefetchPos))
	{
		/* Advanced within current prefetchBatch */
		return BATCH_POS_ADVANCED;
	}

	/*
	 * Ran out of items from current prefetchBatch.  Try to advance
	 * prefetchBatch to the next batch in this scan direction.
	 */
	if (unlikely(tableam_index_batch_full(scan)) || unlikely(throttle))
	{
		/*
		 * Can't advance prefetchBatch because all available ring buffer batch
		 * slots are currently in use (or because caller wants us to throttle
		 * instead of returning another batch).  Undo changes that our call to
		 * tableam_index_pos_advance just made to prefetchPos before
		 * returning, leaving it in a state that's consistent with the work
		 * actually performed (various positional state assertions expect
		 * this).
		 */
		if (ScanDirectionIsForward(direction))
		{
			Assert(prefetchPos->item == (*prefetchBatch)->lastItem + 1);
			prefetchPos->item--;
		}
		else					/* ScanDirectionIsBackward */
		{
			Assert(prefetchPos->item == (*prefetchBatch)->firstItem - 1);
			prefetchPos->item++;
		}

		return BATCH_POS_RING_FULL;
	}

	/* We have a free ring buffer slot to fit another batch */
	*prefetchBatch = tableam_index_fetch_next_batch(scan, direction,
													*prefetchBatch, prefetchPos);
	if (*prefetchBatch == NULL)
	{
		/* No more matches in this scan direction */
		return BATCH_POS_DONE;
	}

	/*
	 * Have a new prefetchBatch.
	 *
	 * tableam_index_fetch_next_batch already appended the new batch to the
	 * ring buffer for us, but we must advance prefetchPos for ourselves.
	 * Position prefetchPos to the start of the new batch.
	 */
	tableam_index_pos_startbatch(direction, *prefetchBatch, prefetchPos);

	return BATCH_POS_BATCH_ADVANCED;
}

#endif							/* TABLEAM_INDEXSCAN_H */
