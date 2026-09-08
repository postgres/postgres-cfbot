/*-------------------------------------------------------------------------
 *
 * parse_graphtable.c
 *	  parsing of GRAPH_TABLE
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_graphtable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_propgraph_label.h"
#include "catalog/pg_propgraph_property.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_clause.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_graphtable.h"
#include "parser/parse_node.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/*
 * Return human-readable name of the type of graph element pattern in
 * GRAPH_TABLE clause, usually for error message purpose.
 */
static const char *
get_gep_kind_name(GraphElementPatternKind gepkind)
{
	switch (gepkind)
	{
		case VERTEX_PATTERN:
			return "vertex";
		case EDGE_PATTERN_LEFT:
			return "edge pointing left";
		case EDGE_PATTERN_RIGHT:
			return "edge pointing right";
		case EDGE_PATTERN_ANY:
			return "edge pointing any direction";
		case PAREN_EXPR:
			return "nested path pattern";
	}

	/*
	 * When a GraphElementPattern is constructed by the parser, it will set a
	 * value from the GraphElementPatternKind enum. But we may get here if the
	 * GraphElementPatternKind value stored in a catalog is corrupted.
	 */
	return "unknown";
}

/*
 * Transform a property reference.
 *
 * A property reference is parsed as a ColumnRef of the form:
 * <variable>.<property>. If <variable> is one of the variables bound to an
 * element pattern in the graph pattern and <property> can be resolved as a
 * property of the property graph, then we return a GraphPropertyRef node
 * representing the property reference. If the <variable> exists in the graph
 * pattern but <property> does not exist in the property graph, we raise an
 * error. However, if <variable> does not exist in the graph pattern, we return
 * NULL to let the caller handle it as some other kind of ColumnRef. The
 * variables bound to the element patterns in the graph pattern are expected to
 * be collected in the GraphTableParseState.
 */
Node *
transformGraphTablePropertyRef(ParseState *pstate, ColumnRef *cref)
{
	GraphTableParseState *gpstate = pstate->p_graph_table_pstate;

	if (!gpstate)
		return NULL;

	if (list_length(cref->fields) == 2)
	{
		Node	   *field1 = linitial(cref->fields);
		Node	   *field2 = lsecond(cref->fields);
		char	   *elvarname;
		char	   *propname;

		if (IsA(field1, A_Star) || IsA(field2, A_Star))
		{
			if (pstate->p_expr_kind == EXPR_KIND_SELECT_TARGET)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("\"*\" is not supported here"),
						parser_errposition(pstate, cref->location));
			else
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("\"*\" not allowed here"),
						parser_errposition(pstate, cref->location));
		}

		elvarname = strVal(field1);
		propname = strVal(field2);

		if (list_member(gpstate->variables, field1))
		{
			GraphPropertyRef *gpr;
			HeapTuple	pgptup;
			Form_pg_propgraph_property pgpform;

			/*
			 * If we are transforming expression in an element pattern,
			 * property references containing only that variable are allowed.
			 */
			if (gpstate->cur_gep)
			{
				if (!gpstate->cur_gep->variable ||
					strcmp(elvarname, gpstate->cur_gep->variable) != 0)
					ereport(ERROR,
							errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("non-local element variable reference is not supported"),
							parser_errposition(pstate, cref->location));
			}

			gpr = makeNode(GraphPropertyRef);
			pgptup = SearchSysCache2(PROPGRAPHPROPNAME, ObjectIdGetDatum(gpstate->graphid), CStringGetDatum(propname));
			if (!HeapTupleIsValid(pgptup))
				ereport(ERROR,
						errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("property \"%s\" does not exist", propname));
			pgpform = (Form_pg_propgraph_property) GETSTRUCT(pgptup);

			gpr->location = cref->location;
			gpr->elvarname = elvarname;
			gpr->propid = pgpform->oid;
			gpr->typeId = pgpform->pgptypid;
			gpr->typmod = pgpform->pgptypmod;
			gpr->collation = pgpform->pgpcollation;

			ReleaseSysCache(pgptup);

			return (Node *) gpr;
		}
	}

	return NULL;
}

