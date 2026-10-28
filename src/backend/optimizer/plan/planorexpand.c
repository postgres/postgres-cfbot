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
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"

/* Parameters set by GUC */
int			or_expansion_limit;

typedef struct OrExpansionArm
{
	PlannerInfo *subroot;
	RelOptInfo *final_rel;
	Path	   *arm_path;
	Index		arm_rti;
}			OrExpansionArm;

typedef struct OrExpansionCandidate
{
	List	   *arms;			/* List of OrExpansionArm */
}			OrExpansionCandidate;

static bool is_query_safe_for_or_expansion(PlannerInfo *root);
static void or_expansion_qp_callback(PlannerInfo *root, void *extra);
static OrExpansionCandidate * create_or_expansion_subpaths(PlannerInfo *root,
														   BoolExpr *orclause,
														   int qual_idx);
static bool has_outer_joins(Node *jtnode);
static bool match_target_expr(Node *want, Node *have);
static Path *or_expansion_align_path(PlannerInfo *root, RelOptInfo *joinrel,
									 Path *arm, PathTarget *target);
static PathTarget *create_subquery_pathtarget(PlannerInfo *root, Index varno,
											  PathTarget *subtarget);

/*
 * is_query_safe_for_or_expansion
 *	  Verify query-level safety gates for OR expansion.
 */
