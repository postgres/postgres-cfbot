/*-------------------------------------------------------------------------
 *
 * planorexpand.c
 *	  Consider whether OR clauses in join queries can be expanded to
 *	  UNION ALL (Append) paths.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planorexpand.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"


static bool is_query_safe_for_or_expansion(PlannerInfo *root);
static bool has_partitioned_table_walker(Node *node, void *ctx);
static void or_expansion_qp_callback(PlannerInfo *root, void *extra);
static List *create_or_expansion_subpaths(PlannerInfo *root, BoolExpr *orclause,
										  int qual_idx);
static bool has_outer_joins(Node *jtnode);
static bool match_target_expr(Node *want, Node *have);
static Path *or_expansion_align_path(PlannerInfo *root, RelOptInfo *joinrel, Path *arm);

/*
 * has_partitioned_table_walker
 *	  Return true if the query (or any subquery within it) references a
 *	  partitioned table.  Used by is_query_safe_for_or_expansion to catch
 *	  partitioned tables that are hidden inside derived-table subqueries.
 */
static bool
has_partitioned_table_walker(Node *node, void *ctx)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		if (rte->rtekind == RTE_RELATION &&
			rte->relkind == RELKIND_PARTITIONED_TABLE)
			return true;
		/* subquery contents are visited by query_tree_walker automatically */
		return false;
	}
	if (IsA(node, Query))
		return query_tree_walker((Query *) node,
								 has_partitioned_table_walker, ctx,
								 QTW_EXAMINE_RTES_BEFORE);
	return expression_tree_walker(node, has_partitioned_table_walker, ctx);
}

/*
 * is_query_safe_for_or_expansion
 *	  Verify query-level safety gates for OR expansion.
 */
static bool
is_query_safe_for_or_expansion(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	ListCell   *lc;

	/* Volatile functions in jointree (WHERE or FROM) are unsafe to duplicate */
	if (contain_volatile_functions((Node *) parse->jointree))
		return false;

	/*
	 * Should we disallow queries with lateral references?
	 */

	/*
	 * if (root->hasLateralRTEs) return false;
	 */

	/* Check RTEs */
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		/*
		 * Do not attempt transformation if the query contains FULL joins.
		 */
		if (rte->rtekind == RTE_JOIN && rte->jointype == JOIN_FULL)
			return false;

		/*
		 * Disallow partitioned tables (directly or inside any subquery) until
		 * arm partition pruning and rangetable index synchronization are
		 * supported.  A partitioned table hidden inside a derived-table
		 * subquery triggers the same index-mismatch as a top-level one.
		 */
		if (rte->rtekind == RTE_RELATION &&
			rte->relkind == RELKIND_PARTITIONED_TABLE)
			return false;
		if (rte->rtekind == RTE_SUBQUERY &&
			has_partitioned_table_walker((Node *) rte->subquery, NULL))
			return false;

		/* Disallow tablesample (arms could sample different rows ?) */
		if (rte->tablesample != NULL)
			return false;

		/* Security barrier quals must not contain volatile functions */
		if (contain_volatile_functions((Node *) rte->securityQuals))
			return false;

		/* Disallow volatile function RTEs */
		if (rte->rtekind == RTE_FUNCTION &&
			contain_volatile_functions((Node *) rte->functions))
			return false;

		/* Disallow volatile expressions in subquery/values/tablefunc */
		if (rte->rtekind == RTE_SUBQUERY &&
			contain_volatile_functions((Node *) rte->subquery))
			return false;

		if (rte->rtekind == RTE_VALUES &&
			contain_volatile_functions((Node *) rte->values_lists))
			return false;

		if (rte->rtekind == RTE_TABLEFUNC &&
			contain_volatile_functions((Node *) rte->tablefunc))
			return false;
	}

	return true;
}

