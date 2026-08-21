/*-------------------------------------------------------------------------
 *
 * parse_rpr.c
 *	  Handle Row Pattern Recognition clauses in parser.
 *
 * This file transforms RPR-related clauses from raw parse tree to planner
 * structures during query analysis:
 *   - Validates frame options (must start at CURRENT ROW, no EXCLUDE)
 *   - Validates PATTERN variable count (max RPR_VARID_MAX + 1)
 *   - Transforms DEFINE clause
 *   - Stores the PATTERN parse tree and the SKIP TO/INITIAL flags
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
#include "optimizer/optimizer.h"
#include "optimizer/rpr.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_rpr.h"
#include "parser/parse_target.h"

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
static List *transformDefineClause(ParseState *pstate, WindowDef *windef,
								   List **targetlist);
static bool define_walker(Node *node, void *context);

/*
 * transformRPR
 *		Process Row Pattern Recognition related clauses.
 *
 * Validates and transforms RPR clauses from parse tree to planner structures:
 *   - Validates frame options (must start at CURRENT ROW, no EXCLUDE)
 *   - Set AFTER MATCH SKIP TO flag
 *   - Set SEEK/INITIAL flag
 *   - Transforms DEFINE clause into TargetEntry list
 *   - Stores PATTERN parse tree for deparsing (optimization happens in planner)
 *
 * Returns early if windef has no rpCommonSyntax (non-RPR window).
 */
