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
#include "storage/predicate.h"
#include "utils/pgstat_internal.h"


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
static pg_always_inline bool heapam_index_heap_fetch(IndexScanDesc scan, IndexScanHeapData *hscan,
													 TupleTableSlot *slot, bool index_only,
													 bool *heap_continue, bool *all_dead);
static pg_noinline void heapam_index_kill_item(IndexScanDesc scan,
											   bool amgetbatch);
static pg_always_inline ItemPointer heapam_index_getnext_scanbatch_pos(IndexScanDesc scan,
																	   IndexScanHeapData *hscan,
																	   ScanDirection direction,
																	   bool *all_visible);
static inline ItemPointer heapam_index_return_scanpos_tid(IndexScanDesc scan,
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

/*
 * Simple, single-shot TID lookup for constraint enforcement code (unique
 * checks and similar).  This is essentially just a heap_hot_search_buffer
 * wrapper.
 *
 * This isn't actually related to index scans, but keeping it near
 * heap_hot_search_buffer can help the compiler generate better code.
 */
bool
heapam_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot,
				 TupleTableSlot *slot, bool *all_dead)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	Buffer		buf;
	bool		found;

	Assert(TTS_IS_BUFFERTUPLE(slot));

	buf = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	found = heap_hot_search_buffer(tid, rel, buf, snapshot,
								   &bslot->base.tupdata, all_dead, true);
	bslot->base.tupdata.t_self = *tid;
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (found)
	{
		ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot, buf);
		Assert(slot->tts_tableOid == RelationGetRelid(rel));
	}
	else
		ReleaseBuffer(buf);

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
	}
	else
	{
		/* amgettuple index AM */
		if (scan->xs_want_itup)
			scan->xs_getnext_slot = heapam_index_only_tuple_getnext_slot;
		else
			scan->xs_getnext_slot = heapam_index_plain_tuple_getnext_slot;
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

	/* Heap fetches from the last rescan don't count towards this limit  */
	hscan->xs_blkswitch_count = 0;

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
	Assert(scan->usebatchring);
	Assert(scan->indexRelation->rd_indam->amcanmarkpos);

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
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;
	bool	   *heap_continue = &scan->xs_heap_continue;
	bool		all_visible = false;
	ItemPointer tid = NULL;

	Assert(TransactionIdIsValid(RecentXmin));
	Assert(index_only || scan->xs_visited_pages_limit == 0);

	for (;;)
	{
		if (!*heap_continue)
		{
			/* Get the next TID from the index */
			if (amgetbatch)
				tid = heapam_index_getnext_scanbatch_pos(scan, hscan,
														 direction,
														 index_only ?
														 &all_visible : NULL);
			else
				tid = tableam_util_fetch_next_tuple_tid(scan, direction);

			/* If we're out of index entries, we're done */
			if (tid == NULL)
				break;

			/* For non-batch index-only scans, check the visibility map */
			if (index_only && !amgetbatch)
				all_visible = VM_ALL_VISIBLE(scan->heapRelation,
											 ItemPointerGetBlockNumber(tid),
											 &hscan->xs_vmbuffer);
		}

		Assert(ItemPointerIsValid(&scan->xs_heaptid));

		if (!index_only || !all_visible)
		{
			bool		all_dead;

			/*
			 * Plain index scan, or index-only scan that requires a heap fetch
			 * to verify item's visibility
			 */
			if (index_only && scan->instrument)
				scan->instrument->ntabletuplefetches++;

			if (!heapam_index_heap_fetch(scan, hscan,
										 index_only ? NULL : slot,
										 index_only, heap_continue,
										 &all_dead))
			{
				/* No visible tuple */
				if (unlikely(all_dead))
					heapam_index_kill_item(scan, amgetbatch);

				/*
				 * If caller set a visited-pages limit (only selfuncs.c's
				 * index-only scans do this), give up once the scan has
				 * switched heap pages too many times
				 */
				if (index_only && unlikely(scan->xs_visited_pages_limit > 0) &&
					hscan->xs_blkswitch_count > scan->xs_visited_pages_limit)
					return false;	/* give up */

				continue;		/* try next index entry */
			}

			/* Found a visible tuple, so its HOT chain can't be all-dead */
			Assert(!all_dead);
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
							  ItemPointerGetBlockNumber(tid),
							  scan->xs_snapshot);
		}

		/*
		 * Found a tuple to return.
		 *
		 * Index-only scans fill caller's slot from the index data the AM
		 * returned (in scan->xs_itup or xs_hitup); plain index scans already
		 * had heapam_index_heap_fetch store the heap tuple (just after it
		 * reset the scan's slot, which also needs separate handling here).
		 */
		if (index_only)
		{
			ExecClearTuple(slot);
			index_fill_ios_slot(scan, slot);
		}

		return true;
	}

	return false;
}

