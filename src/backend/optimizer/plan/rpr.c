/*-------------------------------------------------------------------------
 *
 * rpr.c
 *	  Row Pattern Recognition pattern compilation for planner
 *
 * This file contains functions for optimizing the RPR pattern parse tree and
 * compiling it to a flat element array for NFA execution by WindowAgg.
 *
 * Key components:
 *   1. Pattern Optimization: Simplifies patterns before compilation
 *      (e.g., flatten nested SEQ/ALT, merge consecutive vars)
 *   2. Pattern Compilation: Converts parse tree to flat element array for NFA
 *   3. Absorption Analysis: Computes flags for O(n^2)->O(n) optimization
 *
 * Context Absorption Optimization:
 *   When a pattern starts with a greedy unbounded element (e.g., A+ or (A B)+),
 *   newer contexts cannot produce longer matches than older contexts.
 *   By absorbing (eliminating) redundant newer contexts, we reduce
 *   complexity from O(n^2) to O(n) for patterns like A+ B.
 *
 *   The absorption analysis uses two element flags:
 *   - RPR_ELEM_ABSORBABLE: marks WHERE to compare (comparison point)
 *   - RPR_ELEM_ABSORBABLE_BRANCH: marks the absorbable region
 *
 *   EXPLAIN shows both on the Pattern: line, # for the comparison point and
 *   ~ for the region.
 *
 *   See computeAbsorbability() and the detailed comments before
 *   isUnboundedStart() for the full design explanation.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/rpr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/int.h"
#include "miscadmin.h"
#include "optimizer/rpr.h"

/* Forward declarations */
static bool rprPatternEqual(RPRPatternNode *a, RPRPatternNode *b);
static bool rprPatternChildrenEqual(List *a, List *b);
static int64 rprNodeRowCount(RPRPatternNode *node);
static int64 rprBodyRowCount(List *children);
static bool rprBodyHasUniformLength(List *children);
static bool rprChildrenMatchAt(List *children, int start, List *content);
static List *rprGroupContent(RPRPatternNode *group);
static bool rprTryAddIteration(RPRPatternNode *group);
static pg_nodiscard List *flattenSeqChildren(List *children);
static pg_nodiscard List *mergeConsecutiveVars(List *children);
static pg_nodiscard List *mergeConsecutiveGroups(List *children);
static pg_nodiscard List *mergeConsecutiveAlts(List *children);
static pg_nodiscard List *mergeGroupPrefixSuffix(List *children);
static RPRPatternNode *optimizeSeqPattern(RPRPatternNode *pattern);

static pg_nodiscard List *flattenAltChildren(List *children);
static pg_nodiscard List *removeDuplicateAlternatives(List *children);
static RPRPatternNode *optimizeAltPattern(RPRPatternNode *pattern);

static RPRPatternNode *tryMultiplyQuantifiers(RPRPatternNode *pattern);
static RPRPatternNode *tryUnwrapGroup(RPRPatternNode *pattern);
static RPRPatternNode *optimizeGroupPattern(RPRPatternNode *pattern);

static RPRPatternNode *optimizeRPRPattern(RPRPatternNode *pattern);
static void scanRPRPatternRecursive(RPRPatternNode *node, char **varNames,
									int *numVars, int *numElements,
									RPRDepth depth, RPRDepth *maxDepth);
static void scanRPRPattern(RPRPatternNode *node, char **varNames, int *numVars,
						   int *numElements, RPRDepth *maxDepth);
static RPRPattern *makeRPRPattern(int numVars, int numElements,
								  RPRDepth maxDepth, char **varNamesStack);
static RPRVarId getVarIdFromPattern(RPRPattern *pat, const char *varName);
static RPRElemFlags fillRPRPatternVar(RPRPatternNode *node, RPRPattern *pat,
									  int *idx, RPRDepth depth);
static RPRElemFlags fillRPRPatternGroup(RPRPatternNode *node, RPRPattern *pat,
										int *idx, RPRDepth depth);
static RPRElemFlags fillRPRPatternAlt(RPRPatternNode *node, RPRPattern *pat,
									  int *idx, RPRDepth depth);
static RPRElemFlags fillRPRPattern(RPRPatternNode *node, RPRPattern *pat,
								   int *idx, RPRDepth depth);
static void finalizeRPRPattern(RPRPattern *result);

static bool isFixedLengthChildren(RPRPattern *pattern, RPRElemIdx idx,
								  RPRDepth scopeDepth);
static bool isUnboundedStart(RPRPattern *pattern, RPRElemIdx idx);
static void computeAbsorbabilityRecursive(RPRPattern *pattern,
										  RPRElemIdx startIdx,
										  bool *hasAbsorbable);
static void computeAbsorbability(RPRPattern *pattern);

/*
 * rprPatternEqual
 *		Compare two RPRPatternNode trees for equality.
 *
 * Returns true if the trees are structurally identical.
 */
static bool
rprPatternEqual(RPRPatternNode *a, RPRPatternNode *b)
{
	/* Pattern nodes in children lists must never be NULL */
	Assert(a != NULL && b != NULL);

	/* Must have same node type and quantifiers */
	if (a->nodeType != b->nodeType)
		return false;
	if (a->min != b->min || a->max != b->max)
		return false;
	if (a->reluctant != b->reluctant)
		return false;

	switch (a->nodeType)
	{
		case RPR_PATTERN_VAR:
			return strcmp(a->varName, b->varName) == 0;

		case RPR_PATTERN_SEQ:
		case RPR_PATTERN_ALT:
		case RPR_PATTERN_GROUP:
			return rprPatternChildrenEqual(a->children, b->children);
	}

	pg_unreachable();
	return false;
}

/*
 * rprPatternChildrenEqual
 *		Compare children lists of two pattern nodes for equality.
 *
 * Returns true if the children lists are structurally identical.
 */
static bool
rprPatternChildrenEqual(List *a, List *b)
{
	ListCell   *lca,
			   *lcb;

	if (list_length(a) != list_length(b))
		return false;

	forboth(lca, a, lcb, b)
	{
		if (!rprPatternEqual((RPRPatternNode *) lfirst(lca),
							 (RPRPatternNode *) lfirst(lcb)))
			return false;
	}

	return true;
}

/*
 * rprNodeRowCount
 *		Rows the node always consumes, or -1 if that varies.
 *
 * Alternatives of equal length count as fixed: they may pick different
 * variables, but never different rows.
 */
static int64
rprNodeRowCount(RPRPatternNode *node)
{
	int64		len;

	check_stack_depth();

	switch (node->nodeType)
	{
		case RPR_PATTERN_VAR:
			if (node->min != node->max)
				return -1;
			return node->min;

		case RPR_PATTERN_ALT:
			len = -1;
			foreach_node(RPRPatternNode, branch, node->children)
			{
				int64		branchLen = rprNodeRowCount(branch);

				if (branchLen < 0 || (len >= 0 && branchLen != len))
					return -1;
				len = branchLen;
			}
			return len;

		case RPR_PATTERN_SEQ:
		case RPR_PATTERN_GROUP:
			len = rprBodyRowCount(node->children);
			if (len < 0)
				return -1;
			if (node->nodeType == RPR_PATTERN_SEQ)
				return len;
			if (node->min != node->max)
				return -1;
			len *= node->min;
			/* A count this large cannot arise from real rows anyway */
			if (len >= RPR_QUANTITY_INF)
				return -1;
			return len;
	}
	return -1;
}

/*
 * rprBodyRowCount
 *		Rows the children always consume in sequence, or -1 if that varies.
 */
static int64
rprBodyRowCount(List *children)
{
	int64		total = 0;

	foreach_node(RPRPatternNode, child, children)
	{
		int64		len = rprNodeRowCount(child);

		if (len < 0)
			return -1;
		total += len;

		/* A count this large cannot arise from real rows anyway */
		if (total >= RPR_QUANTITY_INF)
			return -1;
	}
	return total;
}

/*
 * rprBodyHasUniformLength
 *		Do the children always consume the same number of rows?
 */
static bool
rprBodyHasUniformLength(List *children)
{
	return rprBodyRowCount(children) >= 0;
}

/*
 * rprChildrenMatchAt
 *		Do the cells of children at [start, start + list_length(content))
 *		match content element for element?
 *
 * Returns false when that range does not lie inside children, so a caller
 * walking towards either end of the list can just ask.
 */
static bool
rprChildrenMatchAt(List *children, int start, List *content)
{
	int			offset = 0;

	if (start < 0 || start + list_length(content) > list_length(children))
		return false;

	foreach_node(RPRPatternNode, want, content)
	{
		RPRPatternNode *have;

		have = list_nth_node(RPRPatternNode, children, start + offset);
		if (!rprPatternEqual(have, want))
			return false;
		offset++;
	}

	return true;
}

/*
 * rprGroupContent
 *		The elements a GROUP stands for, as they appear in a sequence.
 *
 * A GROUP holds a single child, and a multi-element body arrives wrapped in a
 * SEQ, so unwrap that to compare against elements of the enclosing sequence:
 * (A B)+ holds the sequence A B.
 */
static List *
rprGroupContent(RPRPatternNode *group)
{
	List	   *content = group->children;

	if (list_length(content) == 1)
	{
		RPRPatternNode *inner = linitial_node(RPRPatternNode, content);

		if (inner->nodeType == RPR_PATTERN_SEQ)
			content = inner->children;
	}

	Assert(list_length(content) > 0);
	return content;
}

