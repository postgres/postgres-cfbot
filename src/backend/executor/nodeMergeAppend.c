/*-------------------------------------------------------------------------
 *
 * nodeMergeAppend.c
 *	  routines to handle MergeAppend nodes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMergeAppend.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecInitMergeAppend		- initialize the MergeAppend node
 *		ExecMergeAppend			- retrieve the next tuple from the node
 *		ExecEndMergeAppend		- shut down the MergeAppend node
 *		ExecReScanMergeAppend	- rescan the MergeAppend node
 *
 *	 NOTES
 *		A MergeAppend node contains a list of one or more subplans.
 *		These are each expected to deliver tuples that are sorted according
 *		to a common sort key.  The MergeAppend node merges these streams
 *		to produce output sorted the same way.
 *
 *		MergeAppend supports async-capable subplans (e.g. foreign scans).
 *		Async execution is beneficial during the initial heap fill, where
 *		all async subplans are kicked off concurrently and their first
 *		tuples are fetched in parallel.  In steady state, the heap
 *		algorithm still needs the next tuple from one specific subplan
 *		(the one at the heap top), so we can only make progress on that
 *		one subplan at a time.  For a synchronous subplan we simply call
 *		ExecProcNode() again.  An asynchronous subplan, however, must
 *		still be driven through the async request/response protocol: a
 *		plain ExecProcNode() call bypasses it, and FDWs such as
 *		postgres_fdw treat that as end-of-data once their locally cached
 *		batch of tuples is exhausted.  So for those we issue a fresh
 *		async request and block until it completes.
 *
 *		MergeAppend nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans so
 *		a typical MergeAppend node looks like this in the plan tree:
 *
 *				   ...
 *				   /
 *				MergeAppend---+------+------+--- nil
 *				/	\		  |		 |		|
 *			  nil	nil		 ...    ...    ...
 *								 subplans
 */

#include "postgres.h"

#include "executor/execAppend.h"
#include "executor/execAsync.h"
#include "executor/executor.h"
#include "executor/execPartition.h"
#include "executor/nodeMergeAppend.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "utils/sortsupport.h"
#include "utils/wait_event.h"

/*
 * We have one slot for each item in the heap array.  We use SlotNumber
 * to store slot indexes.  This doesn't actually provide any formal
 * type-safety, but it makes the code more self-documenting.
 */
typedef int32 SlotNumber;

static TupleTableSlot *ExecMergeAppend(PlanState *pstate);
static int	heap_compare_slots(Datum a, Datum b, void *arg);

static void classify_matching_subplans(MergeAppendState *node);
static void ExecMergeAppendAsyncBegin(MergeAppendState *node);
static void ExecMergeAppendAsyncGetNext(MergeAppendState *node, int mplan);
static void ExecMergeAppendAsyncEventWait(MergeAppendState *node);
static void ExecMergeAppendGetNextSlot(MergeAppendState *node, int mplan);


/* ----------------------------------------------------------------
 *		ExecInitMergeAppend
 *
 *		Begin all of the subscans of the MergeAppend node.
 * ----------------------------------------------------------------
 */
