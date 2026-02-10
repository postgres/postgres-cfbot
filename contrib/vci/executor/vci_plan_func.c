/*-------------------------------------------------------------------------
 *
 * vci_plan_func.c
 *	  General-purpose manipulations of plan trees
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_plan_func.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/bitmapset.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"

#include "vci.h"
#include "vci_executor.h"

static bool expression_walker_core(Plan *plan, bool (*walker) (Node *, void *), bool (*walker_initplan) (Node *, void *), void (*attr_cb) (AttrNumber *, void *), void *context);
static bool subplan_mutator(PlannedStmt *plannedstmt, Plan **plan_p, int plan_id, vci_mutator_t mutator, vci_topmost_plan_cb_t topmostplan, void *context, int eflags, bool *changed);
static bool plan_tree_mutator(Plan **edge, Plan *plan, vci_mutator_t mutator, void *context, int eflags, bool *changed);
static bool plan_list_tree_mutator(List **plan_list, Plan *plan, vci_mutator_t mutator, void *context, int eflags, bool *changed);

/*---------------------------------------------------------------------------*/
/* Plan walker                                                               */
/*---------------------------------------------------------------------------*/

/**
 * Helper function that traverse plan tree without updating it
 *
 * @param[in]     plannedstmt Pointer to PlannedStmt type struct that holds the plan tree to be traversed
 * @param[in]     walker      Callback function to be used in traverse. Returns true to stop cycle.
 * @param[in]     topmostplan Callback function to call before analyzing Topmost plan node
 * @param[in,out] context     Pointer to arbitrary data to pass to callback function
:
 * @return true when callback function stop cycle, false if cycle is complete
 */
bool
vci_plannedstmt_tree_walker(PlannedStmt *plannedstmt, bool (*walker) (Plan *, void *), vci_topmost_plan_cb_t topmostplan, void *context)
{
	int			i;
	ListCell   *l;

	if (plannedstmt == NULL)
		return false;

	i = 1;
	foreach(l, plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(l);

		if (subplan == NULL)
			continue;

		if (topmostplan)
			topmostplan(subplan, i /* plan_id */ , context);

		if (walker(subplan, context))
			return true;

		i++;
	}

	if (plannedstmt->planTree)
	{
		if (topmostplan)
			topmostplan(plannedstmt->planTree, 0 /* plan_id */ , context);

		if (walker(plannedstmt->planTree, context))
			return true;
	}

	return false;
}

/**
 * Helper function that traverse plan node without updating it
 *
 * @param[in]     plan        Pointer to Plan type struct that holds the plan node to be traversed
 * @param[in]     walker      Callback function to be used in traverse. Returns true to stop cycle.
 * @param[in,out] context     Pointer to arbitrary data to pass to callback function
 *
 * @return true when callback function stop cycle, false if cycle is complete
 */
bool
vci_plan_tree_walker(Plan *plan, bool (*walker) (Plan *, void *), void *context)
{
	ListCell   *lc;

	switch (nodeTag(plan))
	{
		case T_ForeignScan:
		case T_ModifyTable:
		case T_LockRows:
			elog(DEBUG1, "unsupported node type: %s(%d)",
				 VciGetNodeName(nodeTag(plan)), (int) nodeTag(plan));
			return true;

		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
			{
				if (walker((Plan *) lfirst(lc), context))
					return true;
			}
			break;

		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) plan)->mergeplans)
			{
				if (walker((Plan *) lfirst(lc), context))
					return true;
			}
			break;

		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
			{
				if (walker((Plan *) lfirst(lc), context))
					return true;
			}
			break;

		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
			{
				if (walker((Plan *) lfirst(lc), context))
					return true;
			}
			break;

		case T_SubqueryScan:
			if (((SubqueryScan *) plan)->subplan)
				if (walker(((SubqueryScan *) plan)->subplan, context))
					return true;
			break;

		default:
			break;
	}

	if (outerPlan(plan))
		if (walker(outerPlan(plan), context))
			return true;

	if (innerPlan(plan))
		if (walker(innerPlan(plan), context))
			return true;

	return false;
}

