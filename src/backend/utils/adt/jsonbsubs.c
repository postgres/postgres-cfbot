/*-------------------------------------------------------------------------
 *
 * jsonbsubs.c
 *	  Subscripting support functions for jsonb.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonbsubs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/execExpr.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/jsonfuncs.h"


/* SubscriptingRefState.workspace for jsonb subscripting execution */
typedef struct JsonbSubWorkspace
{
	bool		expectArray;	/* jsonb root is expected to be an array */
	Oid		   *indexOid;		/* OID of coerced subscript expression, could
								 * be only integer or text */
	Datum	   *index;			/* Subscript values in Datum format */
} JsonbSubWorkspace;

typedef struct
{
	ForeachAIterator pub;
	JsonbIterator *it;
	Oid			target_typid;
	int32		target_typmod;
	ForeachAIteratorCoercionStrategy coercion_strategy;
	bool		skip_nested;
	MemoryContext cache_mcxt;
	void	   *cache;
} JsonbForeachAIterator;

/*
 * Finish parse analysis of a SubscriptingRef expression for a jsonb.
 *
 * Transform the subscript expressions, coerce them to text,
 * and determine the result type of the SubscriptingRef node.
 */
static void
jsonb_subscript_transform(SubscriptingRef *sbsref,
						  List *indirection,
						  ParseState *pstate,
						  bool isSlice,
						  bool isAssignment)
{
	List	   *upperIndexpr = NIL;
	ListCell   *idx;

	/*
	 * Transform and convert the subscript expressions. Jsonb subscripting
	 * does not support slices, look only at the upper index.
	 */
	foreach(idx, indirection)
	{
		A_Indices  *ai = lfirst_node(A_Indices, idx);
		Node	   *subExpr;

		if (isSlice)
		{
			Node	   *expr = ai->uidx ? ai->uidx : ai->lidx;

			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("jsonb subscript does not support slices"),
					 parser_errposition(pstate, exprLocation(expr))));
		}

		if (ai->uidx)
		{
			Oid			subExprType = InvalidOid,
						targetType = UNKNOWNOID;

			subExpr = transformExpr(pstate, ai->uidx, pstate->p_expr_kind);
			subExprType = exprType(subExpr);

			if (subExprType != UNKNOWNOID)
			{
				Oid			targets[2] = {INT4OID, TEXTOID};

				/*
				 * Jsonb can handle multiple subscript types, but cases when a
				 * subscript could be coerced to multiple target types must be
				 * avoided, similar to overloaded functions. It could be
				 * possibly extend with jsonpath in the future.
				 */
				for (int i = 0; i < 2; i++)
				{
					if (can_coerce_type(1, &subExprType, &targets[i], COERCION_IMPLICIT))
					{
						/*
						 * One type has already succeeded, it means there are
						 * two coercion targets possible, failure.
						 */
						if (targetType != UNKNOWNOID)
							ereport(ERROR,
									(errcode(ERRCODE_DATATYPE_MISMATCH),
									 errmsg("subscript type %s is not supported", format_type_be(subExprType)),
									 errhint("jsonb subscript must be coercible to only one type, integer or text."),
									 parser_errposition(pstate, exprLocation(subExpr))));

						targetType = targets[i];
					}
				}

				/*
				 * No suitable types were found, failure.
				 */
				if (targetType == UNKNOWNOID)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("subscript type %s is not supported", format_type_be(subExprType)),
							 errhint("jsonb subscript must be coercible to either integer or text."),
							 parser_errposition(pstate, exprLocation(subExpr))));
			}
			else
				targetType = TEXTOID;

			/*
			 * We known from can_coerce_type that coercion will succeed, so
			 * coerce_type could be used. Note the implicit coercion context,
			 * which is required to handle subscripts of different types,
			 * similar to overloaded functions.
			 */
			subExpr = coerce_type(pstate,
								  subExpr, subExprType,
								  targetType, -1,
								  COERCION_IMPLICIT,
								  COERCE_IMPLICIT_CAST,
								  -1);
			if (subExpr == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("jsonb subscript must have text type"),
						 parser_errposition(pstate, exprLocation(subExpr))));
		}
		else
		{
			/*
			 * Slice with omitted upper bound. Should not happen as we already
			 * errored out on slice earlier, but handle this just in case.
			 */
			Assert(isSlice && ai->is_slice);
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("jsonb subscript does not support slices"),
					 parser_errposition(pstate, exprLocation(ai->uidx))));
		}

		upperIndexpr = lappend(upperIndexpr, subExpr);
	}

	/* store the transformed lists into the SubscriptingRef node */
	sbsref->refupperindexpr = upperIndexpr;
	sbsref->reflowerindexpr = NIL;

	/* Determine the result type of the subscripting operation; always jsonb */
	sbsref->refrestype = JSONBOID;
	sbsref->reftypmod = -1;
}

