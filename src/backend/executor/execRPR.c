/*-------------------------------------------------------------------------
 *
 * execRPR.c
 *	  NFA-based Row Pattern Recognition engine for window functions.
 *
 * This file implements the NFA execution engine for the ROWS BETWEEN
 * PATTERN clause (SQL Standard Feature R020: Row Pattern Recognition in
 * Window Functions).
 *
 * The engine executes the compiled RPRPattern structure directly, avoiding
 * regex compilation overhead.  It is called by nodeWindowAgg.c and exposes
 * the interface declared in executor/execRPR.h.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execRPR.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int.h"
#include "executor/execRPR.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/rpr.h"
#include "utils/memutils.h"

/*
 * For the design and execution model of the NFA engine implemented
 * in this file, see src/backend/executor/README.rpr.
 */

/* Bitmap macros for NFA cycle detection (cf. bitmapset.c, tidbitmap.c) */
#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

/*
 * Set the visited bit for elemIdx and update the high-water marks
 * (nfaVisitedMin/MaxWord) so that the next reset only has to clear
 * the touched range instead of the full nfaVisitedElems bitmap.
 */
static inline void
nfa_mark_visited(WindowAggState *winstate, int16 elemIdx)
{
	int16		w = WORDNUM(elemIdx);

	winstate->nfaVisitedElems[w] |= ((bitmapword) 1 << BITNUM(elemIdx));
	winstate->nfaVisitedMinWord = Min(winstate->nfaVisitedMinWord, w);
	winstate->nfaVisitedMaxWord = Max(winstate->nfaVisitedMaxWord, w);
}

/* Forward declarations */
static RPRNFAState *nfa_state_make(WindowAggState *winstate);
static void nfa_state_free(WindowAggState *winstate, RPRNFAState *state);
static void nfa_state_free_list(WindowAggState *winstate, RPRNFAState *list);
static RPRNFAState *nfa_state_clone(WindowAggState *winstate, int16 elemIdx,
									int32 *counts, bool sourceAbsorbable);
static bool nfa_states_equal(WindowAggState *winstate, RPRNFAState *s1,
							 RPRNFAState *s2);
static void nfa_add_state_unique(WindowAggState *winstate, RPRNFAContext *ctx,
								 RPRNFAState *state);
static void nfa_add_matched_state(WindowAggState *winstate, RPRNFAContext *ctx,
								  RPRNFAState *state, int64 matchEndRow);

static RPRNFAContext *nfa_context_make(WindowAggState *winstate);
static void nfa_unlink_context(WindowAggState *winstate, RPRNFAContext *ctx);

static void nfa_update_length_stats(int64 count, NFALengthStats *stats, int64 newLen);
static void nfa_record_context_skipped(WindowAggState *winstate, int64 skippedLen);
static void nfa_record_context_absorbed(WindowAggState *winstate, int64 absorbedLen);

static void nfa_update_absorption_flags(RPRNFAContext *ctx);
static bool nfa_states_covered(RPRPattern *pattern, RPRNFAContext *older,
							   RPRNFAContext *newer);
static void nfa_try_absorb_context(WindowAggState *winstate, RPRNFAContext *ctx);
static void nfa_absorb_contexts(WindowAggState *winstate);

static bool nfa_eval_var_match(WindowAggState *winstate,
							   RPRPatternElement *elem, bool *varMatched);
static void nfa_match(WindowAggState *winstate, RPRNFAContext *ctx,
					  bool *varMatched, int64 currentPos);
static void nfa_route_to_elem(WindowAggState *winstate, RPRNFAContext *ctx,
							  RPRNFAState *state, RPRPatternElement *nextElem,
							  int64 currentPos);
static void nfa_advance_alt(WindowAggState *winstate, RPRNFAContext *ctx,
							RPRNFAState *state, RPRPatternElement *elem,
							int64 currentPos);
static void nfa_advance_begin(WindowAggState *winstate, RPRNFAContext *ctx,
							  RPRNFAState *state, RPRPatternElement *elem,
							  int64 currentPos);
static void nfa_advance_end(WindowAggState *winstate, RPRNFAContext *ctx,
							RPRNFAState *state, RPRPatternElement *elem,
							int64 currentPos);
static void nfa_advance_var(WindowAggState *winstate, RPRNFAContext *ctx,
							RPRNFAState *state, RPRPatternElement *elem,
							int64 currentPos);
static void nfa_advance_state(WindowAggState *winstate, RPRNFAContext *ctx,
							  RPRNFAState *state, int64 currentPos);
static void nfa_advance(WindowAggState *winstate, RPRNFAContext *ctx,
						int64 currentPos);

static void nfa_reevaluate_dependent_vars(WindowAggState *winstate,
										  RPRNFAContext *ctx,
										  int64 currentPos);

/*
 * NFA-based pattern matching implementation
 *
 * These functions implement direct NFA execution using the compiled
 * RPRPattern structure, avoiding regex compilation overhead.
 *
 * Execution Flow: match -> absorb -> advance
 * -----------------------------------------
 * The NFA execution follows a three-phase cycle for each row:
 *
 * 1. MATCH (convergence): Evaluate all waiting states against current row.
 *    States on VAR elements are checked against their defining conditions.
 *    Failed matches are removed, successful ones may transition forward.
 *    This is a "convergence" phase - the number of states tends to decrease.
 *
 * 2. ABSORB: After matching, check if any context can absorb another.
 *    Context absorption is an optimization that merges equivalent contexts.
 *    A context can only be absorbed if ALL its states are absorbable.
 *
 * 3. ADVANCE (divergence): Expand states through epsilon transitions.
 *    States advance through ALT (alternation), END (group end), and
 *    optional elements until reaching VAR or FIN elements where they wait.
 *    This is a "divergence" phase - ALT creates multiple branch states.
 *
 * Key Design Decisions:
 * ---------------------
 * - VAR->END transition in match phase: When a simple VAR (max=1) matches
 *   and the next element is END, we transition immediately in the match
 *   phase rather than waiting for advance. This is necessary for correct
 *   absorption: states must be at END to be marked absorbable before the
 *   absorption check occurs.
 *
 * - Optional VAR skip paths: When advance lands on a VAR with min=0,
 *   we create both a waiting state AND a skip state (like ALT branches).
 *   This ensures patterns like "A B? C" work correctly - we need a state
 *   waiting for B AND a state that has already skipped to C.
 *
 * - END->END count increment: When transitioning from one END to another
 *   END within advance, we must increment the outer END's count. This
 *   handles nested groups like "((A|B)+)+" correctly - exiting the inner
 *   group counts as one iteration of the outer group.
 *
 * - Empty match handling: The initial advance uses currentPos =
 *   startPos - 1 (before any row is consumed). If FIN is reached via
 *   epsilon transitions alone, matchEndRow = startPos - 1 < matchStartRow.
 *   If matchedState is set (FIN was reached), this is an empty match
 *   (RF_EMPTY_MATCH); otherwise it is unmatched (RF_UNMATCHED).
 *   For reluctant min=0 patterns (A*?, A??), the skip path reaches
 *   FIN first and early termination prunes enter paths, yielding an
 *   immediate empty match result. For greedy patterns (A*), the enter
 *   path adds VAR states first, then the skip FIN is recorded but VAR
 *   states survive for later matching.
 *
 * Context Absorption Runtime:
 * ---------------------------
 * Absorption uses flags computed at planning time (in rpr.c) and two
 * context-level flags maintained at runtime:
 *
 * State-level:
 *   state.isAbsorbable: true if state is in the absorbable region.
 *     - Set at creation: elem->flags & RPR_ELEM_ABSORBABLE_BRANCH
 *     - At transition: prevAbsorbable && (newElem->flags & ABSORBABLE_BRANCH)
 *     - Monotonic: once false, stays false forever
 *
 * Context-level:
 *   ctx.hasAbsorbableState: can this context absorb others?
 *     - True if at least one state has isAbsorbable=true
 *     - Monotonic: true->false only (optimization: skip recalc when false)
 *
 *   ctx.allStatesAbsorbable: can this context be absorbed?
 *     - True if ALL states have isAbsorbable=true
 *     - Dynamic: can change false->true (when non-absorbable states die)
 *
 * Absorption Algorithm:
 *   For each pair (older Ctx1, newer Ctx2):
 *   1. Pre-check: Ctx1.hasAbsorbableState && Ctx2.allStatesAbsorbable
 *      -> If false, skip (fast filter)
 *   2. Coverage check: For each Ctx2 state with isAbsorbable=true,
 *      find Ctx1 state with same elemIdx and count >= Ctx2.count
 *   3. If all Ctx2 absorbable states are covered, absorb Ctx2
 *
 * Example: Pattern A+ B
 *   Row 1: Ctx1 at A (count=1)
 *   Row 2: Ctx1 at A (count=2), Ctx2 at A (count=1)
 *   -> Both at same elemIdx (A), Ctx1.count >= Ctx2.count
 *   -> Ctx2 absorbed
 *
 * The asymmetric design (Ctx1 needs hasAbsorbable, Ctx2 needs allAbsorbable)
 * allows absorption even when Ctx1 has extra non-absorbable states.
 */

