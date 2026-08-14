/*-------------------------------------------------------------------------
 *
 * uniquekeys.c
 *	  Routines to deduce what expression sets a relation is distinct over
 *
 * A relation's UniqueKeys record the expression sets over which its output
 * is distinct.  They let us elide a redundant DISTINCT or GROUP BY step and
 * detect unique joins (inner_unique).
 *
 * This file populates the keys of base relations, join relations, and upper
 * relations, and provides the lookups by which the various consumers answer
 * specific distinctness questions.
 *
 * See src/backend/optimizer/README for a great deal of information about
 * the nature and use of unique keys.
 *
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/uniquekeys.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "nodes/makefuncs.h"
#include "nodes/multibitmapset.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "parser/parse_agg.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

/* Arbitrary cap on the number of UniqueKeys tracked per relation */
#define MAX_UNIQUEKEYS			8

static void populate_plain_rel_uniquekeys(PlannerInfo *root,
										  RelOptInfo *rel);
static void populate_subquery_rel_uniquekeys(PlannerInfo *root,
											 RelOptInfo *rel);
static void translate_subquery_uniquekeys(PlannerInfo *root, RelOptInfo *rel);
static bool add_subquery_key_col(PlannerInfo *root, RelOptInfo *rel,
								 SortGroupClause *sgc, TargetEntry *tle,
								 Bitmapset **key_ecs);
static void strengthen_uniquekeys_for_join(PlannerInfo *root,
										   RelOptInfo *joinrel,
										   List *restrictlist);
static bool uniquekey_forced_nonnullable(PlannerInfo *root, UniqueKey *ukey,
										 List *nonnullable_vars);
static bool var_in_nonnullable_vars(Var *var, List *nonnullable_vars);
static Bitmapset *get_interesting_unique_ecs(PlannerInfo *root);
static Bitmapset *add_ojclause_ecs(PlannerInfo *root, Bitmapset *result,
								   List *ojclauses);
static Bitmapset *add_base_ec_positions(PlannerInfo *root, Bitmapset *result,
										EquivalenceClass *ec);
static bool collect_clause_ecs(PlannerInfo *root, List *clause,
							   Bitmapset **ecs_p);
static void add_uniquekey(RelOptInfo *rel, Bitmapset *eclass_indexes,
						  bool null_aware);
static bool rel_has_uniquekey_within(RelOptInfo *rel, Bitmapset *ecs,
									 bool need_null_aware);
static bool side_is_unique(PlannerInfo *root, RelOptInfo *joinrel,
						   RelOptInfo *side, RelOptInfo *otherside,
						   JoinType jointype, List *restrictlist);
static int	find_ec_position(PlannerInfo *root, Expr *expr,
							 List *opfamilies, Oid collation,
							 Bitmapset *candidates);
static int	base_ec_position(PlannerInfo *root, EquivalenceClass *ec);


/*
 * populate_baserel_uniquekeys
 *		Deduce the unique keys of a base relation's output.
 *
 * This is called once per base rel, after its restriction clauses and (for
 * plain relations) index information have been set up, and once per appendrel
 * child, which just inherits its parent's keys.
 */
void
populate_baserel_uniquekeys(PlannerInfo *root, RelOptInfo *rel)
{
	Assert(IS_SIMPLE_REL(rel));

	/*
	 * An appendrel child emits a subset of its parent's rows, so every key of
	 * the parent is a key of the child.  A child Var belongs to the same ECs
	 * as its parent, so the EC indexes carry over unchanged.
	 */
	if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
	{
		rel->uniquekeys = rel->top_parent->uniquekeys;
		return;
	}

	/*
	 * Compute this now even if we have no rules for this rtekind: it is
	 * cached on PlannerInfo and can create ECs, so it must not first run
	 * inside GEQO's short-lived context.
	 */
	(void) get_interesting_unique_ecs(root);

	if (rel->rtekind == RTE_RELATION)
		populate_plain_rel_uniquekeys(root, rel);
	else if (rel->rtekind == RTE_SUBQUERY)
		populate_subquery_rel_uniquekeys(root, rel);
	/* We have no deduction rules for other rtekinds */
}

/*
 * populate_plain_rel_uniquekeys
 *		Deduce unique keys for a plain relation from its unique indexes.
 */