/*
 * Transform a label expression.
 *
 * A label expression is parsed as either a ColumnRef with a single field or a
 * label expression like label disjunction. The single field in the ColumnRef is
 * treated as a label name and transformed to a GraphLabelRef node. The label
 * expression is recursively transformed into an expression tree containing
 * GraphLabelRef nodes corresponding to the names of the labels appearing in the
 * expression. If any label name cannot be resolved to a label in the property
 * graph, an error is raised.
 */
static Node *
transformLabelExpr(GraphTableParseState *gpstate, Node *labelexpr)
{
	Node	   *result;

	if (labelexpr == NULL)
		return NULL;

	check_stack_depth();

	switch (nodeTag(labelexpr))
	{
		case T_ColumnRef:
			{
				ColumnRef  *cref = (ColumnRef *) labelexpr;
				const char *labelname;
				Oid			labelid;
				GraphLabelRef *lref;

				Assert(list_length(cref->fields) == 1);
				labelname = strVal(linitial(cref->fields));

				labelid = GetSysCacheOid2(PROPGRAPHLABELNAME, Anum_pg_propgraph_label_oid, ObjectIdGetDatum(gpstate->graphid), CStringGetDatum(labelname));
				if (!labelid)
					ereport(ERROR,
							errcode(ERRCODE_UNDEFINED_OBJECT),
							errmsg("label \"%s\" does not exist in property graph \"%s\"", labelname, get_rel_name(gpstate->graphid)));

				lref = makeNode(GraphLabelRef);
				lref->labelid = labelid;
				lref->location = cref->location;

				result = (Node *) lref;
				break;
			}

		case T_BoolExpr:
			{
				BoolExpr   *be = (BoolExpr *) labelexpr;
				ListCell   *lc;
				List	   *args = NIL;

				foreach(lc, be->args)
				{
					Node	   *arg = (Node *) lfirst(lc);

					arg = transformLabelExpr(gpstate, arg);
					args = lappend(args, arg);
				}

				result = (Node *) makeBoolExpr(be->boolop, args, be->location);
				break;
			}

		default:
			/* should not reach here */
			elog(ERROR, "unsupported label expression node: %d", (int) nodeTag(labelexpr));
			result = NULL;		/* keep compiler quiet */
			break;
	}

	return result;
}

/*
 * Transform a GraphElementPattern.
 *
 * Transform the label expression and the where clause in the element pattern
 * given by GraphElementPattern. The variable name in the GraphElementPattern is
 * added to the list of variables in the GraphTableParseState which is used to
 * resolve property references in this element pattern or elsewhere in the
 * GRAPH_TABLE.
 */
static Node *
transformGraphElementPattern(ParseState *pstate, GraphElementPattern *gep)
{
	GraphTableParseState *gpstate = pstate->p_graph_table_pstate;

	if (gep->quantifier)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("element pattern quantifier is not supported")));

	Assert(!gpstate->cur_gep);

	gpstate->cur_gep = gep;

	gep->labelexpr = transformLabelExpr(gpstate, gep->labelexpr);

	gep->whereClause = transformWhereClause(pstate, gep->whereClause,
											EXPR_KIND_WHERE, "WHERE");

	/*
	 * Assign collations here for the reason mentioned in the prologue of
	 * transformGraphPattern().
	 */
	assign_expr_collations(pstate, gep->whereClause);

	gpstate->cur_gep = NULL;

	return (Node *) gep;
}

/*
 * Transform a path term (list of GraphElementPattern's).
 */