/*
 * nfa_state_make
 *
 * Allocate an NFA state, reusing from freeList if available.
 * freeList is stored in WindowAggState for reuse across match attempts.
 * Uses flexible array member for counts[].
 */
static RPRNFAState *
nfa_state_make(WindowAggState *winstate)
{
	RPRNFAState *state;

	/* Try to reuse from free list first */
	if (winstate->nfaStateFree != NULL)
	{
		state = winstate->nfaStateFree;
		winstate->nfaStateFree = state->next;
	}
	else
	{
		/* Allocate in partition context for proper lifetime */
		state = MemoryContextAlloc(winstate->partcontext, winstate->nfaStateSize);
	}

	/* Initialize entire state to zero */
	memset(state, 0, winstate->nfaStateSize);

	/* Update statistics */
	winstate->nfaStatesActive++;
	winstate->nfaStatesTotalCreated++;
	winstate->nfaStatesMax = Max(winstate->nfaStatesMax,
								 winstate->nfaStatesActive);

	return state;
}

/*
 * nfa_state_free
 *
 * Return a state to the free list for later reuse.
 */
static void
nfa_state_free(WindowAggState *winstate, RPRNFAState *state)
{
	winstate->nfaStatesActive--;
#ifdef USE_VALGRIND
	/* real free so Valgrind catches use-after-free instead of recycling */
	pfree(state);
#else
	state->next = winstate->nfaStateFree;
	winstate->nfaStateFree = state;
#endif
}

/*
 * nfa_state_free_list
 *
 * Return all states in a list to the free list.
 */
static void
nfa_state_free_list(WindowAggState *winstate, RPRNFAState *list)
{
	RPRNFAState *next;

	for (; list != NULL; list = next)
	{
		next = list->next;
		nfa_state_free(winstate, list);
	}
}

/*
 * nfa_state_clone
 *
 * Clone a state from the given elemIdx and counts.
 * isAbsorbable is computed immediately: inherited AND new element's flag.
 * Monotonic property: once false, stays false through all transitions.
 *
 * Caller is responsible for linking the returned state.
 */
static RPRNFAState *
nfa_state_clone(WindowAggState *winstate, int16 elemIdx,
				int32 *counts, bool sourceAbsorbable)
{
	RPRPattern *pattern = winstate->rpPattern;
	int			maxDepth = pattern->maxDepth;
	RPRNFAState *state = nfa_state_make(winstate);
	RPRPatternElement *elem = &pattern->elements[elemIdx];

	state->elemIdx = elemIdx;
	/* Every reachable caller passes a live state's counts; maxDepth >= 1. */
	Assert(counts != NULL && maxDepth > 0);
	memcpy(state->counts, counts, sizeof(int32) * maxDepth);

	/*
	 * Compute isAbsorbable immediately at transition time. isAbsorbable =
	 * sourceAbsorbable && (elem->flags & ABSORBABLE_BRANCH) Monotonic: once
	 * false, stays false (can't re-enter absorbable region).
	 */
	state->isAbsorbable = sourceAbsorbable && RPRElemIsAbsorbableBranch(elem);

	return state;
}

/*
 * nfa_states_equal
 *
 * Check if two states are equivalent (same elemIdx and counts).
 */
static bool
nfa_states_equal(WindowAggState *winstate, RPRNFAState *s1, RPRNFAState *s2)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elem;
	int			compareDepth;

	if (s1->elemIdx != s2->elemIdx)
		return false;

	/*
	 * Compare counts up to current element's depth.  Two states sharing
	 * elemIdx are equivalent iff every enclosing-or-current depth count
	 * matches.
	 *
	 * The +1 is the slot arithmetic: comparing through depth N requires
	 * counts[0..N], i.e., N+1 entries.  Deeper slots (counts[d] with d >
	 * elem->depth) are excluded because they hold scratch state from inner
	 * groups.  Per the count-clear policy such a slot is zeroed when its
	 * owning element exits (see nfa_advance_var and the inline fast path in
	 * nfa_match), so it must not participate in equivalence judgment.
	 */
	elem = &pattern->elements[s1->elemIdx];
	compareDepth = elem->depth + 1;

	if (memcmp(s1->counts, s2->counts, sizeof(int32) * compareDepth) != 0)
		return false;

	return true;
}

/*
 * nfa_add_state_unique
 *
 * Add the state to the end of the ctx->states linked list, but only if a
 * duplicate state is not already present.
 * Earlier states have better lexical order (DFS traversal order), so existing
 * wins; the new state is freed when a duplicate is found.
 */
static void
nfa_add_state_unique(WindowAggState *winstate, RPRNFAContext *ctx, RPRNFAState *state)
{
	RPRNFAState *s;
	RPRNFAState *tail = NULL;

	/*
	 * Mark VAR in visited before duplicate check to prevent DFS loops. This
	 * is the deferred half of the asymmetric visited-marking scheme; see
	 * nfa_advance_state for the non-VAR (END/ALT/BEGIN/FIN) half and the
	 * rationale for the asymmetry.
	 */
	nfa_mark_visited(winstate, state->elemIdx);

	/* Check for duplicate and find tail */
	for (s = ctx->states; s != NULL; s = s->next)
	{
		CHECK_FOR_INTERRUPTS();

		if (nfa_states_equal(winstate, s, state))
		{
			/*
			 * Duplicate found - existing has better lexical order, discard
			 * new
			 */
			nfa_state_free(winstate, state);
			winstate->nfaStatesMerged++;
			return;
		}
		tail = s;
	}

	/* No duplicate, add at end */
	state->next = NULL;
	if (tail == NULL)
		ctx->states = state;
	else
		tail->next = state;
}

/*
 * nfa_add_matched_state
 *
 * Record a state that reached FIN, replacing any previous match.
 *
 * For SKIP PAST LAST ROW, also prune subsequent contexts whose start row
 * falls within the match range, as they cannot produce output rows.
 */
static void
nfa_add_matched_state(WindowAggState *winstate, RPRNFAContext *ctx,
					  RPRNFAState *state, int64 matchEndRow)
{
	if (ctx->matchedState != NULL)
		nfa_state_free(winstate, ctx->matchedState);

	ctx->matchedState = state;
	state->next = NULL;
	ctx->matchEndRow = matchEndRow;

	/* Prune contexts that started within this match's range */
	if (winstate->rpSkipTo == ST_PAST_LAST_ROW)
	{
		int64		skippedLen;

		while (ctx->next != NULL &&
			   ctx->next->matchStartRow <= matchEndRow)
		{
			RPRNFAContext *nextCtx = ctx->next;

			Assert(nextCtx->lastProcessedRow >= nextCtx->matchStartRow);
			skippedLen = nextCtx->lastProcessedRow - nextCtx->matchStartRow + 1;
			nfa_record_context_skipped(winstate, skippedLen);

			ExecRPRFreeContext(winstate, nextCtx);
		}
	}
}

/*
 * nfa_context_make
 *
 * Allocate an NFA context, reusing from free list if available.
 */