/**
 * Helper function that traverse expression tree in plan node without updating it
 *
 * @param[in]     plan        Pointer to Plan type struct that holds the plan node to be traversed
 * @param[in]     walker      Callback function to be used in traverse. Returns true to stop cycle.
 * @param[in,out] context     Pointer to arbitrary data to pass to callback function
 *
 * @return true when callback function stop cycle, false if cycle is complete
 */
bool
vci_expression_walker(Plan *plan, bool (*walker) (Node *, void *), void *context)
{
	return expression_walker_core(plan, walker, NULL, NULL, context);
}

/**
 * Helper function that traverse expression tree in plan node without updating it
 * If there is attribut information (AttrNumber) other than Var node included in plan node,
 * attr_cb is executed.
 *
 * @param[in]     plan     Pointer to Plan type struct that holds the plan node to be traversed
 * @param[in]     walker   Callback function to be used in traverse. Returns true to stop cycle.
 * @param[in]     attr_cb  Callback function to be called when attribute (column) other than Var exists
 * @param[in,out] context  Pointer to arbitrary data to pass to callback function
 *
 * @return true when callback function stop cycle, false if cycle is complete
 */
bool
vci_expression_and_colid_walker(Plan *plan, bool (*walker) (Node *, void *), void (*attr_cb) (AttrNumber *, void *), void *context)
{
	return expression_walker_core(plan, walker, walker, attr_cb, context);
}

/**
 * Helper function that traverse expression tree in plan node without updating it
 * Run walker_initplan if there is an initPlan associated with the plan node.
 *
 * @param[in]     plan     Pointer to Plan type struct that holds the plan node to be traversed
 * @param[in]     walker   Callback function to be used in traverse. Returns true to stop cycle.
 * @param[in]     walker_initplan Callbac k function to be used in initPlan traverse
 * @param[in,out] context  Pointer to arbitrary data to pass to callback function
 *
 * @return true when callback function stop cycle, false if cycle is complete
 */
bool
vci_expression_and_initplan_walker(Plan *plan, bool (*walker) (Node *, void *), bool (*walker_initplan) (Node *, void *), void *context)
{
	return expression_walker_core(plan, walker, walker_initplan, NULL, context);
}

