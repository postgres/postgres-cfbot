/*-------------------------------------------------------------------------
 *
 * parse_rpr.c
 *	  Handle Row Pattern Recognition clauses in parser.
 *
 * This file transforms RPR-related clauses from raw parse tree to planner
 * structures during query analysis:
 *   - Validates frame options (ROWS only, must start at CURRENT ROW, no
 *     EXCLUDE, and CURRENT ROW is not accepted as the frame end)
 *   - Validates PATTERN variable count (max RPR_VARID_MAX + 1)
 *   - Transforms DEFINE clause
 *   - Stores the PATTERN parse tree and the AFTER MATCH SKIP TO flag
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_rpr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/rpr.h"
#include "parser/parse_clause.h"
#include "parser/parse_rpr.h"

/* DEFINE clause walker context -- see define_walker for usage. */
typedef enum
{
	DEFINE_PHASE_BODY,			/* top-level DEFINE expression */
	DEFINE_PHASE_NAV_ARG,		/* inside an outer nav's arg subtree */
	DEFINE_PHASE_NAV_OFFSET,	/* inside an outer nav's offset_arg /
								 * compound_offset_arg */
} DefinePhase;

typedef struct
{
	ParseState *pstate;
	DefinePhase phase;
	int			nav_count;		/* RPRNavExpr nodes seen in current nav.arg */
	bool		has_column_ref; /* Var seen in current nav scope */
	RPRNavKind	inner_kind;		/* kind of first nested nav in current arg */
} DefineWalkCtx;

/* Forward declarations */
static void validateRPRPatternVarCount(ParseState *pstate, RPRPatternNode *node,
									   List **varNames);
static List *transformDefineClause(ParseState *pstate, WindowDef *windef);
static bool define_walker(Node *node, void *context);
static bool rpr_frame_is_supported(int frameOptions);

/*
 * transformRPR
 *		Process Row Pattern Recognition related clauses.
 *
 * Validates and transforms RPR clauses from parse tree to planner structures:
 *   - Validates frame options (ROWS only, must start at CURRENT ROW, no
 *     EXCLUDE, and CURRENT ROW is not accepted as the frame end)
 *   - Set AFTER MATCH SKIP TO flag
 *   - Transforms DEFINE clause into TargetEntry list
 *   - Stores PATTERN parse tree for deparsing (optimization happens in planner)
 *
 * Returns early if windef has no rpCommonSyntax (non-RPR window).
 */
void
transformRPR(ParseState *pstate, WindowClause *wc, WindowDef *windef)
{
	/* Nothing to do unless the window carries a row pattern */
	if (windef->rpCommonSyntax == NULL)
		return;

	if (!rpr_frame_is_supported(wc->frameOptions))
	{
		/*
		 * The frame type keyword beats the start of the window definition,
		 * which is all a defaulted frame leaves to point at.
		 */
		int			location = windef->frameLocation >= 0 ?
			windef->frameLocation : windef->location;

		ereport(ERROR,
				errcode(ERRCODE_WINDOWING_ERROR),
				errmsg("unsupported frame for row pattern recognition"),
		/*- translator: both %s are SQL window frame specifications */
				errdetail("The frame must be \"%s\" or \"%s\".",
						  "ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING",
						  "ROWS BETWEEN CURRENT ROW AND offset FOLLOWING"),
				parser_errposition(pstate, location));
	}

	/*
	 * EXCLUDE is not part of the frame shape, so it is reported on its own,
	 * and from its own location.
	 */
	if (wc->frameOptions & FRAMEOPTION_EXCLUSION)
	{
		int			location = windef->excludeLocation >= 0 ?
			windef->excludeLocation : windef->location;

		ereport(ERROR,
				errcode(ERRCODE_WINDOWING_ERROR),
				errmsg("cannot use EXCLUDE with row pattern recognition"),
				parser_errposition(pstate, location));
	}

	Assert(wc->frameOptions & FRAMEOPTION_ROWS);

	/* Assign AFTER MATCH SKIP TO flag */
	wc->rpSkipTo = windef->rpCommonSyntax->rpSkipTo;

	/* Transform DEFINE clause into list of TargetEntry's */
	wc->defineClause = transformDefineClause(pstate, windef);

	/* Store PATTERN parse tree for deparsing */
	wc->rpPattern = windef->rpCommonSyntax->rpPattern;
}