MergeAppendState *
ExecInitMergeAppend(MergeAppend *node, EState *estate, int eflags)
{
	MergeAppendState *mergestate = makeNode(MergeAppendState);
	int			i;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create new MergeAppendState for our node
	 */
	mergestate->as.ps.plan = (Plan *) node;
	mergestate->as.ps.state = estate;
	mergestate->as.ps.ExecProcNode = ExecMergeAppend;

	/* Initialize common fields */
	ExecInitAppendBase(&mergestate->as,
					   &node->ab,
					   estate,
					   eflags,
					   -1,
					   NULL);

	if (mergestate->as.nasyncplans > 0 && mergestate->as.valid_subplans_identified)
		classify_matching_subplans(mergestate);

	mergestate->ms_slots = palloc0_array(TupleTableSlot *, mergestate->as.nplans);
	mergestate->ms_heap = binaryheap_allocate(mergestate->as.nplans, heap_compare_slots,
											  mergestate);

	mergestate->ms_asyncremain = NULL;

	/*
	 * initialize sort-key information
	 */
	mergestate->ms_nkeys = node->numCols;
	mergestate->ms_sortkeys = palloc0_array(SortSupportData, node->numCols);

	for (i = 0; i < node->numCols; i++)
	{
		SortSupport sortKey = mergestate->ms_sortkeys + i;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = node->collations[i];
		sortKey->ssup_nulls_first = node->nullsFirst[i];
		sortKey->ssup_attno = node->sortColIdx[i];

		/*
		 * It isn't feasible to perform abbreviated key conversion, since
		 * tuples are pulled into mergestate's binary heap as needed.  It
		 * would likely be counter-productive to convert tuples into an
		 * abbreviated representation as they're pulled up, so opt out of that
		 * additional optimization entirely.
		 */
		sortKey->abbreviate = false;

		PrepareSortSupportFromOrderingOp(node->sortOperators[i], sortKey);
	}

	/*
	 * initialize to show we have not run the subplans yet
	 */
	mergestate->ms_initialized = false;

	return mergestate;
}