static RPRNFAContext *
nfa_context_make(WindowAggState *winstate)
{
	RPRNFAContext *ctx;

	if (winstate->nfaContextFree != NULL)
	{
		ctx = winstate->nfaContextFree;
		winstate->nfaContextFree = ctx->next;
	}
	else
	{
		/* Allocate in partition context for proper lifetime */
		ctx = MemoryContextAlloc(winstate->partcontext, sizeof(RPRNFAContext));
	}

	ctx->next = NULL;
	ctx->prev = NULL;
	ctx->states = NULL;
	ctx->matchStartRow = -1;
	ctx->matchEndRow = -1;
	ctx->lastProcessedRow = -1;
	ctx->matchedState = NULL;

	/* Initialize two-flag absorption design based on pattern */
	ctx->hasAbsorbableState = winstate->rpPattern->isAbsorbable;
	ctx->allStatesAbsorbable = winstate->rpPattern->isAbsorbable;

	/* Update statistics */
	winstate->nfaContextsActive++;
	winstate->nfaContextsTotalCreated++;
	winstate->nfaContextsMax = Max(winstate->nfaContextsMax,
								   winstate->nfaContextsActive);

	return ctx;
}

/*
 * nfa_unlink_context
 *
 * Remove a context from the doubly-linked active context list.
 * Updates head (nfaContext) and tail (nfaContextTail) as needed.
 */
static void
nfa_unlink_context(WindowAggState *winstate, RPRNFAContext *ctx)
{
	if (ctx->prev != NULL)
		ctx->prev->next = ctx->next;
	else
		winstate->nfaContext = ctx->next;	/* was head */

	if (ctx->next != NULL)
		ctx->next->prev = ctx->prev;
	else
		winstate->nfaContextTail = ctx->prev;	/* was tail */

	ctx->next = NULL;
	ctx->prev = NULL;
}

/*
 * nfa_update_length_stats
 *
 * Helper function to update min/max/total length statistics.
 * Called when tracking match/mismatch/absorbed/skipped lengths.
 */
static void
nfa_update_length_stats(int64 count, NFALengthStats *stats, int64 newLen)
{
	if (count == 1)
	{
		stats->min = newLen;
		stats->max = newLen;
	}
	else
	{
		stats->min = Min(stats->min, newLen);
		stats->max = Max(stats->max, newLen);
	}
	stats->total += newLen;
}

/*
 * nfa_record_context_skipped
 *
 * Record a skipped context in statistics.
 */
static void
nfa_record_context_skipped(WindowAggState *winstate, int64 skippedLen)
{
	winstate->nfaContextsSkipped++;
	nfa_update_length_stats(winstate->nfaContextsSkipped,
							&winstate->nfaSkippedLen,
							skippedLen);
}

/*
 * nfa_record_context_absorbed
 *
 * Record an absorbed context in statistics.
 */
static void
nfa_record_context_absorbed(WindowAggState *winstate, int64 absorbedLen)
{
	winstate->nfaContextsAbsorbed++;
	nfa_update_length_stats(winstate->nfaContextsAbsorbed,
							&winstate->nfaAbsorbedLen,
							absorbedLen);
}

/*
 * nfa_update_absorption_flags
 *
 * Update context's absorption flags after state changes.
 *
 * Two flags control absorption behavior:
 *   hasAbsorbableState: true if context has at least one absorbable state.
 *     This flag is monotonic (true -> false only). Once all absorbable states
 *     die, no new absorbable states can be created through transitions.
 *   allStatesAbsorbable: true if ALL states in context are absorbable.
 *     This flag is dynamic and can change false -> true when non-absorbable
 *     states die off.
 *
 * Optimization: Once hasAbsorbableState becomes false, both flags remain false
 * permanently, so we skip recalculation.
 */
static void
nfa_update_absorption_flags(RPRNFAContext *ctx)
{
	RPRNFAState *state;
	bool		hasAbsorbable = false;
	bool		allAbsorbable = true;

	/*
	 * Optimization: Once hasAbsorbableState becomes false, it stays false. No
	 * need to recalculate - both flags remain false permanently.
	 */
	if (!ctx->hasAbsorbableState)
	{
		ctx->allStatesAbsorbable = false;
		return;
	}

	/* No states means no absorbable states */
	if (ctx->states == NULL)
	{
		ctx->hasAbsorbableState = false;
		ctx->allStatesAbsorbable = false;
		return;
	}

	/*
	 * Iterate through all states to check absorption status. Uses
	 * state->isAbsorbable which tracks if state is in absorbable region. This
	 * is different from RPRElemIsAbsorbable(elem) which checks comparison
	 * point.
	 */
	for (state = ctx->states; state != NULL; state = state->next)
	{
		CHECK_FOR_INTERRUPTS();

		if (state->isAbsorbable)
			hasAbsorbable = true;
		else
			allAbsorbable = false;
	}

	/*
	 * A recorded match makes this context non-absorbable: absorption would
	 * free the match, which no absorbing context can reproduce.
	 */
	if (ctx->matchedState != NULL)
		allAbsorbable = false;

	ctx->hasAbsorbableState = hasAbsorbable;
	ctx->allStatesAbsorbable = allAbsorbable;
}

/*
 * nfa_states_covered
 *
 * Check if all states in newer context are "covered" by older context.
 *
 * A newer state is covered when older context has an absorbable state at the
 * same pattern element (elemIdx) with count >= newer's count at that depth.
 * The covering state must be absorbable because only absorbable states can
 * guarantee to produce superset matches.
 *
 * If all newer states are covered, newer context's eventual matches will be
 * a subset of older context's matches, making newer redundant.
 */
static bool
nfa_states_covered(RPRPattern *pattern, RPRNFAContext *older, RPRNFAContext *newer)
{
	RPRNFAState *newerState;

	for (newerState = newer->states; newerState != NULL; newerState = newerState->next)
	{
		RPRNFAState *olderState;
		RPRPatternElement *elem;
		int			depth;
		bool		found = false;

		/* All states are absorbable (caller checks allStatesAbsorbable) */
		elem = &pattern->elements[newerState->elemIdx];
		depth = elem->depth;

		/*
		 * Only compare at absorption comparison points (RPR_ELEM_ABSORBABLE).
		 * Comparison points are where count-dominance guarantees the newer
		 * context's future matches are a subset of the older's.
		 */
		if (!RPRElemIsAbsorbable(elem))
			return false;

		for (olderState = older->states; olderState != NULL; olderState = olderState->next)
		{
			CHECK_FOR_INTERRUPTS();

			/* Covering state must also be absorbable */
			if (olderState->isAbsorbable &&
				olderState->elemIdx == newerState->elemIdx &&
				olderState->counts[depth] >= newerState->counts[depth])
			{
				found = true;
				break;
			}
		}

		if (!found)
			return false;
	}

	return true;
}

/*
 * nfa_try_absorb_context
 *
 * Try to absorb ctx (newer) into an older in-progress context.
 * Returns true if ctx was absorbed and freed.
 *
 * Absorption requires three conditions:
 *   1. ctx must have all states absorbable (allStatesAbsorbable).
 *      If ctx has any non-absorbable state, it may produce unique matches.
 *   2. older must have at least one absorbable state (hasAbsorbableState).
 *      Without absorbable states, older cannot cover newer's states.
 *   3. All ctx states must be covered by older's absorbable states.
 *      This ensures older will produce all matches that ctx would produce.
 *
 * Context list is ordered by creation time (oldest first via prev chain).
 * Each row creates at most one context, so earlier contexts have smaller
 * matchStartRow values.
 */
static void
nfa_try_absorb_context(WindowAggState *winstate, RPRNFAContext *ctx)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRNFAContext *older;

	/* Early exit: ctx must have all states absorbable */
	if (!ctx->allStatesAbsorbable)
		return;

	for (older = ctx->prev; older != NULL; older = older->prev)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * By invariant: ctx->prev chain is in creation order (oldest first),
		 * and each row creates at most one context. So all contexts in this
		 * chain have matchStartRow < ctx->matchStartRow.
		 */

		/* Older must also be in-progress */
		if (older->states == NULL)
			continue;

		/* Older must have at least one absorbable state */
		if (!older->hasAbsorbableState)
			continue;

		/* Check if all newer states are covered by older */
		if (nfa_states_covered(pattern, older, ctx))
		{
			int64		absorbedLen = ctx->lastProcessedRow - ctx->matchStartRow + 1;

			ExecRPRFreeContext(winstate, ctx);
			nfa_record_context_absorbed(winstate, absorbedLen);
			return;
		}
	}
}

/*
 * nfa_absorb_contexts
 *
 * Absorb redundant contexts to reduce memory usage and computation.
 *
 * For patterns like A+, newer contexts starting later will produce subset
 * matches of older contexts with higher counts. By absorbing these redundant
 * contexts early, we avoid duplicate work.
 *
 * Iterates from tail (newest) toward head (oldest) via prev chain.
 * Only in-progress contexts (states != NULL) are candidates for absorption;
 * completed contexts represent valid match results.
 */
