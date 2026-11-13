/*-------------------------------------------------------------------------
 *
 * vci_scan.c
 *	  Routines to handle VCI Scan nodes
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_scan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "vci.h"
#include "vci_executor.h"
#include "vci_utils.h"
#include "vci_fetch_row_store.h"

static Node *vci_scan_CreateCustomScanState(CustomScan *cscan);

/*
* Declarations of Custom Scan Methods callbacks
*/
static void vci_scan_BeginCustomPlan(CustomScanState *node, EState *estate, int eflags);

static void vci_scan_BeginCustomPlan_postprocess_enabling_vp(VciScan *scan, VciScanState *scanstate);
static TupleTableSlot *vci_scan_ExecCustomPlan(CustomScanState *node);
static void vci_scan_EndCustomPlan(CustomScanState *node);

static void vci_scan_ReScanCustomPlan(CustomScanState *node);
static void vci_scan_MarkPosCustomPlan(CustomScanState *cpstate);
static void vci_scan_RestrPosCustomPlan(CustomScanState *cpstate);

static void vci_scan_ExplainCustomPlanTargetRel(CustomScanState *node, ExplainState *es);
static CustomScan *vci_scan_CopyCustomPlan(const CustomScan *_from);

static int	exec_proc_scan_vector(VciScanState *scanstate);
static TupleTableSlot *exec_custom_plan_enabling_vp(VciScanState *scanstate);

/*****************************************************************************/
/* Column-store (basic)                                                      */
/*****************************************************************************/

static Node *
vci_scan_CreateCustomScanState(CustomScan *cscan)
{
	VciScan    *vscan;
	VciScanState *vss = palloc0_object(VciScanState);

	vscan = (VciScan *) cscan;

	vss->vci.css.ss.ps.type = T_CustomScanState;
	vss->vci.css.ss.ps.plan = (Plan *) vscan;
	vss->vci.css.flags = cscan->flags;

	switch (vscan->scan_mode)
	{
		case VCI_SCAN_MODE_COLUMN_STORE:
			vss->vci.css.methods = &vci_scan_exec_column_store_methods;
			break;

		default:
			Assert(0);
			break;
	}
	return (Node *) vss;
}

static void
vci_scan_BeginCustomPlan(CustomScanState *node, EState *estate, int eflags)
{
	VciScanState *scanstate = (VciScanState *) node;
	VciScan    *scan = (VciScan *) node->ss.ps.plan;
	Relation	currentRelation;
	TableScanDesc currentScanDesc;
	vci_initexpr_t initexpr = VCI_INIT_EXPR_NONE;
	TupleDesc	scanDesc;

	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);

	if (ScanDirectionIsBackward(estate->es_direction))
		elog(ERROR, "VCI Scan does not support backward scan");

	switch (scan->scan_mode)
	{
		case VCI_SCAN_MODE_COLUMN_STORE:
			initexpr = VCI_INIT_EXPR_FETCHING_COLUMN_STORE;
			break;

		default:
			Assert(0);
			break;
	}

	/*
	 * create state structure
	 */
	scanstate->is_subextent_grain = scan->is_subextent_grain;
	scanstate->vci.css.ss.ps.state = estate;

	/* create expression context for node */
	ExecAssignExprContext(estate, &scanstate->vci.css.ss.ps);

	/* initialize child expressions */
	scanstate->vci.css.ss.ps.qual =
		VciExecInitQual(scan->vci.cscan.scan.plan.qual, &scanstate->vci.css.ss.ps,
						initexpr);

	if (scan->scan_mode == VCI_SCAN_MODE_COLUMN_STORE)
	{
		vci_create_one_fetch_context_for_fetching_column_store(scanstate, scanstate->vci.css.ss.ps.ps_ExprContext);
	}

	switch (scan->scan_mode)
	{
		case VCI_SCAN_MODE_COLUMN_STORE:

			/*
			 * get the relation object id from the relid'th entry in the range
			 * table, open that relation and acquire appropriate lock on it.
			 */
			currentRelation = ExecOpenScanRelation(estate, scan->scanrelid, eflags);

			/* initialize a heapscan */
			currentScanDesc = table_beginscan(currentRelation,
											  estate->es_snapshot,
											  0,
											  NULL);

			scanstate->vci.css.ss.ss_currentRelation = currentRelation;
			scanstate->vci.css.ss.ss_currentScanDesc = currentScanDesc;

			/* and report the scan tuple slot's rowtype */
			scanDesc = RelationGetDescr(currentRelation);
			break;

		default:
			outerPlanState(scanstate) = ExecInitNode(outerPlan(scan), estate, eflags);
			scanDesc = ExecGetResultType(outerPlanState(scanstate));
			break;
	}

	/* tuple table initialization */
	ExecInitScanTupleSlot(estate, &scanstate->vci.css.ss, scanDesc, &TTSOpsMinimalTuple);
	ExecInitResultTupleSlotTL(&scanstate->vci.css.ss.ps, &TTSOpsMinimalTuple);

	/* ExecAssignScanProjectionInfo() ???? */
	if (scan->scan_mode == VCI_SCAN_MODE_COLUMN_STORE)
	{
		vci_scan_BeginCustomPlan_postprocess_enabling_vp(scan, scanstate);
	}
}

