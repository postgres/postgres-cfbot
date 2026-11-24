/*-------------------------------------------------------------------------
 *
 * execSubattr.c
 *	  Prove that an UPDATE cannot change what an expression or partial index
 *	  stores, from the shape of the SET expressions and the index
 *	  definitions alone.
 *
 * ExecUpdateModifiedIdxAttrs() finds the indexed columns an UPDATE changed
 * by comparing the old and new rows.  For a column used only through index
 * expressions or partial-index predicates it then evaluates those
 * expressions and predicates on both rows.  That is always right, but for a
 * large structured value (a jsonb document, an array, an extension's
 * document type) it reads the value twice per index, and often the SET
 * expression already tells us that the parts an index reads cannot have
 * changed:
 *
 *		CREATE INDEX ON t ((doc->>'name'));
 *		UPDATE t SET doc = jsonb_set(doc, '{visits}', ...);
 *
 * This module works that out once per statement, from the plan tree, so
 * that nothing at run time (the expression interpreter, JIT-compiled code,
 * the functions themselves) has to cooperate.  Functions
 * take part through their support function by answering a
 * SupportRequestSubattr: which parts of which argument a call reads, or
 * which parts of its source argument it may change.  Array and jsonb
 * subscripting are understood directly.  Anything this module does not
 * understand makes it give up for that index, and the executor then
 * evaluates it as before.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/executor/execSubattr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_type.h"
#include "executor/execSubattr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/params.h"
#include "nodes/supportnodes.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/* The column the walkers look for, and what they have found. */
typedef struct SubattrWalk
{
	int			varno;
	AttrNumber	attnum;
	bool		known;			/* false: could not tell */
	List	   *paths;			/* List of List of SubattrPathElem */
} SubattrWalk;

static bool read_walker(Node *node, SubattrWalk *walk);

/*
 * Only RelabelType is looked through: any other wrapper keeps an expression
 * from matching the target Var, which just loses the proof.
 */