void
transformRPR(ParseState *pstate, WindowClause *wc, WindowDef *windef,
			 List **targetlist)
{
	/* Window definition must exist when called */
	Assert(windef != NULL);

	/*
	 * Row Pattern Common Syntax clause exists?
	 */
	if (windef->rpCommonSyntax == NULL)
		return;

	/* Check Frame options */

	/* Frame type must be "ROW" */
	if (wc->frameOptions & FRAMEOPTION_GROUPS)
		ereport(ERROR,
				errcode(ERRCODE_WINDOWING_ERROR),
				errmsg("cannot use FRAME option GROUPS with row pattern recognition"),
				errhint("Use ROWS instead."),
				parser_errposition(pstate,
								   windef->frameLocation >= 0 ?
								   windef->frameLocation : windef->location));
	if (wc->frameOptions & FRAMEOPTION_RANGE)
		ereport(ERROR,
				errcode(ERRCODE_WINDOWING_ERROR),
				errmsg("cannot use FRAME option RANGE with row pattern recognition"),
				errhint("Use ROWS instead."),
				parser_errposition(pstate,
								   windef->frameLocation >= 0 ?
								   windef->frameLocation : windef->location));

	/* Frame must start at current row */
	if ((wc->frameOptions & FRAMEOPTION_START_CURRENT_ROW) == 0)
	{
		const char *frameType = "ROWS";
		const char *startBound = "unknown";

		/* Determine current start bound */
		if (wc->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
			startBound = "UNBOUNDED PRECEDING";
		else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING)
			startBound = "offset PRECEDING";
		else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING)
			startBound = "offset FOLLOWING";

		/* At least one valid frame start option should be set */
		Assert((wc->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING) ||
			   (wc->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING) ||
			   (wc->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING));

		ereport(ERROR,
				errcode(ERRCODE_WINDOWING_ERROR),
				errmsg("FRAME must start at CURRENT ROW when using row pattern recognition"),
				errdetail("Current frame starts with %s.", startBound),
				errhint("Use: %s BETWEEN CURRENT ROW AND ...", frameType),
				parser_errposition(pstate, windef->frameLocation >= 0 ? windef->frameLocation : windef->location));
	}

	/* EXCLUDE options are not permitted */
	if ((wc->frameOptions & FRAMEOPTION_EXCLUSION) != 0)
	{
		const char *excludeType = "EXCLUDE";

		/* Determine which EXCLUDE option was used */
		if (wc->frameOptions & FRAMEOPTION_EXCLUDE_CURRENT_ROW)
			excludeType = "EXCLUDE CURRENT ROW";
		else if (wc->frameOptions & FRAMEOPTION_EXCLUDE_GROUP)
			excludeType = "EXCLUDE GROUP";
		else if (wc->frameOptions & FRAMEOPTION_EXCLUDE_TIES)
			excludeType = "EXCLUDE TIES";

		/* At least one valid exclude option should be set */
		Assert((wc->frameOptions & FRAMEOPTION_EXCLUDE_CURRENT_ROW) ||
			   (wc->frameOptions & FRAMEOPTION_EXCLUDE_GROUP) ||
			   (wc->frameOptions & FRAMEOPTION_EXCLUDE_TIES));

		ereport(ERROR,
				errcode(ERRCODE_WINDOWING_ERROR),
				errmsg("cannot use EXCLUDE options with row pattern recognition"),
				errdetail("Frame definition includes %s.", excludeType),
				errhint("Remove the EXCLUDE clause from the window definition."),
				parser_errposition(pstate, windef->excludeLocation >= 0 ? windef->excludeLocation : windef->location));
	}

	/*
	 * The standard allows only UNBOUNDED FOLLOWING or a positive offset
	 * FOLLOWING as the frame end.  The equivalent 0 FOLLOWING spelling is
	 * caught at runtime in calculate_frame_offsets().
	 */
	if (wc->frameOptions & FRAMEOPTION_END_CURRENT_ROW)
		ereport(ERROR,
				errcode(ERRCODE_WINDOWING_ERROR),
				errmsg("cannot use CURRENT ROW as frame end with row pattern recognition"),
				errhint("Use UNBOUNDED FOLLOWING or a positive offset FOLLOWING."),
				parser_errposition(pstate,
								   windef->frameLocation >= 0 ?
								   windef->frameLocation : windef->location));

	/* Assign AFTER MATCH SKIP TO flag */
	wc->rpSkipTo = windef->rpCommonSyntax->rpSkipTo;

	/* Assign INITIAL flag */
	wc->initial = windef->rpCommonSyntax->initial;

	/* Transform DEFINE clause into list of TargetEntry's */
	wc->defineClause = transformDefineClause(pstate, windef, targetlist);

	/* Store PATTERN parse tree for deparsing */
	wc->rpPattern = windef->rpCommonSyntax->rpPattern;
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
transformDefineClause(ParseState *pstate, WindowDef *windef,
					  List **targetlist)
{
	List	   *defineClause = NIL;
	List	   *patternVarNames = NIL;

	/*
	 * If Row Definition Common Syntax exists, DEFINE clause must exist. (the
	 * raw parser should have already checked it.)
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
		List	   *vars;

		/*
		 * Transform the DEFINE expression and coerce it to boolean.  We must
		 * NOT add the whole expression to the query targetlist, because it
		 * may contain RPRNavExpr nodes (PREV/NEXT/FIRST/LAST) that can only
		 * be evaluated inside the owning WindowAgg.  Coercing here, before
		 * pull_var_clause, keeps pull_var_clause operating on the final
		 * expression form and surfaces a type mismatch before the targetlist
		 * is touched.
		 */
		expr = transformExpr(pstate, restarget->val,
							 EXPR_KIND_RPR_DEFINE);
		expr = coerce_to_boolean(pstate, expr, "DEFINE");

		/* Build the defineClause entry directly from the transformed expr */
		teDefine = makeTargetEntry((Expr *) expr,
								   list_length(defineClause) + 1,
								   pstrdup(restarget->name),
								   true);

		/* build transformed DEFINE clause (list of TargetEntry) */
		defineClause = lappend(defineClause, teDefine);

		/*
		 * Pull out Var nodes from the transformed expression and ensure each
		 * one is present in the targetlist.  This is needed so the planner
		 * propagates the referenced columns through the plan tree, making
		 * them available to the WindowAgg's DEFINE evaluation.
		 */
		vars = pull_var_clause(expr, 0);
		foreach_node(Var, var, vars)
		{
			bool		found = false;

			foreach_node(TargetEntry, tle, *targetlist)
			{
				if (IsA(tle->expr, Var) &&
					((Var *) tle->expr)->varno == var->varno &&
					((Var *) tle->expr)->varattno == var->varattno)
				{
					found = true;
					break;
				}
			}
			if (!found)
			{
				TargetEntry *newtle;

				newtle = makeTargetEntry((Expr *) copyObject(var),
										 list_length(*targetlist) + 1,
										 NULL,
										 true);
				*targetlist = lappend(*targetlist, newtle);
			}
		}
		list_free(vars);
	}
	pstate->p_rpr_pattern_vars = NIL;

	/*
	 * Validate DEFINE expressions: nested PREV/NEXT, column references,
	 * compound flatten, volatile callees -- all in a single walk per
	 * variable.
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

	/* mark column origins */
	markTargetListOrigins(pstate, defineClause);

	/* mark all nodes in the DEFINE clause tree with collation information */
	assign_expr_collations(pstate, (Node *) defineClause);

	return defineClause;
}