static void
nfa_absorb_contexts(WindowAggState *winstate)
{
	RPRNFAContext *ctx;
	RPRNFAContext *nextCtx;

	for (ctx = winstate->nfaContextTail; ctx != NULL; ctx = nextCtx)
	{
		nextCtx = ctx->prev;

		/*
		 * Only absorb in-progress contexts; completed contexts are valid
		 * results
		 */
		if (ctx->states != NULL)
			nfa_try_absorb_context(winstate, ctx);
	}
}

/*
 * nfa_eval_var_match
 *
 * Evaluate if a VAR element matches the current row.
 *
 * varMatched is a pre-evaluated boolean array indexed by varId, computed
 * once per row by evaluating all DEFINE expressions.  A NULL varMatched makes
 * every VAR not match; nfa_match() is called that way to force a mismatch at a
 * frame boundary and at partition-end finalization.
 *
 * Per ISO/IEC 19075-5 Feature R020, pattern variables not listed in DEFINE
 * are implicitly TRUE -- they match every row.  This is checked via
 * varId >= list_length.
 */
static bool
nfa_eval_var_match(WindowAggState *winstate, RPRPatternElement *elem,
				   bool *varMatched)
{
	/* This function should only be called for VAR elements */
	Assert(RPRElemIsVar(elem));

	if (varMatched == NULL)
		return false;
	if (elem->varId >= list_length(winstate->defineVariableList))
		return true;
	return varMatched[elem->varId];
}

/*
 * nfa_match
 *
 * Match phase (convergence): evaluate VAR elements against current row.
 * Only updates counts and removes dead states. Minimal transitions.
 *
 * For VAR elements:
 *   - matched: count++, keep state (unless count > max)
 *   - not matched: remove state (exit alternatives already exist from
 *     previous advance when count >= min was satisfied)
 *
 * For VARs that reached max count followed by END:
 *   - Advance through the END-element chain to the absorption
 *     comparison point
 *   - Only deterministic exits (count >= max, max != INF) are handled
 *   - Chains through END elements while count >= max (must-exit path)
 *
 * Non-VAR elements (ALT, END, FIN) are kept as-is for advance phase.
 *
 * currentPos is threaded in only for debugging visibility (nfa_match is the
 * one NFA helper that otherwise lacks the row index); it has no runtime
 * consumer yet.
 */
static void
nfa_match(WindowAggState *winstate, RPRNFAContext *ctx, bool *varMatched,
		  int64 currentPos)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elements = pattern->elements;
	RPRNFAState **prevPtr = &ctx->states;
	RPRNFAState *state;
	RPRNFAState *nextState;

	/* Evaluate VAR elements against current row. */
	for (state = ctx->states; state != NULL; state = nextState)
	{
		RPRPatternElement *elem = &elements[state->elemIdx];

		CHECK_FOR_INTERRUPTS();

		nextState = state->next;

		if (RPRElemIsVar(elem))
		{
			bool		matched;
			int			depth = elem->depth;
			int32		count = state->counts[depth];

			matched = nfa_eval_var_match(winstate, elem, varMatched);

			if (matched)
			{
				/*
				 * Increment count, saturating at RPR_COUNT_INF to avoid int32
				 * overflow; a saturated count then compares as "unbounded".
				 */
				if (count < RPR_COUNT_INF)
					count++;

				/* Max constraint should not be exceeded */
				Assert(elem->max == RPR_QUANTITY_INF || count <= elem->max);

				state->counts[depth] = count;

				/*
				 * For VAR at max count with END next, advance through END
				 * chain to reach the absorption comparison point.  Only
				 * deterministic exits (count >= max, max finite) are handled;
				 * unbounded VARs stay for advance phase.
				 *
				 * In nested patterns like ((A B){2}){3}, a VAR reaching its
				 * max triggers an exit cascade: inner END increments inner
				 * group count, which may itself reach max, requiring an exit
				 * to the next outer END.  The loop below walks this chain.
				 *
				 * ABSORBABLE_BRANCH marks elements inside the absorbable
				 * region; ABSORBABLE marks the outermost comparison point
				 * where count-dominance is evaluated.  We chain through
				 * BRANCH elements until reaching the ABSORBABLE point or an
				 * element that can still loop (count < max).
				 */
				if (RPRElemIsAbsorbableBranch(elem) &&
					!RPRElemIsAbsorbable(elem) &&
					count >= elem->max &&
					RPRElemIsEnd(&elements[elem->next]))
				{
					RPRPatternElement *endElem = &elements[elem->next];
					int			endDepth = endElem->depth;
					int32		endCount = state->counts[endDepth];

					/* Increment group count */
					if (endCount < RPR_COUNT_INF)
						endCount++;
					Assert(endElem->max == RPR_QUANTITY_INF ||
						   endCount <= endElem->max);

					state->elemIdx = elem->next;
					state->counts[endDepth] = endCount;

					/*
					 * Leaf VAR exited (reached max): clear its own count so
					 * the next occupant enters with zero, as nfa_advance_var
					 * does on exit (this inline path replaces that exit).
					 * depth > endDepth, so this leaves the group count just
					 * written intact.
					 */
					Assert(endDepth < depth);
					state->counts[depth] = 0;

					/*
					 * Chain through END elements within the absorbable region
					 * (ABSORBABLE_BRANCH) until reaching the comparison point
					 * (ABSORBABLE).  Continue only on must-exit path (count
					 * >= max) with END next.
					 */
					while (RPRElemIsAbsorbableBranch(endElem) &&
						   !RPRElemIsAbsorbable(endElem) &&
						   endCount >= endElem->max &&
						   RPRElemIsEnd(&elements[endElem->next]))
					{
						RPRPatternElement *outerEnd = &elements[endElem->next];
						int			outerDepth = outerEnd->depth;
						int32		outerCount = state->counts[outerDepth];

						/*
						 * Exit this intermediate group: clear its own count
						 * (count-clear policy).  It sits below the absorbable
						 * comparison point, so it is excluded from the
						 * dominance comparison; the comparison point where
						 * the chain stops keeps its count.
						 */
						state->counts[endDepth] = 0;

						/* Increment outer group count */
						if (outerCount < RPR_COUNT_INF)
							outerCount++;
						Assert(outerEnd->max == RPR_QUANTITY_INF ||
							   outerCount <= outerEnd->max);

						state->elemIdx = endElem->next;
						state->counts[outerDepth] = outerCount;

						/* Advance to next END in chain */
						endElem = outerEnd;
						endDepth = outerDepth;
						endCount = outerCount;
					}
				}
				/* else: stay at VAR for advance phase */
			}
			else
			{
				/*
				 * Not matched - remove state. Exit alternatives were already
				 * created by advance phase when count >= min was satisfied.
				 */
				*prevPtr = nextState;
				nfa_state_free(winstate, state);
				continue;
			}
		}
		/* Non-VAR elements: keep as-is for advance phase */

		prevPtr = &state->next;
	}
}

/*
 * nfa_route_to_elem
 *
 * Route state to next element. If VAR, add to ctx->states and process
 * skip path if optional. Otherwise, continue epsilon expansion via recursion.
 */
static void
nfa_route_to_elem(WindowAggState *winstate, RPRNFAContext *ctx,
				  RPRNFAState *state, RPRPatternElement *nextElem,
				  int64 currentPos)
{
	if (RPRElemIsVar(nextElem))
	{
		RPRNFAState *skipState = NULL;

		/*
		 * Entry-side check of the count-clear policy: a VAR is always routed
		 * to with a clean slot.  Each element zeroes its own count on exit,
		 * so a nonzero count here would be a leak from an earlier element
		 * (see nfa_advance_var / nfa_advance_end exit handling and the inline
		 * fast path in nfa_match).
		 */
		Assert(state->counts[nextElem->depth] == 0);

		/* Create skip state before add_unique, which may free state */
		if (RPRElemCanSkip(nextElem))
			skipState = nfa_state_clone(winstate, nextElem->next,
										state->counts, state->isAbsorbable);

		if (skipState != NULL && RPRElemIsReluctant(nextElem))
		{
			RPRNFAState *savedMatch = ctx->matchedState;

			/*
			 * Reluctant optional VAR: prefer skipping.  Explore the skip path
			 * first so it outranks the enter (match) path; if it reaches FIN
			 * the shortest match is found and the enter state is dropped.
			 * This mirrors the reluctant branch of nfa_advance_begin used by
			 * the leading-position and optional-group paths.
			 */
			nfa_advance_state(winstate, ctx, skipState, currentPos);

			if (ctx->matchedState != savedMatch)
			{
				nfa_state_free(winstate, state);
				return;
			}

			nfa_add_state_unique(winstate, ctx, state);
		}
		else
		{
			/* Greedy (or non-skippable): enter first, then skip */
			nfa_add_state_unique(winstate, ctx, state);

			if (skipState != NULL)
				nfa_advance_state(winstate, ctx, skipState, currentPos);
		}
	}
	else
	{
		nfa_advance_state(winstate, ctx, state, currentPos);
	}
}

