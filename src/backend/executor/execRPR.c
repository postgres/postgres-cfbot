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
 * the touched range instead of the full nfaVisitedEnds bitmap.
 */
static inline void
nfa_mark_visited(WindowAggState *winstate, int16 elemIdx)
{
	int16		w = WORDNUM(elemIdx);

	winstate->nfaVisitedEnds[w] |= ((bitmapword) 1 << BITNUM(elemIdx));
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
							   RPRPatternElement *elem, RPRVarMatch *varMatched);
static void nfa_match(WindowAggState *winstate, RPRNFAContext *ctx,
					  RPRVarMatch *varMatched, int64 currentPos);
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
 * The engine runs three phases per row: match (evaluate VARs, prune dead
 * states), absorb (drop contexts an older context already covers), advance
 * (expand epsilon transitions until states park on VARs).  Per-element
 * advance behaviour, the absorption argument and the dual-flag contract are
 * documented in README.rpr chapters VIII and IX and in the RPRNFAContext
 * comment in nodes/execnodes.h.
 */

/*
 * nfa_state_make
 *
 * Allocate an NFA state, reusing from freeList if available.
 * freeList is stored in WindowAggState for reuse across match attempts.
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
 * nfa_exit_to
 *
 * Move state out of the construct owning depth and onto targetIdx, then
 * return the target element.  Callers route from there.
 *
 * Centralizes three conventions whose violations are silent:
 *
 * - Count-clear: zero the exited depth slot so the next occupant enters at
 *   zero (asserted on entry by nfa_advance_begin/nfa_route_to_elem).
 * - Arrival increment: landing on an END completes one iteration
 *   (saturating at RPR_COUNT_INF).
 * - isAbsorbable is recomputed against the target and is monotonic.
 *   Reapplying it is idempotent, so clone and in-place callers share this path.
 */
