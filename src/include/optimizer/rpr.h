/*-------------------------------------------------------------------------
 *
 * rpr.h
 *	  Row Pattern Recognition pattern compilation for planner
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/rpr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RPR_H
#define RPR_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"

/* Limits and special values */
/*
 * Maximum pattern variable ID.  Pattern variables occupy varId 0 to
 * RPR_VARID_MAX inclusive (240 distinct variables); any varId with the high
 * nibble set (0xF0 to 0xFF) is reserved for control elements.  Reserving the
 * whole high nibble, rather than just the values currently in use, leaves
 * room for future control elements; this range can only be narrowed safely
 * before release.
 */
#define RPR_VARID_MAX		0xEF	/* pattern variables are 0 to 0xEF */

/*
 * RPR_COUNT_INF is the value a runtime repetition count saturates at to avoid
 * int32 overflow (see the count++ guard in nfa_match).  It is defined as
 * RPR_QUANTITY_INF (from nodes/parsenodes.h, included above) so that a
 * saturated count compares as "unbounded", just like an unbounded
 * quantifier's max.
 */
#define RPR_COUNT_INF		RPR_QUANTITY_INF
#define RPR_ELEMIDX_MAX		PG_INT16_MAX	/* max pattern elements */
#define RPR_ELEMIDX_INVALID	((RPRElemIdx) -1)	/* invalid index */
#define RPR_DEPTH_MAX		PG_UINT8_MAX	/* 255 levels fit RPRDepth; depth
											 * is 0-based, so the deepest
											 * permitted nesting is 254 */

/* Reserved control-element varIds (high nibble 0xF; 0xF0-0xFA spare) */
#define RPR_VARID_BEGIN		((RPRVarId) 0xFB)	/* group begin */
#define RPR_VARID_END		((RPRVarId) 0xFC)	/* group end */
#define RPR_VARID_ALT		((RPRVarId) 0xFD)	/* alternation start */
#define RPR_VARID_SEP		((RPRVarId) 0xFE)	/* alternation branch
												 * separator */
#define RPR_VARID_FIN		((RPRVarId) 0xFF)	/* pattern finish */

/* Element flags */
#define RPR_ELEM_RELUCTANT			0x01	/* reluctant (non-greedy)
											 * quantifier */
#define RPR_ELEM_EMPTY_LOOP			0x02	/* END: group body can produce
											 * empty match */
#define RPR_ELEM_EMPTY_PREFERRED	0x04	/* END: group body prefers the
											 * empty match */
/*
 * The two absorption flags below are explained in README.rpr IV-5
 * ("Absorbability Analysis"), with worked examples in Appendix B; the
 * analysis that sets them is computeAbsorbability() in
 * optimizer/plan/rpr.c.
 */
#define RPR_ELEM_ABSORBABLE_BRANCH	0x08	/* element in absorbable region */
#define RPR_ELEM_ABSORBABLE			0x10	/* absorption comparison point */

/* Accessor macros for RPRPatternElement */
#define RPRElemIsReluctant(e)			(((e)->flags & RPR_ELEM_RELUCTANT) != 0)
#define RPRElemCanEmptyLoop(e)			(((e)->flags & RPR_ELEM_EMPTY_LOOP) != 0)
#define RPRElemIsEmptyPreferred(e)		(((e)->flags & RPR_ELEM_EMPTY_PREFERRED) != 0)
#define RPRElemIsAbsorbableBranch(e)	(((e)->flags & RPR_ELEM_ABSORBABLE_BRANCH) != 0)
#define RPRElemIsAbsorbable(e)			(((e)->flags & RPR_ELEM_ABSORBABLE) != 0)
#define RPRElemIsVar(e)			((e)->varId <= RPR_VARID_MAX)
#define RPRElemIsBegin(e)		((e)->varId == RPR_VARID_BEGIN)
#define RPRElemIsEnd(e)			((e)->varId == RPR_VARID_END)
#define RPRElemIsAlt(e)			((e)->varId == RPR_VARID_ALT)
#define RPRElemIsSep(e)			((e)->varId == RPR_VARID_SEP)
#define RPRElemIsFin(e)			((e)->varId == RPR_VARID_FIN)
#define RPRElemCanSkip(e)		((e)->min == 0)

extern RPRPattern *buildRPRPattern(RPRPatternNode *pattern, List *defineClause,
								   RPSkipTo rpSkipTo, int frameOptions,
								   bool hasMatchStartDependent);

#endif							/* RPR_H */
