/*-------------------------------------------------------------------------
 *
 * execAppend.c
 *	  This code provides support functions for executing MergeAppend and
 *	  Append nodes.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/executor/execAppend.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/execAppend.h"
#include "executor/execAsync.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"

#define EVENT_BUFFER_SIZE			16

/*  Begin all of the subscans of an AppendBase node. */
void
ExecInitAppendBase(AppendBaseState *state,
				   AppendBase *node,
				   EState *estate,
				   int eflags,
				   int first_partial_plan,
				   int *first_valid_partial_plan)
{
	PlanState **appendplanstates;
	const TupleTableSlotOps *appendops;
	Bitmapset  *validsubplans;
	Bitmapset  *asyncplans;
	int			nplans;
	int			nasyncplans;
	int			firstvalid;
	int			i,
				j;

	/* If run-time partition pruning is enabled, then set that up now */
	if (node->part_prune_index >= 0)
	{
		PartitionPruneState *prunestate;

		/*
		 * Set up pruning data structure.  This also initializes the set of
		 * subplans to initialize (validsubplans) by taking into account the
		 * result of performing initial pruning if any.
		 */
		prunestate = ExecInitPartitionExecPruning(&state->ps,
												  list_length(node->subplans),
												  node->part_prune_index,
												  node->apprelids,
												  &validsubplans);
		state->prune_state = prunestate;
		nplans = bms_num_members(validsubplans);

		/*
		 * When no run-time pruning is required and there's at least one
		 * subplan, we can fill valid_subplans immediately, preventing later
		 * calls to ExecFindMatchingSubPlans.
		 */
		if (!prunestate->do_exec_prune && nplans > 0)
		{
			state->valid_subplans = bms_add_range(NULL, 0, nplans - 1);
			state->valid_subplans_identified = true;
		}
	}
	else
	{
		nplans = list_length(node->subplans);

		/*
		 * When run-time partition pruning is not enabled we can just mark all
		 * subplans as valid; they must also all be initialized.
		 */
		Assert(nplans > 0);
		state->valid_subplans = validsubplans =
			bms_add_range(NULL, 0, nplans - 1);
		state->valid_subplans_identified = true;
		state->prune_state = NULL;
	}

	appendplanstates = palloc0_array(PlanState *, nplans);

	/*
	 * call ExecInitNode on each of the valid plans to be executed and save
	 * the results into the appendplanstates array.
	 *
	 * While at it, find out the first valid partial plan.
	 */
	j = 0;
	asyncplans = NULL;
	nasyncplans = 0;
	firstvalid = nplans;
	i = -1;
	while ((i = bms_next_member(validsubplans, i)) >= 0)
	{
		Plan	   *initNode = (Plan *) list_nth(node->subplans, i);

		/*
		 * Record async subplans.  When executing EvalPlanQual, we treat them
		 * as sync ones; don't do this when initializing an EvalPlanQual plan
		 * tree.
		 */
		if (initNode->async_capable && estate->es_epq_active == NULL)
		{
			asyncplans = bms_add_member(asyncplans, j);
			nasyncplans++;
		}

		/*
		 * Record the lowest appendplans index which is a valid partial plan.
		 */
		if (i >= first_partial_plan && j < firstvalid)
			firstvalid = j;

		appendplanstates[j++] = ExecInitNode(initNode, estate, eflags);
	}

	if (first_valid_partial_plan)
		*first_valid_partial_plan = firstvalid;

	state->plans = appendplanstates;
	state->nplans = nplans;

	/*
	 * Initialize Append's result tuple type and slot.  If the child plans all
	 * produce the same fixed slot type, we can use that slot type; otherwise
	 * make a virtual slot.  (Note that the result slot itself is used only to
	 * return a null tuple at end of execution; real tuples are returned to
	 * the caller in the children's own result slots.  What we are doing here
	 * is allowing the parent plan node to optimize if the Append will return
	 * only one kind of slot.)
	 */
	appendops = ExecGetCommonSlotOps(appendplanstates, j);
	if (appendops != NULL)
	{
		ExecInitResultTupleSlotTL(&state->ps, appendops);
	}
	else
	{
		ExecInitResultTupleSlotTL(&state->ps, &TTSOpsVirtual);
		/* show that the output slot type is not fixed */
		state->ps.resultopsset = true;
		state->ps.resultopsfixed = false;
	}

	/* Initialize async state */
	state->asyncplans = asyncplans;
	state->nasyncplans = nasyncplans;
	state->asyncrequests = NULL;
	state->asyncresults = NULL;
	state->needrequest = NULL;
	state->eventset = NULL;
	state->valid_asyncplans = NULL;

	if (nasyncplans > 0)
	{
		state->asyncrequests = palloc0_array(AsyncRequest *, nplans);

		i = -1;
		while ((i = bms_next_member(asyncplans, i)) >= 0)
		{
			AsyncRequest *areq;

			areq = palloc_object(AsyncRequest);
			areq->requestor = (PlanState *) state;
			areq->requestee = appendplanstates[i];
			areq->request_index = i;
			areq->callback_pending = false;
			areq->request_complete = false;
			areq->result = NULL;

			state->asyncrequests[i] = areq;
		}

		state->asyncresults = palloc0_array(TupleTableSlot *, nasyncplans);
	}

	/*
	 * Miscellaneous initialization
	 */
	state->ps.ps_ProjInfo = NULL;
}