static Node *
transformPathTerm(ParseState *pstate, List *path_term)
{
	List	   *result = NIL;
	GraphElementPattern *prev_gep = NULL;

	foreach_node(GraphElementPattern, gep, path_term)
	{
		if (gep->kind != VERTEX_PATTERN && !IS_EDGE_PATTERN(gep->kind))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported element pattern kind: \"%s\"", get_gep_kind_name(gep->kind)),
					 parser_errposition(pstate, gep->location)));

		if (IS_EDGE_PATTERN(gep->kind))
		{
			if (!prev_gep)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("path pattern cannot start with an edge pattern"),
						 parser_errposition(pstate, gep->location)));
			else if (prev_gep->kind != VERTEX_PATTERN)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("edge pattern must be preceded by a vertex pattern"),
						 parser_errposition(pstate, gep->location)));
		}
		else
		{
			if (prev_gep && !IS_EDGE_PATTERN(prev_gep->kind))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("adjacent vertex patterns are not supported"),
						 parser_errposition(pstate, gep->location)));
		}

		result = lappend(result,
						 transformGraphElementPattern(pstate, gep));
		prev_gep = gep;
	}

	/* Path pattern should have at least one element pattern. */
	Assert(prev_gep);

	if (IS_EDGE_PATTERN(prev_gep->kind))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("path pattern cannot end with an edge pattern"),
				 parser_errposition(pstate, prev_gep->location)));
	}

	return (Node *) result;
}

/*
 * Transform a path pattern list (list of path terms).
 */
static Node *
transformPathPatternList(ParseState *pstate, List *path_pattern)
{
	List	   *result = NIL;
	GraphTableParseState *gpstate = pstate->p_graph_table_pstate;

	Assert(gpstate);

	/* Grammar doesn't allow empty path pattern list */
	Assert(list_length(path_pattern) > 0);

	/*
	 * We do not support multiple path patterns in one GRAPH_TABLE clause
	 * right now. But we may do so in future.
	 */
	if (list_length(path_pattern) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("multiple path patterns in one GRAPH_TABLE clause not supported")));

	/*
	 * Collect all the variables in the path pattern into the
	 * GraphTableParseState so that we can detect any non-local element
	 * variable references. We need to do this before transforming the path
	 * pattern so as to detect forward references to element variables in the
	 * WHERE clause of an element pattern.
	 */
	foreach_node(List, path_term, path_pattern)
	{
		foreach_node(GraphElementPattern, gep, path_term)
		{
			if (gep->variable)
				gpstate->variables = list_append_unique(gpstate->variables, makeString(pstrdup(gep->variable)));
		}
	}

	foreach_node(List, path_term, path_pattern)
		result = lappend(result, transformPathTerm(pstate, path_term));

	return (Node *) result;
}

/*
 * Transform a GraphPattern.
 *
 * A GraphPattern consists of a list of one or more path patterns and an
 * optional where clause. Transform them. We use the previously constructed
 * list of variables in the GraphTableParseState to resolve property references
 * in the WHERE clause.
 *
 * Since most parts of the GraphPattern do not require collation assignment, we
 * assign collations to the required expressions as they are transformed.  This
 * avoids the need to traverse the whole GraphPattern again and avoids exposing
 * it to assign_expr_collations().
 */
Node *
transformGraphPattern(ParseState *pstate, GraphPattern *graph_pattern)
{
	List	   *path_pattern_list = castNode(List,
											 transformPathPatternList(pstate, graph_pattern->path_pattern_list));

	graph_pattern->path_pattern_list = path_pattern_list;
	graph_pattern->whereClause = transformWhereClause(pstate, graph_pattern->whereClause,
													  EXPR_KIND_WHERE, "WHERE");
	assign_expr_collations(pstate, graph_pattern->whereClause);

	return (Node *) graph_pattern;
}

/*
 * Collect label OIDs from a label expression (a single GraphLabelRef or an
 * OR tree of GraphLabelRef nodes) into a list.  Returns NIL if labelexpr is
 * NULL; callers decide what a label-less element pattern means (the graph
 * rewrite fallback treats it as "all labels", see
 * get_graph_all_label_oids()).  Shared by the parser, the native planner and
 * executor, and the graph rewrite fallback.
 */