/*
 * rprTryAddIteration
 *		Raise a GROUP's quantifier by one iteration, if that is representable.
 *
 * An unbounded max stands for "no limit", not a count, so it is left alone and
 * nothing can overflow.  A finite bound has to stay below RPR_QUANTITY_INF:
 * one landing on the marker would read as unbounded.  Returns false without
 * touching the node when either bound has no room.
 */
static bool
rprTryAddIteration(RPRPatternNode *group)
{
	if (group->min >= RPR_QUANTITY_INF - 1)
		return false;
	if (group->max != RPR_QUANTITY_INF &&
		group->max >= RPR_QUANTITY_INF - 1)
		return false;

	group->min += 1;
	if (group->max != RPR_QUANTITY_INF)
		group->max += 1;
	return true;
}

/*
 * flattenSeqChildren
 *		Recursively optimize children and flatten nested SEQ.
 *
 * Example:
 *   SEQ(A, SEQ(B, C)) -> SEQ(A, B, C)
 *
 * Returns a new list with optimized children, with nested SEQ children
 * flattened into the parent list.  The helpers in this file follow two
 * conventions -- this one and flattenAltChildren() build a new list, since
 * either can end up longer than what it started with, while the rest compact
 * the cells they already have -- so a caller must always assign the return
 * value.
 */
static List *
flattenSeqChildren(List *children)
{
	List	   *newChildren = NIL;

	foreach_node(RPRPatternNode, child, children)
	{
		RPRPatternNode *opt = optimizeRPRPattern(child);

		/*
		 * GROUP{1,1} should have been unwrapped by optimizeGroupPattern;
		 * tryUnwrapGroup() does so regardless of reluctance.
		 */
		Assert(!(opt->nodeType == RPR_PATTERN_GROUP &&
				 opt->min == 1 && opt->max == 1));

		if (opt->nodeType == RPR_PATTERN_SEQ)
		{
			newChildren = list_concat(newChildren,
									  list_copy(opt->children));
		}
		else
		{
			newChildren = lappend(newChildren, opt);
		}
	}

	return newChildren;
}

/*
 * mergeConsecutiveVars
 *		Merge consecutive identical VAR nodes.
 *
 * Examples:
 *   A{m1,M1} A{m2,M2} -> A{m1+m2, M1+M2} where INF + x = INF.
 *
 * Only merges non-reluctant VAR nodes with the same variable name.
 */
static List *
mergeConsecutiveVars(List *children)
{
	int			writepos = 0;
	int			readpos = 0;

	while (readpos < list_length(children))
	{
		RPRPatternNode *node = list_nth_node(RPRPatternNode, children, readpos);
		int			runlen = 1;

		if (node->nodeType == RPR_PATTERN_VAR && !node->reluctant)
		{
			/* Fold the VARs that follow into node while they fit */
			while (readpos + runlen < list_length(children))
			{
				RPRPatternNode *other;
				int			newmin;
				int			newmax;

				other = list_nth_node(RPRPatternNode, children, readpos + runlen);

				if (other->nodeType != RPR_PATTERN_VAR)
					break;

				/*
				 * A greedy quantifier followed by a reluctant one over the
				 * same variable is not expressible as a single quantifier:
				 * the pair settles the first quantifier's count before the
				 * second one decides, which the standard's
				 * leftmost-choice-first rule (ISO/IEC TR 19075-5 7.2) makes
				 * observable.  Merging them would change the preferred match,
				 * so stop here.
				 */
				if (other->reluctant)
					break;

				if (strcmp(node->varName, other->varName) != 0)
					break;

				/*
				 * RPR_QUANTITY_INF means unbounded, not a count: a finite sum
				 * landing on it is representable, so reject it separately.
				 */
				if (node->max == RPR_QUANTITY_INF ||
					other->max == RPR_QUANTITY_INF)
					newmax = RPR_QUANTITY_INF;
				else if (pg_add_s32_overflow(node->max, other->max, &newmax) ||
						 newmax >= RPR_QUANTITY_INF)
					break;		/* fallback: leave the pair unmerged */

				if (pg_add_s32_overflow(node->min, other->min, &newmin) ||
					newmin >= RPR_QUANTITY_INF)
					break;		/* fallback: leave the pair unmerged */

				node->min = newmin;
				node->max = newmax;
				runlen++;
			}
		}

		/*
		 * Survivors are compacted towards the front.  writepos never passes
		 * readpos, so this cannot overwrite a cell still to be read.
		 */
		lfirst(list_nth_cell(children, writepos++)) = node;
		readpos += runlen;
	}

	return list_truncate(children, writepos);
}

/*
 * mergeConsecutiveGroups
 *		Merge consecutive identical GROUP nodes.
 *
 * Example:
 *   (A B)+ (A B)+ -> (A B){2,}
 *
 * Only merges non-reluctant GROUP nodes with identical children.
 *
 * The body must consume a fixed number of rows.  Otherwise the merge changes
 * which match is preferred: the two groups split the iterations between them,
 * and that split is a choice point the merged form does not have.  With a
 * fixed body, rows consumed rise in step with the iteration count, so both
 * forms reach the same rows in the same order; without one, they need not --
 * (A | B B)+ (A | B B)+ prefers a four-row match where (A | B B){2,} takes two.
 */
static List *
mergeConsecutiveGroups(List *children)
{
	int			writepos = 0;
	int			readpos = 0;

	while (readpos < list_length(children))
	{
		RPRPatternNode *node = list_nth_node(RPRPatternNode, children, readpos);
		int			runlen = 1;

		if (node->nodeType == RPR_PATTERN_GROUP && !node->reluctant)
		{
			/* Fold the GROUPs that follow into node while they fit */
			while (readpos + runlen < list_length(children))
			{
				RPRPatternNode *other;
				int			newmin;
				int			newmax;

				other = list_nth_node(RPRPatternNode, children, readpos + runlen);

				if (other->nodeType != RPR_PATTERN_GROUP || other->reluctant)
					break;

				if (!rprPatternChildrenEqual(node->children, other->children))
					break;

				/* The body must consume a fixed number of rows; see above */
				if (!rprBodyHasUniformLength(node->children))
					break;

				/*
				 * RPR_QUANTITY_INF means unbounded, not a count: a finite sum
				 * landing on it is representable, so reject it separately.
				 */
				if (node->max == RPR_QUANTITY_INF ||
					other->max == RPR_QUANTITY_INF)
					newmax = RPR_QUANTITY_INF;
				else if (pg_add_s32_overflow(node->max, other->max, &newmax) ||
						 newmax >= RPR_QUANTITY_INF)
					break;		/* fallback: leave the pair unmerged */

				if (pg_add_s32_overflow(node->min, other->min, &newmin) ||
					newmin >= RPR_QUANTITY_INF)
					break;		/* fallback: leave the pair unmerged */

				node->min = newmin;
				node->max = newmax;
				runlen++;
			}
		}

		/*
		 * Survivors are compacted towards the front.  writepos never passes
		 * readpos, so this cannot overwrite a cell still to be read.
		 */
		lfirst(list_nth_cell(children, writepos++)) = node;
		readpos += runlen;
	}

	return list_truncate(children, writepos);
}

/*
 * mergeConsecutiveAlts
 *		Merge consecutive identical ALT nodes into a GROUP.
 *
 * Example:
 *   (A | B) (A | B) (A | B) -> (A | B){3}
 *
 * After GROUP{1,1} unwrap, bare alternations like (A | B) become ALT nodes
 * in the SEQ.  This step detects consecutive identical ALT nodes and wraps
 * them in a GROUP with the appropriate quantifier.
 */
static List *
mergeConsecutiveAlts(List *children)
{
	int			writepos = 0;
	int			readpos = 0;

	while (readpos < list_length(children))
	{
		RPRPatternNode *node = list_nth_node(RPRPatternNode, children, readpos);
		int			count = 1;

		/* A quantifier never lands on an ALT, so none of these is reluctant */
		if (node->nodeType == RPR_PATTERN_ALT)
		{
			/* Count the run of ALTs identical to this one */
			while (readpos + count < list_length(children))
			{
				RPRPatternNode *other;

				other = list_nth_node(RPRPatternNode, children, readpos + count);

				if (!rprPatternEqual(node, other))
					break;

				count++;
			}

			if (count > 1)
			{
				/* Wrap the run into GROUP{count,count}(ALT) */
				RPRPatternNode *group = makeNode(RPRPatternNode);

				group->nodeType = RPR_PATTERN_GROUP;
				group->min = count;
				group->max = count;
				group->reluctant = false;
				group->location = -1;
				group->children = list_make1(node);
				node = group;
			}
		}

		/*
		 * Survivors are compacted towards the front.  writepos never passes
		 * readpos, so this cannot overwrite a cell still to be read.
		 */
		lfirst(list_nth_cell(children, writepos++)) = node;
		readpos += count;
	}

	return list_truncate(children, writepos);
}

