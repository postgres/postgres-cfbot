/*-------------------------------------------------------------------------
 *
 * planorexpand.c
 *	  Consider whether join OR clauses can be expanded to UNION ALL
 *	  (Append) paths.
 *
 * This module looks for top-level OR clauses whose arms reference multiple
 * relations, and tries to generate paths representing conversion of such an
 * OR clause into a UNION ALL operation.  For example,
 *
 *		SELECT ... FROM a, b WHERE (cond-on-A OR cond-on-B) AND other-conds
 *
 * can be implemented as
 *
 *		SELECT ... FROM a, b WHERE cond-on-A AND other-conds
 *		UNION ALL
 *		SELECT ... FROM a, b WHERE cond-on-B AND other-conds
 *			AND ((cond-on-A) IS NOT TRUE)
 *
 * Ordinarily, PostgreSQL plans cross-relation disjunctions as a single join
 * tree with the entire OR clause evaluated as a join filter.  That prevents
 * the planner from pushing branch-specific restrictions down into individual
 * index scans, pruning unneeded partitions, or choosing join orders tailored
 * to each branch.  By expanding each branch into an independent query ("arm"),
 * each arm can be planned with restriction clauses specific to that arm,
 * often allowing fast index scans, branch-specific partition pruning, or
 * index-based joins that would otherwise be impossible.
 *
 * To avoid emitting duplicate rows without incurring the overhead of an
 * expensive deduplication step, each branch after the first includes
 * null-safe exclusion conditions:
 *
 *		AND ((earlier-branch-condition) IS NOT TRUE)
 *
 * These exclusion clauses preserve the three-valued logic of the original OR:
 * a subsequent branch excludes only those rows that were already produced by
 * an earlier branch, while correctly retaining rows where earlier conditions
 * evaluated to NULL or false.  Consequently, the arm paths can be combined
 * directly with an Append node without any top-level sort or hashing.
 *
 * Expansion is considered in grouping_planner() just before standard
 * scan/join planning.  For each suitable candidate OR clause, each arm is
 * planned in an independent PlannerInfo subroot using query_planner(),
 * allowing ordinary path-generation machinery to explore the optimal access
 * methods for that arm.  Each arm is then represented by a synthetic
 * RTE_SUBQUERY entry in the master query's rangetable, providing a clean
 * abstraction boundary.  The resulting arm paths are aligned with the master
 * targetlist and combined into an AppendPath (or Parallel AppendPath), which
 * is attached to the top-level scan/join relation to compete on cost against
 * conventional join paths.
 *
 * We must insist that WHERE and JOIN/ON clauses contain no volatile functions
 * or correlated subplans, because branch conditions are evaluated multiple
 * times across arms and exclusion tests.  We also disallow FULL joins, because
 * branch restrictions can cause reduce_outer_joins() to simplify a FULL join
 * into an inner or left join in some arms but not others, producing asymmetric
 * join reduction.  Active unflattened subqueries and CTEs are likewise rejected
 * because evaluating them independently in each arm would violate
 * single-evaluation semantics or multiply expensive work.
 *
 * Because planning each arm requires an independent invocation of
 * query_planner(), planning overhead scales with the number of arms.  The
 * or_expansion_limit GUC caps this overhead by setting the maximum number of
 * arms an OR clause may contain to be considered for expansion.  If a candidate
 * clause contains more arms than this threshold, it is left unexpanded.
 * Setting or_expansion_limit to zero (or less than two) basically just disables the feature
 * entirely.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
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
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"

/*
 * or_expansion_limit sets the maximum number of arms in an OR clause that
 * will be considered for expansion.  Values below 2 disable expansion.
 */
int			or_expansion_limit;