/*
 * During execution, process the subscripts in a SubscriptingRef expression.
 *
 * The subscript expressions are already evaluated in Datum form in the
 * SubscriptingRefState's arrays.  Check and convert them as necessary.
 *
 * If any subscript is NULL, we throw error in assignment cases, or in fetch
 * cases set result to NULL and return false (instructing caller to skip the
 * rest of the SubscriptingRef sequence).
 */
static bool
jsonb_subscript_check_subscripts(ExprState *state,
								 ExprEvalStep *op,
								 ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref_subscript.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;

	/*
	 * In case if the first subscript is an integer, the source jsonb is
	 * expected to be an array. This information is not used directly, all
	 * such cases are handled within corresponding jsonb assign functions. But
	 * if the source jsonb is NULL the expected type will be used to construct
	 * an empty source.
	 */
	if (sbsrefstate->numupper > 0 && sbsrefstate->upperprovided[0] &&
		!sbsrefstate->upperindexnull[0] && workspace->indexOid[0] == INT4OID)
		workspace->expectArray = true;

	/* Process upper subscripts */
	for (int i = 0; i < sbsrefstate->numupper; i++)
	{
		if (sbsrefstate->upperprovided[i])
		{
			/* If any index expr yields NULL, result is NULL or error */
			if (sbsrefstate->upperindexnull[i])
			{
				if (sbsrefstate->isassignment)
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("jsonb subscript in assignment must not be null")));
				*op->resnull = true;
				return false;
			}

			/*
			 * For jsonb fetch and assign functions we need to provide path in
			 * text format. Convert if it's not already text.
			 */
			if (workspace->indexOid[i] == INT4OID)
			{
				Datum		datum = sbsrefstate->upperindex[i];
				char	   *cs = DatumGetCString(DirectFunctionCall1(int4out, datum));

				workspace->index[i] = CStringGetTextDatum(cs);
			}
			else
				workspace->index[i] = sbsrefstate->upperindex[i];
		}
	}

	return true;
}

/*
 * Evaluate SubscriptingRef fetch for a jsonb element.
 *
 * Source container is in step's result variable (it's known not NULL, since
 * we set fetch_strict to true).
 */
static void
jsonb_subscript_fetch(ExprState *state,
					  ExprEvalStep *op,
					  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;
	Jsonb	   *jsonbSource;

	/* Should not get here if source jsonb (or any subscript) is null */
	Assert(!(*op->resnull));

	jsonbSource = DatumGetJsonbP(*op->resvalue);
	*op->resvalue = jsonb_get_element(jsonbSource,
									  workspace->index,
									  sbsrefstate->numupper,
									  op->resnull,
									  false);
}

/*
 * Evaluate SubscriptingRef assignment for a jsonb element assignment.
 *
 * Input container (possibly null) is in result area, replacement value is in
 * SubscriptingRefState's replacevalue/replacenull.
 */
static void
jsonb_subscript_assign(ExprState *state,
					   ExprEvalStep *op,
					   ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;
	JsonbSubWorkspace *workspace = (JsonbSubWorkspace *) sbsrefstate->workspace;
	Jsonb	   *jsonbSource;
	JsonbValue	replacevalue;

	if (sbsrefstate->replacenull)
		replacevalue.type = jbvNull;
	else
		JsonbToJsonbValue(DatumGetJsonbP(sbsrefstate->replacevalue),
						  &replacevalue);

	/*
	 * In case if the input container is null, set up an empty jsonb and
	 * proceed with the assignment.
	 */
	if (*op->resnull)
	{
		JsonbValue	newSource;

		/*
		 * To avoid any surprising results, set up an empty jsonb array in
		 * case of an array is expected (i.e. the first subscript is integer),
		 * otherwise jsonb object.
		 */
		if (workspace->expectArray)
		{
			newSource.type = jbvArray;
			newSource.val.array.nElems = 0;
			newSource.val.array.rawScalar = false;
		}
		else
		{
			newSource.type = jbvObject;
			newSource.val.object.nPairs = 0;
		}

		jsonbSource = JsonbValueToJsonb(&newSource);
		*op->resnull = false;
	}
	else
		jsonbSource = DatumGetJsonbP(*op->resvalue);

	*op->resvalue = jsonb_set_element(jsonbSource,
									  workspace->index,
									  sbsrefstate->numupper,
									  &replacevalue);
	/* The result is never NULL, so no need to change *op->resnull */
}