static void
populate_plain_rel_uniquekeys(PlannerInfo *root, RelOptInfo *rel)
{
	Bitmapset  *interesting = get_interesting_unique_ecs(root);
	List	   *nonnullable_vars = NIL;
	bool		nonnullable_valid = false;

	if (bms_is_empty(interesting))
		return;

	foreach_node(IndexOptInfo, ind, rel->indexlist)
	{
		Bitmapset  *key_ecs = NULL;
		bool		null_aware = true;
		bool		useless = false;
		int			c;

		/*
		 * If the index is not unique, or not immediately enforced, or if it's
		 * a partial index, it's useless here.
		 */
		if (!ind->unique || !ind->immediate || ind->indpred != NIL)
			continue;

		for (c = 0; c < ind->nkeycolumns; c++)
		{
			Expr	   *colexpr;
			EquivalenceClass *ec;
			Oid			equality_op;
			int			pos;

			colexpr = list_nth_node(TargetEntry, ind->indextlist, c)->expr;

			/*
			 * An EC's opfamily list is the full family membership of an
			 * equality operator, which may be more than one family, so we
			 * can't just use the index's opfamily here.
			 */
			equality_op = get_opfamily_member_for_cmptype(ind->opfamily[c],
														  ind->opcintype[c],
														  ind->opcintype[c],
														  COMPARE_EQ);
			if (!OidIsValid(equality_op))	/* shouldn't happen */
				elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
					 COMPARE_EQ, ind->opcintype[c], ind->opcintype[c],
					 ind->opfamily[c]);

			/*
			 * Find the EC mentioning this column.  A column no suitable EC
			 * mentions cannot be referenced by any consumer, so such an index
			 * gives no usable key.
			 */
			pos = find_ec_position(root, colexpr,
								   get_mergejoin_opfamilies(equality_op),
								   ind->indexcollations[c],
								   rel->eclass_indexes);
			if (pos < 0)
			{
				useless = true;
				break;
			}
			ec = (EquivalenceClass *) list_nth(root->eq_classes, pos);

			/*
			 * A column equated to a constant can be left out of the key;
			 * uniqueness is then guaranteed by the remaining columns.
			 */
			if (ec->ec_has_const)
				continue;

			if (!bms_is_member(pos, interesting))
			{
				useless = true;
				break;
			}

			key_ecs = bms_add_member(key_ecs, pos);

			/*
			 * The column might be NULL, unless a NOT NULL constraint or some
			 * strict restriction clause says otherwise; a NULLS NOT DISTINCT
			 * index enforces uniqueness even among NULLs, though.
			 */
			if (null_aware && !ind->nullsnotdistinct)
			{
				if (!nonnullable_valid)
				{
					foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
					{
						nonnullable_vars =
							mbms_add_members(nonnullable_vars,
											 find_nonnullable_vars((Node *) rinfo->clause));
					}
					nonnullable_valid = true;
				}

				if (!IsA(colexpr, Var) ||
					(!var_is_nonnullable(root, (Var *) colexpr, NOTNULL_SOURCE_RELOPT) &&
					 !var_in_nonnullable_vars((Var *) colexpr, nonnullable_vars)))
					null_aware = false;
			}
		}

		if (!useless)
			add_uniquekey(rel, key_ecs, null_aware);
	}
}

/*
 * populate_subquery_rel_uniquekeys
 *		Deduce unique keys for a subquery RTE relation, in the same spirit as
 *		query_is_distinct_for().
 */
static void
populate_subquery_rel_uniquekeys(PlannerInfo *root, RelOptInfo *rel)
{
	Query	   *subquery = planner_rt_fetch(rel->relid, root)->subquery;
	Bitmapset  *interesting = get_interesting_unique_ecs(root);
	Bitmapset  *key_ecs = NULL;

	if (bms_is_empty(interesting))
		return;

	/*
	 * Most of a subquery's distinctness is recorded in the UniqueKeys of its
	 * final relation, so we translate those into outer representation.
	 */
	translate_subquery_uniquekeys(root, rel);

	/*
	 * A tlist SRF is expanded above the set operation below and can duplicate
	 * its rows, which would void the whole-row key we deduce there, so skip
	 * when the subquery has one.
	 */
	if (subquery->hasTargetSRFs)
		return;

	/*
	 * A set operation is the one remaining case with no UniqueKey built for
	 * the subquery's final relation, so record that here.
	 */
	if (subquery->setOperations)
	{
		/*
		 * UNION, INTERSECT, EXCEPT guarantee uniqueness of the whole output
		 * row, except with ALL.
		 */
		SetOperationStmt *topop = castNode(SetOperationStmt,
										   subquery->setOperations);
		ListCell   *lg;

		if (topop->all)
			return;

		lg = list_head(topop->groupClauses);
		foreach_node(TargetEntry, tle, subquery->targetList)
		{
			SortGroupClause *sgc;

			if (tle->resjunk)
				continue;

			Assert(lg != NULL);
			sgc = (SortGroupClause *) lfirst(lg);
			lg = lnext(topop->groupClauses, lg);

			if (!add_subquery_key_col(root, rel, sgc, tle, &key_ecs))
				return;
		}

		/* A set operation deduplicates NULLs too */
		add_uniquekey(rel, key_ecs, true);
	}
}

/*
 * add_subquery_key_col
 *		Add the EC of one deduplicated subquery output column to the key being
 *		built; returns false if the column makes a key impossible.
 */
static bool
add_subquery_key_col(PlannerInfo *root, RelOptInfo *rel,
					 SortGroupClause *sgc, TargetEntry *tle,
					 Bitmapset **key_ecs)
{
	List	   *opfamilies = get_mergejoin_opfamilies(sgc->eqop);
	Oid			collation = exprCollation((Node *) tle->expr);
	Var		   *colvar;
	EquivalenceClass *ec;
	int			pos;

	/* a resjunk column is not visible to the outer query */
	if (tle->resjunk)
		return false;

	/* if the equality operator isn't btree equality, no EC can match */
	if (opfamilies == NIL)
		return false;

	/* Find the EC mentioning the corresponding output Var */
	colvar = makeVar(rel->relid, tle->resno,
					 exprType((Node *) tle->expr),
					 exprTypmod((Node *) tle->expr),
					 collation, 0);
	pos = find_ec_position(root, (Expr *) colvar, opfamilies, collation,
						   rel->eclass_indexes);
	if (pos < 0)
		return false;
	ec = (EquivalenceClass *) list_nth(root->eq_classes, pos);

	if (ec->ec_has_const)
		return true;

	if (!bms_is_member(pos, get_interesting_unique_ecs(root)))
		return false;

	*key_ecs = bms_add_member(*key_ecs, pos);
	return true;
}

/*
 * translate_subquery_uniquekeys
 *		Build an uniquekeys list that describes the distinctness of a
 *		subquery's result, in the terms of the outer query.  This is
 *		essentially a task of conversion.
 *
 * The subquery's final relation carries UniqueKeys over the subquery's own
 * ECs.  For each such key we map every EC to the parent EC of the
 * corresponding output column and, if all of them map into the parent's
 * interesting set, record the translated key on "rel".
 */
