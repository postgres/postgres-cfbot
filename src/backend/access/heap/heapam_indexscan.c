/*-------------------------------------------------------------------------
 *
 * heapam_indexscan.c
 *	  heap table plain index scan and index-only scan code
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_indexscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/heapam.h"
#include "access/indexbatch.h"
#include "access/relscan.h"
#include "access/visibilitymap.h"
#include "optimizer/cost.h"
#include "storage/predicate.h"
#include "utils/pgstat_internal.h"

/*
 * We avoid creating a read stream during very selective scans that require
 * few heap fetches, where the overhead of creating a read stream is unlikely
 * to pay for itself
 */
#define INDEX_PREFETCH_BLKSWITCH_THRESHOLD 4

/*
 * Maximum number of batches that a single heapam_index_prefetch_next_block
 * call may advance prefetchBatch by without returning a heap block
 */
#define INDEX_PREFETCH_MAX_BATCH_ADVANCES 1

/*
 * heapam's per-batch private opaque area (only used during index-only scans).
 *
 * Maintains a per-batch visibility information cache, populated on demand
 * using the visibility map.  It is important that we set each batch item's
 * batchvis[] entry exactly once (or not at all).
 */
typedef struct HeapBatchData
{
	/*
	 * Range of batchvis[] entries with valid visibility info, in items[]-wise
	 * terms.  An item's visibility is cached iff firstVisSet <= item <=
	 * lastVisSet.
	 */
	int			firstVisSet;	/* first valid batchvis[] entry, or > last */
	int			lastVisSet;		/* last valid batchvis[] entry, or < first */

	/* maxitemsbatch-many all-visible states, valid only within range */
	bool		batchvis[FLEXIBLE_ARRAY_MEMBER];
} HeapBatchData;

#define HEAP_BATCH_VIS_CACHED(hbatch, item) \
	((item) >= (hbatch)->firstVisSet && (item) <= (hbatch)->lastVisSet)

static bool heapam_index_plain_batch_getnext_slot(IndexScanDesc scan,
												  ScanDirection direction,
												  TupleTableSlot *slot);
static bool heapam_index_only_batch_getnext_slot(IndexScanDesc scan,
												 ScanDirection direction,
												 TupleTableSlot *slot);
static bool heapam_index_plain_tuple_getnext_slot(IndexScanDesc scan,
												  ScanDirection direction,
												  TupleTableSlot *slot);
static bool heapam_index_only_tuple_getnext_slot(IndexScanDesc scan,
												 ScanDirection direction,
												 TupleTableSlot *slot);
static pg_always_inline bool heapam_index_getnext_slot(IndexScanDesc scan,
													   ScanDirection direction,
													   TupleTableSlot *slot,
													   bool index_only,
													   bool amgetbatch);
static pg_always_inline bool heapam_index_heap_fetch(IndexScanDesc scan,
													 IndexScanHeapData *hscan,
													 TupleTableSlot *slot,
													 bool index_only,
													 bool amgetbatch);
static pg_noinline bool heapam_index_only_heap_fetch(IndexScanDesc scan,
													 bool amgetbatch);
static pg_noinline void heapam_index_kill_item(IndexScanDesc scan,
											   bool amgetbatch);
static inline bool heapam_index_visited_pages_exceeded(IndexScanDesc scan);
static pg_always_inline bool heapam_index_getnext_scanbatch(IndexScanDesc scan,
															ScanDirection direction,
															bool *all_visible);
static inline void heapam_index_set_scanpos_tid(IndexScanDesc scan,
												IndexScanHeapData *hscan,
												ScanDirection direction,
												IndexScanBatch scanBatch,
												BatchRingItemPos *scanPos,
												bool *all_visible);
static pg_noinline void heapam_index_batch_pos_visibility(IndexScanDesc scan,
														  ScanDirection direction,
														  IndexScanBatch batch,
														  HeapBatchData *hbatch,
														  BatchRingItemPos *pos);
static pg_noinline void heapam_index_dirchange_reset(IndexScanDesc scan,
													 IndexScanHeapData *hscan,
													 ScanDirection direction);
static pg_noinline void heapam_index_start_prefetching(IndexScanDesc scan,
													   IndexScanHeapData *hscan);
static BlockNumber heapam_index_prefetch_next_block(ReadStream *stream,
													void *callback_private_data,
													void *per_buffer_data);

/*
 * Simple, single-shot TID lookup for constraint enforcement code (unique
 * checks and similar).  This is just a heap_hot_search_buffer wrapper.
 *
 * This isn't actually related to index scans, but keeping it near
 * heap_hot_search_buffer can help the compiler generate better code.
 */
bool
heapam_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot,
				 bool *all_dead)
{
	HeapTupleData heapTuple;
	Buffer		buf;
	bool		found;

	buf = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	found = heap_hot_search_buffer(tid, rel, buf, snapshot, &heapTuple,
								   all_dead, true);
	UnlockReleaseBuffer(buf);

	return found;
}

/* ------------------------------------------------------------------------
 * Index Scan Callbacks for heap AM
 * ------------------------------------------------------------------------
 */

void
heapam_index_scan_begin(IndexScanDesc scan, uint32 flags)
{
	IndexScanHeapData *hscan = palloc0_object(IndexScanHeapData);

	/* table AM opaque area size is set below for index-only scans */
	scan->batch_table_opaque_size = 0;

	/* Current heap block state */
	Assert(hscan->xs_cbuf == InvalidBuffer);
	hscan->xs_blk = InvalidBlockNumber;

	/* VM related state */
	Assert(hscan->xs_vmbuffer == InvalidBuffer);
	hscan->xs_vm_items = 1;

	/* Remember if scan is read-only */
	hscan->xs_readonly = (flags & SO_HINT_REL_READ_ONLY) != 0;

	/* xs_lastinblock optimization state */
	Assert(!hscan->xs_lastinblock);

	/* Read stream state (other fields initialized by callback) */
	Assert(hscan->xs_read_stream_dir == NoMovementScanDirection);
	Assert(hscan->xs_read_stream == NULL);

	/* Resolve which xs_getnext_slot implementation to use for this scan */
	if (scan->indexRelation->rd_indam->amgetbatch != NULL)
	{
		/* amgetbatch index AM */
		Assert(scan->maxitemsbatch > 0);

		if (scan->xs_want_itup)
		{
			scan->xs_getnext_slot = heapam_index_only_batch_getnext_slot;

			/*
			 * Index-only scans cache visibility info in a HeapBatchData per
			 * batch: a fixed-size header followed by a per-item batchvis[]
			 * array (one bool per batch item)
			 */
			scan->batch_table_opaque_size = offsetof(HeapBatchData, batchvis) +
				sizeof(bool) * scan->maxitemsbatch;
		}
		else
			scan->xs_getnext_slot = heapam_index_plain_batch_getnext_slot;

		/* Set up scan's batch ring buffer */
		tableam_util_batchscan_init(scan);

		/*
		 * We can only safely prefetch during scans where we're able to
		 * unguard (unpin) each batch's buffers right away (MVCC scans).  We
		 * are not prepared to sensibly limit the total number of buffer pins
		 * held.  The read stream handles all pin resource management for us,
		 * and knows nothing about pins held on index pages/within batches.
		 * (It's also convenient for enable_indexscan_prefetch to gate here.)
		 */
		hscan->xs_prefetching_safe = scan->MVCCScan && enable_indexscan_prefetch;
	}
	else
	{
		/* amgettuple index AM */
		if (scan->xs_want_itup)
			scan->xs_getnext_slot = heapam_index_only_tuple_getnext_slot;
		else
			scan->xs_getnext_slot = heapam_index_plain_tuple_getnext_slot;

		/* Prefetching isn't supported during amgettuple scans */
		hscan->xs_prefetching_safe = false;
	}

	/* Expose heapam's private scan state through the scan's opaque pointer */
	scan->xs_table_opaque = hscan;
}

