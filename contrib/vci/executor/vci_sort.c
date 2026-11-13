/*-------------------------------------------------------------------------
 *
 * vci_sort.c
 *	  Routines to handle VCI Agg nodes
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_sort.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "utils/tuplesort.h"

#include "vci.h"
#include "vci_executor.h"
#include "vci_mem.h"

/* ----------------
 *   VCI Sort information
 * ----------------
 */
static Node *
vci_sort_CreateCustomScanState(CustomScan *cs)
{
	VciSort    *vsort;
	VciSortState *vss = palloc0_object(VciSortState);

	vsort = (VciSort *) cs;

	vss->vci.css.ss.ps.type = T_CustomScanState;
	vss->vci.css.ss.ps.plan = (Plan *) vsort;
	vss->vci.css.flags = cs->flags;
	vss->vci.css.methods = &vci_sort_exec_methods;

	return (Node *) vss;
}

static TupleTableSlot *
vci_sort_ExecCustomPlan(CustomScanState *node)
{
	EState	   *estate;
	ScanDirection dir;
	Tuplesortstate *tuplesortstate;
	TupleTableSlot *slot;
	VciSortState *sortstate;

	sortstate = (VciSortState *) node;

	SO1_printf("ExecCustomSort: %s\n",
			   "entering routine");

	estate = sortstate->vci.css.ss.ps.state;
	dir = estate->es_direction;
	tuplesortstate = (Tuplesortstate *) sortstate->tuplesortstate;

	if (!sortstate->sort_Done)
	{
		PlanState  *outerNode;

		SO1_printf("ExecCustomSort: %s\n",
				   "custom sorting subplan");

		SO1_printf("ExecCustomSort: %s\n",
				   "calling tuplesort_begin");

		outerNode = outerPlanState(node);

		tuplesortstate = vci_sort_exec_top_half(sortstate);

		for (;;)
		{
			VciScanState *scanstate = (VciScanState *) outerNode;
			Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);
			slot = VciExecProcScanTuple(scanstate);

			if (TupIsNull(slot))
				break;

			tuplesort_puttupleslot(tuplesortstate, slot);
		}

		vci_sort_perform_sort(sortstate);

		sortstate->sort_Done = true;
		sortstate->bounded_Done = sortstate->bounded;
		sortstate->bound_Done = sortstate->bound;

		SO1_printf("ExecCustomSort: %s\n", "sorting done");
	}

	SO1_printf("ExecCustomSort: %s\n",
			   "retrieving tuple from tuplesort");

	slot = sortstate->vci.css.ss.ps.ps_ResultTupleSlot;

	tuplesort_gettupleslot(tuplesortstate,
						   ScanDirectionIsForward(dir), false,
						   slot, NULL);
	return slot;
}

Tuplesortstate *
vci_sort_exec_top_half(VciSortState *sortstate)
{
	EState	   *estate;
	Tuplesortstate *tuplesortstate;
	VciSort    *plannode = (VciSort *) sortstate->vci.css.ss.ps.plan;
	PlanState  *outerNode;
	TupleDesc	tupDesc;
	int			tuplesortopts = TUPLESORT_NONE;

	estate = sortstate->vci.css.ss.ps.state;
	sortstate->saved_dir = estate->es_direction;
	tuplesortstate = (Tuplesortstate *) sortstate->tuplesortstate;

	estate->es_direction = ForwardScanDirection;

	outerNode = outerPlanState(sortstate);
	tupDesc = ExecGetResultType(outerNode);

	if (sortstate->randomAccess)
		tuplesortopts |= TUPLESORT_RANDOMACCESS;
	if (sortstate->bounded)
		tuplesortopts |= TUPLESORT_ALLOWBOUNDED;

	tuplesortstate = tuplesort_begin_heap(tupDesc,
										  plannode->numCols,
										  plannode->sortColIdx,
										  plannode->sortOperators,
										  plannode->collations,
										  plannode->nullsFirst,
										  work_mem,
										  NULL,
										  tuplesortopts);

	if (sortstate->bounded)
		tuplesort_set_bound(tuplesortstate, sortstate->bound);

	sortstate->tuplesortstate = (void *) tuplesortstate;

	return tuplesortstate;
}

