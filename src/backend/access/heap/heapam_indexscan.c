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
#include "access/relscan.h"
#include "access/visibilitymap.h"
#include "storage/predicate.h"
#include "utils/pgstat_internal.h"


static bool heapam_index_plain_tuple_getnext_slot(IndexScanDesc scan,
												  ScanDirection direction,
												  TupleTableSlot *slot);
static bool heapam_index_only_tuple_getnext_slot(IndexScanDesc scan,
												 ScanDirection direction,
												 TupleTableSlot *slot);
static pg_always_inline bool heapam_index_getnext_slot(IndexScanDesc scan,
													   ScanDirection direction,
													   TupleTableSlot *slot,
													   bool index_only);
static pg_always_inline bool heapam_index_heap_fetch(IndexScanDesc scan, IndexScanHeapData *hscan,
													 TupleTableSlot *slot, bool index_only,
													 bool *heap_continue, bool *all_dead);
static pg_noinline void heapam_index_kill_item(IndexScanDesc scan);

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

	hscan->xs_cbuf = InvalidBuffer;
	hscan->xs_blk = InvalidBlockNumber;
	hscan->xs_vmbuffer = InvalidBuffer;

	/* Remember if scan is read-only */
	hscan->xs_readonly = (flags & SO_HINT_REL_READ_ONLY) != 0;

	/* Resolve which xs_getnext_slot implementation to use for this scan */
	if (scan->xs_want_itup)
		scan->xs_getnext_slot = heapam_index_only_tuple_getnext_slot;
	else
		scan->xs_getnext_slot = heapam_index_plain_tuple_getnext_slot;

	/* Expose heapam's private scan state through the scan's opaque pointer */
	scan->xs_table_opaque = hscan;
}

void
heapam_index_scan_reset(IndexScanDesc scan)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;

	/* Heap fetches from the last rescan don't count towards this limit  */
	hscan->xs_blkswitch_count = 0;

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

	pfree(hscan);
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

/* xs_getnext_slot callback: amgettuple, plain index scan */
static bool
heapam_index_plain_tuple_getnext_slot(IndexScanDesc scan,
									  ScanDirection direction,
									  TupleTableSlot *slot)
{
	Assert(!scan->xs_want_itup);
	Assert(scan->indexRelation->rd_indam->amgettuple != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, false);
}

/* xs_getnext_slot callback: amgettuple, index-only scan */
static bool
heapam_index_only_tuple_getnext_slot(IndexScanDesc scan,
									 ScanDirection direction,
									 TupleTableSlot *slot)
{
	Assert(scan->xs_want_itup);
	Assert(scan->indexRelation->rd_indam->amgettuple != NULL);

	return heapam_index_getnext_slot(scan, direction, slot, true);
}

/*
 * Common implementation for both heapam_index_*_getnext_slot variants.
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
 * The index_only parameter is a compile-time constant at each call site,
 * allowing the compiler to specialize the code for each variant.
 */
static pg_always_inline bool
heapam_index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
						  TupleTableSlot *slot, bool index_only)
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
			tid = index_getnext_tid(scan, direction);

			/* If we're out of index entries, we're done */
			if (tid == NULL)
				break;

			/* For index-only scans, check the visibility map */
			if (index_only)
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
					heapam_index_kill_item(scan);

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
			*heap_continue = !IsMVCCLikeSnapshot(snapshot);

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
heapam_index_kill_item(IndexScanDesc scan)
{
	if (scan->xactStartedInRecovery)
		return;

	/*
	 * Tell amgettuple-based index AM to kill its entry for that TID.  The
	 * next index_getnext_tid call will pass that along to the index AM,
	 * before unsetting the flag again.
	 */
	scan->kill_prior_tuple = true;
}