static RPRPatternElement *
nfa_exit_to(WindowAggState *winstate, RPRNFAState *state, int depth,
			int16 targetIdx)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *nextElem;

	state->counts[depth] = 0;
	state->elemIdx = targetIdx;
	nextElem = &pattern->elements[targetIdx];

	state->isAbsorbable = state->isAbsorbable &&
		RPRElemIsAbsorbableBranch(nextElem);

	if (RPRElemIsEnd(nextElem) &&
		state->counts[nextElem->depth] < RPR_COUNT_INF)
		state->counts[nextElem->depth]++;

	return nextElem;
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
	 * Nothing is parked once this advance has recorded: a state kept here
	 * survives to the next row, where it could complete and replace the match
	 * that outranks it.
	 */
	Assert(!ctx->matchUpdated);

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
	/*
	 * One advance records at most one match.  The guards below stop every
	 * less-preferred path once matchUpdated is set, so arriving here twice
	 * would mean a path that can reach FIN is not reading it.
	 */
	Assert(!ctx->matchUpdated);

	if (ctx->matchedState != NULL)
		nfa_state_free(winstate, ctx->matchedState);

	ctx->matchedState = state;
	state->next = NULL;
	ctx->matchEndRow = matchEndRow;

	/*
	 * Tell the frames that are unwinding.  FIN is not marked visited, so an
	 * expansion can reach it more than once, and the later arrival is the
	 * less preferred one: the paths that cut less-preferred alternatives read
	 * this and stop rather than record again.
	 */
	ctx->matchUpdated = true;

	/* Prune contexts that started within this match's range */
	if (winstate->rpSkipTo == ST_PAST_LAST_ROW)
	{
		int64		skippedLen;

		while (ctx->next != NULL &&
			   ctx->next->matchStartRow <= matchEndRow)
		{
			RPRNFAContext *nextCtx = ctx->next;

			/* Only later-starting contexts are freed; callers walk forward */
			Assert(nextCtx->matchStartRow > ctx->matchStartRow);
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
	ctx->matchUpdated = false;

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
 *   allStatesAbsorbable: true if ALL states in context are absorbable and no
 *     match is recorded.  Dynamic (false -> true as non-absorbable states die
 *     off), except that a recorded match pins it false: absorbing would free
 *     a match no absorbing context can reproduce.
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
 * Try to absorb ctx (newer) into an older in-progress context.  If one is
 * found, ctx is unlinked and freed here.
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
 * varMatched is a per-row tri-state cache indexed by varId.  Evaluation is
 * lazy: the variable's DEFINE predicate is evaluated here the first time the
 * NFA consumes the variable (cache is RPR_VAR_UNEVALUATED), then cached, so a
 * variable that no active state tests at this row is never evaluated.  This
 * matches ISO/IEC 19075-5, where a Boolean condition is evaluated only with
 * the current row tentatively mapped to that variable.  A NULL varMatched
 * makes every VAR not match; nfa_match() is called that way to force a
 * mismatch at a frame boundary and at partition-end finalization.
 *
 * The caller must have set up the current row (ecxt_outertuple, currentpos,
 * nav_match_start, nav_slot cache) via rpr_prepare_row() /
 * nfa_reevaluate_dependent_vars() before consumption.
 *
 * Per ISO/IEC 19075-5 Feature R020, pattern variables not listed in DEFINE
 * are implicitly TRUE -- they match every row.  This is checked via
 * varId >= list_length.
 */
static bool
nfa_eval_var_match(WindowAggState *winstate, RPRPatternElement *elem,
				   RPRVarMatch *varMatched)
{
	int			varId;

	/* This function should only be called for VAR elements */
	Assert(RPRElemIsVar(elem));

	if (varMatched == NULL)
		return false;

	varId = elem->varId;
	if (varId >= list_length(winstate->defineClauseExprs))
		return true;

	/* Lazily evaluate this variable's DEFINE predicate on first consumption. */
	if (varMatched[varId] == RPR_VAR_UNEVALUATED)
	{
		ExprState  *exprState = list_nth(winstate->defineClauseExprs, varId);
		Datum		result;
		bool		isnull;

		result = ExecEvalExpr(exprState, winstate->rprContext, &isnull);
		varMatched[varId] = (!isnull && DatumGetBool(result)) ?
			RPR_VAR_TRUE : RPR_VAR_FALSE;
	}

	return (varMatched[varId] == RPR_VAR_TRUE);
}

/*
 * nfa_match
 *
 * Match phase (convergence): evaluate VAR elements against current row.
 * Only updates counts and removes dead states. Minimal transitions.
 *
 * For VAR elements:
 *   - matched: count++ (saturating at RPR_COUNT_INF), keep state
 *   - not matched: remove state (exit alternatives already exist from
 *     previous advance when count >= min was satisfied)
 *
 * For VARs that reached max count followed by END:
 *   - Advance through the END-element chain to the absorption
 *     comparison point
 *   - Only deterministic exits are handled.  A count saturates at
 *     RPR_COUNT_INF, so count >= max does not by itself exclude an unbounded
 *     VAR; what excludes it is the absorbable-region test, which no unbounded
 *     VAR inside a still-looping group passes.
 *   - Chains through END elements while count >= max (must-exit path)
 *
 * Non-VAR elements (only an END parked by the chain above) are kept as-is for
 * advance phase.
 *
 * currentPos is unused by the matching logic itself; it is accepted so that
 * every NFA helper carries the row index for debugging.
 */
static void
nfa_match(WindowAggState *winstate, RPRNFAContext *ctx, RPRVarMatch *varMatched,
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
				 * In nested patterns like ((A (B C){2}){2})+, a VAR reaching
				 * its max triggers an exit cascade: inner END increments
				 * inner group count, which may itself reach max, requiring an
				 * exit to the next outer END.  The loop below walks this
				 * chain.
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
		{
			RPRPatternElement *landElem;

			skipState = nfa_state_clone(winstate, nextElem->next,
										state->counts, state->isAbsorbable);

			/*
			 * When the skip lands directly on an outer END, increment its
			 * iteration count, just as the exit path in nfa_advance_var does:
			 * a skipped iteration still ran, and that count is what the
			 * group's min check and the cycle guard's below-min fall-through
			 * both read.
			 */
			landElem = &winstate->rpPattern->elements[skipState->elemIdx];
			if (RPRElemIsEnd(landElem) &&
				skipState->counts[landElem->depth] < RPR_COUNT_INF)
				skipState->counts[landElem->depth]++;
		}

		if (skipState != NULL && RPRElemIsReluctant(nextElem))
		{
			/*
			 * Reluctant optional VAR: prefer skipping.  Explore the skip path
			 * first so it outranks the enter (match) path; if it reaches FIN
			 * the shortest match is found and the enter state is dropped.
			 * This mirrors the reluctant branch of nfa_advance_begin used by
			 * the leading-position and optional-group paths.
			 */
			nfa_advance_state(winstate, ctx, skipState, currentPos);

			if (ctx->matchUpdated)
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
 *
 * ALT.next is the first branch's content and ALT.jump is that branch's
 * terminating SEP.  Each SEP.jump links to the next branch's SEP (-1 on the
 * last) and each SEP.next is the next branch's content.  The walk reads SEP
 * but never enters one -- states are always created at a branch's content.
 */
static void
nfa_advance_alt(WindowAggState *winstate, RPRNFAContext *ctx,
				RPRNFAState *state, RPRPatternElement *elem,
				int64 currentPos)
{
	RPRPattern *pattern = winstate->rpPattern;
	RPRPatternElement *elements = pattern->elements;
	RPRElemIdx	branchStart = elem->next;
	RPRElemIdx	sepIdx = elem->jump;

	while (sepIdx != RPR_ELEMIDX_INVALID)
	{
		RPRPatternElement *sepElem;
		RPRNFAState *newState;

		/* Create independent state at this branch's content */
		newState = nfa_state_clone(winstate, branchStart,
								   state->counts, state->isAbsorbable);

		/* Recursively process this branch before the next */
		nfa_advance_state(winstate, ctx, newState, currentPos);

		/*
		 * Branches are enumerated in preference order, so once one of them
		 * has recorded a match the later branches must not be explored: a
		 * later branch would either reach FIN in this same DFS and replace
		 * the preferred match, or park states that complete on a later row
		 * and replace it then.  Same technique as the reluctant paths in
		 * nfa_route_to_elem and nfa_advance_begin.
		 *
		 * PATTERN (A* | B) on a row where A is false and B is true: the A*
		 * branch is explored first, its skip path runs straight to FIN, and
		 * the empty match is recorded here.  Breaking leaves that match
		 * standing, which is what the preference order asks for.  Without the
		 * break, B would be expanded too, would match the row, and its
		 * one-row match would replace the empty one, as if the pattern were
		 * written (B | A*).
		 */
		if (ctx->matchUpdated)
			break;

		Assert(sepIdx >= 0 && sepIdx < pattern->numElements);
		sepElem = &elements[sepIdx];
		Assert(RPRElemIsSep(sepElem));

		/* The last branch's SEP has no link, ending the walk */
		branchStart = sepElem->next;
		sepIdx = sepElem->jump;
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
		RPRPatternElement *landElem;

		skipState = nfa_state_clone(winstate, elem->jump,
									state->counts, state->isAbsorbable);

		/*
		 * As in nfa_route_to_elem, a skip that lands directly on an outer END
		 * still counts as an iteration of that END's group.
		 */
		landElem = &elements[elem->jump];
		if (RPRElemIsEnd(landElem) &&
			skipState->counts[landElem->depth] < RPR_COUNT_INF)
			skipState->counts[landElem->depth]++;
	}

	if (skipState != NULL && RPRElemIsReluctant(elem))
	{
		/* Reluctant: skip first (prefer fewer iterations), enter second */
		nfa_route_to_elem(winstate, ctx, skipState,
						  &elements[elem->jump], currentPos);

		/* The skip matched: do not enter the group over it */
		if (ctx->matchUpdated)
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

		/* Entering matched: do not take the skip over it */
		if (ctx->matchUpdated)
		{
			if (skipState != NULL)
				nfa_state_free(winstate, skipState);
			return;
		}

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
		 * The body decides the order, not the group's own greed: the
		 * fast-forward comes first exactly when the body prefers the empty
		 * match (RPR_ELEM_EMPTY_PREFERRED).  If it then reaches FIN, the
		 * loop-back is dropped so a longer match cannot replace the preferred
		 * one -- mirroring the min<=count<max branch below.  The ffState
		 * snapshot is taken BEFORE modifying state, since both paths diverge
		 * from here.
		 *----------
		 */
		if (RPRElemCanEmptyLoop(elem))
		{
			ffState = nfa_state_clone(winstate, state->elemIdx,
									  state->counts, state->isAbsorbable);

			/*
			 * nfa_exit_to()'s isAbsorbable recompute is a no-op here:
			 * EMPTY_LOOP groups are never in an absorbable region.
			 */
			nextElem = nfa_exit_to(winstate, ffState, depth, elem->next);
		}

		/*
		 * Prepare the loop-back state.  Visited marks are deliberately left
		 * in place; see the cycle guard in nfa_advance_state.
		 */
		state->elemIdx = elem->jump;
		jumpElem = &elements[state->elemIdx];

		if (ffState != NULL && RPRElemIsEmptyPreferred(elem))
		{
			/* Body prefers empty: take the fast-forward (exit) first */
			nfa_route_to_elem(winstate, ctx, ffState, nextElem,
							  currentPos);

			/* The fast-forward matched: do not loop back over it */
			if (ctx->matchUpdated)
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

			/* The loop-back matched: do not fast-forward over it */
			if (ctx->matchUpdated)
			{
				if (ffState != NULL)
					nfa_state_free(winstate, ffState);
				return;
			}

			if (ffState != NULL)
				nfa_route_to_elem(winstate, ctx, ffState, nextElem,
								  currentPos);
		}
	}
	else if (elem->max != RPR_QUANTITY_INF && count >= elem->max)
	{
		/* Must exit: reached max iterations. */
		RPRPatternElement *nextElem;

		nextElem = nfa_exit_to(winstate, state, depth, elem->next);

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
		nextElem = nfa_exit_to(winstate, exitState, depth, elem->next);

		/* Prepare loop state */
		state->elemIdx = elem->jump;
		jumpElem = &elements[state->elemIdx];

		if (RPRElemIsReluctant(elem))
		{
			/* Exit first (preferred for reluctant) */
			nfa_route_to_elem(winstate, ctx, exitState, nextElem,
							  currentPos);

			/* The exit matched: do not loop over it */
			if (ctx->matchUpdated)
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

			/* The loop matched: do not exit over it */
			if (ctx->matchUpdated)
			{
				nfa_state_free(winstate, exitState);
				return;
			}

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
			/* Clone for exit, original stays for loop */
			cloneState = nfa_state_clone(winstate, elem->next,
										 state->counts, state->isAbsorbable);
			nextElem = nfa_exit_to(winstate, cloneState, depth, elem->next);

			/* Exit first (preferred for reluctant) */
			nfa_route_to_elem(winstate, ctx, cloneState, nextElem,
							  currentPos);

			/* The exit matched: do not loop over it */
			if (ctx->matchUpdated)
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

			/* Exit second: nfa_match handles only deterministic exits */
			nextElem = nfa_exit_to(winstate, state, depth, elem->next);

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
		nextElem = nfa_exit_to(winstate, state, depth, elem->next);

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

	/*
	 * Cycle detection.  Only a nullable END is marked, so a set bit means the
	 * body just derived an empty match for this iteration: a DFS takes only
	 * epsilon transitions, so no row was consumed since the last visit.
	 *
	 * Nothing else needs guarding: a revisit is a cycle only when it carries
	 * no progress, and any other loop-back has consumed a row.  Dropping one
	 * loses the match outright -- ((A | B B){1,3}){3} then finds nothing.
	 */
	if (winstate->nfaVisitedEnds[WORDNUM(state->elemIdx)] &
		((bitmapword) 1 << BITNUM(state->elemIdx)))
	{
		RPRPatternElement *hitElem = &pattern->elements[state->elemIdx];

		Assert(RPRElemIsEnd(hitElem) && RPRElemCanEmptyLoop(hitElem));

		if (state->counts[hitElem->depth] >= hitElem->min)
		{
			RPRPatternElement *nextElem;

			/* An END always has a valid exit target after finalization. */
			Assert(hitElem->next != RPR_ELEMIDX_INVALID);

			/*
			 * Leave the group here, and enumerate that exit at this rank
			 * rather than discarding the state: discarding demotes "leave the
			 * group" below the remaining alternatives, and a less-preferred
			 * branch then consumes rows the match should not have.  At or
			 * above the lower bound an empty iteration stops the quantifier
			 * (SQL/RPR follows Perl here).
			 */

			nextElem = nfa_exit_to(winstate, state, hitElem->depth,
								   hitElem->next);

			nfa_route_to_elem(winstate, ctx, state, nextElem, currentPos);
			return;
		}

		/*
		 * Below the lower bound the quantifier cannot exit, so fall through
		 * to the normal must-loop path.  Each empty iteration's arrival
		 * increment advances the count, so this reaches min and exits above.
		 * Clearing the marks here instead would also disarm the guard for
		 * nested reluctant loops, whose empty iterations then recurse without
		 * bound: (A (B*?)+?){2,} on a single matching row.
		 */
	}

	elem = &pattern->elements[state->elemIdx];

	/*
	 * Only a nullable END is ever tested; see the guard above.
	 *
	 * XXX this bounds the cycle, not the cost.  Leaving ALT and BEGIN
	 * unmarked lets them be re-entered any number of times within one
	 * expansion, so a run of alternations whose branches are all nullable
	 * enumerates paths rather than states: nfa_advance_alt() recurses once
	 * per branch and fillRPRPatternAlt() converges every branch tail on the
	 * same element, making k such alternations cost 2^k.  (A?|B?){30} takes
	 * over half an hour, and the recursion depth stays at k, so
	 * check_stack_depth() never fires.  Bounding the cost needs a revisit key
	 * of (elemIdx, counts); this bitmap identifies elemIdx alone.
	 */
	if (RPRElemCanEmptyLoop(elem))
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
			/* VAR element; a SEP would land here, so see fillRPRPatternAlt */
			Assert(!RPRElemIsSep(elem) && RPRElemIsVar(elem));
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

	ctx->states = NULL;			/* Will rebuild */
	ctx->matchUpdated = false;

	/* Process each state in lexical order (DFS order from previous advance) */
	while (states != NULL)
	{
		CHECK_FOR_INTERRUPTS();

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
			memset(&winstate->nfaVisitedEnds[winstate->nfaVisitedMinWord], 0,
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
		if (ctx->matchUpdated && states != NULL)
		{
			nfa_state_free_list(winstate, states);
			break;
		}
	}
}

/*
 * nfa_reevaluate_dependent_vars
 *		Invalidate match_start-dependent DEFINE variables for a context whose
 *		matchStartRow differs from the shared evaluation's nav_match_start.
 *
 * Only variables in defineMatchStartDependent are affected: they are reset to
 * RPR_VAR_UNEVALUATED so nfa_match() re-evaluates them lazily against this
 * context's matchStartRow.  match_start-independent variables keep their
 * cached value across contexts, since they do not read nav_match_start.
 *
 * nav_match_start is installed for this context and left in place: FIRST/LAST
 * read it at evaluation time, which happens later during nfa_match(), so it
 * must NOT be restored here.  The next context's invalidation, or the next
 * row's shared setup in advance_reduced_frame_nfa, overwrites it.
 */
static void
nfa_reevaluate_dependent_vars(WindowAggState *winstate, RPRNFAContext *ctx,
							  int64 currentPos)
{
	int			varIdx = -1;

	/* Caller keeps winstate->currentpos at the scan position for lazy eval. */
	Assert(winstate->currentpos == currentPos);

	/*
	 * Release the previous context's DEFINE evaluation memory.  Match-start-
	 * dependent variables are re-evaluated once per context (they are reset
	 * to UNEVALUATED below), so without this reset their per-tuple scratch
	 * would accumulate across every context of a row -- bounded only by the
	 * per-row reset in rpr_prepare_row.  rprContext is the dedicated DEFINE
	 * context, so this frees neither the input nor the output tuple memory.
	 */
	ResetExprContext(winstate->rprContext);

	/* Install this context's match_start for FIRST/LAST and keep it in place. */
	winstate->nav_match_start = ctx->matchStartRow;

	/* Invalidate nav_slot cache since match_start changed */
	winstate->nav_slot_pos = -1;

	/* Reset only the dependent variables so they re-evaluate lazily. */
	while ((varIdx = bms_next_member(winstate->defineMatchStartDependent,
									 varIdx)) >= 0)
		winstate->nfaVarMatched[varIdx] = RPR_VAR_UNEVALUATED;
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
	 * matchStartRow increases along the list, so the head holds the smallest
	 * -- an ordering other code relies on.  At most one context starts at a
	 * row: the on-demand path in update_reduced_frame creates one only where
	 * none exists.
	 */
	Assert(winstate->nfaContextTail == NULL ||
		   startPos > winstate->nfaContextTail->matchStartRow);
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
	 * FIN is reached via epsilon transitions, matchEndRow = startPos - 1,
	 * which is how an empty match is represented.
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
	RPRVarMatch *varMatched = winstate->nfaVarMatched;
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
		 * the shared evaluation, invalidate its match_start-dependent
		 * variables so nfa_match() re-evaluates them lazily with this
		 * context's matchStartRow.
		 *
		 * The head context carries no explicit invalidation: it relies on the
		 * ambient nav_match_start installed by advance_reduced_frame_nfa, so
		 * it must be reached before any other context overwrites
		 * nav_match_start.
		 */
		Assert(ctx != winstate->nfaContext ||
			   ctx->matchStartRow == winstate->nav_match_start);

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

		/*
		 * Skip contexts that recorded a match (handled by SKIP logic).  Test
		 * matchedState, not matchEndRow: an empty match ends at matchStartRow
		 * - 1, so a row-length test would take it for a failure and count it
		 * as pruned or mismatched.
		 */
		if (ctx->matchedState != NULL)
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
 * Killing the VAR reclassifies pure pursuit as a failure in cleanup
 * (otherwise it lingers without contributing to stats).  The other two both
 * carry a recorded match, so cleanup skips them: an empty match is a
 * length-0 success, not a failure, and update_reduced_frame registers it as
 * such through its head-context path.  They still go through the same
 * uniform path so partition-end classification stays centralized.
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

			/* Defensive: advance leaves only VAR states, all removed above. */
			nfa_advance(winstate, ctx, lastPos);
		}
	}
}
