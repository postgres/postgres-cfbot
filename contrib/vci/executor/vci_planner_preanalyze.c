/*-------------------------------------------------------------------------
 *
 * vci_planner_preanalyze.c
 *	  Preprocessing for plan rewrite routine
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_planner_preanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdlib.h>				/* for qsort() */

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_namespace.h"	/* for PG_PUBLIC_NAMESPACE */
#include "catalog/pg_proc.h"	/* for ProcedureRelationId, Form_pg_proc */
#include "catalog/pg_type.h"	/* for BOOLOID */
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"
#include "utils/syscache.h"

#include "vci.h"

#include "vci_mem.h"
#include "vci_executor.h"
#include "vci_utils.h"
#include "vci_planner.h"

static bool preanalyze_plan_tree_mutator(Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed);
static bool collect_data_in_expression(Node *node, void *context);
static bool collect_data_in_initplan(Node *node, void *context);

static bool isGatherExists;

/**
 * Analysis before plan rewrite
 *
 * @param[in]     target     Pointer to PlannedStmt holding the query
 * @param[in,out] rp_context Pointer to plan rewrite information
 * @param[in]     eflags     execution flags
 *
 * @return true when callback function stops cycle, false if cycle is complete
 */
bool
vci_preanalyze_plan_tree(PlannedStmt *target, vci_rewrite_plan_context_t *rp_context, int eflags, bool *isGather)
{
	bool		dummy;
	int			nParamExec;

	/*
	 * Scans target's plan tree and gathers information. Use
	 * vci_plannedstmt_tree_mutator () instead of vci_plannedstmt_tree_walker
	 * () because target cannot be written but eflags information is collected
	 * for plan nodes.
	 */
	if (vci_plannedstmt_tree_mutator(target, preanalyze_plan_tree_mutator, vci_register_plan_id, rp_context, eflags, &dummy))
	{
		*isGather = isGatherExists;
		return true;
	}

	nParamExec = list_length(target->paramExecTypes);
	for (int i = 0; i < nParamExec; i++)
	{
		rp_context->param_exec_attr_map[i].num_def_plans = bms_num_members(rp_context->param_exec_attr_map[i].def_plan_nos);
		rp_context->param_exec_attr_map[i].num_use_plans = bms_num_members(rp_context->param_exec_attr_map[i].use_plan_nos);
	}
	*isGather = isGatherExists;
	return false;
}

/**
 * Callback function to record Topmost plan node and subplan number
 *
 * @param[in]     plan     Topmost plan node
 * @param[in]     plan_id  subplan number
 * @param[in,out] context  Pointer to plan rewrite information
 *
 * This function specifies topmostplan for vci_plannedstmt_tree_mutator().
 */
void
vci_register_plan_id(Plan *plan, int plan_id, void *context)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;

	rp_context->current_plan_id = plan_id;

	rp_context->subplan_attr_map[plan_id].topmostplan = plan;
}

/**
 * Analysis before plan rewrite
 *
 * @param[in]     plan_p   Pointer to a pointer to plan node
 * @param[in]     parent   Pointer to  plan node that is the parent of *plan_p
 * @param[in,out] context  Pointer to plan rewrite information
 * @param[in]     eflags   execution flags
 * @param[out]    changed  Set true when plan tree has been rewritten
 *
 * @return true when callback function stops cycle, false if cycle is complete
 *
 * This function is specified as mutator for vci_plannedstmt_tree_mutator().
 * Since the plan tree is not rewritten, nothing is written to *changed.
 */