static void
translate_subquery_uniquekeys(PlannerInfo *root, RelOptInfo *rel)
{
	PlannerInfo *subroot = rel->subroot;
	RelOptInfo *subfinal;
	List	   *subquery_tlist;
	Bitmapset  *interesting;

	/* There is no subroot if set_subquery_pathlist() never ran */
	if (subroot == NULL)
		return;

	subfinal = fetch_upper_rel(subroot, UPPERREL_FINAL, NULL);
	if (subfinal->uniquekeys == NIL)
		return;

	subquery_tlist = subroot->processed_tlist;
	interesting = get_interesting_unique_ecs(root);

	foreach_node(UniqueKey, subkey, subfinal->uniquekeys)
	{
		Bitmapset  *parent_ecs = NULL;
		bool		ok = true;
		int			i = -1;

		while ((i = bms_next_member(subkey->eclass_indexes, i)) >= 0)
		{
			EquivalenceClass *sub_ec = list_nth_node(EquivalenceClass,
													 subroot->eq_classes, i);
			EquivalenceClass *outer_ec = NULL;

			/*
			 * Find the parent EC that represents this subquery EC, reached
			 * through one of the subquery's output columns.  This is the
			 * non-volatile arm of convert_subquery_pathkeys(): any matching
			 * outer EC will do, since all of an EC's members are equal, so
			 * uniqueness over one is uniqueness over any.  A volatile EC
			 * can't be a key and won't match, so we don't bother searching
			 * for it.
			 */
			if (!sub_ec->ec_has_volatile)
			{
				foreach_node(EquivalenceMember, sub_member, sub_ec->ec_members)
				{
					Expr	   *sub_expr = sub_member->em_expr;
					Oid			sub_expr_type = sub_member->em_datatype;
					Oid			sub_expr_coll = sub_ec->ec_collation;

					/* Child members should not exist in ec_members */
					Assert(!sub_member->em_is_child);

					foreach_node(TargetEntry, tle, subquery_tlist)
					{
						Var		   *outer_var;
						Expr	   *tle_expr;

						/* Is the TLE actually available to the outer query? */
						outer_var = find_var_for_subquery_tle(rel, tle);
						if (!outer_var)
							continue;

						/*
						 * The targetlist entry is considered to match if it
						 * matches after sort-key canonicalization.  That is
						 * needed since the sub_expr has been through the same
						 * process.
						 */
						tle_expr = canonicalize_ec_expression(tle->expr,
															  sub_expr_type,
															  sub_expr_coll);
						if (!equal(tle_expr, sub_expr))
							continue;

						/* See if we have a matching EC for the TLE */
						outer_ec = get_eclass_for_sort_expr(root,
															(Expr *) outer_var,
															sub_ec->ec_opfamilies,
															sub_expr_type,
															sub_expr_coll,
															0,
															rel->relids,
															false);

						if (outer_ec)
							break;
					}

					if (outer_ec)
						break;
				}
			}

			/* It must map to an EC the parent tracks keys over */
			if (outer_ec == NULL ||
				!bms_is_member(outer_ec->ec_index, interesting))
			{
				ok = false;
				break;
			}

			parent_ecs = bms_add_member(parent_ecs, outer_ec->ec_index);
		}

		/*
		 * An empty subkey (the subquery emits at most one row) translates to
		 * an empty parent key, which the loop above yields with ok == true.
		 */
		if (ok)
			add_uniquekey(rel, parent_ecs, subkey->null_aware);
	}
}

/*
 * populate_joinrel_uniquekeys
 *		Deduce the unique keys of a join relation's output from those of its
 *		input relations.
 *
 * This is called just once per joinrel, with whichever pair of input relations
 * the joinrel happens to be built from first.  Uniqueness is a property of the
 * joinrel's output, so it is independent of the pair used to prove it; other
 * pairs might prove additional keys, but chasing those isn't worth the cycles.
 */