/*
 * rpr_frame_is_supported
 *		Is this the frame shape row pattern recognition matches over?
 *
 * Only ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING and ROWS BETWEEN
 * CURRENT ROW AND offset FOLLOWING are supported.  EXCLUDE is rejected by
 * the caller, since it is not part of the shape.
 *
 * The offset's value is not settled until execution;
 * calculate_frame_offsets() rejects a non-positive one there.
 */
static bool
rpr_frame_is_supported(int frameOptions)
{
	if ((frameOptions & FRAMEOPTION_ROWS) == 0)
		return false;
	if ((frameOptions & FRAMEOPTION_START_CURRENT_ROW) == 0)
		return false;
	if ((frameOptions & (FRAMEOPTION_END_UNBOUNDED_FOLLOWING |
						 FRAMEOPTION_END_OFFSET_FOLLOWING)) == 0)
		return false;

	return true;
}

/*
 * validateRPRPatternVarCount
 *		Validate that PATTERN variable count fits the varId range.
 *
 * Recursively traverses the pattern tree, collecting unique variable names.
 * Throws an error if the number of unique variables would require a varId
 * greater than RPR_VARID_MAX.
 *
 * varNames collects the unique PATTERN variable names, which is what
 * transformColumnRef checks via p_rpr_pattern_vars to identify pattern
 * variable qualifiers.  Cross-checking DEFINE variable names against this
 * list is the caller's responsibility, since it only needs to run once.
 */
static void
validateRPRPatternVarCount(ParseState *pstate, RPRPatternNode *node,
						   List **varNames)
{
	/* Pattern node must exist - parser always provides non-NULL root */
	Assert(node != NULL);

	/*
	 * trailing_alt is a transient grammar flag; splitRPRTrailingAlt must have
	 * cleared it on every node before the pattern reaches parse analysis.
	 */
	Assert(!node->trailing_alt);

	check_stack_depth();

	switch (node->nodeType)
	{
		case RPR_PATTERN_VAR:
			/* Add variable name if not already in list */
			{
				bool		found = false;

				foreach_node(String, varname, *varNames)
				{
					if (strcmp(strVal(varname), node->varName) == 0)
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					/*
					 * Check against RPR_VARID_MAX before adding.  varId
					 * values run 0 to RPR_VARID_MAX inclusive, so the next
					 * varId to be assigned (the current list length) must not
					 * exceed it.
					 */
					if (list_length(*varNames) > RPR_VARID_MAX)
						ereport(ERROR,
								errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								errmsg("too many row pattern variables"),
								errdetail("The maximum number of row pattern variables is %d.", RPR_VARID_MAX + 1),
								parser_errposition(pstate,
												   exprLocation((Node *) node)));

					*varNames = lappend(*varNames, makeString(pstrdup(node->varName)));
				}
			}
			break;

		case RPR_PATTERN_SEQ:
		case RPR_PATTERN_ALT:
		case RPR_PATTERN_GROUP:
			/* Recurse into children */
			foreach_node(RPRPatternNode, child, node->children)
			{
				validateRPRPatternVarCount(pstate, child, varNames);
			}
			break;
	}
}

/*
 * transformDefineClause
 *		Process DEFINE clause and transform ResTarget into list of TargetEntry.
 *
 * Note: Variables not in DEFINE are evaluated as TRUE by the executor.
 * Variables in DEFINE but not in PATTERN are rejected as an error.
 *
 * XXX Pattern variable qualified expressions in DEFINE (e.g. "A.price")
 * are not yet supported.  Currently rejected by transformColumnRef in
 * parse_expr.c via the p_rpr_pattern_vars check.
 */
