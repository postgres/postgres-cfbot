/*-------------------------------------------------------------------------
 *
 * parse_merge.c
 *	  handle merge-statement in parser
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_merge.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "nodes/makefuncs.h"
#include "parser/analyze.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_cte.h"
#include "parser/parse_expr.h"
#include "parser/parse_merge.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parsetree.h"
#include "utils/rel.h"

/*
 * transformMergeStmt -
 *	  transforms a MERGE statement
 */
Query *
transformMergeStmt(ParseState *pstate, MergeStmt *stmt)
{
	Query	   *qry = makeNode(Query);
	ListCell   *l;
	AclMode		targetPerms = ACL_NO_RIGHTS;
	bool		is_terminal[NUM_MERGE_MATCH_KINDS];
	Index		sourceRTI;
	List	   *mergeActionList;
	List	   *sourceNamespace;
	ParseNamespaceItem *nsitem;

	/* There can't be any outer WITH to worry about */
	Assert(pstate->p_ctenamespace == NIL);

	qry->commandType = CMD_MERGE;
	qry->hasRecursive = false;

	/* process the WITH clause independently of all else */
	if (stmt->withClause)
	{
		if (stmt->withClause->recursive)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("WITH RECURSIVE is not supported for MERGE statement")));

		qry->cteList = transformWithClause(pstate, stmt->withClause);
		qry->hasModifyingCTE = pstate->p_hasModifyingCTE;
	}

	/*
	 * Check WHEN clauses for permissions and sanity
	 */
	is_terminal[MERGE_WHEN_MATCHED] = false;
	is_terminal[MERGE_WHEN_NOT_MATCHED_BY_SOURCE] = false;
	is_terminal[MERGE_WHEN_NOT_MATCHED_BY_TARGET] = false;
	foreach(l, stmt->mergeWhenClauses)
	{
		MergeWhenClause *mergeWhenClause = (MergeWhenClause *) lfirst(l);

		/*
		 * Collect permissions to check, according to action types. We require
		 * SELECT privileges for DO NOTHING because it'd be irregular to have
		 * a target relation with zero privileges checked, in case DO NOTHING
		 * is the only action.  There's no damage from that: any meaningful
		 * MERGE command requires at least some access to the table anyway.
		 */
		switch (mergeWhenClause->commandType)
		{
			case CMD_INSERT:
				targetPerms |= ACL_INSERT;
				break;
			case CMD_UPDATE:
				targetPerms |= ACL_UPDATE;
				break;
			case CMD_DELETE:
				targetPerms |= ACL_DELETE;
				break;
			case CMD_NOTHING:
				targetPerms |= ACL_SELECT;
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN clause");
		}

		/*
		 * Check for unreachable WHEN clauses
		 */
		if (is_terminal[mergeWhenClause->matchKind])
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unreachable WHEN clause specified after unconditional WHEN clause")));
		if (mergeWhenClause->condition == NULL)
			is_terminal[mergeWhenClause->matchKind] = true;
	}

	/*
	 * Set up the MERGE target table.  The target table is added to the
	 * namespace below and to joinlist in transform_MERGE_to_join, so don't do
	 * it here.
	 *
	 * Initially mergeTargetRelation is the same as resultRelation, so data is
	 * read from the table being updated.  However, that might be changed by
	 * the rewriter, if the target is a trigger-updatable view, to allow
	 * target data to be read from the expanded view query while updating the
	 * original view relation.
	 */
	qry->resultRelation = setTargetTable(pstate, stmt->relation,
										 stmt->relation->inh,
										 false, targetPerms);
	qry->mergeTargetRelation = qry->resultRelation;

	/* The target relation must be a table or a view */
	if (pstate->p_target_relation->rd_rel->relkind != RELKIND_RELATION &&
		pstate->p_target_relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE &&
		pstate->p_target_relation->rd_rel->relkind != RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot execute MERGE on relation \"%s\"",
						RelationGetRelationName(pstate->p_target_relation)),
				 errdetail_relkind_not_supported(pstate->p_target_relation->rd_rel->relkind)));

	/* Now transform the source relation to produce the source RTE. */
	transformFromClause(pstate,
						list_make1(stmt->sourceRelation));
	sourceRTI = list_length(pstate->p_rtable);
	/* Save the source namespace before adding the target below. */
	sourceNamespace = list_copy(pstate->p_namespace);
	nsitem = GetNSItemByRangeTablePosn(pstate, sourceRTI, 0);

	/*
	 * Check that the target table doesn't conflict with the source table.
	 * This would typically be a checkNameSpaceConflicts call, but we want a
	 * more specific error message.
	 */
	if (strcmp(pstate->p_target_nsitem->p_names->aliasname,
			   nsitem->p_names->aliasname) == 0)
		ereport(ERROR,
				errcode(ERRCODE_DUPLICATE_ALIAS),
				errmsg("name \"%s\" specified more than once",
					   pstate->p_target_nsitem->p_names->aliasname),
				errdetail("The name is used both as MERGE target table and data source."));

	/*
	 * There's no need for a targetlist here; it'll be set up by
	 * preprocess_targetlist later.
	 */
	qry->targetList = NIL;
	qry->rtable = pstate->p_rtable;
	qry->rteperminfos = pstate->p_rteperminfos;

	/*
	 * Transform the join condition.  This includes references to the target
	 * side, so add that to the namespace.
	 */
	addNSItemToQuery(pstate, pstate->p_target_nsitem, false, true, true);
	qry->mergeJoinCondition = transformExpr(pstate, stmt->joinCondition,
											EXPR_KIND_JOIN_ON);

	/*
	 * Create the temporary query's jointree using the joinlist we built using
	 * just the source relation; the target relation is not included. The join
	 * will be constructed fully by transform_MERGE_to_join.
	 */
	qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

	/* Transform the RETURNING list, if any */
	transformReturningClause(pstate, qry, stmt->returningClause,
							 EXPR_KIND_MERGE_RETURNING);

	/*
	 * We now have a good query shape, so now look at the WHEN conditions and
	 * action targetlists.
	 *
	 * Overall, the MERGE Query's targetlist is NIL.
	 *
	 * Each individual action has its own targetlist that needs separate
	 * transformation. These transforms don't do anything to the overall
	 * targetlist, since that is only used for resjunk columns.
	 *
	 * We can reference any column in Target or Source, which is OK because
	 * both of those already have RTEs. There is nothing like the EXCLUDED
	 * pseudo-relation for INSERT ON CONFLICT.
	 */
	mergeActionList = NIL;
	foreach(l, stmt->mergeWhenClauses)
	{
		MergeWhenClause *mergeWhenClause = lfirst_node(MergeWhenClause, l);
		MergeAction *action;
		List	   *save_namespace;

		action = makeNode(MergeAction);
		action->commandType = mergeWhenClause->commandType;
		action->matchKind = mergeWhenClause->matchKind;

		/*
		 * Temporarily restrict the namespace to the relations visible to this
		 * action.  Use the complete source namespace, rather than just its
		 * top-level RTE, so that inputs of an unaliased source join are
		 * handled too.  Preserve the visibility flags established by
		 * transformFromClause().
		 */
		save_namespace = pstate->p_namespace;
		switch (mergeWhenClause->matchKind)
		{
			case MERGE_WHEN_MATCHED:
				/* Both source and target are already visible. */
				break;
			case MERGE_WHEN_NOT_MATCHED_BY_SOURCE:
				pstate->p_namespace = list_make1(pstate->p_target_nsitem);
				break;
			case MERGE_WHEN_NOT_MATCHED_BY_TARGET:
				pstate->p_namespace = sourceNamespace;
				break;
			default:
				elog(ERROR, "unknown match kind in MERGE WHEN clause");
		}

		/*
		 * Transform the WHEN condition.
		 *
		 * Note that these quals are NOT added to the join quals; instead they
		 * are evaluated separately during execution to decide which of the
		 * WHEN MATCHED or WHEN NOT MATCHED actions to execute.
		 */
		action->qual = transformWhereClause(pstate, mergeWhenClause->condition,
											EXPR_KIND_MERGE_WHEN, "WHEN");

		/*
		 * Transform target lists for each INSERT and UPDATE action stmt
		 */
		switch (action->commandType)
		{
			case CMD_INSERT:
				{
					List	   *exprList = NIL;
					ListCell   *lc;
					RTEPermissionInfo *perminfo;
					ListCell   *icols;
					ListCell   *attnos;
					List	   *icolumns;
					List	   *attrnos;

					icolumns = checkInsertTargets(pstate,
												  mergeWhenClause->targetList,
												  &attrnos);
					Assert(list_length(icolumns) == list_length(attrnos));

					action->override = mergeWhenClause->override;

					/*
					 * Handle INSERT much like in transformInsertStmt
					 */
					if (mergeWhenClause->values == NIL)
					{
						/*
						 * We have INSERT ... DEFAULT VALUES.  We can handle
						 * this case by emitting an empty targetlist --- all
						 * columns will be defaulted when the planner expands
						 * the targetlist.
						 */
						exprList = NIL;
					}
					else
					{
						/*
						 * Process INSERT ... VALUES with a single VALUES
						 * sublist.  We treat this case separately for
						 * efficiency.  The sublist is just computed directly
						 * as the Query's targetlist, with no VALUES RTE.  So
						 * it works just like a SELECT without any FROM.
						 */

						/*
						 * Do basic expression transformation (same as a ROW()
						 * expr, but allow SetToDefault at top level)
						 */
						exprList = transformExpressionList(pstate,
														   mergeWhenClause->values,
														   EXPR_KIND_VALUES_SINGLE,
														   true);

						/* Prepare row for assignment to target table */
						exprList = transformInsertRow(pstate, exprList,
													  mergeWhenClause->targetList,
													  icolumns, attrnos,
													  false);
					}

					/*
					 * Generate action's target list using the computed list
					 * of expressions. Also, mark all the target columns as
					 * needing insert permissions.
					 */
					perminfo = pstate->p_target_nsitem->p_perminfo;
					forthree(lc, exprList, icols, icolumns, attnos, attrnos)
					{
						Expr	   *expr = (Expr *) lfirst(lc);
						ResTarget  *col = lfirst_node(ResTarget, icols);
						AttrNumber	attr_num = (AttrNumber) lfirst_int(attnos);
						TargetEntry *tle;

						tle = makeTargetEntry(expr,
											  attr_num,
											  col->name,
											  false);
						action->targetList = lappend(action->targetList, tle);

						perminfo->insertedCols =
							bms_add_member(perminfo->insertedCols,
										   attr_num - FirstLowInvalidHeapAttributeNumber);
					}
				}
				break;
			case CMD_UPDATE:
				action->targetList =
					transformUpdateTargetList(pstate,
											  mergeWhenClause->targetList);
				break;
			case CMD_DELETE:
				break;

			case CMD_NOTHING:
				action->targetList = NIL;
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN clause");
		}

		/* Restore the namespace for subsequent actions. */
		pstate->p_namespace = save_namespace;

		mergeActionList = lappend(mergeActionList, action);
	}

	qry->mergeActionList = mergeActionList;

	qry->hasTargetSRFs = false;
	qry->hasSubLinks = pstate->p_hasSubLinks;

	assign_query_collations(pstate, qry);

	return qry;
}