static Node *
strip_relabel(Node *node)
{
	while (node && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
}

static bool
is_target_var(Node *node, SubattrWalk *walk)
{
	node = strip_relabel(node);
	return node && IsA(node, Var) &&
		((Var *) node)->varno == walk->varno &&
		((Var *) node)->varattno == walk->attnum &&
		((Var *) node)->varlevelsup == 0;
}

static List *
make_path_elem(SubattrPathElemKind kind, const char *key, int position)
{
	SubattrPathElem *e = palloc0_object(SubattrPathElem);

	e->kind = kind;
	e->key = key ? pstrdup(key) : NULL;
	e->position = position;
	return list_make1(e);
}

/* The function and arguments of a FuncExpr or OpExpr, else InvalidOid. */
static Oid
call_of(Node *node, List **args)
{
	if (IsA(node, FuncExpr))
	{
		*args = ((FuncExpr *) node)->args;
		return ((FuncExpr *) node)->funcid;
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;

		*args = op->args;
		return OidIsValid(op->opfuncid) ? op->opfuncid : get_opcode(op->opno);
	}
	return InvalidOid;
}

/*
 * Ask the support function of funcid about a call with these arguments.
 * Returns false if there is none or it declines.
 */
static bool
ask_support(Oid funcid, List *args, SubattrRequestKind kind,
			SupportRequestSubattr *req)
{
	RegProcedure prosupport = get_func_support(funcid);

	if (!OidIsValid(prosupport))
		return false;

	memset(req, 0, sizeof(*req));
	req->type = T_SupportRequestSubattr;
	req->kind = kind;
	req->funcid = funcid;
	req->args = args;
	req->source = -1;

	if (DatumGetPointer(OidFunctionCall1(prosupport, PointerGetDatum(req))) !=
		(Pointer) req)
		return false;
	if (req->source < 0 || req->source >= list_length(args))
		return false;

	/* an answer we can't read counts as no answer */
	foreach_ptr(List, path, req->paths)
	{
		foreach_ptr(SubattrPathElem, e, path)
		{
			if (e == NULL ||
				(e->kind != SUBATTR_ELEM_KEY &&
				 e->kind != SUBATTR_ELEM_POSITION &&
				 e->kind != SUBATTR_ELEM_ANY_POSITION) ||
				(e->kind == SUBATTR_ELEM_KEY && e->key == NULL))
				return false;
		}
	}
	return true;
}

/*
 * The path named by the subscripts of an array or jsonb subscripting
 * expression, if they are all constants.
 *
 * jsonb subscripting turns an integer subscript into its text form and then
 * reads each subscript as an object key or, in an array, as a position, so
 * jsonb subscripts are always keys here; see paths_may_overlap() for how a
 * key that reads as an integer is treated.
 */
static bool
subscript_path(SubscriptingRef *sbsref, List **path)
{
	ListCell   *lc;
	List	   *result = NIL;
	RegProcedure handler = get_typsubscript(sbsref->refcontainertype, NULL);

	if (handler != F_ARRAY_SUBSCRIPT_HANDLER &&
		handler != F_JSONB_SUBSCRIPT_HANDLER)
		return false;
	if (sbsref->reflowerindexpr != NIL)
		return false;			/* a slice */

	foreach(lc, sbsref->refupperindexpr)
	{
		Node	   *sub = strip_relabel(lfirst(lc));
		Const	   *c = (Const *) sub;

		if (!sub || !IsA(sub, Const) || c->constisnull)
			return false;
		if (handler == F_ARRAY_SUBSCRIPT_HANDLER && c->consttype == INT4OID)
			result = list_concat(result,
								 make_path_elem(SUBATTR_ELEM_POSITION, NULL,
												DatumGetInt32(c->constvalue)));
		else if (handler == F_JSONB_SUBSCRIPT_HANDLER &&
				 c->consttype == INT4OID)
			result = list_concat(result,
								 make_path_elem(SUBATTR_ELEM_KEY,
												psprintf("%d", DatumGetInt32(c->constvalue)),
												0));
		else if (handler == F_JSONB_SUBSCRIPT_HANDLER &&
				 c->consttype == TEXTOID)
			result = list_concat(result,
								 make_path_elem(SUBATTR_ELEM_KEY,
												TextDatumGetCString(c->constvalue),
												0));
		else
			return false;
	}
	*path = result;
	return true;
}

/*
 * If node is a chain of part-reading accessors ending at the target column,
 * return the paths it reads in *paths.  *subvalue says whether the result of
 * the chain is itself the part at the (single) path, so that an enclosing
 * accessor can extend the path.
 */
static bool
accessor_path(Node *node, SubattrWalk *walk, List **paths, bool *subvalue)
{
	List	   *args;
	Oid			funcid;

	check_stack_depth();

	node = strip_relabel(node);
	if (is_target_var(node, walk))
	{
		*paths = list_make1(NIL);
		*subvalue = true;
		return true;
	}

	if (IsA(node, SubscriptingRef) &&
		((SubscriptingRef *) node)->refassgnexpr == NULL)
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) node;
		List	   *inner;
		List	   *mine;
		bool		innersub;

		if (!accessor_path((Node *) sbsref->refexpr, walk, &inner, &innersub) ||
			!innersub || !subscript_path(sbsref, &mine))
			return false;
		*paths = list_make1(list_concat(linitial(inner), mine));
		*subvalue = true;		/* fetching an element yields the element */
		return true;
	}

	if (IsA(node, FieldSelect))
	{
		FieldSelect *fselect = (FieldSelect *) node;
		List	   *inner;
		bool		innersub;

		if (!accessor_path((Node *) fselect->arg, walk, &inner, &innersub) ||
			!innersub)
			return false;
		*paths = list_make1(list_concat(linitial(inner),
										make_path_elem(SUBATTR_ELEM_POSITION,
													   NULL,
													   fselect->fieldnum)));
		*subvalue = true;
		return true;
	}

	funcid = call_of(node, &args);
	if (OidIsValid(funcid))
	{
		SupportRequestSubattr req;
		List	   *inner;
		bool		innersub;
		ListCell   *lc;

		/*
		 * A call that reads several paths cannot claim to return the part at
		 * a path, so nothing can extend them.  An extended path is only
		 * understood if the inner chain names one part.
		 */
		if (!ask_support(funcid, args, SUBATTR_READ, &req) ||
			req.paths == NIL ||
			(req.subvalue && list_length(req.paths) != 1) ||
			!accessor_path(list_nth(args, req.source), walk, &inner, &innersub) ||
			!innersub)
			return false;

		/* The other arguments must not read the column at all. */
		for (int i = 0; i < list_length(args); i++)
		{
			SubattrWalk other = {walk->varno, walk->attnum, true, NIL};

			if (i == req.source)
				continue;
			read_walker(list_nth(args, i), &other);
			if (other.paths != NIL || !other.known)
				return false;
		}

		*paths = NIL;
		foreach(lc, req.paths)
		{
			List	   *path = list_concat_copy(linitial(inner), lfirst(lc));

			*paths = lappend(*paths, path);
		}
		*subvalue = req.subvalue;
		return true;
	}

	return false;
}