static List *
transformDefineClause(ParseState *pstate, WindowDef *windef)
{
	List	   *defineClause = NIL;
	List	   *patternVarNames = NIL;

	/*
	 * The grammar builds an RPCommonSyntax only for a window specification
	 * that carries DEFINE, so the list is never empty here.
	 */
	Assert(windef->rpCommonSyntax->rpDefs != NULL);

	/*
	 * Validate PATTERN variable count and collect the PATTERN variable names
	 * for transformColumnRef.
	 */
	validateRPRPatternVarCount(pstate, windef->rpCommonSyntax->rpPattern,
							   &patternVarNames);
	pstate->p_rpr_pattern_vars = patternVarNames;

	/*
	 * Open the DEFINE scope.  The restrictions a DEFINE condition is under
	 * hold for the whole condition, so they are keyed on this rather than on
	 * p_expr_kind, which names only the innermost clause and is replaced by
	 * anything nested in the condition that has a kind of its own.  It is
	 * closed below, once every DEFINE expression has been transformed.
	 */
	pstate->p_rpr_define = true;

	/*
	 * Reject any DEFINE variable whose name does not appear in PATTERN.  This
	 * cross-check only needs to run once, so it lives here in the caller
	 * rather than in the recursive validateRPRPatternVarCount().
	 */
	foreach_node(ResTarget, rt, windef->rpCommonSyntax->rpDefs)
	{
		bool		found = false;

		foreach_node(String, varname, patternVarNames)
		{
			if (strcmp(strVal(varname), rt->name) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("DEFINE variable \"%s\" is not used in PATTERN",
						   rt->name),
					parser_errposition(pstate, rt->location));
	}

	/*
	 * Check for duplicate row pattern definition variables.  The standard
	 * requires that no two row pattern definition variable names shall be
	 * equivalent.  Report the error at the later (duplicate) definition.
	 */
	foreach_node(ResTarget, restarget, windef->rpCommonSyntax->rpDefs)
	{
		foreach_node(ResTarget, prior, windef->rpCommonSyntax->rpDefs)
		{
			if (prior == restarget)
				break;
			if (strcmp(prior->name, restarget->name) == 0)
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("DEFINE variable \"%s\" appears more than once",
							   restarget->name),
						parser_errposition(pstate,
										   exprLocation((Node *) restarget)));
		}
	}

	foreach_node(ResTarget, restarget, windef->rpCommonSyntax->rpDefs)
	{
		TargetEntry *teDefine;
		Node	   *expr;

		/*
		 * Transform the DEFINE expression and coerce it to boolean.  The
		 * result belongs in wc->defineClause, never in the query targetlist
		 * as a whole: it may contain RPRNavExpr nodes (PREV/NEXT/FIRST/LAST)
		 * that only the owning WindowAgg can evaluate.
		 */
		expr = transformWhereClause(pstate, restarget->val,
									EXPR_KIND_RPR_DEFINE, "DEFINE");

		/* Build the defineClause entry directly from the transformed expr */
		teDefine = makeTargetEntry((Expr *) expr,
								   list_length(defineClause) + 1,
								   pstrdup(restarget->name),
								   true);

		/* build transformed DEFINE clause (list of TargetEntry) */
		defineClause = lappend(defineClause, teDefine);
	}
	pstate->p_rpr_define = false;
	pstate->p_rpr_pattern_vars = NIL;

	/*
	 * Validate DEFINE expressions: nested PREV/NEXT, column references,
	 * compound flatten -- all in a single walk per variable.
	 */
	foreach_ptr(TargetEntry, te, defineClause)
	{
		DefineWalkCtx ctx;

		ctx.pstate = pstate;
		ctx.phase = DEFINE_PHASE_BODY;
		ctx.nav_count = 0;
		ctx.has_column_ref = false;
		ctx.inner_kind = 0;
		(void) define_walker((Node *) te->expr, &ctx);
	}

	return defineClause;
}