/*
 * Initialize the heap table AM's per-batch opaque area
 */
void
heapam_index_scan_batch_init(IndexScanDesc scan, IndexScanBatch batch)
{
	HeapBatchData *hbatch;

	/*
	 * The core code should only call here during index-only scans.  Plain
	 * scans don't request a HeapBatchData area at all.
	 */
	Assert(scan->xs_want_itup && scan->usebatchring);

	/* Resetting the valid range makes it safe to use/recycle the batch */
	hbatch = index_scan_batch_table_area(scan, batch);
	hbatch->firstVisSet = INT_MAX;
	hbatch->lastVisSet = -1;
}

/*
 * Reset table AM index scan state in preparation for a rescan
 */
void
heapam_index_scan_rescan(IndexScanDesc scan)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;

	/* Rescans should avoid an excessive number of VM lookups */
	hscan->xs_vm_items = 1;

	/* Heap fetches from the last rescan don't count towards this limit */
	hscan->xs_blkswitch_count = 0;

	/* Defensively do an unconditional read stream direction reset */
	hscan->xs_read_stream_dir = NoMovementScanDirection;

	if (hscan->xs_read_stream)
	{
		hscan->xs_paused = false;
		read_stream_reset(hscan->xs_read_stream);
	}

	/* Reset batch ring buffer state */
	if (scan->usebatchring)
		tableam_util_batchscan_reset(scan, false);

	/*
	 * Deliberately avoid dropping pins now held in xs_cbuf and xs_vmbuffer.
	 * This saves cycles during certain tight nested loop joins (it can avoid
	 * repeated pinning and unpinning of the same buffer across rescans).
	 */
}

void
heapam_index_scan_end(IndexScanDesc scan)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;

	/* drop pin if there's a pinned heap page */
	if (BufferIsValid(hscan->xs_cbuf))
		ReleaseBuffer(hscan->xs_cbuf);

	/* drop pin if there's a pinned visibility map page */
	if (BufferIsValid(hscan->xs_vmbuffer))
		ReleaseBuffer(hscan->xs_vmbuffer);

	if (hscan->xs_read_stream)
		read_stream_end(hscan->xs_read_stream);

	/* Free all batch related resources */
	if (scan->usebatchring)
		tableam_util_batchscan_end(scan);

	pfree(hscan);
}

/*
 * Save batch ring buffer's current scanPos as its markPos
 */
void
heapam_index_scan_markpos(IndexScanDesc scan)
{
	Assert(scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amcanmarkpos);

	tableam_util_batchscan_mark_pos(scan);
}

/*
 * Restore batch ring buffer's markPos into its scanPos
 */
void
heapam_index_scan_restrpos(IndexScanDesc scan)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;

	Assert(scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amcanmarkpos);

	if (hscan->xs_read_stream)
	{
		hscan->xs_paused = false;
		read_stream_reset(hscan->xs_read_stream);
	}

	tableam_util_batchscan_restore_pos(scan);
}

/*
 *	heap_hot_search_buffer	- search HOT chain for tuple satisfying snapshot
 *
 * On entry, *tid is the TID of a tuple (either a simple tuple, or the root
 * of a HOT chain), and buffer is the buffer holding this tuple.  We search
 * for the first chain member satisfying the given snapshot.  If one is
 * found, we update *tid to reference that tuple's offset number, and
 * return true.  If no match, return false without modifying *tid.
 *
 * heapTuple is a caller-supplied buffer.  When a match is found, we return
 * the tuple here, in addition to updating *tid.  If no match is found, the
 * contents of this buffer on return are undefined.
 *
 * If all_dead is not NULL, we check non-visible tuples to see if they are
 * globally dead; *all_dead is set true if all members of the HOT chain
 * are vacuumable, false if not.
 *
 * Unlike heap_fetch, the caller must already have pin and (at least) share
 * lock on the buffer; it is still pinned/locked at exit.
 */
bool
heap_hot_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
					   Snapshot snapshot, HeapTuple heapTuple,
					   bool *all_dead, bool first_call)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prev_xmax = InvalidTransactionId;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		at_chain_start;
	bool		valid;
	bool		skip;
	GlobalVisState *vistest = NULL;

	/* If this is not the first call, previous call returned a (live!) tuple */
	if (all_dead)
		*all_dead = first_call;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);
	at_chain_start = first_call;
	skip = !first_call;

	/* XXX: we should assert that a snapshot is pushed or registered */
	Assert(TransactionIdIsValid(RecentXmin));
	Assert(BufferGetBlockNumber(buffer) == blkno);

	/* Scan through possible multiple members of HOT-chain */
	for (;;)
	{
		ItemId		lp;

		/* check for bogus TID */
		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
			break;

		lp = PageGetItemId(page, offnum);

		/* check for unused, dead, or redirected items */
		if (!ItemIdIsNormal(lp))
		{
			/* We should only see a redirect at start of chain */
			if (ItemIdIsRedirected(lp) && at_chain_start)
			{
				/* Follow the redirect */
				offnum = ItemIdGetRedirect(lp);
				at_chain_start = false;
				continue;
			}
			/* else must be end of chain */
			break;
		}

		/*
		 * Update heapTuple to point to the element of the HOT chain we're
		 * currently investigating. Having t_self set correctly is important
		 * because the SSI checks and the *Satisfies routine for historical
		 * MVCC snapshots need the correct tid to decide about the visibility.
		 */
		heapTuple->t_data = (HeapTupleHeader) PageGetItem(page, lp);
		heapTuple->t_len = ItemIdGetLength(lp);
		heapTuple->t_tableOid = RelationGetRelid(relation);
		ItemPointerSet(&heapTuple->t_self, blkno, offnum);

		/*
		 * Shouldn't see a HEAP_ONLY tuple at chain start.
		 */
		if (at_chain_start && HeapTupleIsHeapOnly(heapTuple))
			break;

		/*
		 * The xmin should match the previous xmax value, else chain is
		 * broken.
		 */
		if (TransactionIdIsValid(prev_xmax) &&
			!TransactionIdEquals(prev_xmax,
								 HeapTupleHeaderGetXmin(heapTuple->t_data)))
			break;

		/*
		 * When first_call is true (and thus, skip is initially false) we'll
		 * return the first tuple we find.  But on later passes, heapTuple
		 * will initially be pointing to the tuple we returned last time.
		 * Returning it again would be incorrect (and would loop forever), so
		 * we skip it and return the next match we find.
		 */
		if (!skip)
		{
			/* If it's visible per the snapshot, we must return it */
			valid = HeapTupleSatisfiesVisibility(heapTuple, snapshot, buffer);
			HeapCheckForSerializableConflictOut(valid, relation, heapTuple,
												buffer, snapshot);

			if (valid)
			{
				ItemPointerSetOffsetNumber(tid, offnum);
				PredicateLockTID(relation, &heapTuple->t_self, snapshot,
								 HeapTupleHeaderGetXmin(heapTuple->t_data));
				if (all_dead)
					*all_dead = false;
				return true;
			}
		}
		skip = false;

		/*
		 * If we can't see it, maybe no one else can either.  At caller
		 * request, check whether all chain members are dead to all
		 * transactions.
		 *
		 * Note: if you change the criterion here for what is "dead", fix the
		 * planner's get_actual_variable_range() function to match.
		 */
		if (all_dead && *all_dead)
		{
			if (!vistest)
				vistest = GlobalVisTestFor(relation);

			if (!HeapTupleIsSurelyDead(heapTuple, vistest))
				*all_dead = false;
		}

		/*
		 * Check to see if HOT chain continues past this tuple; if so fetch
		 * the next offnum and loop around.
		 */
		if (HeapTupleIsHotUpdated(heapTuple))
		{
			Assert(ItemPointerGetBlockNumber(&heapTuple->t_data->t_ctid) ==
				   blkno);
			offnum = ItemPointerGetOffsetNumber(&heapTuple->t_data->t_ctid);
			at_chain_start = false;
			prev_xmax = HeapTupleHeaderGetUpdateXid(heapTuple->t_data);
		}
		else
			break;				/* end of chain */

	}

	return false;
}

