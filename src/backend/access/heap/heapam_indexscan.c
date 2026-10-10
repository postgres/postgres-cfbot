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
#include "access/hot_indexed.h"
#include "access/relscan.h"
#include "access/hot_indexed.h"
#include "access/sysattr.h"
#include "access/tableam_indexscan.h"
#include "access/visibilitymap.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/relcache.h"


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
static pg_always_inline bool heapam_index_heap_fetch(IndexScanDesc scan,
													 IndexScanHeapData *hscan,
													 TupleTableSlot *slot,
													 bool index_only);
static pg_noinline bool heapam_index_only_heap_fetch(IndexScanDesc scan);
static pg_noinline void heapam_index_kill_item(IndexScanDesc scan);
static inline bool heapam_index_visited_pages_exceeded(IndexScanDesc scan);
static bool heapam_index_entry_needs_recheck(IndexScanDesc scan, Relation indexRelation);

/*
 * TID-based lookup used by constraint enforcement code (e.g., unique index
 * enforcement).
 *
 * This isn't actually used by index scans, but this is as good a place for it
 * as anywhere else.
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
								   all_dead, true, NULL, NULL, NULL);
	UnlockReleaseBuffer(buf);

	return found;
}

/*
 * heapam_fetch_tid_check
 *
 * Constraint-enforcement TID fetch that also fills a slot and reports whether
 * the arriving index entry may fail to exactly identify the live tuple's
 * current key.  Used by unique/exclusion enforcement (e.g. _bt_check_unique)
 * which holds a TID taken from an index but performs no index scan.
 *
 * Walks the HOT chain from *tid; on a visible match stores the live tuple into
 * *slot and returns true, and sets *recheck true iff the walk crossed a
 * HOT-selectively-updated hop after the arriving entry's own tuple (its stored
 * key may no longer match the live tuple).  The caller performs its own key
 * comparison in that case.  This is heap's implementation of the
 * fetch_tid_check table-AM callback (see table_index_fetch_tuple_check).
 */
bool
heapam_fetch_tid_check(Relation rel, ItemPointer tid, Snapshot snapshot,
					   bool *all_dead, bool *recheck, TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	Buffer		buf;
	bool		found;
	bool		hi_recheck = false;
	int			relnatts = RelationGetNumberOfAttributes(rel);
	uint8	   *crossed = palloc0(HotIndexedBitmapBytes(relnatts));

	Assert(TTS_IS_BUFFERTUPLE(slot));

	buf = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	found = heap_hot_search_buffer(tid, rel, buf, snapshot,
								   &bslot->base.tupdata,
								   all_dead, true,
								   &hi_recheck, crossed, NULL);
	if (found)
	{
		bslot->base.tupdata.t_self = *tid;
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		ExecStorePinnedBufferHeapTuple(&bslot->base.tupdata, slot, buf);
	}
	else
		UnlockReleaseBuffer(buf);

	pfree(crossed);

	if (recheck != NULL)
		*recheck = found && hi_recheck;

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

	/*
	 * Scratch space for the union of modified-attrs bitmaps that a HOT/SIU
	 * chain walk crosses, sized for this relation's column count.  Consumed
	 * by heapam_index_entry_needs_recheck.
	 */
	hscan->xs_hot_indexed_crossed =
		palloc0(HotIndexedBitmapBytes(RelationGetNumberOfAttributes(scan->heapRelation)));

	/* Expose heapam's private scan state through the scan's opaque pointer */
	scan->xs_table_opaque = hscan;
}