static bool
has_outer_joins(Node *jtnode)
{
	if (jtnode == NULL)
		return false;
	if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
		{
			if (has_outer_joins(lfirst(lc)))
				return true;
		}
		return false;
	}
	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (IS_OUTER_JOIN(j->jointype))
			return true;
		return has_outer_joins(j->larg) || has_outer_joins(j->rarg);
	}

	return false;
}

/*
 * or_expansion_qp_callback
 *	  Query planner callback for subroot arm planning.
 */
static void
or_expansion_qp_callback(PlannerInfo *root, void *extra)
{
	/*
	 * Ordering is destroyed through UNION ALL / Append, so clear pathkeys
	 * that might otherwise restrict lower paths.
	 */
	root->group_pathkeys = NIL;
	root->window_pathkeys = NIL;
	root->distinct_pathkeys = NIL;
	root->sort_pathkeys = NIL;
	root->query_pathkeys = NIL;
}

/*
 * create_or_expansion_subpaths
 *	  Create best paths for each arm of the candidate OR clause.
 */
static List *
create_or_expansion_subpaths(PlannerInfo *root, BoolExpr *orclause, int qual_idx)
{
	List	   *subpaths = NIL;
	int			num_arms = list_length(orclause->args);
	int			k;

	for (k = 0; k < num_arms; k++)
	{
		PlannerInfo *subroot;
		Query	   *parse;
		List	   *arm_quals = NIL;
		ListCell   *lc;
		int			q_idx;
		Node	   *arm_k;
		int			j;
		RelOptInfo *final_rel;
		Path	   *subpath;

		/* Clone PlannerInfo state modeled on planagg.c */
		subroot = (PlannerInfo *) palloc(sizeof(PlannerInfo));
		memcpy(subroot, root, sizeof(PlannerInfo));
		subroot->parse = parse = (Query *) copyObject(root->parse);

		/*
		 * subroot will set up its own rel/rte arrays inside query_planner.
		 */
		subroot->simple_rel_array = NULL;
		subroot->simple_rte_array = NULL;
		subroot->simple_rel_array_size = 0;

		subroot->processed_tlist = (List *) copyObject(root->processed_tlist);
		subroot->append_rel_list = (List *) copyObject(root->append_rel_list);
		subroot->tuple_fraction = 1.0;
		subroot->limit_tuples = -1.0;

		/*
		 * give subroot its own JoinDomain and reset planner working lists so
		 * query_planner(subroot) does not corrupt root's state.
		 */
		subroot->join_domains = list_make1(makeNode(JoinDomain));
		subroot->plan_params = NIL;
		subroot->outer_params = NULL;
		subroot->rowMarks = NIL;
		subroot->init_plans = NIL;
		subroot->agginfos = NIL;
		subroot->aggtransinfos = NIL;
		subroot->join_info_list = NIL;
		subroot->eq_classes = NIL;
		subroot->placeholder_list = NIL;
		subroot->placeholder_array = NULL;
		subroot->placeholder_array_size = 0;
		subroot->left_join_clauses = NIL;
		subroot->right_join_clauses = NIL;
		subroot->full_join_clauses = NIL;
		subroot->fkey_list = NIL;
		subroot->initial_rels = NIL;
		subroot->last_rinfo_serial = 0;

		/*
		 * Build arm quals: R AND C_k AND (C_0 IS NOT TRUE) AND ... AND
		 * (C_{k-1} IS NOT TRUE)
		 */
		q_idx = 0;
		foreach(lc, (List *) root->parse->jointree->quals)
		{
			if (q_idx != qual_idx)
				arm_quals = lappend(arm_quals, copyObject(lfirst(lc)));
			q_idx++;
		}

		/* Add arm k's disjunct, flattened if it is an AND expression */
		arm_k = (Node *) copyObject(list_nth(orclause->args, k));
		arm_quals = list_concat(arm_quals, make_ands_implicit((Expr *) arm_k));

		/* Add null-safe exclusion conditions for all previous arms */
		for (j = 0; j < k; j++)
		{
			Node	   *prev_arm = (Node *) copyObject(list_nth(orclause->args, j));
			BooleanTest *btest;

			btest = makeNode(BooleanTest);
			btest->arg = (Expr *) prev_arm;
			btest->booltesttype = IS_NOT_TRUE;
			btest->location = -1;

			arm_quals = lappend(arm_quals, btest);
		}

		parse->jointree->quals = (Node *) arm_quals;

		/* Re-run reduce_outer_joins if rtable contains outer joins */
		if (has_outer_joins((Node *) parse->jointree))
			reduce_outer_joins(subroot);

		/* Plan this arm */
		final_rel = query_planner(subroot, or_expansion_qp_callback, NULL);

		if (!final_rel || !final_rel->cheapest_total_path ||
			final_rel->cheapest_total_path->param_info != NULL)
			return NIL;

		subpath = final_rel->cheapest_total_path;
		subpaths = lappend(subpaths, subpath);
	}

	return subpaths;
}

