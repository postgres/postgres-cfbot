/*-------------------------------------------------------------------------
 *
 * execIndexing.c
 *	  routines for inserting index tuples and enforcing unique and
 *	  exclusion constraints.
 *
 * ExecInsertIndexTuples() is the main entry point.  It's called after
 * inserting a tuple to the heap, and it inserts corresponding index tuples
 * into all indexes.  At the same time, it enforces any unique and
 * exclusion constraints:
 *
 * Unique Indexes
 * --------------
 *
 * Enforcing a unique constraint is straightforward.  When the index AM
 * inserts the tuple to the index, it also checks that there are no
 * conflicting tuples in the index already.  It does so atomically, so that
 * even if two backends try to insert the same key concurrently, only one
 * of them will succeed.  All the logic to ensure atomicity, and to wait
 * for in-progress transactions to finish, is handled by the index AM.
 *
 * If a unique constraint is deferred, we request the index AM to not
 * throw an error if a conflict is found.  Instead, we make note that there
 * was a conflict and return the list of indexes with conflicts to the
 * caller.  The caller must re-check them later, by calling index_insert()
 * with the UNIQUE_CHECK_EXISTING option.
 *
 * Exclusion Constraints
 * ---------------------
 *
 * Exclusion constraints are different from unique indexes in that when the
 * tuple is inserted to the index, the index AM does not check for
 * duplicate keys at the same time.  After the insertion, we perform a
 * separate scan on the index to check for conflicting tuples, and if one
 * is found, we throw an error and the transaction is aborted.  If the
 * conflicting tuple's inserter or deleter is in-progress, we wait for it
 * to finish first.
 *
 * There is a chance of deadlock, if two backends insert a tuple at the
 * same time, and then perform the scan to check for conflicts.  They will
 * find each other's tuple, and both try to wait for each other.  The
 * deadlock detector will detect that, and abort one of the transactions.
 * That's fairly harmless, as one of them was bound to abort with a
 * "duplicate key error" anyway, although you get a different error
 * message.
 *
 * If an exclusion constraint is deferred, we still perform the conflict
 * checking scan immediately after inserting the index tuple.  But instead
 * of throwing an error if a conflict is found, we return that information
 * to the caller.  The caller must re-check them later by calling
 * check_exclusion_constraint().
 *
 * Speculative insertion
 * ---------------------
 *
 * Speculative insertion is a two-phase mechanism used to implement
 * INSERT ... ON CONFLICT.  The tuple is first inserted into the heap
 * and the indexes are updated as usual, but if a constraint is violated,
 * we can still back out of the insertion without aborting the whole
 * transaction.  In an INSERT ... ON CONFLICT statement, if a conflict is
 * detected, the inserted tuple is backed out and the ON CONFLICT action is
 * executed instead.
 *
 * Insertion to a unique index works as usual: the index AM checks for
 * duplicate keys atomically with the insertion.  But instead of throwing
 * an error on a conflict, the speculatively inserted heap tuple is backed
 * out.
 *
 * Exclusion constraints are slightly more complicated.  As mentioned
 * earlier, there is a risk of deadlock when two backends insert the same
 * key concurrently.  That was not a problem for regular insertions, when
 * one of the transactions has to be aborted anyway, but with a speculative
 * insertion we cannot let a deadlock happen, because we only want to back
 * out the speculatively inserted tuple on conflict, not abort the whole
 * transaction.
 *
 * When a backend detects that the speculative insertion conflicts with
 * another in-progress tuple, it has two options:
 *
 * 1. back out the speculatively inserted tuple, then wait for the other
 *	  transaction, and retry. Or,
 * 2. wait for the other transaction, with the speculatively inserted tuple
 *	  still in place.
 *
 * If two backends insert at the same time, and both try to wait for each
 * other, they will deadlock.  So option 2 is not acceptable.  Option 1
 * avoids the deadlock, but it is prone to a livelock instead.  Both
 * transactions will wake up immediately as the other transaction backs
 * out.  Then they both retry, and conflict with each other again, lather,
 * rinse, repeat.
 *
 * To avoid the livelock, one of the backends must back out first, and then
 * wait, while the other one waits without backing out.  It doesn't matter
 * which one backs out, so we employ an arbitrary rule that the transaction
 * with the higher XID backs out.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execIndexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/multirangetypes.h"
#include "utils/rangetypes.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* waitMode argument to check_exclusion_or_unique_constraint() */
typedef enum
{
	CEOUC_WAIT,
	CEOUC_NOWAIT,
	CEOUC_LIVELOCK_PREVENTING_WAIT,
} CEOUC_WAIT_MODE;

static bool check_exclusion_or_unique_constraint(Relation heap, Relation index,
												 IndexInfo *indexInfo,
												 const ItemPointerData *tupleid,
												 const Datum *values, const bool *isnull,
												 EState *estate, bool newIndex,
												 CEOUC_WAIT_MODE waitMode,
												 bool violationOK,
												 ItemPointer conflictTid);

static bool index_recheck_constraint(Relation index, const Oid *constr_procs,
									 const Datum *existing_values, const bool *existing_isnull,
									 const Datum *new_values);