/*
 * Get the scan's next heap tuple.
 *
 * The result is a visible heap tuple associated with the index TID most
 * recently fetched by our caller in scan->xs_heaptid, or NULL if no more
 * matching tuples exist.  (There can be more than one matching tuple because
 * of HOT chains, although when using an MVCC snapshot it should be impossible
 * for more than one such tuple to exist.)
 *
 * On success, the buffer containing the heap tup is pinned.  The pin must be
 * dropped elsewhere.
 */
static pg_always_inline bool
heapam_index_heap_fetch(IndexScanDesc scan, IndexScanHeapData *hscan,
						TupleTableSlot *slot, bool index_only,
						bool *heap_continue, bool *all_dead)
{
	Relation	rel = scan->heapRelation;
	ItemPointer tid = &scan->xs_heaptid;
	Snapshot	snapshot = scan->xs_snapshot;
	HeapTupleData idxtupdata;
	HeapTuple	heapTuple;
	bool		got_heap_tuple;

	*all_dead = false;

	if (!index_only)
	{
		/* Plain index scans have us store fetched tuple in their slot */
		BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;

		Assert(TTS_IS_BUFFERTUPLE(slot));
		heapTuple = &bslot->base.tupdata;
	}
	else
	{
		/*
		 * Index-only scans just need us to verify tuple visibility, so don't
		 * pass us a slot
		 */
		pg_assume(slot == NULL);
		heapTuple = &idxtupdata;
	}

	/* We can skip the buffer-switching logic if we're on the same page. */
	if (hscan->xs_blk != ItemPointerGetBlockNumber(tid))
	{
		Assert(!*heap_continue);

		/* Remember this buffer's block number for next time */
		hscan->xs_blk = ItemPointerGetBlockNumber(tid);

		/*
		 * We're switching to a new heap block, so count it
		 */
		hscan->xs_blkswitch_count++;

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
											all_dead,
											!*heap_continue);
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
			*heap_continue = !scan->MVCCScan;

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
			 * We can only safely do this when heap_continue is false, since
			 * otherwise the caller will need xs_cbuf to remain valid for the
			 * next call.
			 */
			if (hscan->xs_lastinblock && !*heap_continue)
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
			*heap_continue = false;
		}

		pgstat_count_heap_fetch(scan->indexRelation);
	}
	else
	{
		/* We've reached the end of the HOT chain. */
		*heap_continue = false;
	}

	return got_heap_tuple;
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
	else
	{
		/*
		 * Tell amgettuple-based index AM to kill its entry for that TID.  The
		 * next tableam_util_fetch_next_tuple_tid call will pass that along to
		 * the index AM, before unsetting the flag again.
		 */
		scan->kill_prior_tuple = true;
	}
}

/*
 * Get next TID from batch ring buffer, moving in the given scan direction.
 * Also sets *all_visible for item when caller passes a non-NULL arg.
 */
static pg_always_inline ItemPointer
heapam_index_getnext_scanbatch_pos(IndexScanDesc scan, IndexScanHeapData *hscan,
								   ScanDirection direction, bool *all_visible)
{
	BatchRingItemPos *scanPos = &scan->batchringbuf.scanPos;
	IndexScanBatch scanBatch;

	Assert(all_visible == NULL || scan->xs_want_itup);

	/*
	 * Attempt to increment the position of any existing loaded scanBatch
	 * (always fails on first call here for the scan)
	 */
	if (tableam_util_scanpos_advance(scan, direction, &scanBatch, scanPos))
	{
		/*
		 * Incremented scanPos within existing loaded scanBatch; return the
		 * new position's TID to caller
		 */
		return heapam_index_return_scanpos_tid(scan, hscan, direction,
											   scanBatch, scanPos,
											   all_visible);
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
		return NULL;
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
	tableam_util_scanpos_nextbatch(scan, direction, scanBatch);

	/*
	 * Set scanPos to first item for newly loaded scanBatch; return the new
	 * position's TID to caller
	 */
	return heapam_index_return_scanpos_tid(scan, hscan, direction,
										   scanBatch, scanPos, all_visible);
}

/*
 * Save the current scanPos/scanBatch item's TID in scan's xs_heaptid, and
 * return a pointer to that TID.  When all_visible isn't NULL (during an
 * index-only scan), also sets item's visibility status in *all_visible.
 *
 * heapam_index_getnext_scanbatch_pos helper function.
 */
static inline ItemPointer
heapam_index_return_scanpos_tid(IndexScanDesc scan, IndexScanHeapData *hscan,
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

		return &scan->xs_heaptid;
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

	return &scan->xs_heaptid;
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
