/*-------------------------------------------------------------------------
 *
 * vci_agg.c
 *	  Routines to handle VCI Agg nodes
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_agg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

#include "vci.h"
#include "vci_executor.h"
#include "vci_utils.h"
#include "vci_aggref.h"

static void advance_transition_function(VciAggState *aggstate,
										VciAggStatePerAgg peraggstate,
										VciAggStatePerGroup pergroupstate);
static void advance_aggregates_vector(VciAggState *aggstate, VciAggStatePerGroup *entries, int max_slots);
static void find_cols(VciAggState *aggstate, Bitmapset **unaggregated);
static bool find_cols_walker(Node *node, Bitmapset **colnos);
static void build_hash_table(VciAggState *aggstate);
static void hash_create_memory(VciAggState *aggstate);
static List *find_hash_columns(VciAggState *aggstate);
static void lookup_hash_entry_vector(VciAggState *aggstate,
									 VciAggStatePerGroup *entries, int max_slots);
static TupleTableSlot *agg_retrieve_direct(VciAggState *aggstate);
static void agg_fill_hash_table_vector(VciAggState *aggstate);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);

static void vci_agg_BeginCustomPlan_preprocess(VciAggState *aggstate);
static void vci_agg_BeginCustomPlan_postprocess_for_advance_aggref(VciAggState *aggstate);
static void vci_agg_BeginCustomPlan_postprocess_for_vp(VciAggState *aggstate, ExprContext *econtext);
static void vci_ExecFreeExprContext(PlanState *planstate);

/**
 * Initialize all aggregates for a new group of input values.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 *
 * copied from src/backend/executor/nodeAgg.c
 */
void
vci_initialize_aggregates(VciAggState *aggstate,
						  VciAggStatePerAgg peragg,
						  VciAggStatePerGroup pergroup)
{
	for (int aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		VciAggStatePerAgg peraggstate = &peragg[aggno];
		VciAggStatePerGroup pergroupstate = &pergroup[aggno];

		Assert(peraggstate->numSortCols == 0);

		/*
		 * (Re)set transValue to the initial value.
		 *
		 * Note that when the initial value is pass-by-ref, we must copy it
		 * (into the aggcontext) since we will pfree the transValue later.
		 */
		if (peraggstate->initValueIsNull)
			pergroupstate->transValue = peraggstate->initValue;
		else
		{
			MemoryContext oldContext;

			oldContext = MemoryContextSwitchTo(aggstate->aggcontext);
			pergroupstate->transValue = datumCopy(peraggstate->initValue,
												  peraggstate->transtypeByVal,
												  peraggstate->transtypeLen);
			MemoryContextSwitchTo(oldContext);
		}
		pergroupstate->transValueIsNull = peraggstate->initValueIsNull;

		/*
		 * If the initial value for the transition state doesn't exist in the
		 * pg_aggregate table then we will let the first non-NULL value
		 * returned from the outer procNode become the initial value. (This is
		 * useful for aggregates like max() and min().) The noTransValue flag
		 * signals that we still need to do this.
		 */
		pergroupstate->noTransValue = peraggstate->initValueIsNull;
	}
}

/**
 * Given new input value(s), advance the transition function of an aggregate.
 *
 * The new values (and null flags) have been preloaded into argument positions
 * 1 and up in peraggstate->transfn_fcinfo, so that we needn't copy them again
 * to pass to the transition function.  We also expect that the static fields
 * of the fcinfo are already initialized; that was done by ExecInitAgg().
 *
 * It doesn't matter which memory context this is called in.
 *
 * copied from src/backend/executor/nodeAgg.c
 */
static void
advance_transition_function(VciAggState *aggstate,
							VciAggStatePerAgg peraggstate,
							VciAggStatePerGroup pergroupstate)
{
	FunctionCallInfo fcinfo = peraggstate->transfn_fcinfo;
	MemoryContext oldContext;
	Datum		newVal;

	if (peraggstate->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		int			numTransInputs = peraggstate->numTransInputs;

		for (int i = 1; i <= numTransInputs; i++)
		{
			if (fcinfo->args[i].isnull)
				return;
		}
		if (pergroupstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into aggcontext if it is pass-by-ref. We
			 * do not need to pfree the old transValue, since it's NULL.
			 */
			oldContext = MemoryContextSwitchTo(aggstate->aggcontext);
			pergroupstate->transValue = datumCopy(fcinfo->args[1].value,
												  peraggstate->transtypeByVal,
												  peraggstate->transtypeLen);
			pergroupstate->transValueIsNull = false;
			pergroupstate->noTransValue = false;
			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (pergroupstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			return;
		}
	}

	/* We run the transition functions in per-input-tuple memory context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	/* set up aggstate->curperagg for AggGetAggref() */
	aggstate->pseudo_aggstate->curperagg = (AggStatePerAgg) peraggstate;	/* @remark */

	/*
	 * OK to call the transition function
	 */
	fcinfo->args[0].value = pergroupstate->transValue;
	fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
	fcinfo->isnull = false;		/* just in case transfn doesn't set it */

	newVal = FunctionCallInvoke(fcinfo);

	aggstate->pseudo_aggstate->curperagg = NULL;

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * pfree the prior transValue.  But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 */
	if (!peraggstate->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
	{
		if (!fcinfo->isnull)
		{
			MemoryContextSwitchTo(aggstate->aggcontext);
			newVal = datumCopy(newVal,
							   peraggstate->transtypeByVal,
							   peraggstate->transtypeLen);
		}
		else
		{
			/*
			 * Ensure that VciAggStatePerGroup->transValue ends up being 0, so
			 * callers can safely compare newValue/oldValue without having to
			 * check their respective nullness.
			 */
			newVal = (Datum) 0;
		}
		if (!pergroupstate->transValueIsNull)
			pfree(DatumGetPointer(pergroupstate->transValue));
	}

	pergroupstate->transValue = newVal;
	pergroupstate->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}

/**
 * Perform aggregation processing for 1 input
 *
 * @param[in,out] aggstate VCI Agg State
 * @param[in,out] pergroup Pointer to the VciAggStatePerGroup struct holding the Transition data
 */
void
vci_advance_aggregates(VciAggState *aggstate, VciAggStatePerGroup pergroup)
{
	for (int aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		VciAggStatePerAgg peraggstate = &aggstate->peragg[aggno];
		VciAggStatePerGroup pergroupstate = &pergroup[aggno];
		int			numTransInputs = peraggstate->numTransInputs;
		TupleTableSlot *slot;

		/* Evaluate the current input expressions for this aggregate */
		slot = VciExecProject(peraggstate->evalproj);

		Assert(peraggstate->numSortCols == 0);

		{
			/* We can apply the transition function immediately */
			FunctionCallInfo fcinfo = peraggstate->transfn_fcinfo;

			/* Load values into fcinfo */
			/* Start from 1, since the 0th arg will be the transition value */
			Assert(slot->tts_nvalid >= numTransInputs);
			for (int i = 0; i < numTransInputs; i++)
			{
				fcinfo->args[i + 1].value = slot->tts_values[i];
				fcinfo->args[i + 1].isnull = slot->tts_isnull[i];
			}

			advance_transition_function(aggstate, peraggstate, pergroupstate);
		}
	}
}

/**
 * Perform aggregation processing for 1 vector
 *
 * @param[in,out] aggstate  VCI Agg State
 * @param[in,out] entries   Pointer to VciAggHashEntry struct holding a pair of hash key and Transition data
 * @param[in]     max_slots Number of vector rows
 */
static void
advance_aggregates_vector(VciAggState *aggstate, VciAggStatePerGroup *entries, int max_slots)
{
	aggstate->tmpcontext->ecxt_outertuple = NULL;

	for (int aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		VciAggStatePerAgg peraggstate = &aggstate->peragg[aggno];

		/*
		 * slot_getsomeattrs() is not required
		 */
		Assert(peraggstate->advance_aggref != NULL);
		peraggstate->advance_aggref(aggstate, aggno, entries, max_slots);
	}
}

/**
 * Compute the final value of one aggregate.
 *
 * The finalfunction will be run, and the result delivered, in the
 * output-tuple context; caller's CurrentMemoryContext does not matter.
 *
 * copied from src/backend/executor/nodeAgg.c
 */
void
vci_finalize_aggregate(VciAggState *aggstate,
					   VciAggStatePerAgg peraggstate,
					   VciAggStatePerGroup pergroupstate,
					   Datum *resultVal, bool *resultIsNull)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	bool		anynull = false;
	MemoryContext oldContext;
	int			i;

	oldContext = MemoryContextSwitchTo(aggstate->vci.css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Evaluate any direct arguments.  We do this even if there's no finalfn
	 * (which is unlikely anyway), so that side-effects happen as expected.
	 * The direct arguments go into arg positions 1 and up, leaving position 0
	 * for the transition state value.
	 */
	i = 1;

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peraggstate->finalfn_oid))
	{
		int			numFinalArgs = peraggstate->numFinalArgs;

		/* set up aggstate->curperagg for AggGetAggref() */
		aggstate->pseudo_aggstate->curperagg = (AggStatePerAgg) peraggstate;	/* @remark */

		InitFunctionCallInfoData(*fcinfo, &(peraggstate->finalfn),
								 numFinalArgs,
								 peraggstate->aggCollation,
								 (Node *) aggstate->pseudo_aggstate, NULL);

		/* Fill in the transition state value */
		fcinfo->args[0].value =
			MakeExpandedObjectReadOnly(pergroupstate->transValue,
									   pergroupstate->transValueIsNull,
									   peraggstate->transtypeLen);
		fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
		anynull |= pergroupstate->transValueIsNull;

		/* Fill any remaining argument positions with nulls */
		for (; i < numFinalArgs; i++)
		{
			fcinfo->args[i].value = (Datum) 0;
			fcinfo->args[i].isnull = true;
			anynull = true;
		}

		if (fcinfo->flinfo->fn_strict && anynull)
		{
			/* don't call a strict function with NULL inputs */
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			Datum		result;

			result = FunctionCallInvoke(fcinfo);
			*resultIsNull = fcinfo->isnull;
			*resultVal = MakeExpandedObjectReadOnly(result,
													fcinfo->isnull,
													peraggstate->resulttypeLen);
		}
		aggstate->pseudo_aggstate->curperagg = NULL;
	}
	else
	{
		*resultVal =
			MakeExpandedObjectReadOnly(pergroupstate->transValue,
									   pergroupstate->transValueIsNull,
									   peraggstate->transtypeLen);
		*resultIsNull = pergroupstate->transValueIsNull;
	}

	MemoryContextSwitchTo(oldContext);
}