/*
 * mergeGroupPrefixSuffix
 *		Merge sequence prefix/suffix into GROUP with matching children.
 *
 * When a GROUP's children appear as a prefix before and/or suffix after
 * the GROUP in a SEQ, merge them by incrementing the GROUP's quantifier.
 * This runs iteratively: A B A B (A B)+ A B -> (A B){4,}.
 *
 * Algorithm:
 *   For each GROUP encountered in the sequence:
 *   1. PREFIX phase: compare the last N survivors kept so far against the
 *      GROUP's children.  On match, drop them and increment the GROUP's
 *      min/max.  Repeat until no match.
 *   2. SUFFIX phase: compare the next N elements not yet read against the
 *      GROUP's children.  On match, skip them and increment min/max.
 *      Repeat until no match.
 *
 * Examples:
 *   A B (A B)+ -> (A B){2,}
 *   (A B)+ A B -> (A B){2,}
 *   A B (A B)+ A B -> (A B){3,}
 *
 * The two phases are not equally safe.  A prefix copy is mandatory and comes
 * before the group, exactly like the leading mandatory iterations it becomes,
 * so the decision trees stay isomorphic for any content.  A suffix copy comes
 * after the group has already decided to stop, which the merged form defers
 * until after the last iteration's own choices.  Merge a suffix only when the
 * content consumes a fixed number of rows, which leaves it nothing to decide;
 * see mergeConsecutiveGroups for why that condition is the right one.
 */
static List *
mergeGroupPrefixSuffix(List *children)
{
	int			numChildren;
	int			writepos;
	int			readpos;

	/*
	 * PREFIX phase.  Every copy that sits immediately before a GROUP is
	 * folded into it, over the whole sequence, before any suffix is
	 * considered.  A copy between two GROUPs is a suffix of the one before it
	 * and a prefix of the one after, and this order hands it to the second,
	 * which is the safer of the two rules: a mandatory copy before a group
	 * already sits where the leading iterations it becomes would sit, so
	 * folding it as a prefix holds for any content, while folding one as a
	 * suffix needs a fixed-length body.
	 *
	 * A prefix copy is one of the survivors already stored, so folding it is
	 * a step back of the write cursor.
	 */
	writepos = 0;
	numChildren = list_length(children);

	for (readpos = 0; readpos < numChildren; readpos++)
	{
		RPRPatternNode *child = list_nth_node(RPRPatternNode, children, readpos);

		if (child->nodeType == RPR_PATTERN_GROUP && !child->reluctant)
		{
			List	   *content = rprGroupContent(child);
			int			content_len = list_length(content);

			while (rprChildrenMatchAt(children, writepos - content_len, content) &&
				   rprTryAddIteration(child))
				writepos -= content_len;
		}

		/*
		 * Survivors are compacted towards the front.  writepos never passes
		 * readpos, so this cannot overwrite a cell still to be read.
		 */
		lfirst(list_nth_cell(children, writepos++)) = child;
	}

	children = list_truncate(children, writepos);

	/*
	 * SUFFIX phase.  A suffix copy comes after the GROUP has already decided
	 * to stop, which the merged form defers until after the last iteration's
	 * own choices, so the body must consume a fixed number of rows; see
	 * above.
	 *
	 * Such a copy is still unread, so folding it is a step forward of the
	 * read cursor.
	 */
	writepos = 0;
	readpos = 0;
	numChildren = list_length(children);

	while (readpos < numChildren)
	{
		RPRPatternNode *child = list_nth_node(RPRPatternNode, children, readpos);
		int			runlen = 1;

		if (child->nodeType == RPR_PATTERN_GROUP && !child->reluctant)
		{
			List	   *content = rprGroupContent(child);
			int			content_len = list_length(content);

			while (rprBodyHasUniformLength(content) &&
				   rprChildrenMatchAt(children, readpos + runlen, content) &&
				   rprTryAddIteration(child))
				runlen += content_len;
		}

		lfirst(list_nth_cell(children, writepos++)) = child;
		readpos += runlen;
	}

	return list_truncate(children, writepos);
}

/*
 * optimizeSeqPattern
 *		Optimize SEQ pattern node.
 *
 * Optimizations, in the order they run:
 *   1. Recursively optimize the children and flatten nested SEQ
 *   2. Merge consecutive identical VAR nodes
 *   3. Merge consecutive identical GROUP nodes
 *   4. Merge consecutive identical ALT nodes into GROUP
 *   5. Merge prefix/suffix into GROUP with matching children
 *   6. Merge consecutive identical GROUP nodes once more
 *   7. Unwrap single-item SEQ
 *
 * That order carries meaning: 1 is what optimizes the children, so every pass
 * after it sees a flat list of finished nodes, and 6 runs for the reason
 * given there.
 */
static RPRPatternNode *
optimizeSeqPattern(RPRPatternNode *pattern)
{
	pattern->children = flattenSeqChildren(pattern->children);
	pattern->children = mergeConsecutiveVars(pattern->children);
	pattern->children = mergeConsecutiveGroups(pattern->children);
	pattern->children = mergeConsecutiveAlts(pattern->children);
	pattern->children = mergeGroupPrefixSuffix(pattern->children);

	/*
	 * Two identical GROUPs can end up next to each other with nothing having
	 * put them side by side: the ALT merge wraps a run into a GROUP that may
	 * land beside an identical one, and folding a prefix or a suffix away can
	 * close the gap between two.  So the GROUP merge gets a second look.  One
	 * is enough: it only drops elements and raises quantifiers, so it creates
	 * no copy for the prefix/suffix pass to fold in turn.
	 */
	pattern->children = mergeConsecutiveGroups(pattern->children);

	/* Unwrap single-item SEQ: SEQ[A] -> A */
	if (list_length(pattern->children) == 1)
		return (RPRPatternNode *) linitial(pattern->children);

	return pattern;
}

/*
 * flattenAltChildren
 *		Recursively optimize children and flatten nested ALT nodes.
 *
 * Example:
 *   (A | (B | C)) -> (A | B | C)
 *
 * Splices each nested ALT's children into the parent list at the position the
 * ALT occupied, so the flattened alternatives keep their place.  Like
 * flattenSeqChildren(), this pass can end up longer than what it started
 * with, so it builds a new list rather than compacting the cells it has; the
 * caller must assign the result.
 */
static List *
flattenAltChildren(List *children)
{
	List	   *flattened = NIL;

	foreach_node(RPRPatternNode, child, children)
	{
		RPRPatternNode *optimized = optimizeRPRPattern(child);

		if (optimized->nodeType == RPR_PATTERN_ALT)
			flattened = list_concat(flattened, optimized->children);
		else
			flattened = lappend(flattened, optimized);
	}

	return flattened;
}

/*
 * removeDuplicateAlternatives
 *		Remove duplicate alternatives from a list.
 *
 * Examples:
 *   (A | B | A) -> (A | B)
 *   (X | Y | X | Z | Y) -> (X | Y | Z)
 *
 * Keeps the first of each and compacts the survivors towards the front of the
 * list it was given, so the caller must assign the truncated result.
 */
static List *
removeDuplicateAlternatives(List *children)
{
	int			writepos = 0;

	for (int readpos = 0; readpos < list_length(children); readpos++)
	{
		RPRPatternNode *node = list_nth_node(RPRPatternNode, children, readpos);
		bool		isDuplicate = false;

		/*
		 * Survivors are compacted towards the front, so those already kept
		 * are the cells below writepos.  writepos never passes readpos, so
		 * the store below cannot overwrite a cell still to be read.
		 */
		for (int keptpos = 0; keptpos < writepos; keptpos++)
		{
			if (rprPatternEqual(list_nth_node(RPRPatternNode, children, keptpos),
								node))
			{
				isDuplicate = true;
				break;
			}
		}

		if (!isDuplicate)
			lfirst(list_nth_cell(children, writepos++)) = node;
	}

	return list_truncate(children, writepos);
}

/*
 * optimizeAltPattern
 *		Optimize ALT pattern node.
 *
 * Optimizations:
 *   1. Flatten nested ALT
 *   2. Remove duplicate alternatives
 *   3. Unwrap single-item ALT
 */
static RPRPatternNode *
optimizeAltPattern(RPRPatternNode *pattern)
{
	/* Recursively optimize children and flatten nested ALT */
	pattern->children = flattenAltChildren(pattern->children);

	/* Remove duplicate alternatives */
	pattern->children = removeDuplicateAlternatives(pattern->children);

	/* Unwrap single-item ALT: ALT[A] -> A */
	if (list_length(pattern->children) == 1)
		return (RPRPatternNode *) linitial(pattern->children);

	return pattern;
}

/*
 * tryMultiplyQuantifiers
 *		Try to flatten (child{p,q}){m,n} into child{p*m, q*n}.
 *
 * Below, p,q are the child's {min,max} and m,n the outer {min,max}.
 *
 * Flattening is valid only when the repetition counts the nested quantifiers
 * can produce form exactly the contiguous interval [p*m, q*n].  For an outer
 * iteration count t (m <= t <= n) the child contributes any count in
 * [t*p, t*q], and t = 0 contributes {0}.  The union of those intervals is
 * contiguous, hence flattenable, when:
 *
 *   - m == n: a single outer count, so the result is just [m*p, m*q]; or
 *   - p == 0: every interval starts at 0, so they all overlap; or
 *   - consecutive intervals touch and the zero case (if any) connects:
 *       p <= Max(m,1)*(q-p) + 1   (touch; trivially true if q is unbounded)
 *       and (m >= 1 or p <= 1)    (when m == 0, {0} must reach [p,q])
 *
 * Otherwise gaps appear and the pattern is left unflattened: (A{2}){2,3}
 * yields {4,6} (not 4..6), and (A{2,})* yields {0} UNION [2,INF) (not
 * [0,INF), so A* would wrongly admit a single A).
 *
 * Contiguity settles the set of counts, not which count is preferred, so a
 * further condition is needed; see the comment on safe below.
 *
 * Returns the child node with multiplied quantifiers if successful,
 * otherwise returns the original pattern unchanged.
 */