/*
 * plan_or_expansion_arms
 *	  Check eligibility and generate subpaths for OR clauses in top-level WHERE.
 *	  Called by grouping_planner() before query_planner().
 */
List *
plan_or_expansion_arms(PlannerInfo *root)
{
	List	   *results = NIL;
	Query	   *parse = root->parse;
	bool		checked_query = false;
	ListCell   *lc;
	int			qual_idx;

	/* Cheap gates first */
	if (or_expansion_limit < 2)
		return NIL;

	if (parse->commandType != CMD_SELECT ||
		parse->rowMarks != NIL ||
		parse->setOperations != NULL ||
		parse->cteList != NIL ||
		root->hasRecursion)
		return NIL;

	if (list_length(parse->rtable) < 2)
		return NIL;

	if (parse->jointree == NULL || parse->jointree->quals == NULL)
		return NIL;

	/* Scan top-level WHERE quals for eligible OR clauses */
	qual_idx = 0;
	foreach(lc, (List *) parse->jointree->quals)
	{
		Node	   *qual = (Node *) lfirst(lc);

		if (is_orclause(qual))
		{
			BoolExpr   *orclause = (BoolExpr *) qual;
			int			num_arms = list_length(orclause->args);
			Relids		all_arm_varnos = NULL;
			ListCell   *lc_arm;

			if (num_arms < 2 || num_arms > or_expansion_limit)
			{
				qual_idx++;
				continue;
			}

			/* Structural case gate: check referenced relations */
			foreach(lc_arm, orclause->args)
			{
				Node	   *arm = (Node *) lfirst(lc_arm);
				Relids		arm_varnos = pull_varnos(root, arm);

				all_arm_varnos = bms_join(all_arm_varnos, arm_varnos);
			}

			/*
			 * Require the OR clause to reference at least 2 distinct
			 * relations.
			 */
			if (bms_num_members(all_arm_varnos) < 2)
			{
				bms_free(all_arm_varnos);
				qual_idx++;
				continue;
			}
			bms_free(all_arm_varnos);

			/* Query-level safety check */
			if (!checked_query)
			{
				if (!is_query_safe_for_or_expansion(root))
					return NIL;
				checked_query = true;
			}

			/* Transform OR clause and generate subpaths for each arm */
			{
				List	   *subpaths = create_or_expansion_subpaths(root, orclause, qual_idx);

				if (subpaths != NIL)
					results = lappend(results, subpaths);
			}
		}
		qual_idx++;
	}

	return results;
}

/*
 * match_target_expr
 *	  Check if two targetlist expressions match, ignoring varnullingrels
 *	  differences that may arise from outer join reduction in subroot.
 */