/**
 * find_cols
 *	  Construct a bitmapset of the column numbers of un-aggregated Vars
 *	  appearing in our targetlist and qual (HAVING clause)
 *
 * copied from src/backend/executor/nodeAgg.c
 */
static void
find_cols(VciAggState *aggstate, Bitmapset **unaggregated)
{
	VciAgg	   *node = (VciAgg *) aggstate->vci.css.ss.ps.plan;
	Bitmapset  *colnos;

	colnos = NULL;
	(void) find_cols_walker((Node *) node->vci.cscan.scan.plan.targetlist,
							&colnos);
	(void) find_cols_walker((Node *) node->vci.cscan.scan.plan.qual,
							&colnos);

	*unaggregated = colnos;
}

static bool
find_cols_walker(Node *node, Bitmapset **colnos)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* setrefs.c should have set the varno to OUTER_VAR */
		Assert(var->varno == OUTER_VAR);
		Assert(var->varlevelsup == 0);
		*colnos = bms_add_member(*colnos, var->varattno);
		return false;
	}

	if (IsA(node, Aggref))		/* do not descend into aggregate exprs */
		return false;

	return expression_tree_walker(node, find_cols_walker, colnos);
}

/**
 * Initialize the hash table to empty.
 *
 * The hash table always lives in the aggcontext memory context.
 *
 * copied from src/backend/executor/nodeAgg.c
 */
static void
build_hash_table(VciAggState *aggstate)
{
	VciAgg	   *node = (VciAgg *) aggstate->vci.css.ss.ps.plan;
	MemoryContext metacxt = aggstate->hash_metacxt;
	MemoryContext tuplescxt = aggstate->hash_tuplescxt;
	MemoryContext tmpcxt = aggstate->tmpcontext->ecxt_per_tuple_memory;
	Size		additionalsize;

	Assert(node->aggstrategy == AGG_HASHED ||
		   node->aggstrategy == AGG_MIXED);

	Assert(node->numGroups > 0);

	additionalsize = aggstate->numaggs * sizeof(VciAggStatePerGroupData);

	aggstate->hashtable = BuildTupleHashTable(&aggstate->vci.css.ss.ps,
											  aggstate->hashslot->tts_tupleDescriptor,
											  NULL,
											  node->numCols,
											  node->grpColIdx,
											  aggstate->eqfuncoids,
											  aggstate->hashfunctions,
											  node->grpCollations,
											  node->numGroups,
											  additionalsize,
											  metacxt,
											  tuplescxt,
											  tmpcxt,
											  false);
}

/**
 * Create a list of the tuple columns that actually need to be stored in
 * hashtable entries.  The incoming tuples from the child plan node will
 * contain grouping columns, other columns referenced in our targetlist and
 * qual, columns used to compute the aggregate functions, and perhaps just
 * junk columns we don't use at all.  Only columns of the first two types
 * need to be stored in the hashtable, and getting rid of the others can
 * make the table entries significantly smaller.  To avoid messing up Var
 * numbering, we keep the same tuple descriptor for hashtable entries as the
 * incoming tuples have, but set unwanted columns to NULL in the tuples that
 * go into the table.
 *
 * To eliminate duplicates, we build a bitmapset of the needed columns, then
 * convert it to an integer list (cheaper to scan at runtime). The list is
 * in decreasing order so that the first entry is the largest;
 * lookup_hash_entry depends on this to use slot_getsomeattrs correctly.
 * Note that the list is preserved over ExecReScanAgg, so we allocate it in
 * the per-query context (unlike the hash table itself).
 *
 * Note: at present, searching the tlist/qual is not really necessary since
 * the parser should disallow any unaggregated references to ungrouped
 * columns.  However, the search will be needed when we add support for
 * SQL99 semantics that allow use of "functionally dependent" columns that
 * haven't been explicitly grouped by.
 *
 * copied from src/backend/executor/nodeAgg.c
 */
static List *
find_hash_columns(VciAggState *aggstate)
{
	VciAgg	   *node = (VciAgg *) aggstate->vci.css.ss.ps.plan;
	Bitmapset  *colnos;
	List	   *collist;
	int			i;

	/* Find Vars that will be needed in tlist and qual */
	find_cols(aggstate, &colnos);
	/* Add in all the grouping columns */
	for (i = 0; i < node->numCols; i++)
		colnos = bms_add_member(colnos, node->grpColIdx[i]);
	/* Convert to list, using lcons so largest element ends up first */
	collist = NIL;
	i = -1;
	while ((i = bms_next_member(colnos, i)) >= 0)
		collist = lcons_int(i, collist);
	bms_free(colnos);

	return collist;
}

/*
 * Create memory contexts used for hash aggregation.
 *
 * copied from src/backend/executor/nodeAgg.c
 */