static bool
preanalyze_plan_tree_mutator(Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;
	Plan	   *plan;
	AttrNumber	plan_no;
	bool		saved_forbid_parallel_exec;
	bool		result;

	plan = *plan_p;

	Assert(plan->plan_no == 0);
	plan_no = plan->plan_no = ++rp_context->last_plan_no;

	/* If the capacity of plan_attr_map[] is insufficient, double it */
	vci_expand_plan_attr_map(rp_context);

	rp_context->plan_attr_map[plan_no].preset_eflags = eflags;

	rp_context->current_plan_no = plan_no;

	/*
	 * Investigate plan nodes that prohibit parallel execution Set
	 * rp_context->forbid_parallel_exec to false and scan subplan tree
	 */
	saved_forbid_parallel_exec = rp_context->forbid_parallel_exec;
	rp_context->forbid_parallel_exec = false;

	/* Scan expression tree included in plan and collect data */
	vci_expression_and_initplan_walker(plan, collect_data_in_expression, collect_data_in_initplan, context);

	switch (nodeTag(plan))
	{
		case T_SubqueryScan:	/* Since using VCI custom scan for initplans
								 * slows down the performance, block VCI scan
								 * to be replaced for subquery scan */
			return true;
		case T_ModifyTable:
		case T_TidScan:
		case T_TidRangeScan:
		case T_FunctionScan:
		case T_ValuesScan:
		case T_CteScan:
		case T_ForeignScan:
		case T_CustomScan:
		case T_CustomPlanMarkPos:
		case T_LockRows:
			rp_context->forbid_parallel_exec = true;
			break;
		case T_Gather:
		case T_GatherMerge:

			/*
			 * Verify the targetlist of Gather node and underlying node is
			 * same or not. VCI scan replacement assumes Gather node and
			 * underlying node has same targetlist. But, in some scenarios it
			 * is not the case. So, avoid rewriting VCI plan where Gather node
			 * has different targetlist than underlying node. E.g: SELECT c2,
			 * (select key from testtable1 where key=1 ) FROM testtable2 where
			 * c1 = 1 limit 1.
			 */

			if (list_length(plan->targetlist) != list_length(plan->lefttree->targetlist))
				return true;

			/*
			 * Set the flag to verify the presence of Gather node in current
			 * query plan generated by OSS. If there are no Gather nodes
			 * present, then the step to update the query plan to remove
			 * Gather node can be skipped. This way unnecessary recursive
			 * function calls to remove Gather nodes will be skipped when
			 * there are no Gather plan exists in query plan
			 */

			isGatherExists = true;
			break;
		case T_NestLoop:
			{
				NestLoop   *nl;
				ListCell   *lc;

				nl = (NestLoop *) plan;

				foreach(lc, nl->nestParams)
				{
					NestLoopParam *nlp = (NestLoopParam *) lfirst(lc);
					int			paramid = nlp->paramno;

					if ((rp_context->param_exec_attr_map[paramid].type != VCI_PARAM_EXEC_UNKNOWN) &&
						(rp_context->param_exec_attr_map[paramid].type != VCI_PARAM_EXEC_NESTLOOP))
						return true;

					rp_context->param_exec_attr_map[paramid].type = VCI_PARAM_EXEC_NESTLOOP;
					rp_context->param_exec_attr_map[paramid].def_plan_nos =
						bms_add_member(rp_context->param_exec_attr_map[paramid].def_plan_nos, plan_no);
					rp_context->plan_attr_map[plan_no].def_param_ids =
						bms_add_member(rp_context->plan_attr_map[plan_no].def_param_ids, paramid);

					if (bms_is_member(paramid, rp_context->plan_attr_map[plan_no].use_param_ids))
						return true;
				}
			}
			break;

		default:
			break;
	}

	rp_context->current_plan_no = 0;

	result = vci_plan_tree_mutator(plan_p, parent, preanalyze_plan_tree_mutator, context, eflags, changed);

	rp_context->plan_attr_map[plan_no].plan_compat = rp_context->forbid_parallel_exec ? VCI_PLAN_COMPAT_FORBID_TYPE : VCI_PLAN_COMPAT_OK;
	rp_context->forbid_parallel_exec |= saved_forbid_parallel_exec;

	return result;
}

/**
 * Search expression tree and collect data related to PARAM_EXEC type Param
 * and subquery calls.
 */
static bool
collect_data_in_expression(Node *node, void *context)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;
	AttrNumber	plan_no;

	if (node == NULL)
		return false;

	plan_no = rp_context->current_plan_no;

	switch (nodeTag(node))
	{
		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;
				ListCell   *lc;

				if ((rp_context->subplan_attr_map[subplan->plan_id].type != VCI_SUBPLAN_UNKNOWN) &&
					(rp_context->subplan_attr_map[subplan->plan_id].type != VCI_SUBPLAN_SUBPLAN))
					return true;

				rp_context->subplan_attr_map[subplan->plan_id].type = VCI_SUBPLAN_SUBPLAN;
				rp_context->subplan_attr_map[rp_context->current_plan_id].plan_ids =
					bms_add_member(rp_context->subplan_attr_map[rp_context->current_plan_id].plan_ids, subplan->plan_id);

				foreach(lc, subplan->parParam)
				{
					int			paramid = lfirst_int(lc);

					if ((rp_context->param_exec_attr_map[paramid].type != VCI_PARAM_EXEC_UNKNOWN) &&
						(rp_context->param_exec_attr_map[paramid].type != VCI_PARAM_EXEC_SUBPLAN))
						return true;

					rp_context->param_exec_attr_map[paramid].type = VCI_PARAM_EXEC_SUBPLAN;
					rp_context->param_exec_attr_map[paramid].def_plan_nos =
						bms_add_member(rp_context->param_exec_attr_map[paramid].def_plan_nos, plan_no);
					rp_context->plan_attr_map[plan_no].def_param_ids =
						bms_add_member(rp_context->plan_attr_map[plan_no].def_param_ids, paramid);
				}

				return expression_tree_walker((Node *) subplan->args, collect_data_in_expression, context);
			}

		case T_Param:
			{
				Param	   *param = (Param *) node;

				if (param->paramkind == PARAM_EXEC)
				{
					int			paramid = param->paramid;

					rp_context->param_exec_attr_map[paramid].use_plan_nos =
						bms_add_member(rp_context->param_exec_attr_map[paramid].use_plan_nos, plan_no);
					rp_context->plan_attr_map[plan_no].use_param_ids =
						bms_add_member(rp_context->plan_attr_map[plan_no].use_param_ids, paramid);

					if (rp_context->param_exec_attr_map[paramid].type == VCI_PARAM_EXEC_INITPLAN)
					{
						rp_context->param_exec_attr_map[paramid].def_plan_nos =
							bms_add_member(rp_context->param_exec_attr_map[paramid].def_plan_nos, plan_no);
						rp_context->plan_attr_map[plan_no].def_param_ids =
							bms_add_member(rp_context->plan_attr_map[plan_no].def_param_ids, paramid);
					}
				}
			}
			return false;

		default:
			break;
	}

	return expression_tree_walker(node, collect_data_in_expression, context);
}