static RPRPatternNode *
tryMultiplyQuantifiers(RPRPatternNode *pattern)
{
	RPRPatternNode *child;
	bool		safe;
	int32		newmin;
	int32		newmax;

	/* Parser always creates GROUP with exactly one child */
	Assert(list_length(pattern->children) == 1);

	if (pattern->reluctant)
		return pattern;

	child = (RPRPatternNode *) linitial(pattern->children);

	if ((child->nodeType != RPR_PATTERN_VAR &&
		 child->nodeType != RPR_PATTERN_GROUP) ||
		child->reluctant)
		return pattern;

	/*
	 * Flattening erases the outer block boundaries, so how the child's
	 * iterations split across blocks must not matter.  A fixed-length body
	 * ensures that -- splits with the same total span the same rows -- as
	 * does an exact child quantifier, which admits only one split.  Otherwise
	 * preferment shifts: ((A | B B){1,2}){2} would become (A | B B){2,4},
	 * which stops at two A's where the nested form prefers A (B B) A A.
	 *
	 * A VAR child has no body to measure, and its own quantifier already
	 * settles this, so the test applies to a GROUP child only.
	 */
	if (child->min != child->max &&
		child->nodeType == RPR_PATTERN_GROUP &&
		!rprBodyHasUniformLength(child->children))
		return pattern;

	/*
	 * Decide whether the achievable counts form one contiguous interval.  The
	 * child quantifier is {child->min, child->max} and the outer one is
	 * {pattern->min, pattern->max}; either max may be RPR_QUANTITY_INF.
	 */
	if (pattern->min == pattern->max || child->min == 0)
		safe = true;
	else
	{
		bool		touch;
		bool		zero_ok;
		bool		order_ok;

		/*
		 * Consecutive intervals [t*min, t*max] and [(t+1)*min, (t+1)*max]
		 * touch when (t+1)*min <= t*max + 1, i.e. min <= t*(max-min) + 1.
		 * This is tightest at the smallest t in play, Max(pattern->min, 1).
		 * An unbounded child->max makes every interval reach INF, so they
		 * always touch.
		 */
		if (child->max == RPR_QUANTITY_INF)
			touch = true;
		else
			touch = ((int64) child->min <=
					 (int64) Max(pattern->min, 1) * (child->max - child->min) + 1);

		/*
		 * A skippable outer (min 0) also needs {0} adjacent to the child
		 * range.
		 */
		zero_ok = (pattern->min >= 1 || child->min <= 1);

		/*
		 * Contiguity is necessary but not sufficient: the flattened form must
		 * prefer the same match too.  The nested form settles the first
		 * iteration's count before iterating again, so it stops early when
		 * the tail cannot reach the child's lower bound -- (A{2,3}){1,2}
		 * prefers three rows where the flattened A{2,6} takes four.  Only a
		 * bounded lower bound >= 2 can be undershot.
		 */
		order_ok = (child->min <= 1 || child->max == RPR_QUANTITY_INF);

		safe = touch && zero_ok && order_ok;
	}

	if (!safe)
		return pattern;

	/* Flatten the child quantifier, declining the rewrite if it does not fit */
	if (pg_mul_s32_overflow(pattern->min, child->min, &newmin) ||
		newmin >= RPR_QUANTITY_INF)
		return pattern;

	/*
	 * RPR_QUANTITY_INF means unbounded, not a count: a finite product landing
	 * on it is representable, so reject it separately.
	 */
	if (pattern->max == RPR_QUANTITY_INF || child->max == RPR_QUANTITY_INF)
		newmax = RPR_QUANTITY_INF;
	else if (pg_mul_s32_overflow(pattern->max, child->max, &newmax) ||
			 newmax >= RPR_QUANTITY_INF)
		return pattern;

	child->min = newmin;
	child->max = newmax;
	return child;
}

/*
 * tryUnwrapGroup
 *		Try to unwrap GROUP{1,1} node.
 *
 * Examples:
 *   (A){1,1}   -> A
 *   (A B){1,1} -> SEQ(A, B)  (unwraps the inner SEQ)
 *   (A)?       -> A?         (propagate quantifier to single VAR child)
 *   (A)+?      -> A+?        (propagate quantifier including reluctant)
 *
 * If GROUP has min=1, max=1, return the child directly (reluctant on
 * {1,1} is meaningless).  If GROUP has a single VAR child with default
 * quantifier {1,1}, propagate the GROUP's quantifier to the child and
 * unwrap.  Otherwise returns the pattern unchanged.
 *
 * Note: Parser always creates GROUP with exactly one child via list_make1().
 */
static RPRPatternNode *
tryUnwrapGroup(RPRPatternNode *pattern)
{
	RPRPatternNode *child;

	/* Parser always creates GROUP with single child */
	Assert(list_length(pattern->children) == 1);

	child = (RPRPatternNode *) linitial(pattern->children);

	/* GROUP{1,1}: unwrap directly (reluctant on {1,1} is meaningless) */
	if (pattern->min == 1 && pattern->max == 1)
		return child;

	/*
	 * Single VAR child with default {1,1}: propagate GROUP's quantifier to
	 * the child and unwrap.  E.g., (A)?? -> A??, (A)+? -> A+?
	 */
	if (child->nodeType == RPR_PATTERN_VAR &&
		child->min == 1 && child->max == 1)
	{
		child->min = pattern->min;
		child->max = pattern->max;
		child->reluctant = pattern->reluctant;
		return child;
	}

	return pattern;
}

/*
 * optimizeGroupPattern
 *		Optimize GROUP pattern node.
 *
 * Optimizations:
 *   1. Quantifier multiplication: (A{m}){n} -> A{m*n}
 *   2. Unwrap GROUP{1,1}
 */
static RPRPatternNode *
optimizeGroupPattern(RPRPatternNode *pattern)
{
	ListCell   *lc;
	RPRPatternNode *result;

	/* Recursively optimize children */
	foreach(lc, pattern->children)
	{
		lfirst(lc) = optimizeRPRPattern((RPRPatternNode *) lfirst(lc));
	}

	/* Try quantifier multiplication */
	result = tryMultiplyQuantifiers(pattern);
	if (result != pattern)
		return result;

	/* Try unwrapping GROUP{1,1} */
	return tryUnwrapGroup(pattern);
}

/*
 * optimizeRPRPattern
 *		Optimize RPRPatternNode tree (dispatcher).
 *
 * Dispatches to type-specific optimization functions.
 * Returns the optimized pattern (may be a different node).
 */
static RPRPatternNode *
optimizeRPRPattern(RPRPatternNode *pattern)
{
	RPRPatternNode *result = pattern;

	/* Pattern nodes from parser are never NULL */
	Assert(pattern != NULL);

	check_stack_depth();

	/*
	 * A fixed count leaves reluctance nothing to decide.  Drop it here: the
	 * merge and multiplication rewrites below decline a reluctant node, so
	 * {n,n}? would otherwise miss what {n,n} gets.
	 */
	if (pattern->min == pattern->max)
		pattern->reluctant = false;

	switch (pattern->nodeType)
	{
		case RPR_PATTERN_VAR:
			break;
		case RPR_PATTERN_SEQ:
			result = optimizeSeqPattern(pattern);
			break;
		case RPR_PATTERN_ALT:
			result = optimizeAltPattern(pattern);
			break;
		case RPR_PATTERN_GROUP:
			result = optimizeGroupPattern(pattern);
			break;
	}

	/* Again: a rewrite may have produced a fixed count of its own */
	if (result->min == result->max)
		result->reluctant = false;

	return result;
}

/*
 * scanRPRPatternRecursive
 *		Recursively scan pattern parse tree (pass 1 internal).
 *
 * Collects unique variable names and counts elements while tracking depth.
 * Variables from DEFINE clause are already in varNames; this adds any
 * additional variables found in the pattern.
 */
static void
scanRPRPatternRecursive(RPRPatternNode *node, char **varNames, int *numVars,
						int *numElements, RPRDepth depth, RPRDepth *maxDepth)
{
	int			i;

	/* Pattern nodes from parser are never NULL */
	Assert(node != NULL);

	check_stack_depth();

	/* Check recursion depth limit before overflow occurs */
	if (depth >= RPR_DEPTH_MAX)
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("pattern nesting too deep"),
				errdetail("Pattern nesting depth %d exceeds maximum %d.",
						  depth, RPR_DEPTH_MAX - 1));

	/* Track maximum depth */
	*maxDepth = Max(*maxDepth, depth);

	switch (node->nodeType)
	{
		case RPR_PATTERN_VAR:
			/* Count element */
			(*numElements)++;

			/* Collect variable name if not already present */
			for (i = 0; i < *numVars; i++)
			{
				if (strcmp(varNames[i], node->varName) == 0)
					return;		/* Already have this variable */
			}

			/*
			 * Variable not in DEFINE clause - this is valid per ISO/IEC
			 * 19075-5 Feature R020.  Such variables are implicitly TRUE. Add
			 * to varNames so they get a varId >= the number of DEFINE clause
			 * expressions, which executor treats as TRUE.
			 */
			Assert(*numVars <= RPR_VARID_MAX);
			varNames[(*numVars)++] = node->varName;
			break;

		case RPR_PATTERN_SEQ:
			/* Sequence: just recurse into children */
			foreach_node(RPRPatternNode, child, node->children)
			{
				scanRPRPatternRecursive(child, varNames,
										numVars, numElements, depth, maxDepth);
			}
			break;

		case RPR_PATTERN_GROUP:

			/*
			 * Add BEGIN element if group has non-trivial quantifier (not
			 * {1,1})
			 */
			if (node->min != 1 || node->max != 1)
				(*numElements)++;

			/* Recurse into children at increased depth */
			foreach_node(RPRPatternNode, child, node->children)
			{
				scanRPRPatternRecursive(child, varNames,
										numVars, numElements, depth + 1, maxDepth);
			}

			/* Add END element if group has non-trivial quantifier (not {1,1}) */
			if (node->min != 1 || node->max != 1)
				(*numElements)++;
			break;

		case RPR_PATTERN_ALT:
			/* Count ALT start element */
			(*numElements)++;

			/* Recurse into children at increased depth */
			foreach_node(RPRPatternNode, child, node->children)
			{
				/* Each branch is terminated by a SEP branch-separator marker */
				(*numElements)++;
				scanRPRPatternRecursive(child, varNames,
										numVars, numElements, depth + 1, maxDepth);
			}
			break;
	}
}

