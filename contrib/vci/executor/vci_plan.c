/*-------------------------------------------------------------------------
 *
 * vci_plan.c
 *	  Common processing for VCI plan nodes
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/instrument.h"
#include "executor/nodeSubplan.h"
#include "nodes/bitmapset.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"	/* for makeVarFromTargetEntry() */
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"

#include "vci.h"
#include "vci_executor.h"

static VciScan *search_scan(Plan *node, AttrNumber scan_plan_no);
static VciScanState *search_scan_state(PlanState *node, Plan *target);

/**
 * Determine if given plan node is CustomPlan
 *
 * @param[in] plan plan node
 * @return true if CustomPlan, else false
 */
bool
vci_is_custom_plan(Plan *plan)
{
	NodeTag		type;

	type = nodeTag(plan);

	if ((type == T_CustomScan) || (type == T_CustomPlanMarkPos))
		return true;

	return false;
}

/**
 * Returns type of VCI plan node with VCI_CUSTOMPLAN_XXX macro
 *
 * @param[in] plan plan node
 * @retval 0     not VCI plan node
 * @retval non 0  is a VCI plan node
 */
int
vci_get_vci_plan_type(Plan *plan)
{
	if (plan == NULL)
		return 0;

	if (!vci_is_custom_plan(plan))
		return 0;

	return ((CustomScan *) plan)->flags & VCI_CUSTOMPLAN_MASK;
}

/**
 * Copy only the basic part of the VCI-derived plan nodes given to src to dest.
 *
 * @param[out]    dest Copy destination
 * @param[in]     src  Copy source
 */
void
vci_copy_plan(VciPlan *dest, const VciPlan *src)
{
	dest->scan_plan_no = src->scan_plan_no;

	/* Do not copy scan_cached */
	dest->scan_cached = NULL;
}

/**
 * Search and return VCI Scan node that is the source of data input for the VCI plan node
 *
 * @param[in]     node Pointer to the VCI plan that serves as search starting point
 * @return Pointer to VCI Scan plan
 */
VciScan *
vci_search_scan(VciPlan *node)
{
	AttrNumber	scan_plan_no;
	VciScan    *result;

	if (node->scan_cached)
		return node->scan_cached;

	scan_plan_no = node->scan_plan_no;
	if (scan_plan_no == 0)
		return NULL;

	result = search_scan(&node->cscan.scan.plan, scan_plan_no);

	if (node->scan_cached == NULL)
		node->scan_cached = result;

	return result;
}

/**
 * Subroutine for vci_search_scan()
 *
 * Recursively descend and search for VCI Scan nodes.
 */
static VciScan *
search_scan(Plan *node, AttrNumber scan_plan_no)
{
	if (node->plan_no == scan_plan_no)
		return (VciScan *) node;

	if (outerPlan(node))
	{
		VciScan    *result = search_scan(outerPlan(node), scan_plan_no);

		if (result != NULL)
			return result;
	}

	if (innerPlan(node))
	{
		VciScan    *result = search_scan(innerPlan(node), scan_plan_no);

		if (result != NULL)
			return result;
	}

	/*
	 * Some types of plan nodes have plans other than outerPlan and innerPlan,
	 * but they do not contain VCI Scan nodes.
	 */

	return NULL;
}

/**
 * Search and return VCI Scan State node that is the source of data input for the VCI plan state node
 *
 * @param[in]     node Pointer to the VCI plan state node that serves as search starting point
 * @return Pointer to VCI Scan plan state node
 */
VciScanState *
vci_search_scan_state(VciPlanState *node)
{
	VciScan    *scan;
	VciScanState *result;

	if (node->scanstate_cached)
		return node->scanstate_cached;

	scan = vci_search_scan((VciPlan *) node->css.ss.ps.plan);
	if (scan == NULL)
		return NULL;

	result = search_scan_state(&node->css.ss.ps, &scan->vci.cscan.scan.plan);

	if (node->scanstate_cached == NULL)
		node->scanstate_cached = result;

	return result;
}

/**
 * Subroutine for vci_search_scan_state()
 *
 * Recursively descend and search for VCI Scan state nodes.
 */
static VciScanState *
search_scan_state(PlanState *node, Plan *target)
{
	if (node->plan == target)
	{
		Assert(node->type == T_CustomScanState);
		return (VciScanState *) node;
	}

	if (outerPlanState(node))
	{
		VciScanState *result = search_scan_state(outerPlanState(node), target);

		if (result != NULL)
			return result;
	}

	if (innerPlanState(node))
	{
		VciScanState *result = search_scan_state(innerPlanState(node), target);

		if (result != NULL)
			return result;
	}

	/*
	 * Depending on the type of Plan State, some may have Plan States other
	 * than outerPlanState and innerPlanState, but they do not have VCI Scan
	 * State.
	 */

	return NULL;
}

/**
 * Create a target list that pass through the lower nodes required for
 * Materialize node.
 *
 * @param[in] targetlist target list
 * @return created pass through target list
 */
List *
vci_generate_pass_through_target_list(List *targetlist)
{
	List	   *new_targetlist = NIL;
	ListCell   *lc;

	foreach(lc, targetlist)
	{
		TargetEntry *src_tle = (TargetEntry *) lfirst(lc);
		TargetEntry *new_tle;

		new_tle = makeNode(TargetEntry);

		*new_tle = *src_tle;
		new_tle->expr = (Expr *) makeVarFromTargetEntry(OUTER_VAR, src_tle);

		new_targetlist = lappend(new_targetlist, new_tle);
	}

	return new_targetlist;
}