static void
vci_scan_BeginCustomPlan_postprocess_enabling_vp(VciScan *scan, VciScanState *scanstate)
{
	int			i,
				max_targetlist;
	uint16	   *skip_list;
	ListCell   *l;

	max_targetlist = list_length(scanstate->vci.css.ss.ps.plan->targetlist);

	skip_list = vci_CSGetSkipAddrFromVirtualTuples(scanstate->vector_set);

	if (scanstate->vci.css.ss.ps.qual)
	{

		scanstate->vp_qual = VciBuildVectorProcessing(scanstate->vci.css.ss.ps.qual->expr,
													  (PlanState *) scanstate,
													  scanstate->vci.css.ss.ps.ps_ExprContext,
													  skip_list);
	}
	scanstate->result_values = palloc_array(Datum *, max_targetlist);
	scanstate->result_isnull = palloc_array(bool *, max_targetlist);
	scanstate->vp_targets = palloc0_array(VciVPContext *, max_targetlist);

	i = 0;
	foreach(l, scanstate->vci.css.ss.ps.plan->targetlist)
	{
		TargetEntry *tle = castNode(TargetEntry, lfirst(l));
		AttrNumber	resind = tle->resno - 1;

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;
			int			index;

			Assert(var->varno == scan->scanrelid);

			index = scanstate->attr_map[var->varattno] - 1;

			Assert(index >= 0);
			Assert(index < scanstate->vector_set->num_columns);

			scanstate->result_values[resind] = vci_CSGetValueAddrFromVirtualTuplesColumnwise(scanstate->vector_set, index);
			scanstate->result_isnull[resind] = vci_CSGetIsNullAddrFromVirtualTuplesColumnwise(scanstate->vector_set, index);
		}
		else
		{
			scanstate->vp_targets[i] =
				VciBuildVectorProcessing((Expr *) tle->expr,
										 (PlanState *) scanstate,
										 scanstate->vci.css.ss.ps.ps_ExprContext,
										 skip_list);

			scanstate->result_values[resind] = scanstate->vp_targets[i]->resultValue;
			scanstate->result_isnull[resind] = scanstate->vp_targets[i]->resultIsNull;

			i++;
		}
	}
	scanstate->num_vp_targets = i;
}

static TupleTableSlot *
vci_scan_ExecCustomPlan(CustomScanState *cstate)
{
	VciScanState *scanstate = (VciScanState *) cstate;

	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);

	return VciExecProcScanTuple(scanstate);
}

/**
 * Processing equivalent to ExecProcNode() for VCI Scan.
 *
 * When calling the ExecProcNode of a lower VCI Scan from an upper VCI plan,
 * overhead occurs if going through the CustomPlanState's ExecCustomPlan.
 * This is a special version to avoid that.
 *
 * @param[in] scanstate VCI Scan state
 * @return output tuple
 *
 * @todo This can be abolished.
 */
TupleTableSlot *
VciExecProcScanTuple(VciScanState *scanstate)
{
	TupleTableSlot *result;
	PlanState  *node;
	bool		use_instrumentation;

	node = &scanstate->vci.css.ss.ps;

	/*
	 * XXX - This is a workaround to make sure that the plan node we are
	 * reusing had not already started timing. This is needed to prevent
	 * "ERROR: InstrStartNode called twice in a row", which can happen for
	 * EXPLAIN ANALYZE SELECT ...
	 */
	use_instrumentation = node->instrument && INSTR_TIME_IS_ZERO(node->instrument->starttime);

	CHECK_FOR_INTERRUPTS();

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node);		/* let ReScan handle this */

	if (use_instrumentation)
		InstrStartNode(node->instrument);

	result = exec_custom_plan_enabling_vp(scanstate);

	if (use_instrumentation)
		InstrStopNode(node->instrument, TupIsNull(result) ? 0.0 : 1.0);

	return result;
}