/*
 * Compute old jsonb element value for a SubscriptingRef assignment
 * expression.  Will only be called if the new-value subexpression
 * contains SubscriptingRef or FieldStore.  This is the same as the
 * regular fetch case, except that we have to handle a null jsonb,
 * and the value should be stored into the SubscriptingRefState's
 * prevvalue/prevnull fields.
 */
static void
jsonb_subscript_fetch_old(ExprState *state,
						  ExprEvalStep *op,
						  ExprContext *econtext)
{
	SubscriptingRefState *sbsrefstate = op->d.sbsref.state;

	if (*op->resnull)
	{
		/* whole jsonb is null, so any element is too */
		sbsrefstate->prevvalue = (Datum) 0;
		sbsrefstate->prevnull = true;
	}
	else
	{
		Jsonb	   *jsonbSource = DatumGetJsonbP(*op->resvalue);

		sbsrefstate->prevvalue = jsonb_get_element(jsonbSource,
												   sbsrefstate->upperindex,
												   sbsrefstate->numupper,
												   &sbsrefstate->prevnull,
												   false);
	}
}

/*
 * Set up execution state for a jsonb subscript operation. Opposite to the
 * arrays subscription, there is no limit for number of subscripts as jsonb
 * type itself doesn't have nesting limits.
 */
static void
jsonb_exec_setup(const SubscriptingRef *sbsref,
				 SubscriptingRefState *sbsrefstate,
				 SubscriptExecSteps *methods)
{
	JsonbSubWorkspace *workspace;
	ListCell   *lc;
	int			nupper = sbsref->refupperindexpr->length;
	char	   *ptr;

	/* Allocate type-specific workspace with space for per-subscript data */
	workspace = palloc0(MAXALIGN(sizeof(JsonbSubWorkspace)) +
						nupper * (sizeof(Datum) + sizeof(Oid)));
	workspace->expectArray = false;
	ptr = ((char *) workspace) + MAXALIGN(sizeof(JsonbSubWorkspace));

	/*
	 * This coding assumes sizeof(Datum) >= sizeof(Oid), else we might
	 * misalign the indexOid pointer
	 */
	workspace->index = (Datum *) ptr;
	ptr += nupper * sizeof(Datum);
	workspace->indexOid = (Oid *) ptr;

	sbsrefstate->workspace = workspace;

	/* Collect subscript data types necessary at execution time */
	foreach(lc, sbsref->refupperindexpr)
	{
		Node	   *expr = lfirst(lc);
		int			i = foreach_current_index(lc);

		workspace->indexOid[i] = exprType(expr);
	}

	/*
	 * Pass back pointers to appropriate step execution functions.
	 */
	methods->sbs_check_subscripts = jsonb_subscript_check_subscripts;
	methods->sbs_fetch = jsonb_subscript_fetch;
	methods->sbs_assign = jsonb_subscript_assign;
	methods->sbs_fetch_old = jsonb_subscript_fetch_old;
}

/*
 * Convert JsonbValue to Datum. This function is used by
 * generic array iterator.
 */