/* xs_getnext_slot callback: amgetbatch, plain index scan */
static bool
heapam_index_plain_batch_getnext_slot(IndexScanDesc scan,
									  ScanDirection direction,
									  TupleTableSlot *slot)
{
	Assert(!scan->xs_want_itup && scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amgetbatch != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, false, true);
}

/* xs_getnext_slot callback: amgetbatch, index-only scan */
static bool
heapam_index_only_batch_getnext_slot(IndexScanDesc scan,
									 ScanDirection direction,
									 TupleTableSlot *slot)
{
	Assert(scan->xs_want_itup && scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amgetbatch != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, true, true);
}

/* xs_getnext_slot callback: amgettuple, plain index scan */
static bool
heapam_index_plain_tuple_getnext_slot(IndexScanDesc scan,
									  ScanDirection direction,
									  TupleTableSlot *slot)
{
	Assert(!scan->xs_want_itup && !scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amgettuple != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, false, false);
}

/* xs_getnext_slot callback: amgettuple, index-only scan */
static bool
heapam_index_only_tuple_getnext_slot(IndexScanDesc scan,
									 ScanDirection direction,
									 TupleTableSlot *slot)
{
	Assert(scan->xs_want_itup && !scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amgettuple != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, true, false);
}

/*
 * Common implementation for all four heapam_index_*_getnext_slot variants.
 *
 * The result is true if a tuple satisfying the scan keys and the snapshot was
 * found, false otherwise.  On success the slot is filled: for plain index
 * scans with the heap tuple; for index-only scans with the index data (from
 * xs_itup/xs_hitup, via index_fill_ios_slot).
 *
 * On success, resources (like buffer pins) are likely to be held, and will be
 * dropped by a future call here (or by a later call to heapam_index_scan_end
 * through index_endscan).
 *
 * The index_only and amgetbatch parameters are compile-time constants at each
 * call site, allowing the compiler to specialize the code for each variant.
 */
static pg_always_inline bool
heapam_index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
						  TupleTableSlot *slot, bool index_only,
						  bool amgetbatch)
{
	Assert(TransactionIdIsValid(RecentXmin));
	Assert(index_only || scan->xs_visited_pages_limit == 0);

	for (;;)
	{
		IndexScanHeapData *hscan;
		bool		all_visible;

		/*
		 * Get the next TID from the index, unless we're still working through
		 * a HOT chain (index-only scans never do that, and plain index scans
		 * only do it with a non-MVCC snapshot)
		 */
		Assert(!index_only || !scan->xs_heap_continue);
		if (index_only || likely(!scan->xs_heap_continue))
		{
			if (amgetbatch)
			{
				if (!heapam_index_getnext_scanbatch(scan, direction,
													index_only ? &all_visible :
													NULL))
					return false;
			}
			else				/* amgettuple */
			{
				if (!index_getnext_tid(scan, direction))
					return false;
			}
		}

		/*
		 * The scan's next TID was set in scan->xs_heaptid for us
		 */
		Assert(ItemPointerIsValid(&scan->xs_heaptid));

		hscan = (IndexScanHeapData *) scan->xs_table_opaque;

		if (!index_only)
		{
			/* Plain index scan */
			if (!heapam_index_heap_fetch(scan, hscan, slot, false, amgetbatch))
				continue;		/* no visible tuple, try next index entry */
		}
		else
		{
			/* amgetbatch case already set all_visible; amgettuple did not */
			if (!amgetbatch)
				all_visible =
					VM_ALL_VISIBLE(scan->heapRelation,
								   ItemPointerGetBlockNumber(&scan->xs_heaptid),
								   &hscan->xs_vmbuffer);

			/*
			 * Index-only scan that requires a heap fetch to verify item's
			 * visibility.  This is rare (it takes a not-all-visible page), so
			 * the fetch is kept out of line, which keeps the frame of the
			 * getnext_slot callback small.
			 */
			if (unlikely(!all_visible))
			{
				if (!heapam_index_only_heap_fetch(scan, amgetbatch))
				{
					/* No visible tuple */
					if (heapam_index_visited_pages_exceeded(scan))
						return false;	/* give up for selfuncs.c caller */

					continue;	/* try next index entry */
				}
			}
			else
			{
				/*
				 * Index-only scan with all-visible item.
				 *
				 * We won't access the heap, so we'll need to take a predicate
				 * lock explicitly, as if we had.  For now we do that at page
				 * level.
				 */
				PredicateLockPage(scan->heapRelation,
								  ItemPointerGetBlockNumber(&scan->xs_heaptid),
								  scan->xs_snapshot);
			}

			/*
			 * Fill slot with data returned by the index AM (during plain
			 * scans heapam_index_heap_fetch does this for us instead).
			 *
			 * index_fill_ios_slot is the authority on how to do this, but we
			 * directly handle its common IndexTuple case to avoid paying for
			 * a per-tuple call. XXX Is there a more elegant way to do this?
			 */
			ExecClearTuple(slot);
			if (likely(scan->xs_hitup == NULL && scan->xs_itup != NULL &&
					   scan->xs_name_cstring_attnums == NULL))
			{
				Assert(slot->tts_tupleDescriptor->natts ==
					   scan->xs_itupdesc->natts);

				index_deform_tuple(scan->xs_itup, scan->xs_itupdesc,
								   slot->tts_values, slot->tts_isnull);
				ExecStoreVirtualTuple(slot);
			}
			else
				index_fill_ios_slot(scan, slot);
		}

		return true;
	}

	pg_unreachable();

	return false;
}