static void ExecWithoutOverlapsNotEmpty(Relation rel, NameData attname, Datum attval,
										char typtype, Oid atttypid);

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Find the indices associated with a result relation, open them,
 *		and save information about them in the result ResultRelInfo.
 *
 *		At entry, caller has already opened and locked
 *		resultRelInfo->ri_RelationDesc.
 * ----------------------------------------------------------------
 */
void
ExecOpenIndices(ResultRelInfo *resultRelInfo, bool speculative)
{
	Relation	resultRelation = resultRelInfo->ri_RelationDesc;
	List	   *indexoidlist;
	ListCell   *l;
	int			len,
				i;
	RelationPtr relationDescs;
	IndexInfo **indexInfoArray;

	resultRelInfo->ri_NumIndices = 0;

	/* fast path if no indexes */
	if (!RelationGetForm(resultRelation)->relhasindex)
		return;

	/*
	 * Get cached list of index OIDs
	 */
	indexoidlist = RelationGetIndexList(resultRelation);
	len = list_length(indexoidlist);
	if (len == 0)
		return;

	/* This Assert will fail if ExecOpenIndices is called twice */
	Assert(resultRelInfo->ri_IndexRelationDescs == NULL);

	/*
	 * allocate space for result arrays
	 */
	relationDescs = palloc_array(Relation, len);
	indexInfoArray = palloc_array(IndexInfo *, len);

	resultRelInfo->ri_NumIndices = len;
	resultRelInfo->ri_IndexRelationDescs = relationDescs;
	resultRelInfo->ri_IndexRelationInfo = indexInfoArray;

	/*
	 * For each index, open the index relation and save pg_index info. We
	 * acquire RowExclusiveLock, signifying we will update the index.
	 *
	 * Note: we do this even if the index is not indisready; it's not worth
	 * the trouble to optimize for the case where it isn't.
	 */
	i = 0;
	foreach(l, indexoidlist)
	{
		Oid			indexOid = lfirst_oid(l);
		Relation	indexDesc;
		IndexInfo  *ii;

		indexDesc = index_open(indexOid, RowExclusiveLock);

		/* extract index key information from the index's pg_index info */
		ii = BuildIndexInfo(indexDesc);

		/*
		 * If the indexes are to be used for speculative insertion, add extra
		 * information required by unique index entries.
		 */
		if (speculative && ii->ii_Unique && !indexDesc->rd_index->indisexclusion)
			BuildSpeculativeIndexInfo(indexDesc, ii);

		relationDescs[i] = indexDesc;
		indexInfoArray[i] = ii;
		i++;
	}

	list_free(indexoidlist);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelInfo
 * ----------------------------------------------------------------
 */
void
ExecCloseIndices(ResultRelInfo *resultRelInfo)
{
	int			i;
	int			numIndices;
	RelationPtr indexDescs;
	IndexInfo **indexInfos;

	numIndices = resultRelInfo->ri_NumIndices;
	indexDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfos = resultRelInfo->ri_IndexRelationInfo;

	for (i = 0; i < numIndices; i++)
	{
		/* This Assert will fail if ExecCloseIndices is called twice */
		Assert(indexDescs[i] != NULL);

		/* Give the index a chance to do some post-insert cleanup */
		index_insert_cleanup(indexDescs[i], indexInfos[i]);

		/* Drop lock acquired by ExecOpenIndices */
		index_close(indexDescs[i], RowExclusiveLock);

		/* Mark the index as closed */
		indexDescs[i] = NULL;
	}

	/*
	 * We don't attempt to free the IndexInfo data structures or the arrays,
	 * instead assuming that such stuff will be cleaned up automatically in
	 * FreeExecutorState.
	 */
}

/* ----------------------------------------------------------------
 *		ExecInsertIndexTuples
 *
 *		This routine takes care of inserting index tuples
 *		into all the relations indexing the result relation
 *		when a heap tuple is inserted into the result relation.
 *
 *		When EIIT_IS_UPDATE is set, the executor is performing an
 *		UPDATE.  The per-index ii_IndexUnchanged flag (populated by
 *		ExecSetIndexUnchanged()) indicates whether each index's key
 *		values are unchanged by this update.  When ii_IndexUnchanged
 *		is true, we pass indexUnchanged=true to index_insert() as a
 *		hint for bottom-up deletion optimization.
 *
 *		Unique and exclusion constraints are enforced at the same
 *		time.  This returns a list of index OIDs for any unique or
 *		exclusion constraints that are deferred and that had
 *		potential (unconfirmed) conflicts.  (if EIIT_NO_DUPE_ERROR,
 *		the same is done for non-deferred constraints, but report
 *		if conflict was speculative or deferred conflict to caller)
 *
 *		If 'arbiterIndexes' is nonempty, EIIT_NO_DUPE_ERROR applies only to
 *		those indexes.  NIL means EIIT_NO_DUPE_ERROR applies to all indexes.
 * ----------------------------------------------------------------
 */
List *
ExecInsertIndexTuples(ResultRelInfo *resultRelInfo,
					  EState *estate,
					  uint32 flags,
					  TupleTableSlot *slot,
					  List *arbiterIndexes,
					  bool *specConflict)
{
	ItemPointer tupleid = &slot->tts_tid;
	List	   *result = NIL;
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	Assert(ItemPointerIsValid(tupleid));

	/*
	 * Get information from the result relation info structure.
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/* Sanity check: slot must belong to the same rel as the resultRelInfo. */
	Assert(slot->tts_tableOid == RelationGetRelid(heapRelation));

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and index expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * for each index, form and insert the index tuple
	 */
	for (i = 0; i < numIndices; i++)
	{
		Relation	indexRelation = relationDescs[i];
		IndexInfo  *indexInfo;
		bool		applyNoDupErr;
		IndexUniqueCheck checkUnique;
		bool		indexUnchanged;
		bool		satisfiesConstraint;

		if (indexRelation == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/*
		 * UPDATE skip rule.  ExecSetIndexUnchanged populated
		 * ii_IndexNeedsUpdate for every index: true when the table AM stored
		 * an independent new version, or when any attribute the index
		 * references (key, INCLUDE, expression, or partial-predicate column)
		 * overlaps the modified-attrs bitmap.  When it is false on a
		 * non-summarizing index we skip the insert entirely; the HOT chain
		 * keeps existing entries pointing at the chain root.  Summarizing
		 * indexes always get a chance to update their block-level summaries.
		 */
		if ((flags & EIIT_IS_UPDATE) &&
			!indexInfo->ii_IndexNeedsUpdate &&
			!indexInfo->ii_Summarizing)
		{
			/*
			 * This index was skipped because its key attributes did not
			 * change.  When the overall update is a partial (selective-index)
			 * update (some other non-summarizing index did change), record
			 * the skip on this index's pgstat entry.  A classic-HOT update
			 * (no indexed attribute changed) does not reach this path --
			 * ExecInsertIndexTuples is only invoked when at least one index
			 * needs a fresh entry.
			 */
			if (flags & EIIT_PARTIAL_UPDATE)
				pgstat_count_hot_indexed_upd_skipped(indexRelation);
			continue;
		}

		/* Check for partial index */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *predicate;

			/*
			 * If predicate state not set up yet, create it (in the estate's
			 * per-query context)
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NULL)
			{
				predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * Non-skipped index under a HOT-indexed update: this index is
		 * receiving a fresh entry because one of its key attributes changed.
		 * Summarizing indexes always insert regardless of the HOT-indexed
		 * decision (same as classic HOT), so they are not counted here.  Count
		 * only now that the partial-index predicate (if any) has also passed,
		 * so a predicate-excluded partial index is not counted as matched.
		 */
		if ((flags & EIIT_PARTIAL_UPDATE) && !indexInfo->ii_Summarizing)
			pgstat_count_hot_indexed_upd_matched(indexRelation);

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		/* Check whether to apply noDupErr to this index */
		applyNoDupErr = (flags & EIIT_NO_DUPE_ERROR) &&
			(arbiterIndexes == NIL ||
			 list_member_oid(arbiterIndexes,
							 indexRelation->rd_index->indexrelid));

		/*
		 * The index AM does the actual insertion, plus uniqueness checking.
		 *
		 * For an immediate-mode unique index, we just tell the index AM to
		 * throw error if not unique.
		 *
		 * For a deferrable unique index, we tell the index AM to just detect
		 * possible non-uniqueness, and we add the index OID to the result
		 * list if further checking is needed.
		 *
		 * For a speculative insertion (used by INSERT ... ON CONFLICT), do
		 * the same as for a deferrable unique index.
		 */
		if (!indexRelation->rd_index->indisunique)
			checkUnique = UNIQUE_CHECK_NO;
		else if (applyNoDupErr)
			checkUnique = UNIQUE_CHECK_PARTIAL;
		else if (indexRelation->rd_index->indimmediate)
			checkUnique = UNIQUE_CHECK_YES;
		else
			checkUnique = UNIQUE_CHECK_PARTIAL;

		/*
		 * For UPDATE operations, use the per-index ii_IndexUnchanged flag
		 * (populated by ExecSetIndexUnchanged) to hint whether the index
		 * values are unchanged.  This helps the index AM optimize for
		 * bottom-up deletion of duplicate index entries.
		 */
		indexUnchanged = (flags & EIIT_IS_UPDATE) ?
			indexInfo->ii_IndexUnchanged : false;

		/*
		 * A fresh entry planted here under a HOT-indexed update points at the
		 * new heap-only tuple itself (tupleid), not at the chain's root the
		 * way every other index entry does; that positional distinction is
		 * what lets the read side judge staleness from the crossed-attribute
		 * bitmap without a value recheck (see hot_indexed.h).  The entry is
		 * stored as an ordinary, exact TID; the hazard it creates for BitmapAnd
		 * where an unrelated index's root-pointing entry for the same row
		 * names a different offset on the same block, is signalled per block
		 * by the heap, which sets VISIBILITYMAP_LOCATOR_SPLIT during the update
		 * (see heap_update) and which BitmapAnd reads through the table AM's
		 * bitmap_and_inexact hook (see tidbitmap.c).  Nothing marks the TID.
		 */
		satisfiesConstraint =
			index_insert(indexRelation, /* index relation */
						 values,	/* array of index Datums */
						 isnull,	/* null flags */
						 tupleid,	/* tid of heap tuple */
						 heapRelation,	/* heap relation */
						 checkUnique,	/* type of uniqueness check to do */
						 indexUnchanged,	/* UPDATE without logical change? */
						 indexInfo);	/* index AM may need this */

		/*
		 * If the index has an associated exclusion constraint, check that.
		 * This is simpler than the process for uniqueness checks since we
		 * always insert first and then check.  If the constraint is deferred,
		 * we check now anyway, but don't throw error on violation or wait for
		 * a conclusive outcome from a concurrent insertion; instead we'll
		 * queue a recheck event.  Similarly, noDupErr callers (speculative
		 * inserters) will recheck later, and wait for a conclusive outcome
		 * then.
		 *
		 * An index for an exclusion constraint can't also be UNIQUE (not an
		 * essential property, we just don't allow it in the grammar), so no
		 * need to preserve the prior state of satisfiesConstraint.
		 */
		if (indexInfo->ii_ExclusionOps != NULL)
		{
			bool		violationOK;
			CEOUC_WAIT_MODE waitMode;

			if (applyNoDupErr)
			{
				violationOK = true;
				waitMode = CEOUC_LIVELOCK_PREVENTING_WAIT;
			}
			else if (!indexRelation->rd_index->indimmediate)
			{
				violationOK = true;
				waitMode = CEOUC_NOWAIT;
			}
			else
			{
				violationOK = false;
				waitMode = CEOUC_WAIT;
			}

			satisfiesConstraint =
				check_exclusion_or_unique_constraint(heapRelation,
													 indexRelation, indexInfo,
													 tupleid, values, isnull,
													 estate, false,
													 waitMode, violationOK, NULL);
		}

		if ((checkUnique == UNIQUE_CHECK_PARTIAL ||
			 indexInfo->ii_ExclusionOps != NULL) &&
			!satisfiesConstraint)
		{
			/*
			 * The tuple potentially violates the uniqueness or exclusion
			 * constraint, so make a note of the index so that we can re-check
			 * it later.  Speculative inserters are told if there was a
			 * speculative conflict, since that always requires a restart.
			 */
			result = lappend_oid(result, RelationGetRelid(indexRelation));
			if (indexRelation->rd_index->indimmediate && specConflict)
				*specConflict = true;
		}
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecCheckIndexConstraints
 *
 *		This routine checks if a tuple violates any unique or
 *		exclusion constraints.  Returns true if there is no conflict.
 *		Otherwise returns false, and the TID of the conflicting
 *		tuple is returned in *conflictTid.
 *
 *		If 'arbiterIndexes' is given, only those indexes are checked.
 *		NIL means all indexes.
 *
 *		Note that this doesn't lock the values in any way, so it's
 *		possible that a conflicting tuple is inserted immediately
 *		after this returns.  This can be used for either a pre-check
 *		before insertion or a re-check after finding a conflict.
 *
 *		'tupleid' should be the TID of the tuple that has been recently
 *		inserted (or can be invalid if we haven't inserted a new tuple yet).
 *		This tuple will be excluded from conflict checking.
 * ----------------------------------------------------------------
 */
bool
ExecCheckIndexConstraints(ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
						  EState *estate, ItemPointer conflictTid,
						  const ItemPointerData *tupleid, List *arbiterIndexes)
{
	int			i;
	int			numIndices;
	RelationPtr relationDescs;
	Relation	heapRelation;
	IndexInfo **indexInfoArray;
	ExprContext *econtext;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ItemPointerData invalidItemPtr;
	bool		checkedIndex = false;

	ItemPointerSetInvalid(conflictTid);
	ItemPointerSetInvalid(&invalidItemPtr);

	/*
	 * Get information from the result relation info structure.
	 */
	numIndices = resultRelInfo->ri_NumIndices;
	relationDescs = resultRelInfo->ri_IndexRelationDescs;
	indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	heapRelation = resultRelInfo->ri_RelationDesc;

	/*
	 * We will use the EState's per-tuple context for evaluating predicates
	 * and index expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/*
	 * For each index, form index tuple and check if it satisfies the
	 * constraint.
	 */
	for (i = 0; i < numIndices; i++)
	{
		Relation	indexRelation = relationDescs[i];
		IndexInfo  *indexInfo;
		bool		satisfiesConstraint;

		if (indexRelation == NULL)
			continue;

		indexInfo = indexInfoArray[i];

		if (!indexInfo->ii_Unique && !indexInfo->ii_ExclusionOps)
			continue;

		/* If the index is marked as read-only, ignore it */
		if (!indexInfo->ii_ReadyForInserts)
			continue;

		/* When specific arbiter indexes requested, only examine them */
		if (arbiterIndexes != NIL &&
			!list_member_oid(arbiterIndexes,
							 indexRelation->rd_index->indexrelid))
			continue;

		if (!indexRelation->rd_index->indimmediate)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("ON CONFLICT does not support deferrable unique constraints/exclusion constraints as arbiters"),
					 errtableconstraint(heapRelation,
										RelationGetRelationName(indexRelation))));

		checkedIndex = true;

		/* Check for partial index */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *predicate;

			/*
			 * If predicate state not set up yet, create it (in the estate's
			 * per-query context)
			 */
			predicate = indexInfo->ii_PredicateState;
			if (predicate == NULL)
			{
				predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);
				indexInfo->ii_PredicateState = predicate;
			}

			/* Skip this index-update if the predicate isn't satisfied */
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * FormIndexDatum fills in its values and isnull parameters with the
		 * appropriate values for the column(s) of the index.
		 */
		FormIndexDatum(indexInfo,
					   slot,
					   estate,
					   values,
					   isnull);

		satisfiesConstraint =
			check_exclusion_or_unique_constraint(heapRelation, indexRelation,
												 indexInfo, tupleid,
												 values, isnull, estate, false,
												 CEOUC_WAIT, true,
												 conflictTid);
		if (!satisfiesConstraint)
			return false;
	}

	if (arbiterIndexes != NIL && !checkedIndex)
		elog(ERROR, "unexpected failure to find arbiter index");

	return true;
}

/*
 * Check for violation of an exclusion or unique constraint
 *
 * heap: the table containing the new tuple
 * index: the index supporting the constraint
 * indexInfo: info about the index, including the exclusion properties
 * tupleid: heap TID of the new tuple we have just inserted (invalid if we
 *		haven't inserted a new tuple yet)
 * values, isnull: the *index* column values computed for the new tuple
 * estate: an EState we can do evaluation in
 * newIndex: if true, we are trying to build a new index (this affects
 *		only the wording of error messages)
 * waitMode: whether to wait for concurrent inserters/deleters
 * violationOK: if true, don't throw error for violation
 * conflictTid: if not-NULL, the TID of the conflicting tuple is returned here
 *
 * Returns true if OK, false if actual or potential violation
 *
 * 'waitMode' determines what happens if a conflict is detected with a tuple
 * that was inserted or deleted by a transaction that's still running.
 * CEOUC_WAIT means that we wait for the transaction to commit, before
 * throwing an error or returning.  CEOUC_NOWAIT means that we report the
 * violation immediately; so the violation is only potential, and the caller
 * must recheck sometime later.  This behavior is convenient for deferred
 * exclusion checks; we need not bother queuing a deferred event if there is
 * definitely no conflict at insertion time.
 *
 * CEOUC_LIVELOCK_PREVENTING_WAIT is like CEOUC_NOWAIT, but we will sometimes
 * wait anyway, to prevent livelocking if two transactions try inserting at
 * the same time.  This is used with speculative insertions, for INSERT ON
 * CONFLICT statements. (See notes in file header)
 *
 * If violationOK is true, we just report the potential or actual violation to
 * the caller by returning 'false'.  Otherwise we throw a descriptive error
 * message here.  When violationOK is false, a false result is impossible.
 *
 * Note: The indexam is normally responsible for checking unique constraints,
 * so this normally only needs to be used for exclusion constraints.  But this
 * function is also called when doing a "pre-check" for conflicts on a unique
 * constraint, when doing speculative insertion.  Caller may use the returned
 * conflict TID to take further steps.
 */
static bool
check_exclusion_or_unique_constraint(Relation heap, Relation index,
									 IndexInfo *indexInfo,
									 const ItemPointerData *tupleid,
									 const Datum *values, const bool *isnull,
									 EState *estate, bool newIndex,
									 CEOUC_WAIT_MODE waitMode,
									 bool violationOK,
									 ItemPointer conflictTid)
{
	Oid		   *constr_procs;
	uint16	   *constr_strats;
	Oid		   *index_collations = index->rd_indcollation;
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	IndexScanDesc index_scan;
	ScanKeyData scankeys[INDEX_MAX_KEYS];
	SnapshotData DirtySnapshot;
	int			i;
	bool		conflict;
	bool		found_self;
	bool		found_self_siu_hit;
	ExprContext *econtext;
	TupleTableSlot *existing_slot;
	TupleTableSlot *save_scantuple;

	if (indexInfo->ii_ExclusionOps)
	{
		constr_procs = indexInfo->ii_ExclusionProcs;
		constr_strats = indexInfo->ii_ExclusionStrats;
	}
	else
	{
		constr_procs = indexInfo->ii_UniqueProcs;
		constr_strats = indexInfo->ii_UniqueStrats;
	}

	/*
	 * If this is a WITHOUT OVERLAPS constraint, we must also forbid empty
	 * ranges/multiranges. This must happen before we look for NULLs below, or
	 * a UNIQUE constraint could insert an empty range along with a NULL
	 * scalar part.
	 */
	if (indexInfo->ii_WithoutOverlaps)
	{
		/*
		 * Look up the type from the heap tuple, but check the Datum from the
		 * index tuple.
		 */
		AttrNumber	attno = indexInfo->ii_IndexAttrNumbers[indnkeyatts - 1];

		if (!isnull[indnkeyatts - 1])
		{
			TupleDesc	tupdesc = RelationGetDescr(heap);
			Form_pg_attribute att = TupleDescAttr(tupdesc, attno - 1);
			TypeCacheEntry *typcache = lookup_type_cache(att->atttypid,
														 TYPECACHE_DOMAIN_BASE_INFO);
			char		typtype;

			if (OidIsValid(typcache->domainBaseType))
				typtype = get_typtype(typcache->domainBaseType);
			else
				typtype = typcache->typtype;

			ExecWithoutOverlapsNotEmpty(heap, att->attname,
										values[indnkeyatts - 1],
										typtype, att->atttypid);
		}
	}

	/*
	 * If any of the input values are NULL, and the index uses the default
	 * nulls-are-distinct mode, the constraint check is assumed to pass (i.e.,
	 * we assume the operators are strict).  Otherwise, we interpret the
	 * constraint as specifying IS NULL for each column whose input value is
	 * NULL.
	 */
	if (!indexInfo->ii_NullsNotDistinct)
	{
		for (i = 0; i < indnkeyatts; i++)
		{
			if (isnull[i])
				return true;
		}
	}

	/*
	 * Search the tuples that are in the index for any violations, including
	 * tuples that aren't visible yet.
	 */
	InitDirtySnapshot(DirtySnapshot);

	for (i = 0; i < indnkeyatts; i++)
	{
		ScanKeyEntryInitialize(&scankeys[i],
							   isnull[i] ? SK_ISNULL | SK_SEARCHNULL : 0,
							   i + 1,
							   constr_strats[i],
							   InvalidOid,
							   index_collations[i],
							   constr_procs[i],
							   values[i]);
	}

	/*
	 * Need a TupleTableSlot to put existing tuples in.
	 *
	 * To use FormIndexDatum, we have to make the econtext's scantuple point
	 * to this slot.  Be sure to save and restore caller's value for
	 * scantuple.
	 */
	existing_slot = table_slot_create(heap, NULL);

	econtext = GetPerTupleExprContext(estate);
	save_scantuple = econtext->ecxt_scantuple;
	econtext->ecxt_scantuple = existing_slot;

	/*
	 * May have to restart scan from this point if a potential conflict is
	 * found.
	 */
retry:
	conflict = false;
	found_self = false;
	found_self_siu_hit = false;
	index_scan = index_beginscan(heap, index, false,
								 &DirtySnapshot, NULL, indnkeyatts, 0,
								 SO_NONE);
	index_rescan(index_scan, scankeys, indnkeyatts, NULL, 0);

	while (table_index_getnext_slot(index_scan, ForwardScanDirection,
									existing_slot))
	{
		TransactionId xwait;
		XLTW_Oper	reason_wait;
		Datum		existing_values[INDEX_MAX_KEYS];
		bool		existing_isnull[INDEX_MAX_KEYS];
		char	   *error_new;
		char	   *error_existing;

		/*
		 * Ignore the entry for the tuple we're trying to check.  With HOT-
		 * indexed (hot-indexed) updates, several index entries may chain-lead
		 * to the same heap tuple (a stale entry for the old key and a fresh
		 * entry for the new key).  They all resolve to the same TID here and
		 * must all be treated as "self", not as a duplicate error.  We
		 * tolerate the duplicate self arrival whenever *either* this
		 * iteration or an earlier one saw xs_entry_needs_recheck; the
		 * canonical direct entry and the stale chain-walk entries can arrive
		 * in either order.
		 */
		if (ItemPointerIsValid(tupleid) &&
			ItemPointerEquals(tupleid, &existing_slot->tts_tid))
		{
			if (found_self)
			{
				/*
				 * A repeat self-arrival is legitimate only in the HOT-indexed
				 * case: the canonical direct entry plus one or more stale
				 * chain-walk entries all resolve to this TID, and they may
				 * arrive in either order.  Tolerate the repeat when either the
				 * current arrival is stale, or an earlier arrival for this TID
				 * was (covering the direct-entry-after-stale-entry order).  A
				 * repeat that is non-stale with no stale arrival seen is a
				 * genuine duplicate-TID corruption; keep raising on it.
				 */
				if (index_scan->xs_entry_needs_recheck || found_self_siu_hit)
				{
					if (index_scan->xs_entry_needs_recheck)
						found_self_siu_hit = true;
					continue;
				}
				elog(ERROR, "found self tuple multiple times in index \"%s\"",
					 RelationGetRelationName(index));
			}
			if (index_scan->xs_entry_needs_recheck)
				found_self_siu_hit = true;
			found_self = true;
			continue;
		}

		/*
		 * Extract the index column values and isnull flags from the existing
		 * tuple.
		 */
		FormIndexDatum(indexInfo, existing_slot, estate,
					   existing_values, existing_isnull);

		/* If lossy indexscan, must recheck the condition */
		if (index_scan->xs_recheck)
		{
			if (!index_recheck_constraint(index,
										  constr_procs,
										  existing_values,
										  existing_isnull,
										  values))
				continue;		/* tuple doesn't actually match, so no
								 * conflict */
		}

		/*
		 * HOT-indexed chains can reach this loop via a stale btree leaf entry
		 * whose key is different from the heap tuple's current index-form.
		 * existing_values holds the current heap tuple's index-form
		 * (FormIndexDatum above).  Compare it against our new tuple's values
		 * using the same constraint operators; if they don't agree, the
		 * chain-walked tuple is not actually in conflict with our insertion
		 * it just shared a TID with a stale leaf entry we happened to scan
		 * through.  Skip it.
		 *
		 * This mirrors _bt_check_unique's HOT-indexed recheck path; for
		 * exclusion constraints the user-supplied operator in constr_procs
		 * replaces the btree equality comparator, and
		 * index_recheck_constraint does the right thing for either.
		 */
		if (index_scan->xs_entry_needs_recheck)
		{
			if (!index_recheck_constraint(index,
										  constr_procs,
										  existing_values,
										  existing_isnull,
										  values))
				continue;		/* stale chain hit, not a real conflict */
		}

		/*
		 * At this point we have either a conflict or a potential conflict.
		 *
		 * If an in-progress transaction is affecting the visibility of this
		 * tuple, we need to wait for it to complete and then recheck (unless
		 * the caller requested not to).  For simplicity we do rechecking by
		 * just restarting the whole scan --- this case probably doesn't
		 * happen often enough to be worth trying harder, and anyway we don't
		 * want to hold any index internal locks while waiting.
		 */
		xwait = TransactionIdIsValid(DirtySnapshot.xmin) ?
			DirtySnapshot.xmin : DirtySnapshot.xmax;

		if (TransactionIdIsValid(xwait) &&
			(waitMode == CEOUC_WAIT ||
			 (waitMode == CEOUC_LIVELOCK_PREVENTING_WAIT &&
			  DirtySnapshot.speculativeToken &&
			  TransactionIdPrecedes(GetCurrentTransactionId(), xwait))))
		{
			reason_wait = indexInfo->ii_ExclusionOps ?
				XLTW_RecheckExclusionConstr : XLTW_InsertIndex;
			index_endscan(index_scan);
			if (DirtySnapshot.speculativeToken)
				SpeculativeInsertionWait(DirtySnapshot.xmin,
										 DirtySnapshot.speculativeToken);
			else
				XactLockTableWait(xwait, heap,
								  &existing_slot->tts_tid, reason_wait);
			goto retry;
		}

		/*
		 * We have a definite conflict (or a potential one, but the caller
		 * didn't want to wait).  Return it to caller, or report it.
		 */
		if (violationOK)
		{
			conflict = true;
			if (conflictTid)
			{
				*conflictTid = existing_slot->tts_tid;

				/*
				 * The conflicting tuple decides the outcome of INSERT ... ON
				 * CONFLICT, so for SSI purposes it has been read, even when
				 * nothing gets written afterwards.  The dirty snapshot used
				 * by the scan is not an MVCC snapshot, so SSI ignored that
				 * read.  Read the tuple again with the query snapshot to
				 * record it. The result is of no interest here, the caller
				 * checks visibility itself.
				 */
				if (IsolationIsSerializable())
				{
					INJECTION_POINT("check-exclusion-or-unique-constraint-conflict", NULL);
					(void) table_tuple_fetch_row_version(heap, conflictTid,
														 estate->es_snapshot,
														 existing_slot);
				}
			}
			break;
		}

		error_new = BuildIndexValueDescription(index, values, isnull);
		error_existing = BuildIndexValueDescription(index, existing_values,
													existing_isnull);
		if (newIndex)
			ereport(ERROR,
					(errcode(ERRCODE_EXCLUSION_VIOLATION),
					 errmsg("could not create exclusion constraint \"%s\"",
							RelationGetRelationName(index)),
					 error_new && error_existing ?
					 errdetail("Key %s conflicts with key %s.",
							   error_new, error_existing) :
					 errdetail("Key conflicts exist."),
					 errtableconstraint(heap,
										RelationGetRelationName(index))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_EXCLUSION_VIOLATION),
					 errmsg("conflicting key value violates exclusion constraint \"%s\"",
							RelationGetRelationName(index)),
					 error_new && error_existing ?
					 errdetail("Key %s conflicts with existing key %s.",
							   error_new, error_existing) :
					 errdetail("Key conflicts with existing key."),
					 errtableconstraint(heap,
										RelationGetRelationName(index))));
	}

	index_endscan(index_scan);

	/*
	 * Ordinarily, at this point the search should have found the originally
	 * inserted tuple (if any), unless we exited the loop early because of
	 * conflict.  However, it is possible to define exclusion constraints for
	 * which that wouldn't be true --- for instance, if the operator is <>. So
	 * we no longer complain if found_self is still false.
	 */

	econtext->ecxt_scantuple = save_scantuple;

	ExecDropSingleTupleTableSlot(existing_slot);

#ifdef USE_INJECTION_POINTS
	if (!conflict)
		INJECTION_POINT("check-exclusion-or-unique-constraint-no-conflict", NULL);
#endif

	return !conflict;
}

/*
 * Check for violation of an exclusion constraint
 *
 * This is a dumbed down version of check_exclusion_or_unique_constraint
 * for external callers. They don't need all the special modes.
 */
void
check_exclusion_constraint(Relation heap, Relation index,
						   IndexInfo *indexInfo,
						   const ItemPointerData *tupleid,
						   const Datum *values, const bool *isnull,
						   EState *estate, bool newIndex)
{
	(void) check_exclusion_or_unique_constraint(heap, index, indexInfo, tupleid,
												values, isnull,
												estate, newIndex,
												CEOUC_WAIT, false, NULL);
}

/*
 * Check existing tuple's index values to see if it really matches the
 * exclusion condition against the new_values.  Returns true if conflict.
 */
static bool
index_recheck_constraint(Relation index, const Oid *constr_procs,
						 const Datum *existing_values, const bool *existing_isnull,
						 const Datum *new_values)
{
	int			indnkeyatts = IndexRelationGetNumberOfKeyAttributes(index);
	int			i;

	for (i = 0; i < indnkeyatts; i++)
	{
		/* Assume the exclusion operators are strict */
		if (existing_isnull[i])
			return false;

		if (!DatumGetBool(OidFunctionCall2Coll(constr_procs[i],
											   index->rd_indcollation[i],
											   existing_values[i],
											   new_values[i])))
			return false;
	}

	return true;
}

/*
 * ExecSetIndexUnchanged
 *
 * Populate two per-index flags ahead of ExecInsertIndexTuples:
 *
 *   - ii_IndexNeedsUpdate (wide) drives the skip decision.  It is true when
 *     the table AM moved the row (row_moved: it stored an independent new
 *     version the old index entries no longer locate) or when any attribute
 *     the index references (key, INCLUDE, expression, or partial-predicate
 *     column, per RelationGetIndexedAttrs()) changed.  A non-summarizing
 *     index for which this is false is skipped: its existing entry keeps
 *     resolving the HOT chain.
 *
 *   - ii_IndexUnchanged (narrow) is the indexUnchanged hint to aminsert,
 *     consumed by nbtree deduplication / bottom-up deletion.  Per the
 *     historical rule it counts only key columns; INCLUDE and predicate
 *     columns are deliberately ignored, and an expression key is treated
 *     conservatively as possibly changed.
 */
void
ExecSetIndexUnchanged(ResultRelInfo *resultRelInfo,
					  const Bitmapset *modified_idx_attrs,
					  bool row_moved)
{
	int			numIndices = resultRelInfo->ri_NumIndices;
	IndexInfo **indexInfoArray = resultRelInfo->ri_IndexRelationInfo;
	RelationPtr indexDescs = resultRelInfo->ri_IndexRelationDescs;

	if (numIndices == 0)
		return;

	for (int i = 0; i < numIndices; i++)
	{
		IndexInfo  *indexInfo = indexInfoArray[i];
		Relation	indexDesc = indexDescs[i];
		Bitmapset  *indexedattrs;
		bool		keychanged;

		if (indexDesc == NULL)
			continue;

		/*
		 * Skip decision (wide).  The index needs a new entry if the AM moved
		 * the row, or if any attribute it references (key, INCLUDE,
		 * expression, or partial-predicate column) changed.
		 * RelationGetIndexedAttrs() covers all of those.  (An UPDATE that
		 * touches an expression-index attribute never reaches the HOT-indexed
		 * path: HeapUpdateHotAllowable disqualifies it, pending
		 * expression-aware maintenance.)
		 */
		indexedattrs = RelationGetIndexedAttrs(indexDesc);
		indexInfo->ii_IndexNeedsUpdate =
			row_moved || bms_overlap(indexedattrs, modified_idx_attrs);
		bms_free(indexedattrs);

		/*
		 * aminsert hint (narrow).  ii_IndexUnchanged feeds nbtree
		 * deduplication / bottom-up deletion heuristics and, per the
		 * historical rule, counts only key columns: a change to an INCLUDE
		 * column or to a partial-index predicate column does not disqualify
		 * the hint.  An expression key column is treated conservatively as
		 * possibly changed.
		 */
		keychanged = false;
		for (int k = 0; k < indexInfo->ii_NumIndexKeyAttrs; k++)
		{
			AttrNumber	keycol = indexInfo->ii_IndexAttrNumbers[k];

			if (keycol == 0)	/* expression key: assume it may have changed */
			{
				keychanged = true;
				break;
			}
			if (bms_is_member(keycol - FirstLowInvalidHeapAttributeNumber,
							  modified_idx_attrs))
			{
				keychanged = true;
				break;
			}
		}
		indexInfo->ii_IndexUnchanged = !keychanged;
	}
}

/*
 * ExecWithoutOverlapsNotEmpty - raise an error if the tuple has an empty
 * range or multirange in the given attribute.
 */
static void
ExecWithoutOverlapsNotEmpty(Relation rel, NameData attname, Datum attval, char typtype, Oid atttypid)
{
	bool		isempty;
	RangeType  *r;
	MultirangeType *mr;

	switch (typtype)
	{
		case TYPTYPE_RANGE:
			r = DatumGetRangeTypeP(attval);
			isempty = RangeIsEmpty(r);
			break;
		case TYPTYPE_MULTIRANGE:
			mr = DatumGetMultirangeTypeP(attval);
			isempty = MultirangeIsEmpty(mr);
			break;
		default:
			elog(ERROR, "WITHOUT OVERLAPS column \"%s\" is not a range or multirange",
				 NameStr(attname));
	}

	/* Report a CHECK_VIOLATION */
	if (isempty)
		ereport(ERROR,
				(errcode(ERRCODE_CHECK_VIOLATION),
				 errmsg("empty WITHOUT OVERLAPS value found in column \"%s\" in relation \"%s\"",
						NameStr(attname), RelationGetRelationName(rel))));
}