/*
 * Single-pass DEFINE clause validator.
 *
 * One walker function (define_walker) visits every node in a DEFINE
 * expression exactly once and enforces, for each outer RPRNavExpr (per
 * ISO/IEC 19075-5 5.6.4 nesting rules):
 *   - arg must contain at least one column reference
 *   - PREV/NEXT wrapping FIRST/LAST flattens to a compound kind
 *   - Other nestings are rejected (FIRST(PREV()), PREV(PREV()), ...)
 *   - offset_arg / compound_offset_arg must not contain column refs
 *     or nested navigation operations
 *
 * Volatile callees (and sequence operations) are rejected later in the
 * planner via validate_rpr_define_volatility(); see optimizer/plan/rpr.c.
 *
 * The walker uses a phase tag to know which subtree it is in: DEFINE
 * body (top-level), inside a nav.arg, or inside a nav.offset_arg /
 * compound_offset_arg.  When entering an outer nav (PHASE_BODY), it
 * walks nav.arg in PHASE_NAV_ARG to collect nesting/column-ref state,
 * applies compound flatten or raises a nesting error, then walks the
 * (post-flatten) offset(s) in PHASE_NAV_OFFSET to enforce the
 * constant-offset and no-nested-nav rules.  No subtree is walked twice.
 */

/*
 * define_walker
 *		Single-pass DEFINE clause validator.  At each node, enforces:
 *
 *		  [1] for each outer RPRNavExpr (PHASE_BODY -> PHASE_NAV_ARG):
 *			  - nav.arg must contain at least one column reference
 *			  - PREV/NEXT wrapping FIRST/LAST is flattened in place
 *				to a compound kind (PREV_FIRST, PREV_LAST, NEXT_FIRST,
 *				NEXT_LAST)
 *			  - any other nesting is rejected (FIRST(PREV()),
 *				PREV(PREV()), FIRST(FIRST()), three-or-more deep)
 *		  [2] for each nav offset (PHASE_NAV_OFFSET):
 *			  - must be a run-time constant (no column references)
 *			  - must not contain a row pattern navigation operation
 *
 * Var sightings feed the column-ref rule for the enclosing nav scope;
 * RPRNavExpr sightings inside PHASE_NAV_ARG feed the nesting decision.
 * See the comment block above DefinePhase for the overall design and
 * how each subtree is walked exactly once.
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
			 * and volatile callees are still observed.
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

					/* Reject triple-or-deeper nesting */
					if (ctx->nav_count > 1)
						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("cannot nest row pattern navigation more than two levels deep"),
								errhint("Only PREV(FIRST()), PREV(LAST()), NEXT(FIRST()), and NEXT(LAST()) compound forms are allowed."),
								parser_errposition(ctx->pstate, nav->location));

					if (!IsA(nav->arg, RPRNavExpr))
						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("row pattern navigation operation must be a direct argument of the outer navigation"),
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
							errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("row pattern navigation offset must be a run-time constant"),
							parser_errposition(ctx->pstate, exprLocation((Node *) nav->offset_arg)));
			}
			if (flattened && nav->compound_offset_arg != NULL)
			{
				ctx->has_column_ref = false;
				(void) define_walker((Node *) nav->compound_offset_arg, ctx);
				if (ctx->has_column_ref)
					ereport(ERROR,
							errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("row pattern navigation offset must be a run-time constant"),
							parser_errposition(ctx->pstate, exprLocation((Node *) nav->compound_offset_arg)));
			}

			*ctx = saved;
			return false;
		}
	}

	return expression_tree_walker(node, define_walker, ctx);
}
