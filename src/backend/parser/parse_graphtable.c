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
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_propgraph_element_label.h"
#include "catalog/pg_propgraph_label.h"
#include "catalog/pg_propgraph_property.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
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

			/*
			 * A property reference made outside any element pattern (COLUMNS
			 * clause or graph-level WHERE) to a variable bound to a
			 * quantified (variable-length) edge denotes the list of the
			 * property's values over every edge traversed along the matched
			 * path.  Represent that as an array of the property's type.
			 * Inside the edge's own [e WHERE ...] clause cur_gep is set and e
			 * refers to the single candidate edge, so no array is built.
			 */
			if (gpstate->cur_gep == NULL)
			{
				foreach_node(GraphElementPattern, gep, gpstate->pattern_elements)
				{
					Oid			arrtype;

					if (!gep->variable ||
						strcmp(gep->variable, elvarname) != 0)
						continue;
					if (!IS_EDGE_PATTERN(gep->kind) || !gep->quantifier)
						break;
					arrtype = get_array_type(pgpform->pgptypid);
					if (arrtype == InvalidOid)
						arrtype = pgpform->pgptypid;
					gpr->vle_list = true;
					gpr->typeId = arrtype;
					gpr->typmod = -1;
					break;
				}
			}

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

	/* Quantifiers are handled natively; the rewrite fallback cannot use them. */
	if (!enable_native_graphtable && gep->quantifier)
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
			gpstate->pattern_elements = lappend(gpstate->pattern_elements, gep);
		}
	}

	foreach_node(List, path_term, path_pattern)
		result = lappend(result, transformPathTerm(pstate, path_term));

	return (Node *) result;
}

/*
 * Validate that no two element patterns in the same path have the same
 * variable name with incompatible definitions.
 *
 * Cases:
 * 1. Same variable, different element kinds (vertex vs edge) → error
 * 2. Same variable, both vertex, different label expressions → error
 * 3. Same variable, both vertex, compatible label expressions → allowed
 */
static void
validate_element_variable_names(GraphPattern *pattern)
{
	List	   *path;
	int			path_len;
	int			i;

	if (pattern == NULL ||
		pattern->path_pattern_list == NIL)
		return;

	path = linitial(pattern->path_pattern_list);
	if (path == NULL)
		return;
	path_len = list_length(path);

	for (i = 0; i < path_len; i++)
	{
		GraphElementPattern *gep_i = (GraphElementPattern *)
			list_nth(path, i);
		int			j;

		if (gep_i == NULL || gep_i->variable == NULL)
			continue;

		for (j = i + 1; j < path_len; j++)
		{
			GraphElementPattern *gep_j = (GraphElementPattern *)
				list_nth(path, j);

			if (gep_j == NULL || gep_j->variable == NULL)
				continue;

			if (strcmp(gep_i->variable, gep_j->variable) != 0)
				continue;

			/*
			 * Same variable name.  Check if element pattern types differ
			 * (vertex vs edge).
			 */
			if (gep_i->kind != gep_j->kind &&
				!(IS_EDGE_PATTERN(gep_i->kind) &&
				  IS_EDGE_PATTERN(gep_j->kind)))
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("element patterns with same variable "
								"name \"%s\" but different element "
								"pattern types", gep_i->variable)));
			}

			/*
			 * Same kind (both vertex or both edge).  Check label expressions.
			 * Error only if both have non-NULL label expressions that differ.
			 */
			if (gep_i->labelexpr != NULL && gep_j->labelexpr != NULL &&
				!equal(gep_i->labelexpr, gep_j->labelexpr))
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("element patterns with same "
								"variable name \"%s\" but "
								"different label expressions "
								"are not supported",
								gep_i->variable)));
			}
		}
	}
}

/*
 * Validate that an edge variable appearing at multiple hop positions
 * always connects the same pair of vertex variables.  For example, in
 * (a)->[e]->(b)<-[e]-(c), the edge e appears twice connecting a→b and
 * c→b, which requires e to connect three vertices — an impossibility.
 */
static void
validate_edge_connectivity(GraphPattern *pattern)
{
	List	   *path;
	int			path_len;
	int			n_hops;

	if (pattern == NULL ||
		pattern->path_pattern_list == NIL)
		return;

	path = linitial(pattern->path_pattern_list);
	if (path == NULL)
		return;
	path_len = list_length(path);
	if (path_len < 4)			/* need at least (v)-[e]->(v)-[e]->(v) */
		return;

	n_hops = (path_len - 1) / 2;
	{
		int			hi;

		for (hi = 0; hi < n_hops; hi++)
		{
			int			edge_pi = 2 * hi + 1;
			GraphElementPattern *edge_gep = (GraphElementPattern *)
				list_nth(path, edge_pi);
			int			ji;

			if (edge_gep == NULL || edge_gep->variable == NULL ||
				!IS_EDGE_PATTERN(edge_gep->kind))
				continue;

			for (ji = hi + 1; ji < n_hops; ji++)
			{
				int			je_pi = 2 * ji + 1;
				GraphElementPattern *je_gep = (GraphElementPattern *)
					list_nth(path, je_pi);
				int			src_pi_i;
				int			dst_pi_i;
				int			src_pi_j;
				int			dst_pi_j;
				GraphElementPattern *src_gep_i;
				GraphElementPattern *dst_gep_i;
				GraphElementPattern *src_gep_j;
				GraphElementPattern *dst_gep_j;

				if (je_gep == NULL || je_gep->variable == NULL ||
					!IS_EDGE_PATTERN(je_gep->kind))
					continue;

				if (strcmp(edge_gep->variable, je_gep->variable) != 0)
					continue;

				/* Same edge variable at two hop positions */
				src_pi_i = 2 * hi;
				dst_pi_i = 2 * hi + 2;
				src_pi_j = 2 * ji;
				dst_pi_j = 2 * ji + 2;

				src_gep_i = (GraphElementPattern *) list_nth(path, src_pi_i);
				dst_gep_i = (GraphElementPattern *) list_nth(path, dst_pi_i);
				src_gep_j = (GraphElementPattern *) list_nth(path, src_pi_j);
				dst_gep_j = (GraphElementPattern *) list_nth(path, dst_pi_j);

				/*
				 * An edge variable connecting multiple hops must have the
				 * same source and destination vertex variables in each hop.
				 * If either side differs, the edge would need to connect more
				 * than two distinct vertices.
				 */
				if ((src_gep_i->variable == NULL ||
					 dst_gep_i->variable == NULL ||
					 src_gep_j->variable == NULL ||
					 dst_gep_j->variable == NULL) ||
					strcmp(src_gep_i->variable, src_gep_j->variable) != 0 ||
					strcmp(dst_gep_i->variable, dst_gep_j->variable) != 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("an edge cannot connect more than two "
									"vertices even in a cyclic pattern")));
				}
			}
		}
	}
}