static TupleTableSlot *
exec_custom_plan_enabling_vp(VciScanState *scanstate)
{
	ExprContext *econtext;
	TupleTableSlot *outputslot;
	TupleDesc	tupdesc;
	int			slot_index;

	econtext = scanstate->vci.css.ss.ps.ps_ExprContext;

	if (!scanstate->first_fetch || (scanstate->pos.num_fetched_rows <= scanstate->pos.current_row))
	{
		int			result;

		ResetExprContext(econtext);

		do
		{
			result = exec_proc_scan_vector(scanstate);

			if (result == -1)
			{
				ExecClearTuple(scanstate->vci.css.ss.ss_ScanTupleSlot);

				return NULL;
			}
		} while (result == 0);
	}

	outputslot = scanstate->vci.css.ss.ps.ps_ResultTupleSlot;
	tupdesc = outputslot->tts_tupleDescriptor;
	slot_index = scanstate->pos.current_row;

	ExecClearTuple(outputslot);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		outputslot->tts_values[i] = (scanstate->result_values[i])[slot_index];
		outputslot->tts_isnull[i] = (scanstate->result_isnull[i])[slot_index];
	}

	ExecStoreVirtualTuple(outputslot);

	vci_step_next_tuple_from_column_store(scanstate);

	return outputslot;
}

int
VciExecProcScanVector(VciScanState *scanstate)
{
	int			result;
	PlanState  *node;

	node = &scanstate->vci.css.ss.ps;

	CHECK_FOR_INTERRUPTS();

	if (node->chgParam != NULL) /* something changed */
		ExecReScan(node);		/* let ReScan handle this */

	if (node->instrument)
		InstrStartNode(node->instrument);

	do
	{
		result = exec_proc_scan_vector(scanstate);

		if (result == -1)
		{
			ExecClearTuple(scanstate->vci.css.ss.ss_ScanTupleSlot);

			result = 0;
			break;
		}
	} while (result == 0);

	if (node->instrument)
		InstrStopNode(node->instrument, 1.0 * result);

	return result;
}

static int
exec_proc_scan_vector(VciScanState *scanstate)
{
	int			max_slots;
	int			num_slots = 0;
	int			slot_index;
	int			check_slot_index;
	ExprContext *econtext;
	ExprState  *qual;
	TupleTableSlot *old_tts;
	MemoryContext oldContext;
	uint16	   *skip_list;

	econtext = scanstate->vci.css.ss.ps.ps_ExprContext;
	qual = scanstate->vci.css.ss.ps.qual;

	CHECK_FOR_INTERRUPTS();

	ResetExprContext(econtext);

	if (!vci_fill_vector_set_from_column_store(scanstate))
		return -1;

	old_tts = econtext->ecxt_scantuple;
	econtext->ecxt_scantuple = NULL;	/* safety */
	max_slots = scanstate->pos.num_fetched_rows;

	Assert(max_slots > 0);

	oldContext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

	num_slots = 0;
	skip_list = vci_CSGetSkipFromVirtualTuples(scanstate->vector_set);
	slot_index = skip_list[0];
	check_slot_index = 0;

	if (qual)
	{
		VciVPContext *vpcontext = scanstate->vp_qual;

		VciExecEvalVectorProcessing(vpcontext, econtext, max_slots);

		for (; slot_index < max_slots; slot_index += skip_list[slot_index + 1] + 1)
		{
			if (!vpcontext->resultIsNull[slot_index] && DatumGetBool(vpcontext->resultValue[slot_index]))
			{
				check_slot_index = slot_index + 1;
				num_slots++;
			}
			else
			{
				InstrCountFiltered1(&scanstate->vci.css.ss, 1);
				skip_list[check_slot_index] += skip_list[slot_index + 1] + 1;
			}
		}

		scanstate->pos.current_row = skip_list[0];

		VciExecTargetListWithVectorProcessing(scanstate, econtext, max_slots);
	}
	else
	{
		VciExecTargetListWithVectorProcessing(scanstate, econtext, max_slots);

		for (; slot_index < max_slots; slot_index += skip_list[slot_index + 1] + 1)
			num_slots++;
	}

	MemoryContextSwitchTo(oldContext);

	econtext->ecxt_scantuple = old_tts;

	if (num_slots == 0)
	{
		scanstate->pos.current_row = scanstate->pos.num_fetched_rows;
		return 0;
	}

	return max_slots;
}

static void
vci_scan_EndCustomPlan(CustomScanState *node)
{
	VciScan    *scan;
	VciScanState *scanstate = (VciScanState *) node;
	TableScanDesc scanDesc;

	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);

	scan = (VciScan *) scanstate->vci.css.ss.ps.plan;

	scanDesc = scanstate->vci.css.ss.ss_currentScanDesc;

	switch (scan->scan_mode)
	{
		case VCI_SCAN_MODE_COLUMN_STORE:
			vci_destroy_one_fetch_context_for_fetching_column_store(scanstate);

			/* close the heap scan */
			table_endscan(scanDesc);

			break;

		default:
			/* LCOV_EXCL_START */
			elog(PANIC, "Should not reach here");
			/* LCOV_EXCL_STOP */
			break;
	}
}

