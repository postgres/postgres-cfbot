/*-------------------------------------------------------------------------
 *
 * nodeGraphScan.c
 *	  Routines to handle graph scan nodes.
 *
 * The full graph scan executor (variable-length hop traversal) will be
 * implemented together with the hop machinery; for now this only supports
 * EXPLAIN: the plan is initialized so it can be displayed, but actually
 * executing it raises a "not yet implemented" error.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGraphScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeGraphScan.h"
#include "miscadmin.h"
#include "parser/parse_target.h"

static TupleTableSlot *ExecGraphScan(PlanState *pstate);

static TupleTableSlot *
ExecGraphScan(PlanState *pstate)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("graph scan execution is not yet implemented")));
	return NULL;				/* keep compiler quiet */
}

GraphScanState *
ExecInitGraphScan(GraphScan * node, EState *estate, int eflags)
{
	GraphScanState *scanstate;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * GraphScan should not have any "normal" children
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(GraphScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecGraphScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * initialize inner (single quantified hop) plan as a nested child
	 */
	if (node->inner_plan != NULL)
		scanstate->inner_plan = ExecInitNode(node->inner_plan, estate, eflags);

	/*
	 * Initialize scan slot.  There is no heap relation to describe it, so we
	 * build a virtual slot whose descriptor comes from the scan's own
	 * targetlist; the result slot is built from the targetlist by
	 * ExecInitResultTypeTL.
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  ExecTypeFromTL(node->scan.plan.targetlist),
						  &TTSOpsVirtual, 0);

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	return scanstate;
}

void
ExecEndGraphScan(GraphScanState * node)
{
	if (node->inner_plan)
		ExecEndNode(node->inner_plan);

	/*
	 * XXX: ExecFreeExprContext is not called here on purpose; the expr
	 * context is freed by the parent scan code.  The scan slot and result
	 * slot are cleaned up by ExecEndPlan.
	 */
}

void
ExecReScanGraphScan(GraphScanState * node)
{
	ExecScanReScan(&node->ss);

	if (node->inner_plan)
	{
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(node->inner_plan, node->ss.ps.chgParam);

		if (node->inner_plan->chgParam == NULL)
			ExecReScan(node->inner_plan);
	}
}
