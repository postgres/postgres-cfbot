/*-------------------------------------------------------------------------
 *
 * nodeRecursiveunion.c
 *	  routines to handle RecursiveUnion nodes.
 *
 * To implement UNION (without ALL), we need a hashtable that stores tuples
 * already seen.  The hash key is computed from the grouping columns.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeRecursiveunion.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeRecursiveunion.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/sortsupport.h"



/*
 * Initialize the hash table to empty.
 */
static void
build_hash_table(RecursiveUnionState *rustate)
{
	RecursiveUnion *node = (RecursiveUnion *) rustate->ps.plan;
	TupleDesc	desc = ExecGetResultType(outerPlanState(rustate));

	Assert(node->numCols > 0);

	/*
	 * If both child plans deliver the same fixed tuple slot type, we can tell
	 * BuildTupleHashTable to expect that slot type as input.  Otherwise,
	 * we'll pass NULL denoting that any slot type is possible.
	 */
	rustate->hashtable = BuildTupleHashTable(&rustate->ps,
											 desc,
											 ExecGetCommonChildSlotOps(&rustate->ps),
											 node->numCols,
											 node->dupColIdx,
											 rustate->eqfuncoids,
											 rustate->hashfunctions,
											 node->dupCollations,
											 node->numGroups,
											 0,
											 rustate->ps.state->es_query_cxt,
											 rustate->tuplesContext,
											 rustate->tempContext,
											 false);
}


/* ----------------------------------------------------------------
 *		ExecRecursiveUnion(node)
 *
 *		Scans the recursive query sequentially and returns the next
 *		qualifying tuple.
 *
 * 1. evaluate non recursive term and assign the result to RT
 *
 * 2. execute recursive terms
 *
 * 2.1 WT := RT
 * 2.2 while WT is not empty repeat 2.3 to 2.6. if WT is empty returns RT
 * 2.3 replace the name of recursive term with WT
 * 2.4 evaluate the recursive term and store into WT
 * 2.5 append WT to RT
 * 2.6 go back to 2.2
 * ----------------------------------------------------------------
 */
static void
populate_result_table_from_hash(RecursiveUnionState *rustate)
{
	TupleHashTable hashtable = rustate->hashtable;
	tuplehash_iterator iter;
	TupleHashEntry entry;
	TupleTableSlot *slot = rustate->ps.ps_ResultTupleSlot;

	tuplehash_start_iterate(hashtable->hashtab, &iter);
	while ((entry = tuplehash_iterate(hashtable->hashtab, &iter)) != NULL)
	{
		ExecStoreMinimalTuple(entry->firstTuple, slot, false);
		tuplestore_puttupleslot(rustate->result_table, slot);
		ExecClearTuple(slot);
	}
}

