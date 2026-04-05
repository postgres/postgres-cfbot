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
#include "executor/execPartition.h"
#include "executor/executor.h"

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
	int			nplans;
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
	firstvalid = nplans;
	i = -1;
	while ((i = bms_next_member(validsubplans, i)) >= 0)
	{
		Plan	   *initNode = (Plan *) list_nth(node->subplans, i);

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

	/* Initialize async state to safe defaults */
	state->asyncplans = NULL;
	state->nasyncplans = 0;
	state->asyncrequests = NULL;
	state->asyncresults = NULL;
	state->needrequest = NULL;
	state->eventset = NULL;
	state->valid_asyncplans = NULL;

	/*
	 * Miscellaneous initialization
	 */
	state->ps.ps_ProjInfo = NULL;
}

void
ExecReScanAppendBase(AppendBaseState *node)
{
	int			i;

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
		 * first ExecProcNode.
		 */
		if (subnode->chgParam == NULL)
			ExecReScan(subnode);
	}
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