static void
hash_create_memory(VciAggState *aggstate)
{
	Size		maxBlockSize = ALLOCSET_DEFAULT_MAXSIZE;

#if 0
	/*
	 * The hashcontext's per-tuple memory will be used for byref transition
	 * values and returned by AggCheckCallContext().
	 */
	aggstate->hashcontext = CreateWorkExprContext(aggstate->ss.ps.state);
#endif

	/*
	 * The meta context will be used for the bucket array of
	 * TupleHashEntryData (or arrays, in the case of grouping sets). As the
	 * hash table grows, the bucket array will double in size and the old one
	 * will be freed, so an AllocSet is appropriate. For large bucket arrays,
	 * the large allocation path will be used, so it's not worth worrying
	 * about wasting space due to power-of-two allocations.
	 */
	aggstate->hash_metacxt = AllocSetContextCreate(/* aggstate->ss.ps.state->es_query_cxt, */
												   aggstate->vci.css.ss.ps.state->es_query_cxt,
												   "HashAgg meta context",
												   ALLOCSET_DEFAULT_SIZES);

	/*
	 * The hash entries themselves, which include the grouping key
	 * (firstTuple) and pergroup data, are stored in the table context. The
	 * bump allocator can be used because the entries are not freed until the
	 * entire hash table is reset. The bump allocator is faster for
	 * allocations and avoids wasting space on the chunk header or
	 * power-of-two allocations.
	 *
	 * Like CreateWorkExprContext(), use smaller sizings for smaller work_mem,
	 * to avoid large jumps in memory usage.
	 */

	/*
	 * Like CreateWorkExprContext(), use smaller sizings for smaller work_mem,
	 * to avoid large jumps in memory usage.
	 */
	maxBlockSize = pg_prevpower2_size_t(work_mem * (Size) 1024 / 16);

	/* But no bigger than ALLOCSET_DEFAULT_MAXSIZE */
	maxBlockSize = Min(maxBlockSize, ALLOCSET_DEFAULT_MAXSIZE);

	/* and no smaller than ALLOCSET_DEFAULT_INITSIZE */
	maxBlockSize = Max(maxBlockSize, ALLOCSET_DEFAULT_INITSIZE);

	aggstate->hash_tuplescxt = BumpContextCreate(/*aggstate->ss.ps.state->es_query_cxt,*/
												 aggstate->vci.css.ss.ps.state->es_query_cxt,
												 "HashAgg hashed tuples",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 maxBlockSize);

}

static void
lookup_hash_entry_vector(VciAggState *aggstate,
						 VciAggStatePerGroup *entries, int max_slots)
{
	VciScanState *scanstate = (VciScanState *) outerPlanState(aggstate);
	TupleTableSlot *hashslot = aggstate->hashslot;
	uint16	   *skip_list;

	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);

	skip_list = vci_CSGetSkipFromVirtualTuples(scanstate->vector_set);

	/* Clear the tuple */
	ExecClearTuple(hashslot);

	/*
	 * Fill all the columns of the virtual tuple with nulls
	 */
	MemSet(hashslot->tts_values, 0,
		   hashslot->tts_tupleDescriptor->natts * sizeof(Datum));
	memset(hashslot->tts_isnull, true,
		   hashslot->tts_tupleDescriptor->natts * sizeof(bool));

	for (int slot_index = skip_list[0]; slot_index < max_slots; slot_index += skip_list[slot_index + 1] + 1)
	{
		TupleHashEntry entry;
		bool		isnew;
		VciAggStatePerGroup pergroup;

		ExecClearTuple(hashslot);
		memset(hashslot->tts_isnull, true,
			   hashslot->tts_tupleDescriptor->natts * sizeof(bool));

		for (int i = 0; i < aggstate->num_hash_needed; i++)
		{
			int			varNumber = aggstate->hash_needed[i] - 1;

			hashslot->tts_values[varNumber] = aggstate->hash_input_values[i][slot_index];
			hashslot->tts_isnull[varNumber] = aggstate->hash_input_isnull[i][slot_index];
		}
		ExecStoreVirtualTuple(hashslot);

		/* find or create the hashtable entry using the filtered tuple */
		entry = LookupTupleHashEntry(aggstate->hashtable,
									 hashslot,
									 &isnew,
									 NULL);

		pergroup = (VciAggStatePerGroup) TupleHashEntryGetAdditional(aggstate->hashtable, entry);

		if (isnew && aggstate->numaggs)
		{
			/* initialize aggregates for new tuple group */
			vci_initialize_aggregates(aggstate, aggstate->peragg, pergroup);
		}

		entries[slot_index] = pergroup;
	}
}

/**
 * ExecAgg for non-hashed case
 *
 * copied from src/backend/executor/nodeAgg.c
 */
static TupleTableSlot *
agg_retrieve_direct(VciAggState *aggstate)
{
	VciAgg	   *node = (VciAgg *) aggstate->vci.css.ss.ps.plan;
	PlanState  *outerPlan;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	VciAggStatePerAgg peragg;
	VciAggStatePerGroup pergroup;
	TupleTableSlot *outerslot;
	TupleTableSlot *firstSlot;

	/*
	 * get state info from node
	 */
	outerPlan = outerPlanState(aggstate);
	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->vci.css.ss.ps.ps_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	/* tmpcontext is the per-input-tuple expression context */
	tmpcontext = aggstate->tmpcontext;
	peragg = aggstate->peragg;
	pergroup = aggstate->pergroup;
	firstSlot = aggstate->vci.css.ss.ss_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one matching
	 * aggstate->ss.ps.qual
	 */
	while (!aggstate->agg_done)
	{
		/*
		 * If we don't already have the first tuple of the new group, fetch it
		 * from the outer plan.
		 */
		if (aggstate->grp_firstTuple == NULL)
		{
			outerslot = ExecProcNode(outerPlan);
			if (!TupIsNull(outerslot))
			{
				/*
				 * Make a copy of the first input tuple; we will use this for
				 * comparisons (in group mode) and for projection.
				 */
				aggstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
			}
			else
			{
				/* outer plan produced no tuples at all */
				aggstate->agg_done = true;
				/* If we are grouping, we should produce no tuples too */
				if (node->aggstrategy != AGG_PLAIN)
					return NULL;
			}
		}

		/*
		 * Clear the per-output-tuple context for each group, as well as
		 * aggcontext (which contains any pass-by-ref transvalues of the old
		 * group).  We also clear any child contexts of the aggcontext; some
		 * aggregate functions store working state in such contexts.
		 *
		 * We use ReScanExprContext not just ResetExprContext because we want
		 * any registered shutdown callbacks to be called.  That allows
		 * aggregate functions to ensure they've cleaned up any non-memory
		 * resources.
		 */
		ReScanExprContext(econtext);

		MemoryContextReset(aggstate->aggcontext);

		/*
		 * Initialize working state for a new input tuple group
		 */
		vci_initialize_aggregates(aggstate, peragg, pergroup);

		if (aggstate->grp_firstTuple != NULL)
		{
			/*
			 * Store the copied first input tuple in the tuple table slot
			 * reserved for it.  The tuple will be deleted when it is cleared
			 * from the slot.
			 */
			ExecForceStoreHeapTuple(aggstate->grp_firstTuple,
									firstSlot,
									true);
			aggstate->grp_firstTuple = NULL;	/* don't keep two pointers */

			/* set up for first advance_aggregates call */
			tmpcontext->ecxt_outertuple = firstSlot;

			/*
			 * Process each outer-plan tuple, and then fetch the next one,
			 * until we exhaust the outer plan or cross a group boundary.
			 */
			for (;;)
			{
				vci_advance_aggregates(aggstate, pergroup);

				/* Reset per-input-tuple context after each tuple */
				ResetExprContext(tmpcontext);

				outerslot = ExecProcNode(outerPlan);
				if (TupIsNull(outerslot))
				{
					/* no more outer-plan tuples available */
					aggstate->agg_done = true;
					break;
				}
				/* set up for next advance_aggregates call */
				tmpcontext->ecxt_outertuple = outerslot;

				/*
				 * If we are grouping, check whether we've crossed a group
				 * boundary.
				 */
				if (node->aggstrategy == AGG_SORTED)
				{
					tmpcontext->ecxt_innertuple = firstSlot;
					if (!ExecQual(aggstate->eqfunctions[0],
								  tmpcontext))
					{
						/*
						 * Save the first input tuple of the next group.
						 */
						aggstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
						break;
					}
				}
			}
		}

		/*
		 * Use the representative input tuple for any references to
		 * non-aggregated input columns in aggregate direct args, the node
		 * qual, and the tlist.  (If we are not grouping, and there are no
		 * input rows at all, we will come here with an empty firstSlot ...
		 * but if not grouping, there can't be any references to
		 * non-aggregated input columns, so no problem.)
		 */
		econtext->ecxt_outertuple = firstSlot;

		/*
		 * Done scanning input tuple group. Finalize each aggregate
		 * calculation, and stash results in the per-output-tuple context.
		 */
		for (int aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			VciAggStatePerAgg peraggstate = &peragg[aggno];
			VciAggStatePerGroup pergroupstate = &pergroup[aggno];

			Assert(peraggstate->numSortCols == 0);

			vci_finalize_aggregate(aggstate, peraggstate, pergroupstate,
								   &aggvalues[aggno], &aggnulls[aggno]);
		}

		/*
		 * Check the qual (HAVING clause); if the group does not match, ignore
		 * it and loop back to try to process another group.
		 */
		if (ExecQual(aggstate->vci.css.ss.ps.qual, econtext))
		{
			/*
			 * Form and return a projection tuple using the aggregate results
			 * and the representative input tuple.
			 */
			TupleTableSlot *result;

			result = VciExecProject(aggstate->vps_ProjInfo);

			return result;
		}
		else
			InstrCountFiltered1(aggstate, 1);
	}

	/* No more groups */
	return NULL;
}