void
populate_joinrel_uniquekeys(PlannerInfo *root, RelOptInfo *joinrel,
							RelOptInfo *outerrel,
							RelOptInfo *innerrel,
							SpecialJoinInfo *sjinfo,
							List *restrictlist)
{
	JoinType	jointype = sjinfo->jointype;
	RelOptInfo *lhs = outerrel;
	RelOptInfo *rhs = innerrel;

	/* A full join null-extends both sides; nothing survives. */
	if (jointype == JOIN_FULL)
		return;

	/* Nothing to derive if neither input has keys */
	if (outerrel->uniquekeys == NIL && innerrel->uniquekeys == NIL)
		return;

	/* Identify the special join's LHS */
	if (jointype != JOIN_INNER)
	{
		if (bms_is_subset(sjinfo->min_lefthand, outerrel->relids) &&
			bms_is_subset(sjinfo->min_righthand, innerrel->relids))
		{
			lhs = outerrel;
			rhs = innerrel;
		}
		else if (bms_is_subset(sjinfo->min_lefthand, innerrel->relids) &&
				 bms_is_subset(sjinfo->min_righthand, outerrel->relids))
		{
			lhs = innerrel;
			rhs = outerrel;
		}
		else
		{
			/*
			 * The special join is not applied at this join: a semijoin RHS
			 * unique-ified and joined to only part of its LHS (see
			 * join_is_legal).  We could derive keys from the unique-ified
			 * rel's distinctness, but don't bother.
			 */
			Assert(jointype == JOIN_SEMI);
			return;
		}
	}

	/*
	 * Preservation of the LHS keys: unconditional for a semi/antijoin, else
	 * the RHS must be unique for the clauses so each LHS row yields at most
	 * one join row.  A lateral reference into the RHS re-executes the LHS per
	 * RHS row, voiding this.
	 */
	if (lhs->uniquekeys != NIL &&
		!bms_overlap(lhs->lateral_relids, rhs->relids) &&
		((jointype == JOIN_SEMI || jointype == JOIN_ANTI) ||
		 side_is_unique(root, joinrel, rhs, lhs, jointype, restrictlist)))
	{
		foreach_node(UniqueKey, ukey, lhs->uniquekeys)
		{
			add_uniquekey(joinrel, ukey->eclass_indexes, ukey->null_aware);
		}
	}

	/* The RHS keys are not usable above a semi/anti join */
	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI)
	{
		/*
		 * A semijoin emits an LHS row only when some RHS row satisfied the
		 * join clauses, so every output row satisfies them.  An antijoin
		 * emits exactly the rows that no RHS row matched.
		 */
		if (jointype == JOIN_SEMI)
			strengthen_uniquekeys_for_join(root, joinrel, restrictlist);
		return;
	}

	/*
	 * Preservation of the RHS keys, if the LHS is unique for the clauses.
	 * Across a left join they lose NULL-awareness, and an empty key does not
	 * survive null-extension (one null-extended row per unmatched LHS row).
	 */
	if (rhs->uniquekeys != NIL &&
		!bms_overlap(rhs->lateral_relids, lhs->relids) &&
		side_is_unique(root, joinrel, lhs, rhs, jointype, restrictlist))
	{
		foreach_node(UniqueKey, ukey, rhs->uniquekeys)
		{
			if (jointype == JOIN_INNER)
				add_uniquekey(joinrel, ukey->eclass_indexes, ukey->null_aware);
			else if (!bms_is_empty(ukey->eclass_indexes))
			{
				Assert(jointype == JOIN_LEFT);
				add_uniquekey(joinrel, ukey->eclass_indexes, false);
			}
		}
	}

	/* Combination: the union of one key from each side is a key */
	foreach_node(UniqueKey, lkey, lhs->uniquekeys)
	{
		foreach_node(UniqueKey, rkey, rhs->uniquekeys)
		{
			add_uniquekey(joinrel,
						  bms_union(lkey->eclass_indexes,
									rkey->eclass_indexes),
						  lkey->null_aware && rkey->null_aware);
		}
	}

	/* Strengthen keys whose columns the join's clauses force non-NULL */
	if (jointype == JOIN_INNER)
		strengthen_uniquekeys_for_join(root, joinrel, restrictlist);
}

/*
 * strengthen_uniquekeys_for_join
 *		Mark a joinrel's non-NULL-aware keys NULL-aware where the join's own
 *		clauses force their columns non-NULL.
 *
 * The caller must have established that every clause holds of every output
 * row, which is so at an inner join and at a semijoin.  Any Var a strict
 * clause requires non-NULL is then non-NULL throughout the output, and if
 * every EC of a key holds such a Var, no output row has a NULL key column, so
 * the key is really NULL-aware.
 */
static void
strengthen_uniquekeys_for_join(PlannerInfo *root, RelOptInfo *joinrel,
							   List *restrictlist)
{
	List	   *nonnullable_vars = NIL;

	/* Which Vars do this join's clauses force non-NULL? */
	foreach_node(RestrictInfo, rinfo, restrictlist)
	{
		nonnullable_vars =
			mbms_add_members(nonnullable_vars,
							 find_nonnullable_vars((Node *) rinfo->clause));
	}

	if (nonnullable_vars == NIL)
		return;

	foreach_node(UniqueKey, ukey, joinrel->uniquekeys)
	{
		/* Only a non-NULL-aware key that has columns can be strengthened */
		if (ukey->null_aware || bms_is_empty(ukey->eclass_indexes))
			continue;

		if (uniquekey_forced_nonnullable(root, ukey, nonnullable_vars))
			ukey->null_aware = true;
	}
}

/*
 * uniquekey_forced_nonnullable
 *		Does every EquivalenceClass of ukey hold a Var that nonnullable_vars
 *		forces non-NULL?
 */