static void
vci_scan_ReScanCustomPlan(CustomScanState *node)
{
	VciScanState *scanstate;

	scanstate = (VciScanState *) node;

	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);
	/* Rescan EvalPlanQual tuple if we're inside an EvalPlanQual recheck */
	Assert(scanstate->vci.css.ss.ps.state->es_epq_active == NULL);

	scanstate->first_fetch = false;
}

static void
vci_scan_MarkPosCustomPlan(CustomScanState *node)
{
	VciScanState *scanstate = (VciScanState *) node;

	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);

	vci_mark_pos_vector_set_from_column_store(scanstate);
}

static void
vci_scan_RestrPosCustomPlan(CustomScanState *node)
{
	VciScanState *scanstate = (VciScanState *) node;

	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);

	ExecClearTuple(scanstate->vci.css.ss.ss_ScanTupleSlot);

	vci_restr_pos_vector_set_from_column_store(scanstate);
}

static void
vci_scan_ExplainCustomPlanTargetRel(CustomScanState *node, ExplainState *es)
{
	VciScanState *scanstate;
	VciScan    *scan;
	Index		scanrelid;
	char	   *refname;
	char	   *objectname = NULL;
	char	   *namespace = NULL;
	const char *indexname = NULL;
	RangeTblEntry *rte;

	scanstate = (VciScanState *) node;
	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);
	scan = (VciScan *) scanstate->vci.css.ss.ps.plan;
	scanrelid = scan->scanrelid;

	rte = rt_fetch(scanrelid, es->rtable);
	Assert(rte->rtekind == RTE_RELATION);

	refname = (char *) list_nth(es->rtable_names, scanrelid - 1);
	if (refname == NULL)
		refname = rte->eref->aliasname;
	objectname = get_rel_name(rte->relid);
	if (es->verbose)
		namespace = get_namespace_name(get_rel_namespace(rte->relid));

	indexname = get_rel_name(scan->indexoid);
	if (indexname == NULL)
		elog(ERROR, "cache lookup failed for index %u", scan->indexoid);

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		appendStringInfo(es->str, " using %s on",
						 quote_identifier(indexname));

		if (namespace != NULL)
			appendStringInfo(es->str, " %s.%s", quote_identifier(namespace),
							 quote_identifier(objectname));
		else if (objectname != NULL)
			appendStringInfo(es->str, " %s", quote_identifier(objectname));
		if (objectname == NULL || strcmp(refname, objectname) != 0)
			appendStringInfo(es->str, " %s", quote_identifier(refname));
	}
	else
	{
		ExplainPropertyText("Index Name", indexname, es);
		if (objectname != NULL)
			ExplainPropertyText("Relation Name", objectname, es);
		if (namespace != NULL)
			ExplainPropertyText("Schema", namespace, es);
		ExplainPropertyText("Alias", refname, es);
	}
}

static CustomScan *
vci_scan_CopyCustomPlan(const CustomScan *_from)
{
	const VciScan *from = (const VciScan *) _from;
	VciScan    *newnode;

	newnode = palloc0_object(VciScan);

	vci_copy_plan(&newnode->vci, &from->vci);

	newnode->scan_mode = from->scan_mode;
	newnode->scanrelid = from->scanrelid;
	newnode->reloid = from->reloid;
	newnode->indexoid = from->indexoid;
	newnode->attr_used = bms_copy(from->attr_used);
	newnode->num_attr_used = from->num_attr_used;
	newnode->is_all_simple_vars = from->is_all_simple_vars;
	newnode->estimate_tuples = from->estimate_tuples;
	newnode->is_subextent_grain = from->is_subextent_grain;
	newnode->index_ph_id = from->index_ph_id;
	newnode->fetch_ph_id = from->fetch_ph_id;

	((Node *) newnode)->type = nodeTag((Node *) from);

	return &newnode->vci.cscan;
}

/*****************************************************************************/
/* Callback                                                                  */
/*****************************************************************************/

CustomScanMethods vci_scan_scan_methods = {
	"VCI Scan",
	vci_scan_CreateCustomScanState,
	vci_scan_CopyCustomPlan
};

CustomExecMethods vci_scan_exec_column_store_methods = {
	"VCI Scan",
	vci_scan_BeginCustomPlan,
	vci_scan_ExecCustomPlan,
	vci_scan_EndCustomPlan,
	vci_scan_ReScanCustomPlan,
	vci_scan_MarkPosCustomPlan,
	vci_scan_RestrPosCustomPlan,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	vci_scan_ExplainCustomPlanTargetRel
};