/**
 * When Hashed aggregation is selected, tuples are received from lower nodes,
 * constructs a has table, and aggregate them. However, processing is performed in vector units.
 *
 * @param[in,out] aggstate  VCI Agg State
 */
void
vci_agg_fill_hash_table(VciAggState *aggstate)
{
	agg_fill_hash_table_vector(aggstate);
}

static void
agg_fill_hash_table_vector(VciAggState *aggstate)
{
	ExprContext *tmpcontext;
	VciScanState *scanstate = (VciScanState *) outerPlanState(aggstate);

	Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);

	/*
	 * get state info from node
	 */
	/* tmpcontext is the per-input-tuple expression context */
	tmpcontext = aggstate->tmpcontext;

	/*
	 * Process each outer-plan tuple, and then fetch the next one, until we
	 * exhaust the outer plan.
	 */
	for (;;)
	{
		int			max_slots;
		VciAggStatePerGroup entries[VCI_MAX_FETCHING_ROWS];

		/* fetch VCI_MAX_FETCHING_ROWS rows from column store */
		max_slots = VciExecProcScanVector(scanstate);

		if (max_slots == 0)
			break;

		tmpcontext->ecxt_outertuple = NULL; /* safety */

		lookup_hash_entry_vector(aggstate, entries, max_slots);

		/* Advance the aggregates */
		advance_aggregates_vector(aggstate, entries, max_slots);

		/* Reset per-input-tuple context after each tuple */
		ResetExprContext(tmpcontext);

		/* Vector loading is complete */
		vci_finish_vector_set_from_column_store(scanstate);
	}

	aggstate->table_filled = true;
	/* Initialize to walk the hash table */
	ResetTupleHashIterator(aggstate->hashtable, &aggstate->hashiter);
}

/**
 * Retrieve 1 tuple at a time from the hash table
 *
 * @param[in,out] aggstate  VCI Agg State
 * @return Resulting output tuple
 *
 * @note This function is used after executing vci_agg_fill_hash_table().
 */
TupleTableSlot *
vci_agg_retrieve_hash_table(VciAggState *aggstate)
{
	ExprContext *econtext;
	Datum	   *aggvalues;
	bool	   *aggnulls;
	VciAggStatePerAgg peragg;
	VciAggStatePerGroup pergroup;
	TupleHashEntry entry;
	TupleTableSlot *firstSlot;

	/*
	 * get state info from node
	 */
	/* econtext is the per-output-tuple expression context */
	econtext = aggstate->vci.css.ss.ps.ps_ExprContext;
	aggvalues = econtext->ecxt_aggvalues;
	aggnulls = econtext->ecxt_aggnulls;
	peragg = aggstate->peragg;
	firstSlot = aggstate->vci.css.ss.ss_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one satisfying
	 * aggstate->ss.ps.qual
	 */
	while (!aggstate->agg_done)
	{

		/*
		 * Find the next entry in the hash table
		 */
		entry = vci_agg_find_group_from_hash_table(aggstate);
		if (entry == NULL)
		{
			/* No more entries in hashtable, so done */
			aggstate->agg_done = true;
			return NULL;
		}

		/*
		 * Clear the per-output-tuple context for each group
		 *
		 * We intentionally don't use ReScanExprContext here; if any aggs have
		 * registered shutdown callbacks, they mustn't be called yet, since we
		 * might not be done with that agg.
		 */
		ResetExprContext(econtext);

		/*
		 * Store the copied first input tuple in the tuple table slot reserved
		 * for it, so that it can be used in ExecProject.
		 */
		ExecForceStoreMinimalTuple(entry->firstTuple,
								   firstSlot,
								   false);

		pergroup = (VciAggStatePerGroup) TupleHashEntryGetAdditional(aggstate->hashtable, entry);

		/*
		 * Finalize each aggregate calculation, and stash results in the
		 * per-output-tuple context.
		 */
		for (int aggno = 0; aggno < aggstate->numaggs; aggno++)
		{
			VciAggStatePerAgg peraggstate = &peragg[aggno];
			VciAggStatePerGroup pergroupstate = &pergroup[aggno];

			Assert(peraggstate->numSortCols == 0);
			vci_finalize_aggregate(aggstate, peraggstate, pergroupstate,
								   &aggvalues[aggno], &aggnulls[aggno]);
		}

		/*
		 * Use the representative input tuple for any references to
		 * non-aggregated input columns in the qual and tlist.
		 */
		econtext->ecxt_outertuple = firstSlot;

		/*
		 * Check the qual (HAVING clause); if the group does not match, ignore
		 * it and loop back to try to process another group.
		 */
		if (ExecQual(aggstate->vci.css.ss.ps.qual, econtext))
		{
			/*
			 * Form and return a projection tuple using the aggregate results
			 * and the representative input tuple.
			 */
			TupleTableSlot *result;

			result = VciExecProject(aggstate->vps_ProjInfo);

			return result;
		}
		else
			InstrCountFiltered1(aggstate, 1);
	}

	/* No more groups */
	return NULL;
}

/**
 * Retrive only 1 entry from hash table
 *
 * @param[in,out] aggstate  VCI Agg State
 * @return One VciAggHashEntry retrieved from hash table
 */