typedef struct OrExpansionArm
{
	PlannerInfo *subroot;
	RelOptInfo *final_rel;
	Path	   *arm_path;		/* Cheapest complete path */
	Path	   *arm_partial_path;	/* Cheapest partial path, if any */
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
static List *build_arm_subquery_colnames(int nattrs);

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
	 * Only top-level queries may be expanded.  Expanding within subqueries
	 * risks planning overhead when nested.
	 */
	if (root->parent_root != NULL)
		return false;

	/*
	 * Reject queries with volatile expressions in the jointree. Duplicating
	 * volatile functions across arms can alter execution counts or produce
	 * inconsistent results between arm evaluation and exclusion.
	 */
	if (contain_volatile_functions((Node *) parse->jointree))
		return false;

	/*
	 * Reject queries whose jointree contains any SubPlan, AlternativeSubPlan,
	 * or SubLink.  Uncorrelated sublinks were already converted to InitPlans
	 * represented by PARAM_EXEC Params, which are safely evaluated once at
	 * query startup and shared across arms.  Correlated subplans cannot be
	 * duplicated across arms without outer-parameter coordination.
	 *
	 * SubPlans in the query targetlist need not be rejected here, because
	 * targetlist evaluation occurs above the Append during final projection.
	 */
	if (contain_subplans((Node *) parse->jointree))
		return false;

	/*
	 * Check range table entries for features that are unsafe to replicate
	 * across multiple arms.
	 */
	foreach(lc, parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		switch (rte->rtekind)
		{
			case RTE_RELATION:

				/*
				 * Disallow TABLESAMPLE because arms planned independently
				 * would sample different random subsets of rows.
				 */
				if (rte->tablesample != NULL)
					return false;
				break;

			case RTE_JOIN:

				/*
				 * Disallow queries with FULL joins.  Addition of branch
				 * restriction clauses in individual arms can cause
				 * reduce_outer_joins() to simplify a FULL join into a LEFT or
				 * INNER join in some arms but not others, leading to
				 * asymmetric join reduction across arms.
				 */
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

				/*
				 * Allow flattened subqueries (subquery == NULL) and pulled-up
				 * UNION ALL append relations (rte->inh == true).  Reject
				 * active unflattened subqueries because each arm would plan
				 * and execute an independent SubqueryScan, which could
				 * duplicate expensive work or introduce some inconsistencies.
				 */
				if (rte->subquery != NULL && !rte->inh)
					return false;
				break;

			case RTE_GROUP:
			case RTE_RESULT:
				break;

			default:

				/*
				 * Reject other RTE kinds, particularly CTEs, which cannot be
				 * duplicated across arms without sharing execution state.
				 */
				return false;
		}

		/*
		 * Security barrier quals must not contain volatile functions or
		 * subplans.
		 */
		if (contain_volatile_functions((Node *) rte->securityQuals) ||
			contain_subplans((Node *) rte->securityQuals))
			return false;
	}

	/*
	 * Append relations (such as pulled-up UNION ALL subqueries) can harbor
	 * expressions in their translation lists that will be substituted into
	 * arm quals during planning.  Ensure that translated expressions contain
	 * no volatile functions or subplans, which would otherwise bypass the
	 * jointree checks above.
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);

		if (contain_volatile_functions((Node *) appinfo->translated_vars) ||
			contain_subplans((Node *) appinfo->translated_vars))
			return false;
	}

	return true;
}

/*
 * has_outer_joins
 *	  Recursively search the jointree for outer joins.  Returns true if any
 *	  JoinExpr satisfies IS_OUTER_JOIN (such as JOIN_LEFT or JOIN_ANTI).
 */
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
	 * We generate Append paths rather than MergeAppend paths, which do not
	 * preserve sort order across arms. MergeAppends are an improvement for
	 * the future.
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

		/*
		 * Create a clean PlannerInfo for this arm, matching the subquery
		 * initialization pattern in subquery_planner() and
		 * pull_up_simple_subquery(). Using makeNode() ensures all workspace
		 * arrays, hash tables, and working lists are zeroed, avoiding fragile
		 * manual field resets.
		 */
		subroot = makeNode(PlannerInfo);
		subroot->parse = parse = (Query *) copyObject(root->parse);
		subroot->glob = root->glob;
		subroot->query_level = root->query_level + 1;
		subroot->plan_name = choose_plan_name(root->glob, "orexpand", false);
		subroot->alternative_plan_name = root->plan_name;
		subroot->parent_root = root;
		subroot->planner_cxt = CurrentMemoryContext;
		subroot->processed_tlist = (List *) copyObject(root->processed_tlist);
		subroot->append_rel_list = (List *) copyObject(root->append_rel_list);
		subroot->qual_security_level = root->qual_security_level;
		subroot->hasHavingQual = root->hasHavingQual;
		subroot->hasLateralRTEs = root->hasLateralRTEs;
		subroot->group_rtindex = root->group_rtindex;
		subroot->numOrderedAggs = root->numOrderedAggs;
		subroot->hasJoinRTEs = root->hasJoinRTEs;

		/*
		 * If there is no ORDER BY, aggregation, grouping, or distinct, the
		 * Append directly feeds the Limit node, so inherit tuple_fraction and
		 * limit_tuples from grouping_planner. If not, those upper nodes will
		 * consume all tuples, so arms must optimize for total cost.
		 */
		if (root->parse->sortClause == NIL &&
			!root->parse->hasAggs &&
			root->parse->groupClause == NIL &&
			root->parse->havingQual == NULL &&
			root->parse->distinctClause == NIL &&
			root->parse->groupingSets == NIL &&
			!root->parse->hasWindowFuncs &&
			!root->parse->hasTargetSRFs)
		{
			subroot->tuple_fraction = root->tuple_fraction;
			subroot->limit_tuples = root->limit_tuples;
		}
		else
		{
			subroot->tuple_fraction = 0.0;
			subroot->limit_tuples = -1.0;
		}

		subroot->wt_param_id = -1;
		subroot->join_domains = list_make1(makeNode(JoinDomain));

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

		/* Re-run reduce_outer_joins if jointree contains outer joins */
		if (has_outer_joins((Node *) parse->jointree))
		{
			reduce_outer_joins(subroot);

			/*
			 * reduce_outer_joins() may update parse->targetList via
			 * remove_nulling_relids().  Re-sync processed_tlist so that
			 * varnullingrels are consistent during arm plan creation.
			 */
			subroot->processed_tlist = (List *) copyObject(subroot->parse->targetList);
		}

		/* Plan this arm */
		final_rel = query_planner(subroot, or_expansion_qp_callback, NULL);

		if (!final_rel || !final_rel->cheapest_total_path ||
			final_rel->cheapest_total_path->param_info != NULL)
			return NULL;


		/* Cope with params and initplans used within this subquery */
		SS_identify_outer_params(subroot);
		SS_charge_for_initplans(subroot, final_rel);

		set_cheapest(final_rel);

		arm = palloc_object(OrExpansionArm);
		arm->subroot = subroot;
		arm->final_rel = final_rel;
		arm->arm_path = get_cheapest_fractional_path(final_rel,
													 subroot->tuple_fraction);
		arm->arm_partial_path = (final_rel->partial_pathlist != NIL) ?
			(Path *) linitial(final_rel->partial_pathlist) : NULL;
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
			OrExpansionCandidate *cand;

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
			cand = create_or_expansion_subpaths(root, orclause, qual_idx);
			if (cand != NULL)
				results = lappend(results, cand);
		}
		qual_idx++;
	}

	/*
	 * If any candidates were found, append their synthetic RTE_SUBQUERY
	 * entries to root's rtable in a single pass.
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

	/* Apply projection in-place if we can, or wrap with a ProjectionPath */
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
 * build_arm_subquery_colnames
 *	  Construct dummy column names for a synthetic subquery RTE.
 *
 *	  build_simple_rel() expects eref->colnames to have as many entries as
 *	  the subquery emits in order to set max_attr and allocate attribute
 *	  arrays.  Because the synthetic SubqueryScan has a trivial targetlist
 *	  and will be elided by setrefs.c, dummy column names suffice.
 */