void
ExecReScanAppendBase(AppendBaseState *node)
{
	int			i;
	int			nasyncplans = node->nasyncplans;

	/*
	 * If any PARAM_EXEC Params used in pruning expressions have changed, then
	 * we'd better unset the valid subplans so that they are reselected for
	 * the new parameter values.
	 */
	if (node->prune_state &&
		bms_overlap(node->ps.chgParam,
					node->prune_state->execparamids))
	{
		node->valid_subplans_identified = false;
		bms_free(node->valid_subplans);
		node->valid_subplans = NULL;
		bms_free(node->valid_asyncplans);
		node->valid_asyncplans = NULL;
	}

	for (i = 0; i < node->nplans; i++)
	{
		PlanState  *subnode = node->plans[i];

		/*
		 * ExecReScan doesn't know about my subplans, so I have to do
		 * changed-parameter signaling myself.
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * If chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.  For an async-capable subplan, that's by
		 * ExecAppendBaseAsyncBegin()/ExecAsyncRequest() instead, the next
		 * time it fires a request for it.
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}

	/*
	 * Reset async requests. Append node has already polled for async request.
	 * MergeAppend can't have pending async requests in ExecReScan.
	 */
	if (nasyncplans > 0)
	{
		i = -1;
		while ((i = bms_next_member(node->asyncplans, i)) >= 0)
		{
			AsyncRequest *areq = node->asyncrequests[i];

			Assert(!areq->callback_pending);
			areq->request_complete = false;
			areq->result = NULL;
		}

		bms_free(node->needrequest);
		node->needrequest = NULL;
	}
}

/*  Wait or poll for file descriptor events and fire callbacks. */
void
ExecAppendBaseAsyncEventWait(AppendBaseState *node, int timeout,
							 uint32 wait_event_info)
{
	int			nevents = node->nasyncplans + 2;	/* one for PM death and
													 * one for latch */
	int			noccurred;
	int			i;
	WaitEvent	occurred_event[EVENT_BUFFER_SIZE];

	Assert(node->eventset == NULL);

	node->eventset = CreateWaitEventSet(CurrentResourceOwner, nevents);
	AddWaitEventToSet(node->eventset, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET,
					  NULL, NULL);

	/* Give each waiting subplan a chance to add an event. */
	i = -1;
	while ((i = bms_next_member(node->asyncplans, i)) >= 0)
	{
		AsyncRequest *areq = node->asyncrequests[i];

		if (areq->callback_pending)
			ExecAsyncConfigureWait(areq);
	}

	/*
	 * No need for further processing if none of the subplans configured any
	 * events.
	 */
	if (GetNumRegisteredWaitEvents(node->eventset) == 1)
	{
		FreeWaitEventSet(node->eventset);
		node->eventset = NULL;
		return;
	}

	/*
	 * Add the process latch to the set, so that we wake up to process the
	 * standard interrupts with CHECK_FOR_INTERRUPTS().
	 *
	 * NOTE: For historical reasons, it's important that this is added to the
	 * WaitEventSet after the ExecAsyncConfigureWait() calls.  Namely,
	 * postgres_fdw calls "GetNumRegisteredWaitEvents(set) == 1" to check if
	 * any other events are in the set.  That's a poor design, it's
	 * questionable for postgres_fdw to be doing that in the first place, but
	 * we cannot change it now.  The pattern has possibly been copied to other
	 * extensions too.
	 */
	AddWaitEventToSet(node->eventset, WL_LATCH_SET, PGINVALID_SOCKET,
					  MyLatch, NULL);

	/* Return at most EVENT_BUFFER_SIZE events in one call. */
	if (nevents > EVENT_BUFFER_SIZE)
		nevents = EVENT_BUFFER_SIZE;

	/*
	 * If the timeout is -1, wait until at least one event occurs.  If the
	 * timeout is 0, poll for events, but do not wait at all.
	 */
	noccurred = WaitEventSetWait(node->eventset, timeout, occurred_event,
								 nevents, wait_event_info);
	FreeWaitEventSet(node->eventset);
	node->eventset = NULL;
	if (noccurred == 0)
		return;

	/* Deliver notifications. */
	for (i = 0; i < noccurred; i++)
	{
		WaitEvent  *w = &occurred_event[i];

		/*
		 * Each waiting subplan should have registered its wait event with
		 * user_data pointing back to its AsyncRequest.
		 */
		if ((w->events & WL_SOCKET_READABLE) != 0)
		{
			AsyncRequest *areq = (AsyncRequest *) w->user_data;

			if (areq->callback_pending)
			{
				/*
				 * Mark it as no longer needing a callback.  We must do this
				 * before dispatching the callback in case the callback resets
				 * the flag.
				 */
				areq->callback_pending = false;

				/* Do the actual work. */
				ExecAsyncNotify(areq);
			}
		}

		/* Handle standard interrupts */
		if ((w->events & WL_LATCH_SET) != 0)
		{
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}
}

/*  Begin executing async-capable subplans. */
void
ExecAppendBaseAsyncBegin(AppendBaseState *node)
{
	int			i;

	/* Backward scan is not supported by async-aware Appends. */
	Assert(ScanDirectionIsForward(node->ps.state->es_direction));

	/* We should never be called when there are no subplans */
	Assert(node->nplans > 0);

	/* We should never be called when there are no async subplans. */
	Assert(node->nasyncplans > 0);

	/* Make a request for each of the valid async subplans. */
	i = -1;
	while ((i = bms_next_member(node->valid_asyncplans, i)) >= 0)
	{
		AsyncRequest *areq = node->asyncrequests[i];

		Assert(areq->request_index == i);
		Assert(!areq->callback_pending);

		/* Do the actual work. */
		ExecAsyncRequest(areq);
	}
}

/*
 * classify_matching_subplans_common
 *		Common part of classify_matching_subplans() for Append and MergeAppend.
 *
 * Splits valid_subplans into sync and async sets.  Returns false if there
 * are no valid async subplans, true otherwise.
 */
bool
classify_matching_subplans_common(Bitmapset **valid_subplans,
								  Bitmapset *asyncplans,
								  Bitmapset **valid_asyncplans)
{
	Assert(*valid_asyncplans == NULL);

	/* Checked by classify_matching_subplans() */
	Assert(!bms_is_empty(*valid_subplans));

	/* Nothing to do if there are no valid async subplans. */
	if (!bms_overlap(*valid_subplans, asyncplans))
		return false;

	/* Get valid async subplans. */
	*valid_asyncplans = bms_intersect(asyncplans,
									  *valid_subplans);

	/* Adjust the valid subplans to contain sync subplans only. */
	*valid_subplans = bms_del_members(*valid_subplans,
									  *valid_asyncplans);
	return true;
}

/*  Shuts down the subplans of an AppendBase node. */
void
ExecEndAppendBase(AppendBaseState *node)
{
	PlanState **subplans;
	int			nplans;
	int			i;

	/*
	 * get information from the node
	 */
	subplans = node->plans;
	nplans = node->nplans;

	/*
	 * shut down each of the subscans
	 */
	for (i = 0; i < nplans; i++)
		ExecEndNode(subplans[i]);
}