TupleHashEntry
vci_agg_find_group_from_hash_table(VciAggState *aggstate)
{
	while (!aggstate->agg_done)
	{
		return (TupleHashEntry) ScanTupleHashTable(aggstate->hashtable, &aggstate->hashiter);
	}

	/* No more groups */
	return NULL;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

/***********************************************************************
 * API exposed to aggregate functions
 ***********************************************************************/

/*
 * The following function is a callback function from AggState,
 * but there is no need to directly maintain it in VCI Agg.
 *
 * - AggCheckCallContext - test if a SQL function is being called as an aggregate
 * - AggGetAggref - allow an aggregate support function to get its Aggref
 * - AggGetTempMemoryContext - fetch short-term memory context for aggregates
 * - AggRegisterCallback - register a cleanup callback for an aggregate
 */

/* ----------------
 *   VciAgg information
 * ----------------
 */
static Node *
vci_agg_CreateCustomScanState(CustomScan *cscan)
{
	VciAgg	   *vagg = (VciAgg *) cscan;
	VciAggState *vas = palloc0_object(VciAggState);

	vas->vci.css.ss.ps.type = T_CustomScanState;
	vas->vci.css.ss.ps.plan = (Plan *) vagg;

	vas->vci.css.flags = cscan->flags;

	switch (vagg->aggstrategy)
	{
		case AGG_HASHED:
			vas->vci.css.methods = &vci_hashagg_exec_methods;
			break;

		case AGG_SORTED:
			vas->vci.css.methods = &vci_groupagg_exec_methods;
			break;

		case AGG_PLAIN:
			vas->vci.css.methods = &vci_agg_exec_methods;
			break;

		default:
			break;
	}

	vas->aggs = NIL;
	vas->numaggs = 0;
	vas->eqfunctions = NULL;
	vas->hashfunctions = NULL;
	vas->peragg = NULL;
	vas->agg_done = false;
	vas->pergroup = NULL;
	vas->grp_firstTuple = NULL;
	vas->hashtable = NULL;

	return (Node *) vas;
}

/**
 * ExecCustomPlan callback called from CustomPlanState of VCI Agg
 */
static TupleTableSlot *
vci_agg_ExecCustomPlan(CustomScanState *node)
{
	VciAggState *aggstate;

	aggstate = (VciAggState *) node;

	/*
	 * Exit if nothing left to do.  (We must do the ps_TupFromTlist check
	 * first, because in some cases agg_done gets set before we emit the final
	 * aggregate tuple, and we have to finish running SRFs for it.)
	 */
	if (aggstate->agg_done)
		return NULL;

	Assert(IsA(node->ss.ps.plan, CustomScan));

	/* Dispatch based on strategy */
	if (((VciAgg *) node->ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		if (!aggstate->table_filled)
			vci_agg_fill_hash_table(aggstate);
		return vci_agg_retrieve_hash_table(aggstate);
	}
	else
		return agg_retrieve_direct(aggstate);

	return NULL;
}

/**
 * Copy the contents of VCI Agg State to pseudo Agg state
 */
static void
copy_into_pseudo_aggstate(AggState *pseudo_aggstate, VciAggState *aggstate)
{
	pseudo_aggstate->ss.ps.plan = aggstate->vci.css.ss.ps.plan;
	pseudo_aggstate->ss.ps.state = aggstate->vci.css.ss.ps.state;
	pseudo_aggstate->ss.ps.instrument = aggstate->vci.css.ss.ps.instrument;
	pseudo_aggstate->ss.ps.qual = aggstate->vci.css.ss.ps.qual;
	pseudo_aggstate->ss.ps.lefttree = aggstate->vci.css.ss.ps.lefttree;
	pseudo_aggstate->ss.ps.righttree = aggstate->vci.css.ss.ps.righttree;
	pseudo_aggstate->ss.ps.initPlan = aggstate->vci.css.ss.ps.initPlan;
	pseudo_aggstate->ss.ps.subPlan = aggstate->vci.css.ss.ps.subPlan;
	pseudo_aggstate->ss.ps.chgParam = aggstate->vci.css.ss.ps.chgParam;
	pseudo_aggstate->ss.ps.ps_ResultTupleSlot = aggstate->vci.css.ss.ps.ps_ResultTupleSlot;
	pseudo_aggstate->ss.ps.ps_ExprContext = aggstate->vci.css.ss.ps.ps_ExprContext;
	pseudo_aggstate->ss.ps.ps_ProjInfo = aggstate->vci.css.ss.ps.ps_ProjInfo;

	pseudo_aggstate->ss.ss_currentRelation = aggstate->vci.css.ss.ss_currentRelation;
	pseudo_aggstate->ss.ss_currentScanDesc = aggstate->vci.css.ss.ss_currentScanDesc;
	pseudo_aggstate->ss.ss_ScanTupleSlot = aggstate->vci.css.ss.ss_ScanTupleSlot;

	pseudo_aggstate->aggs = aggstate->aggs;
	pseudo_aggstate->numaggs = aggstate->numaggs;
	pseudo_aggstate->phases[0].eqfunctions = aggstate->eqfunctions;
	pseudo_aggstate->perhash->hashfunctions = aggstate->hashfunctions;
	pseudo_aggstate->peragg = (AggStatePerAgg) aggstate->peragg;
	pseudo_aggstate->tmpcontext = aggstate->tmpcontext;
	pseudo_aggstate->curperagg = NULL;
	pseudo_aggstate->agg_done = aggstate->agg_done;
	pseudo_aggstate->pergroups = (AggStatePerGroup *) &aggstate->pergroup;
	pseudo_aggstate->grp_firstTuple = aggstate->grp_firstTuple;
	pseudo_aggstate->perhash->hashtable = aggstate->hashtable;
	pseudo_aggstate->perhash->hashslot = NULL;
	pseudo_aggstate->table_filled = aggstate->table_filled;
	pseudo_aggstate->perhash->hashiter = aggstate->hashiter;
}

/**
 * BeginCustomPlan callback called from CustomPlan of VCI Agg
 */
static void
vci_agg_BeginCustomPlan(CustomScanState *node, EState *estate, int eflags)
{
	VciAgg	   *agg;
	VciAggState *aggstate;
	VciAggStatePerAgg peragg;
	Plan	   *outerPlan;
	ExprContext *econtext;
	int			max_aggno;
	int			numaggs;
	ListCell   *l;
	vci_initexpr_t initexpr;
	TupleDesc	scanDesc;
	bool		use_hashing;

	agg = (VciAgg *) node->ss.ps.plan;

	use_hashing = (agg->aggstrategy == AGG_HASHED || agg->aggstrategy == AGG_MIXED);

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	aggstate = (VciAggState *) node;

	aggstate->vci.css.ss.ps.state = estate;

	if (vci_get_vci_plan_type(outerPlan(agg)) == VCI_CUSTOMPLAN_SCAN)
	{
		aggstate->enable_vp = true;
	}

	vci_agg_BeginCustomPlan_preprocess(aggstate);

	/*
	 * Create expression contexts.  We need three or more, one for
	 * per-input-tuple processing, one for per-output-tuple processing, and
	 * one for each grouping set.  The per-tuple memory context of the
	 * per-grouping-set ExprContexts (aggcontexts) replaces the standalone
	 * memory context formerly used to hold transition values.  We cheat a
	 * little by using ExecAssignExprContext() to build all of them.
	 *
	 * NOTE: the details of what is stored in aggcontexts and what is stored
	 * in the regular per-query memory context are driven by a simple
	 * decision: we want to reset the aggcontext at group boundaries (if not
	 * hashing) and in ExecReScanAgg to recover no-longer-wanted space.
	 */
	ExecAssignExprContext(estate, &aggstate->vci.css.ss.ps);
	aggstate->tmpcontext = aggstate->vci.css.ss.ps.ps_ExprContext;
	ExecAssignExprContext(estate, &aggstate->vci.css.ss.ps);

	aggstate->pseudo_aggstate->aggcontexts[0] = aggstate->vci.css.ss.ps.ps_ExprContext;
	ExecAssignExprContext(estate, &aggstate->vci.css.ss.ps);

	aggstate->aggcontext =
		AllocSetContextCreate(CurrentMemoryContext,
							  "VciAggContext",
							  ALLOCSET_DEFAULT_SIZES);

	if (use_hashing)
		hash_create_memory(aggstate);

	/*
	 * The timing of ExecInitExpr() for targetlist and qual, and the timing of
	 * ExecInitNode() for outer node are reversed from the original.
	 *
	 * This is because we want VciScanState to exist when Var is evaluated.
	 */

	/*
	 * initialize child nodes
	 *
	 * If we are doing a hashed aggregation then the child plan does not need
	 * to handle REWIND efficiently; see ExecReScanAgg.
	 */
	if (agg->aggstrategy == AGG_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlan = outerPlan(node->ss.ps.plan);

	outerPlanState(aggstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * tuple table initialization
	 */
	aggstate->vci.css.ss.ps.outerops =
		ExecGetResultSlotOps(outerPlanState(&aggstate->vci.css.ss),
							 &aggstate->vci.css.ss.ps.outeropsfixed);
	aggstate->vci.css.ss.ps.outeropsset = true;

	ExecCreateScanSlotFromOuterPlan(estate, &aggstate->vci.css.ss,
									aggstate->vci.css.ss.ps.outerops);
	scanDesc = aggstate->vci.css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	ExecInitResultTupleSlotTL(&aggstate->vci.css.ss.ps, &TTSOpsVirtual);
	aggstate->hashslot = ExecInitExtraTupleSlot(estate, scanDesc, &TTSOpsMinimalTuple);

	/*
	 * In the case of hashed aggregation, Var in targetlist and qual are read
	 * using outer tuple, but targetlist under Aggref will fetch column store.
	 * (However if outer is other than VCI Scan, read from outer tuple)
	 *
	 * Sorted aggregation and plain aggregation are all read from outer tuple.
	 */
	if (agg->aggstrategy == AGG_HASHED)
		initexpr = VCI_INIT_EXPR_FETCHING_COLUMN_STORE;
	else
		initexpr = VCI_INIT_EXPR_NORMAL;

	/*
	 * initialize child expressions
	 *
	 * Note: ExecInitExpr finds Aggrefs for us, and also checks that no aggs
	 * contain other agg calls in their arguments.  This would make no sense
	 * under SQL semantics anyway (and it's forbidden by the spec). Because
	 * that is true, we don't need to worry about evaluating the aggs in any
	 * particular order.
	 */
	aggstate->vci.css.ss.ps.qual =
		VciExecInitQual(agg->vci.cscan.scan.plan.qual, (PlanState *) aggstate, initexpr);

	/*
	 * Initialize projection info.
	 */
	aggstate->vps_ProjInfo =
		VciExecBuildProjectionInfo(aggstate->vci.css.ss.ps.plan->targetlist,
								   aggstate->vci.css.ss.ps.ps_ExprContext,
								   aggstate->vci.css.ss.ps.ps_ResultTupleSlot,
								   &aggstate->vci.css.ss.ps,
								   NULL);

	/*
	 * get the count of aggregates in targetlist and quals
	 */
	max_aggno = -1;
	foreach(l, aggstate->aggs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(l);

		max_aggno = Max(max_aggno, aggref->aggno);
	}
	aggstate->numaggs = numaggs = max_aggno + 1;

	/*
	 * If we are grouping, precompute fmgr lookup data for inner loop. We need
	 * both equality and hashing functions to do it by hashing, but only
	 * equality if not hashing.
	 */
	if (agg->numCols > 0)
	{
		if (agg->aggstrategy == AGG_HASHED)
			execTuplesHashPrepare(agg->numCols,
								  agg->grpOperators,
								  &aggstate->eqfuncoids,
								  &aggstate->hashfunctions);
		else
		{
			aggstate->eqfunctions =
				palloc0_array(ExprState *, 1);
			aggstate->eqfunctions[0] =
				execTuplesMatchPrepare(scanDesc,
									   agg->numCols,
									   agg->grpColIdx,
									   agg->grpOperators,
									   agg->grpCollations,
									   (PlanState *) aggstate);
		}
	}

	/*
	 * Set up aggregate-result storage in the output expr context, and also
	 * allocate my private per-agg working storage
	 */
	econtext = aggstate->vci.css.ss.ps.ps_ExprContext;
	econtext->ecxt_aggvalues = palloc0_array(Datum, numaggs);
	econtext->ecxt_aggnulls = palloc0_array(bool, numaggs);

	peragg = palloc0_array(VciAggStatePerAggData, numaggs);
	aggstate->peragg = peragg;

	if (agg->aggstrategy == AGG_HASHED)
	{
		int			i;
		List	   *hash_need;
		ListCell   *lc;

		/* Compute the columns we actually need to hash on */
		hash_need = find_hash_columns(aggstate);
		aggstate->num_hash_needed = list_length(hash_need);
		aggstate->hash_needed = palloc_array(int, aggstate->num_hash_needed);

		Assert(aggstate->num_hash_needed > 0);

		i = 0;
		foreach(lc, hash_need)
		{
			aggstate->hash_needed[i++] = lfirst_int(lc);

			if (aggstate->last_hash_column < lfirst_int(lc))
				aggstate->last_hash_column = lfirst_int(lc);
		}

		{
			VciScanState *scanstate = (VciScanState *) outerPlanState(aggstate);

			Assert(scanstate->vci.css.ss.ps.type == T_CustomScanState);

			aggstate->hash_input_values = palloc_array(Datum *, aggstate->num_hash_needed);
			aggstate->hash_input_isnull = palloc_array(bool *, aggstate->num_hash_needed);

			for (i = 0; i < aggstate->num_hash_needed; i++)
			{
				int			varNumber = aggstate->hash_needed[i] - 1;

				aggstate->hash_input_values[i] =
					scanstate->result_values[varNumber];

				aggstate->hash_input_isnull[i] =
					scanstate->result_isnull[varNumber];
			}
		}

		build_hash_table(aggstate);
		aggstate->table_filled = false;
	}
	else
	{
		VciAggStatePerGroup pergroup;

		pergroup = palloc0_array(VciAggStatePerGroupData, numaggs);
		aggstate->pergroup = pergroup;
	}

	/*
	 * Perform lookups of aggregate function info, and initialize the
	 * unchanging fields of the per-agg data.  We also detect duplicate
	 * aggregates (for example, "SELECT sum(x) ... HAVING sum(x) > 0"). When
	 * duplicates are detected, we only make an AggStatePerAgg struct for the
	 * first one.  The clones are simply pointed at the same result entry by
	 * giving them duplicate aggno values.
	 */
	foreach(l, aggstate->aggs)
	{
		Aggref	   *aggref = lfirst(l);
		VciAggStatePerAgg peraggstate;
		Oid			inputTypes[FUNC_MAX_ARGS];
		int			numArguments;
		int			numDirectArgs;
		int			numInputs;
		int			numSortCols;
		int			numDistinctCols;
		List	   *sortlist;
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		Oid			aggtranstype;
		AclResult	aclresult;
		Oid			transfn_oid,
					finalfn_oid;
		Expr	   *transfnexpr,
				   *finalfnexpr;
		Datum		textInitVal;

		/* Planner should have assigned aggregate to correct level */
		Assert(aggref->agglevelsup == 0);

		peraggstate = &peragg[aggref->aggno];

		/* Check if we initialized the state for this aggregate already. */
		if (peraggstate->aggref != NULL)
			continue;

		peraggstate->aggref = aggref;
		peraggstate->sortstate = NULL;

		/* Fetch the pg_aggregate row */
		aggTuple = SearchSysCache1(AGGFNOID,
								   ObjectIdGetDatum(aggref->aggfnoid));
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 aggref->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		/* Check permission to call aggregate function */
		aclresult = object_aclcheck(ProcedureRelationId, aggref->aggfnoid, GetUserId(),
									ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_AGGREGATE,
						   get_func_name(aggref->aggfnoid));
		InvokeFunctionExecuteHook(aggref->aggfnoid);

		peraggstate->transfn_oid = transfn_oid = aggform->aggtransfn;
		peraggstate->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		/* Check that aggregate owner has permission to call component fns */
		{
			HeapTuple	procTuple;
			Oid			aggOwner;

			procTuple = SearchSysCache1(PROCOID,
										ObjectIdGetDatum(aggref->aggfnoid));
			if (!HeapTupleIsValid(procTuple))
				elog(ERROR, "cache lookup failed for function %u",
					 aggref->aggfnoid);
			aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
			ReleaseSysCache(procTuple);

			aclresult = object_aclcheck(ProcedureRelationId, transfn_oid, aggOwner,
										ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_AGGREGATE,
							   get_func_name(transfn_oid));
			InvokeFunctionExecuteHook(transfn_oid);
			if (OidIsValid(finalfn_oid))
			{
				aclresult = object_aclcheck(ProcedureRelationId, finalfn_oid, aggOwner,
											ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_AGGREGATE,
								   get_func_name(finalfn_oid));
				InvokeFunctionExecuteHook(finalfn_oid);
			}
		}

		/*
		 * Get actual datatypes of the (nominal) aggregate inputs.  These
		 * could be different from the agg's declared input types, when the
		 * agg accepts ANY or a polymorphic type.
		 */
		numArguments = get_aggregate_argtypes(aggref, inputTypes);
		peraggstate->numArguments = numArguments;

		/* Count the "direct" arguments, if any */
		numDirectArgs = list_length(aggref->aggdirectargs);

		/* Count the number of aggregated input columns */
		numInputs = list_length(aggref->args);
		peraggstate->numInputs = numInputs;

		Assert(!AGGKIND_IS_ORDERED_SET(aggref->aggkind));
		Assert(!aggform->aggfinalextra);

		peraggstate->numTransInputs = numArguments;
		peraggstate->numFinalArgs = numDirectArgs + 1;

		/* resolve actual type of transition state, if polymorphic */
		aggtranstype = resolve_aggregate_transtype(aggref->aggfnoid,
												   aggform->aggtranstype,
												   inputTypes,
												   numArguments);

		/* build expression trees using actual argument & result types */
		build_aggregate_transfn_expr(inputTypes,
									 numArguments,
									 numDirectArgs,
									 aggref->aggvariadic,
									 aggtranstype,
									 aggref->inputcollid,
									 transfn_oid,
									 InvalidOid,	/* invtrans is not needed
													 * here */
									 &transfnexpr,
									 NULL);

		/* set up infrastructure for calling the transfn */
		fmgr_info(transfn_oid, &peraggstate->transfn);
		fmgr_info_set_expr((Node *) transfnexpr, &peraggstate->transfn);

		if (OidIsValid(finalfn_oid))
		{
			build_aggregate_finalfn_expr(inputTypes,
										 peragg->numFinalArgs,
										 aggtranstype,
										 aggref->aggtype,
										 aggref->inputcollid,
										 finalfn_oid,
										 &finalfnexpr);

			/* set up infrastructure for calling the finalfn */
			fmgr_info(finalfn_oid, &peraggstate->finalfn);
			fmgr_info_set_expr((Node *) finalfnexpr, &peraggstate->finalfn);
		}

		peraggstate->aggCollation = aggref->inputcollid;

		peraggstate->transfn_fcinfo =
			(FunctionCallInfo) palloc(SizeForFunctionCallInfo(peraggstate->numTransInputs + 1));
		InitFunctionCallInfoData(*peraggstate->transfn_fcinfo,
								 &peraggstate->transfn,
								 peraggstate->numTransInputs + 1,
								 peraggstate->aggCollation,
								 (void *) aggstate->pseudo_aggstate, NULL);

		/* get info about relevant datatypes */
		get_typlenbyval(aggref->aggtype,
						&peraggstate->resulttypeLen,
						&peraggstate->resulttypeByVal);
		get_typlenbyval(aggtranstype,
						&peraggstate->transtypeLen,
						&peraggstate->transtypeByVal);

		/*
		 * initval is potentially null, so don't try to access it as a struct
		 * field. Must do it the hard way with SysCacheGetAttr.
		 */
		textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
									  Anum_pg_aggregate_agginitval,
									  &peraggstate->initValueIsNull);

		if (peraggstate->initValueIsNull)
			peraggstate->initValue = (Datum) 0;
		else
			peraggstate->initValue = GetAggInitVal(textInitVal,
												   aggtranstype);

		/*
		 * If the transfn is strict and the initval is NULL, make sure input
		 * type and transtype are the same (or at least binary-compatible), so
		 * that it's OK to use the first aggregated input value as the initial
		 * transValue.  This should have been checked at agg definition time,
		 * but we must check again in case the transfn's strictness property
		 * has been changed.
		 */
		if (peraggstate->transfn.fn_strict && peraggstate->initValueIsNull)
		{
			if (numArguments <= numDirectArgs ||
				!IsBinaryCoercible(inputTypes[numDirectArgs], aggtranstype))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate %u needs to have compatible input type and transition type",
								aggref->aggfnoid)));
		}

		/*
		 * Get a tupledesc corresponding to the aggregated inputs (including
		 * sort expressions) of the agg.
		 */
		peraggstate->evaldesc = ExecTypeFromTL(aggref->args);

		/* Create slot we're going to do argument evaluation in */
		peraggstate->evalslot = ExecInitExtraTupleSlot(estate, peraggstate->evaldesc, &TTSOpsMinimalTuple);

		/* Set up projection info for evaluation */
		peraggstate->evalproj = VciExecBuildProjectionInfo(aggref->args,
														   aggstate->tmpcontext,
														   peraggstate->evalslot,
														   &aggstate->vci.css.ss.ps,
														   NULL);

		Assert(!AGGKIND_IS_ORDERED_SET(aggref->aggkind));
		Assert(!aggref->aggdistinct);

		sortlist = aggref->aggorder;
		numSortCols = list_length(sortlist);
		numDistinctCols = 0;

		peraggstate->numSortCols = numSortCols;
		peraggstate->numDistinctCols = numDistinctCols;

		Assert(numSortCols == 0);

		Assert(aggref->aggdistinct == NIL);

		ReleaseSysCache(aggTuple);
	}

	if (agg->aggstrategy == AGG_HASHED)
	{
		vci_agg_BeginCustomPlan_postprocess_for_advance_aggref(aggstate);
		vci_agg_BeginCustomPlan_postprocess_for_vp(aggstate, econtext);
	}

	/* Recopy dummy AggState */
	copy_into_pseudo_aggstate(aggstate->pseudo_aggstate, aggstate);
}