static bool
expression_walker_core(Plan *plan, bool (*walker) (Node *, void *), bool (*walker_initplan) (Node *, void *), void (*attr_cb) (AttrNumber *, void *), void *context)
{
	if (walker_initplan)
	{
		if (expression_tree_walker((Node *) plan->initPlan, walker_initplan, context))
			return true;
	}

	switch (nodeTag(plan))
	{
		case T_Result:
			{
				Result	   *result = (Result *) plan;

				if (expression_tree_walker((Node *) result->resconstantqual, walker, context))
					return true;
			}
			break;

		case T_MergeAppend:
			if (attr_cb)
			{
				MergeAppend *merge_append = (MergeAppend *) plan;

				for (int i = 0; i < merge_append->numCols; i++)
					attr_cb(&merge_append->sortColIdx[i], context);

			}
			break;

		case T_RecursiveUnion:
			if (attr_cb)
			{
				RecursiveUnion *recursive_union = (RecursiveUnion *) plan;

				for (int i = 0; i < recursive_union->numCols; i++)
					attr_cb(&recursive_union->dupColIdx[i], context);
			}
			break;

		case T_IndexScan:
			{
				IndexScan  *index_scan = (IndexScan *) plan;

				if (expression_tree_walker((Node *) index_scan->indexqual, walker, context))
					return true;

				if (expression_tree_walker((Node *) index_scan->indexqualorig, walker, context))
					return true;

				if (expression_tree_walker((Node *) index_scan->indexorderby, walker, context))
					return true;

				if (expression_tree_walker((Node *) index_scan->indexorderbyorig, walker, context))
					return true;
			}
			break;

		case T_IndexOnlyScan:
			{
				IndexOnlyScan *index_only_scan = (IndexOnlyScan *) plan;

				if (expression_tree_walker((Node *) index_only_scan->indexqual, walker, context))
					return true;

				if (expression_tree_walker((Node *) index_only_scan->indexorderby, walker, context))
					return true;

				if (expression_tree_walker((Node *) index_only_scan->indextlist, walker, context))
					return true;
			}
			break;

		case T_BitmapIndexScan:
			{
				BitmapIndexScan *bitmap_index_scan = (BitmapIndexScan *) plan;

				if (expression_tree_walker((Node *) bitmap_index_scan->indexqual, walker, context))
					return true;

				if (expression_tree_walker((Node *) bitmap_index_scan->indexqualorig, walker, context))
					return true;
			}
			break;

		case T_BitmapHeapScan:
			{
				BitmapHeapScan *bitmap_heap_scan = (BitmapHeapScan *) plan;

				if (expression_tree_walker((Node *) bitmap_heap_scan->bitmapqualorig, walker, context))
					return true;
			}
			break;

		case T_TidScan:
			{
				TidScan    *tid_scan = (TidScan *) plan;

				if (expression_tree_walker((Node *) tid_scan->tidquals, walker, context))
					return true;
			}
			break;

		case T_TidRangeScan:
			{
				TidRangeScan *tid_range_scan = (TidRangeScan *) plan;

				if (expression_tree_walker((Node *) tid_range_scan->tidrangequals, walker, context))
					return true;
			}
			break;

		case T_FunctionScan:
			{
				FunctionScan *func_scan = (FunctionScan *) plan;

				if (expression_tree_walker((Node *) func_scan->functions, walker, context))
					return true;
			}
			break;

		case T_ValuesScan:
			{
				ValuesScan *values_scan = (ValuesScan *) plan;

				if (expression_tree_walker((Node *) values_scan->values_lists, walker, context))
					return true;
			}
			break;

		case T_CteScan:
			break;

		case T_WorkTableScan:
			break;

		case T_NestLoop:
			{
				NestLoop   *nest_loop = (NestLoop *) plan;
				ListCell   *lc;

				if (expression_tree_walker((Node *) nest_loop->join.joinqual, walker, context))
					return true;

				foreach(lc, nest_loop->nestParams)
				{
					NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);

					if (walker((Node *) nlp->paramval, context))
						return true;
				}
			}
			break;

		case T_Memoize:
			{
				Memoize    *memoize = (Memoize *) plan;

				if (expression_tree_walker((Node *) memoize->param_exprs, walker, context))
					return true;
			}
			break;

		case T_MergeJoin:
			{
				MergeJoin  *merge_join = (MergeJoin *) plan;

				if (expression_tree_walker((Node *) merge_join->join.joinqual, walker, context))
					return true;

				if (expression_tree_walker((Node *) merge_join->mergeclauses, walker, context))
					return true;
			}
			break;

		case T_HashJoin:
			{
				HashJoin   *hash_join = (HashJoin *) plan;

				if (expression_tree_walker((Node *) hash_join->join.joinqual, walker, context))
					return true;

				if (expression_tree_walker((Node *) hash_join->hashclauses, walker, context))
					return true;

				if (expression_tree_walker((Node *) hash_join->hashkeys, walker, context))
					return true;
			}
			break;

		case T_Sort:
			if (attr_cb)
			{
				Sort	   *sort = (Sort *) plan;

				for (int i = 0; i < sort->numCols; i++)
					attr_cb(&sort->sortColIdx[i], context);
			}
			break;

		case T_Group:
			if (attr_cb)
			{
				Group	   *group = (Group *) plan;

				for (int i = 0; i < group->numCols; i++)
					attr_cb(&group->grpColIdx[i], context);
			}
			break;

		case T_Agg:
			if (attr_cb)
			{
				Agg		   *agg = (Agg *) plan;

				for (int i = 0; i < agg->numCols; i++)
					attr_cb(&agg->grpColIdx[i], context);
			}
			break;

		case T_WindowAgg:
			if (attr_cb)
			{
				WindowAgg  *window_agg = (WindowAgg *) plan;

				for (int i = 0; i < window_agg->partNumCols; i++)
					attr_cb(&window_agg->partColIdx[i], context);

				for (int i = 0; i < window_agg->ordNumCols; i++)
					attr_cb(&window_agg->ordColIdx[i], context);
			}
			break;

		case T_Unique:
			if (attr_cb)
			{
				Unique	   *unique = (Unique *) plan;

				for (int i = 0; i < unique->numCols; i++)
					attr_cb(&unique->uniqColIdx[i], context);
			}
			break;

		case T_Hash:
			break;

		case T_SetOp:
			if (attr_cb)
			{
				SetOp	   *setop = (SetOp *) plan;

				for (int i = 0; i < setop->numCols; i++)
					attr_cb(&setop->cmpColIdx[i], context);
			}
			break;

		case T_Limit:
			{
				Limit	   *limit = (Limit *) plan;

				if (expression_tree_walker((Node *) limit->limitOffset, walker, context))
					return true;

				if (expression_tree_walker((Node *) limit->limitCount, walker, context))
					return true;
			}
			break;

		case T_CustomScan:
		case T_CustomPlanMarkPos:
			switch (vci_get_vci_plan_type(plan))
			{
				case VCI_CUSTOMPLAN_SCAN:
				case VCI_CUSTOMPLAN_SORT:
				case VCI_CUSTOMPLAN_AGG:
				case VCI_CUSTOMPLAN_GATHER:
					break;

				default:
					break;
			}
			break;

		case T_ForeignScan:
		case T_ModifyTable:
		case T_LockRows:
			elog(DEBUG1, "unsupported node type: %s(%d)",
				 VciGetNodeName(nodeTag(plan)), (int) nodeTag(plan));
			return true;

		default:
			break;
	}

	if (expression_tree_walker((Node *) plan->qual, walker, context))
		return true;

	if (expression_tree_walker((Node *) plan->targetlist, walker, context))
		return true;

	/* Success */
	return false;
}