static bool
match_target_expr(Node *want, Node *have)
{
	if (want == have)
		return true;
	if (want == NULL || have == NULL)
		return false;
	if (nodeTag(want) != nodeTag(have))
		return false;

	if (IsA(want, Var))
	{
		Var		   *v1 = (Var *) want;
		Var		   *v2 = (Var *) have;

		return (v1->varno == v2->varno &&
				v1->varattno == v2->varattno &&
				v1->varlevelsup == v2->varlevelsup);
	}

	if (IsA(want, PlaceHolderVar))
	{
		PlaceHolderVar *p1 = (PlaceHolderVar *) want;
		PlaceHolderVar *p2 = (PlaceHolderVar *) have;

		return (p1->phid == p2->phid &&
				p1->phlevelsup == p2->phlevelsup);
	}

	return equal(want, have);
}

/*
 * or_expansion_align_path
 *	  Ensure the arm path emits columns in the exact order needed by joinrel.
 *	  If column order differs, wrap with a projection using the arm's own
 *	  expressions.
 */
static Path *
or_expansion_align_path(PlannerInfo *root, RelOptInfo *joinrel, Path *arm)
{
	List	   *want = joinrel->reltarget->exprs;
	List	   *have = arm->pathtarget->exprs;
	List	   *new_exprs = NIL;
	bool		identical = true;
	ListCell   *lc_want;
	ListCell   *lc_have;
	PathTarget *newtarget;

	if (list_length(want) != list_length(have))
		identical = false;

	lc_have = list_head(have);
	foreach(lc_want, want)
	{
		Node	   *want_node = (Node *) lfirst(lc_want);
		Node	   *matched = NULL;
		ListCell   *lc;

		if (identical && lc_have != NULL)
		{
			if (match_target_expr(want_node, (Node *) lfirst(lc_have)))
			{
				matched = (Node *) lfirst(lc_have);
				lc_have = lnext(have, lc_have);
			}
			else
				identical = false;
		}

		if (matched == NULL)
		{
			foreach(lc, have)
			{
				Node	   *have_node = (Node *) lfirst(lc);

				if (match_target_expr(want_node, have_node))
				{
					matched = have_node;
					break;
				}
			}
		}

		if (matched == NULL)
			return NULL;

		new_exprs = lappend(new_exprs, matched);
	}

	if (identical)
		return arm;

	newtarget = create_empty_pathtarget();
	newtarget->exprs = new_exprs;
	set_pathtarget_cost_width(root, newtarget);

	/* Apply projection in-place if supported, or wrap with a ProjectionPath */
	return apply_projection_to_path(root, joinrel, arm, newtarget);
}

/*
 * add_or_expansion_paths
 *	  Combine arm subpaths into an AppendPath and add to the top join rel.
 *	  Called by grouping_planner() right after query_planner().
 */
void
add_or_expansion_paths(PlannerInfo *root, RelOptInfo *joinrel, List *or_cands)
{
	ListCell   *lc;
	bool		added_any = false;

	if (joinrel == NULL || joinrel->reltarget == NULL)
		return;

	foreach(lc, or_cands)
	{
		List	   *subpaths = (List *) lfirst(lc);
		List	   *aligned_subpaths = NIL;
		ListCell   *lc_subpath;
		bool		valid = true;
		AppendPathInput ainput;
		AppendPath *appendpath;

		foreach(lc_subpath, subpaths)
		{
			Path	   *subpath = (Path *) lfirst(lc_subpath);
			Path	   *aligned;

			aligned = or_expansion_align_path(root, joinrel, subpath);
			if (aligned == NULL)
			{
				valid = false;
				break;
			}
			aligned_subpaths = lappend(aligned_subpaths, aligned);
		}

		if (!valid)
			continue;

		ainput.subpaths = aligned_subpaths;
		ainput.partial_subpaths = NIL;
		ainput.child_append_relid_sets = NIL;

		/* Build Append path combining arm subpaths */
		appendpath = create_append_path(root,
										joinrel,
										ainput,
										NIL,
										NULL,
										0,
										false,
										-1);

		add_path(joinrel, (Path *) appendpath);
		added_any = true;
	}

	/* We need to refigure which is the cheapest path for the joinrel */
	if (added_any)
		set_cheapest(joinrel);
}