/**
 * Create and connect a pseudo Agg state to VCI Agg State
 */
static void
vci_agg_BeginCustomPlan_preprocess(VciAggState *aggstate)
{
	AggState   *pseudo_aggstate;

	/*
	 * Create dummy AggState
	 *
	 * aggregation function registered in pg_proc system catalog checks if
	 * Execution Plan State Node is AggState or WindowsAggState. VciAggState
	 * is not considered an AggState because it is a CustomPlanState. The
	 * dummy AggState is used to fool Execution Plan State Node seen by
	 * aggregation function.
	 *
	 * Since it is necessary to set aggstate->pseudo_aggstate at the stage
	 * when the AggrefState is initialized, insert it before
	 * vci_agg_BeginCustomPlan.
	 */
	pseudo_aggstate = makeNode(AggState);

	/* only one (no grouping setsallowed) */
	pseudo_aggstate->aggcontexts =
		palloc0_array(ExprContext *, 1);
	ExecAssignExprContext(aggstate->vci.css.ss.ps.state, &aggstate->vci.css.ss.ps);
	pseudo_aggstate->aggcontexts[0] = aggstate->vci.css.ss.ps.ps_ExprContext;
	pseudo_aggstate->curaggcontext = pseudo_aggstate->aggcontexts[0];

	pseudo_aggstate->phases = palloc0_object(AggStatePerPhaseData);
	pseudo_aggstate->phases[0].grouped_cols = NULL;
	pseudo_aggstate->phases[0].sortnode = NULL;
	pseudo_aggstate->phases[0].numsets = 0;
	pseudo_aggstate->phases[0].gset_lengths = NULL;

	pseudo_aggstate->perhash = palloc0_object(AggStatePerHashData);

	copy_into_pseudo_aggstate(pseudo_aggstate, aggstate);
	aggstate->pseudo_aggstate = pseudo_aggstate;
}