/*---------------------------------------------------------------------------*/
/* Plan mutator                                                              */
/*---------------------------------------------------------------------------*/

/**
 * Rewrite each plan node in PlannedStmt according to conditions from mutator
 *
 * @param[in,out] plannedstmt Pointer to PlannedStmt type struct containing plan tree to be rewritten
 * @param[in]     mutator     Callback function to be used in rewrite
 * @param[in]     topmostplan Callback function to be called before parsing Topmost plan
 * @param[in,out] context     Pointer to arbitrary data to pass to callback function
 * @param[in]     eflags      Specify same eflags value as the one passed when plan tree starts ExecutorStart()
 * @param[out]    changed     Write true to *changed if any rewrite is done. Do nothing if not.
 *
 * @return true when callback function stop cycle, false if cycle is complete
 *
 * @note rewrite is executed in in-place
 */
bool
vci_plannedstmt_tree_mutator(PlannedStmt *plannedstmt, vci_mutator_t mutator, vci_topmost_plan_cb_t topmostplan, void *context, int eflags, bool *changed)
{
	int			i;
	ListCell   *l;

	if (plannedstmt == NULL)
		return false;

	i = 1;
	foreach(l, plannedstmt->subplans)
	{
		Plan	  **plan_p = (Plan **) &lfirst(l);

		if (*plan_p == NULL)
			continue;

		if (subplan_mutator(plannedstmt, plan_p, i, mutator, topmostplan, context, eflags, changed))
			return true;

		i++;
	}

	if (plannedstmt->planTree)
	{
		Plan	   *oldplan;
		Plan	   *newplan;

		oldplan = newplan = plannedstmt->planTree;

		if (topmostplan)
			topmostplan(oldplan, 0 /* plan_id */ , context);

		if (mutator(&newplan, NULL, context, eflags, changed))
			return true;

		if (newplan != oldplan)
			plannedstmt->planTree = newplan;

		if (topmostplan)
			topmostplan(newplan, 0 /* plan_id */ , context);
	}

	return false;
}