static List *
build_arm_subquery_colnames(int nattrs)
{
	List	   *colnames = NIL;
	int			i;

	for (i = 0; i < nattrs; i++)
		colnames = lappend(colnames, makeString(pstrdup("?column?")));

	return colnames;
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
	bool		consider_parallel;

	if (joinrel == NULL || joinrel->reltarget == NULL)
		return;

	consider_parallel = joinrel->consider_parallel &&
		(max_parallel_workers_per_gather > 0);

	foreach(lc, or_cands)
	{
		OrExpansionCandidate *cand = (OrExpansionCandidate *) lfirst(lc);
		ListCell   *lc_arm;
		List	   *live_arm_rels = NIL;
		bool		cand_valid = true;

		/*
		 * Build and populate synthetic relations for each arm.
		 */
		foreach(lc_arm, cand->arms)
		{
			OrExpansionArm *arm = (OrExpansionArm *) lfirst(lc_arm);
			Path	   *aligned_cheapest;
			RelOptInfo *arm_rel;
			SubqueryScanPath *sqpath;
			RangeTblEntry *arm_rte;

			/* Align the arm's cheapest path to determine the arm reltarget */
			aligned_cheapest = or_expansion_align_path(arm->subroot,
													   arm->final_rel,
													   arm->arm_path,
													   joinrel->reltarget);
			if (aligned_cheapest == NULL)
			{
				cand_valid = false;
				break;
			}

			/* Synchronize subroot tlist with the arm's actual output target */
			arm->subroot->processed_tlist =
				arm->subroot->parse->targetList =
				make_tlist_from_pathtarget(aligned_cheapest->pathtarget);

			/*
			 * Provide column names for the synthetic subquery RTE so that
			 * build_simple_rel() accurately computes max_attr > 0.
			 */
			arm_rte = rt_fetch(arm->arm_rti, root->parse->rtable);
			arm_rte->eref->colnames =
				build_arm_subquery_colnames(list_length(joinrel->reltarget->exprs));

			/* Make rel for the synthetic subquery RTE */
			arm_rel = build_simple_rel(root, arm->arm_rti, NULL);
			arm_rel->subroot = arm->subroot;
			arm_rel->tuples = arm_rel->rows = aligned_cheapest->rows;
			arm_rel->consider_parallel = consider_parallel;
			arm_rel->reltarget = create_subquery_pathtarget(root,
															arm->arm_rti,
															aligned_cheapest->pathtarget);

			/* Cheapest total path */
			sqpath = create_subqueryscan_path(root,
											  arm_rel,
											  aligned_cheapest,
											  true,
											  NIL,
											  NULL);
			add_path(arm_rel, (Path *) sqpath);

			/*
			 * If parallel execution is considered, populate partial and
			 * parallel-safe paths on arm_rel so that
			 * add_paths_to_append_rel() can generate parallel Append paths.
			 */
			if (consider_parallel)
			{
				if (arm->arm_partial_path != NULL)
				{
					Path	   *aligned_partial;
					Path	   *ps_partial;

					aligned_partial = or_expansion_align_path(arm->subroot,
															  arm->final_rel,
															  arm->arm_partial_path,
															  joinrel->reltarget);
					ps_partial = (Path *) create_subqueryscan_path(root,
																   arm_rel,
																   aligned_partial,
																   true,
																   NIL,
																   NULL);
					add_partial_path(arm_rel, ps_partial);
				}

				/*
				 * If the cheapest path is not parallel-safe, try a
				 * parallel-safe inner path.
				 */
				if (!aligned_cheapest->parallel_safe)
				{
					Path	   *nppath;

					nppath = get_cheapest_parallel_safe_total_inner(arm->final_rel->pathlist);
					if (nppath != NULL)
					{
						Path	   *aligned_np;
						Path	   *ps_np;

						aligned_np = or_expansion_align_path(arm->subroot,
															 arm->final_rel,
															 nppath,
															 joinrel->reltarget);
						ps_np = (Path *) create_subqueryscan_path(root,
																  arm_rel,
																  aligned_np,
																  true,
																  NIL,
																  NULL);
						add_path(arm_rel, ps_np);
					}
				}
			}

			set_cheapest(arm_rel);
			live_arm_rels = lappend(live_arm_rels, arm_rel);
		}

		/*
		 * If all arms produced valid subpaths, generate AppendPath and
		 * Parallel AppendPath alternatives via add_paths_to_append_rel().
		 */
		if (cand_valid)
		{
			add_paths_to_append_rel(root, joinrel, live_arm_rels);
			added_any = true;
		}
	}

	/* Refigure which is the cheapest path for the joinrel */
	if (added_any)
		set_cheapest(joinrel);
}