/*
 * Check that each label used in an element pattern matches the element's
 * kind (vertex or edge).  A label that only appears on vertex elements
 * cannot be used in an edge pattern, and vice versa.
 *
 * Called at parse time, when the pattern is transformed.  Unlike the label
 * matching done later during planning/execution, this check runs once per
 * query parse; DDL changes made after the query was parsed (e.g. a label
 * dropped from all vertex tables after a prepared statement was created)
 * are caught by the planner-side element resolution instead.
 */
void
validate_graph_element_label_kinds(GraphPattern *pattern, Oid graph_oid)
{
	List	   *path;
	int			path_len;
	int			pi;

	if (pattern == NULL ||
		pattern->path_pattern_list == NIL)
		return;

	path = linitial(pattern->path_pattern_list);
	if (path == NULL)
		return;
	path_len = list_length(path);

	for (pi = 0; pi < path_len; pi++)
	{
		GraphElementPattern *gep = (GraphElementPattern *)
			list_nth(path, pi);
		List	   *label_oids;
		ListCell   *lc;
		char		elem_kind;
		const char *kind_str;

		if (gep == NULL || gep->labelexpr == NULL)
			continue;

		/*
		 * Determine what kind this pattern element should be.
		 */
		if (!graph_element_kind_info(gep->kind, &elem_kind, &kind_str))
			continue;

		label_oids = get_label_oids_for_labelexpr(gep->labelexpr);
		if (label_oids == NIL)
			continue;

		/*
		 * For each label OID, scan pg_propgraph_element_label joined with
		 * pg_propgraph_element to find what element kinds this label is
		 * associated with.
		 */
		foreach(lc, label_oids)
		{
			Oid			labelid = lfirst_oid(lc);
			Relation	el_label_rel;
			SysScanDesc el_label_scan;
			ScanKeyData el_label_key[1];
			HeapTuple	el_label_tup;
			bool		has_vertex = false;
			bool		has_edge = false;

			el_label_rel = table_open(PropgraphElementLabelRelationId,
									  AccessShareLock);
			ScanKeyInit(&el_label_key[0],
						Anum_pg_propgraph_element_label_pgellabelid,
						BTEqualStrategyNumber,
						F_OIDEQ, ObjectIdGetDatum(labelid));
			el_label_scan = systable_beginscan(el_label_rel,
											   PropgraphElementLabelLabelIndexId,
											   true, NULL, 1, el_label_key);

			while (HeapTupleIsValid(el_label_tup =
									systable_getnext(el_label_scan)))
			{
				Form_pg_propgraph_element_label el_form =
					(Form_pg_propgraph_element_label) GETSTRUCT(el_label_tup);

				/*
				 * Look up the element to find its kind.
				 */
				{
					HeapTuple	elem_tup;
					Form_pg_propgraph_element elem_form;

					elem_tup = SearchSysCache1(PROPGRAPHELOID,
											   ObjectIdGetDatum(
																el_form->pgelelid));
					if (!HeapTupleIsValid(elem_tup))
						continue;
					elem_form = (Form_pg_propgraph_element)
						GETSTRUCT(elem_tup);
					if (elem_form->pgepgid == graph_oid)
					{
						if (elem_form->pgekind == 'v')
							has_vertex = true;
						else if (elem_form->pgekind == 'e')
							has_edge = true;
					}
					ReleaseSysCache(elem_tup);
				}

				if (has_vertex && has_edge)
					break;
			}

			systable_endscan(el_label_scan);
			table_close(el_label_rel, AccessShareLock);

			/*
			 * If the label is not associated with the required kind, produce
			 * the standard error.
			 */
			if ((elem_kind == 'v' && !has_vertex) ||
				(elem_kind == 'e' && !has_edge))
			{
				const char *labelname;

				labelname = get_propgraph_label_name(labelid);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("no property graph element of type "
								"\"%s\" has label \"%s\" associated "
								"with it in property graph \"%s\"",
								kind_str, labelname,
								get_rel_name(graph_oid))));
			}
		}
	}
}

/*
 * Run all native-specific validations on a graph pattern.
 * Called from transformGraphPattern when enable_native_graphtable is on.
 */
static void
validate_native_graph_query(ParseState *pstate,
							GraphPattern *pattern)
{
	GraphTableParseState *gpstate = pstate->p_graph_table_pstate;

	validate_element_variable_names(pattern);
	validate_edge_connectivity(pattern);
	validate_graph_element_label_kinds(pattern, gpstate->graphid);
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

	if (enable_native_graphtable)
		validate_native_graph_query(pstate, graph_pattern);

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
 * caller only needs the character.  Used by the label-kind validator.
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