void
vci_sort_perform_sort(VciSortState *sortstate)
{
	EState	   *estate;
	Tuplesortstate *tuplesortstate;

	estate = sortstate->vci.css.ss.ps.state;
	tuplesortstate = (Tuplesortstate *) sortstate->tuplesortstate;

	tuplesort_performsort(tuplesortstate);

	estate->es_direction = sortstate->saved_dir;
}

static void
vci_sort_BeginCustomPlan(CustomScanState *node, EState *estate, int eflags)
{
	VciSort    *sort;
	VciSortState *sortstate;

	SO1_printf("vci_sort_BeginCustomPlan: %s\n",
			   "initializing custom sort node");

	sort = (VciSort *) node->ss.ps.plan;

	/*
	 * create state structure
	 */
	sortstate = (VciSortState *) node;

	sortstate->vci.css.ss.ps.state = estate;

	sortstate->randomAccess = (eflags & (EXEC_FLAG_REWIND |
										 EXEC_FLAG_BACKWARD |
										 EXEC_FLAG_MARK)) != 0;

	sortstate->bounded = false;
	sortstate->sort_Done = false;
	sortstate->tuplesortstate = NULL;

	/*
	 * initialize child nodes
	 *
	 * We shield the child node from the need to support REWIND, BACKWARD, or
	 * MARK/RESTORE.
	 */

	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlanState(sortstate) = ExecInitNode(outerPlan(sort), estate, eflags);

	/*
	 * Initialize scan slot and type.
	 */
	ExecCreateScanSlotFromOuterPlan(estate, &sortstate->vci.css.ss, &TTSOpsVirtual);

	/*
	 * Initialize return slot and type. No need to initialize projection info
	 * because this node doesn't do projections.
	 */
	ExecInitResultTupleSlotTL(&sortstate->vci.css.ss.ps, &TTSOpsMinimalTuple);
	sortstate->vci.css.ss.ps.ps_ProjInfo = NULL;

	SO1_printf("vci_sort_BeginCustomPlan: %s\n",
			   "sort node initialized");
}

static void
vci_sort_EndCustomPlan(CustomScanState *node)
{
	VciSortState *sortstate;

	sortstate = (VciSortState *) node;

	SO1_printf("ExecEndSort: %s\n",
			   "shutting down custom sort node");

	ExecClearTuple(sortstate->vci.css.ss.ss_ScanTupleSlot);
	ExecClearTuple(sortstate->vci.css.ss.ps.ps_ResultTupleSlot);

	if (sortstate->tuplesortstate != NULL)
		tuplesort_end((Tuplesortstate *) sortstate->tuplesortstate);

	sortstate->tuplesortstate = NULL;

	ExecEndNode(outerPlanState(sortstate));

	SO1_printf("ExecEndSort: %s\n",
			   "VCI Sort node shutdown");
}

static void
vci_sort_ReScanCustomPlan(CustomScanState *node)
{
	VciSortState *sortstate;

	sortstate = (VciSortState *) node;

	if (!sortstate->sort_Done)
		return;

	ExecClearTuple(sortstate->vci.css.ss.ps.ps_ResultTupleSlot);

	if (sortstate->vci.css.ss.ps.lefttree->chgParam != NULL ||
		sortstate->bounded != sortstate->bounded_Done ||
		sortstate->bound != sortstate->bound_Done ||
		!sortstate->randomAccess)
	{
		sortstate->sort_Done = false;
		tuplesort_end((Tuplesortstate *) sortstate->tuplesortstate);
		sortstate->tuplesortstate = NULL;

		if (sortstate->vci.css.ss.ps.lefttree->chgParam == NULL)
			ExecReScan(sortstate->vci.css.ss.ps.lefttree);
	}
	else
		tuplesort_rescan((Tuplesortstate *) sortstate->tuplesortstate);
}

/* LCOV_EXCL_START */