/*
 * Collect, into walk->paths, every part of the target column that "node"
 * reads.  A reference to the column that is not the start of an accessor
 * chain we understand reads the whole value.
 */
static bool
read_walker(Node *node, SubattrWalk *walk)
{
	List	   *paths;
	bool		subvalue;

	if (node == NULL)
		return false;

	if (accessor_path(node, walk, &paths, &subvalue))
	{
		walk->paths = list_concat(walk->paths, paths);
		return false;
	}

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* a whole-row reference reads every column */
		if (var->varno == walk->varno && var->varattno == InvalidAttrNumber &&
			var->varlevelsup == 0)
			walk->known = false;
		return false;
	}

	if (IsA(node, SubLink) || IsA(node, SubPlan))
	{
		walk->known = false;	/* not attempting to see inside */
		return true;
	}

	return expression_tree_walker(node, read_walker, walk);
}

/*
 * Record the parts of the target column that the SET expression "expr" may
 * change.  It must be the column itself, passed through modifications we
 * understand; anything else is unknown.
 */
static void
collect_modifies(Node *expr, SubattrWalk *walk)
{
	Oid			funcid;
	List	   *args;

	check_stack_depth();
	expr = strip_relabel(expr);
	if (!walk->known)
		return;
	if (expr == NULL)
	{
		walk->known = false;
		return;
	}

	if (is_target_var(expr, walk))
		return;					/* this part is unchanged */

	/* a domain check passes the value through or fails */
	if (IsA(expr, CoerceToDomain))
	{
		collect_modifies((Node *) ((CoerceToDomain *) expr)->arg, walk);
		return;
	}

	if (IsA(expr, SubscriptingRef) &&
		((SubscriptingRef *) expr)->refassgnexpr != NULL)
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) expr;
		List	   *path;

		/*
		 * col[subscripts] := value.  Assigning beyond the bounds of an array
		 * fills the gap with nulls, which read the same as the missing
		 * elements did; jsonb assignment may create containers on the way to
		 * the path, but those are at a prefix of it, which overlaps.
		 */
		if (!subscript_path(sbsref, &path))
		{
			walk->known = false;
			return;
		}
		walk->paths = lappend(walk->paths, path);
		collect_modifies((Node *) sbsref->refexpr, walk);
		return;
	}

	if (IsA(expr, FieldStore))
	{
		FieldStore *fstore = (FieldStore *) expr;
		ListCell   *lc;

		/* col.field := value, for one or more fields */
		foreach(lc, fstore->fieldnums)
			walk->paths = lappend(walk->paths,
								  make_path_elem(SUBATTR_ELEM_POSITION, NULL,
												 lfirst_int(lc)));
		collect_modifies((Node *) fstore->arg, walk);
		return;
	}

	funcid = call_of(expr, &args);
	if (OidIsValid(funcid))
	{
		SupportRequestSubattr req;

		if (ask_support(funcid, args, SUBATTR_MODIFY, &req))
		{
			walk->paths = list_concat(walk->paths, req.paths);
			collect_modifies(list_nth(args, req.source), walk);
			return;
		}
	}

	/* COALESCE(f(col, ...), col): each branch is the column, modified */
	if (IsA(expr, CoalesceExpr))
	{
		ListCell   *lc;

		foreach(lc, ((CoalesceExpr *) expr)->args)
			collect_modifies(lfirst(lc), walk);
		return;
	}

	walk->known = false;
}

/*
 * Could the key be read as an array position?  jsonb reads a path element
 * as a position with strtoint(), which allows leading white space and a
 * sign.  Trailing white space is allowed too: answering yes too often only
 * makes two paths overlap that need not.  White space is what strtol()
 * skips, isspace() in the current locale; digits are only '0' to '9'
 * there, so they are tested by hand.
 */