/*
 * nfa_advance_alt
 *
 * Handle ALT element: expand all branches in lexical order via DFS.
 */
static void
nfa_advance_alt(WindowAggState *winstate, RPRNFAContext *ctx,
				RPRNFAState *state, RPRPatternElement *elem,
				int64 currentPos)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elements = pattern->elements;
	RPRElemIdx	altIdx = elem->next;

	while (altIdx >= 0)
	{
		RPRPatternElement *altElem;
		RPRNFAState *newState;

		/* Branch jump/next links are always -1 or a valid index */
		Assert(altIdx < pattern->numElements);
		altElem = &elements[altIdx];

		/*
		 * Stop if element is outside ALT scope (not a branch).  The check
		 * fires when the last branch is a quantified group whose BEGIN.jump
		 * (set by fillRPRPatternGroup) is preserved -- not overridden by
		 * fillRPRPatternAlt, which only links non-last branch heads -- and
		 * leads to a post-ALT element.  Other branch shapes terminate the
		 * walk earlier via altIdx = RPR_ELEMIDX_INVALID.  Use <=, not <: the
		 * post-ALT element may sit at the same depth as the ALT when the ALT
		 * has a sibling at that level.
		 */
		if (altElem->depth <= elem->depth)
			break;

		/* Create independent state for each branch */
		newState = nfa_state_clone(winstate, altIdx,
								   state->counts, state->isAbsorbable);

		/* Recursively process this branch before next */
		nfa_advance_state(winstate, ctx, newState, currentPos);
		altIdx = altElem->jump;
	}

	nfa_state_free(winstate, state);
}

/*
 * nfa_advance_begin
 *
 * Handle BEGIN element: group entry logic.
 * BEGIN is only visited at initial group entry; loop-back from END goes
 * directly to first child, bypassing BEGIN.  Per the count-clear policy the
 * group's own count slot is therefore already zero on entry (asserted below).
 * If min=0, creates a skip path past the group.
 */
static void
nfa_advance_begin(WindowAggState *winstate, RPRNFAContext *ctx,
				  RPRNFAState *state, RPRPatternElement *elem,
				  int64 currentPos)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elements = pattern->elements;
	RPRNFAState *skipState = NULL;

	/*
	 * Entry-side check of the count-clear policy: the group's own count slot
	 * is already zero here.  BEGIN is only visited at initial group entry,
	 * and the previous occupant of this depth slot cleared it on exit.
	 */
	Assert(state->counts[elem->depth] == 0);

	/* Optional group: create skip path (but don't route yet) */
	if (elem->min == 0)
	{
		skipState = nfa_state_clone(winstate, elem->jump,
									state->counts, state->isAbsorbable);
	}

	if (skipState != NULL && RPRElemIsReluctant(elem))
	{
		RPRNFAState *savedMatch = ctx->matchedState;

		/* Reluctant: skip first (prefer fewer iterations), enter second */
		nfa_route_to_elem(winstate, ctx, skipState,
						  &elements[elem->jump], currentPos);

		/*
		 * If skip path reached FIN, shortest match is found. Skip group entry
		 * to prevent longer matches.
		 */
		if (ctx->matchedState != savedMatch)
		{
			nfa_state_free(winstate, state);
			return;
		}

		state->elemIdx = elem->next;
		nfa_route_to_elem(winstate, ctx, state,
						  &elements[state->elemIdx], currentPos);
	}
	else
	{
		/*
		 * Greedy-or-non-nullable: route to the first child.  For optional
		 * groups (skipState != NULL, greedy min=0) additionally create the
		 * skip path; for non-nullable groups (skipState == NULL, min>0) the
		 * skip-path action is suppressed by the guard below.
		 */
		state->elemIdx = elem->next;
		nfa_route_to_elem(winstate, ctx, state,
						  &elements[state->elemIdx], currentPos);

		if (skipState != NULL)
		{
			nfa_route_to_elem(winstate, ctx, skipState,
							  &elements[elem->jump], currentPos);
		}
	}
}

/*
 * nfa_advance_end
 *
 * Handle END element: group repetition logic.
 * Decides whether to loop back or exit based on count vs min/max.
 */
static void
nfa_advance_end(WindowAggState *winstate, RPRNFAContext *ctx,
				RPRNFAState *state, RPRPatternElement *elem,
				int64 currentPos)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elements = pattern->elements;
	int			depth = elem->depth;
	int32		count = state->counts[depth];

	if (count < elem->min)
	{
		RPRPatternElement *jumpElem;
		RPRNFAState *ffState = NULL;
		RPRPatternElement *nextElem = NULL;

		/*----------
		 * Two paths are explored when the group body is nullable
		 * (RPR_ELEM_EMPTY_LOOP):
		 *
		 * 1. Loop-back path: attempt real matches in the next iteration
		 *    (state, modified below).
		 *
		 * 2. Fast-forward path: skip directly to after the group, treating
		 *    all remaining required iterations as empty matches (ffState).
		 *    Route to elem->next (not nfa_advance_end) to avoid creating
		 *    competing greedy/reluctant loop states.
		 *
		 * Greedy prefers the loop-back first (more iterations); reluctant
		 * prefers the fast-forward (exit) first and, if it reaches FIN, drops
		 * the loop-back so a longer match cannot replace the shortest one --
		 * mirroring the min<=count<max branch below.  The ffState snapshot is
		 * taken BEFORE modifying state, since both paths diverge from here.
		 *----------
		 */
		if (RPRElemCanEmptyLoop(elem))
		{
			ffState = nfa_state_clone(winstate, state->elemIdx,
									  state->counts, state->isAbsorbable);

			/* Exit the group: clear its own count (count-clear policy) */
			ffState->counts[depth] = 0;
			ffState->elemIdx = elem->next;
			nextElem = &elements[ffState->elemIdx];

			/*
			 * Unlike the must-exit path, no isAbsorbable update is needed:
			 * the fast-forward path runs only for EMPTY_LOOP (nullable)
			 * groups, which are never inside an absorbable region, so
			 * isAbsorbable is already false here.
			 */

			/* END->END: increment outer END's count */
			if (RPRElemIsEnd(nextElem) &&
				ffState->counts[nextElem->depth] < RPR_COUNT_INF)
				ffState->counts[nextElem->depth]++;
		}

		/* Prepare the loop-back state */
		state->elemIdx = elem->jump;
		jumpElem = &elements[state->elemIdx];

		if (ffState != NULL && RPRElemIsReluctant(elem))
		{
			RPRNFAState *savedMatch = ctx->matchedState;

			/* Reluctant: take the fast-forward (exit) first */
			nfa_route_to_elem(winstate, ctx, ffState, nextElem,
							  currentPos);

			/*
			 * If the exit reached FIN, the shortest match is found.  Skip the
			 * loop-back to prevent longer matches from replacing it.
			 */
			if (ctx->matchedState != savedMatch)
			{
				nfa_state_free(winstate, state);
				return;
			}

			/* Loop-back second */
			nfa_route_to_elem(winstate, ctx, state, jumpElem,
							  currentPos);
		}
		else
		{
			/* Greedy (or non-nullable): loop-back first, fast-forward second */
			nfa_route_to_elem(winstate, ctx, state, jumpElem,
							  currentPos);
			if (ffState != NULL)
				nfa_route_to_elem(winstate, ctx, ffState, nextElem,
								  currentPos);
		}
	}
	else if (elem->max != RPR_QUANTITY_INF && count >= elem->max)
	{
		/* Must exit: reached max iterations. */
		RPRPatternElement *nextElem;

		/* Exit: clear the group's own count (count-clear policy) */
		state->counts[depth] = 0;
		state->elemIdx = elem->next;
		nextElem = &elements[state->elemIdx];

		/* Update isAbsorbable for target element (monotonic) */
		state->isAbsorbable = state->isAbsorbable &&
			RPRElemIsAbsorbableBranch(nextElem);

		/* END->END: increment outer END's count */
		if (RPRElemIsEnd(nextElem) && state->counts[nextElem->depth] < RPR_COUNT_INF)
			state->counts[nextElem->depth]++;

		nfa_route_to_elem(winstate, ctx, state, nextElem, currentPos);
	}
	else
	{
		/*
		 * Between min and max (with at least one iteration) - can exit or
		 * loop. Greedy: loop first (prefer more iterations). Reluctant: exit
		 * first (prefer fewer iterations).
		 */
		RPRNFAState *exitState;
		RPRPatternElement *jumpElem;
		RPRPatternElement *nextElem;

		/*
		 * Create exit state first (need original counts before modifying
		 * state)
		 */
		exitState = nfa_state_clone(winstate, elem->next,
									state->counts, state->isAbsorbable);
		/* Exit branch: clear the group's own count (count-clear policy) */
		exitState->counts[depth] = 0;
		nextElem = &elements[exitState->elemIdx];

		/* END->END: increment outer END's count */
		if (RPRElemIsEnd(nextElem) && exitState->counts[nextElem->depth] < RPR_COUNT_INF)
			exitState->counts[nextElem->depth]++;

		/* Prepare loop state */
		state->elemIdx = elem->jump;
		jumpElem = &elements[state->elemIdx];

		if (RPRElemIsReluctant(elem))
		{
			RPRNFAState *savedMatch = ctx->matchedState;

			/* Exit first (preferred for reluctant) */
			nfa_route_to_elem(winstate, ctx, exitState, nextElem,
							  currentPos);

			/*
			 * If exit path reached FIN, shortest match is found. Skip loop to
			 * prevent longer matches from replacing it.
			 */
			if (ctx->matchedState != savedMatch)
			{
				nfa_state_free(winstate, state);
				return;
			}

			/* Loop second */
			nfa_route_to_elem(winstate, ctx, state, jumpElem,
							  currentPos);
		}
		else
		{
			/* Loop first (preferred for greedy) */
			nfa_route_to_elem(winstate, ctx, state, jumpElem,
							  currentPos);
			/* Exit second */
			nfa_route_to_elem(winstate, ctx, exitState, nextElem,
							  currentPos);
		}
	}
}