static void
vci_sort_MarkPosCustomPlan(CustomScanState *node)
{

	elog(PANIC, "VCI Sort does not support MarkPosCustomPlan call convention");

}

static void
vci_sort_RestrPosCustomPlan(CustomScanState *node)
{
	elog(PANIC, "VCI Sort does not support RestrPosCustomPlan call convention");
}

/* LCOV_EXCL_STOP */

static void
vci_sort_ExplainCustomPlan(CustomScanState *csstate,
						   List *ancestors,
						   ExplainState *es)
{
	VciSortState *sortstate = (VciSortState *) csstate;
	VciSort    *sort = (VciSort *) csstate->ss.ps.plan;

	ExplainPropertySortGroupKeys(&csstate->ss.ps, "Sort Key",
								 sort->numCols, sort->sortColIdx,
								 ancestors, es);

	if (es->analyze && sortstate->sort_Done &&
		sortstate->tuplesortstate != NULL)
	{
		Tuplesortstate *state = (Tuplesortstate *) sortstate->tuplesortstate;
		TuplesortInstrumentation stats;
		const char *sortMethod;
		const char *spaceType;
		int64		spaceUsed;

		tuplesort_get_stats(state, &stats);
		sortMethod = tuplesort_method_name(stats.sortMethod);
		spaceType = tuplesort_space_type_name(stats.spaceType);
		spaceUsed = stats.spaceUsed;

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Sort Method: %s  %s: " INT64_FORMAT "kB\n",
							 sortMethod, spaceType, spaceUsed);
		}
		else
		{
			ExplainPropertyText("Sort Method", sortMethod, es);
			ExplainPropertyInteger("Sort Space Used", "kB", spaceUsed, es);
			ExplainPropertyText("Sort Space Type", spaceType, es);
		}
	}
}

static CustomScan *
vci_sort_CopyCustomPlan(const CustomScan *_from)
{
	const VciSort *from = (const VciSort *) _from;
	VciSort    *newnode = palloc0_object(VciSort);
	int			numCols;

	vci_copy_plan(&newnode->vci, &from->vci);

	numCols = from->numCols;

	newnode->numCols = numCols;

	if (numCols > 0)
	{
		newnode->sortColIdx = palloc_array(AttrNumber, numCols);
		newnode->sortOperators = palloc_array(Oid, numCols);
		newnode->collations = palloc_array(Oid, numCols);
		newnode->nullsFirst = palloc_array(bool, numCols);

		for (int i = 0; i < numCols; i++)
		{
			newnode->sortColIdx[i] = from->sortColIdx[i];
			newnode->sortOperators[i] = from->sortOperators[i];
			newnode->collations[i] = from->collations[i];
			newnode->nullsFirst[i] = from->nullsFirst[i];
		}
	}

	((Node *) newnode)->type = nodeTag((Node *) from);

	return &newnode->vci.cscan;
}

static void
vci_sort_SetBoundCustomScan(const LimitState *node, CustomScanState *css)
{
	VciSortState *sortState = (VciSortState *) css;
	int64		tuples_needed = node->count + node->offset;

	/* negative test checks for overflow in sum */
	if (node->noCount || tuples_needed < 0)
	{
		/* make sure flag gets reset if needed upon rescan */
		sortState->bounded = false;
	}
	else
	{
		sortState->bounded = true;
		sortState->bound = tuples_needed;
	}
}

CustomScanMethods vci_sort_scan_methods = {
	"VCI Sort",
	vci_sort_CreateCustomScanState,
	vci_sort_CopyCustomPlan
};

CustomExecMethods vci_sort_exec_methods = {
	"VCI Sort",
	vci_sort_BeginCustomPlan,
	vci_sort_ExecCustomPlan,
	vci_sort_EndCustomPlan,
	vci_sort_ReScanCustomPlan,
	vci_sort_MarkPosCustomPlan,
	vci_sort_RestrPosCustomPlan,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	vci_sort_ExplainCustomPlan,
	vci_sort_SetBoundCustomScan,
	NULL
};