/**
 * Rewrite plan node in PlannedStmt according to conditions from mutator. But, specify the rewrite order of subplan.
 *
 * @param[in,out] plannedstmt Pointer to PlannedStmt type struct containing plan tree to be rewritten
 * @param[in]     mutator     Callback function to be used in rewrite
 * @param[in]     topmostplan Callback function to be called before parsing Topmost plan
 * @param[in,out] context     Pointer to arbitrary data to pass to callback function
 * @param[in]     eflags      Specify same eflags value as the one passed when plan tree starts ExecutorStart()
 * @param[out]    changed     Write true to *changed if any rewrite is done. Do nothing if not.
 * @param[in]     subplan_order Array of ID in order of subplan to be parsed(including main plan)
 *
 * @return true when callback function stops cycle, false if cycle is complete
 *
 * @note rewrite is executed in in-place
 */
bool
vci_plannedstmt_tree_mutator_order(PlannedStmt *plannedstmt, vci_mutator_t mutator, vci_topmost_plan_cb_t topmostplan, void *context, int eflags, bool *changed, int *subplan_order)
{
	int			i;
	int			max_subplans;
	bool		mainplan_changed = false,
				subplans_changed = false;
	Plan	  **subplan_array;
	List	   *subplans = NIL;
	ListCell   *l;

	if (plannedstmt == NULL)
		return false;

	max_subplans = list_length(plannedstmt->subplans);

	subplan_array = palloc0_array(Plan *, max_subplans);

	i = 0;
	foreach(l, plannedstmt->subplans)
		subplan_array[i++] = (Plan *) lfirst(l);

	for (i = 0; i < max_subplans + 1; i++)
	{
		int			plan_id = subplan_order[i];

		if (plan_id == 0)
		{
			Plan	   *oldplan;
			Plan	   *newplan;

			oldplan = newplan = plannedstmt->planTree;

			if (topmostplan)
				topmostplan(oldplan, 0 /* plan_id */ , context);

			if (mutator(&newplan, NULL, context, eflags, &mainplan_changed))
				return true;

			if (newplan != oldplan)
				plannedstmt->planTree = newplan;

			if (topmostplan)
				topmostplan(newplan, 0 /* plan_id */ , context);
		}
		else
		{
			Plan	  **plan_p = &subplan_array[plan_id - 1];

			if (*plan_p == NULL)
				continue;

			if (subplan_mutator(plannedstmt, plan_p, plan_id, mutator, topmostplan, context, eflags, &subplans_changed))
				return true;
		}
	}

	*changed = mainplan_changed || subplans_changed;

	if (subplans_changed)
	{
		for (i = 0; i < max_subplans; i++)
			subplans = lappend(subplans, subplan_array[i]);

		plannedstmt->subplans = subplans;
	}

	pfree(subplan_array);

	return false;
}

static bool
subplan_mutator(PlannedStmt *plannedstmt, Plan **plan_p, int plan_id, vci_mutator_t mutator, vci_topmost_plan_cb_t topmostplan, void *context, int eflags, bool *changed)
{
	int			sp_eflags;
	Plan	   *oldplan;
	Plan	   *newplan;

	/*
	 * A subplan will never need to do BACKWARD scan nor MARK/RESTORE. If it
	 * is a parameterless subplan (not initplan), we suggest that it be
	 * prepared to handle REWIND efficiently; otherwise there is no need.
	 */
	sp_eflags = eflags
		& (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_WITH_NO_DATA);
	if (bms_is_member(plan_id, plannedstmt->rewindPlanIDs))
		sp_eflags |= EXEC_FLAG_REWIND;

	oldplan = newplan = *plan_p;

	if (topmostplan)
		topmostplan(oldplan, plan_id, context);

	if (mutator(&newplan, NULL, context, sp_eflags, changed))
		return true;

	if (newplan != oldplan)
	{
		*plan_p = (void *) newplan;
		*changed = true;
	}

	if (topmostplan)
		topmostplan(newplan, plan_id, context);

	return false;
}