/*
 * scanRPRPattern
 *		Scan pattern parse tree (pass 1 entry point).
 *
 * Collects unique variable names (appending to those from DEFINE clause),
 * counts total elements (including FIN marker), and tracks maximum depth.
 * Reports error if element count exceeds RPR_ELEMIDX_MAX.
 */
static void
scanRPRPattern(RPRPatternNode *node, char **varNames, int *numVars,
			   int *numElements, RPRDepth *maxDepth)
{
	*numElements = 0;
	*maxDepth = 0;

	scanRPRPatternRecursive(node, varNames, numVars, numElements, 0, maxDepth);

	(*numElements)++;			/* +1 for FIN marker */

	if (*numElements > RPR_ELEMIDX_MAX)
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("pattern too complex"),
				errdetail("Pattern has %d elements, maximum is %d.",
						  *numElements, RPR_ELEMIDX_MAX));
}

/*
 * makeRPRPattern
 *		Allocate and initialize RPRPattern structure.
 *
 * Creates the pattern structure, copies variable names, and allocates
 * the elements array. The elements array is zero-initialized.
 */
static RPRPattern *
makeRPRPattern(int numVars, int numElements, RPRDepth maxDepth,
			   char **varNamesStack)
{
	RPRPattern *result;
	int			i;

	result = makeNode(RPRPattern);
	result->numVars = numVars;

	/* depth < RPR_DEPTH_MAX, so maxDepth + 1 does not exceed RPR_DEPTH_MAX. */
	Assert(maxDepth < RPR_DEPTH_MAX);
	result->maxDepth = maxDepth + 1;	/* +1: depth is 0-based */
	result->numElements = numElements;

	/* Copy varNames (pattern must have at least one variable) */
	Assert(numVars > 0);
	result->varNames = palloc_array(char *, numVars);
	for (i = 0; i < numVars; i++)
		result->varNames[i] = pstrdup(varNamesStack[i]);

	/* Allocate elements array (zero-init for reserved fields) */
	Assert(numElements >= 2);
	result->elements = palloc0_array(RPRPatternElement, numElements);

	return result;
}

/*
 * getVarIdFromPattern
 *		Get variable ID for a variable name from RPRPattern.
 *
 * Returns the index of the variable in the varNames array.
 */
static RPRVarId
getVarIdFromPattern(RPRPattern *pat, const char *varName)
{
	for (int i = 0; i < pat->numVars; i++)
	{
		if (strcmp(pat->varNames[i], varName) == 0)
			return (RPRVarId) i;
	}

	/* Should not happen - variable should already be collected */
	elog(ERROR, "pattern variable \"%s\" not found", varName);
	pg_unreachable();
}

/*
 * fillRPRPatternVar
 *		Fill a VAR pattern element.
 *
 * Returns the empty-match flags for this VAR: RPR_ELEM_EMPTY_LOOP when it is
 * nullable (min 0, can match zero rows), plus RPR_ELEM_EMPTY_PREFERRED when its
 * preferred derivation is the empty one.  A bare variable consumes a row; only
 * a reluctant quantifier that may take zero repetitions prefers to skip it.
 */
static RPRElemFlags
fillRPRPatternVar(RPRPatternNode *node, RPRPattern *pat, int *idx, RPRDepth depth)
{
	RPRPatternElement *elem = &pat->elements[*idx];
	RPRElemFlags flags = 0;

	memset(elem, 0, sizeof(RPRPatternElement));
	elem->varId = getVarIdFromPattern(pat, node->varName);
	elem->depth = depth;
	elem->min = node->min;
	elem->max = node->max;
	Assert(elem->min >= 0 && elem->min < RPR_QUANTITY_INF &&
		   elem->max >= 1 && elem->min <= elem->max);
	elem->next = RPR_ELEMIDX_INVALID;
	elem->jump = RPR_ELEMIDX_INVALID;
	if (node->reluctant)
		elem->flags |= RPR_ELEM_RELUCTANT;
	(*idx)++;

	if (node->min == 0)
	{
		/* nullable; a reluctant quantifier also prefers the empty match */
		flags |= RPR_ELEM_EMPTY_LOOP;
		if (node->reluctant)
			flags |= RPR_ELEM_EMPTY_PREFERRED;
	}
	return flags;
}

/*
 * fillRPRPatternGroup
 *		Fill a GROUP pattern and its children.
 *
 * Creates elements for group content at increased depth, plus BEGIN/END
 * marker pair if the group has a non-trivial quantifier (not {1,1}).
 *
 * Element layout for (A B){2,3}:
 *
 *   [BEGIN]  [A]  [B]  [END]  [next element...]
 *     |                  |          ^
 *     |                  +-- jump --+ (loop back to first child)
 *     +---- jump -------------------+ (skip to after END)
 *
 * BEGIN.jump points past END (the skip path taken when min == 0; a count is
 * only tested at END, so a BEGIN never takes it for reaching max).
 * END.jump points to the first child (loop-back path).
 * BEGIN.next and END.next are set later by finalizeRPRPattern().
 *
 * Returns the group's empty-match flags.  RPR_ELEM_EMPTY_LOOP is set when the
 * group is nullable -- its min is 0 (can be skipped entirely) or its body is
 * nullable (every path through the body can match zero rows).
 * RPR_ELEM_EMPTY_PREFERRED is set when the group's preferred derivation is
 * empty: a reluctant min-0 group prefers to take no iteration, and otherwise
 * the group follows its body.  The END element inherits the body's bits.
 */
static RPRElemFlags
fillRPRPatternGroup(RPRPatternNode *node, RPRPattern *pat, int *idx, RPRDepth depth)
{
	int			groupStartIdx = *idx;
	int			beginIdx = -1;
	RPRElemFlags bodyFlags = RPR_ELEM_EMPTY_LOOP | RPR_ELEM_EMPTY_PREFERRED;
	RPRElemFlags result;

	/* Add BEGIN marker if group has non-trivial quantifier (not {1,1}) */
	if (node->min != 1 || node->max != 1)
	{
		RPRPatternElement *elem = &pat->elements[*idx];

		beginIdx = *idx;
		memset(elem, 0, sizeof(RPRPatternElement));
		elem->varId = RPR_VARID_BEGIN;
		elem->depth = depth;
		elem->min = node->min;
		elem->max = node->max;
		Assert(elem->min >= 0 && elem->min < RPR_QUANTITY_INF &&
			   elem->max >= 1 && elem->min <= elem->max);
		elem->next = RPR_ELEMIDX_INVALID;	/* set by finalize */
		elem->jump = RPR_ELEMIDX_INVALID;	/* set after END */
		if (node->reluctant)
			elem->flags |= RPR_ELEM_RELUCTANT;
		(*idx)++;
		groupStartIdx = *idx;	/* children start after BEGIN */
	}

	/* A concatenation is nullable / empty-preferred iff every child is (AND) */
	foreach_node(RPRPatternNode, child, node->children)
		bodyFlags &= fillRPRPattern(child, pat, idx, depth + 1);

	/* Add group end marker if group has non-trivial quantifier (not {1,1}) */
	if (node->min != 1 || node->max != 1)
	{
		RPRPatternElement *beginElem = &pat->elements[beginIdx];
		RPRPatternElement *endElem = &pat->elements[*idx];

		memset(endElem, 0, sizeof(RPRPatternElement));
		endElem->varId = RPR_VARID_END;
		endElem->depth = depth;
		endElem->min = node->min;
		endElem->max = node->max;
		Assert(endElem->min >= 0 && endElem->min < RPR_QUANTITY_INF &&
			   endElem->max >= 1 && endElem->min <= endElem->max);
		endElem->next = RPR_ELEMIDX_INVALID;
		endElem->jump = groupStartIdx;	/* loop to first child */
		if (node->reluctant)
			endElem->flags |= RPR_ELEM_RELUCTANT;

		/* The END carries the body's bits, not the group's; see README IV-4b */
		endElem->flags |= bodyFlags;

		(*idx)++;

		/* Set BEGIN skip pointer (next is set by finalize) */
		beginElem->jump = *idx; /* skip: go to after END */
	}

	result = bodyFlags;
	if (node->min == 0)
	{
		/* skippable entirely; if reluctant, the group prefers to skip */
		result |= RPR_ELEM_EMPTY_LOOP;
		if (node->reluctant)
			result |= RPR_ELEM_EMPTY_PREFERRED;
	}
	return result;
}