static TupleTableSlot *
ExecRecursiveUnion(PlanState *pstate)
{
	RecursiveUnionState *node = castNode(RecursiveUnionState, pstate);
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;
	TupleTableSlot *slot;
	bool		isnew;

	CHECK_FOR_INTERRUPTS();

	/* If we need sorting, we must buffer and return from result_table */
	if (plan->numSortCols > 0)
	{
		if (node->result_table == NULL)
		{
			/* Run the entire recursion loop and buffer results */
			node->result_table = tuplestore_begin_heap(false, false, work_mem);

			/* 1. Process non-recursive term */
			for (;;)
			{
				slot = ExecProcNode(outerPlan);
				if (TupIsNull(slot))
					break;

				if (plan->numCols > 0)
				{
					TupleHashEntry entry;
					entry = LookupTupleHashEntry(node->hashtable, slot, &isnew, NULL);
					if (!isnew)
					{
						ReplaceTupleHashEntryIfBetter(node->hashtable,
													  entry,
													  slot,
													  node->sort_firstTupleSlot,
													  node->sortKeys,
													  plan->numSortCols);
						continue;
					}
				}
				tuplestore_puttupleslot(node->working_table, slot);
			}

			/* 2. Process recursive term */
			node->recursing = true;
			for (;;)
			{
				slot = ExecProcNode(innerPlan);
				if (TupIsNull(slot))
				{
					Tuplestorestate *swaptemp;

					if (node->intermediate_empty)
						break; /* End of recursion */

					tuplestore_clear(node->working_table);
					swaptemp = node->working_table;
					node->working_table = node->intermediate_table;
					node->intermediate_table = swaptemp;
					node->intermediate_empty = true;
					innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
														 plan->wtParam);
					continue;
				}

				if (plan->numCols > 0)
				{
					TupleHashEntry entry;
					entry = LookupTupleHashEntry(node->hashtable, slot, &isnew, NULL);
					if (!isnew)
					{
						bool replaced = ReplaceTupleHashEntryIfBetter(node->hashtable,
																	  entry,
																	  slot,
																	  node->sort_firstTupleSlot,
																	  node->sortKeys,
																	  plan->numSortCols);
						if (replaced)
						{
							/* Replaced! Explore this better path */
							node->intermediate_empty = false;
							tuplestore_puttupleslot(node->intermediate_table, slot);
						}
						continue;
					}
				}

				node->intermediate_empty = false;
				tuplestore_puttupleslot(node->intermediate_table, slot);
			}

			/* Populate result_table from hashtable */
			populate_result_table_from_hash(node);
		}

		/* Read from result_table */
		slot = node->ps.ps_ResultTupleSlot;
		if (tuplestore_gettupleslot(node->result_table, true, false, slot))
			return slot;

		return NULL;
	}

	/* Original pipelined behavior (numSortCols == 0) */
	/* 1. Evaluate non-recursive term */
	if (!node->recursing)
	{
		for (;;)
		{
			slot = ExecProcNode(outerPlan);
			if (TupIsNull(slot))
				break;
			if (plan->numCols > 0)
			{
				/* Find or build hashtable entry for this tuple's group */
				LookupTupleHashEntry(node->hashtable, slot, &isnew, NULL);
				/* Must reset temp context after each hashtable lookup */
				MemoryContextReset(node->tempContext);
				/* Ignore tuple if already seen */
				if (!isnew)
					continue;
			}
			/* Each non-duplicate tuple goes to the working table ... */
			tuplestore_puttupleslot(node->working_table, slot);
			/* ... and to the caller */
			return slot;
		}
		node->recursing = true;
	}

	/* 2. Execute recursive term */
	for (;;)
	{
		slot = ExecProcNode(innerPlan);
		if (TupIsNull(slot))
		{
			Tuplestorestate *swaptemp;

			/* Done if there's nothing in the intermediate table */
			if (node->intermediate_empty)
				break;

			/*
			 * Now we let the intermediate table become the work table.  We
			 * need a fresh intermediate table, so delete the tuples from the
			 * current working table and use that as the new intermediate
			 * table.  This saves a round of free/malloc from creating a new
			 * tuple store.
			 */
			tuplestore_clear(node->working_table);

			swaptemp = node->working_table;
			node->working_table = node->intermediate_table;
			node->intermediate_table = swaptemp;

			/* mark the intermediate table as empty */
			node->intermediate_empty = true;

			/* reset the recursive term */
			innerPlan->chgParam = bms_add_member(innerPlan->chgParam,
												 plan->wtParam);

			/* and continue fetching from recursive term */
			continue;
		}

		if (plan->numCols > 0)
		{
			/* Find or build hashtable entry for this tuple's group */
			LookupTupleHashEntry(node->hashtable, slot, &isnew, NULL);
			/* Must reset temp context after each hashtable lookup */
			MemoryContextReset(node->tempContext);
			/* Ignore tuple if already seen */
			if (!isnew)
				continue;
		}

		/* Else, tuple is good; stash it in intermediate table ... */
		node->intermediate_empty = false;
		tuplestore_puttupleslot(node->intermediate_table, slot);
		/* ... and return it */
		return slot;
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitRecursiveUnion
 * ----------------------------------------------------------------
 */
RecursiveUnionState *
ExecInitRecursiveUnion(RecursiveUnion *node, EState *estate, int eflags)
{
	RecursiveUnionState *rustate;
	ParamExecData *prmdata;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	rustate = makeNode(RecursiveUnionState);
	rustate->ps.plan = (Plan *) node;
	rustate->ps.state = estate;
	rustate->ps.ExecProcNode = ExecRecursiveUnion;

	rustate->eqfuncoids = NULL;
	rustate->hashfunctions = NULL;
	rustate->hashtable = NULL;
	rustate->tempContext = NULL;
	rustate->tuplesContext = NULL;
	rustate->result_table = NULL;
	rustate->sortKeys = NULL;
	rustate->sort_firstTupleSlot = NULL;

	/* initialize processing state */
	rustate->recursing = false;
	rustate->intermediate_empty = true;
	rustate->working_table = tuplestore_begin_heap(false, false, work_mem);
	rustate->intermediate_table = tuplestore_begin_heap(false, false, work_mem);

	/*
	 * If hashing, we need a per-tuple memory context for comparisons, and a
	 * longer-lived context to store the hash table.  The table can't just be
	 * kept in the per-query context because we want to be able to throw it
	 * away when rescanning.  We can use a BumpContext to save storage,
	 * because we will have no need to delete individual table entries.
	 */
	if (node->numCols > 0)
	{
		rustate->tempContext =
			AllocSetContextCreate(CurrentMemoryContext,
								  "RecursiveUnion",
								  ALLOCSET_DEFAULT_SIZES);
		if (node->numSortCols > 0)
		{
			rustate->tuplesContext =
				AllocSetContextCreate(CurrentMemoryContext,
									  "RecursiveUnion hashed tuples",
									  ALLOCSET_DEFAULT_SIZES);
		}
		else
		{
			rustate->tuplesContext =
				BumpContextCreate(CurrentMemoryContext,
								  "RecursiveUnion hashed tuples",
								  ALLOCSET_DEFAULT_SIZES);
		}
	}

	/*
	 * Make the state structure available to descendant WorkTableScan nodes
	 * via the Param slot reserved for it.
	 */
	prmdata = &(estate->es_param_exec_vals[node->wtParam]);
	Assert(prmdata->execPlan == NULL);
	prmdata->value = PointerGetDatum(rustate);
	prmdata->isnull = false;

	/*
	 * Miscellaneous initialization
	 *
	 * RecursiveUnion plans don't have expression contexts because they never
	 * call ExecQual or ExecProject.
	 */
	Assert(node->plan.qual == NIL);

	/*
	 * RecursiveUnion nodes still have Result slots, which hold pointers to
	 * tuples, so we have to initialize them.
	 */
	ExecInitResultTypeTL(&rustate->ps);
	if (node->numSortCols > 0)
		ExecInitResultSlot(&rustate->ps, &TTSOpsMinimalTuple);

	/*
	 * Initialize result tuple type.  (Note: we have to set up the result type
	 * before initializing child nodes, because nodeWorktablescan.c expects it
	 * to be valid.)
	 */
	rustate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize child nodes
	 */
	outerPlanState(rustate) = ExecInitNode(outerPlan(node), estate, eflags);
	innerPlanState(rustate) = ExecInitNode(innerPlan(node), estate, eflags);

	/*
	 * If hashing, precompute fmgr lookup data for inner loop, and create the
	 * hash table.
	 */
	if (node->numCols > 0)
	{
		execTuplesHashPrepare(node->numCols,
							  node->dupOperators,
							  &rustate->eqfuncoids,
							  &rustate->hashfunctions);
		build_hash_table(rustate);
	}

	if (node->numSortCols > 0)
	{
		TupleDesc	desc = ExecGetResultType(outerPlanState(rustate));
		int			i;

		rustate->sortKeys = (SortSupportData *) palloc0(node->numSortCols * sizeof(SortSupportData));
		rustate->sort_firstTupleSlot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsMinimalTuple);

		for (i = 0; i < node->numSortCols; i++)
		{
			SortSupport skey = &rustate->sortKeys[i];

			skey->ssup_cxt = CurrentMemoryContext;
			skey->ssup_collation = node->sortCollations[i];
			skey->ssup_nulls_first = node->sortNullsFirst[i];
			skey->ssup_attno = node->sortColIdx[i];
			PrepareSortSupportFromOrderingOp(node->sortOperators[i], skey);
		}
	}

	return rustate;
}