/**
 * Rewrite nodes under plan
 * (Do not rewrite plan itself)
 *
 * @param[in,out] plan_p   Pointer to a pointer to Plan typepe struct that holds the plan node to be rewritten
 * @param[in]     parent   Parent plan node of plan node to rewrite. NULL is there is no parent.
 * @param[in]     mutator  Callback function to be used in rewrite
 * @param[in,out] context  Pointer to arbitrary data to pass to callback function
 * @param[in]     eflags   eflags value same as the one passed by plan tree in ExecutorStart()
 * @param[out]    changed  Write true to *changed if any rewrite is done. Do nothing if not.
 *
 * @return true when callback function stops cycle, false if cycle is complete
 */
bool
vci_plan_tree_mutator(Plan **plan_p, Plan *parent, vci_mutator_t mutator, void *context, int eflags, bool *changed)
{
	int			eflags_outer,
				eflags_inner;
	Plan	   *plan;

	plan = *plan_p;

	if (plan == NULL)
		return false;

	/*
	 * Determine unsupported plan nodes
	 */
	switch (nodeTag(plan))
	{
		case T_ForeignScan:
		case T_ModifyTable:
		case T_LockRows:
			elog(DEBUG1, "unsupported node type: %s(%d)",
				 VciGetNodeName(nodeTag(plan)), (int) nodeTag(plan));
			return true;
		case T_Agg:
			{
				if ((parent != NULL) && (nodeTag(parent) == T_Gather || nodeTag(parent) == T_GatherMerge))
					return true;	/* If underlying plan is Aggregate then it
									 * skip using VCI as OSS parallel
									 * aggregation is performing better */
			}
			break;

		case T_Gather:
		case T_GatherMerge:

			/*
			 * For parallel aggregates, there will be two aggregate nodes:
			 * partial and final. The Gather node could be in between these
			 * two nodes with a Sort in between. So check if the either the
			 * parent or the child of an Aggregate is a Gather node. for eg:
			 * Finalize Aggregate->Gather->Sort->Partial Aggregate
			 */
			if ((parent != NULL) && (nodeTag(parent) == T_Agg))
				return true;
		default:
			break;
	}

	eflags_outer = eflags_inner = eflags;

	switch (nodeTag(plan))
	{
		case T_Material:
		case T_Sort:
			eflags_outer = eflags_inner = (eflags & ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK));
			break;

		case T_CteScan:
			eflags_outer = eflags_inner = (eflags | EXEC_FLAG_REWIND);
			break;

		case T_MergeJoin:
			eflags_inner = eflags | EXEC_FLAG_MARK;
			break;

		case T_NestLoop:
			if (((NestLoop *) plan)->nestParams == NIL)
				eflags_inner = (eflags | EXEC_FLAG_REWIND);
			else
				eflags_inner = (eflags & ~EXEC_FLAG_REWIND);
			break;

		case T_SetOp:
			if (((SetOp *) plan)->strategy == SETOP_HASHED)
				eflags_outer &= ~EXEC_FLAG_REWIND;
			break;

		default:
			break;
	}

	if (plan_tree_mutator(&plan->lefttree, plan, mutator, context, eflags_outer, changed))
		return true;

	if (plan_tree_mutator(&plan->righttree, plan, mutator, context, eflags_inner, changed))
		return true;

	/*
	 * Process nodes other than lefttree and rightree connected to this plan
	 * node
	 */
	switch (nodeTag(plan))
	{
		case T_Append:
			{
				Append	   *node = (Append *) plan;

				if (plan_list_tree_mutator(&node->appendplans, plan, mutator, context, eflags_outer, changed))
					return true;
			}
			break;

		case T_MergeAppend:
			{
				MergeAppend *node = (MergeAppend *) plan;

				if (plan_list_tree_mutator(&node->mergeplans, plan, mutator, context, eflags_outer, changed))
					return true;
			}
			break;

		case T_BitmapAnd:
			{
				BitmapAnd  *node = (BitmapAnd *) plan;

				if (plan_list_tree_mutator(&node->bitmapplans, plan, mutator, context, eflags_outer, changed))
					return true;
			}
			break;

		case T_BitmapOr:
			{
				BitmapOr   *node = (BitmapOr *) plan;

				if (plan_list_tree_mutator(&node->bitmapplans, plan, mutator, context, eflags_outer, changed))
					return true;
			}
			break;

		case T_SubqueryScan:
			{
				SubqueryScan *node = (SubqueryScan *) plan;

				if (plan_tree_mutator(&node->subplan, plan, mutator, context, eflags_outer, changed))
					return true;
			}
			break;

		default:
			break;
	}

	return false;
}