/*
 * fillRPRPatternAlt
 *		Fill an ALT pattern and its alternatives.
 *
 * Creates the ALT marker and fills each alternative at increased depth,
 * terminating every alternative (including the last) with a SEP
 * branch-separator marker.  The branch link runs from ALT through the SEP
 * chain, never through the branch content: a branch's first element may
 * itself be a group BEGIN, whose jump is that group's skip-past-END path.
 *
 *   ALT.next  -> first branch content      SEP.next -> next branch content
 *   ALT.jump  -> first SEP                            (post-ALT on the last)
 *   SEP.jump  -> next SEP (-1 on the last)
 *
 * SEP is a marker, never a state: a branch's tail and a branch-terminal
 * group's BEGIN skip are redirected past the alternation.
 *
 * Returns the alternation's empty-match flags.  RPR_ELEM_EMPTY_LOOP is set if
 * any branch is nullable (OR: one nullable branch suffices).
 * RPR_ELEM_EMPTY_PREFERRED follows the first branch alone: lexicographic order
 * makes it the preferred one, so whether the later branches prefer empty does
 * not matter.
 */
static RPRElemFlags
fillRPRPatternAlt(RPRPatternNode *node, RPRPattern *pat, int *idx, RPRDepth depth)
{
	ListCell   *lc;
	ListCell   *lc2;
	RPRPatternElement *elem;
	int			altIdx = *idx;
	List	   *altBranchStarts = NIL;
	List	   *altEndPositions = NIL;
	int			afterAltIdx;
	RPRElemFlags altFlags = 0;	/* OR of branch EMPTY_LOOP bits */
	RPRElemFlags firstFlags = 0;	/* first branch's flags (for
									 * EMPTY_PREFERRED) */
	bool		firstBranch = true;

	/* Add alternation start marker */
	elem = &pat->elements[*idx];
	memset(elem, 0, sizeof(RPRPatternElement));
	elem->varId = RPR_VARID_ALT;
	elem->depth = depth;
	elem->min = 1;
	elem->max = 1;
	elem->next = RPR_ELEMIDX_INVALID;
	elem->jump = RPR_ELEMIDX_INVALID;
	(*idx)++;

	/* ALT enters the first branch's content (the contiguous next element) */
	pat->elements[altIdx].next = *idx;

	/* Fill each alternative, terminated by a SEP branch-separator marker */
	foreach_node(RPRPatternNode, alt, node->children)
	{
		int			branchStart = *idx;
		RPRPatternElement *sep;
		RPRElemFlags branchFlags;

		altBranchStarts = lappend_int(altBranchStarts, branchStart);
		branchFlags = fillRPRPattern(alt, pat, idx, depth + 1);

		/* nullable if ANY branch is; empty-preferred per the FIRST branch */
		altFlags |= (branchFlags & RPR_ELEM_EMPTY_LOOP);
		if (firstBranch)
		{
			firstFlags = branchFlags;
			firstBranch = false;
		}
		altEndPositions = lappend_int(altEndPositions, *idx - 1);

		/* SEP terminates this branch, so it sits at branch end + 1 */
		sep = &pat->elements[*idx];
		memset(sep, 0, sizeof(RPRPatternElement));
		sep->varId = RPR_VARID_SEP;
		sep->depth = depth;		/* ALT-level boundary, not branch-level */
		sep->min = 1;
		sep->max = 1;
		sep->next = RPR_ELEMIDX_INVALID;
		sep->jump = RPR_ELEMIDX_INVALID;
		(*idx)++;
	}

	afterAltIdx = *idx;

	/* ALT reaches the first branch's terminating SEP */
	pat->elements[altIdx].jump = linitial_int(altEndPositions) + 1;

	/*
	 * Wire the SEP chain, and redirect each branch's exits past the
	 * alternation.
	 */
	forboth(lc, altBranchStarts, lc2, altEndPositions)
	{
		int			branchStart = lfirst_int(lc);
		int			endPos = lfirst_int(lc2);
		int			sepIdx = endPos + 1;
		ListCell   *nextEnd = lnext(altEndPositions, lc2);
		int			elemIdx;

		/* SEP.jump -> next SEP; SEP.next -> next branch content */
		if (nextEnd != NULL)
		{
			pat->elements[sepIdx].jump = lfirst_int(nextEnd) + 1;
			pat->elements[sepIdx].next = lfirst_int(lnext(altBranchStarts, lc));
		}
		else
		{
			pat->elements[sepIdx].jump = RPR_ELEMIDX_INVALID;
			pat->elements[sepIdx].next = afterAltIdx;
		}

		/*
		 * Redirect the branch's fall-through exit to after the alternation.
		 * The natural exit is the element past the branch content, which is
		 * this branch's SEP; a simple tail leaves next unset (finalize would
		 * fall it through to the SEP), while an inner ALT already set next to
		 * that position.
		 */
		if (pat->elements[endPos].next != RPR_ELEMIDX_INVALID)
		{
			int			oldTarget = pat->elements[endPos].next;

			for (elemIdx = branchStart; elemIdx <= endPos; elemIdx++)
			{
				if (pat->elements[elemIdx].next == oldTarget)
					pat->elements[elemIdx].next = afterAltIdx;
			}
		}
		else
		{
			pat->elements[endPos].next = afterAltIdx;
		}

		/*
		 * A branch-terminal group's BEGIN skip-past-END path points at the
		 * element following the group, which is this branch's SEP; redirect
		 * it past the alternation too so the skip never lands on a SEP.
		 */
		for (elemIdx = branchStart; elemIdx <= endPos; elemIdx++)
		{
			if (pat->elements[elemIdx].jump == sepIdx)
				pat->elements[elemIdx].jump = afterAltIdx;
		}
	}

	list_free(altBranchStarts);
	list_free(altEndPositions);

	return altFlags | (firstFlags & RPR_ELEM_EMPTY_PREFERRED);
}

/*
 * fillRPRPattern
 *		Fill pattern elements array from parse tree (pass 2).
 *
 * Recursively traverses the parse tree and populates pre-allocated elements
 * array.
 * Dispatches to type-specific fill functions.
 *
 * Returns the pattern's empty-match flags (RPR_ELEM_EMPTY_LOOP for nullable,
 * RPR_ELEM_EMPTY_PREFERRED for empty-preferred).  For a SEQ, a concatenation is
 * nullable / empty-preferred only when every child is, so the children's flags
 * are AND-ed together.
 */
static RPRElemFlags
fillRPRPattern(RPRPatternNode *node, RPRPattern *pat, int *idx, RPRDepth depth)
{
	/* Pattern nodes from parser are never NULL */
	Assert(node != NULL);

	check_stack_depth();

	switch (node->nodeType)
	{
		case RPR_PATTERN_SEQ:
			{
				RPRElemFlags flags = RPR_ELEM_EMPTY_LOOP | RPR_ELEM_EMPTY_PREFERRED;

				foreach_node(RPRPatternNode, child, node->children)
					flags &= fillRPRPattern(child, pat, idx, depth);
				return flags;
			}

		case RPR_PATTERN_VAR:
			return fillRPRPatternVar(node, pat, idx, depth);

		case RPR_PATTERN_GROUP:
			return fillRPRPatternGroup(node, pat, idx, depth);

		case RPR_PATTERN_ALT:
			return fillRPRPatternAlt(node, pat, idx, depth);
	}

	pg_unreachable();
	return 0;
}

/*
 * finalizeRPRPattern
 *		Finalize pattern structure after filling elements.
 *
 * This performs:
 *   1. Initialize absorption flag to false
 *   2. Set up next pointers for sequential flow
 *   3. Add FIN marker at the end
 */
static void
finalizeRPRPattern(RPRPattern *result)
{
	int			finIdx = result->numElements - 1;
	int			i;
	RPRPatternElement *finElem;

	/* Initialize absorption flag */
	result->isAbsorbable = false;

	/* Set up next pointers for elements that don't have one */
	for (i = 0; i < finIdx; i++)
	{
		RPRPatternElement *elem = &result->elements[i];

		if (elem->next == RPR_ELEMIDX_INVALID)
			elem->next = (i < finIdx - 1) ? i + 1 : finIdx;

		/* Verify quantifier range is valid */
		Assert(elem->min >= 0 && elem->min < RPR_QUANTITY_INF &&
			   elem->max >= 1 && elem->min <= elem->max);
	}

	/* Add FIN marker at the end */
	finElem = &result->elements[finIdx];
	memset(finElem, 0, sizeof(RPRPatternElement));
	finElem->varId = RPR_VARID_FIN;
	finElem->depth = 0;
	finElem->min = 1;
	finElem->max = 1;
	finElem->next = RPR_ELEMIDX_INVALID;
	finElem->jump = RPR_ELEMIDX_INVALID;
}