/*
 * Get the scan's next heap tuple.
 *
 * Returns true if a visible heap tuple associated with the index TID most
 * recently fetched by our caller in scan->xs_heaptid was found, false if no
 * more matching tuples exist.  (There can be more than one matching tuple
 * because of HOT chains, although when using an MVCC snapshot it should be
 * impossible for more than one such tuple to exist.)
 *
 * Plain index scans have us store the tuple in their slot, and its buffer
 * stays pinned until a later call here (or heapam_index_scan_end) releases
 * it.  Index-only scans just need us to verify tuple visibility, so they pass
 * a NULL slot.
 *
 * When the TID's whole HOT chain turns out to be dead, we arrange for the
 * index AM to kill its entry for the TID before returning false.
 */
static pg_always_inline bool
heapam_index_heap_fetch(IndexScanDesc scan, IndexScanHeapData *hscan,
						TupleTableSlot *slot, bool index_only,
						bool amgetbatch)
{
	Relation	rel = scan->heapRelation;
	ItemPointer tid = &scan->xs_heaptid;
	Snapshot	snapshot = scan->xs_snapshot;
	HeapTupleData tupdata;
	HeapTuple	heapTuple;
	bool		got_heap_tuple;
	bool		all_dead;

	if (!index_only)
	{
		/* Plain index scans have us store fetched tuple in their slot */
		BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

		Assert(TTS_IS_BUFFERTUPLE(slot));
		heapTuple = &bslot->base.tupdata;
	}
	else
	{
		/* Index-only scans only need to verify tuple visibility */
		pg_assume(slot == NULL);
		heapTuple = &tupdata;

		if (scan->instrument)
			scan->instrument->ntabletuplefetches++;
	}

	/* We can skip the buffer-switching logic if we're on the same page. */
	if (hscan->xs_blk != ItemPointerGetBlockNumber(tid))
	{
		Assert(!scan->xs_heap_continue);

		/* Remember this buffer's block number for next time */
		hscan->xs_blk = ItemPointerGetBlockNumber(tid);

		/*
		 * We're switching to a new heap block, so count it
		 */
		hscan->xs_blkswitch_count++;

		/*
		 * Once enough distinct blocks are fetched, start prefetching (though
		 * only if we haven't already).
		 *
		 * The counter can overflow.  This is harmless because we only need it
		 * for this and for xs_visited_pages_limit accounting.
		 */
		if (hscan->xs_read_stream == NULL && hscan->xs_prefetching_safe &&
			hscan->xs_blkswitch_count == INDEX_PREFETCH_BLKSWITCH_THRESHOLD)
			heapam_index_start_prefetching(scan, hscan);

		/*
		 * Drop the xs_blk pin independently held on by slot (if any) now,
		 * before calling ReleaseBuffer.  This avoids expensive calls to
		 * GetPrivateRefCountEntrySlow caused by ExecStoreBufferHeapTuple
		 * failing to hit the backend's cache for the release of the old pin.
		 */
		if (!index_only)
			ExecClearTuple(slot);

		if (BufferIsValid(hscan->xs_cbuf))
			ReleaseBuffer(hscan->xs_cbuf);

		/*
		 * When using a read stream, the stream will already know which block
		 * number comes next (though an assertion will verify a match below)
		 */
		if (hscan->xs_read_stream)
			hscan->xs_cbuf = read_stream_next_buffer(hscan->xs_read_stream, NULL);
		else
			hscan->xs_cbuf = ReadBuffer(rel, hscan->xs_blk);

		/*
		 * Prune page when it is pinned for the first time
		 */
		heap_page_prune_opt(rel, hscan->xs_cbuf, &hscan->xs_vmbuffer,
							hscan->xs_readonly);
	}

	Assert(BufferGetBlockNumber(hscan->xs_cbuf) == hscan->xs_blk);
	Assert(hscan->xs_blk == ItemPointerGetBlockNumber(tid));

	/* Obtain share-lock on the buffer so we can examine visibility */
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_SHARE);
	got_heap_tuple = heap_hot_search_buffer(tid,
											rel,
											hscan->xs_cbuf,
											snapshot,
											heapTuple,
											&all_dead,
											!scan->xs_heap_continue);
	heapTuple->t_self = *tid;
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

	if (got_heap_tuple)
	{
		if (!index_only)
		{
			/*
			 * Only in a non-MVCC snapshot plain scan can more than one member
			 * of the HOT chain be visible
			 */
			scan->xs_heap_continue = !scan->MVCCScan;

			/*
			 * If xs_lastinblock indicates that `tid` is the last TID on the
			 * current heap block, transfer our buffer pin to the slot rather
			 * than having the slot increment the pin count.  This saves a
			 * pair of IncrBufferRefCount and ReleaseBuffer calls, since the
			 * next call here would just release the same pin on xs_cbuf
			 * anyway. (Actually, this is only true if you assume that the
			 * scan will continue in the current direction, but it generally
			 * does.  An incorrect prediction costs us little.)
			 *
			 * We can only safely do this when xs_heap_continue is false,
			 * since otherwise the caller will need xs_cbuf to remain valid
			 * for the next call.
			 */
			if (hscan->xs_lastinblock && !scan->xs_heap_continue)
			{
				ExecStorePinnedBufferHeapTuple(heapTuple, slot, hscan->xs_cbuf);
				hscan->xs_cbuf = InvalidBuffer;
				hscan->xs_blk = InvalidBlockNumber;

				/*
				 * Note: the pin now owned by the slot is expected to be
				 * released on the next call here, via an explicit
				 * ExecClearTuple.  This avoids churn in the backend's private
				 * refcount cache.
				 */
			}
			else
				ExecStoreBufferHeapTuple(heapTuple, slot, hscan->xs_cbuf);

			Assert(slot->tts_tableOid == RelationGetRelid(rel));
		}
		else
		{
			/*
			 * Index-only scans unconditionally assume that the first member
			 * of a HOT chain should be considered visible.  This is the
			 * normal MVCC snapshot behavior, and works well enough for
			 * non-MVCC index-only scans (currently the only core code that
			 * uses a non-MVCC index-only scan is selfuncs.c).
			 */
			scan->xs_heap_continue = false;
		}

		pgstat_count_heap_fetch(scan->indexRelation);
	}
	else
	{
		/* We've reached the end of the HOT chain. */
		scan->xs_heap_continue = false;

		if (unlikely(all_dead))
			heapam_index_kill_item(scan, amgetbatch);
	}

	return got_heap_tuple;
}