List *
get_label_oids_for_labelexpr(Node *labelexpr)
{
	List	   *result = NIL;

	if (labelexpr == NULL)
		return NIL;

	if (IsA(labelexpr, GraphLabelRef))
	{
		GraphLabelRef *lref = (GraphLabelRef *) labelexpr;

		result = lappend_oid(result, lref->labelid);
	}
	else if (IsA(labelexpr, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) labelexpr;

		foreach_ptr(Node, arg, b->args)
		{
			List	   *sub = get_label_oids_for_labelexpr(arg);

			if (sub != NIL)
				result = list_concat(result, sub);
		}
	}
	else
	{
		/*
		 * Should not reach here: gram.y only generates label expressions
		 * built from GraphLabelRef and OR.
		 */
		elog(ERROR, "unsupported label expression node: %d",
			 (int) nodeTag(labelexpr));
	}

	return result;
}

/*
 * Return the OIDs of all labels belonging to the given property graph.
 *
 * A graph element pattern without a label expression is equivalent to
 * "%|!%" (SQL/PGQ 9.2 subclause 2.a.ii), i.e. it matches every label of the
 * graph.
 */
List *
get_graph_all_label_oids(Oid propgraphid)
{
	List	   *label_oids = NIL;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;

	rel = table_open(PropgraphLabelRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_label_pglpgid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(propgraphid));
	scan = systable_beginscan(rel, PropgraphLabelGraphNameIndexId,
							  true, NULL, 1, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_propgraph_label label = (Form_pg_propgraph_label) GETSTRUCT(tup);

		label_oids = lappend_oid(label_oids, label->oid);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return label_oids;
}

/*
 * Map a graph element pattern kind to the element-kind character ('v' for
 * vertex, 'e' for edge) and the human-readable class name ("vertex"/"edge").
 * Returns false for pattern kinds that do not denote a vertex or edge (e.g.
 * PAREN_EXPR), leaving the outputs untouched.  kind_str may be NULL if the
 * caller only needs the character.  Shared by the parser validators, the
 * native planner, and the native executor.
 */
bool
graph_element_kind_info(GraphElementPatternKind kind,
						char *element_kind, const char **kind_str)
{
	switch (kind)
	{
		case VERTEX_PATTERN:
			*element_kind = 'v';
			if (kind_str)
				*kind_str = "vertex";
			return true;
		case EDGE_PATTERN_ANY:
		case EDGE_PATTERN_RIGHT:
		case EDGE_PATTERN_LEFT:
			*element_kind = 'e';
			if (kind_str)
				*kind_str = "edge";
			return true;
		default:
			return false;
	}
}

/*
 * Match a label expression against an element, using the supplied
 * membership callback.  A label expression is a single GraphLabelRef or a
 * BoolExpr (OR) tree of GraphLabelRef nodes; the element matches if it
 * carries any of the referenced labels (OR semantics).  A NULL labelexpr
 * matches everything.
 *
 * See graph_label_expr_matches() in parse_graphtable.h for the shared API.
 */
bool
graph_label_expr_matches(Node *labelexpr, GraphLabelHasFn has_label,
						 void *arg)
{
	if (labelexpr == NULL)
		return true;
	if (IsA(labelexpr, GraphLabelRef))
		return has_label(((GraphLabelRef *) labelexpr)->labelid, arg);
	if (IsA(labelexpr, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) labelexpr;

		foreach_ptr(Node, sub, b->args)
		{
			if (graph_label_expr_matches(sub, has_label, arg))
				return true;
		}
		return false;
	}
	elog(ERROR, "unsupported label expression node: %d",
		 (int) nodeTag(labelexpr));
	return false;				/* keep compiler quiet */
}