/*
 * nfa_advance_var
 *
 * Handle VAR element: loop/exit transitions.
 * After match phase, all VAR states have matched - decide next action.
 */
static void
nfa_advance_var(WindowAggState *winstate, RPRNFAContext *ctx,
				RPRNFAState *state, RPRPatternElement *elem,
				int64 currentPos)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elements = pattern->elements;
	int			depth = elem->depth;
	int32		count = state->counts[depth];
	bool		canLoop = (elem->max == RPR_QUANTITY_INF || count < elem->max);
	bool		canExit = (count >= elem->min);

	/* min <= max, so !canExit (count < min) implies canLoop (count < max) */
	Assert(canLoop || canExit);

	/* elem->next must be a valid index for any reachable VAR */
	Assert(elem->next >= 0 && elem->next < pattern->numElements);

	if (canLoop && canExit)
	{
		/*
		 * Both loop and exit possible. Greedy: loop first (prefer longer
		 * match). Reluctant: exit first (prefer shorter match).
		 */
		RPRNFAState *cloneState;
		RPRPatternElement *nextElem;
		bool		reluctant = RPRElemIsReluctant(elem);

		/*
		 * Clone state for the first-priority path. For greedy, clone is the
		 * loop state; for reluctant, clone is the exit state.
		 */
		if (reluctant)
		{
			RPRNFAState *savedMatch = ctx->matchedState;

			/* Clone for exit, original stays for loop */
			cloneState = nfa_state_clone(winstate, elem->next,
										 state->counts, state->isAbsorbable);
			/* Exit: clear the VAR's own count (count-clear policy) */
			cloneState->counts[depth] = 0;
			nextElem = &elements[cloneState->elemIdx];

			/* When exiting directly to an outer END, increment its count */
			if (RPRElemIsEnd(nextElem))
			{
				if (cloneState->counts[nextElem->depth] < RPR_COUNT_INF)
					cloneState->counts[nextElem->depth]++;
			}

			/* Exit first (preferred for reluctant) */
			nfa_route_to_elem(winstate, ctx, cloneState, nextElem,
							  currentPos);

			/*
			 * If exit path reached FIN, the shortest match is found. Skip
			 * loop state to prevent longer matches from replacing it.
			 */
			if (ctx->matchedState != savedMatch)
			{
				nfa_state_free(winstate, state);
				return;
			}

			/* Loop second */
			nfa_add_state_unique(winstate, ctx, state);
		}
		else
		{
			/* Clone for loop, original used for exit */
			cloneState = nfa_state_clone(winstate, state->elemIdx,
										 state->counts, state->isAbsorbable);

			/* Loop first (preferred for greedy) */
			nfa_add_state_unique(winstate, ctx, cloneState);

			/* Exit second: clear the VAR's own count (count-clear policy) */
			state->counts[depth] = 0;
			state->elemIdx = elem->next;
			nextElem = &elements[state->elemIdx];

			/*
			 * Update isAbsorbable for target element (monotonic: AND
			 * preserves false)
			 */
			state->isAbsorbable = state->isAbsorbable &&
				RPRElemIsAbsorbableBranch(nextElem);

			/*
			 * When exiting directly to an outer END, increment its iteration
			 * count.  Simple VARs (min=max=1) handle this via inline advance
			 * in nfa_match, but quantified VARs bypass that path.
			 */
			if (RPRElemIsEnd(nextElem))
			{
				if (state->counts[nextElem->depth] < RPR_COUNT_INF)
					state->counts[nextElem->depth]++;
			}

			nfa_route_to_elem(winstate, ctx, state, nextElem,
							  currentPos);
		}
	}
	else if (canLoop)
	{
		/* Loop only: keep state as-is */
		nfa_add_state_unique(winstate, ctx, state);
	}
	else
	{
		/* Exit only: advance to next element (canExit necessarily true) */
		RPRPatternElement *nextElem;

		Assert(canExit);
		/* Exit: clear the VAR's own count (count-clear policy) */
		state->counts[depth] = 0;
		state->elemIdx = elem->next;
		nextElem = &elements[state->elemIdx];

		/*
		 * Update isAbsorbable for target element (monotonic: AND preserves
		 * false)
		 */
		state->isAbsorbable = state->isAbsorbable &&
			RPRElemIsAbsorbableBranch(nextElem);

		/* See comment above: increment outer END count for quantified VARs */
		if (RPRElemIsEnd(nextElem))
		{
			if (state->counts[nextElem->depth] < RPR_COUNT_INF)
				state->counts[nextElem->depth]++;
		}

		nfa_route_to_elem(winstate, ctx, state, nextElem, currentPos);
	}
}

/*
 * nfa_advance_state
 *
 * Recursively process a single state through epsilon transitions.
 * DFS traversal ensures states are added to ctx->states in lexical order.
 */
static void
nfa_advance_state(WindowAggState *winstate, RPRNFAContext *ctx,
				  RPRNFAState *state, int64 currentPos)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elem;

	Assert(state->elemIdx >= 0 && state->elemIdx < pattern->numElements);

	/* Protect against stack overflow for deeply complex patterns */
	check_stack_depth();

	/* Cycle detection: if this elemIdx was already visited in this DFS, bail */
	if (winstate->nfaVisitedElems[WORDNUM(state->elemIdx)] &
		((bitmapword) 1 << BITNUM(state->elemIdx)))
	{
		nfa_state_free(winstate, state);
		return;
	}

	elem = &pattern->elements[state->elemIdx];

	/*
	 * Mark epsilon elements (END, ALT, BEGIN, FIN) in visited to prevent
	 * infinite epsilon cycles.  VAR elements are marked later when added to
	 * the state list (nfa_add_state_unique), allowing legitimate loop-back to
	 * the same VAR in a new iteration.
	 */
	if (!RPRElemIsVar(elem))
		nfa_mark_visited(winstate, state->elemIdx);

	switch (elem->varId)
	{
		case RPR_VARID_FIN:
			/* FIN: record match */
			nfa_add_matched_state(winstate, ctx, state, currentPos);
			break;

		case RPR_VARID_ALT:
			nfa_advance_alt(winstate, ctx, state, elem, currentPos);
			break;

		case RPR_VARID_BEGIN:
			nfa_advance_begin(winstate, ctx, state, elem, currentPos);
			break;

		case RPR_VARID_END:
			nfa_advance_end(winstate, ctx, state, elem, currentPos);
			break;

		default:
			/* VAR element */
			nfa_advance_var(winstate, ctx, state, elem, currentPos);
			break;
	}
}