/* ----------------------------------------------------------------
 *		ExecEndRecursiveUnion
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndRecursiveUnion(RecursiveUnionState *node)
{
	/* Release tuplestores */
	tuplestore_end(node->working_table);
	tuplestore_end(node->intermediate_table);

	/* free subsidiary stuff including hashtable data */
	if (node->tempContext)
		MemoryContextDelete(node->tempContext);
	if (node->tuplesContext)
		MemoryContextDelete(node->tuplesContext);
	if (node->result_table)
		tuplestore_end(node->result_table);

	/*
	 * close down subplans
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

/* ----------------------------------------------------------------
 *		ExecReScanRecursiveUnion
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanRecursiveUnion(RecursiveUnionState *node)
{
	PlanState  *outerPlan = outerPlanState(node);
	PlanState  *innerPlan = innerPlanState(node);
	RecursiveUnion *plan = (RecursiveUnion *) node->ps.plan;

	/*
	 * Set recursive term's chgParam to tell it that we'll modify the working
	 * table and therefore it has to rescan.
	 */
	innerPlan->chgParam = bms_add_member(innerPlan->chgParam, plan->wtParam);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.  Because of above, we only have to do this to the
	 * non-recursive term.
	 */
	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);

	/* Empty hashtable if needed */
	if (plan->numCols > 0)
		ResetTupleHashTable(node->hashtable);

	/* reset processing state */
	node->recursing = false;
	node->intermediate_empty = true;
	tuplestore_clear(node->working_table);
	tuplestore_clear(node->intermediate_table);
	if (node->result_table)
	{
		tuplestore_end(node->result_table);
		node->result_table = NULL;
	}
}
