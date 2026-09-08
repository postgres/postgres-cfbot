/*-------------------------------------------------------------------------
 *
 * joinhardness.c
 *	  Estimate the "hardness" (cost) of the dynamic-programming join search.
 *
 * The complexity of the exhaustive join order search (as implemented in the
 * standard_join_search) may be measured by the number of make_join_rel()
 * candidate pairs it considers.
 *
 * This number depends on the shape of the join graph, not merely on the
 * number of relations: an n-way chain is cubic, while an n-way clique or
 * star is exponential.
 *
 * That is why join_collapse_limit is such a poor way to judge complexity
 * of joins - joins with the same number of relations may have fundamentally
 * different complexity. The same issue applies to geqo_threshold. Those
 * values are too eager on large but sparse queries and too timid on dense
 * medium-sized ones. It's hard to pick a value that works for both.
 *
 * Per Moerkotte & Neumann (2006), the number of make_join_rel() candidates
 * equals the count of "connected subgraphs / connected complement" pairs
 * (#ccp) of the join graph.
 *
 * estimate_join_search_effort() computes an upper bound on #ccp, capped at
 * a caller-supplied budget (to allow aborting once we know the join search
 * is more complex).
 *
 * It first builds the "effective" join graph: with a vertex per initial
 * rel, and an edge between two rels whenever there is a join clause, an
 * EquivalenceClass-derived join clause, or a join-order restriction linking
 * them (have_relevant_joinclause() and have_join_order_restriction()
 * encapsulate exactly those tests).
 *
 * It then counts csg-cmp pairs with a budgeted DPccp enumeration that
 * aborts as soon as the accumulated work exceeds the budget. This makes the
 * estimate cheap even for catastrophic graphs (e.g. cliques), yet it is
 * exact (hence the estimate is as tight as possible) whenever the true
 * #ccp is below the budget.
 *
 * The estimate is monotone in the set of relations and edges, so it
 * composes naturally with recursive sub-joinlist planning in
 * make_rel_from_joinlist().
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/join_hardness/joinhardness.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "optimizer/geqo.h"
#include "optimizer/joininfo.h"
#include "optimizer/paths.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC_EXT(
					.name = "join_hardness",
					.version = PG_VERSION
);

/* GUCs */
static bool	join_hardness_enabled = false;
static bool	join_hardness_split = false;
static bool	join_hardness_fast = false;
static int	join_hardness_max_effort = 10000;

static join_search_hook_type prev_join_search_hook = NULL;

/*
 * The effective join graph over the initial rels, indexed 0 .. n-1.
 * neighbors[i] holds the indexes adjacent to vertex i.
 */
typedef struct JoinGraph
{
	int			n;				/* number of vertices */
	Bitmapset **neighbors;		/* adjacency, one Bitmapset per vertex */
} JoinGraph;

/*
 * Mutable state for the recursive enumeration.
 *
 * "work" counts every connected subgraph and every emitted csg-cmp pair.
 * This is the quantity measuring join search complexity, and compared
 * against the budget so that the whole walk is bounded regardless of
 * graph shape.
 *
 * "ccp" counts only the csg-cmp pairs, which is the metric we report when
 * the enumeration completes without aborting.
 *
 * XXX The small subtle difference between "work" and "ccp" counters is
 * that "work" is incremented even for subgraphs for which we don't find
 * any suitable complement.
 */
typedef struct JoinEnumState
{
	JoinGraph  *graph;
	int64		budget;			/* abort once work exceeds this */
	int64		work;			/* csg + ccp emissions so far */
	int64		ccp;			/* csg-cmp pairs so far */
	bool		aborted;		/* set true once budget is exceeded */
} JoinEnumState;

static JoinGraph *build_join_graph(PlannerInfo *root, List *initial_rels);
static void free_join_graph(JoinGraph *graph);
static int64 join_ordering_upper_bound(int n, int64 budget);
static int	count_join_graph_components(JoinGraph *graph);
static int64 clauseless_combination_effort(int c, int64 budget);
static Bitmapset *neighborhood(JoinEnumState *state, Bitmapset *s, Bitmapset *x);
static void on_connected_subgraph(JoinEnumState *state, Bitmapset *s1);