/*
 * nfa_advance
 *
 * Advance phase (divergence): transition from all surviving states.
 * Called after match phase with matched VAR states, or at context creation
 * for initial epsilon expansion (with currentPos = startPos - 1).
 *
 * Processes states in order, using recursive DFS to maintain lexical order.
 */
static void
nfa_advance(WindowAggState *winstate, RPRNFAContext *ctx, int64 currentPos)
{
	RPRNFAState *states = ctx->states;
	RPRNFAState *state;
	RPRNFAState *savedMatchedState;

	ctx->states = NULL;			/* Will rebuild */

	/* Process each state in lexical order (DFS order from previous advance) */
	while (states != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		savedMatchedState = ctx->matchedState;

		/*
		 * Clear visited bitmap before each state's DFS expansion.  Only the
		 * range touched since the previous reset (tracked via the high-water
		 * marks updated in nfa_mark_visited) needs to be cleared; for small
		 * NFAs this is the whole array, but for large NFAs whose DFS only
		 * reaches a few elements per advance it avoids walking the full
		 * bitmap.
		 */
		if (winstate->nfaVisitedMaxWord >= winstate->nfaVisitedMinWord)
		{
			memset(&winstate->nfaVisitedElems[winstate->nfaVisitedMinWord], 0,
				   sizeof(bitmapword) *
				   (winstate->nfaVisitedMaxWord -
					winstate->nfaVisitedMinWord + 1));
			winstate->nfaVisitedMinWord = PG_INT16_MAX;
			winstate->nfaVisitedMaxWord = -1;
		}

		state = states;
		states = states->next;

		/*
		 * Boundary contract: state->next is reset to NULL here, before
		 * crossing into nfa_advance_state's epsilon-expansion DFS.  The inner
		 * branches (nfa_advance_var, nfa_advance_begin/end/alt) treat
		 * state->next as already-NULL and don't reset it themselves; the
		 * other linking site is nfa_add_state_unique, which sets it when
		 * appending to ctx->states.
		 */
		state->next = NULL;

		nfa_advance_state(winstate, ctx, state, currentPos);

		/*
		 * Early termination: if a FIN was newly reached in this advance,
		 * remaining old states have worse lexical order and can be pruned.
		 * Only check for new FIN arrivals (not ones from previous rows).
		 */
		if (ctx->matchedState != savedMatchedState && states != NULL)
		{
			nfa_state_free_list(winstate, states);
			break;
		}
	}
}

/*
 * nfa_reevaluate_dependent_vars
 *		Re-evaluate match_start-dependent DEFINE variables for a specific
 *		context whose matchStartRow differs from the shared evaluation's
 *		nav_match_start.
 *
 * Only variables in defineMatchStartDependent are re-evaluated.  The
 * current row's slot (ecxt_outertuple) must already be set up by
 * rpr_evaluate_row().
 */
static void
nfa_reevaluate_dependent_vars(WindowAggState *winstate, RPRNFAContext *ctx,
							  int64 currentPos)
{
	ExprContext *econtext = winstate->rprContext;
	int64		saved_match_start = winstate->nav_match_start;
	int64		saved_pos = winstate->currentpos;

	/* Release the previous evaluation's DEFINE expression memory */
	ResetExprContext(econtext);

	/* Temporarily set nav_match_start and currentpos for FIRST/LAST */
	winstate->nav_match_start = ctx->matchStartRow;
	winstate->currentpos = currentPos;

	/* Invalidate nav_slot cache since match_start changed */
	winstate->nav_slot_pos = -1;

	foreach_ptr(ExprState, exprState, winstate->defineClauseExprs)
	{
		int			varIdx = foreach_current_index(exprState);

		if (bms_is_member(varIdx, winstate->defineMatchStartDependent))
		{
			Datum		result;
			bool		isnull;

			result = ExecEvalExpr(exprState, econtext, &isnull);
			winstate->nfaVarMatched[varIdx] = (!isnull && DatumGetBool(result));
		}
	}

	/* Restore original match_start, currentpos, and invalidate cache */
	winstate->nav_match_start = saved_match_start;
	winstate->currentpos = saved_pos;
	winstate->nav_slot_pos = -1;
}


/***********************************************************************
 * API exposed to nodeWindowAgg.c
 ***********************************************************************/

/*
 * ExecRPRStartContext
 *
 * Start a new match context at given position.
 * Initializes context, state absorption flags, and performs initial advance
 * to expand epsilon transitions (ALT branches, optional elements).
 * Adds context to the tail of winstate->nfaContext list.
 */
RPRNFAContext *
ExecRPRStartContext(WindowAggState *winstate, int64 startPos)
{
	RPRNFAContext *ctx;
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elem;

	ctx = nfa_context_make(winstate);
	ctx->matchStartRow = startPos;
	ctx->states = nfa_state_make(winstate); /* initial state at elem 0 */

	elem = &pattern->elements[0];

	if (RPRElemIsAbsorbableBranch(elem))
	{
		ctx->states->isAbsorbable = true;
	}
	else
	{
		ctx->hasAbsorbableState = false;
		ctx->allStatesAbsorbable = false;
		ctx->states->isAbsorbable = false;
	}

	/*
	 * Add to tail of active context list (doubly-linked, oldest-first).
	 * matchStartRow is nondecreasing along the list, so the head holds the
	 * smallest -- an ordering other code relies on.
	 */
	Assert(winstate->nfaContextTail == NULL ||
		   startPos >= winstate->nfaContextTail->matchStartRow);
	ctx->prev = winstate->nfaContextTail;
	ctx->next = NULL;
	if (winstate->nfaContextTail != NULL)
		winstate->nfaContextTail->next = ctx;
	else
		winstate->nfaContext = ctx; /* first context becomes head */
	winstate->nfaContextTail = ctx;

	/*
	 * Initial advance (divergence): expand ALT branches and create exit
	 * states for VAR elements with min=0. This prepares the context for the
	 * first row's match phase.
	 *
	 * Use startPos - 1 as currentPos since no row has been consumed yet. If
	 * FIN is reached via epsilon transitions, matchEndRow = startPos - 1
	 * which is less than matchStartRow, resulting in UNMATCHED treatment.
	 */
	nfa_advance(winstate, ctx, startPos - 1);

	return ctx;
}

/*
 * ExecRPRGetHeadContext
 *
 * Return the head context if its start position matches pos.
 * Returns NULL if no context exists or head doesn't match pos.
 */
RPRNFAContext *
ExecRPRGetHeadContext(WindowAggState *winstate, int64 pos)
{
	RPRNFAContext *ctx = winstate->nfaContext;

	/*
	 * Contexts are sorted by matchStartRow ascending.  If the head context
	 * doesn't match pos, no context exists for this position.
	 */
	if (ctx == NULL || ctx->matchStartRow != pos)
		return NULL;

	return ctx;
}

/*
 * ExecRPRFreeContext
 *
 * Unlink context from active list and return it to free list.
 * Also frees any states in the context.
 */
void
ExecRPRFreeContext(WindowAggState *winstate, RPRNFAContext *ctx)
{
	/* Unlink from active list first */
	nfa_unlink_context(winstate, ctx);

	/* Update statistics */
	winstate->nfaContextsActive--;

	if (ctx->states != NULL)
		nfa_state_free_list(winstate, ctx->states);
	if (ctx->matchedState != NULL)
		nfa_state_free(winstate, ctx->matchedState);

	ctx->states = NULL;
	ctx->matchedState = NULL;
	ctx->next = winstate->nfaContextFree;
	winstate->nfaContextFree = ctx;
}

/*
 * ExecRPRRecordContextSuccess
 *
 * Record a successful context in statistics.
 */
