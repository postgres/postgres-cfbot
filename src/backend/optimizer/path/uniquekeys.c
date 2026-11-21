/*-------------------------------------------------------------------------
 *
 * uniquekeys.c
 *	  Utilities for proving relations distinct using UniqueKeys
 *
 * A UniqueKey is a set of EquivalenceClasses that no two rows of a base
 * relation agree on.  Plain relations get them from their unique indexes.
 * Join relations have no UniqueKeys of their own: whether a join relation is
 * distinct is worked out when needed, from the UniqueKeys of the base
 * relations it consists of.
 *
 * There are two kinds of question.  Mergejoinable join clauses are assumed
 * to be strict, so when matching them we need not care about rows that have
 * NULLs in the columns of interest.  DISTINCT and GROUP BY, on the other
 * hand, consider NULLs equal to each other, so for them only UniqueKeys that
 * hold on those terms can be used.
 *
 * See src/backend/optimizer/README for more information.
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
#include "nodes/multibitmapset.h"
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/lsyscache.h"

static bool joinrel_is_distinct_for_ecs(PlannerInfo *root, RelOptInfo *joinrel,
										Bitmapset *known_ecs,
										bool nulls_equal);
static Bitmapset *get_joined_ecs(PlannerInfo *root, RelOptInfo *joinrel);
static void build_base_rel_uniquekeys(PlannerInfo *root);
static void build_plain_rel_uniquekeys(PlannerInfo *root, RelOptInfo *rel);
static void add_uniquekey(RelOptInfo *rel, Bitmapset *eclass_indexes,
						  bool null_aware);
static int	find_ec_index(PlannerInfo *root, RelOptInfo *rel, Expr *expr,
						  List *opfamilies, Oid collation);
static bool rel_has_uniquekey_in(PlannerInfo *root, RelOptInfo *rel,
								 Bitmapset *eclass_indexes,
								 Bitmapset *joined_ecs,
								 bool nulls_equal);
static Bitmapset *get_own_eclass_indexes(PlannerInfo *root, RelOptInfo *rel);


/*
 * joinrel_supports_distinctness
 *		Could the join relation possibly be proven distinct?
 *
 * This is a pre-checking function for joinrel_is_distinct_for(), in the same
 * way as rel_supports_distinctness() is for rel_is_distinct_for().
 */
bool
joinrel_supports_distinctness(PlannerInfo *root, RelOptInfo *joinrel)
{
	Assert(IS_JOIN_REL(joinrel));

	/* A grouped join rel's rows are not combinations of base rel rows */
	if (IS_GROUPED_REL(joinrel))
		return false;

	/*
	 * We don't handle join rels that include outer joins.  A left join pushed
	 * down per outer-join identity 3 is missed here, which is harmless since
	 * its clauses are in no EC.
	 */
	if (bms_overlap(joinrel->relids, root->outer_join_rels))
		return false;

	build_base_rel_uniquekeys(root);

	/* Every base rel in the join must have a UniqueKey */
	return !bms_overlap(joinrel->relids, root->no_uniquekey_relids);
}

/*
 * joinrel_is_distinct_for
 *		Does the join relation return only distinct rows according to
 *		clause_list?
 *
 * clause_list is as for rel_is_distinct_for(): each clause is a mergejoinable
 * equality with an expression of this relation on one side, as identified by
 * its outer_is_left flag, and an expression not involving this relation on
 * the other.  Return true if no two rows emitted by the join relation could
 * possibly join to the same row of the other relation.
 */
bool
joinrel_is_distinct_for(PlannerInfo *root, RelOptInfo *joinrel,
						List *clause_list)
{
	Bitmapset  *known_ecs = NULL;

	if (!joinrel_supports_distinctness(root, joinrel))
		return false;

	foreach_node(RestrictInfo, rinfo, clause_list)
	{
		EquivalenceClass *ec;

		/* caller identified the side belonging to this relation for us */
		ec = rinfo->outer_is_left ? rinfo->right_ec : rinfo->left_ec;
		if (ec == NULL)
			continue;

		/* the cached EC might be non-canonical, so chase up to the top */
		while (ec->ec_merged)
			ec = ec->ec_merged;

		known_ecs = bms_add_member(known_ecs, ec->ec_index);
	}

	/* The clauses are mergejoinable, hence strict, so NULLs don't matter */
	return joinrel_is_distinct_for_ecs(root, joinrel, known_ecs, false);
}

/*
 * rel_is_distinct_for_pathkeys
 *		Does the relation return only distinct rows according to the given
 *		pathkeys?
 *
 * Return true if no two rows emitted by the relation could possibly have
 * equal values in all the pathkeys' ECs, with NULLs considered equal to each
 * other.  The sort ordering of the pathkeys is of no interest.
 *
 * An empty list means all rows are considered equal, so the relation is then
 * distinct only if it has at most one row.
 */