/**
 * Replace transition function for each Aggref with an optimized version.
 */
static void
vci_agg_BeginCustomPlan_postprocess_for_advance_aggref(VciAggState *aggstate)
{
	for (int aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		VciAggStatePerAgg peraggstate = &aggstate->peragg[aggno];

		peraggstate->advance_aggref = VciGetSpecialAdvanceAggrefFunc(peraggstate);
	}
}

/**
 * Create vector processing context from targetlist to execute vector processing
 */
static void
vci_agg_BeginCustomPlan_postprocess_for_vp(VciAggState *aggstate, ExprContext *econtext)
{
	VciScanState *scansate = vci_search_scan_state(&aggstate->vci);
	uint16	   *skip_list;

	skip_list = vci_CSGetSkipAddrFromVirtualTuples(scansate->vector_set);

	for (int aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		VciAggStatePerAgg peraggstate = &aggstate->peragg[aggno];
		VciProjectionInfo *proj = peraggstate->evalproj;

		if (proj->pi_tle_array_len > 0)
			proj->pi_vp_tle_array = palloc0_array(VciVPContext *, proj->pi_tle_array_len);

		for (int i = 0; i < proj->pi_tle_array_len; i++)
		{
			TargetEntry *tle;

			tle = (TargetEntry *) proj->pi_tle_array[i];

			proj->pi_vp_tle_array[i] =
				VciBuildVectorProcessing(tle->expr, (PlanState *) aggstate,
										 econtext, skip_list);
		}
	}
}

/* ----------------
 *		vci_ExecFreeExprContext
 *
 * A plan node's ExprContext should be freed explicitly during executor
 * shutdown because there may be shutdown callbacks to call.  (Other resources
 * made by the above routines, such as projection info, don't need to be freed
 * explicitly because they're just memory in the per-query memory context.)
 */
static void
vci_ExecFreeExprContext(PlanState *planstate)
{
	/*
	 * Per above discussion, don't actually delete the ExprContext. We do
	 * unlink it from the plan node, though.
	 */
	planstate->ps_ExprContext = NULL;
}

/**
 * EndCustomPlan callback called from CustomPlanState of VCI Agg
 */
