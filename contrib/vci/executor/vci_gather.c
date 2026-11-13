/*-------------------------------------------------------------------------
 *
 * vci_gather.c
 *	  Routines to handle VCI Gather nodes
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_gather.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/nodeCustom.h"

#include "vci.h"
#include "vci_executor.h"
#include "vci_utils.h"

/*
 * Declarations of Custom Plan Methods callbacks
 */
static void vci_gather_BeginCustomPlan(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *vci_gather_ExecCustomPlan(CustomScanState *node);
static void vci_gather_EndCustomPlan(CustomScanState *node);
static void vci_gather_ReScanCustomPlan(CustomScanState *node);
static void vci_gather_MarkPosCustomPlan(CustomScanState *cpstate);
static void vci_gather_RestrPosCustomPlan(CustomScanState *cpstate);

static CustomScan *vci_gather_CopyCustomPlan(const CustomScan *_from);

static Node *
vci_gather_CreateCustomScanState(CustomScan *cs)
{
	VciGather  *vgather;
	VciGatherState *vgs = palloc0_object(VciGatherState);

	vgather = (VciGather *) cs;

	vgs->vci.css.ss.ps.type = T_CustomScanState;
	vgs->vci.css.ss.ps.plan = (Plan *) vgather;
	vgs->vci.css.flags = cs->flags;
	vgs->vci.css.methods = &vci_gather_exec_methods;

	return (Node *) vgs;
}

static void
vci_gather_BeginCustomPlan(CustomScanState *node, EState *estate, int eflags)
{
	VciGather  *gather;
	VciGatherState *gatherstate;

	gather = (VciGather *) node->ss.ps.plan;

	/*
	 * create state structure
	 */
	gatherstate = (VciGatherState *) node;

	gatherstate->vci.css.ss.ps.state = estate;

	/* create expression context for node */
	ExecAssignExprContext(estate, &gatherstate->vci.css.ss.ps);

	outerPlanState(gatherstate) = ExecInitNode(outerPlan(gather), estate, eflags);

	ExecInitResultTupleSlotTL(&gatherstate->vci.css.ss.ps, &TTSOpsVirtual);
}

static TupleTableSlot *
vci_gather_ExecCustomPlan(CustomScanState *cstate)
{
	VciGatherState *gatherstate = (VciGatherState *) cstate;

	return ExecProcNode(outerPlanState(gatherstate));
}

static void
vci_gather_EndCustomPlan(CustomScanState *node)
{
	VciGatherState *gatherstate = (VciGatherState *) node;

	/* clean out the tuple table */
	ExecClearTuple(gatherstate->vci.css.ss.ps.ps_ResultTupleSlot);

	ExecEndNode(outerPlanState(node));
}

static void
vci_gather_ReScanCustomPlan(CustomScanState *node)
{
	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->ss.ps.lefttree->chgParam == NULL)
		ExecReScan(node->ss.ps.lefttree);
}

static void
vci_gather_MarkPosCustomPlan(CustomScanState *node)
{
	elog(PANIC, "VCI Gather does not support MarkPosCustomPlan call convention");
}

/* LCOV_EXCL_START */

static void
vci_gather_RestrPosCustomPlan(CustomScanState *node)
{
	elog(PANIC, "VCI Gather does not support RestrPosCustomPlan call convention");
}

/* LCOV_EXCL_STOP */

static CustomScan *
vci_gather_CopyCustomPlan(const CustomScan *_from)
{
	const VciGather *from = (const VciGather *) _from;
	VciGather  *newnode;

	newnode = palloc0_object(VciGather);

	vci_copy_plan(&newnode->vci, &from->vci);

	((Node *) newnode)->type = nodeTag((Node *) from);

	return &newnode->vci.cscan;
}

CustomScanMethods vci_gather_scan_methods = {
	"VCI Gather",
	vci_gather_CreateCustomScanState,
	vci_gather_CopyCustomPlan,
};

CustomExecMethods vci_gather_exec_methods = {
	"VCI Gather",
	vci_gather_BeginCustomPlan,
	vci_gather_ExecCustomPlan,
	vci_gather_EndCustomPlan,
	vci_gather_ReScanCustomPlan,
	vci_gather_MarkPosCustomPlan,
	vci_gather_RestrPosCustomPlan,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};