void
heapam_index_scan_reset(IndexScanDesc scan)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;

	/* Heap fetches from the last rescan don't count towards this limit */
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

	if (hscan->xs_hot_indexed_crossed != NULL)
		pfree(hscan->xs_hot_indexed_crossed);

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
 * If hot_indexed_recheck is not NULL, *hot_indexed_recheck is set true iff the
 * walk crossed a HOT-selectively-updated (HOT/SIU) hop after the entry tuple
 * on the way to the returned tuple, i.e. the arriving index entry's stored
 * key may no longer match the live tuple, so the caller must recheck it (via
 * a leaf-key comparison or a qual recheck).  The entry tuple's own producing
 * hop is excluded, so a fresh entry pointing directly at its tuple is not
 * flagged.  When no such hop was crossed, *hot_indexed_recheck is left false.
 *
 * Unlike heap_fetch, the caller must already have pin and (at least) share
 * lock on the buffer; it is still pinned/locked at exit.
 */
bool
heap_hot_search_buffer(ItemPointer tid, Relation relation, Buffer buffer,
					   Snapshot snapshot, HeapTuple heapTuple,
					   bool *all_dead, bool first_call,
					   bool *hot_indexed_recheck,
					   uint8 *crossed_bitmap,
					   bool *prefix_all_dead)
{
	Page		page = BufferGetPage(buffer);
	TransactionId prev_xmax = InvalidTransactionId;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		at_chain_start;
	bool		valid;
	bool		skip;
	bool		prefix_dead;
	GlobalVisState *vistest = NULL;
	int			relnatts = RelationGetNumberOfAttributes(relation);
	int			chain_hops;
	OffsetNumber maxoff_guard;

	/* Only track prefix_dead when the caller actually wants it. */
	prefix_dead = (prefix_all_dead != NULL);

	/* If this is not the first call, previous call returned a (live!) tuple */
	if (all_dead)
		*all_dead = first_call;

	/*
	 * On the first call, clear the recheck flag and the crossed-attrs union.
	 * On subsequent calls (same chain continuing) keep whatever an earlier
	 * hop already accumulated.
	 */
	if (first_call)
	{
		if (hot_indexed_recheck)
			*hot_indexed_recheck = false;
		if (crossed_bitmap)
			memset(crossed_bitmap, 0, HotIndexedBitmapBytes(relnatts));
	}

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);
	at_chain_start = first_call;
	skip = !first_call;

	/* XXX: we should assert that a snapshot is pushed or registered */
	Assert(TransactionIdIsValid(RecentXmin));
	Assert(BufferGetBlockNumber(buffer) == blkno);

	/*
	 * Bound the HOT/SIU chain walk.  A corrupt page whose forward links form
	 * a cycle among valid in-range offsets (e.g. stub A -> stub B -> stub A)
	 * would otherwise spin this loop forever under a buffer share-lock, with
	 * no CHECK_FOR_INTERRUPTS to break out.  No legitimate chain visits more
	 * offsets than the page holds.
	 */
	chain_hops = 0;
	maxoff_guard = PageGetMaxOffsetNumber(page);

	/* Scan through possible multiple members of HOT-chain */
	for (;;)
	{
		ItemId		lp;

		CHECK_FOR_INTERRUPTS();
		if (chain_hops++ > maxoff_guard)
			break;			/* defend against a corrupt forward-link cycle */

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
				/*
				 * Follow the redirect.  A collapsed dead prefix is preserved
				 * as a run of forwarding stubs, each carrying its segment's
				 * modified-attrs bitmap, ending at the first live tuple;
				 * chain collapse reclaims a dead member only when its
				 * attributes are a subset of the surviving later hops (see
				 * pruneheap.c).  So the stubs and live hops this walk crosses
				 * below contribute the complete union of every collapsed
				 * hop's modified attributes, and that union drives the
				 * overlap staleness test for the index-access layer.
				 */
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
		 * A collapse-survivor stub is an LP_NORMAL item but not a real tuple:
		 * it is a freeze-safe forwarding node carrying the modified-attrs
		 * bitmap for the chain segment it represents.  Treat it like a
		 * crossed HOT/SIU hop, so arm the recheck and OR its bitmap into the
		 * crossed union (unless we arrived directly at it, in which case the
		 * arriving entry already reflects this segment's value), then
		 * follow its forward link.  A stub is never visible and never
		 * returned, and its forward link is a logical, not xid-continuous,
		 * edge, so reset prev_xmax to skip the chain-integrity check on the
		 * next member.
		 */
		if (HotIndexedHeaderIsStub(heapTuple->t_data))
		{
			if (!at_chain_start)
			{
				if (hot_indexed_recheck)
					*hot_indexed_recheck = true;
				if (crossed_bitmap)
				{
					int			bmnatts =
						HotIndexedTupleBitmapNatts(heapTuple->t_data);

					/*
					 * A hop's write-time natts can never legitimately exceed
					 * the relation's current natts.  On a corrupt page a
					 * stub's unbounded stashed natts could otherwise overflow
					 * crossed_bitmap, which is allocated for relnatts; clamp
					 * defensively.
					 */
					Assert(bmnatts >= 0 && bmnatts <= relnatts);
					if (bmnatts < 0 || bmnatts > relnatts)
						bmnatts = relnatts;

					HotIndexedBitmapUnion(crossed_bitmap,
										  HotIndexedGetModifiedBitmap(heapTuple->t_data,
																	  heapTuple->t_len,
																	  bmnatts),
										  bmnatts);
				}
			}
			offnum = HotIndexedStubGetForward(heapTuple->t_data);
			at_chain_start = false;
			prev_xmax = InvalidTransactionId;
			continue;
		}

		/*
		 * Shouldn't see a HEAP_ONLY tuple at chain start, unless that tuple
		 * is the target of a freshly-inserted hot-indexed index entry: then
		 * arriving directly at a heap-only HOT-indexed tuple is legal and the
		 * tuple is the canonical visible version, so we fall through and
		 * apply normal visibility checks to it.  Otherwise, treat it as a
		 * broken chain.
		 */
		if (at_chain_start && HeapTupleIsHeapOnly(heapTuple))
		{
			if ((heapTuple->t_data->t_infomask2 & HEAP_INDEXED_UPDATED) == 0)
				break;

			/*
			 * We were pointed directly at this hot-indexed tuple.  The index
			 * entry we arrived through was inserted *for* this update, so it
			 * reflects this tuple's current attribute values; its own
			 * producing hop is not a crossed hop, so it is not flagged for
			 * recheck (a fresh entry is never stale for its own index).
			 */
		}
		else if (hot_indexed_recheck != NULL &&
				 (heapTuple->t_data->t_infomask2 & HEAP_INDEXED_UPDATED) != 0)
		{
			/*
			 * A HOT/SIU hop reached by following the chain (or a redirect)
			 * from an earlier entry: this hop is crossed, so the arriving
			 * entry's stored key may no longer match the live tuple.  Set the
			 * recheck flag to tell the index-access layer to consult the
			 * crossed-attrs union; that union (accumulated below) is what
			 * decides staleness.
			 */
			*hot_indexed_recheck = true;

			/*
			 * Accumulate this hop's modified-attrs bitmap into the crossed
			 * union.  A tuple's inline bitmap records the indexed attributes
			 * that changed at the hop INTO it, which is exactly the hop we
			 * just crossed by advancing to it; ORing each crossed hop yields
			 * the indexed attributes that changed after the entry's own
			 * tuple.
			 */
			if (crossed_bitmap)
			{
				int			bmnatts =
					HotIndexedTupleBitmapNatts(heapTuple->t_data);

				/* See the comment on the stub case's crossed_bitmap use. */
				Assert(bmnatts >= 0 && bmnatts <= relnatts);
				if (bmnatts < 0 || bmnatts > relnatts)
					bmnatts = relnatts;

				HotIndexedBitmapUnion(crossed_bitmap,
									  HotIndexedGetModifiedBitmap(heapTuple->t_data,
																  heapTuple->t_len,
																  bmnatts),
									  bmnatts);
			}
		}

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

				/*
				 * Report whether every chain member skipped before this
				 * visible tuple is dead to all transactions.  With a stale
				 * verdict this lets the caller kill the arriving leaf safely.
				 */
				if (prefix_all_dead)
					*prefix_all_dead = prefix_dead;

				return true;
			}
		}
		skip = false;

		/*
		 * If we can't see it, maybe no one else can either.  At caller
		 * request, check whether all chain members are dead to all
		 * transactions.  The same surely-dead test feeds prefix_dead, which
		 * (unlike all_dead) is not reset when a visible tuple is found, so it
		 * records whether the members skipped ahead of the returned tuple are
		 * all dead to all, the safe-to-kill-this-leaf condition.
		 *
		 * Note: if you change the criterion here for what is "dead", fix the
		 * planner's get_actual_variable_range() function to match.
		 */
		if ((all_dead && *all_dead) || (prefix_all_dead && prefix_dead))
		{
			if (!vistest)
				vistest = GlobalVisTestFor(relation);

			if (!HeapTupleIsSurelyDead(heapTuple, vistest))
			{
				if (all_dead)
					*all_dead = false;
				prefix_dead = false;
			}
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
 * found, false otherwise.  This is per the table_index_getnext_slot
 * interface.
 *
 * The index_only parameter is a compile-time constant at each call site,
 * allowing the compiler to specialize the code for each variant.
 */
static pg_always_inline bool
heapam_index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
						  TupleTableSlot *slot, bool index_only)
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
			if (!tableam_index_getnext_tid(scan, direction))
				return false;
		}

		/* The scan's next TID was set in scan->xs_heaptid for us */
		Assert(ItemPointerIsValid(&scan->xs_heaptid));

		hscan = (IndexScanHeapData *) scan->xs_table_opaque;

		/*
		 * Clear the per-entry HOT/SIU staleness signal before the fetch.  The
		 * heap fetch below (re)sets it for the tuple it returns; if we skip
		 * the fetch (index-only scan on an all-visible page) it stays false,
		 * which is correct because prune keeps any page that can carry a stale
		 * HOT-indexed leaf out of the visibility map, so an all-visible entry
		 * is never stale.
		 */
		scan->xs_entry_needs_recheck = false;

		if (!index_only)
		{
			/* Plain index scan */
			if (!heapam_index_heap_fetch(scan, hscan, slot, false))
				continue;		/* no visible tuple, try next index entry */

			/*
			 * If the chain walk to reach this tuple crossed a HOT/SIU hop that
			 * changed a column this scan's index covers, the leaf we arrived
			 * through is stale.  Surface that to the executor (nodeIndexscan
			 * drops it; the fresh entry supplies the row).
			 */
			scan->xs_entry_needs_recheck =
				heapam_index_entry_needs_recheck(scan, scan->indexRelation);
		}
		else
		{
			/*
			 * Note: VM_ALL_VISIBLE does not lock the visibility map buffer,
			 * so the result could be slightly stale.  See the comments above
			 * visibilitymap_get_status for why this is okay.
			 */
			all_visible = VM_ALL_VISIBLE(scan->heapRelation,
										 ItemPointerGetBlockNumber(&scan->xs_heaptid),
										 &hscan->xs_vmbuffer);

			if (unlikely(!all_visible))
			{
				/* Page isn't all-visible, so need a heap fetch */
				if (!heapam_index_only_heap_fetch(scan))
				{
					/* No visible tuple */
					if (heapam_index_visited_pages_exceeded(scan))
						return false;	/* give up */

					continue;	/* try next index entry */
				}

				/*
				 * Index-only scans serve values out of the index tuple, so a
				 * stale leaf would surface the wrong values.  Unlike a plain
				 * scan we cannot leave the drop to the executor (the heap
				 * fetch happened here), so drop the stale entry now; the fresh
				 * entry for the new value returns the row correctly.
				 */
				if (heapam_index_entry_needs_recheck(scan, scan->indexRelation))
					continue;	/* stale leaf, try next index entry */
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
			 * scans heapam_index_heap_fetch does this for us instead)
			 */
			tableam_index_fill_ios_slot(scan, slot);
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
						TupleTableSlot *slot, bool index_only)
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

		/* We're switching to a new heap block, so count it */
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
											&all_dead,
											!scan->xs_heap_continue,
											&hscan->xs_hot_indexed_recheck,
											hscan->xs_hot_indexed_crossed,
											&hscan->xs_prefix_all_dead);
	if (!got_heap_tuple)
	{
		hscan->xs_hot_indexed_recheck = false;
		hscan->xs_prefix_all_dead = false;
	}
	heapTuple->t_self = *tid;
	LockBuffer(hscan->xs_cbuf, BUFFER_LOCK_UNLOCK);

	if (got_heap_tuple)
	{
		if (!index_only)
		{
			/*
			 * Only with a non-MVCC snapshot can more than one HOT chain
			 * member be visible, so only then must we keep walking the chain
			 */
			scan->xs_heap_continue = !IsMVCCLikeSnapshot(snapshot);

			ExecStoreBufferHeapTuple(heapTuple, slot, hscan->xs_cbuf);

			Assert(slot->tts_tableOid == RelationGetRelid(rel));
		}
		else
		{
			/*
			 * Index-only scans stop at the first visible HOT chain member.
			 * With a non-MVCC snapshot a later member could also be visible,
			 * but we never look.  That's fine for the only non-MVCC
			 * index-only scan caller (selfuncs.c), which only needs to know
			 * that some version is visible.
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
			heapam_index_kill_item(scan);
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
heapam_index_only_heap_fetch(IndexScanDesc scan)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;

	return heapam_index_heap_fetch(scan, hscan, NULL, true);
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
	 * next tableam_index_getnext_tid call will pass that along to the index
	 * AM, before unsetting the flag again.
	 */
	scan->kill_prior_tuple = true;
}

/*
 * Did an index-only scan switch heap pages more times than the caller's
 * visited-pages limit allows?
 *
 * Caller passes scan rather than hscan to avoiding keeping hscan live across
 * heap fetches.
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
 * heapam_index_entry_needs_recheck
 *
 * Report whether the entry that led to the last-fetched tuple may fail to
 * exactly identify the live tuple's current key.  The chain walk in
 * heap_hot_search_buffer recorded, privately in IndexScanHeapData, whether it
 * crossed a HOT/SIU hop after the arriving entry's own tuple
 * (xs_hot_indexed_recheck) and the union of modified attributes it crossed
 * (xs_hot_indexed_crossed).
 *
 * With indexRelation NULL, report the raw "a hop was crossed at all" signal.
 * With indexRelation given, narrow it: the entry is stale only if a crossed
 * hop changed one of the columns that index covers, tested by overlapping the
 * crossed-attribute bitmap with the index's indexed attributes.  No key
 * comparison is needed: any overlap means one of this index's inputs changed
 * after the entry's tuple, so its key is stale.
 */
static bool
heapam_index_entry_needs_recheck(IndexScanDesc scan, Relation indexRelation)
{
	IndexScanHeapData *hscan = (IndexScanHeapData *) scan->xs_table_opaque;
	Bitmapset  *idxattrs;
	int			x = -1;
	bool		needs_recheck = false;

	if (!hscan->xs_hot_indexed_recheck || hscan->xs_hot_indexed_crossed == NULL)
		return false;

	/* Raw signal: a HOT/SIU hop was crossed, un-narrowed to any index. */
	if (indexRelation == NULL)
		return true;

	idxattrs = RelationGetIndexedAttrs(indexRelation);
	while ((x = bms_next_member(idxattrs, x)) >= 0)
	{
		AttrNumber	attnum = x + FirstLowInvalidHeapAttributeNumber;

		/* the crossed bitmap records only user attributes */
		if (attnum >= 1 &&
			HotIndexedAttrIsModified(hscan->xs_hot_indexed_crossed, attnum))
		{
			needs_recheck = true;
			break;
		}
	}
	bms_free(idxattrs);

	return needs_recheck;
}