/*
 * Out-of-line heapam_index_heap_fetch wrapper for index-only scans.
 *
 * Index-only scans usually avoid heap fetches using the visibility map, so
 * keeping their fetch out of line keeps the frame of their getnext_slot
 * callback small.
 */
static pg_noinline bool
heapam_index_only_heap_fetch(IndexScanDesc scan, bool amgetbatch)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;

	return heapam_index_heap_fetch(scan, hscan, NULL, true, amgetbatch);
}

/*
 * Called when we scanned a whole HOT chain and found only dead tuples:
 * arrange for the index AM to kill its entry for that TID.  We do not do this
 * when in recovery because it may violate MVCC to do so.  See comments in
 * RelationGetIndexScan().
 */
static pg_noinline void
heapam_index_kill_item(IndexScanDesc scan, bool amgetbatch)
{
	if (scan->xactStartedInRecovery)
		return;

	if (amgetbatch)
		tableam_util_scanpos_killitem(scan);
	else						/* amgettuple */
	{
		/*
		 * Tell amgettuple-based index AM to kill its entry for that TID.  The
		 * next index_getnext_tid call will pass that along to the index AM,
		 * before unsetting the flag again.
		 */
		scan->kill_prior_tuple = true;
	}
}

/*
 * Did an index-only scan switch heap pages more times than the caller's
 * visited-pages limit allows?  Only selfuncs.c's index-only scans set a
 * limit; the scan gives up when it's exceeded by a failed heap fetch.
 *
 * Takes scan rather than hscan so that callers need not keep hscan live
 * across their (out-of-line) heap fetch call.
 */
static inline bool
heapam_index_visited_pages_exceeded(IndexScanDesc scan)
{
	IndexScanHeapData *hscan;

	if (likely(scan->xs_visited_pages_limit == 0))
		return false;

	hscan = (IndexScanHeapData *) scan->xs_table_opaque;

	return hscan->xs_blkswitch_count > scan->xs_visited_pages_limit;
}

/*
 * Advance to the next TID from batch ring buffer, moving in the given scan
 * direction.  Returns true when a TID was found (and saved in xs_heaptid),
 * false when there are no more matches in the current scan direction.
 * Also sets *all_visible for item when caller passes a non-NULL arg.
 */
static pg_always_inline bool
heapam_index_getnext_scanbatch(IndexScanDesc scan, ScanDirection direction,
							   bool *all_visible)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	IndexScanBatch scanBatch;

	Assert(all_visible == NULL || scan->xs_want_itup);

	/*
	 * Changing the scan direction mid-scan requires an MVCC snapshot: with
	 * any other snapshot type, more than one member of a HOT chain can be
	 * visible, and resuming a partially-returned chain only works in the
	 * forward direction.  All non-MVCC callers scan in one fixed direction.
	 */
	Assert(scan->MVCCScan ||
		   hscan->xs_read_stream_dir == NoMovementScanDirection ||
		   hscan->xs_read_stream_dir == direction);

	/* Handle resetting the read stream when scan direction changes */
	if (hscan->xs_read_stream_dir == NoMovementScanDirection)
		hscan->xs_read_stream_dir = direction;	/* first call */
	else if (unlikely(hscan->xs_read_stream_dir != direction))
		heapam_index_dirchange_reset(scan, hscan, direction);

	/*
	 * Attempt to increment the position of any existing loaded scanBatch
	 * (always fails on first call here for the scan)
	 */
	if (tableam_util_scanpos_advance(scan, direction, &scanBatch, scanPos))
	{
		/*
		 * Incremented scanPos within existing loaded scanBatch; save the new
		 * position's TID for caller
		 */
		heapam_index_set_scanpos_tid(scan, hscan, direction, scanBatch,
									 scanPos, all_visible);
		return true;
	}

	/* Try to advance scanBatch to the next batch (or get the first batch) */
	scanBatch = tableam_util_fetch_next_batch(scan, direction,
											  scanBatch, scanPos);

	if (!scanBatch)
	{
		/*
		 * We're done; no more batches in the current scan direction.
		 *
		 * Note: if scanPos was ever valid (if amgetbatch ever returned a
		 * batch), it remains valid now.  The current scanPos.item is now
		 * scanBatch.lastItem + 1 (or scanBatch.firstItem - 1, when scanning
		 * backwards).  We are therefore prepared to move the scan in the
		 * opposite direction, within our still-loaded scanBatch.  Similarly,
		 * we can still restore a saved mark in the usual way.
		 */
		return false;
	}

	/*
	 * We have a new scanBatch, but scanPos hasn't been advanced to it just
	 * yet.  Update batchringbuf and scanPos such that the scan can continue
	 * with our new scanBatch.
	 *
	 * This will position scanPos to the start of our new scanBatch.  It will
	 * also remove the old head batch/scanBatch from the batch ring buffer,
	 * and release the underlying batch storage.
	 */
	if (tableam_util_scanpos_nextbatch(scan, direction, scanBatch))
	{
		/* A previously occupied ring buffer slot was freed */
		if (unlikely(hscan->xs_paused))
		{
			/*
			 * heapam_index_prefetch_next_block paused the scan's read stream
			 * due to our running out of batch slots (or it "throttled" the
			 * read stream to avoid reading too far ahead in the index).
			 *
			 * Now that the scanBatch that was current when we paused has been
			 * removed from the batch ring buffer, we must resume prefetching.
			 */
			read_stream_resume(hscan->xs_read_stream);
			hscan->xs_paused = false;
		}
	}

	Assert(!hscan->xs_paused);

	/*
	 * Set scanPos to first item for newly loaded scanBatch; save the new
	 * position's TID for caller
	 */
	heapam_index_set_scanpos_tid(scan, hscan, direction, scanBatch, scanPos,
								 all_visible);
	return true;
}

/*
 * Save the current scanPos/scanBatch item's TID in scan's xs_heaptid.  When
 * all_visible isn't NULL (during an index-only scan), also sets item's
 * visibility status in *all_visible.
 *
 * heapam_index_getnext_scanbatch helper function.
 */