/*
 * define_walker
 *		Single-pass DEFINE clause validator.  At each node, enforces:
 *
 *		  [1] for each outer RPRNavExpr (PHASE_BODY -> PHASE_NAV_ARG):
 *			  - nav.arg must contain at least one column reference
 *			  - PREV/NEXT wrapping FIRST/LAST is flattened in place
 *				to a compound kind (PREV_FIRST, PREV_LAST, NEXT_FIRST,
 *				NEXT_LAST)
 *			  - an inner navigation that is not nav.arg itself is
 *				rejected as not being a direct argument
 *			  - any other nesting is rejected (FIRST(PREV()),
 *				PREV(PREV()), FIRST(FIRST()), three-or-more deep)
 *		  [2] for each nav offset (PHASE_NAV_OFFSET):
 *			  - must be a run-time constant (no column references)
 *			  - must not contain a row pattern navigation operation
 *
 * Entering an outer nav, the walker walks nav.arg in PHASE_NAV_ARG to collect
 * nesting and column-ref state, flattens a compound form or raises a nesting
 * error, then walks the post-flatten offset(s) in PHASE_NAV_OFFSET.  A
 * compound form's inner offset is walked in both passes: PHASE_NAV_ARG only
 * asks whether nav.arg as a whole holds a column reference, so the offset is
 * walked again to catch one it would have leaked.
 *
 * Var sightings feed the column-ref rule for the enclosing nav scope;
 * RPRNavExpr sightings inside PHASE_NAV_ARG feed the nesting decision.
 * The phases themselves are described where DefinePhase is declared.
 */