static bool
uniquekey_forced_nonnullable(PlannerInfo *root, UniqueKey *ukey,
							 List *nonnullable_vars)
{
	int			i = -1;

	while ((i = bms_next_member(ukey->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *ec = list_nth_node(EquivalenceClass,
											 root->eq_classes, i);
		ListCell   *lc;

		/* The EC must hold a Var some clause forces non-NULL */
		foreach(lc, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);

			if (IsA(em->em_expr, Var) &&
				var_in_nonnullable_vars((Var *) em->em_expr, nonnullable_vars))
				break;
		}
		if (lc == NULL)
			return false;
	}

	return true;
}

/*
 * var_in_nonnullable_vars
 *		Is "var" listed in the "nonnullable_vars" multibitmapset?
 */
static bool
var_in_nonnullable_vars(Var *var, List *nonnullable_vars)
{
	/* skip upper-level Vars */
	if (var->varlevelsup != 0)
		return false;

	return mbms_is_member(var->varno,
						  var->varattno - FirstLowInvalidHeapAttributeNumber,
						  nonnullable_vars);
}

/*
 * populate_grouped_rel_uniquekeys
 *		Deduce the unique keys of the output of the query's grouping step: it
 *		is NULL-aware distinct over the grouping columns.  With no
 *		non-redundant grouping columns, at most one row is emitted.
 *
 * When the grouping step is a no-op, the output is just the input, and every
 * key of the input survives.
 */
void
populate_grouped_rel_uniquekeys(PlannerInfo *root, RelOptInfo *grouped_rel,
								RelOptInfo *input_rel, bool is_noop)
{
	Query	   *parse = root->parse;
	Bitmapset  *key_ecs;

	/* Set-returning functions are expanded above the grouping step */
	if (parse->hasTargetSRFs)
		return;

	if (parse->groupingSets)
	{
		/*
		 * Grouping sets emit one row per set, so the output is distinct only
		 * when a single (necessarily empty) set remains, or GROUP BY DISTINCT
		 * collapses them to one; grouping sets over expressions (groupClause
		 * set) are not distinct.  Either way the result is one row: an empty
		 * key.
		 */
		if (parse->groupClause ||
			(!parse->groupDistinct &&
			 list_length(parse->groupingSets) != 1))
			return;

		add_uniquekey(grouped_rel, NULL, true);
		return;
	}

	/* Carry over the input's keys if grouping does no work */
	if (is_noop)
	{
		foreach_node(UniqueKey, ukey, input_rel->uniquekeys)
		{
			add_uniquekey(grouped_rel, ukey->eclass_indexes, ukey->null_aware);
		}
	}

	/* Collect the ECs of the query's grouping columns */
	if (!collect_clause_ecs(root, root->processed_groupClause, &key_ecs))
		return;

	if (!bms_is_subset(key_ecs, get_interesting_unique_ecs(root)))
		return;

	add_uniquekey(grouped_rel, key_ecs, true);
}

/*
 * populate_distinct_rel_uniquekeys
 *		Deduce the unique keys of the output of the query's DISTINCT step: it
 *		is NULL-aware distinct over the DISTINCT (or DISTINCT ON) columns.
 *
 * Like grouping, when the DISTINCT step is a no-op, the output is just the
 * input, and every key of the input survives.
 *
 * Unlike grouping, DISTINCT is applied after target-list SRF expansion, so its
 * output is distinct over the clause columns even when the tlist has SRFs.
 */
void
populate_distinct_rel_uniquekeys(PlannerInfo *root, RelOptInfo *distinct_rel,
								 RelOptInfo *input_rel, bool is_noop)
{
	Bitmapset  *key_ecs;

	/*
	 * Nothing within a query level consumes distinctness after the DISTINCT
	 * step, so these keys are useful only to a parent query.  Don't bother
	 * unless we are a subquery.
	 */
	if (root->query_level <= 1)
		return;

	/* Carry over the input's keys if DISTINCT does no work */
	if (is_noop)
	{
		foreach_node(UniqueKey, ukey, input_rel->uniquekeys)
		{
			add_uniquekey(distinct_rel, ukey->eclass_indexes, ukey->null_aware);
		}
	}

	/* Collect the ECs of the query's DISTINCT columns */
	if (!collect_clause_ecs(root, root->processed_distinctClause, &key_ecs))
		return;

	if (!bms_is_subset(key_ecs, get_interesting_unique_ecs(root)))
		return;

	add_uniquekey(distinct_rel, key_ecs, true);
}

/*
 * populate_unique_rel_uniquekeys
 *		Deduce the unique keys established by unique-ifying a semijoin's RHS.
 *
 * Like grouping, when the unique-ification step is a no-op, the output is just
 * the input, and every key of the input survives.
 */
void
populate_unique_rel_uniquekeys(PlannerInfo *root, RelOptInfo *unique_rel,
							   RelOptInfo *input_rel, SpecialJoinInfo *sjinfo,
							   bool is_noop)
{
	Bitmapset  *key_ecs = NULL;
	ListCell   *lc1;
	ListCell   *lc2;

	Assert(sjinfo->jointype == JOIN_SEMI);

	/* Carry over the input's keys if DISTINCT does no work */
	if (is_noop)
	{
		foreach_node(UniqueKey, ukey, input_rel->uniquekeys)
		{
			add_uniquekey(unique_rel, ukey->eclass_indexes, ukey->null_aware);
		}
	}

	/*
	 * Unique-ification deduplicates the RHS over the semijoin's right-hand
	 * expressions, keeping one row per group (treating NULLs as equal, like
	 * GROUP BY), so the result is NULL-aware distinct over those expressions.
	 */
	forboth(lc1, sjinfo->semi_rhs_exprs, lc2, sjinfo->semi_operators)
	{
		Expr	   *expr = (Expr *) lfirst(lc1);
		List	   *opfamilies = get_mergejoin_opfamilies(lfirst_oid(lc2));
		Oid			collation = exprCollation((Node *) expr);
		EquivalenceClass *ec;
		int			pos;

		/* The key needs all these expressions, so we can't skip one */
		if (opfamilies == NIL)
			return;

		/* Match in base-EC space: strip any outer-join nulling first */
		expr = (Expr *) remove_nulling_relids((Node *) expr,
											  root->outer_join_rels, NULL);
		pos = find_ec_position(root, expr, opfamilies, collation, NULL);
		if (pos < 0)
			return;

		ec = list_nth_node(EquivalenceClass, root->eq_classes, pos);

		/* A column equated to a constant is not needed in the key */
		if (ec->ec_has_const)
			continue;

		key_ecs = bms_add_member(key_ecs, pos);
	}

	add_uniquekey(unique_rel, key_ecs, true);
}

/*
 * uniquekeys_distinct_is_noop
 *		Is the input relation of the query's DISTINCT step already provably
 *		distinct over the DISTINCT columns, making the step a no-op?
 */
bool
uniquekeys_distinct_is_noop(PlannerInfo *root, RelOptInfo *input_rel)
{
	Query	   *parse = root->parse;
	Bitmapset  *covered = NULL;

	if (input_rel->uniquekeys == NIL)
		return false;

	if (parse->hasDistinctOn)
		return false;

	if (parse->hasTargetSRFs)
		return false;

	/* Collect the DISTINCT columns' ECs */
	foreach_node(PathKey, pathkey, root->distinct_pathkeys)
	{
		int			pos = base_ec_position(root, pathkey->pk_eclass);

		if (pos >= 0)
			covered = bms_add_member(covered, pos);
	}

	/* DISTINCT considers NULLs equal, so only NULL-aware keys qualify */
	return rel_has_uniquekey_within(input_rel, covered, true);
}

/*
 * uniquekeys_grouping_is_noop
 *		Is the input relation of the query's grouping step already provably
 *		distinct over the grouping columns, i.e. does every group consist of
 *		exactly one row?
 */
bool
uniquekeys_grouping_is_noop(PlannerInfo *root, RelOptInfo *input_rel)
{
	Query	   *parse = root->parse;
	Bitmapset  *covered = NULL;
	ListCell   *lc;

	if (input_rel->uniquekeys == NIL)
		return false;

	if (parse->groupClause == NIL ||
		parse->groupingSets != NIL ||
		parse->hasAggs ||
		parse->havingQual != NULL)
		return false;

	/* Collect the grouping columns' ECs from the leading pathkeys */
	foreach(lc, root->group_pathkeys)
	{
		int			pos;

		/* Only the leading num_groupby_pathkeys are grouping columns */
		if (foreach_current_index(lc) >= root->num_groupby_pathkeys)
			break;

		pos = base_ec_position(root, ((PathKey *) lfirst(lc))->pk_eclass);
		if (pos >= 0)
			covered = bms_add_member(covered, pos);
	}

	/* like DISTINCT, grouping considers NULLs equal */
	return rel_has_uniquekey_within(input_rel, covered, true);
}

/*
 * uniquekeys_match_join_clauses
 *		Do the given join clauses prove the relation distinct, i.e. do their
 *		rel-side expressions cover some unique key of it?
 *
 * The caller must have already determined that each clause is a mergejoinable
 * equality with an expression of this relation on one side, with the transient
 * outer_is_left flags set accordingly; this matches the clause lists built for
 * rel_is_distinct_for().
 */
bool
uniquekeys_match_join_clauses(PlannerInfo *root, RelOptInfo *rel,
							  List *clause_list)
{
	Bitmapset  *covered = NULL;

	if (rel->uniquekeys == NIL)
		return false;

	foreach_node(RestrictInfo, rinfo, clause_list)
	{
		EquivalenceClass *ec;
		int			pos;

		/* caller identified the inner side for us */
		ec = rinfo->outer_is_left ? rinfo->right_ec : rinfo->left_ec;
		if (ec && (pos = base_ec_position(root, ec)) >= 0)
			covered = bms_add_member(covered, pos);
	}

	/*
	 * Nullable keys are fine here: rows with NULL key values cannot match any
	 * outer row through these strict clauses.
	 */
	return rel_has_uniquekey_within(rel, covered, false);
}

/*
 * uniquekeys_uniquification_is_noop
 *		Would unique-ifying the given semijoin RHS relation be a no-op, because
 *		its output is already provably distinct over the unique-ification
 *		columns?
 *
 * Nullable keys are acceptable: the semijoin operators are strict, so rows
 * with NULL unique-ification columns match nothing, and not collapsing their
 * duplicates cannot change the result.
 */
bool
uniquekeys_uniquification_is_noop(PlannerInfo *root, RelOptInfo *rel,
								  SpecialJoinInfo *sjinfo)
{
	Bitmapset  *covered = NULL;
	ListCell   *lc1;
	ListCell   *lc2;

	Assert(sjinfo->jointype == JOIN_SEMI);

	if (rel->uniquekeys == NIL)
		return false;

	/*
	 * Collect the unique-ification columns' ECs.  Columns with no matching EC
	 * are simply left out: unique-ification groups by all the columns and
	 * keeps one row per group, and a rel distinct over a subset of them is
	 * necessarily distinct over the full set.  So a key covering any subset
	 * of the columns already proves the step a no-op.
	 */
	forboth(lc1, sjinfo->semi_rhs_exprs, lc2, sjinfo->semi_operators)
	{
		Expr	   *expr = (Expr *) lfirst(lc1);
		List	   *opfamilies = get_mergejoin_opfamilies(lfirst_oid(lc2));
		Oid			collation = exprCollation((Node *) expr);
		int			pos;

		if (opfamilies == NIL)
			continue;
		/* Match in base-EC space: strip any outer-join nulling first */
		expr = (Expr *) remove_nulling_relids((Node *) expr,
											  root->outer_join_rels, NULL);
		pos = find_ec_position(root, expr, opfamilies, collation, NULL);
		if (pos >= 0)
			covered = bms_add_member(covered, pos);
	}

	return rel_has_uniquekey_within(rel, covered, false);
}

/*
 * get_interesting_unique_ecs
 *		Determine which ECs are worth tracking in UniqueKeys: those of the
 *		DISTINCT clause, of the GROUP BY clause when its step might be a no-op,
 *		and those that can generate join clauses.  The last group is the
 *		multi-relation ECs, plus the single-member ECs on each side of a
 *		mergejoinable outer-join clause; the latter never merge into a join EC
 *		of their own, but inner_unique proofs look them up, so we track them
 *		too.
 *
 * Each contributing EC is recorded as its base (un-nulled) position via
 * add_base_ec_positions, which may materialize a base EC on demand; the
 * resulting set therefore contains only base-EC positions.
 *
 * Computed once and cached.  ECs created later (e.g. for sort pathkeys) are
 * never interesting, which is fine: keys come from clauses and pathkeys that
 * predate the join search.
 */
static Bitmapset *
get_interesting_unique_ecs(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Bitmapset  *result = NULL;
	int			necs;
	int			i;

	if (root->interesting_unique_ecs_valid)
		return root->interesting_unique_ecs;

	/* The multi-relation ECs, i.e. those that can generate join clauses */
	necs = list_length(root->eq_classes);
	for (i = 0; i < necs; i++)
	{
		EquivalenceClass *ec = list_nth_node(EquivalenceClass,
											 root->eq_classes, i);
		Relids		base_relids;

		if (ec->ec_has_volatile)
			continue;

		base_relids = bms_difference(ec->ec_relids, root->outer_join_rels);

		if (bms_membership(base_relids) == BMS_MULTIPLE)
			result = add_base_ec_positions(root, result, ec);

		bms_free(base_relids);
	}

	/* The DISTINCT columns */
	foreach_node(PathKey, pathkey, root->distinct_pathkeys)
	{
		result = add_base_ec_positions(root, result, pathkey->pk_eclass);
	}

	/* The GROUP BY columns, when the grouping step might be a no-op */
	if (parse->groupClause != NIL &&
		parse->groupingSets == NIL &&
		!parse->hasAggs &&
		parse->havingQual == NULL)
	{
		ListCell   *lc;

		foreach(lc, root->group_pathkeys)
		{
			/* Only the leading num_groupby_pathkeys are grouping columns */
			if (foreach_current_index(lc) >= root->num_groupby_pathkeys)
				break;

			result = add_base_ec_positions(root, result,
										   ((PathKey *) lfirst(lc))->pk_eclass);
		}
	}

	/* Both sides of each mergejoinable outer-join clause */
	result = add_ojclause_ecs(root, result, root->left_join_clauses);
	result = add_ojclause_ecs(root, result, root->right_join_clauses);
	result = add_ojclause_ecs(root, result, root->full_join_clauses);

	root->interesting_unique_ecs = result;
	root->interesting_unique_ecs_valid = true;

	return result;
}

/*
 * add_ojclause_ecs
 *		Subroutine for get_interesting_unique_ecs: add the ECs of both sides of
 *		the given mergejoinable outer-join clauses.
 */
static Bitmapset *
add_ojclause_ecs(PlannerInfo *root, Bitmapset *result, List *ojclauses)
{
	foreach_node(OuterJoinClauseInfo, ojcinfo, ojclauses)
	{
		RestrictInfo *rinfo = ojcinfo->rinfo;

		if (rinfo->left_ec)
			result = add_base_ec_positions(root, result, rinfo->left_ec);
		if (rinfo->right_ec)
			result = add_base_ec_positions(root, result, rinfo->right_ec);
	}
	return result;
}

/*
 * add_base_ec_positions
 *		Subroutine for get_interesting_unique_ecs: add the position(s) of the
 *		base (un-nulled) EC(s) corresponding to "ec" to "result".
 *
 * Keys live in base-EC space, so the interesting set holds base positions.  If
 * "ec" is already a base EC, we add its own position.  Otherwise we strip the
 * nullingrels and use get_eclass_for_sort_expr to bring the base EC into
 * existence if it doesn't already exist, so the base key can later be built
 * and matched.
 */
static Bitmapset *
add_base_ec_positions(PlannerInfo *root, Bitmapset *result,
					  EquivalenceClass *ec)
{
	/* The passed ec might be non-canonical, so chase up to the top */
	while (ec->ec_merged)
		ec = ec->ec_merged;

	/* A const-only EC has no base position */
	if (bms_is_empty(ec->ec_relids))
		return result;

	/* If ec is already a base EC, we add its own position */
	if (!bms_overlap(ec->ec_relids, root->outer_join_rels))
		return bms_add_member(result, ec->ec_index);

	/* Otherwise materialize the base EC of each nulled member */
	foreach_node(EquivalenceMember, em, ec->ec_members)
	{
		Node	   *base_expr;
		EquivalenceClass *base_ec;

		if (em->em_is_const ||
			!bms_overlap(em->em_relids, root->outer_join_rels))
			continue;

		base_expr = remove_nulling_relids((Node *) em->em_expr,
										  root->outer_join_rels, NULL);
		base_ec = get_eclass_for_sort_expr(root, (Expr *) base_expr,
										   ec->ec_opfamilies,
										   em->em_datatype,
										   ec->ec_collation,
										   0, NULL, true);
		result = bms_add_member(result, base_ec->ec_index);
	}

	return result;
}

/*
 * collect_clause_ecs
 *		Find the ECs of the columns of a grouping or DISTINCT clause, omitting
 *		columns that are equivalent to constants.
 *
 * If any column has no matching EC, return false: the caller is building a
 * rel's key, which must account for all columns.
 */
static bool
collect_clause_ecs(PlannerInfo *root, List *clause, Bitmapset **ecs_p)
{
	Bitmapset  *ecs = NULL;

	*ecs_p = NULL;

	foreach_node(SortGroupClause, sgc, clause)
	{
		TargetEntry *tle = get_sortgroupclause_tle(sgc,
												   root->parse->targetList);
		List	   *opfamilies = get_mergejoin_opfamilies(sgc->eqop);
		Oid			collation = exprCollation((Node *) tle->expr);
		EquivalenceClass *ec;
		int			pos = -1;

		if (opfamilies != NIL)
		{
			/* Match in base-EC space: strip any outer-join nulling first */
			Expr	   *expr;

			expr = (Expr *) remove_nulling_relids((Node *) tle->expr,
												  root->outer_join_rels, NULL);
			pos = find_ec_position(root, expr, opfamilies, collation, NULL);
		}
		if (pos < 0)
			return false;
		ec = (EquivalenceClass *) list_nth(root->eq_classes, pos);

		if (!ec->ec_has_const)
			ecs = bms_add_member(ecs, pos);
	}

	*ecs_p = ecs;
	return true;
}

/*
 * add_uniquekey
 *		Add a UniqueKey to a relation's uniquekeys list, maintaining
 *		minimality: a key dominated by another one, whose EC set is a
 *		subset and whose NULL-awareness is at least as strong, is never
 *		kept.
 */
static void
add_uniquekey(RelOptInfo *rel, Bitmapset *eclass_indexes, bool null_aware)
{
	UniqueKey  *ukey;
	ListCell   *lc;

	foreach(lc, rel->uniquekeys)
	{
		UniqueKey  *other = lfirst_node(UniqueKey, lc);

		/* Reject the new key if some existing key dominates it */
		if ((other->null_aware || !null_aware) &&
			bms_is_subset(other->eclass_indexes, eclass_indexes))
			return;
		/* ... and remove existing keys the new one dominates */
		if ((null_aware || !other->null_aware) &&
			bms_is_subset(eclass_indexes, other->eclass_indexes))
			rel->uniquekeys = foreach_delete_current(rel->uniquekeys, lc);
	}

	if (list_length(rel->uniquekeys) >= MAX_UNIQUEKEYS)
		return;

	ukey = makeNode(UniqueKey);
	ukey->eclass_indexes = eclass_indexes;
	ukey->null_aware = null_aware;
	rel->uniquekeys = lappend(rel->uniquekeys, ukey);
}

/*
 * rel_has_uniquekey_within
 *		Does the rel have a key whose ECs all appear in "ecs"?  If
 *		need_null_aware, only NULL-aware keys qualify.
 */
static bool
rel_has_uniquekey_within(RelOptInfo *rel, Bitmapset *ecs, bool need_null_aware)
{
	ListCell   *lc;

	foreach(lc, rel->uniquekeys)
	{
		UniqueKey  *ukey = lfirst_node(UniqueKey, lc);

		if ((!need_null_aware || ukey->null_aware) &&
			bms_is_subset(ukey->eclass_indexes, ecs))
			return true;
	}
	return false;
}

/*
 * side_is_unique
 *		Check whether "side" provably contains at most one row matching any
 *		given row of "otherside", using the join's restrictlist.
 */
static bool
side_is_unique(PlannerInfo *root, RelOptInfo *joinrel,
			   RelOptInfo *side, RelOptInfo *otherside,
			   JoinType jointype, List *restrictlist)
{
	/* A one-row rel is trivially unique, even with no join clauses */
	if (rel_has_uniquekey_within(side, NULL, false))
		return true;

	return innerrel_is_unique(root, joinrel->relids, otherside->relids,
							  side, jointype, restrictlist, false);
}

/*
 * find_ec_position
 *		Find the position in root->eq_classes of an EC containing an expression
 *		matching "expr".  If "candidates" is non-NULL, consider only the ECs at
 *		those positions.  Returns -1 if there is none.
 *
 * Keys live in base-EC space, so a caller matching a reference that an outer
 * join may have null-extended must first strip its varnullingrels.
 */
static int
find_ec_position(PlannerInfo *root, Expr *expr,
				 List *opfamilies, Oid collation, Bitmapset *candidates)
{
	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		int			pos = ec->ec_index;

		/* ec_index is final once EC merging is done */
		Assert(list_nth(root->eq_classes, pos) == ec);

		if (candidates && !bms_is_member(pos, candidates))
			continue;
		if (ec->ec_has_volatile ||
			ec->ec_collation != collation ||
			!equal(ec->ec_opfamilies, opfamilies))
			continue;
		if (find_ec_member_matching_expr(ec, expr, NULL))
			return pos;
	}
	return -1;
}