/**
 * Search for initPlan and analyze SubPlan
 */
static bool
collect_data_in_initplan(Node *node, void *context)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;

	if (node == NULL)
		return false;

	if (IsA(node, SubPlan))
	{
		SubPlan    *subplan = (SubPlan *) node;
		ListCell   *lc;

		if ((rp_context->subplan_attr_map[subplan->plan_id].type != VCI_SUBPLAN_UNKNOWN) &&
			(rp_context->subplan_attr_map[subplan->plan_id].type != VCI_SUBPLAN_INITPLAN))
			return true;

		rp_context->subplan_attr_map[subplan->plan_id].type = VCI_SUBPLAN_INITPLAN;
		rp_context->subplan_attr_map[rp_context->current_plan_id].plan_ids =
			bms_add_member(rp_context->subplan_attr_map[rp_context->current_plan_id].plan_ids, subplan->plan_id);

		foreach(lc, subplan->setParam)
		{
			int			paramid = lfirst_int(lc);

			if ((rp_context->param_exec_attr_map[paramid].type != VCI_PARAM_EXEC_UNKNOWN) &&
				(rp_context->param_exec_attr_map[paramid].type != VCI_PARAM_EXEC_INITPLAN))
				return true;

			rp_context->param_exec_attr_map[paramid].type = VCI_PARAM_EXEC_INITPLAN;

			rp_context->param_exec_attr_map[paramid].plan_id = subplan->plan_id;
		}

		return false;
	}

	return expression_tree_walker(node, collect_data_in_initplan, context);
}

/**
 * Expand array of analysis data for each plan node as necessary
 */
void
vci_expand_plan_attr_map(vci_rewrite_plan_context_t *rp_context)
{
	if (rp_context->max_plan_attrs <= rp_context->last_plan_no)
	{
		int			old_max_plan_attrs = rp_context->max_plan_attrs;
		vci_plan_attr_t *old_plan_attr_map = rp_context->plan_attr_map;

		rp_context->max_plan_attrs *= 2;
		rp_context->plan_attr_map = palloc0_array(vci_plan_attr_t, rp_context->max_plan_attrs);

		for (int i = 0; i < old_max_plan_attrs; i++)
			rp_context->plan_attr_map[i] = old_plan_attr_map[i];

		pfree(old_plan_attr_map);
	}
}

vci_inner_plan_type_t
vci_get_inner_plan_type(vci_rewrite_plan_context_t *context, const Plan *plan)
{
	Assert(plan->plan_no > 0);

	return context->plan_attr_map[plan->plan_no].plan_type;
}

AttrNumber
vci_get_inner_scan_plan_no(vci_rewrite_plan_context_t *context, const Plan *plan)
{
	Assert(plan->plan_no > 0);

	return context->plan_attr_map[plan->plan_no].scan_plan_no;
}

void
vci_set_inner_plan_type_and_scan_plan_no(vci_rewrite_plan_context_t *context, Plan *plan, vci_inner_plan_type_t plan_type, AttrNumber scan_plan_no)
{
	Assert(plan->plan_no > 0);

	context->plan_attr_map[plan->plan_no].plan_type = plan_type;
	context->plan_attr_map[plan->plan_no].scan_plan_no = scan_plan_no;
}