static bool
define_walker(Node *node, void *context)
{
	DefineWalkCtx *ctx = (DefineWalkCtx *) context;

	if (node == NULL)
		return false;

	/* Var sighting feeds the column-ref rule for the enclosing nav scope. */
	if (IsA(node, Var) &&
		(ctx->phase == DEFINE_PHASE_NAV_ARG ||
		 ctx->phase == DEFINE_PHASE_NAV_OFFSET))
		ctx->has_column_ref = true;

	if (IsA(node, RPRNavExpr))
	{
		RPRNavExpr *nav = (RPRNavExpr *) node;

		if (ctx->phase == DEFINE_PHASE_NAV_ARG)
		{
			/*
			 * Nested nav inside an outer nav.arg: record for the outer's
			 * compound / nesting decision, then keep recursing so deeper Vars
			 * are still observed.
			 */
			if (ctx->nav_count == 0)
				ctx->inner_kind = nav->kind;
			ctx->nav_count++;
			return expression_tree_walker(node, define_walker, ctx);
		}
		else if (ctx->phase == DEFINE_PHASE_NAV_OFFSET)
		{
			/*
			 * A navigation offset must be a run-time constant, so it cannot
			 * contain a navigation operation.
			 */
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("row pattern navigation offset cannot contain a row pattern navigation operation"),
					errdetail("A navigation offset must be a run-time constant."),
					parser_errposition(ctx->pstate, nav->location));
		}
		else
		{
			/*
			 * PHASE_BODY: this is an outer nav at top level.  Walk arg first
			 * to collect nesting / column-ref state, then validate and (for
			 * compound forms) flatten, then walk offset(s).
			 */
			DefineWalkCtx saved = *ctx;
			bool		outer_phys = (nav->kind == RPR_NAV_PREV ||
									  nav->kind == RPR_NAV_NEXT);
			bool		flattened = false;

			ctx->phase = DEFINE_PHASE_NAV_ARG;
			ctx->nav_count = 0;
			ctx->has_column_ref = false;
			ctx->inner_kind = 0;
			(void) define_walker((Node *) nav->arg, ctx);

			if (ctx->nav_count > 0)
			{
				bool		inner_phys = (ctx->inner_kind == RPR_NAV_PREV ||
										  ctx->inner_kind == RPR_NAV_NEXT);

				if (outer_phys && !inner_phys)
				{
					RPRNavExpr *inner;

					/* Reject an inner nav that is not the whole argument */
					if (!IsA(nav->arg, RPRNavExpr))
						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("row pattern navigation operation must be a direct argument of the outer navigation"),
								errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
								parser_errposition(ctx->pstate, nav->location));

					/* Reject triple-or-deeper nesting; siblings caught above */
					if (ctx->nav_count > 1)
						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("cannot nest row pattern navigation more than two levels deep"),
								errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
								parser_errposition(ctx->pstate, nav->location));

					inner = (RPRNavExpr *) nav->arg;

					if (nav->kind == RPR_NAV_PREV && inner->kind == RPR_NAV_FIRST)
						nav->kind = RPR_NAV_PREV_FIRST;
					else if (nav->kind == RPR_NAV_PREV && inner->kind == RPR_NAV_LAST)
						nav->kind = RPR_NAV_PREV_LAST;
					else if (nav->kind == RPR_NAV_NEXT && inner->kind == RPR_NAV_FIRST)
						nav->kind = RPR_NAV_NEXT_FIRST;
					else if (nav->kind == RPR_NAV_NEXT && inner->kind == RPR_NAV_LAST)
						nav->kind = RPR_NAV_NEXT_LAST;

					nav->compound_offset_arg = nav->offset_arg;
					nav->offset_arg = inner->offset_arg;
					nav->arg = inner->arg;
					flattened = true;

					/*
					 * The flattened argument must include a column reference,
					 * just like the simple-nav case below.
					 */
					if (!ctx->has_column_ref)
						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("argument of row pattern navigation operation must include at least one column reference"),
								parser_errposition(ctx->pstate, nav->location));
				}
				else if (!outer_phys && inner_phys)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("FIRST and LAST cannot contain PREV or NEXT"),
							errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
							parser_errposition(ctx->pstate, nav->location));
				else if (outer_phys && inner_phys)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("PREV and NEXT cannot contain PREV or NEXT"),
							errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
							parser_errposition(ctx->pstate, nav->location));
				else
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("FIRST and LAST cannot contain FIRST or LAST"),
							errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
							parser_errposition(ctx->pstate, nav->location));
			}
			else if (!ctx->has_column_ref)
			{
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("argument of row pattern navigation operation must include at least one column reference"),
						parser_errposition(ctx->pstate, nav->location));
			}

			/*
			 * Walk offset arg(s) in PHASE_NAV_OFFSET to enforce the
			 * constant-offset rule.  For compound forms, both the inner
			 * (post-flatten nav->offset_arg) and outer (compound_offset_arg)
			 * offsets must be constants; the inner's column-ref status was
			 * not separately tracked during the PHASE_NAV_ARG walk (which
			 * only checks that nav.arg as a whole has at least one Var), so
			 * it is re-walked here to catch column references the inner
			 * offset would have leaked.
			 */
			ctx->phase = DEFINE_PHASE_NAV_OFFSET;

			if (nav->offset_arg != NULL)
			{
				ctx->has_column_ref = false;
				(void) define_walker((Node *) nav->offset_arg, ctx);
				if (ctx->has_column_ref)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("row pattern navigation offset must be a run-time constant"),
							parser_errposition(ctx->pstate, exprLocation((Node *) nav->offset_arg)));
			}
			if (flattened && nav->compound_offset_arg != NULL)
			{
				ctx->has_column_ref = false;
				(void) define_walker((Node *) nav->compound_offset_arg, ctx);
				if (ctx->has_column_ref)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("row pattern navigation offset must be a run-time constant"),
							parser_errposition(ctx->pstate, exprLocation((Node *) nav->compound_offset_arg)));
			}

			*ctx = saved;
			return false;
		}
	}

	return expression_tree_walker(node, define_walker, ctx);
}