/* ----------------------------------------------------------------
 *	   ExecMergeAppend
 *
 *		Handles iteration over multiple subplans.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecMergeAppend(PlanState *pstate)
{
	MergeAppendState *node = castNode(MergeAppendState, pstate);
	TupleTableSlot *result;
	SlotNumber	i;

	CHECK_FOR_INTERRUPTS();

	if (!node->ms_initialized)
	{
		/* Nothing to do if all subplans were pruned */
		if (node->as.nplans == 0)
			return ExecClearTuple(node->as.ps.ps_ResultTupleSlot);

		/*
		 * If we've yet to determine the valid subplans then do so now.  If
		 * run-time pruning is disabled then the valid subplans will always be
		 * set to all subplans.
		 */
		if (!node->as.valid_subplans_identified)
		{
			node->as.valid_subplans =
				ExecFindMatchingSubPlans(node->as.prune_state, false, NULL);
			node->as.valid_subplans_identified = true;
			classify_matching_subplans(node);
		}

		/* If there are any async subplans, begin executing them. */
		if (node->as.nasyncplans > 0)
			ExecMergeAppendAsyncBegin(node);

		/*
		 * First time through: pull the first tuple from each valid subplan,
		 * and set up the heap.
		 */
		i = -1;
		while ((i = bms_next_member(node->as.valid_subplans, i)) >= 0)
		{
			node->ms_slots[i] = ExecProcNode(node->as.plans[i]);
			if (!TupIsNull(node->ms_slots[i]))
				binaryheap_add_unordered(node->ms_heap, Int32GetDatum(i));
		}

		/* Look at valid async subplans */
		i = -1;
		while ((i = bms_next_member(node->as.valid_asyncplans, i)) >= 0)
		{
			ExecMergeAppendAsyncGetNext(node, i);
			if (!TupIsNull(node->ms_slots[i]))
				binaryheap_add_unordered(node->ms_heap, Int32GetDatum(i));
		}

		binaryheap_build(node->ms_heap);
		node->ms_initialized = true;
	}
	else
	{
		/*
		 * Otherwise, pull the next tuple from whichever subplan we returned
		 * from last time, and reinsert the subplan index into the heap,
		 * because it might now compare differently against the existing
		 * elements of the heap.  (We could perhaps simplify the logic a bit
		 * by doing this before returning from the prior call, but it's better
		 * to not pull tuples until necessary.)
		 */
		i = DatumGetInt32(binaryheap_first(node->ms_heap));
		ExecMergeAppendGetNextSlot(node, i);
		if (!TupIsNull(node->ms_slots[i]))
			binaryheap_replace_first(node->ms_heap, Int32GetDatum(i));
		else
			(void) binaryheap_remove_first(node->ms_heap);
	}

	if (binaryheap_empty(node->ms_heap))
	{
		/* All the subplans are exhausted, and so is the heap */
		result = ExecClearTuple(node->as.ps.ps_ResultTupleSlot);
	}
	else
	{
		i = DatumGetInt32(binaryheap_first(node->ms_heap));
		result = node->ms_slots[i];
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecMergeAppendGetNextSlot
 *
 *		Retrieve the next tuple for the given subplan into ms_slots[mplan].
 *		Synchronous subplans are pulled directly; asynchronous ones must go
 *		through the async request/response protocol, since a plain
 *		ExecProcNode() call bypasses it and would be mistaken by an
 *		async-capable FDW for end-of-data (see the file header comment).
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendGetNextSlot(MergeAppendState *node, int mplan)
{
	if (bms_is_member(mplan, node->as.asyncplans))
	{
		AsyncRequest *areq = node->as.asyncrequests[mplan];

		Assert(bms_is_member(mplan, node->as.valid_asyncplans));
		Assert(!areq->callback_pending);

		/* Discard the previously-returned result before requesting a new one. */
		node->ms_slots[mplan] = NULL;
		ExecAsyncRequest(areq);
		ExecMergeAppendAsyncGetNext(node, mplan);
	}
	else
		node->ms_slots[mplan] = ExecProcNode(node->as.plans[mplan]);
}

/*
 * Compare the tuples in the two given slots.
 */
static int32
heap_compare_slots(Datum a, Datum b, void *arg)
{
	MergeAppendState *node = (MergeAppendState *) arg;
	SlotNumber	slot1 = DatumGetInt32(a);
	SlotNumber	slot2 = DatumGetInt32(b);

	TupleTableSlot *s1 = node->ms_slots[slot1];
	TupleTableSlot *s2 = node->ms_slots[slot2];
	int			nkey;

	Assert(!TupIsNull(s1));
	Assert(!TupIsNull(s2));

	for (nkey = 0; nkey < node->ms_nkeys; nkey++)
	{
		SortSupport sortKey = node->ms_sortkeys + nkey;
		AttrNumber	attno = sortKey->ssup_attno;
		Datum		datum1,
					datum2;
		bool		isNull1,
					isNull2;
		int			compare;

		datum1 = slot_getattr(s1, attno, &isNull1);
		datum2 = slot_getattr(s2, attno, &isNull2);

		compare = ApplySortComparator(datum1, isNull1,
									  datum2, isNull2,
									  sortKey);
		if (compare != 0)
		{
			INVERT_COMPARE_RESULT(compare);
			return compare;
		}
	}
	return 0;
}

/* ----------------------------------------------------------------
 *		ExecEndMergeAppend
 *
 *		Shuts down the subscans of the MergeAppend node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndMergeAppend(MergeAppendState *node)
{
	ExecEndAppendBase(&node->as);
}

void
ExecReScanMergeAppend(MergeAppendState *node)
{
	int			nasyncplans = node->as.nasyncplans;

	/*
	 * MergeAppend never has an async request still pending a callback at
	 * rescan time: it uses async only to seed the heap, fetching one tuple
	 * from each child, and every async fetch is immediately followed by a
	 * synchronous wait for its result (see ExecMergeAppendGetNextSlot()).  So
	 * the async reset in ExecReScanAppendBase() has nothing to drain for us.
	 */
	ExecReScanAppendBase(&node->as);

	/* Reset MergeAppend-specific state */
	if (nasyncplans > 0)
	{
		bms_free(node->ms_asyncremain);
		node->ms_asyncremain = NULL;
	}
	binaryheap_reset(node->ms_heap);
	node->ms_initialized = false;

	/*
	 * Discard any results left over from before the rescan.  An async
	 * subplan's slot can still hold a tuple that was delivered but never
	 * consumed (e.g. because a LIMIT above us stopped pulling before that
	 * subplan reached the heap top); if we didn't clear it here, the next
	 * scan's initial fill would mistake it for a fresh result via its
	 * TupIsNull() check, instead of waiting for the subplan's new async
	 * response.
	 */
	memset(node->ms_slots, 0, node->as.nplans * sizeof(TupleTableSlot *));
}

