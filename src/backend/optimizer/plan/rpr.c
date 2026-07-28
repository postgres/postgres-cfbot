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

#include "catalog/pg_proc.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/rpr.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"

/* Forward declarations */
static bool rprPatternEqual(RPRPatternNode *a, RPRPatternNode *b);
static bool rprPatternChildrenEqual(List *a, List *b);
static List *flattenSeqChildren(List *children);
static List *mergeConsecutiveVars(List *children);
static List *mergeConsecutiveGroups(List *children);
static List *mergeConsecutiveAlts(List *children);
static List *mergeGroupPrefixSuffix(List *children);
static RPRPatternNode *optimizeSeqPattern(RPRPatternNode *pattern);

static List *flattenAltChildren(List *children);
static List *removeDuplicateAlternatives(List *children);
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
static bool fillRPRPatternVar(RPRPatternNode *node, RPRPattern *pat,
							  int *idx, RPRDepth depth);
static bool fillRPRPatternGroup(RPRPatternNode *node, RPRPattern *pat,
								int *idx, RPRDepth depth);
static bool fillRPRPatternAlt(RPRPatternNode *node, RPRPattern *pat,
							  int *idx, RPRDepth depth);
static bool fillRPRPattern(RPRPatternNode *node, RPRPattern *pat,
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
 * flattenSeqChildren
 *		Recursively optimize children and flatten nested SEQ.
 *
 * Example:
 *   SEQ(A, SEQ(B, C)) -> SEQ(A, B, C)
 *
 * Returns a new list with optimized children, with nested SEQ children
 * flattened into the parent list.
 */
static List *
flattenSeqChildren(List *children)
{
	List	   *newChildren = NIL;

	foreach_node(RPRPatternNode, child, children)
	{
		RPRPatternNode *opt = optimizeRPRPattern(child);

		/* GROUP{1,1} should have been unwrapped by optimizeGroupPattern */
		Assert(!(opt->nodeType == RPR_PATTERN_GROUP &&
				 opt->min == 1 && opt->max == 1 && opt->reluctant == false));

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
	List	   *mergedChildren = NIL;
	RPRPatternNode *prev = NULL;

	foreach_node(RPRPatternNode, child, children)
	{
		if (child->nodeType == RPR_PATTERN_VAR && child->reluctant == false)
		{
			/* ----------------------
			 * Can merge consecutive VAR nodes if:
			 * 1. Same variable name
			 * 2. No min overflow: prev->min + child->min < INF
			 * 3. No max overflow: prev->max + child->max < INF (or either is INF)
			 *
			 * Strict <: a sum equal to INF would alias the unbounded sentinel
			 * (min must stay finite; a finite max must not become INF).
			 */
			if (prev != NULL &&
				strcmp(prev->varName, child->varName) == 0 &&
				prev->min < RPR_QUANTITY_INF - child->min &&
				(prev->max < RPR_QUANTITY_INF - child->max ||
				 prev->max == RPR_QUANTITY_INF ||
				 child->max == RPR_QUANTITY_INF))
			{
				/*
				 * Merge: accumulate min/max into prev. prev is guaranteed to
				 * be a non-reluctant VAR by the outer condition.
				 */
				Assert(prev->nodeType == RPR_PATTERN_VAR && prev->reluctant == false);

				prev->min += child->min;

				if (prev->max == RPR_QUANTITY_INF ||
					child->max == RPR_QUANTITY_INF)
					prev->max = RPR_QUANTITY_INF;
				else
					prev->max += child->max;
			}
			else
			{
				/* Flush previous and start new */
				if (prev != NULL)
					mergedChildren = lappend(mergedChildren, prev);
				prev = child;
			}
		}
		else
		{
			/* Non-mergeable - flush previous */
			if (prev != NULL)
				mergedChildren = lappend(mergedChildren, prev);
			mergedChildren = lappend(mergedChildren, child);
			prev = NULL;
		}
	}

	/* Flush remaining */
	if (prev != NULL)
		mergedChildren = lappend(mergedChildren, prev);

	return mergedChildren;
}

/*
 * mergeConsecutiveGroups
 *		Merge consecutive identical GROUP nodes.
 *
 * Example:
 *   (A B)+ (A B)+ -> (A B){2,}
 *
 * Only merges non-reluctant GROUP nodes with identical children.
 */
static List *
mergeConsecutiveGroups(List *children)
{
	List	   *mergedChildren = NIL;
	RPRPatternNode *prev = NULL;

	foreach_node(RPRPatternNode, child, children)
	{
		if (child->nodeType == RPR_PATTERN_GROUP && child->reluctant == false)
		{
			/* ----------------------
			 * Can merge consecutive GROUP nodes if:
			 * 1. Identical children
			 * 2. No min overflow: prev->min + child->min < INF
			 * 3. No max overflow: prev->max + child->max < INF (or either is INF)
			 *
			 * Strict <: a sum equal to INF would alias the unbounded sentinel
			 * (min must stay finite; a finite max must not become INF).
			 */
			if (prev != NULL &&
				rprPatternChildrenEqual(prev->children, child->children) &&
				prev->min < RPR_QUANTITY_INF - child->min &&
				(prev->max < RPR_QUANTITY_INF - child->max ||
				 prev->max == RPR_QUANTITY_INF ||
				 child->max == RPR_QUANTITY_INF))
			{
				/*
				 * Merge: accumulate min/max into prev. prev is guaranteed to
				 * be a non-reluctant GROUP by the outer condition.
				 */
				Assert(prev->nodeType == RPR_PATTERN_GROUP && prev->reluctant == false);

				prev->min += child->min;

				if (prev->max == RPR_QUANTITY_INF ||
					child->max == RPR_QUANTITY_INF)
					prev->max = RPR_QUANTITY_INF;
				else
					prev->max += child->max;
			}
			else
			{
				/* Flush previous and start new */
				if (prev != NULL)
					mergedChildren = lappend(mergedChildren, prev);
				prev = child;
			}
		}
		else
		{
			/* Non-mergeable - flush previous */
			if (prev != NULL)
				mergedChildren = lappend(mergedChildren, prev);
			mergedChildren = lappend(mergedChildren, child);
			prev = NULL;
		}
	}

	/* Flush remaining */
	if (prev != NULL)
		mergedChildren = lappend(mergedChildren, prev);

	return mergedChildren;
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
	List	   *mergedChildren = NIL;
	RPRPatternNode *prev = NULL;
	int			count = 0;

	foreach_node(RPRPatternNode, child, children)
	{
		if (child->nodeType == RPR_PATTERN_ALT && child->reluctant == false)
		{
			if (prev != NULL &&
				rprPatternChildrenEqual(prev->children, child->children))
			{
				/* Same ALT as prev - accumulate */
				count++;
			}
			else
			{
				/* Different ALT or first ALT - flush previous */
				if (prev != NULL)
				{
					if (count > 1)
					{
						/* Wrap in GROUP{count,count}(ALT) */
						RPRPatternNode *group = makeNode(RPRPatternNode);

						group->nodeType = RPR_PATTERN_GROUP;
						group->min = count;
						group->max = count;
						group->reluctant = false;
						group->location = -1;
						group->children = list_make1(prev);
						mergedChildren = lappend(mergedChildren, group);
					}
					else
						mergedChildren = lappend(mergedChildren, prev);
				}
				prev = child;
				count = 1;
			}
		}
		else
		{
			/* Non-ALT - flush previous */
			if (prev != NULL)
			{
				if (count > 1)
				{
					RPRPatternNode *group = makeNode(RPRPatternNode);

					group->nodeType = RPR_PATTERN_GROUP;
					group->min = count;
					group->max = count;
					group->reluctant = false;
					group->location = -1;
					group->children = list_make1(prev);
					mergedChildren = lappend(mergedChildren, group);
				}
				else
					mergedChildren = lappend(mergedChildren, prev);
			}
			mergedChildren = lappend(mergedChildren, child);
			prev = NULL;
			count = 0;
		}
	}

	/* Flush remaining */
	if (prev != NULL)
	{
		if (count > 1)
		{
			RPRPatternNode *group = makeNode(RPRPatternNode);

			group->nodeType = RPR_PATTERN_GROUP;
			group->min = count;
			group->max = count;
			group->reluctant = false;
			group->location = -1;
			group->children = list_make1(prev);
			mergedChildren = lappend(mergedChildren, group);
		}
		else
			mergedChildren = lappend(mergedChildren, prev);
	}

	return mergedChildren;
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
 *   1. PREFIX phase: compare the last N elements already in the result
 *      list against the GROUP's children.  On match, remove them from
 *      result and increment the GROUP's min/max.  Repeat until no match.
 *   2. SUFFIX phase: compare the next N elements in the input against
 *      the GROUP's children.  On match, skip them (via skipUntil) and
 *      increment min/max.  Repeat until no match.
 *
 * Examples:
 *   A B (A B)+ -> (A B){2,}
 *   (A B)+ A B -> (A B){2,}
 *   A B (A B)+ A B -> (A B){3,}
 */
static List *
mergeGroupPrefixSuffix(List *children)
{
	List	   *result = NIL;
	int			numChildren = list_length(children);
	int			i;
	int			skipUntil = -1; /* skip suffix elements already merged */

	for (i = 0; i < numChildren; i++)
	{
		RPRPatternNode *child = (RPRPatternNode *) list_nth(children, i);

		/*
		 * The suffix merge logic below adjusts i to skip merged elements,
		 * ensuring we never revisit them. Verify this invariant.
		 */
		Assert(i >= skipUntil);

		/*
		 * If this is a GROUP, see if preceding/following elements match its
		 * children. GROUP's content may be wrapped in a SEQ - unwrap for
		 * comparison.
		 */
		if (child->nodeType == RPR_PATTERN_GROUP && child->reluctant == false)
		{
			List	   *groupContent = child->children;
			int			groupChildCount;
			int			prefixLen = list_length(result);
			List	   *trimmed;

			/*
			 * If GROUP has single SEQ child, compare with SEQ's children.
			 * e.g., (A B)+ internally contains sequence A B; compare against
			 * that.
			 */
			if (list_length(groupContent) == 1)
			{
				RPRPatternNode *inner = (RPRPatternNode *) linitial(groupContent);

				if (inner->nodeType == RPR_PATTERN_SEQ)
					groupContent = inner->children;
			}

			groupChildCount = list_length(groupContent);

			/*
			 * PREFIX MERGE: Check if preceding elements match. Keep merging
			 * as long as we have matching prefixes.
			 */
			while (prefixLen >= groupChildCount && groupChildCount > 0)
			{
				List	   *prefixElements = NIL;
				int			j;

				/* Extract last groupChildCount elements from prefix */
				for (j = prefixLen - groupChildCount; j < prefixLen; j++)
				{
					prefixElements = lappend(prefixElements,
											 list_nth(result, j));
				}

				/* Compare with GROUP's (possibly unwrapped) children */
				if (rprPatternChildrenEqual(prefixElements, groupContent) &&
					child->min < RPR_QUANTITY_INF - 1 &&
					(child->max == RPR_QUANTITY_INF ||
					 child->max < RPR_QUANTITY_INF - 1))
				{
					/*
					 * Match! Merge by incrementing GROUP's quantifier. Remove
					 * the prefix elements from output.
					 */
					child->min += 1;
					if (child->max != RPR_QUANTITY_INF)
						child->max += 1;

					/* Rebuild result without matched prefix */
					trimmed = NIL;
					for (j = 0; j < prefixLen - groupChildCount; j++)
					{
						trimmed = lappend(trimmed,
										  list_nth(result, j));
					}
					result = trimmed;
					prefixLen = list_length(result);
				}
				else
				{
					list_free(prefixElements);
					break;
				}

				list_free(prefixElements);
			}

			/*
			 * SUFFIX MERGE: Check if following elements match. Keep merging
			 * as long as we have matching suffixes.
			 */
			while (i + groupChildCount < numChildren && groupChildCount > 0)
			{
				List	   *suffixElements = NIL;
				int			j;
				int			suffixStart = i + 1;

				/* suffixStart always >= skipUntil after i adjustment */
				Assert(skipUntil <= suffixStart);

				/* Extract next groupChildCount elements as suffix */
				for (j = 0; j < groupChildCount; j++)
				{
					int			idx = suffixStart + j;

					/* while condition guarantees idx < numChildren */
					Assert(idx < numChildren);
					suffixElements = lappend(suffixElements,
											 list_nth(children, idx));
				}

				/* Compare with GROUP's children */
				if (list_length(suffixElements) == groupChildCount &&
					rprPatternChildrenEqual(suffixElements, groupContent) &&
					child->min < RPR_QUANTITY_INF - 1 &&
					(child->max == RPR_QUANTITY_INF ||
					 child->max < RPR_QUANTITY_INF - 1))
				{
					/*
					 * Match! Merge suffix by incrementing quantifier and
					 * skipping.
					 */
					child->min += 1;
					if (child->max != RPR_QUANTITY_INF)
						child->max += 1;
					skipUntil = suffixStart + groupChildCount;

					/*
					 * Update i to continue suffix check after merged elements
					 */
					i = skipUntil - 1;
				}
				else
				{
					list_free(suffixElements);
					break;
				}

				list_free(suffixElements);
			}
		}

		result = lappend(result, child);
	}

	return result;
}

/*
 * optimizeSeqPattern
 *		Optimize SEQ pattern node.
 *
 * Optimizations:
 *   1. Flatten nested SEQ and GROUP{1,1}
 *   2. Merge consecutive identical VAR nodes
 *   3. Merge consecutive identical GROUP nodes
 *   4. Merge consecutive identical ALT nodes into GROUP
 *   5. Merge prefix/suffix into GROUP with matching children
 *   6. Unwrap single-item SEQ
 */
static RPRPatternNode *
optimizeSeqPattern(RPRPatternNode *pattern)
{
	/* Recursively optimize children and flatten nested SEQ/GROUP{1,1} */
	pattern->children = flattenSeqChildren(pattern->children);

	/* Merge consecutive identical VAR nodes */
	pattern->children = mergeConsecutiveVars(pattern->children);

	/* Merge consecutive identical GROUP nodes */
	pattern->children = mergeConsecutiveGroups(pattern->children);

	/* Merge consecutive identical ALT nodes into GROUP */
	pattern->children = mergeConsecutiveAlts(pattern->children);

	/* Merge prefix/suffix into GROUP with matching children */
	pattern->children = mergeGroupPrefixSuffix(pattern->children);

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
 * Returns a new list with optimized children, with nested ALT children
 * flattened into the parent list.
 */
static List *
flattenAltChildren(List *children)
{
	List	   *newChildren = NIL;

	foreach_node(RPRPatternNode, child, children)
	{
		RPRPatternNode *opt = optimizeRPRPattern(child);

		if (opt->nodeType == RPR_PATTERN_ALT)
			newChildren = list_concat(newChildren, list_copy(opt->children));
		else
			newChildren = lappend(newChildren, opt);
	}

	return newChildren;
}

/*
 * removeDuplicateAlternatives
 *		Remove duplicate alternatives from a list.
 *
 * Examples:
 *   (A | B | A) -> (A | B)
 *   (X | Y | X | Z | Y) -> (X | Y | Z)
 *
 * Returns a new list with only unique children (first occurrence kept).
 */
static List *
removeDuplicateAlternatives(List *children)
{
	List	   *uniqueChildren = NIL;

	foreach_node(RPRPatternNode, child, children)
	{
		bool		isDuplicate = false;

		foreach_node(RPRPatternNode, uchild, uniqueChildren)
		{
			if (rprPatternEqual(uchild, child))
			{
				isDuplicate = true;
				break;
			}
		}

		if (!isDuplicate)
			uniqueChildren = lappend(uniqueChildren, child);
	}

	return uniqueChildren;
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
 * Returns the child node with multiplied quantifiers if successful,
 * otherwise returns the original pattern unchanged.
 */
static RPRPatternNode *
tryMultiplyQuantifiers(RPRPatternNode *pattern)
{
	RPRPatternNode *child;
	bool		safe;
	int64		new_min_64;
	int64		new_max_64;

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

		safe = touch && zero_ok;
	}

	if (!safe)
		return pattern;

	/* Flatten the child quantifier, guarding against overflow. */
	new_min_64 = (int64) pattern->min * child->min;
	if (new_min_64 >= RPR_QUANTITY_INF)
		return pattern;			/* overflow, skip optimization */

	if (pattern->max == RPR_QUANTITY_INF || child->max == RPR_QUANTITY_INF)
		new_max_64 = RPR_QUANTITY_INF;
	else
	{
		new_max_64 = (int64) pattern->max * child->max;
		if (new_max_64 >= RPR_QUANTITY_INF)
			return pattern;
	}

	child->min = (int) new_min_64;
	child->max = (int) new_max_64;
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
		child->min == 1 && child->max == 1 && child->reluctant == false)
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
	List	   *newChildren;
	RPRPatternNode *result;

	/* Recursively optimize children */
	newChildren = NIL;
	foreach_node(RPRPatternNode, child, pattern->children)
	{
		newChildren = lappend(newChildren, optimizeRPRPattern(child));
	}
	pattern->children = newChildren;

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
	/* Pattern nodes from parser are never NULL */
	Assert(pattern != NULL);

	check_stack_depth();

	switch (pattern->nodeType)
	{
		case RPR_PATTERN_VAR:
			return pattern;
		case RPR_PATTERN_SEQ:
			return optimizeSeqPattern(pattern);
		case RPR_PATTERN_ALT:
			return optimizeAltPattern(pattern);
		case RPR_PATTERN_GROUP:
			return optimizeGroupPattern(pattern);
	}

	pg_unreachable();
	return pattern;
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
			 * to varNames so they get a varId >= defineVariableList length,
			 * which executor treats as TRUE.
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
 * Returns true if this VAR is nullable (can match zero rows).
 */
static bool
fillRPRPatternVar(RPRPatternNode *node, RPRPattern *pat, int *idx, RPRDepth depth)
{
	RPRPatternElement *elem = &pat->elements[*idx];

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

	return (node->min == 0);
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
 * BEGIN.jump points past END (skip path when count >= max or min == 0).
 * END.jump points to the first child (loop-back path).
 * BEGIN.next and END.next are set later by finalizeRPRPattern().
 *
 * Returns true if this group is nullable.  A group is nullable when its
 * min is 0 (can be skipped entirely) or its body is nullable (every path
 * through the body can match zero rows).
 */
static bool
fillRPRPatternGroup(RPRPatternNode *node, RPRPattern *pat, int *idx, RPRDepth depth)
{
	int			groupStartIdx = *idx;
	int			beginIdx = -1;
	bool		bodyNullable = true;

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

	foreach_node(RPRPatternNode, child, node->children)
	{
		if (!fillRPRPattern(child, pat, idx, depth + 1))
			bodyNullable = false;
	}

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

		/*
		 * If the group body is nullable, mark the END element so that
		 * nfa_advance_end can fast-forward the iteration count to min when
		 * reached via empty-match skip paths.
		 */
		if (bodyNullable)
			endElem->flags |= RPR_ELEM_EMPTY_LOOP;

		(*idx)++;

		/* Set BEGIN skip pointer (next is set by finalize) */
		beginElem->jump = *idx; /* skip: go to after END */
	}

	return (node->min == 0 || bodyNullable);
}

/*
 * fillRPRPatternAlt
 *		Fill an ALT pattern and its alternatives.
 *
 * Creates the ALT marker, fills each alternative at increased depth,
 * sets jump pointers for backtracking, and next pointers for successful paths.
 *
 * Returns true if any branch is nullable (OR semantics: one nullable
 * branch suffices for the alternation to produce an empty match).
 */
static bool
fillRPRPatternAlt(RPRPatternNode *node, RPRPattern *pat, int *idx, RPRDepth depth)
{
	ListCell   *lc;
	ListCell   *lc2;
	RPRPatternElement *elem;
	List	   *altBranchStarts = NIL;
	List	   *altEndPositions = NIL;
	int			afterAltIdx;
	bool		anyNullable = false;

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

	/* Fill each alternative */
	foreach_node(RPRPatternNode, alt, node->children)
	{
		int			branchStart = *idx;

		altBranchStarts = lappend_int(altBranchStarts, branchStart);
		if (fillRPRPattern(alt, pat, idx, depth + 1))
			anyNullable = true;
		altEndPositions = lappend_int(altEndPositions, *idx - 1);
	}

	/* Set jump on first element of each alternative to next alternative */
	foreach(lc, altBranchStarts)
	{
		int			firstElemIdx = lfirst_int(lc);

		if (lnext(altBranchStarts, lc) != NULL)
			pat->elements[firstElemIdx].jump = lfirst_int(lnext(altBranchStarts, lc));
	}

	/* Set next on last element of each alternative to after the alternation */
	afterAltIdx = *idx;

	forboth(lc, altEndPositions, lc2, altBranchStarts)
	{
		int			endPos = lfirst_int(lc);
		int			branchStart = lfirst_int(lc2);

		if (pat->elements[endPos].next != RPR_ELEMIDX_INVALID)
		{
			/*
			 * An inner ALT already set next on this element.  Redirect all
			 * elements in this branch that share the same target to point to
			 * after this ALT instead.
			 */
			int			oldTarget = pat->elements[endPos].next;
			int			j;

			for (j = branchStart; j <= endPos; j++)
			{
				if (pat->elements[j].next == oldTarget)
					pat->elements[j].next = afterAltIdx;
			}
		}
		else
		{
			pat->elements[endPos].next = afterAltIdx;
		}
	}

	list_free(altBranchStarts);
	list_free(altEndPositions);

	return anyNullable;
}

/*
 * fillRPRPattern
 *		Fill pattern elements array from parse tree (pass 2).
 *
 * Recursively traverses the parse tree and populates pre-allocated elements
 * array.
 * Dispatches to type-specific fill functions.
 *
 * Returns true if the pattern is nullable (can match zero rows).
 * For SEQ nodes, all children must be nullable (AND).
 */
static bool
fillRPRPattern(RPRPatternNode *node, RPRPattern *pat, int *idx, RPRDepth depth)
{
	bool		allNullable = true;

	/* Pattern nodes from parser are never NULL */
	Assert(node != NULL);

	check_stack_depth();

	switch (node->nodeType)
	{
		case RPR_PATTERN_SEQ:
			foreach_node(RPRPatternNode, child, node->children)
			{
				if (!fillRPRPattern(child, pat, idx, depth))
					allNullable = false;
			}
			return allNullable;

		case RPR_PATTERN_VAR:
			return fillRPRPatternVar(node, pat, idx, depth);

		case RPR_PATTERN_GROUP:
			return fillRPRPatternGroup(node, pat, idx, depth);

		case RPR_PATTERN_ALT:
			return fillRPRPatternAlt(node, pat, idx, depth);
	}

	pg_unreachable();
	return false;
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
 *   Element 0 (A): ABSORBABLE_BRANCH
 *   Element 1 (B): ABSORBABLE_BRANCH
 *   Element 2 (END): ABSORBABLE | ABSORBABLE_BRANCH  <- comparison point
 *   Element 3 (C): (none)
 *   -> Compare at END every 2 rows. When contexts move to C, absorption stops.
 *
 *   Pattern: (A+ B+)+ C
 *   Element 0 (A): ABSORBABLE | ABSORBABLE_BRANCH  <- only first A+ flagged
 *   Element 1 (B): (none)
 *   Element 2 (END): (none)
 *   Element 3 (C): (none)
 *   -> Only first unbounded portion (A+) gets flags. Absorption happens
 *      at A during first iteration. After moving to B+, absorption stops.
 *
 * First Unbounded Portion Strategy:
 *   The algorithm only flags the FIRST unbounded portion starting from
 *   element 0. This is sufficient because:
 *   - Absorption in first portion already achieves O(n) complexity
 *   - Later portions have different synchronization characteristics
 *   - Nested unbounded patterns are too complex for simple absorption
 *   - Complex patterns (nested groups, etc.) naturally die from mismatch
 *
 * Runtime Usage (in nodeWindowAgg.c):
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
 *   - A+? B (reluctant quantifier)
 *   - (A | B)+ (ALT inside group)
 *   - (A B+)+ (variable-length element inside group)
 *   - (A B{2,5})+ (min != max inside group)
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

	/* Find the END element at startDepth - 1 */
	e = &pattern->elements[idx];
	while (e->depth >= startDepth)
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
 * If the element at startIdx is ALT, recursively checks each branch independently.
 * Each branch gets its own absorbability status, and if any branch is absorbable,
 * the ALT element itself is marked with RPR_ELEM_ABSORBABLE_BRANCH.
 *
 * If BEGIN, skips to first child.
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
		/* ALT: recursively check each branch */
		RPRElemIdx	branchIdx = elem->next;
		RPRPatternElement *branchFirst;
		bool		branchAbsorbable;

		while (branchIdx != RPR_ELEMIDX_INVALID)
		{
			branchAbsorbable = false;

			Assert(branchIdx < pattern->numElements);
			branchFirst = &pattern->elements[branchIdx];

			/* Stop if element is outside ALT scope (not a branch) */
			if (branchFirst->depth <= elem->depth)
				break;

			/* Recursively check this branch */
			computeAbsorbabilityRecursive(pattern, branchIdx, &branchAbsorbable);
			if (branchAbsorbable)
			{
				*hasAbsorbable = true;
			}

			branchIdx = branchFirst->jump;
		}

		/* Mark ALT element if any branch is absorbable */
		if (*hasAbsorbable)
			elem->flags |= RPR_ELEM_ABSORBABLE_BRANCH;
	}
	else if (RPRElemIsBegin(elem))
	{
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
		{
			*hasAbsorbable = true;
		}
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
 * decrease property required for safe absorption.
 *
 * This function sets two flags:
 *   RPR_ELEM_ABSORBABLE: Absorption comparison point
 *     - Simple unbounded VAR: the VAR itself (e.g., A in A+)
 *     - Unbounded GROUP: the END element (e.g., END in (A B)+)
 *   RPR_ELEM_ABSORBABLE_BRANCH: All elements in absorbable region
 *     - All elements within the same scope as unbounded start
 *
 * Examples:
 *   A+ B C         - absorbable (A gets both flags)
 *   (A B)+ C       - absorbable (A,B,END get BRANCH, END gets ABSORBABLE)
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
 * rpr_volatile_func_checker
 *		check_functions_in_node callback: true if funcid is VOLATILE.
 */
static bool
rpr_volatile_func_checker(Oid funcid, void *context)
{
	return (func_volatile(funcid) == PROVOLATILE_VOLATILE);
}

/*
 * rpr_define_errposition
 *		Error cursor position for a DEFINE subexpression.
 *
 * The planner has no ParseState, but the original query text is available in
 * debug_query_string, so we can still point at the offending location exactly
 * as parser_errposition() would.
 */
static int
rpr_define_errposition(int location)
{
	if (location < 0 || debug_query_string == NULL)
		return 0;
	return errposition(pg_mbstrlen_with_len(debug_query_string, location) + 1);
}

/*
 * reject_volatile_in_define_walker
 *		Reject volatile callees and sequence operations anywhere in a DEFINE
 *		expression: they are non-deterministic across the multiple predicate
 *		evaluations that NFA backtracking and PREV/NEXT navigation may trigger
 *		for a single row.
 *
 * NextValueExpr is checked separately because it is not a function call and
 * so is not caught by check_functions_in_node().
 */
static bool
reject_volatile_in_define_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (check_functions_in_node(node, rpr_volatile_func_checker, NULL))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("volatile functions are not allowed in DEFINE clause"),
				rpr_define_errposition(exprLocation(node)));
	if (IsA(node, NextValueExpr))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("sequence operations are not allowed in DEFINE clause"),
				rpr_define_errposition(exprLocation(node)));
	return expression_tree_walker(node, reject_volatile_in_define_walker, context);
}

/*
 * validate_rpr_define_volatility
 *		Reject volatile functions / sequence operations in a DEFINE clause.
 *
 * Called from the planner (subquery_planner) for every RPR WindowClause,
 * including windows not referenced by any OVER clause, so the check is applied
 * regardless of whether the window survives to execution -- matching the
 * coverage of the former parse-time check.
 */
void
validate_rpr_define_volatility(List *defineClause)
{
	foreach_node(TargetEntry, te, defineClause)
	{
		(void) reject_volatile_in_define_walker((Node *) te->expr, NULL);
	}
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
	 * 1. SKIP TO PAST LAST ROW required (not SKIP TO NEXT ROW): With NEXT
	 * ROW, after each match the search resumes from the next row, so contexts
	 * are immediately discarded. No redundant contexts accumulate, making
	 * absorption unnecessary.
	 *
	 * 2. Unbounded frame end required (not ROWS with bounded end): With a
	 * bounded frame (e.g., ROWS BETWEEN CURRENT ROW AND 10 FOLLOWING),
	 * matches may be truncated at frame boundaries. This changes the
	 * absorption semantics - older contexts don't necessarily produce longer
	 * matches when frame limits apply differently to each context.
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

/*
 * nav_traversal_walker
 *		Shared expression-tree walker that locates RPRNavExpr nodes in a
 *		DEFINE expression and dispatches each one to a caller-supplied
 *		visitor.  Used by:
 *		  - planner (visit_nav_plan in createplan.c) to collect tuplestore
 *		    trim offsets and per-variable match_start dependency
 *		  - executor (visit_nav_exec in nodeWindowAgg.c) to evaluate
 *		    non-constant nav offsets at WindowAggState init time
 *
 * The driver wraps a mode-specific context in a NavTraversal and passes
 * it as ctx; the visitor casts t->data to its own context type.  Children
 * of an RPRNavExpr are not walked: the parser's nesting restrictions
 * ensure offsets and dependencies are fully captured by the outer nav
 * kind, so the visitor only needs to inspect the RPRNavExpr itself.
 */
bool
nav_traversal_walker(Node *node, void *ctx)
{
	if (node == NULL)
		return false;

	if (IsA(node, RPRNavExpr))
	{
		NavTraversal *t = (NavTraversal *) ctx;

		t->visit(t, (RPRNavExpr *) node);
		return false;
	}

	return expression_tree_walker(node, nav_traversal_walker, ctx);
}