bool
rel_is_distinct_for_pathkeys(PlannerInfo *root, RelOptInfo *rel,
							 List *pathkeys)
{
	Bitmapset  *known_ecs = NULL;

	if (IS_JOIN_REL(rel))
	{
		if (!joinrel_supports_distinctness(root, rel))
			return false;
	}
	else if (rel->reloptkind == RELOPT_BASEREL)
	{
		/* a base rel without UniqueKeys just fails the lookup below */
		build_base_rel_uniquekeys(root);
	}
	else
		return false;			/* we know nothing about other relations */

	foreach_node(PathKey, pathkey, pathkeys)
	{
		known_ecs = bms_add_member(known_ecs, pathkey->pk_eclass->ec_index);
	}

	if (IS_JOIN_REL(rel))
		return joinrel_is_distinct_for_ecs(root, rel, known_ecs, true);

	return rel_has_uniquekey_in(root, rel, known_ecs, NULL, true);
}

/*
 * joinrel_is_distinct_for_ecs
 *		Does the join relation return only distinct rows according to the
 *		given set of ECs?
 *
 * Return true if no two rows emitted by the join relation could possibly
 * have equal values in all the ECs of known_ecs, which is a set of indexes
 * into root->eq_classes.  If nulls_equal is true, NULLs are considered equal
 * to each other; otherwise rows having a NULL in any of the ECs are ignored.
 *
 * A join emits each combination of its base relations' rows at most once.
 * So the join is distinct if fixing the values of the known ECs leaves only
 * one possible row for each of its base relations.  A base relation with a
 * UniqueKey within the set of known ECs can have only one row, and so it
 * fixes the value of every EC that has a member belonging to it; those ECs
 * are added to the set.  We repeat this until all base relations are
 * accounted for or we can make no more progress.
 *
 * The caller must have checked joinrel_supports_distinctness().
 */
static bool
joinrel_is_distinct_for_ecs(PlannerInfo *root, RelOptInfo *joinrel,
							Bitmapset *known_ecs, bool nulls_equal)
{
	Bitmapset  *joined_ecs = NULL;
	Relids		remaining;
	bool		found;

	/*
	 * When NULLs are considered equal, a UniqueKey that is not null_aware can
	 * still be used if join clauses keep its ECs from being NULL; see
	 * rel_has_uniquekey_in().  So find those ECs.
	 */
	if (nulls_equal)
		joined_ecs = get_joined_ecs(root, joinrel);

	/* There are no outer join relids, so all the relids are base relations */
	remaining = bms_copy(joinrel->relids);

	do
	{
		int			relid = -1;

		found = false;

		while ((relid = bms_next_member(remaining, relid)) >= 0)
		{
			RelOptInfo *rel = find_base_rel(root, relid);

			if (!rel_has_uniquekey_in(root, rel, known_ecs, joined_ecs,
									  nulls_equal))
				continue;

			remaining = bms_del_member(remaining, relid);
			known_ecs = bms_add_members(known_ecs,
										get_own_eclass_indexes(root, rel));
			found = true;
		}
	} while (found && remaining != NULL);

	return remaining == NULL;
}

/*
 * get_joined_ecs
 *		Get the indexes of the ECs that are known to be non-NULL in the join
 *		relation, because a join clause within it equates two of their
 *		members.
 *
 * The members of an EC are equated using mergejoinable operators, which we
 * assume to be strict (as nodeMergejoin.c does).  So an EC with two members
 * that can be computed within the join relation cannot be NULL there.
 *
 * This is only of interest for UniqueKeys that are not null_aware, so to
 * save work we look at just the ECs in those.
 */
static Bitmapset *
get_joined_ecs(PlannerInfo *root, RelOptInfo *joinrel)
{
	Bitmapset  *result = NULL;
	int			relid = -1;

	while ((relid = bms_next_member(joinrel->relids, relid)) >= 0)
	{
		RelOptInfo *rel = find_base_rel(root, relid);

		foreach_node(UniqueKey, ukey, rel->uniquekeys)
		{
			int			i = -1;

			if (ukey->null_aware)
				continue;

			while ((i = bms_next_member(ukey->eclass_indexes, i)) >= 0)
			{
				EquivalenceClass *ec;
				int			nmembers = 0;

				if (bms_is_member(i, result))
					continue;

				/* The members of a broken EC are not known to be equal */
				ec = list_nth_node(EquivalenceClass, root->eq_classes, i);
				if (ec->ec_broken)
					continue;

				foreach_node(EquivalenceMember, em, ec->ec_members)
				{
					if (!em->em_is_const &&
						bms_is_subset(em->em_relids, joinrel->relids) &&
						++nmembers >= 2)
					{
						result = bms_add_member(result, i);
						break;
					}
				}
			}
		}
	}

	return result;
}