static inline void
heapam_index_set_scanpos_tid(IndexScanDesc scan, IndexScanHeapData *hscan,
							 ScanDirection direction,
							 IndexScanBatch scanBatch,
							 BatchRingItemPos *scanPos,
							 bool *all_visible)
{
	amgettransform_function amgettransform =
		scan->indexRelation->rd_indam->amgettransform;
	HeapBatchData *hbatch;

	/* Set xs_heaptid, which caller (and core executor) will need */
	scan->xs_heaptid = scanBatch->items[scanPos->item].tableTid;

	/*
	 * Let the index AM set this item's per-tuple output.  An AM that provides
	 * amgettransform uses it to set the item's qual recheck flag
	 * (scan->xs_recheck), an ordered scan's ORDER BY distances
	 * (xs_orderbyvals/xs_recheckorderby), and an index-only scan's returnable
	 * tuple (xs_hitup).
	 */
	if (amgettransform != NULL)
		amgettransform(scan, scanBatch, scanPos->item);

	if (all_visible == NULL)
	{
		int			nextItem;
		bool		hasNext;

		/*
		 * Plain index scan.
		 *
		 * Set xs_lastinblock to indicate whether the next item in the current
		 * scan direction is on a different heap block to the current item.
		 * heapam_index_heap_fetch will apply this information about
		 * scanPos.item's tableTID before we return to the core executor.
		 *
		 * Note: We don't set this for index-only scans because it doesn't
		 * seem worth the trouble of reasoning about all-visible items.
		 *
		 * Note: We deliberately don't consider the batch after scanBatch,
		 * because doing so would add complexity for little benefit.  It's
		 * okay if xs_lastinblock is spuriously set to false.
		 */
		Assert(!scan->xs_want_itup);
		if (ScanDirectionIsForward(direction))
		{
			nextItem = scanPos->item + 1;
			hasNext = (nextItem <= scanBatch->lastItem);
		}
		else
		{
			nextItem = scanPos->item - 1;
			hasNext = (nextItem >= scanBatch->firstItem);
		}

		hscan->xs_lastinblock = hasNext &&
			ItemPointerGetBlockNumber(&scanBatch->items[nextItem].tableTid) !=
			ItemPointerGetBlockNumber(&scan->xs_heaptid);

		return;
	}

	/* Index-only scan */
	Assert(scan->xs_want_itup);

	/*
	 * Unless the index AM already produced the returnable tuple via
	 * amgettransform above (in xs_hitup), set the original index tuple that
	 * amgetbatch stored in currTuples in xs_itup.
	 */
	if (amgettransform == NULL)
		scan->xs_itup = (IndexTuple) (scanBatch->currTuples +
									  scanBatch->items[scanPos->item].tupleOffset);

	/*
	 * Set visibility info for the current scanPos item (plus possibly some
	 * additional items in the current scan direction) as needed
	 */
	hbatch = index_scan_batch_table_area(scan, scanBatch);
	if (!HEAP_BATCH_VIS_CACHED(hbatch, scanPos->item))
		heapam_index_batch_pos_visibility(scan, direction,
										  scanBatch, hbatch, scanPos);

	/* Finally, set all_visible for caller */
	*all_visible = hbatch->batchvis[scanPos->item];
}

/*
 * Obtain visibility information for a TID from caller's batch.
 *
 * Called during amgetbatch index-only scans.  We always make sure that the
 * visibility of caller's item (an offset into caller's batch->items[] array)
 * has been set in its batch's batchvis[].  We might also set visibility info
 * for other items from caller's batch more proactively when that makes sense.
 *
 * Every item has its batchvis[] entry set exactly once (or never).  We make
 * sure that the scan has a fixed picture of which blocks it'll need to fetch
 * in the near future.  If caller's position's item (or other nearby items)
 * already have a valid batchvis[] entry, we must avoid clobbering that entry.
 *
 * We keep two competing considerations in balance when determining whether to
 * check additional items: the need to keep the cost of visibility map access
 * under control when most items will never be returned by the scan anyway
 * (important for inner index scans of anti-joins and semi-joins), and the
 * need to unguard batches promptly.
 *
 * In no event will an MVCC scan be allowed to guard more than one batch at a
 * time.  The primary reason for this restriction is to avoid unintended
 * interactions with the read stream, which has its own strategy for keeping
 * the number of pins held by the backend under control.  (Unguarding via the
 * amunguardbatch callback often means releasing a buffer pin on an index
 * page, which counts against the same backend pin limit.)
 *
 * Once we've resolved visibility for all items in a batch, we can safely
 * unguard it by calling amunguardbatch.  This is safe with respect to
 * concurrent VACUUM because the batch's guard (typically a buffer pin on the
 * originating index page) blocks VACUUM from acquiring a conflicting cleanup
 * lock on that page.  Copying the relevant visibility map data into our local
 * cache suffices to prevent unsafe concurrent TID recycling: if any of these
 * TIDs point to dead heap tuples, VACUUM cannot possibly return from
 * ambulkdelete and mark the pointed-to heap pages as all-visible.  VACUUM
 * _can_ do so once the batch is unguarded, but that's okay; we'll be working
 * off of cached visibility info that indicates that the dead TIDs are NOT
 * all-visible.
 *
 * What about the opposite case, where a page was all-visible when we cached
 * the VM bits but tuples on it are deleted afterwards?  That is safe too: any
 * tuple that was visible to all when we read the VM must also be visible to
 * our MVCC snapshot, so it is correct to skip the heap fetch for those TIDs.
 */