static void enumerate_csg(JoinEnumState *state, Bitmapset *s, Bitmapset *x);
static void enumerate_csg_subsets(JoinEnumState *state, Bitmapset *s,
								  Bitmapset *x, Bitmapset *nbr,
								  int *members, int k, int idx,
								  Bitmapset **subset);
static void enumerate_cmp(JoinEnumState *state, Bitmapset *s1);

static void grow_cmp(JoinEnumState *state, Bitmapset *s1, Bitmapset *s2,
					 Bitmapset *x);
static void grow_cmp_subsets(JoinEnumState *state, Bitmapset *s1, Bitmapset *s2,
							 Bitmapset *x, Bitmapset *nbr,
							 int *members, int k, int idx,
							 Bitmapset **subset);

static RelOptInfo *join_search_hardness_hook(PlannerInfo *root,
											 int levels_needed,
											 List *initial_rels);

/*
 * Module Load Callback
 */
void
_PG_init(void)
{
	DefineCustomBoolVariable("join_hardness.enable",
							 "enable calculating join hardness estimate.",
							 NULL,
							 &join_hardness_enabled,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("join_hardness.split",
							 "allow splitting joins that are too hard to plan.",
							 NULL,
							 &join_hardness_split,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("join_hardness.fast",
							 "allow fast-path for small joins",
							 NULL,
							 &join_hardness_fast,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomIntVariable("join_hardness.threshold",
							"maximum hardness before a join is considered too hard.",
							NULL,
							&join_hardness_max_effort,
							10000,
							1, INT_MAX,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("join_hardness");

	/* Install Hooks */
	prev_join_search_hook = join_search_hook;
	join_search_hook = join_search_hardness_hook;
}


/*
 * estimate_join_search_effort
 *		Return an upper bound on the number of make_join_rel() candidate pairs
 *		(the #ccp metric) that standard_join_search() would consider for the
 *		given set of initial rels, capped at "budget".
 *
 * The returned value is in the range [0, budget].  It equals the exact #ccp
 * when that is <= budget; otherwise it equals budget, signalling that the join
 * search is at least as hard as the budget allows.  The computation never does
 * more than O(budget) units of enumeration work, so it is cheap to call even
 * for pathological join graphs.
 *
 * If the budget is set to 0, it's not enforced.
 */
int64
estimate_join_search_effort(PlannerInfo *root, List *initial_rels,
							int64 budget)
{
	JoinEnumState state;
	JoinGraph   *graph;
	int			n = list_length(initial_rels);
	int			i;
	int64		max_complexity;
	int64		result;

	TimestampTz	ts_start,
				ts_end;
	uint64		microsec;

	ts_start = GetCurrentTimestamp();

	/* nothing to join */
	if (n <= 1)
		return 0;

	/*
	 * Calculate the (very loose) upper boundary from the number of relations.
	 * This grows very fast - at 6 tables it's ~30k, at 8 it's already ~17M.
	 * But for very small joins it allows us to do without the join graph.
	 */
	max_complexity = join_ordering_upper_bound(n, budget);
	if (max_complexity < budget)
		return max_complexity;

	/* build the adjacency matrix representing the graph */
	graph = build_join_graph(root, initial_rels);

	/* initialize the enumeration state */
	state.graph = graph;
	state.budget = budget;
	state.work = 0.0;
	state.ccp = 0.0;
	state.aborted = false;

	/*
	 * DPccp enumeration: seed a connected subgraph at every vertex, working
	 * from the highest index downwards. Forbidding all lower-or-equal
	 * indexes when growing from the seed guarantees that each connected
	 * subgraph is enumerated exactly once.
	 */
	for (i = graph->n - 1; i >= 0 && !state.aborted; i--)
	{
		Bitmapset  *s = bms_make_singleton(i);
		Bitmapset  *x = NULL;
		int			j;

		on_connected_subgraph(&state, s);

		if (!state.aborted)
		{
			/*
			 * Disallow all lower-or-equal vertices (i.e. those won't be
			 * part of the subgraphs enumerated in enumerate_csg).
			 */
			for (j = 0; j <= i; j++)
				x = bms_add_member(x, j);

			enumerate_csg(&state, s, x);
		}

		bms_free(s);
		bms_free(x);
	}

	/*
	 * DPccp counts only the csg-cmp pairs reachable through join-graph edges,
	 * so it has not accounted for the clauseless Cartesian-product joins that
	 * standard_join_search() must perform to glue together the connected
	 * components of a disconnected graph.  Add that term on top of the
	 * per-component pair count (it is zero for a connected graph), keeping the
	 * total capped at the budget.  If the walk aborted, the work already
	 * exceeded the budget and the answer is simply the budget.
	 */
	if (state.aborted)
		result = budget;
	else
	{
		int			components = count_join_graph_components(graph);
		int64		cross = clauseless_combination_effort(components, budget);

		result = state.ccp + cross;
		if ((budget != 0) && (result > budget))
			result = budget;
	}

	free_join_graph(graph);

	ts_end = GetCurrentTimestamp();
	microsec = TimestampDifferenceMicroseconds(ts_start, ts_end);

	/*
	 * If aborted, return the budget (to show we aborted), otherwise return
	 * the number of ccp pairs.
	 */
	elog(WARNING, "estimate_join_search_effort: rels %d aborted %d "
				  "result " INT64_FORMAT " ccp " INT64_FORMAT
				  " budget " INT64_FORMAT " timing " INT64_FORMAT " us",
		 n, state.aborted, result, state.ccp, budget, microsec);

	return result;
}

/*
 * join_search_too_hard
 *	  Decide whether to this join search is too hard to solve exhaustively.
 *
 * The traditional rule simply compares the number of jointree items against
 * geqo_threshold.  That ignores the join graph's shape, even though the cost
 * of the dynamic-programming search depends strongly on it (an n-way chain is
 * cheap while an n-way clique or star explodes).
 *
 * When enable_join_search_estimate is set we instead estimate the actual
 * search effort (the number of make_join_rel() candidate pairs) and switch to
 * GEQO only when that estimate reaches join_search_effort_limit, so that large
 * but sparse queries keep the benefit of exhaustive planning while dense queries
 * are offloaded sooner.
 */
static bool
join_search_too_hard(PlannerInfo *root, int levels_needed,
					 List *initial_rels)
{
	if (join_hardness_enabled)
	{
		double		budget = (double) join_hardness_max_effort;
		double		effort;

		effort = estimate_join_search_effort(root, initial_rels, budget);
		return effort >= budget;
	}

	return false;
}

/*
 * subproblem_is_joinable
 *	Can the given list of rels be joined into a single relation on its own?
 *
 * When make_rel_from_joinlist() splits a large join problem into smaller
 * subproblems, each subproblem is solved independently with
 * standard_join_search().  That only works if the subproblem's relations can
 * actually be combined into a single join relation.  Inner joins place no
 * restriction on this, but an outer join does: its relations must be joined
 * in a particular order, so a relation set that includes one side of the
 * outer join together with only part of the other side cannot be joined by
 * itself.  Attempting to do so makes standard_join_search() fail with
 * "failed to build any N-way joins".
 *
 * Return true only if, for every outer/special join, the set of relids in
 * "rels" is "closed": if it touches both syntactic sides of the join it must
 * contain the whole join.  Sets that stay entirely on one side of a join (or
 * fully contain it) are fine.  In addition, every relation's LATERAL
 * dependencies must be satisfied within the set, since a laterally-dependent
 * relation likewise cannot be joined before the relations it references.
 */
static bool
subproblem_is_joinable(PlannerInfo *root, List *rels)
{
	Relids	  relids = NULL;
	ListCell   *lc;

	foreach(lc, rels)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(lc);

		relids = bms_add_members(relids, rel->relids);
	}

	/*
	* Every LATERAL reference of a relation in the set must point to another
	* relation in the set, otherwise the dependency cannot be satisfied while
	* joining the subproblem on its own.
	*/
	foreach(lc, rels)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(lc);

		if (!bms_is_subset(rel->lateral_relids, relids))
		{
			 bms_free(relids);
			 return false;
		}
	}

	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		Relids	  ojspan;

		/* full syntactic span of this join, including its own relid */
		ojspan = bms_union(sjinfo->syn_lefthand, sjinfo->syn_righthand);
		if (sjinfo->ojrelid != 0)
			 ojspan = bms_add_member(ojspan, sjinfo->ojrelid);

		/*
		 * If the candidate set touches both sides of this join but does not
		 * contain all of it, the join cannot be formed within the subproblem,
		 * and so the relations cannot be joined into a single relation.
		 */
		if (bms_overlap(relids, sjinfo->syn_lefthand) &&
			 bms_overlap(relids, sjinfo->syn_righthand) &&
			 !bms_is_subset(ojspan, relids))
		{
			 bms_free(ojspan);
			 bms_free(relids);
			 return false;
		}

		bms_free(ojspan);
	}

	bms_free(relids);
	return true;
}

static RelOptInfo *
join_search_hardness_hook(PlannerInfo *root, int levels_needed, List *initial_rels)
{
	/*
	 * With both the DPccp estimate and replanning enabled, see if this
	 * join is too complex and try to be split it into smaller problems.
	 * And then hand over the new join problem to standard_join_search
	 * or whatever.
	 */
	if (join_hardness_enabled)
	{
		/*
		 * Without splitting large joins based on effort, just calculate
		 * the estimate with no budget (so no aborts). Otherwise check if
		 * the join is expected to be too hard, and maybe split.
		 */
		if (!join_hardness_split)
		{
			/* calculate the estimate */
			estimate_join_search_effort(root, initial_rels, 0);
		}
		else if (join_search_too_hard(root, levels_needed, initial_rels))
		{
			List   *remaining_rels = NIL;

			/*
			 * Try removing rels from the join one by one, see if the rest is
			 * still joinable. Stop once we're below the limit, or when we
			 * fail to find a removable rel.
			 *
			 * XXX This is probably a bit too simple, and possibly also quite
			 * expensive if we're calculating the estimate over and over.
			 *
			 * First, it's not clear if any single rel is removable, because
			 * we may be doing something like (A join B) LEFT (C join D). So
			 * maybe we should be splitting the list into two parts, until we
			 * find a split that works. After all, that's inverse to how
			 * deconstruct_recurse() builds the joinlist, by appending to it
			 * (and hitting join_collapse_limit would split it like that too).
			 *
			 * Second, the estimate is quite cheap but not entirely free, so
			 * maybe it's not a good idea to call it too often. If we're to
			 * split the list, it seems best to split it about in half, which
			 * has the best chance of reducing the hardness enough?
			 *
			 * XXX Alternative idea: deconstruct the jointree as if there
			 * was join_collapse_limit=1, and now combine the subproblems,
			 * as long as the hardness is below the budget. But then we have
			 * to calculate the estimate over and over, which is not great.
			 * However, we'd need to keep track of which "splits" were
			 * forced by JOIN_FULL (and we can't undo those), and which were
			 * just due to limit=1. But we already know the relids for full
			 * joins, so that seems feasible?
			 *
			 * XXX If we knew about starjoin clusters (or other information
			 * that reduces the join search complexity), we would consider it
			 * here. E.g. we might prefer not to remove parts of the starjoin
			 * cluster, as that has very little impact on the complexity,
			 * if we enforce the canonical order). Or maybe we should postpone
			 * those joins instead? In any case, the starjoin cluster should
			 * not increase the hardness very much, so removing other rels has
			 * more chance to meet the budget.
			 */
			while (true)
			{
				List   *new_initial_rels = initial_rels;
				bool	removed = false;

				for (int i = 0; i < list_length(new_initial_rels); i++)
				{
					RelOptInfo *rel = list_nth(new_initial_rels, i);
					List *tmp_initial_rels = list_copy(new_initial_rels);

					Assert(IsA(rel, RelOptInfo));

					/* create a copy with the i-th rel removed */
					tmp_initial_rels = list_delete_nth_cell(tmp_initial_rels, i);

					root->initial_rels = tmp_initial_rels;
					if (!subproblem_is_joinable(root, tmp_initial_rels))
						continue;

					/* we have a new subproblem */
					new_initial_rels = tmp_initial_rels;
					remaining_rels = lappend(remaining_rels, rel);
					removed = true;

					/*
					 * is the problem simple enough now? small problems (with
					 * less than 3 tables) are automatically OK, for larger
					 * ones we re-calculate the difficulty
					 *
					 * XXX We should try to find a split that gets us the
					 * closest to the target difficulty, not the first problem
					 * that can be joined.
					 */
					if (list_length(new_initial_rels) > 3)
					{
						/* found a sufficiently simple subproblem, try it */
						if (!join_search_too_hard(root, list_length(new_initial_rels),
												  new_initial_rels))
						{
							RelOptInfo *joinrel;

							/* do the join search for the subproblem */
							root->initial_rels = new_initial_rels;
							joinrel = standard_join_search(root, list_length(new_initial_rels), new_initial_rels);

							Assert(IsA(joinrel, RelOptInfo));

							/* replace the problem with the reduced one */
							new_initial_rels = list_make1(joinrel);
							new_initial_rels = list_concat(new_initial_rels, remaining_rels);
							remaining_rels = NIL;

							break;
						}
					}
				}

				/* failed to find a removable rel */
				if (!removed)
				{
					/* we can't do better so plan with the current split */
					if (remaining_rels != NIL)
					{
						RelOptInfo *joinrel;

						/* do the join search for the subproblem */
						root->initial_rels = new_initial_rels;
						joinrel = standard_join_search(root, list_length(new_initial_rels), new_initial_rels);

						Assert(IsA(joinrel, RelOptInfo));

						/* replace the problem with the reduced one */
						new_initial_rels = list_make1(joinrel);
						new_initial_rels = list_concat(new_initial_rels, remaining_rels);
						remaining_rels = NIL;
					}

					break;
				}

				/* how difficult is the new problem */
				initial_rels = new_initial_rels;
				levels_needed = list_length(initial_rels);

				/*
				 * The remaining problem is sufficiently simple, we're done.
				 *
				 * XXX In some cases we've already called join_search_too_hard() on
				 * this join problem above, in which case we should not do that again.
				 */
				if (!join_search_too_hard(root, levels_needed, initial_rels))
					break;
			}
		}
	}

	if (prev_join_search_hook)
		return (*prev_join_search_hook) (root, levels_needed, initial_rels);
	else if (enable_geqo && levels_needed >= geqo_threshold)
		return geqo(root, levels_needed, initial_rels);
	else
		return standard_join_search(root, levels_needed, initial_rels);
}

/*
 * join_ordering_upper_bound
 *		Compute n! * C(n-1), saturated at "budget".
 *
 * This is the maximum number of binary join orderings for n base rels, where
 * C(k) is the k-th Catalan number.  The value is an upper bound independent of
 * join graph shape, so if it is below the caller's budget then no DPccp search
 * is needed to prove the budget cannot be reached.
 */
static int64
join_ordering_upper_bound(int n, int64 budget)
{
	int64		result = 1.0;
	int			i;

	/* disable fast-path (useful for testing the full estimation logic) */
	if (!join_hardness_fast)
		return budget;

	/* Compute n!, stopping once it reaches the only threshold we care about. */
	for (i = 2; i <= n; i++)
	{
		if ((budget != 0) && (result * i >= budget))
			return budget;

		result *= i;
	}

	/* Compute C(n-1) by recurrence: C_k = C_{k-1} * 2(2k-1)/(k+1). */
	for (i = 1; i < n; i++)
	{
		double		factor = 2.0 * (2.0 * (double) i - 1.0) /
			(double) (i + 1);

		if ((budget != 0) && (result >= budget / factor))
			return budget;

		result *= factor;
	}

	return result;
}

/*
 * build_join_graph
 *		Populate "graph" with one vertex per initial rel and an edge between
 *		every pair of rels that share a join clause, an EquivalenceClass join,
 *		or a join-order restriction.
 *
 * have_relevant_joinclause() already covers both ordinary join clauses and
 * EquivalenceClass-derived ones, so the resulting graph reflects every link
 * that standard_join_search() would actually use to combine two rels.
 */
static JoinGraph *
build_join_graph(PlannerInfo *root, List *initial_rels)
{
	JoinGraph *graph = palloc(sizeof(JoinGraph));
	int			n = list_length(initial_rels);
	RelOptInfo **rels = (RelOptInfo **) palloc(n * sizeof(RelOptInfo *));
	ListCell   *lc;
	int			i;
	int			j;

	i = 0;
	foreach(lc, initial_rels)
		rels[i++] = (RelOptInfo *) lfirst(lc);

	graph->n = n;
	graph->neighbors = (Bitmapset **) palloc0(n * sizeof(Bitmapset *));

	for (i = 0; i < n; i++)
	{
		for (j = i + 1; j < n; j++)
		{
			if (have_relevant_joinclause(root, rels[i], rels[j]) ||
				have_join_order_restriction(root, rels[i], rels[j]))
			{
				graph->neighbors[i] = bms_add_member(graph->neighbors[i], j);
				graph->neighbors[j] = bms_add_member(graph->neighbors[j], i);
			}
		}
	}

	pfree(rels);

	return graph;
}

/*
 * free_join_graph
 *		Release the adjacency storage built by build_join_graph().
 */
static void
free_join_graph(JoinGraph *graph)
{
	int			i;

	for (i = 0; i < graph->n; i++)
		bms_free(graph->neighbors[i]);

	pfree(graph->neighbors);
	pfree(graph);
}

/*
 * count_join_graph_components
 *		Return the number of connected components of the join graph.
 *
 * A connected graph has a single component; each additional component is a set
 * of rels that share no join clause or join-order restriction with the rest of
 * the query and can therefore only be combined with the others by a Cartesian
 * product.  We find the components with a simple flood fill.
 */
static int
count_join_graph_components(JoinGraph *graph)
{
	Bitmapset  *seen = NULL;
	int			components = 0;
	int			i;

	for (i = 0; i < graph->n; i++)
	{
		Bitmapset  *stack;
		int			v;

		if (bms_is_member(i, seen))
			continue;

		/* Start a new component and flood fill it from vertex i. */
		components++;
		seen = bms_add_member(seen, i);
		stack = bms_make_singleton(i);

		while ((v = bms_next_member(stack, -1)) >= 0)
		{
			int			w = -1;

			stack = bms_del_member(stack, v);
			while ((w = bms_next_member(graph->neighbors[v], w)) >= 0)
			{
				if (!bms_is_member(w, seen))
				{
					seen = bms_add_member(seen, w);
					stack = bms_add_member(stack, w);
				}
			}
		}

		bms_free(stack);
	}

	bms_free(seen);

	return components;
}

/*
 * clauseless_combination_effort
 *		Estimate the make_join_rel() pairs forced by combining "c" connected
 *		components purely through Cartesian products, saturated at "budget".
 *
 * standard_join_search() builds up each connected component using that
 * component's join clauses, but when the join graph splits into several
 * components it has to glue them together with clauseless (Cartesian-product)
 * joins.  Treating each fully-built component as an atomic unit, the
 * left-/right-sided clauseless joins that join_search_one_level() generates
 * amount to
 *
 *		sum_{L=2..c} C(c, L-1) * (c-L+1)  =  c * (2^(c-1) - 1)
 *
 * candidate pairs, where C(n, k) is the binomial coefficient.  (Bushy
 * clauseless joins are deliberately skipped by the join search, so this
 * left-/right-sided count is the relevant figure.)  DPccp never traverses
 * these cross-component edges, so the term is added on top of the
 * per-component #ccp; otherwise a disconnected join graph would be
 * mis-estimated as essentially free.  The result is zero for a connected graph
 * (c <= 1).  The accumulation saturates at "budget" so it cannot overflow.
 */
static int64
clauseless_combination_effort(int c, int64 budget)
{
	double		pow2 = 1.0;		/* will hold 2^(c-1) */
	int			i;

	if (c <= 1)
		return 0.0;

	for (i = 1; i <= c - 1; i++)
	{
		pow2 *= 2.0;
		if ((budget != 0) && ((double) c * (pow2 - 1.0) >= budget))
			return budget;
	}

	return (double) c * (pow2 - 1.0);
}

/*
 * neighborhood
 *		Return the open neighborhood of vertex set "s".
 *
 * The open neighborhood is the union of the adjacency lists of its members,
 * minus "s" itself and minus the forbidden set "x".
 *
 * The caller owns and must free the result.
 */
static Bitmapset *
neighborhood(JoinEnumState *state, Bitmapset *s, Bitmapset *x)
{
	Bitmapset  *nbr = NULL;
	int			v = -1;

	while ((v = bms_next_member(s, v)) >= 0)
		nbr = bms_add_members(nbr, state->graph->neighbors[v]);

	nbr = bms_del_members(nbr, s);
	if (x != NULL)
		nbr = bms_del_members(nbr, x);

	return nbr;
}

/*
 * on_connected_subgraph
 *		Account for one connected subgraph S1 and enumerate the csg-cmp pairs
 *		that use it as the "left" side.
 */
static void
on_connected_subgraph(JoinEnumState *state, Bitmapset *s1)
{
	state->work += 1;
	if ((state->budget != 0) && (state->work > state->budget))
	{
		state->aborted = true;
		return;
	}

	enumerate_cmp(state, s1);
}

/*
 * enumerate_csg
 *		Recursively enumerate the connected subgraphs that extend "s" using
 *		only vertices outside the forbidden set "x".
 */
static void
enumerate_csg(JoinEnumState *state, Bitmapset *s, Bitmapset *x)
{
	Bitmapset  *nbr;
	Bitmapset  *subset = NULL;
	int		   *members;
	int			k;
	int			v;
	int			i;

	if (state->aborted)
		return;

	nbr = neighborhood(state, s, x);
	k = bms_num_members(nbr);
	if (k == 0)
	{
		bms_free(nbr);
		return;
	}

	members = (int *) palloc(k * sizeof(int));
	v = -1;
	i = 0;
	while ((v = bms_next_member(nbr, v)) >= 0)
		members[i++] = v;

	enumerate_csg_subsets(state, s, x, nbr, members, k, 0, &subset);

	pfree(members);
	bms_free(nbr);
}

/*
 * enumerate_csg_subsets
 *		Enumerate every non-empty subset of the neighborhood "nbr" (given as
 *		the member array members[0..k)).  For each subset S', the union S u S'
 *		is a connected subgraph: account for it and recurse to grow it further,
 *		forbidding the whole neighborhood to avoid duplicate enumerations.
 *
 * "subset" accumulates the members chosen so far; it is mutated in place and
 * restored on the way back up so a single buffer is reused across the walk.
 */
static void
enumerate_csg_subsets(JoinEnumState *state, Bitmapset *s, Bitmapset *x,
					  Bitmapset *nbr, int *members, int k, int idx,
					  Bitmapset **subset)
{
	if (state->aborted)
		return;

	if (idx == k)
	{
		Bitmapset  *snew;
		Bitmapset  *xnew;

		if (*subset == NULL)
			return;				/* skip the empty subset */

		snew = bms_union(s, *subset);
		on_connected_subgraph(state, snew);
		if (!state->aborted)
		{
			xnew = bms_union(x, nbr);
			enumerate_csg(state, snew, xnew);
			bms_free(xnew);
		}
		bms_free(snew);

		return;
	}

	/* Branch that excludes members[idx]. */
	enumerate_csg_subsets(state, s, x, nbr, members, k, idx + 1, subset);
	if (state->aborted)
		return;

	/* Branch that includes members[idx]. */
	*subset = bms_add_member(*subset, members[idx]);
	enumerate_csg_subsets(state, s, x, nbr, members, k, idx + 1, subset);
	*subset = bms_del_member(*subset, members[idx]);
}

/*
 * enumerate_cmp
 *		Enumerate every connected complement S2 for the connected subgraph S1,
 *		counting one csg-cmp pair per (S1, S2).
 *
 * To count each unordered pair exactly once we forbid all vertices whose index
 * is <= min(S1); that forces the global minimum of S1 u S2 to lie in S1.  The
 * complements are seeded from each neighbor of S1 in decreasing index order,
 * forbidding the lower-indexed neighbors as we go so that a complement reached
 * through several entry points is only generated once.
 */
static void
enumerate_cmp(JoinEnumState *state, Bitmapset *s1)
{
	int			mn = bms_next_member(s1, -1);
	Bitmapset  *x;
	Bitmapset  *nbr;
	int		   *members;
	int			k;
	int			v;
	int			i;

	if (state->aborted)
		return;

	/* x = S1 u {0 .. mn} */
	x = bms_copy(s1);
	for (i = 0; i <= mn; i++)
		x = bms_add_member(x, i);

	nbr = neighborhood(state, s1, x);
	k = bms_num_members(nbr);
	if (k == 0)
	{
		bms_free(nbr);
		bms_free(x);
		return;
	}

	members = (int *) palloc(k * sizeof(int));
	v = -1;
	i = 0;
	while ((v = bms_next_member(nbr, v)) >= 0)
		members[i++] = v;

	/* Seed complements from the highest-index neighbor downwards. */
	for (i = k - 1; i >= 0 && !state->aborted; i--)
	{
		Bitmapset  *s2;
		Bitmapset  *xrec;
		int			j;

		/* Count the pair (S1, {members[i]}). */
		state->work += 1;
		state->ccp += 1;

		if ((state->budget != 0) && (state->work > state->budget))
		{
			state->aborted = true;
			break;
		}

		/* Forbid x plus every lower-indexed neighbor (members[0 .. i-1]). */
		xrec = bms_copy(x);
		for (j = 0; j < i; j++)
			xrec = bms_add_member(xrec, members[j]);

		s2 = bms_make_singleton(members[i]);
		grow_cmp(state, s1, s2, xrec);

		bms_free(s2);
		bms_free(xrec);
	}

	pfree(members);
	bms_free(nbr);
	bms_free(x);
}

/*
 * grow_cmp
 *		Grow the connected complement "s2" of "s1" by every non-empty subset of
 *		its neighborhood, counting one csg-cmp pair for each enlarged
 *		complement.
 */
static void
grow_cmp(JoinEnumState *st, Bitmapset *s1, Bitmapset *s2, Bitmapset *x)
{
	Bitmapset  *nbr;
	Bitmapset  *subset = NULL;
	int		   *members;
	int			k;
	int			v;
	int			i;

	if (st->aborted)
		return;

	nbr = neighborhood(st, s2, x);
	k = bms_num_members(nbr);
	if (k == 0)
	{
		bms_free(nbr);
		return;
	}

	members = (int *) palloc(k * sizeof(int));
	v = -1;
	i = 0;
	while ((v = bms_next_member(nbr, v)) >= 0)
		members[i++] = v;

	grow_cmp_subsets(st, s1, s2, x, nbr, members, k, 0, &subset);

	pfree(members);
	bms_free(nbr);
}

/*
 * grow_cmp_subsets
 *		Subset-enumeration helper for grow_cmp(), mirroring
 *		enumerate_csg_subsets() but counting csg-cmp pairs as it enlarges the
 *		complement.
 */
static void
grow_cmp_subsets(JoinEnumState *state, Bitmapset *s1, Bitmapset *s2, Bitmapset *x,
				 Bitmapset *nbr, int *members, int k, int idx,
				 Bitmapset **subset)
{
	if (state->aborted)
		return;

	if (idx == k)
	{
		Bitmapset  *s2new;
		Bitmapset  *xnew;

		if (*subset == NULL)
			return;				/* skip the empty subset */

		state->work += 1;
		state->ccp += 1;

		if ((state->budget != 0) && (state->work > state->budget))
		{
			state->aborted = true;
			return;
		}

		s2new = bms_union(s2, *subset);
		xnew = bms_union(x, nbr);
		grow_cmp(state, s1, s2new, xnew);
		bms_free(s2new);
		bms_free(xnew);

		return;
	}

	/* Branch that excludes members[idx]. */
	grow_cmp_subsets(state, s1, s2, x, nbr, members, k, idx + 1, subset);

	if (state->aborted)
		return;

	/* Branch that includes members[idx]. */
	*subset = bms_add_member(*subset, members[idx]);
	grow_cmp_subsets(state, s1, s2, x, nbr, members, k, idx + 1, subset);
	*subset = bms_del_member(*subset, members[idx]);
}