/*
 * build_base_rel_uniquekeys
 *		Build the UniqueKeys of all base relations, if not already done.
 *
 * We do this the first time UniqueKeys are needed, so that queries that
 * never need them don't pay for it.  Also set root->no_uniquekey_relids.
 */
static void
build_base_rel_uniquekeys(PlannerInfo *root)
{
	MemoryContext oldcontext;
	Relids		no_uniquekey_relids = NULL;
	int			rti;

	if (root->uniquekeys_built)
		return;

	/* UniqueKeys refer to ECs by index, so the EC list must be final */
	Assert(root->ec_merging_done);

	/* Be sure to keep the results in the planner_cxt even if within GEQO */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (rel == NULL)
			continue;

		/* Currently we only know about plain relations */
		if (rel->reloptkind == RELOPT_BASEREL &&
			rel->rtekind == RTE_RELATION)
			build_plain_rel_uniquekeys(root, rel);

		if (rel->uniquekeys == NIL)
			no_uniquekey_relids = bms_add_member(no_uniquekey_relids, rti);
	}

	/*
	 * We don't handle semijoins and antijoins.  Rather than checking for them
	 * in each join relation, treat their right-hand relations as having no
	 * UniqueKeys.
	 */
	foreach_node(SpecialJoinInfo, sjinfo, root->join_info_list)
	{
		if (sjinfo->jointype == JOIN_SEMI || sjinfo->jointype == JOIN_ANTI)
			no_uniquekey_relids = bms_add_members(no_uniquekey_relids,
												  sjinfo->syn_righthand);
	}

	root->no_uniquekey_relids = no_uniquekey_relids;
	root->uniquekeys_built = true;

	MemoryContextSwitchTo(oldcontext);
}

/*
 * build_plain_rel_uniquekeys
 *		Build UniqueKeys for a plain relation from its unique indexes.
 *
 * An index column that is in no EC cannot be matched by any clause, so an
 * index having such a column yields no UniqueKey.
 */
static void
build_plain_rel_uniquekeys(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *nonnullable_vars = NIL;
	bool		nonnullable_vars_valid = false;

	/* Short-circuit if no EC mentions this relation */
	if (rel->eclass_indexes == NULL)
		return;

	foreach_node(IndexOptInfo, ind, rel->indexlist)
	{
		Bitmapset  *key_ecs = NULL;
		bool		null_aware = true;
		int			c;

		/*
		 * If the index is not unique, or not immediately enforced, or if it's
		 * a partial index, it's useless here.
		 */
		if (!ind->unique || !ind->immediate || ind->indpred != NIL)
			continue;

		for (c = 0; c < ind->nkeycolumns; c++)
		{
			TargetEntry *tle = list_nth_node(TargetEntry, ind->indextlist, c);
			EquivalenceClass *ec;
			Oid			eq_op;
			int			ec_index;

			/*
			 * An EC's opfamily list comes from its equality operator, which
			 * can belong to more opfamilies than just the index's.  So look
			 * up the index column's equality operator to compare with.
			 */
			eq_op = get_opfamily_member_for_cmptype(ind->opfamily[c],
													ind->opcintype[c],
													ind->opcintype[c],
													COMPARE_EQ);
			if (!OidIsValid(eq_op))
				break;

			ec_index = find_ec_index(root, rel, tle->expr,
									 get_mergejoin_opfamilies(eq_op),
									 ind->indexcollations[c]);
			if (ec_index < 0)
				break;

			/*
			 * A column that is equal to a constant has the same value in all
			 * the relation's rows, so the remaining columns are unique
			 * without it.
			 */
			ec = list_nth_node(EquivalenceClass, root->eq_classes, ec_index);
			if (ec->ec_has_const)
				continue;

			key_ecs = bms_add_member(key_ecs, ec_index);

			/*
			 * Unless the index is NULLS NOT DISTINCT, it allows duplicates
			 * among rows that have a NULL in this column.  So the UniqueKey
			 * is not null_aware unless the column cannot be NULL, due to a
			 * NOT NULL constraint or a strict restriction clause.
			 */
			if (null_aware && !ind->nullsnotdistinct)
			{
				int			attno = ind->indexkeys[c];

				if (attno == 0)
					null_aware = false;
				else if (!bms_is_member(attno, rel->notnullattnums))
				{
					if (!nonnullable_vars_valid)
					{
						foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
						{
							nonnullable_vars =
								mbms_add_members(nonnullable_vars,
												 find_nonnullable_vars((Node *) rinfo->clause));
						}
						nonnullable_vars_valid = true;
					}

					if (!mbms_is_member(rel->relid,
										attno - FirstLowInvalidHeapAttributeNumber,
										nonnullable_vars))
						null_aware = false;
				}
			}
		}

		/* Add the UniqueKey if we matched all the index columns */
		if (c == ind->nkeycolumns)
			add_uniquekey(rel, key_ecs, null_aware);
	}
}