/*
 * base_ec_position
 *		Find the position of the base (un-nulled) EC corresponding to "ec".
 *
 * A consumer's column reference may belong to a nulled EC, while keys are kept
 * on the base ECs.  Take a usable member of "ec", strip it to base form, and
 * look that up.  If "ec" is already a base EC, this returns its own position.
 */
static int
base_ec_position(PlannerInfo *root, EquivalenceClass *ec)
{
	/* The passed ec might be non-canonical, so chase up to the top */
	while (ec->ec_merged)
		ec = ec->ec_merged;

	/* ec_index is final once EC merging is done */
	Assert(list_nth(root->eq_classes, ec->ec_index) == ec);

	/* A const-only EC has no base position */
	if (bms_is_empty(ec->ec_relids))
		return -1;

	/* If ec is already a base EC, return its own position */
	if (!bms_overlap(ec->ec_relids, root->outer_join_rels))
		return ec->ec_index;

	foreach_node(EquivalenceMember, em, ec->ec_members)
	{
		Node	   *expr;
		int			pos;

		if (em->em_is_const || bms_is_empty(em->em_relids))
			continue;

		expr = (Node *) em->em_expr;
		if (bms_overlap(em->em_relids, root->outer_join_rels))
			expr = remove_nulling_relids(expr, root->outer_join_rels, NULL);

		pos = find_ec_position(root, (Expr *) expr, ec->ec_opfamilies,
							   ec->ec_collation, NULL);
		if (pos >= 0)
			return pos;
	}

	return -1;
}