void
ExecRPRRecordContextSuccess(WindowAggState *winstate, int64 matchLen)
{
	winstate->nfaMatchesSucceeded++;
	nfa_update_length_stats(winstate->nfaMatchesSucceeded,
							&winstate->nfaMatchLen,
							matchLen);
}

/*
 * ExecRPRRecordContextFailure
 *
 * Record a failed context in statistics.
 * If failedLen == 1, count as pruned (failed on first row).
 * If failedLen > 1, count as mismatched and update length stats.
 */
void
ExecRPRRecordContextFailure(WindowAggState *winstate, int64 failedLen)
{
	if (failedLen == 1)
	{
		winstate->nfaContextsPruned++;
	}
	else
	{
		winstate->nfaMatchesFailed++;
		nfa_update_length_stats(winstate->nfaMatchesFailed,
								&winstate->nfaFailLen,
								failedLen);
	}
}

/*
 * ExecRPRProcessRow
 *
 * Process all contexts for one row:
 *   1. Match all contexts (convergence) - evaluate VARs, prune dead states
 *   2. Absorb redundant contexts - ideal timing after convergence
 *   3. Advance all contexts (divergence) - create new states for next row
 */
void
ExecRPRProcessRow(WindowAggState *winstate, int64 currentPos,
				  bool hasLimitedFrame, int64 frameOffset)
{
	RPRNFAContext *ctx;
	bool	   *varMatched = winstate->nfaVarMatched;
	bool		hasDependent = !bms_is_empty(winstate->defineMatchStartDependent);

	/* Allow query cancellation once per row for simple/low-state patterns */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Phase 1: Match all contexts (convergence).  Evaluate VAR elements,
	 * update counts, remove dead states.
	 */
	for (ctx = winstate->nfaContext; ctx != NULL; ctx = ctx->next)
	{
		if (ctx->states == NULL)
			continue;

		/* Check frame boundary - finalize the context when it is reached */
		if (hasLimitedFrame)
		{
			int64		ctxFrameEnd;

			/*
			 * Clamp to PG_INT64_MAX on overflow.  frameOffset can be as large
			 * as PG_INT64_MAX (e.g. "ROWS <huge> FOLLOWING"), so add the
			 * offset and the trailing +1 in two separately checked steps to
			 * avoid signed-integer overflow in the "frameOffset + 1"
			 * subexpression.
			 */
			if (pg_add_s64_overflow(ctx->matchStartRow, frameOffset,
									&ctxFrameEnd) ||
				pg_add_s64_overflow(ctxFrameEnd, 1, &ctxFrameEnd))
				ctxFrameEnd = PG_INT64_MAX;

			/*
			 * currentPos advances by exactly one per call, and a finalized
			 * context is skipped by the states == NULL guard above, so it can
			 * only ever reach ctxFrameEnd, never overshoot it.  The Assert
			 * turns a future change that broke that invariant into an
			 * immediate failure rather than a silent slip past the boundary.
			 */
			Assert(currentPos <= ctxFrameEnd);

			if (currentPos == ctxFrameEnd)
			{
				/* Frame boundary reached: force mismatch */
				nfa_match(winstate, ctx, NULL, currentPos);
				continue;
			}
		}

		/*
		 * If this context has a different matchStartRow than the one used in
		 * the shared evaluation, re-evaluate match_start-dependent variables
		 * with this context's matchStartRow.
		 */
		if (hasDependent && ctx->matchStartRow != winstate->nav_match_start)
			nfa_reevaluate_dependent_vars(winstate, ctx, currentPos);
		nfa_match(winstate, ctx, varMatched, currentPos);
		ctx->lastProcessedRow = currentPos;
	}

	/*
	 * Phase 2: Absorb redundant contexts.  After match phase, states have
	 * converged - ideal for absorption.  First update absorption flags that
	 * may have changed due to state removal.
	 */
	if (winstate->rpPattern->isAbsorbable)
	{
		for (ctx = winstate->nfaContext; ctx != NULL; ctx = ctx->next)
			nfa_update_absorption_flags(ctx);

		nfa_absorb_contexts(winstate);
	}

	/*
	 * Phase 3: Advance all contexts (divergence).  Create new states
	 * (loop/exit) from surviving matched states.
	 */
	for (ctx = winstate->nfaContext; ctx != NULL; ctx = ctx->next)
	{
		if (ctx->states == NULL)
			continue;

		/*
		 * Phase 1 already handled frame boundary exceeded contexts by forcing
		 * mismatch (nfa_match with NULL), which removes all states (all
		 * states are at VAR positions after advance). So any surviving
		 * context here must be within its frame boundary.
		 *
		 * Compute the (clamped) frame end the same way as Phase 1, using two
		 * separately checked adds so that "frameOffset + 1" cannot overflow
		 * when frameOffset is near PG_INT64_MAX.
		 */
#ifdef USE_ASSERT_CHECKING
		if (hasLimitedFrame)
		{
			int64		ctxFrameEnd;

			if (pg_add_s64_overflow(ctx->matchStartRow, frameOffset,
									&ctxFrameEnd) ||
				pg_add_s64_overflow(ctxFrameEnd, 1, &ctxFrameEnd))
				ctxFrameEnd = PG_INT64_MAX;
			Assert(currentPos < ctxFrameEnd);
		}
#endif

		nfa_advance(winstate, ctx, currentPos);
	}
}

/*
 * ExecRPRCleanupDeadContexts
 *
 * Remove contexts that have failed (no active states and no match).
 * These are contexts that failed during normal processing and should be
 * counted as pruned (if length 1) or mismatched (if length > 1).
 */
void
ExecRPRCleanupDeadContexts(WindowAggState *winstate, RPRNFAContext *excludeCtx)
{
	RPRNFAContext *ctx;
	RPRNFAContext *next;

	for (ctx = winstate->nfaContext; ctx != NULL; ctx = next)
	{
		CHECK_FOR_INTERRUPTS();

		next = ctx->next;

		/* Skip the target context and contexts still processing */
		if (ctx == excludeCtx || ctx->states != NULL)
			continue;

		/* Skip successfully matched contexts (will be handled by SKIP logic) */
		if (ctx->matchEndRow >= ctx->matchStartRow)
			continue;

		/*
		 * Failed context: always removed below.  Only record the failure
		 * statistic if it actually processed its start row; contexts created
		 * for beyond-partition rows are removed without being counted.
		 */
		if (ctx->lastProcessedRow >= ctx->matchStartRow)
		{
			int64		failedLen = ctx->lastProcessedRow - ctx->matchStartRow + 1;

			ExecRPRRecordContextFailure(winstate, failedLen);
		}

		ExecRPRFreeContext(winstate, ctx);
	}
}

/*
 * ExecRPRFinalizeAllContexts
 *
 * Partition-end classification policy: kill any VAR states still pursuing
 * when rows run out, so cleanup sees a uniform ctx->states == NULL across
 * every context.  By the time this runs, all genuine FIN reaches have
 * already been recorded in-flight; three shapes survive here:
 *
 *   - Pure pursuit (matchedState == NULL): VAR states waiting for input
 *     that never arrives (e.g., A+ B mid-pattern at partition end).
 *   - Empty-match candidate + pursuit (matchedState != NULL,
 *     matchEndRow < matchStartRow): initial-advance FIN-via-skip recorded
 *     an empty match while VAR states are still chasing a longer one
 *     (e.g., greedy A*).
 *   - Real match + pursuit (matchedState != NULL,
 *     matchEndRow >= matchStartRow): a match has been recorded and VAR
 *     states are still looping for a longer one.
 *
 * Killing the VAR reclassifies the first two as failures in cleanup
 * (otherwise they linger without contributing to stats).  The third is
 * stat-neutral -- cleanup skips it either way -- but goes through the
 * same uniform path so partition-end classification stays centralized.
 *
 * Implementation: nfa_match with NULL forces VAR mismatch; nfa_advance
 * then drains any remaining epsilon transitions.
 */
void
ExecRPRFinalizeAllContexts(WindowAggState *winstate, int64 lastPos)
{
	RPRNFAContext *ctx;

	for (ctx = winstate->nfaContext; ctx != NULL; ctx = ctx->next)
	{
		CHECK_FOR_INTERRUPTS();

		if (ctx->states != NULL)
		{
			nfa_match(winstate, ctx, NULL, lastPos);
			nfa_advance(winstate, ctx, lastPos);
		}
	}
}