/*-------------------------------------------------------------------------
 * CONTEXT ABSORPTION: TWO-FLAG DESIGN
 *-------------------------------------------------------------------------
 *
 * Context absorption eliminates redundant match searches by absorbing newer
 * contexts that cannot produce longer matches than older contexts. This
 * achieves O(n^2) -> O(n) performance improvement for patterns like A+ B.
 *
 * Core Insight:
 *   For pattern A+ B, if Ctx1 starts at row 0 and Ctx2 starts at row 1,
 *   both matching A continuously, Ctx1 will always have more A matches.
 *   When B finally appears, Ctx1's match (0 to current) is always longer
 *   than Ctx2's match (1 to current). So Ctx2 can be safely eliminated.
 *
 * Two Flags:
 *   1. RPR_ELEM_ABSORBABLE - "Absorption comparison point"
 *      WHERE contexts can be compared for absorption.
 *      - Simple unbounded VAR (A+): the VAR element itself
 *      - Unbounded GROUP ((A B)+): the END element only
 *
 *   2. RPR_ELEM_ABSORBABLE_BRANCH - "Absorbable region marker"
 *      ALL elements within the absorbable region.
 *      - Used for tracking state.isAbsorbable at runtime
 *      - States leaving this region become non-absorbable permanently
 *
 * Why Two Flags?
 *   For pattern "(A B)+", contexts at different positions (one at A,
 *   another at B) cannot be compared - they must synchronize at END.
 *
 *   Example: "(A B)+" with input A B A B A B...
 *     Row 0 (A): Ctx1 starts, matches A
 *     Row 1 (B): Ctx1 matches B -> END (count=1)
 *     Row 2 (A): Ctx1 loops to A, Ctx2 starts at A
 *     Row 3 (B): Ctx1 at END (count=2), Ctx2 at END (count=1)
 *                -> Both at END, comparable! Ctx1 absorbs Ctx2.
 *
 *   Contexts synchronize at END every group-length rows. Therefore:
 *   - ABSORBABLE marks END as comparison point (where to compare)
 *   - ABSORBABLE_BRANCH keeps state.isAbsorbable=true through A->B->END
 *
 * Pattern Examples:
 *
 *   Pattern: A+ B
 *   Element 0 (A): ABSORBABLE | ABSORBABLE_BRANCH  <- comparison point
 *   Element 1 (B): (none)
 *   -> Compare at A every row. When contexts move to B, absorption stops.
 *
 *   Pattern: (A B)+ C
 *   Element 0 (BEGIN): ABSORBABLE_BRANCH
 *   Element 1 (A): ABSORBABLE_BRANCH
 *   Element 2 (B): ABSORBABLE_BRANCH
 *   Element 3 (END): ABSORBABLE | ABSORBABLE_BRANCH  <- comparison point
 *   Element 4 (C): (none)
 *   -> Compare at END every 2 rows. When contexts move to C, absorption stops.
 *
 *   Pattern: (A+ B+)+ C
 *   Element 0 (BEGIN): ABSORBABLE_BRANCH
 *   Element 1 (A): ABSORBABLE | ABSORBABLE_BRANCH  <- comparison point
 *   Element 2 (B): (none)
 *   Element 3 (END): (none)
 *   Element 4 (C): (none)
 *   -> Compare at A during the first iteration. After moving to B+,
 *      absorption stops.
 *
 * First Unbounded Portion Strategy:
 *   Along one path the algorithm only flags the FIRST unbounded portion
 *   starting from element 0; an alternation is walked branch by branch, so
 *   each branch may contribute one (A+ | B+ gives both). This is sufficient
 *   because:
 *   - Absorption in first portion already achieves O(n) complexity
 *   - Later portions have different synchronization characteristics
 *   - Nested unbounded patterns are too complex for simple absorption
 *   - Complex patterns (nested groups, etc.) naturally die from mismatch
 *
 * Runtime Usage (in execRPR.c):
 *   - state.isAbsorbable = (previous && elem.ABSORBABLE_BRANCH)
 *   - Monotonic: once false, stays false (cannot re-enter region)
 *   - context.hasAbsorbableState: can absorb others (>=1 absorbable state)
 *   - context.allStatesAbsorbable: can be absorbed (ALL states absorbable)
 *   - Absorption check: if Ctx1.hasAbsorbable && Ctx2.allAbsorbable,
 *     compare counts at same elemIdx, absorb if Ctx1.count >= Ctx2.count
 *
 *-------------------------------------------------------------------------
 */

/*
 * isFixedLengthChildren
 *		Check if all children at scopeDepth have fixed-length quantifiers
 *		(min == max), recursively for nested subgroups.
 *
 * A fixed-length group is semantically equivalent to unrolling each child
 * to {1,1} copies, which is the existing Case 2 already proven correct
 * for absorption.  This check recognizes fixed-length groups at compile
 * time without actually unrolling them.
 *
 * Traverses the flat element array starting at idx.  For VAR elements,
 * checks min == max.  For BEGIN elements (nested subgroups), recurses
 * into the subgroup and also checks the subgroup's END quantifier.
 * ALT elements are rejected (alternation inside absorbable group is
 * not supported).
 *
 * Returns true if all children are fixed-length, stopping at the END
 * element at scopeDepth - 1.
 */
static bool
isFixedLengthChildren(RPRPattern *pattern, RPRElemIdx idx, RPRDepth scopeDepth)
{
	RPRPatternElement *e = &pattern->elements[idx];

	check_stack_depth();

	while (e->depth == scopeDepth)
	{
		if (RPRElemIsVar(e))
		{
			if (e->min != e->max)
				return false;
		}
		else if (RPRElemIsBegin(e))
		{
			RPRElemIdx	childIdx = e->next;

			/* Recurse into subgroup children at scopeDepth + 1 */
			if (!isFixedLengthChildren(pattern, childIdx, scopeDepth + 1))
				return false;

			/* Advance past the subgroup to its END element */
			e = &pattern->elements[e->next];
			while (e->depth > scopeDepth)
				e = &pattern->elements[e->next];

			/* e is now the END at scopeDepth; check its quantifier */
			Assert(RPRElemIsEnd(e) && e->depth == scopeDepth);
			if (e->min != e->max)
				return false;
		}
		else
		{
			/* ALT inside group: not supported for absorption */
			return false;
		}

		Assert(e->next != RPR_ELEMIDX_INVALID);
		e = &pattern->elements[e->next];
	}

	return true;
}

/*
 * isUnboundedStart
 *		Check if the element at idx starts an unbounded greedy sequence.
 *
 * For context absorption to work, the sequence starting at idx must be:
 *   - Unbounded (max = infinity)
 *   - Greedy (not reluctant)
 *   - At the start of current scope
 *
 * Two cases are handled:
 *   1. Simple VAR: A+ B C - A has max=INF, gets both flags
 *   2. Unbounded GROUP with fixed-length children: (A B{2})+ C
 *      All children must have min == max (recursively for nested subgroups).
 *      This is equivalent to unrolling to {1,1} VARs, e.g., (A B B)+ C.
 *      All elements within the group get ABSORBABLE_BRANCH.
 *      Only the unbounded END gets ABSORBABLE (comparison point).
 *
 *      In the examples below, "step" is the number of VARs in one fully
 *      unrolled iteration of the group (its fixed per-iteration length).
 *      Examples:
 *        (A B{2})+ C          - B{2} has min==max, step=3
 *        (A (B C){2} D)+ E    - nested {2} subgroup, step=6
 *        ((A (B C){2}){2})+   - doubly nested {2}, step=10
 *        (A ((B C{3}){2} D){2} E)+ F  - deep nesting, step=20
 *
 * Returns false for patterns where absorption cannot work:
 *   - A B+ (unbounded not at start)
 *   - A+? B (the unbounded quantifier itself is reluctant)
 *   - (A | B)+ (ALT inside group)
 *   - (A B+)+ (variable-length element inside group)
 *   - (A B{2,5})+ (min != max inside group)
 *
 * The reluctance test covers only the quantifier examined here.  A
 * reluctant quantifier on an enclosing group -- (A+)??, where A+ itself
 * is greedy -- is rejected at that group's BEGIN by
 * computeAbsorbabilityRecursive(), before this function is reached.
 */
static bool
isUnboundedStart(RPRPattern *pattern, RPRElemIdx idx)
{
	RPRPatternElement *elem = &pattern->elements[idx];
	RPRDepth	startDepth = elem->depth;
	RPRPatternElement *e;

	/* Case 1: Simple unbounded VAR at start (greedy only) */
	if (RPRElemIsVar(elem) && elem->max == RPR_QUANTITY_INF &&
		!RPRElemIsReluctant(elem))
	{
		/* Set both flags on first element */
		elem->flags |= RPR_ELEM_ABSORBABLE_BRANCH | RPR_ELEM_ABSORBABLE;
		return true;
	}

	/*
	 * Case 2: Unbounded GROUP with fixed-length children.  Each child must
	 * have min == max (recursively for nested subgroups), ensuring a fixed
	 * step size per iteration so that count-dominance holds.
	 */
	if (!isFixedLengthChildren(pattern, idx, startDepth))
		return false;

	/*
	 * Find the END that closes the group beginning at idx, at startDepth - 1.
	 * FIN bounds the walk: depth alone cannot when startDepth is 0, and FIN's
	 * next is RPR_ELEMIDX_INVALID, so the walk would read outside the array.
	 * Only a tree optimizeRPRPattern() did not produce reaches it that way.
	 */
	e = &pattern->elements[idx];
	while (e->depth >= startDepth && !RPRElemIsFin(e))
		e = &pattern->elements[e->next];

	/* END must be unbounded greedy */
	if (e->depth == startDepth - 1 &&
		RPRElemIsEnd(e) && e->max == RPR_QUANTITY_INF &&
		!RPRElemIsReluctant(e))
	{
		Assert(e->jump == idx); /* END points back to first child */

		/* Set ABSORBABLE_BRANCH on all children, ABSORBABLE on END only */
		for (e = elem; !RPRElemIsEnd(e) || e->depth >= startDepth;
			 e = &pattern->elements[e->next])
			e->flags |= RPR_ELEM_ABSORBABLE_BRANCH;
		e->flags |= RPR_ELEM_ABSORBABLE_BRANCH | RPR_ELEM_ABSORBABLE;
		return true;
	}

	return false;
}