static pg_noinline void
heapam_index_batch_pos_visibility(IndexScanDesc scan, ScanDirection direction,
								  IndexScanBatch batch, HeapBatchData *hbatch,
								  BatchRingItemPos *pos)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;
	int			posItem = pos->item;
	int			loItem,
				hiItem;
	BlockNumber curvmheapblkno = InvalidBlockNumber;
	bool		curvmheapallvis = false;

	Assert(hbatch == index_scan_batch_table_area(scan, batch));

	/*
	 * The batch must still be guarded whenever we're called.
	 *
	 * amunguardbatch can't be called until we've already set _every_ batch
	 * item's batchvis[] status, but if we've already done so for this batch
	 * then it shouldn't ever get passed to us again by some subsequent call.
	 * (This relies on index-only scans always being !batchImmediateUnguard.)
	 */
	Assert(batch->isGuarded && !scan->batchImmediateUnguard);

	/*
	 * Set visibility info for a range of items, in scan order.
	 *
	 * Note: visibilitymap_get_status does not lock the visibility map buffer,
	 * so the result could be slightly stale.  See the "Memory ordering
	 * effects" discussion above visibilitymap_get_status for an explanation
	 * of why this is okay.
	 */
	if (ScanDirectionIsForward(direction))
	{
		int			lastSetItem = Min(batch->lastItem,
									  posItem + hscan->xs_vm_items - 1);

		for (int setItem = posItem; setItem <= lastSetItem; setItem++)
		{
			ItemPointer tid = &batch->items[setItem].tableTid;
			BlockNumber heapblkno = ItemPointerGetBlockNumber(tid);

			/* Must never overwrite any batch item's cached visibility info */
			if (HEAP_BATCH_VIS_CACHED(hbatch, setItem))
				continue;

			if (heapblkno != curvmheapblkno)
			{
				curvmheapallvis = VM_ALL_VISIBLE(scan->heapRelation, heapblkno,
												 &hscan->xs_vmbuffer);
				curvmheapblkno = heapblkno;
			}

			hbatch->batchvis[setItem] = curvmheapallvis;
		}

		/* We just cached visibility for items [posItem, lastSetItem] */
		loItem = posItem;
		hiItem = lastSetItem;
	}
	else
	{
		int			lastSetItem = Max(batch->firstItem,
									  posItem - hscan->xs_vm_items + 1);

		for (int setItem = posItem; setItem >= lastSetItem; setItem--)
		{
			ItemPointer tid = &batch->items[setItem].tableTid;
			BlockNumber heapblkno = ItemPointerGetBlockNumber(tid);

			/* Must never overwrite any batch item's cached visibility info */
			if (HEAP_BATCH_VIS_CACHED(hbatch, setItem))
				continue;

			if (heapblkno != curvmheapblkno)
			{
				curvmheapallvis = VM_ALL_VISIBLE(scan->heapRelation, heapblkno,
												 &hscan->xs_vmbuffer);
				curvmheapblkno = heapblkno;
			}

			hbatch->batchvis[setItem] = curvmheapallvis;
		}

		/* We just cached visibility for items [lastSetItem, posItem] */
		loItem = lastSetItem;
		hiItem = posItem;
	}

	/*
	 * Extend the batch's valid range to cover the items we just cached.  The
	 * set of cached items is always contiguous, because the scan visits items
	 * in order and only ever extends the range from firstItem forwards or
	 * from lastItem backwards.
	 */
	Assert(hbatch->firstVisSet > hbatch->lastVisSet ||	/* still contiguous? */
		   (loItem <= hbatch->lastVisSet + 1 &&
			hiItem >= hbatch->firstVisSet - 1));
	hbatch->firstVisSet = Min(hbatch->firstVisSet, loItem);
	hbatch->lastVisSet = Max(hbatch->lastVisSet, hiItem);

	/*
	 * It's safe to unguard the batch (via amunguardbatch) as soon as we've
	 * resolved the visibility status of all of its items (unless this is a
	 * non-MVCC scan)
	 */
	if (hbatch->firstVisSet <= batch->firstItem &&
		hbatch->lastVisSet >= batch->lastItem)
	{
		Assert(hbatch->firstVisSet == batch->firstItem &&
			   hbatch->lastVisSet == batch->lastItem);

		/*
		 * Note: nodeIndexonlyscan.c only supports MVCC snapshots, but we
		 * still cope with index-only scan callers with other snapshot types.
		 * This is certainly not unexpected; selfuncs.c performs index-only
		 * scans that use SnapshotNonVacuumable.
		 */
		if (scan->MVCCScan)
			tableam_util_unguard_batch(scan, batch);
	}

	/*
	 * Else check visibility for twice as many items next time, or all items.
	 * We check all items in one go once we're passed the scan's first batch.
	 */
	else if (hscan->xs_vm_items < (batch->lastItem - batch->firstItem))
		hscan->xs_vm_items *= 2;
	else
		hscan->xs_vm_items = scan->maxitemsbatch;
}

/*
 * Handle a change in index scan direction (at the tuple granularity).
 *
 * Resets the read stream, since we can't rely on scanPos continuing to agree
 * with the blocks that read stream already consumed using prefetchPos.
 *
 * Note: iff the scan _continues_ in this new direction, and actually steps
 * off scanBatch to an earlier index page, tableam_util_fetch_next_batch will
 * deal with it.  But that might never happen; the scan might yet change
 * direction again (or just end before returning more items).
 */
static pg_noinline void
heapam_index_dirchange_reset(IndexScanDesc scan, IndexScanHeapData *hscan,
							 ScanDirection direction)
{
	/* Reset read stream state */
	scan->batchringbuf.prefetchPos.valid = false;
	hscan->xs_paused = false;
	hscan->xs_read_stream_dir = direction;
	hscan->xs_blkswitch_count = 0;

	/* Reset read stream itself */
	if (hscan->xs_read_stream)
		read_stream_reset(hscan->xs_read_stream);
}

/*
 * Start a read stream for heap block prefetching during an index scan
 */
static pg_noinline void
heapam_index_start_prefetching(IndexScanDesc scan, IndexScanHeapData *hscan)
{
	Assert(hscan->xs_blk != InvalidBlockNumber);
	Assert(!hscan->xs_read_stream);
	Assert(!scan->batchringbuf.prefetchPos.valid);

	hscan->xs_read_stream =
		read_stream_begin_relation(READ_STREAM_DEFAULT, NULL,
								   scan->heapRelation, MAIN_FORKNUM,
								   heapam_index_prefetch_next_block, scan, 0);

	if (scan->instrument)
		read_stream_enable_stats(hscan->xs_read_stream,
								 &scan->instrument->io);
}

/*
 * Return the next block to the read stream when performing index prefetching.
 *
 * The initial batch is always loaded by heapam_index_getnext_scanbatch.  We
 * don't get called until the first read_stream_next_buffer call, when a heap
 * block is requested from the scan's stream for the first time.
 *
 * The position of the read stream is stored in prefetchPos, which typically
 * stays ahead of scanPos (the scan's read position).  When we return, we
 * always leave scanPos <= prefetchPos.
 */