static bool
is_query_safe_for_or_expansion(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	ListCell   *lc;

	/*
	 * Only top-level queries may be expanded. Expanding within subqueries
	 * leads to extreme planning overhead with nested subqueries.
	 */
	if (root->parent_root != NULL)
		return false;

	/* Volatile functions in jointree (WHERE or FROM) are unsafe to duplicate */
	if (contain_volatile_functions((Node *) parse->jointree))
		return false;

	/*
	 * Reject if the jointree contains any SubPlan or SubLink (in WHERE or
	 * outer join ON conditions). At this point, SS_process_sublinks() has
	 * already converted sublinks into SubPlan nodes; contain_subplans()
	 * catches SubPlans, InitPlans, and any remaining SubLinks.
	 */
	if (contain_subplans((Node *) parse->jointree))
		return false;

	/* Disallow queries with lateral references */
	if (root->hasLateralRTEs)
		return false;

	/* Disallow if query already contains append relations */
	if (root->append_rel_list != NIL)
		return false;

	/*
	 * Check RTEs. We allow only known to be safe RTE cases: - RTE_RELATION:
	 * plain base tables (not partitioned, no inheritance, no tablesample, not
	 * foreign) - RTE_JOIN: joins (except FULL joins) - RTE_VALUES:
	 * non-volatile values lists without subplans - RTE_FUNCTION: non-volatile
	 * function RTEs without subplans - RTE_TABLEFUNC: non-volatile table
	 * functions without subplans - RTE_SUBQUERY: only flattened/inert
	 * subqueries (subquery == NULL) - RTE_GROUP, RTE_RESULT: synthetic
	 * planner entries Any other or active RTE kind (e.g. active RTE_SUBQUERY,
	 * RTE_CTE) is rejected for safety.
	 */
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		switch (rte->rtekind)
		{
			case RTE_RELATION:

				/*
				 * Disallow partitioned tables, foreign tables, and
				 * inheritance
				 */
				if (rte->inh ||
					rte->relkind == RELKIND_PARTITIONED_TABLE ||
					rte->relkind == RELKIND_FOREIGN_TABLE)
					return false;

				/* Disallow tablesample (arms could sample different rows) */
				if (rte->tablesample != NULL)
					return false;
				break;

			case RTE_JOIN:
				if (rte->jointype == JOIN_FULL)
					return false;
				break;

			case RTE_VALUES:
				if (contain_volatile_functions((Node *) rte->values_lists) ||
					contain_subplans((Node *) rte->values_lists))
					return false;
				break;

			case RTE_FUNCTION:
				if (contain_volatile_functions((Node *) rte->functions) ||
					contain_subplans((Node *) rte->functions))
					return false;
				break;

			case RTE_TABLEFUNC:
				if (contain_volatile_functions((Node *) rte->tablefunc) ||
					contain_subplans((Node *) rte->tablefunc))
					return false;
				break;

			case RTE_SUBQUERY:
				/* Only allow inert/flattened subqueries; reject active ones */
				if (rte->subquery != NULL)
					return false;
				break;

			case RTE_GROUP:
			case RTE_RESULT:
				break;

			default:
				/* Reject RTE_CTE, RTE_NAMEDTUPLESTORE, and any future types */
				return false;
		}

		/*
		 * Security barrier quals must not contain volatile functions or
		 * subplans
		 */
		if (contain_volatile_functions((Node *) rte->securityQuals) ||
			contain_subplans((Node *) rte->securityQuals))
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
 *	  Create best paths for each arm of the candidate OR clause and append
 *	  synthetic RTE_SUBQUERY entries to root's rtable.
 */
static OrExpansionCandidate *
create_or_expansion_subpaths(PlannerInfo *root, BoolExpr *orclause, int qual_idx)
{
	OrExpansionCandidate *cand;
	List	   *arms = NIL;
	int			num_arms = list_length(orclause->args);
	int			k;

	for (k = 0; k < num_arms; k++)
	{
		PlannerInfo *subroot;
		Query	   *parse;
		List	   *arm_quals = NIL;
		Node	   *arm_k;
		int			j;
		RelOptInfo *final_rel;
		OrExpansionArm *arm;

		/* Clone PlannerInfo state modeled on planagg.c */
		subroot = palloc_object(PlannerInfo);
		memcpy(subroot, root, sizeof(PlannerInfo));
		subroot->query_level++;
		subroot->parent_root = root;
		subroot->plan_name = choose_plan_name(root->glob, "orexpand", false);
		subroot->parse = parse = (Query *) copyObject(root->parse);

		subroot->simple_rel_array = NULL;
		subroot->simple_rte_array = NULL;
		subroot->simple_rel_array_size = 0;

		subroot->processed_tlist = (List *) copyObject(root->processed_tlist);
		subroot->append_rel_list = (List *) copyObject(root->append_rel_list);

		/* Each arm is planned for full retrieval by the Append */
		subroot->tuple_fraction = 0.0;
		subroot->limit_tuples = -1.0;

		/*
		 * Drop upper relation pointers copied from root, as the arm plans
		 * only the scan/join level.
		 */
		memset(subroot->upper_rels, 0, sizeof(subroot->upper_rels));
		memset(subroot->upper_targets, 0, sizeof(subroot->upper_targets));

		/*
		 * Give subroot its own JoinDomain and reset planner working lists so
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
		arm_quals = list_delete_nth_cell(copyObject((List *) root->parse->jointree->quals),
										 qual_idx);

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
			return NULL;

		/* Cope with params and initplans used within this subquery */
		SS_identify_outer_params(subroot);
		SS_charge_for_initplans(subroot, final_rel);

		arm = palloc_object(OrExpansionArm);
		arm->subroot = subroot;
		arm->final_rel = final_rel;
		arm->arm_path = final_rel->cheapest_total_path;
		arm->arm_rti = 0;

		arms = lappend(arms, arm);
	}

	cand = palloc_object(OrExpansionCandidate);
	cand->arms = arms;
	return cand;
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
			Relids		all_arm_varnos;
			int			nrelations = 0;
			int			rti = -1;

			if (num_arms < 2 || num_arms > or_expansion_limit)
			{
				qual_idx++;
				continue;
			}

			all_arm_varnos = pull_varnos(root, (Node *) orclause);

			/*
			 * Require the OR clause to reference at least 2 distinct
			 * relations. pull_varnos() includes outer-join RTE_JOIN relids
			 * via Var.varnullingrels; skip these so we only count base
			 * relations.
			 */
			while ((rti = bms_next_member(all_arm_varnos, rti)) >= 0)
			{
				RangeTblEntry *rte = rt_fetch(rti, parse->rtable);

				if (rte->rtekind != RTE_JOIN)
					nrelations++;
			}
			bms_free(all_arm_varnos);

			if (nrelations < 2)
			{
				qual_idx++;
				continue;
			}

			/* Query-level safety check */
			if (!checked_query)
			{
				if (!is_query_safe_for_or_expansion(root))
					return NIL;
				checked_query = true;
			}

			/* Transform OR clause and generate subpaths for each arm */
			{
				OrExpansionCandidate *cand = create_or_expansion_subpaths(root, orclause, qual_idx);

				if (cand != NULL)
					results = lappend(results, cand);
			}
		}
		qual_idx++;
	}

	/*
	 * If any candidates were found, append their synthetic RTE_SUBQUERY
	 * entries to root's rtable in a single pass.  Doing this after all
	 * candidates are planned ensures that later candidates do not copy
	 * earlier candidates' synthetic RTEs into their subroot parse trees.
	 */
	if (results != NIL)
	{
		ListCell   *lc_cand;

		foreach(lc_cand, results)
		{
			OrExpansionCandidate *cand = (OrExpansionCandidate *) lfirst(lc_cand);
			ListCell   *lc_arm;

			foreach(lc_arm, cand->arms)
			{
				OrExpansionArm *arm = (OrExpansionArm *) lfirst(lc_arm);
				RangeTblEntry *rte = makeNode(RangeTblEntry);

				rte->rtekind = RTE_SUBQUERY;
				rte->subquery = arm->subroot->parse;
				rte->alias = NULL;
				rte->eref = makeAlias(arm->subroot->plan_name, NIL);
				rte->lateral = false;
				rte->inh = false;
				rte->inFromCl = false;

				root->parse->rtable = lappend(root->parse->rtable, rte);
				arm->arm_rti = list_length(root->parse->rtable);
			}
		}
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
 *	  Ensure the arm path emits columns in the exact order needed by target.
 *	  If column order differs, wrap with a projection using the arm's own
 *	  expressions.
 */
static Path *
or_expansion_align_path(PlannerInfo *root, RelOptInfo *joinrel, Path *arm, PathTarget *target)
{
	List	   *want = target->exprs;
	List	   *have = arm->pathtarget->exprs;
	List	   *new_exprs = NIL;
	ListCell   *lc_want;
	PathTarget *newtarget;

	/* identical target lists */
	if (equal(want, have))
		return arm;

	if (list_length(want) != list_length(have))
		return NULL;

	foreach(lc_want, want)
	{
		Node	   *want_node = (Node *) lfirst(lc_want);
		Node	   *matched = NULL;
		ListCell   *lc;

		foreach(lc, have)
		{
			Node	   *have_node = (Node *) lfirst(lc);

			if (match_target_expr(want_node, have_node))
			{
				matched = have_node;
				break;
			}
		}

		if (matched == NULL)
			return NULL;

		new_exprs = lappend(new_exprs, matched);
	}

	if (equal(new_exprs, have))
		return arm;

	newtarget = create_empty_pathtarget();
	newtarget->exprs = new_exprs;
	set_pathtarget_cost_width(root, newtarget);

	/* Apply projection in-place if supported, or wrap with a ProjectionPath */
	return apply_projection_to_path(root, joinrel, arm, newtarget);
}

/*
 * create_subquery_pathtarget
 *	  Build a PathTarget for a synthetic subquery relation containing identity
 *	  Vars referencing the subpath's output columns (varno, 1..n).
 */
static PathTarget *
create_subquery_pathtarget(PlannerInfo *root, Index varno, PathTarget *subtarget)
{
	PathTarget *target = create_empty_pathtarget();
	int			attno = 0;
	ListCell   *lc;

	foreach(lc, subtarget->exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		Var		   *var;

		attno++;
		var = makeVar(varno,
					  attno,
					  exprType(expr),
					  exprTypmod(expr),
					  exprCollation(expr),
					  0);
		target->exprs = lappend(target->exprs, var);
	}

	return set_pathtarget_cost_width(root, target);
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
		OrExpansionCandidate *cand = (OrExpansionCandidate *) lfirst(lc);
		List	   *aligned_subpaths = NIL;
		ListCell   *lc_arm;
		bool		valid = true;
		AppendPathInput ainput;
		AppendPath *appendpath;

		/* Align and wrap each arm into a SubqueryScanPath */
		foreach(lc_arm, cand->arms)
		{
			OrExpansionArm *arm = (OrExpansionArm *) lfirst(lc_arm);
			Path	   *aligned;
			RelOptInfo *arm_rel;
			SubqueryScanPath *sqpath;

			aligned = or_expansion_align_path(arm->subroot,
											  arm->final_rel,
											  arm->arm_path,
											  joinrel->reltarget);
			if (aligned == NULL)
			{
				valid = false;
				break;
			}

			/* Synchronize subroot tlist with the arm's actual output target */
			arm->subroot->processed_tlist =
				arm->subroot->parse->targetList =
				make_tlist_from_pathtarget(aligned->pathtarget);

			/* Build the simple relation for the synthetic subquery RTE */
			arm_rel = build_simple_rel(root, arm->arm_rti, NULL);
			arm_rel->subroot = arm->subroot;
			arm_rel->tuples = arm_rel->rows = aligned->rows;

			/* Build reltarget with identity Vars (arm_rti, 1..n) */
			arm_rel->reltarget = create_subquery_pathtarget(root,
															arm->arm_rti,
															aligned->pathtarget);

			/* Wrap the arm subpath in a SubqueryScanPath */
			sqpath = create_subqueryscan_path(root,
											  arm_rel,
											  aligned,
											  true, /* trivial_pathtarget */
											  NIL,
											  NULL);

			aligned_subpaths = lappend(aligned_subpaths, (Path *) sqpath);
		}

		if (!valid)
			continue;

		ainput.subpaths = aligned_subpaths;
		ainput.partial_subpaths = NIL;
		ainput.child_append_relid_sets = NIL;

		/*
		 * Build an UPPERREL_SETOP rel keyed by the arm subqueries' relids,
		 * This guarantees Append.apprelids holds subquery RTIs (all
		 * RTE_SUBQUERY).
		 */
		{
			Relids		arm_relids = NULL;
			RelOptInfo *setop_rel;

			foreach(lc_arm, cand->arms)
			{
				OrExpansionArm *arm = (OrExpansionArm *) lfirst(lc_arm);

				arm_relids = bms_add_member(arm_relids, arm->arm_rti);
			}

			setop_rel = fetch_upper_rel(root, UPPERREL_SETOP, arm_relids);
			setop_rel->reltarget = joinrel->reltarget;

			/*
			 * Pass joinrel->rows to create_append_path to keep all paths for
			 * joinrel at one consistent row estimate.
			 */
			appendpath = create_append_path(root,
											setop_rel,
											ainput,
											NIL,
											NULL,
											0,
											false,
											joinrel->rows);
		}

		add_path(joinrel, (Path *) appendpath);
		added_any = true;
	}

	/* Refigure which is the cheapest path for the joinrel */
	if (added_any)
		set_cheapest(joinrel);
}