static Datum
JsonbValueToDatum(JsonbValue *jbv,
				  Oid *typid, int32 *typmod, bool *isnull,
				  void **cache, MemoryContext mcxt,
				  Oid target_typid, int32 target_typmod,
				  ForeachAIteratorCoercionStrategy coercion_strategy)
{
	Datum		result;

	*isnull = false;
	*typmod = -1;

	if (coercion_strategy == COERCION_STRATEGY_COMMON_TYPE)
	{
		result = PointerGetDatum(JsonbValueToJsonb(jbv));
		*typid = JSONBOID;

		return result;
	}
	else if (coercion_strategy == COERCION_STRATEGY_COERCION)
	{
		Jsonb	   *jb = JsonbValueToJsonb(jbv);

		result = json_populate_type(PointerGetDatum(jb), JSONBOID,
									target_typid, target_typmod,
									cache, mcxt,
									isnull, false, NULL);

		*typid = target_typid;
		*typmod = target_typmod;

		return result;
	}
	else
	{
		/*
		 * These types can holds JSON null, so must be processed
		 * before processing jbvNull. We don't want to convert
		 * JSON null, to SQL null, when targer is of JSON.
		 */
		if (target_typid == JSONBOID || target_typid == JSONOID)
		{
			Jsonb	   *jb = JsonbValueToJsonb(jbv);

			if (target_typid == JSONOID)
			{
				char	   *str;

				str = JsonbToCString(NULL, &jb->root, VARSIZE(jb));
				result = PointerGetDatum(cstring_to_text(str));
			}
			else
				result = PointerGetDatum(jb);

			*typid = target_typid;
		}

		/*
		 * For special cases we can skip conversion to Jsonb
		 * and possibly IO cast.
		 */
		else if (jbv->type == jbvNull)
		{
			result = (Datum) 0;
			*isnull = true;
			*typid = target_typid;
		}
		else if (jbv->type == jbvString)
		{
			text	   *txt = cstring_to_text_with_len(jbv->val.string.val,
													   jbv->val.string.len);

			result = PointerGetDatum(txt);
			*typid = TEXTOID;
		}
		else if (jbv->type == jbvNumeric)
		{
			result = PointerGetDatum(jbv->val.numeric);
			*typid = NUMERICOID;
		}
		else if (jbv->type == jbvBool)
		{
			result = BoolGetDatum(jbv->val.boolean);
			*typid = BOOLOID;
		}

		/* generic conversion */
		else
		{
			Jsonb	   *jb = JsonbValueToJsonb(jbv);

			result = json_populate_type(PointerGetDatum(jb), JSONBOID,
										target_typid, target_typmod,
										cache, mcxt,
										isnull, false, NULL);

			*typid = target_typid;
			*typmod = target_typmod;
		}

		return result;
	}
}

static bool
jsonb_foreach_a_iterate(ForeachAIterator *self,
						Datum *value, bool *isnull,
						Oid *typid, int32 *typmod)
{
	JsonbForeachAIterator *iter = (JsonbForeachAIterator *) self;
	JsonbIteratorToken r;
	JsonbValue	jbv;

	while ((r = JsonbIteratorNext(&iter->it, &jbv, iter->skip_nested)) != WJB_DONE)
	{
		iter->skip_nested = true;

		if (r == WJB_ELEM)
		{
			*value = JsonbValueToDatum(&jbv, typid, typmod, isnull,
									   &iter->cache, iter->cache_mcxt,
									   iter->target_typid, iter->target_typmod,
									   iter->coercion_strategy);

			return true;
		}
	}

	return false;
}

/*
 * Create foreach array iterator for jsonb array
 */
static ForeachAIterator *
create_jsonb_foreach_a_iterator(Datum value, Oid typid, int32 typmod,
								int slice, ForeachAIteratorOptions *options)
{
	JsonbForeachAIterator *iter = palloc0(sizeof(JsonbForeachAIterator));
	Jsonb	   *jb;

	Assert(typid == JSONBOID);

	if (slice > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("jsonb array iterator doesn't support slicing")));

	jb = DatumGetJsonbP(value);

	/*
	 * Jsonb iterator is designed like jsonb_array_element. Input value
	 * must be json array.
	 */
	if (JB_ROOT_IS_SCALAR(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot iterate over a scalar value.")));
	else if (JB_ROOT_IS_OBJECT(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot iterate over an object value.")));

	Assert(JB_ROOT_IS_ARRAY(jb));

	iter->pub.iterate = jsonb_foreach_a_iterate;
	iter->it = JsonbIteratorInit(&jb->root);

	if (options)
	{
		iter->target_typid = options->target_typid;
		iter->target_typmod = options->target_typmod;
		iter->coercion_strategy = options->coercion_strategy;
	}
	else
	{
		iter->target_typid = InvalidOid;
		iter->target_typmod = -1;
		iter->coercion_strategy = COERCION_STRATEGY_COMMON_TYPE;
	}

	iter->cache_mcxt = CurrentMemoryContext;

	return (ForeachAIterator *) iter;
}

/*
 * jsonb_subscript_handler
 *		Subscripting handler for jsonb.
 *
 */
Datum
jsonb_subscript_handler(PG_FUNCTION_ARGS)
{
	static const SubscriptRoutines sbsroutines = {
		.transform = jsonb_subscript_transform,
		.exec_setup = jsonb_exec_setup,
		.fetch_strict = true,	/* fetch returns NULL for NULL inputs */
		.fetch_leakproof = true,	/* fetch returns NULL for bad subscript */
		.store_leakproof = false,	/* ... but assignment throws error */
		.create_foreach_a_iterator = create_jsonb_foreach_a_iterator
	};

	PG_RETURN_POINTER(&sbsroutines);
}