/**
 * Rewrite edge connected to plan node of interest (*plan_p)
 *
 * @param[in,out] plan_p  Pointer to a pointer to Plan typepe struct that holds the plan node to be rewritten
 * @param[in]     parent  Parent plan node of plan node to rewrite. NULL is there is no parent.
 * @param[in]     mutator Callback function to be used in rewrite
 * @param[in,out] context Pointer to arbitrary data to pass to callback function
 * @param[in]     eflags  eflags value same as the one passed by plan tree in ExecutorStart()
 *
 * @param[out]    changed Write true to *changed if any rewrite is done. Do nothing if not.
 */
static bool
plan_tree_mutator(Plan **plan_p, Plan *parent, vci_mutator_t mutator, void *context, int eflags, bool *changed)
{
	if (*plan_p == NULL)
		return false;

	if (mutator(plan_p, parent, context, eflags, changed))
		return true;

	return false;
}

/**
 * Rewrite plan node list
 *
 * @param[in,out] plan_list Pointer to List type struct that holds list of plan node to be rewritten
 * @param[in]     parent    Parent plan node of plan node list to be rewritten. NULL if no parent.
 * @param[in]     mutator   Callback function to be used in rewrite
 * @param[in,out] context   Pointer to arbitrary data to pass to callback function
 * @param[in]     eflags    eflags value same as the one passed by plan tree in ExecutorStart()
 * @param[out]    changed   Write true to *changed if any rewrite is done. Do nothing if not.
 */
static bool
plan_list_tree_mutator(List **plan_list, Plan *parent, vci_mutator_t mutator, void *context, int eflags, bool *changed)
{
	List	   *newlist = NIL;
	List	   *list = *plan_list;
	ListCell   *lc;
	bool		any_changed = false;

	if (list == NIL)
		return false;

	if (list_length(list) == 0)
		return false;

	foreach(lc, list)
	{
		Plan	   *child = (Plan *) lfirst(lc);

		/*
		 * In case of List of plans, we need to verify any of the list item
		 * has Gather node in top-level plan.i.e.,
		 * Appenedplans->Gather->Parallel Seq scan. If yes, plan tree walker
		 * cannot replace the gather node properly. So, skip re-writing VCI
		 * plan in such scenarios.
		 */
		if (newlist == NIL)		/* Using this just to make this code check
								 * work only for the first time which is what
								 * needed */
		{
			if (nodeTag(child) == T_Gather || nodeTag(child) == T_GatherMerge)
				return true;
		}

		if (plan_tree_mutator(&child, parent, mutator, context, eflags, &any_changed))
			return true;

		newlist = lappend(newlist, child);
	}

	if (any_changed)
	{
		*plan_list = newlist;
		*changed = true;
	}
	else
	{
		list_free(newlist);
	}

	return false;
}