/* ----------------------------------------------------------------
 *		classify_matching_subplans
 *
 *		Classify the node's ms_valid_subplans into sync ones and
 *		async ones, adjust it to contain sync ones only, and save
 *		async ones in the node's as.valid_asyncplans.
 * ----------------------------------------------------------------
 */
static void
classify_matching_subplans(MergeAppendState *node)
{
	Assert(node->as.valid_subplans_identified);

	/* Nothing to do if there are no valid subplans. */
	if (bms_is_empty(node->as.valid_subplans))
	{
		node->ms_asyncremain = NULL;
		return;
	}

	/* No valid async subplans identified. */
	if (!classify_matching_subplans_common(&node->as.valid_subplans,
										   node->as.asyncplans,
										   &node->as.valid_asyncplans))
		node->ms_asyncremain = NULL;
}

/* ----------------------------------------------------------------
 *		ExecMergeAppendAsyncBegin
 *
 *		Begin executing designed async-capable subplans.
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendAsyncBegin(MergeAppendState *node)
{
	/* ExecMergeAppend() identifies valid subplans */
	Assert(node->as.valid_subplans_identified);

	/* Initialize state variables. */
	node->ms_asyncremain = bms_copy(node->as.valid_asyncplans);

	/* Nothing to do if there are no valid async subplans. */
	if (bms_is_empty(node->ms_asyncremain))
		return;

	ExecAppendBaseAsyncBegin(&node->as);
}

/* ----------------------------------------------------------------
 *		ExecMergeAppendAsyncGetNext
 *
 *		Get the next tuple from specified asynchronous subplan.
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendAsyncGetNext(MergeAppendState *node, int mplan)
{
	/*
	 * All initial async requests were fired by ExecMergeAppendAsyncBegin. The
	 * result may already be cached from a prior event wait - if so, nothing
	 * to do.  Otherwise, wait for the specific subplan to deliver a tuple or
	 * report exhaustion.
	 */
	while (TupIsNull(node->ms_slots[mplan]) &&
		   bms_is_member(mplan, node->ms_asyncremain))
	{
		CHECK_FOR_INTERRUPTS();
		ExecMergeAppendAsyncEventWait(node);
	}
}

/* ----------------------------------------------------------------
 *		ExecAsyncMergeAppendResponse
 *
 *		Receive a response from an asynchronous request we made.
 * ----------------------------------------------------------------
 */
void
ExecAsyncMergeAppendResponse(AsyncRequest *areq)
{
	MergeAppendState *node = (MergeAppendState *) areq->requestor;
	TupleTableSlot *slot = areq->result;

	/* The result should be a TupleTableSlot or NULL. */
	Assert(slot == NULL || IsA(slot, TupleTableSlot));

	/* Nothing to do if the request is pending. */
	if (!areq->request_complete)
	{
		/* The request would have been pending for a callback. */
		Assert(areq->callback_pending);
		return;
	}

	/* If the result is NULL or an empty slot, the subplan is exhausted. */
	if (TupIsNull(slot))
	{
		/* The ending subplan wouldn't have been pending for a callback. */
		Assert(!areq->callback_pending);
		node->ms_asyncremain = bms_del_member(node->ms_asyncremain,
											  areq->request_index);
		return;
	}

	/* Save result directly into the merge slot array. */
	node->ms_slots[areq->request_index] = slot;
}

/* ----------------------------------------------------------------
 *		ExecMergeAppendAsyncEventWait
 *
 *		Wait or poll for file descriptor events and fire callbacks.
 * ----------------------------------------------------------------
 */
static void
ExecMergeAppendAsyncEventWait(MergeAppendState *node)
{
	/* We should never be called when there are no valid async subplans. */
	Assert(bms_num_members(node->ms_asyncremain) > 0);

	ExecAppendBaseAsyncEventWait(&node->as, -1 /* no timeout */ ,
								 WAIT_EVENT_MERGE_APPEND_READY);
}