/*
 * computeAbsorbabilityRecursive
 *		Recursively check absorbability starting from given index.
 *
 * If the element at startIdx is ALT, recursively checks each branch
 * independently.  Each branch gets its own absorbability status, and if
 * any branch is absorbable, the ALT element itself is marked with
 * RPR_ELEM_ABSORBABLE_BRANCH.
 *
 * If BEGIN, skips to first child -- but only when the group's own
 * quantifier is greedy.  Absorption assumes an earlier context subsumes a
 * later one, which a reluctant group inverts; isUnboundedStart() sees only
 * the quantifier it is handed, so the greedy A+ in (A+)?? needs this check.
 *
 * Otherwise (VAR), checks if the element starts an unbounded sequence via
 * isUnboundedStart.
 */
static void
computeAbsorbabilityRecursive(RPRPattern *pattern, RPRElemIdx startIdx,
							  bool *hasAbsorbable)
{
	RPRPatternElement *elem = &pattern->elements[startIdx];

	check_stack_depth();

	if (RPRElemIsAlt(elem))
	{
		/* ALT: recursively check each branch via the SEP chain */
		RPRElemIdx	branchStart = elem->next;
		RPRElemIdx	sepIdx = elem->jump;

		while (sepIdx != RPR_ELEMIDX_INVALID)
		{
			RPRPatternElement *sepElem;
			bool		branchAbsorbable = false;

			/* Recursively check this branch's content */
			computeAbsorbabilityRecursive(pattern, branchStart,
										  &branchAbsorbable);
			if (branchAbsorbable)
				*hasAbsorbable = true;

			Assert(sepIdx >= 0 && sepIdx < pattern->numElements);
			sepElem = &pattern->elements[sepIdx];
			Assert(RPRElemIsSep(sepElem));

			/* The last branch's SEP has no link, ending the walk */
			branchStart = sepElem->next;
			sepIdx = sepElem->jump;
		}

		/* Mark ALT element if any branch is absorbable */
		if (*hasAbsorbable)
			elem->flags |= RPR_ELEM_ABSORBABLE_BRANCH;
	}
	else if (RPRElemIsBegin(elem))
	{
		/*
		 * Not an absorbable region.  The group's quantifier sits on its BEGIN
		 * as well as its END, so this element answers for the group.
		 */
		if (RPRElemIsReluctant(elem))
			return;

		/*
		 * BEGIN: first try to treat this BEGIN's children as an unbounded
		 * group directly (handles nested fixed-length groups like ((A{2}
		 * B{3}){2})+).  If that fails, skip to first child and recurse as
		 * before.
		 */
		if (isUnboundedStart(pattern, elem->next))
		{
			*hasAbsorbable = true;
			elem->flags |= RPR_ELEM_ABSORBABLE_BRANCH;
		}
		else
		{
			computeAbsorbabilityRecursive(pattern, elem->next, hasAbsorbable);

			/* Mark BEGIN element if contents are absorbable */
			if (*hasAbsorbable)
				elem->flags |= RPR_ELEM_ABSORBABLE_BRANCH;
		}
	}
	else
	{
		/* Should never reach END - structural invariant of pattern parse tree */
		Assert(!RPRElemIsEnd(elem));

		/* Non-ALT, non-BEGIN: check if unbounded start */
		if (isUnboundedStart(pattern, startIdx))
			*hasAbsorbable = true;
	}
}

/*
 * computeAbsorbability
 *		Determine if pattern supports context absorption optimization.
 *
 * Context absorption eliminates redundant match searches by absorbing
 * newer contexts that cannot produce longer matches than older contexts.
 * This achieves O(n^2) -> O(n) performance improvement.
 *
 * Only greedy unbounded quantifiers at pattern start can be absorbable.
 * Reluctant quantifiers are excluded because they don't maintain monotonic
 * decrease property required for safe absorption -- both the unbounded
 * quantifier itself and any group quantifier enclosing it.
 *
 * This function sets two flags:
 *   RPR_ELEM_ABSORBABLE: Absorption comparison point
 *     - Simple unbounded VAR: the VAR itself (e.g., A in A+)
 *     - Unbounded GROUP: the END element (e.g., END in (A B)+)
 *   RPR_ELEM_ABSORBABLE_BRANCH: All elements in absorbable region
 *     - Simple unbounded VAR: the VAR itself only
 *     - Unbounded GROUP: the whole body (including nested subgroups) and the
 *       group's END, plus any enclosing BEGIN/ALT on the path to it
 *
 * Examples:
 *   A+ B C         - absorbable (A gets both flags)
 *   (A B)+ C       - absorbable (BEGIN,A,B,END get BRANCH, END gets ABSORBABLE)
 *   A B+           - NOT absorbable (unbounded not at start)
 *   A+? B C        - NOT absorbable (reluctant quantifier)
 *   (A+ B+)+       - only first A+ on first iteration (nested unbounded not supported)
 *   A+ | B+        - both branches absorbable independently
 *   A+ | C D       - only A+ branch absorbable (C D branch not absorbable)
 *   ((A+ B) | C) D - nested ALT: A+ branch is absorbable
 */
static void
computeAbsorbability(RPRPattern *pattern)
{
	bool		hasAbsorbable = false;

	/* Parser always produces at least one element + FIN */
	Assert(pattern->numElements >= 2);

	/* Start recursion from first element */
	computeAbsorbabilityRecursive(pattern, 0, &hasAbsorbable);
	pattern->isAbsorbable = hasAbsorbable;
}

/*
 * buildRPRPattern
 *		Compile pattern parse tree to flat bytecode array.
 *
 * Compilation phases:
 *   1. Optimize parse tree (flatten, merge, deduplicate)
 *   2. Scan: collect variables, count elements (pass 1)
 *   3. Allocate result structure
 *   4. Fill elements from parse tree (pass 2)
 *   5. Finalize pattern structure
 *   6. Compute context absorption eligibility
 *
 * Called from createplan.c during plan creation.
 */
RPRPattern *
buildRPRPattern(RPRPatternNode *pattern, List *defineClause,
				RPSkipTo rpSkipTo, int frameOptions,
				bool hasMatchStartDependent)
{
	RPRPattern *result;
	RPRPatternNode *optimized;
	char	   *varNamesStack[RPR_VARID_MAX + 1];
	int			numVars;
	int			numElements;
	RPRDepth	maxDepth;
	int			idx;

	/* Caller must check for NULL pattern before calling */
	Assert(pattern != NULL);
	/* RPR is ROWS-only: transformRPR() rejects RANGE/GROUPS up front */
	Assert(frameOptions & FRAMEOPTION_ROWS);

	/* Optimize the pattern tree */
	optimized = optimizeRPRPattern(copyObject(pattern));

	numVars = 0;

	/*
	 * Populate varNamesStack with the DEFINE variable names in DEFINE order.
	 * This ensures varId == defineClause index, eliminating runtime mapping.
	 */
	foreach_node(TargetEntry, te, defineClause)
	{
		/* Parser always assigns a name to each DEFINE entry */
		Assert(te->resname != NULL);

		varNamesStack[numVars++] = pstrdup(te->resname);
	}

	/* Scan pattern: collect variables, count elements, validate limits */
	scanRPRPattern(optimized, varNamesStack, &numVars, &numElements, &maxDepth);

	/*
	 * numVars may reach RPR_VARID_MAX + 1 (valid varIds are 0 ..
	 * RPR_VARID_MAX)
	 */
	Assert(numVars <= RPR_VARID_MAX + 1);

	/* Allocate result structure */
	result = makeRPRPattern(numVars, numElements, maxDepth, varNamesStack);

	/* Fill elements (pass 2) */
	idx = 0;
	fillRPRPattern(optimized, result, &idx, 0);

	/* Finalize: set up next pointers, flags, and add FIN marker */
	finalizeRPRPattern(result);

	/*
	 * Compute context absorption eligibility. Absorption requires both
	 * structural absorbability and runtime conditions. Check runtime
	 * conditions first to avoid unnecessary pattern analysis.
	 *
	 * Runtime conditions for absorption:
	 *
	 * 1. SKIP TO PAST LAST ROW required (not SKIP TO NEXT ROW): with NEXT
	 * ROW, matches overlap and every row must report its own match, so
	 * absorption (sharing one result) is not semantically possible.  A
	 * completed context does linger until its own start row is queried; that
	 * is the inherent cost of per-row match reporting, not redundancy
	 * absorption could remove.
	 *
	 * 2. Unbounded frame end required (not ROWS with bounded end): With a
	 * bounded frame (e.g., ROWS BETWEEN CURRENT ROW AND 10 FOLLOWING),
	 * matches may be truncated at frame boundaries. This changes the
	 * absorption semantics - older contexts don't necessarily produce longer
	 * matches when frame limits apply differently to each context.
	 *
	 * 3. No DEFINE may depend on match_start: such a variable is evaluated
	 * against the start of its own match, so two contexts that differ only in
	 * where they started can classify the same row differently and the older
	 * one no longer covers the newer.
	 */
	if (rpSkipTo == ST_PAST_LAST_ROW &&
		(frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING) &&
		!hasMatchStartDependent)
	{
		/* Runtime conditions met - check structural absorbability */
		computeAbsorbability(result);
	}

	return result;
}