static BlockNumber
heapam_index_prefetch_next_block(ReadStream *stream,
								 void *callback_private_data,
								 void *per_buffer_data)
{
	IndexScanDesc scan = (IndexScanDesc) callback_private_data;
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;
	BatchRingBuffer *batchringbuf = &scan->batchringbuf;
	BatchRingItemPos *scanPos PG_USED_FOR_ASSERTS_ONLY = &batchringbuf->scanPos;
	BatchRingItemPos *prefetchPos = &batchringbuf->prefetchPos;
	ScanDirection direction = hscan->xs_read_stream_dir;
	IndexScanBatch prefetchBatch;
	HeapBatchData *hbatch = NULL;
	int			nbatchadvances_this_call = 0;

	Assert(!hscan->xs_paused && hscan->xs_prefetching_safe);
	Assert(direction != NoMovementScanDirection);

	/*
	 * Handle initialization of prefetchPos: set it from the scan's current
	 * scanPos when it isn't already (validly) ahead of scanPos.  This is
	 * required during the first call here for the scan (and in certain edge
	 * cases).  See tableam_util_prefetchpos_catchup for full details.
	 */
	if (tableam_util_prefetchpos_catchup(scan, direction))
	{
		BatchMatchingItem *item;

		/* prefetchPos has been initialized from scanPos for us */
		prefetchBatch = index_scan_batch(scan, prefetchPos->batch);

		/*
		 * We must avoid keeping any batch guarded for more than an instant,
		 * to avoid undesirable interactions with the scan's read stream. See
		 * comment and assertion at the top of the loop below.
		 */
		if (scan->xs_want_itup)
		{
			BatchRingItemPos startPos;

			/*
			 * Index-only scan batches aren't unguarded immediately.  Deal
			 * with that.
			 */
			hbatch = index_scan_batch_table_area(scan, prefetchBatch);

			/*
			 * The requested item can't be all-visible according to its
			 * batch's cached visibility information; if it were, we'd never
			 * have been called in the first place
			 */
			Assert(HEAP_BATCH_VIS_CACHED(hbatch, prefetchPos->item) &&
				   !hbatch->batchvis[prefetchPos->item]);

			/*
			 * Load any visibility info not already set through scanBatch, so
			 * that scanBatch/prefetchBatch is unguarded right away
			 */
			startPos = *prefetchPos;
			startPos.item = ScanDirectionIsForward(direction) ?
				prefetchBatch->firstItem : prefetchBatch->lastItem;
			hscan->xs_vm_items = scan->maxitemsbatch;	/* must unguard */
			if (prefetchBatch->isGuarded)
				heapam_index_batch_pos_visibility(scan, direction,
												  prefetchBatch, hbatch,
												  &startPos);

			/*
			 * Later calls to heapam_index_batch_pos_visibility will always
			 * unguard batches right away, which we rely on in the loop below
			 */
		}

		Assert(!prefetchBatch->isGuarded);

		item = &prefetchBatch->items[prefetchPos->item];
		hscan->xs_prefetch_block = ItemPointerGetBlockNumber(&item->tableTid);

		/*
		 * Special case: when we return, prefetchPos won't be ahead of scanPos
		 * (it'll just be equal to scanPos).  We're merely fetching through a
		 * read stream; true prefetching hasn't really started yet.
		 */
		Assert(index_scan_pos_cmp(scanPos, prefetchPos, direction) == 0);

		return hscan->xs_prefetch_block;
	}

	/*
	 * We're picking up prefetching from where the last call here left off
	 */
	Assert(index_scan_pos_cmp(scanPos, prefetchPos, direction) <= 0);
	prefetchBatch = index_scan_batch(scan, prefetchPos->batch);
	if (scan->xs_want_itup)
		hbatch = index_scan_batch_table_area(scan, prefetchBatch);

	/*
	 * Assert in passing that xs_prefetch_block matches the last item we
	 * returned.
	 *
	 * Note: we don't actually need a xs_prefetch_block field at all; we could
	 * just take the last block we returned from prefetchPos directly instead.
	 * But maintaining xs_prefetch_block explicitly is slightly more robust.
	 * It gives us a way to make sure that the last call here left prefetchPos
	 * in a consistent state (e.g., when the read stream had to be paused).
	 */
#ifdef USE_ASSERT_CHECKING
	{
		BatchMatchingItem *lastitem = &prefetchBatch->items[prefetchPos->item];
		BlockNumber last_block = ItemPointerGetBlockNumber(&lastitem->tableTid);

		/*
		 * Note: when a previous call paused the read stream, prefetchPos
		 * might point to an item whose TID doesn't match last_block.  This
		 * can only happen when the item was never returned due to it being
		 * all-visible.
		 */
		Assert(last_block == hscan->xs_prefetch_block ||
			   (hbatch && HEAP_BATCH_VIS_CACHED(hbatch, prefetchPos->item) &&
				hbatch->batchvis[prefetchPos->item]));
	}
#endif

	for (;;)
	{
		BatchMatchingItem *item;
		BlockNumber prefetch_block;
		bool		throttle;

		/*
		 * We never call amgetbatch without immediately unguarding the batch
		 * once prefetching begins.  That way index AMs won't hold onto any
		 * "extra" index page pins needed as TID recycling interlock guards.
		 *
		 * This is defensive.  The read stream tries to be careful about not
		 * pinning too many buffers, and that's harder to do reliably if there
		 * are variable numbers of pins taken without such care.
		 */
		Assert(!prefetchBatch->isGuarded);

		/*
		 * Before advancing prefetchPos, consider if read stream's current
		 * call here already advanced prefetchBatch.  This is possible during
		 * index-only scans with long runs of batches containing only items
		 * that are all-visible (it's also possible during plain index scans
		 * with unusual batch layouts, though that's much less common).
		 *
		 * When we detect this condition, we forcibly throttle prefetching,
		 * which pauses the read stream.  That'll give scanPos the opportunity
		 * to return the next item to the scan.  We impose a ceiling on how
		 * far prefetchBatch can get ahead of scanBatch without our producing
		 * even one additional heap block for the read stream to prefetch.
		 */
		throttle = nbatchadvances_this_call >= INDEX_PREFETCH_MAX_BATCH_ADVANCES;

		/* Increment prefetchPos to determine the next item to prefetch */
		switch (tableam_util_prefetchpos_advance(scan, direction,
												 &prefetchBatch, prefetchPos,
												 throttle))
		{
			case BATCH_POS_ADVANCED:
				/* Advanced to next item in current/previous prefetchBatch */
				break;
			case BATCH_POS_BATCH_ADVANCED:
				/* Advanced to first item in new prefetchBatch */
				nbatchadvances_this_call++;
				if (hbatch)
				{
					/*
					 * Extra heapam-specific step: bulk-load visibility info
					 * up front to unguard batch immediately
					 */
					Assert(scan->xs_want_itup);

					hbatch = index_scan_batch_table_area(scan, prefetchBatch);

					Assert(hscan->xs_vm_items == scan->maxitemsbatch);
					if (prefetchBatch->isGuarded)
						heapam_index_batch_pos_visibility(scan, direction,
														  prefetchBatch,
														  hbatch, prefetchPos);
				}
				break;
			case BATCH_POS_DONE:
				/* No more batches in this scan direction */
				return InvalidBlockNumber;
			case BATCH_POS_RING_FULL:

				/*
				 * Edge case: Ran out of items from prefetchBatch, but can't
				 * advance to the scan's next batch right now (all available
				 * batchringbuf batch slots are currently in use).  This also
				 * happens when we deliberately throttled prefetching.
				 *
				 * Deal with this by momentarily pausing the read stream.
				 * heapam_index_getnext_scanbatch will resume the read stream
				 * later, though only after scanPos has consumed all remaining
				 * items from scanBatch (at which point the current head batch
				 * will be freed, making a slot available for reuse).
				 */
				hscan->xs_paused = true;
				return read_stream_pause(stream);
		}

		/*
		 * prefetchPos now points to the next item whose TID's heap block
		 * number might need to be prefetched.
		 *
		 * scanPos must be < prefetchPos when we return from this loop path.
		 */
		Assert(index_scan_pos_cmp(scanPos, prefetchPos, direction) < 0);

		if (hbatch)
		{
			Assert(scan->xs_want_itup);
			Assert(HEAP_BATCH_VIS_CACHED(hbatch, prefetchPos->item));

			if (hbatch->batchvis[prefetchPos->item])
			{
				/* item is known to be all-visible -- don't prefetch */
				continue;
			}
		}

		item = &prefetchBatch->items[prefetchPos->item];
		prefetch_block = ItemPointerGetBlockNumber(&item->tableTid);

		if (prefetch_block == hscan->xs_prefetch_block)
		{
			/*
			 * prefetch_block matches the last prefetchPos item's TID's heap
			 * block number; we must not return the same prefetch_block twice
			 * (twice in succession)
			 */
			continue;
		}

		/* We have a new heap block number to return to read stream */
		hscan->xs_prefetch_block = prefetch_block;
		return prefetch_block;
	}

	pg_unreachable();

	return InvalidBlockNumber;
}