static void
vci_agg_EndCustomPlan(CustomScanState *node)
{
	VciAggState *aggstate;
	PlanState  *outerPlan;

	aggstate = (VciAggState *) node;

	node = (CustomScanState *) aggstate;

	/* And ensure any agg shutdown callbacks have been called */
	ReScanExprContext(aggstate->vci.css.ss.ps.ps_ExprContext);

	/*
	 * Free both the expr contexts.
	 */
	vci_ExecFreeExprContext(&aggstate->vci.css.ss.ps);
	node->ss.ps.ps_ExprContext = aggstate->tmpcontext;
	vci_ExecFreeExprContext(&aggstate->vci.css.ss.ps);

	MemoryContextDelete(aggstate->aggcontext);

	/* Release hash tables too */
	if (aggstate->hash_metacxt != NULL)
	{
		MemoryContextDelete(aggstate->hash_metacxt);
		aggstate->hash_metacxt = NULL;
	}
	if (aggstate->hash_tuplescxt != NULL)
	{
		MemoryContextDelete(aggstate->hash_tuplescxt);
		aggstate->hash_tuplescxt = NULL;
	}

	outerPlan = outerPlanState(node);

	ExecEndNode(outerPlan);
}

/**
 * ReScanCustomPlan callback called from CustomPlanState of VCI Agg
 */
static void
vci_agg_ReScanCustomPlan(CustomScanState *node)
{
	VciAggState *aggstate;
	ExprContext *econtext;

	aggstate = (VciAggState *) node;

	econtext = aggstate->vci.css.ss.ps.ps_ExprContext;

	aggstate->agg_done = false;

	if (((VciAgg *) aggstate->vci.css.ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		/*
		 * In the hashed case, if we haven't yet built the hash table then we
		 * can just return; nothing done yet, so nothing to undo. If subnode's
		 * chgParam is not NULL then it will be re-scanned by ExecProcNode,
		 * else no reason to re-scan it at all.
		 */
		if (!aggstate->table_filled)
			return;

		/*
		 * If we do have the hash table and the subplan does not have any
		 * parameter changes, then we can just rescan the existing hash table;
		 * no need to build it again.
		 */
		if (aggstate->vci.css.ss.ps.lefttree->chgParam == NULL)
		{
			ResetTupleHashIterator(aggstate->hashtable, &aggstate->hashiter);
			return;
		}
	}

	/* We don't need to ReScanExprContext here; ExecReScan already did it */

	/* Release first tuple of group, if we have made a copy */
	if (aggstate->grp_firstTuple != NULL)
	{
		heap_freetuple(aggstate->grp_firstTuple);
		aggstate->grp_firstTuple = NULL;
	}

	/* Forget current agg values */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * aggstate->numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * aggstate->numaggs);

	/*
	 * Release all temp storage. Note that with AGG_HASHED, the hash table is
	 * allocated in a sub-context of the hash_metacxt. We're going to rebuild
	 * the hash table from scratch, so we need to use MemoryContextReset() to
	 * avoid leaking the old hash table's memory context header.
	 */
	MemoryContextReset(aggstate->aggcontext);

	Assert(IsA(aggstate->vci.css.ss.ps.plan, CustomScan));

	if (((VciAgg *) aggstate->vci.css.ss.ps.plan)->aggstrategy == AGG_HASHED)
	{
		MemoryContextReset(aggstate->hash_metacxt);
		MemoryContextReset(aggstate->hash_tuplescxt);

		/* Rebuild an empty hash table */
		build_hash_table(aggstate);
		aggstate->table_filled = false;
	}
	else
	{
		/*
		 * Reset the per-group state (in particular, mark transvalues null)
		 */
		MemSet(aggstate->pergroup, 0,
			   sizeof(VciAggStatePerGroupData) * aggstate->numaggs);
	}

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (aggstate->vci.css.ss.ps.lefttree->chgParam == NULL)
		ExecReScan(aggstate->vci.css.ss.ps.lefttree);
}

/* LCOV_EXCL_START */

/**
 * MarkPosCustomPlan callback called by CustomPlanState of VCI Agg
 */
static void
vci_agg_MarkPosCustomPlan(CustomScanState *node)
{
	elog(PANIC, "VCI Agg does not support MarkPosCustomPlan call convention");
}

/**
 * RestrPosCustomPlan callback called by CustomPlanState of VCI Agg
 */
static void
vci_agg_RestrPosCustomPlan(CustomScanState *node)
{
	elog(PANIC, "VCI Agg does not support RestrPosCustomPlan call convention");
}

/* LCOV_EXCL_STOP */

/**
 * ExplainCustomPlan callback called by CustomPlanState of VCI Agg
 */
static void
vci_agg_ExplainCustomPlan(CustomScanState *cpstate,
						  List *ancestors,
						  ExplainState *es)
{
	VciAgg	   *agg = (VciAgg *) cpstate->ss.ps.plan;

	if (agg->numCols > 0)
	{
		/* The key columns refer to the tlist of the child plan */
		ancestors = lcons(&cpstate->ss.ps, ancestors);

		ExplainPropertySortGroupKeys(outerPlanState(&cpstate->ss.ps), "Group Key",
									 agg->numCols, agg->grpColIdx,
									 ancestors, es);
		ancestors = list_delete_first(ancestors);
	}
}

/**
 * CopyCustomPlan callback called by CustomPlan of VCI Agg
 */
static CustomScan *
vci_agg_CopyCustomPlan(const CustomScan *_from)
{
	const VciAgg *from = (const VciAgg *) _from;
	VciAgg	   *newnode = (VciAgg *) newNode(sizeof(VciAgg), _from->scan.plan.type);
	int			numCols;

	vci_copy_plan(&newnode->vci, &from->vci);

	newnode->aggstrategy = from->aggstrategy;

	numCols = from->numCols;
	newnode->numCols = numCols;
	if (numCols > 0)
	{
		newnode->grpColIdx = palloc_array(AttrNumber, numCols);
		newnode->grpOperators = palloc_array(Oid, numCols);
		newnode->grpCollations = palloc_array(Oid, numCols);
		for (int i = 0; i < numCols; i++)
		{
			newnode->grpColIdx[i] = from->grpColIdx[i];
			newnode->grpOperators[i] = from->grpOperators[i];
			newnode->grpCollations[i] = from->grpCollations[i];
		}
	}
	newnode->numGroups = from->numGroups;

	((Node *) newnode)->type = nodeTag((Node *) from);

	return &newnode->vci.cscan;
}

CustomScanMethods vci_agg_scan_methods = {
	"VCI Aggregate",
	vci_agg_CreateCustomScanState,
	vci_agg_CopyCustomPlan
};

CustomScanMethods vci_hashagg_scan_methods = {
	"VCI HashAggregate",
	vci_agg_CreateCustomScanState,
	vci_agg_CopyCustomPlan
};

CustomScanMethods vci_groupagg_scan_methods = {
	"VCI GroupAggregate",
	vci_agg_CreateCustomScanState,
	vci_agg_CopyCustomPlan
};

/**
 * VCI Agg's CustomPlanMethods callbacks
 */
CustomExecMethods vci_agg_exec_methods = {
	"VCI Aggregate",
	vci_agg_BeginCustomPlan,
	vci_agg_ExecCustomPlan,
	vci_agg_EndCustomPlan,
	vci_agg_ReScanCustomPlan,
	vci_agg_MarkPosCustomPlan,
	vci_agg_RestrPosCustomPlan,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	vci_agg_ExplainCustomPlan,
	NULL,
	NULL
};

/**
 * VCI Agg's CustomPlanMethods callbacks
 */
CustomExecMethods vci_hashagg_exec_methods = {
	"VCI HashAggregate",
	vci_agg_BeginCustomPlan,
	vci_agg_ExecCustomPlan,
	vci_agg_EndCustomPlan,
	vci_agg_ReScanCustomPlan,
	vci_agg_MarkPosCustomPlan,
	vci_agg_RestrPosCustomPlan,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	vci_agg_ExplainCustomPlan,
	NULL,
	NULL
};

/**
 * VCI Agg's CustomPlanMethods callbacks
 */
CustomExecMethods vci_groupagg_exec_methods = {
	"VCI GroupAggregate",
	vci_agg_BeginCustomPlan,
	vci_agg_ExecCustomPlan,
	vci_agg_EndCustomPlan,
	vci_agg_ReScanCustomPlan,
	vci_agg_MarkPosCustomPlan,
	vci_agg_RestrPosCustomPlan,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	vci_agg_ExplainCustomPlan,
	NULL,
	NULL
};