/*
 * add_uniquekey
 *		Add a UniqueKey for the given set of ECs to the relation, unless it
 *		is redundant.
 *
 * A UniqueKey is redundant if another one of the relation is a subset of it
 * and can be used wherever it can.
 */
static void
add_uniquekey(RelOptInfo *rel, Bitmapset *eclass_indexes,
			  bool null_aware)
{
	UniqueKey  *ukey;
	ListCell   *lc;

	foreach(lc, rel->uniquekeys)
	{
		UniqueKey  *other = lfirst_node(UniqueKey, lc);

		/* If an existing key makes the new one redundant, we're done */
		if (bms_is_subset(other->eclass_indexes, eclass_indexes) &&
			(other->null_aware || !null_aware))
			return;

		/* Remove any existing key that the new one makes redundant */
		if (bms_is_subset(eclass_indexes, other->eclass_indexes) &&
			(null_aware || !other->null_aware))
			rel->uniquekeys = foreach_delete_current(rel->uniquekeys, lc);
	}

	ukey = makeNode(UniqueKey);
	ukey->eclass_indexes = eclass_indexes;
	ukey->null_aware = null_aware;
	rel->uniquekeys = lappend(rel->uniquekeys, ukey);
}

/*
 * find_ec_index
 *		Find the EC that has the given expression of the given base relation
 *		as a member, and that uses the given opfamilies and collation.
 *
 * Returns the EC's index in root->eq_classes, or -1 if there is none.
 */
static int
find_ec_index(PlannerInfo *root, RelOptInfo *rel, Expr *expr,
			  List *opfamilies, Oid collation)
{
	int			i = -1;

	while ((i = bms_next_member(rel->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *ec = list_nth_node(EquivalenceClass,
											 root->eq_classes, i);

		/* The members of a broken EC are not known to be equal */
		if (ec->ec_broken || ec->ec_has_volatile)
			continue;

		if (ec->ec_collation != collation ||
			!equal(ec->ec_opfamilies, opfamilies))
			continue;

		if (find_ec_member_matching_expr(ec, expr, NULL))
			return i;
	}

	return -1;
}

/*
 * rel_has_uniquekey_in
 *		Does the relation have a UniqueKey that is a subset of the given set
 *		of ECs?
 *
 * If nulls_equal is true, the values of the ECs can be NULL, so only
 * UniqueKeys that are null_aware are of use, or ones whose ECs are all in
 * joined_ecs and so cannot be NULL (see get_joined_ecs).
 */
static bool
rel_has_uniquekey_in(PlannerInfo *root, RelOptInfo *rel,
					 Bitmapset *eclass_indexes, Bitmapset *joined_ecs,
					 bool nulls_equal)
{
	foreach_node(UniqueKey, ukey, rel->uniquekeys)
	{
		bool		broken = false;
		int			i = -1;

		if (nulls_equal && !ukey->null_aware &&
			!bms_is_subset(ukey->eclass_indexes, joined_ecs))
			continue;

		if (!bms_is_subset(ukey->eclass_indexes, eclass_indexes))
			continue;

		/* An EC can be found to be broken after the key was built */
		while ((i = bms_next_member(ukey->eclass_indexes, i)) >= 0)
		{
			EquivalenceClass *ec = list_nth_node(EquivalenceClass,
												 root->eq_classes, i);

			if (ec->ec_broken)
			{
				broken = true;
				break;
			}
		}

		if (!broken)
			return true;
	}

	return false;
}

/*
 * get_own_eclass_indexes
 *		Get the indexes of the ECs that have a member belonging to the given
 *		base relation alone.
 *
 * This is not the same as rel->eclass_indexes, which also includes ECs whose
 * members only use the relation together with other relations.
 *
 * The result is cached in the RelOptInfo.  An empty result is not cached,
 * but that is cheap to recompute.
 */
static Bitmapset *
get_own_eclass_indexes(PlannerInfo *root, RelOptInfo *rel)
{
	MemoryContext oldcontext;
	Bitmapset  *result = NULL;
	int			i = -1;

	if (rel->own_eclass_indexes != NULL)
		return rel->own_eclass_indexes;

	/* Be sure to keep the result in the planner_cxt even if within GEQO */
	oldcontext = MemoryContextSwitchTo(root->planner_cxt);

	while ((i = bms_next_member(rel->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *ec = list_nth_node(EquivalenceClass,
											 root->eq_classes, i);

		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			if (bms_equal(em->em_relids, rel->relids))
			{
				result = bms_add_member(result, i);
				break;
			}
		}
	}

	MemoryContextSwitchTo(oldcontext);

	rel->own_eclass_indexes = result;

	return result;
}