static bool
key_may_be_position(const char *key)
{
	const char *p = key;

	while (isspace((unsigned char) *p))
		p++;
	if (*p == '-' || *p == '+')
		p++;
	if (*p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9')
		p++;
	while (isspace((unsigned char) *p))
		p++;
	return *p == '\0';
}

/*
 * Can paths a and b name overlapping parts?  See SupportRequestSubattr.
 *
 * Two keys that both read as integers are taken to overlap even if they
 * differ: "01" and "1" select the same array element, and inserting into or
 * deleting from an array, or appending past its end, moves or creates
 * elements at positions other than the one named.
 */
static bool
paths_may_overlap(List *a, List *b)
{
	ListCell   *la,
			   *lb;

	forboth(la, a, lb, b)
	{
		SubattrPathElem *ea = lfirst(la);
		SubattrPathElem *eb = lfirst(lb);

		if (ea->kind == SUBATTR_ELEM_KEY && eb->kind == SUBATTR_ELEM_KEY)
		{
			if (strcmp(ea->key, eb->key) != 0 &&
				!(key_may_be_position(ea->key) &&
				  key_may_be_position(eb->key)))
				return false;
		}
		else if (ea->kind == SUBATTR_ELEM_KEY || eb->kind == SUBATTR_ELEM_KEY)
		{
			/* a key against a position or any position */
			SubattrPathElem *key = ea->kind == SUBATTR_ELEM_KEY ? ea : eb;

			if (!key_may_be_position(key->key))
				return false;
		}
		else if (ea->kind == SUBATTR_ELEM_POSITION &&
				 eb->kind == SUBATTR_ELEM_POSITION)
		{
			if (ea->position != eb->position)
				return false;
		}
		else
		{
			/* any position against a position or any position: overlap */
			Assert(ea->kind == SUBATTR_ELEM_ANY_POSITION ||
				   eb->kind == SUBATTR_ELEM_ANY_POSITION);
		}
	}
	return true;
}

/*
 * Replace external Params whose values are fixed for the execution by
 * Consts, as eval_const_expressions() does, so that support functions see
 * the values of the arguments of a generic plan.  Nothing is evaluated.
 */
static Node *
substitute_params(Node *node, ParamListInfo params)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXTERN)
	{
		Param	   *param = (Param *) node;
		ParamExternData prmdata;
		ParamExternData *prm;

		if (param->paramid <= 0 || param->paramid > params->numParams)
			return node;
		if (params->paramFetch != NULL)
			prm = params->paramFetch(params, param->paramid, true, &prmdata);
		else
			prm = &params->params[param->paramid - 1];

		if (prm != NULL && prm->ptype == param->paramtype &&
			OidIsValid(prm->ptype) &&
			(prm->pflags & PARAM_FLAG_CONST))
		{
			int16		typlen;
			bool		typbyval;

			get_typlenbyval(param->paramtype, &typlen, &typbyval);
			return (Node *) makeConst(param->paramtype, param->paramtypmod,
									  param->paramcollid, (int) typlen,
									  prm->isnull ? (Datum) 0 :
									  datumCopy(prm->value, typbyval, typlen),
									  prm->isnull, typbyval);
		}
		return node;
	}
	return expression_tree_mutator(node, substitute_params, params);
}

/*
 * ExecSubattrProveUntouched
 *
 * setexpr computes column attnum of the result relation in an UPDATE, with
 * Vars of varno referring to the row being updated, and Params whose values
 * are in params.  Return the indexes,
 * as positions in resultRelInfo's index arrays, that provably read nothing
 * of the column that setexpr may change, provided the column is not null in
 * the old or the new row.  An index that uses the column in any way we do
 * not understand, or as a plain key or INCLUDE column, is never included.
 */
Bitmapset *
ExecSubattrProveUntouched(ResultRelInfo *resultRelInfo, Node *setexpr,
						  int varno, AttrNumber attnum, ParamListInfo params)
{
	SubattrWalk mods = {varno, attnum, true, NIL};
	Bitmapset  *result = NULL;

	if (params != NULL && params->numParams > 0)
		setexpr = substitute_params(setexpr, params);
	collect_modifies(setexpr, &mods);
	if (!mods.known || mods.paths == NIL)
		return NULL;

	for (int i = 0; i < resultRelInfo->ri_NumIndices; i++)
	{
		IndexInfo  *ii = resultRelInfo->ri_IndexRelationInfo[i];
		SubattrWalk reads = {1, attnum, true, NIL};
		bool		disjoint = true;
		ListCell   *lr;

		if (resultRelInfo->ri_IndexRelationDescs[i] == NULL)
			continue;
		for (int k = 0; k < ii->ii_NumIndexAttrs; k++)
		{
			if (ii->ii_IndexAttrNumbers[k] == attnum)
				reads.known = false;
		}
		if (!reads.known)
			continue;

		/* index expressions and predicates refer to the table as varno 1 */
		read_walker((Node *) ii->ii_Expressions, &reads);
		read_walker((Node *) ii->ii_Predicate, &reads);
		if (!reads.known || reads.paths == NIL)
			continue;

		foreach(lr, reads.paths)
		{
			ListCell   *lm;

			foreach(lm, mods.paths)
			{
				if (paths_may_overlap(lfirst(lr), lfirst(lm)))
				{
					disjoint = false;
					break;
				}
			}
			if (!disjoint)
				break;
		}
		if (disjoint)
			result = bms_add_member(result, i);
	}

	return result;
}
