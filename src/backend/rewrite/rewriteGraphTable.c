/*-------------------------------------------------------------------------
 *
 * rewriteGraphTable.c
 *		Support for rewriting GRAPH_TABLE clauses.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteGraphTable.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_propgraph_element_label.h"
#include "catalog/pg_propgraph_label.h"
#include "catalog/pg_propgraph_label_property.h"
#include "catalog/pg_propgraph_property.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "parser/parse_graphtable.h"
#include "rewrite/rewriteGraphTable.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rowsecurity.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * Represents one path factor in a path.
 *
 * In a non-cyclic path, one path factor corresponds to one element pattern.
 *
 * In a cyclic path, one path factor corresponds to all the element patterns with
 * the same variable name.
 */
struct path_factor
{
	GraphElementPatternKind kind;
	const char *variable;
	Node	   *labelexpr;
	Node	   *whereClause;
	int			factorpos;		/* Position of this path factor in the list of
								 * path factors representing a given path
								 * pattern. */
	List	   *labeloids;		/* OIDs of all the labels referenced in
								 * labelexpr. */
	/* Links to adjacent vertex path factors if this is an edge path factor. */
	struct path_factor *src_pf;
	struct path_factor *dest_pf;
};

/*
 * Represents one property graph element (vertex or edge) in the path.
 *
 * Label expression in an element pattern resolves into a set of elements. We
 * create one path_element object for each of those elements.
 */
struct path_element
{
	/* Path factor from which this element is derived. */
	struct path_factor *path_factor;
	Oid			elemoid;
	Oid			reloid;
	/* Source and destination vertex elements for an edge element. */
	Oid			srcvertexid;
	Oid			destvertexid;
	/* Source and destination conditions for an edge element. */
	List	   *src_quals;
	List	   *dest_quals;
};

static Node *replace_property_refs(Oid propgraphid, Node *node, const List *mappings);
static List *build_edge_vertex_link_quals(HeapTuple edgetup, int edgerti, int refrti, Oid refid, AttrNumber catalog_key_attnum, AttrNumber catalog_ref_attnum, AttrNumber catalog_eqop_attnum);
static List *generate_queries_for_path_pattern(RangeTblEntry *rte, List *path_pattern);
static Query *generate_query_for_graph_path(RangeTblEntry *rte, List *graph_path);
static Node *generate_setop_from_pathqueries(List *pathqueries, List **rtable, List **targetlist);
static List *generate_queries_for_path_pattern_recurse(RangeTblEntry *rte, List *pathqueries, List *cur_path, List *path_elem_lists, int elempos);
static Query *generate_query_for_empty_path_pattern(RangeTblEntry *rte);
static Query *generate_union_from_pathqueries(List **pathqueries);
static List *get_path_elements_for_path_factor(Oid propgraphid, struct path_factor *pf);
static bool is_property_associated_with_label(Oid labeloid, Oid propoid);
extern Node *get_element_property_expr(Oid elemoid, Oid propoid, int rtindex);

/*
 * Decompose a GRAPH_TABLE clause into a subquery using relational operators.
 *
 * This builds the relational Query that represents the graph pattern: every
 * element pattern is resolved (via labels to concrete graph elements backing
 * tables) and the path patterns are generated as JOIN queries, unioned with
 * UNION ALL.  The rewriter uses it for the non-native (fallback) path; the
 * native planner reuses it for the relational (unquantified) parts of a
 * pattern.  The RTE itself is left untouched here.
 */
Query *
decomposeGraphTable(RangeTblEntry *rte)
{
	Query	   *graph_table_query;
	List	   *path_pattern;
	List	   *pathqueries = NIL;

	Assert(list_length(rte->graph_pattern->path_pattern_list) == 1);

	path_pattern = linitial(rte->graph_pattern->path_pattern_list);
	pathqueries = generate_queries_for_path_pattern(rte, path_pattern);
	graph_table_query = generate_union_from_pathqueries(&pathqueries);

	AcquireRewriteLocks(graph_table_query, true, false);

	return graph_table_query;
}

/*
 * Convert GRAPH_TABLE clause into a subquery using relational
 * operators.
 *
 * If enable_native_graphtable is true, the rewriting is bypassed and the
 * RTE_GRAPH_TABLE is left intact for the planner to decompose natively.
 */
Query *
rewriteGraphTable(Query *parsetree, int rt_index)
{
	RangeTblEntry *rte;

	rte = rt_fetch(rt_index, parsetree->rtable);

	/* Native mode: leave RTE_GRAPH_TABLE intact for the planner */
	if (enable_native_graphtable)
		return parsetree;

	rte->subquery = decomposeGraphTable(rte);

	rte->rtekind = RTE_SUBQUERY;
	rte->lateral = true;

	/*
	 * Reset no longer applicable fields, to appease
	 * WRITE_READ_PARSE_PLAN_TREES.
	 */
	rte->graph_pattern = NULL;
	rte->graph_table_columns = NIL;

	return parsetree;
}

/*
 * Generate queries representing the given path pattern applied to the given
 * property graph.
 *
 * A path pattern consists of one or more element patterns. Each of the element
 * patterns may be satisfied by multiple elements. A path satisfying the given
 * path pattern consists of one element from each element pattern.  There can be
 * as many paths as the number of combinations of the elements.  A path pattern
 * in itself is a K-partite graph where K = number of element patterns in the
 * path pattern. The possible paths are computed by performing a DFS in this
 * graph. The DFS is implemented as recursion. Each of these paths is converted
 * into a query connecting all the elements in that path. Set of these queries is
 * returned.
 *
 * Between every two vertex elements in the path there is an edge element that
 * connects them.  An edge connects two vertices identified by the source and
 * destination keys respectively. The connection between an edge and its
 * adjacent vertex is naturally computed as an equi-join between edge and vertex
 * table on their respective keys. Hence the query representing one path
 * consists of JOINs between edge and vertex tables.
 *
 * generate_queries_for_path_pattern() starts the recursion but actual work is
 * done by generate_queries_for_path_pattern_recurse().
 * generate_query_for_graph_path() constructs a query for a given path.
 *
 * A path pattern may end up producing no path if any of the element patterns
 * yields no elements or the edge patterns yield no edges connecting adjacent
 * vertex patterns.  In such a case a dummy query which returns no result is
 * returned (generate_query_for_empty_path_pattern()).
 *
 * 'path_pattern' is given path pattern to be applied on the property graph in
 * the GRAPH_TABLE clause represented by given 'rte'.
 */
static List *
generate_queries_for_path_pattern(RangeTblEntry *rte, List *path_pattern)
{
	List	   *pathqueries = NIL;
	List	   *path_elem_lists = NIL;
	int			factorpos = 0;
	List	   *path_factors = NIL;
	struct path_factor *prev_pf = NULL;

	Assert(list_length(path_pattern) > 0);

	/*
	 * Create a list of path factors representing the given path pattern
	 * linking edge path factors to their adjacent vertex path factors.
	 *
	 * While doing that merge element patterns with the same variable name
	 * into a single path_factor.
	 */
	foreach_node(GraphElementPattern, gep, path_pattern)
	{
		struct path_factor *pf = NULL;

		/*
		 * Unsupported conditions should have been caught by the parser
		 * itself. We have corresponding Asserts here to document the
		 * assumptions in this code.
		 */
		Assert(gep->kind == VERTEX_PATTERN || IS_EDGE_PATTERN(gep->kind));
		Assert(!gep->quantifier);

		foreach_ptr(struct path_factor, other, path_factors)
		{
			if (gep->variable && other->variable &&
				strcmp(gep->variable, other->variable) == 0)
			{
				if (other->kind != gep->kind)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("element patterns with same variable name \"%s\" but different element pattern types",
									gep->variable)));

				/*
				 * If both the element patterns have label expressions, they
				 * need to be conjuncted, which is not supported right now.
				 *
				 * However, an empty label expression means all labels.
				 * Conjunction of any label expression with all labels is the
				 * expression itself. Hence if only one of the two element
				 * patterns has a label expression use that expression.
				 */
				if (!other->labelexpr)
					other->labelexpr = gep->labelexpr;
				else if (gep->labelexpr && !equal(other->labelexpr, gep->labelexpr))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("element patterns with same variable name \"%s\" but different label expressions are not supported",
									gep->variable)));

				/*
				 * If two element patterns have the same variable name, they
				 * represent the same set of graph elements and hence are
				 * constrained by conditions from both the element patterns.
				 */
				if (!other->whereClause)
					other->whereClause = gep->whereClause;
				else if (gep->whereClause)
					other->whereClause = (Node *) makeBoolExpr(AND_EXPR,
															   list_make2(other->whereClause, gep->whereClause),
															   -1);
				pf = other;
				break;
			}
		}

		if (!pf)
		{
			pf = palloc0_object(struct path_factor);
			pf->factorpos = factorpos++;
			pf->kind = gep->kind;
			pf->labelexpr = gep->labelexpr;
			pf->variable = gep->variable;
			pf->whereClause = gep->whereClause;

			path_factors = lappend(path_factors, pf);
		}

		/*
		 * Setup links to the previous path factor in the path.
		 *
		 * If the previous path factor represents an edge, this path factor
		 * represents an adjacent vertex; the source vertex for an edge
		 * pointing left or the destination vertex for an edge pointing right.
		 * If this path factor represents an edge, the previous path factor
		 * represents an adjacent vertex; source vertex for an edge pointing
		 * right or the destination vertex for an edge pointing left.
		 *
		 * Edge pointing in any direction is treated similar to that pointing
		 * in right direction here.  When constructing a query in
		 * generate_query_for_graph_path(), we will try links in both the
		 * directions.
		 *
		 * If multiple edge patterns share the same variable name, they
		 * constrain the adjacent vertex patterns since an edge can connect
		 * only one pair of vertices. These adjacent vertex patterns need to
		 * be merged even though they have different variables. Such element
		 * patterns form a walk of graph where vertex and edges are repeated.
		 * For example, in (a)-[b]->(c)<-[b]-(d), (a) and (d) represent the
		 * same vertex element. This is slightly harder to implement and
		 * probably less useful. Hence not supported for now.
		 */
		if (prev_pf)
		{
			if (prev_pf->kind == EDGE_PATTERN_RIGHT || prev_pf->kind == EDGE_PATTERN_ANY)
			{
				Assert(!IS_EDGE_PATTERN(pf->kind));
				if (prev_pf->dest_pf && prev_pf->dest_pf != pf)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("an edge cannot connect more than two vertices even in a cyclic pattern"));
				prev_pf->dest_pf = pf;
			}
			else if (prev_pf->kind == EDGE_PATTERN_LEFT)
			{
				Assert(!IS_EDGE_PATTERN(pf->kind));
				if (prev_pf->src_pf && prev_pf->src_pf != pf)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("an edge cannot connect more than two vertices even in a cyclic pattern"));
				prev_pf->src_pf = pf;
			}
			else
			{
				Assert(prev_pf->kind == VERTEX_PATTERN);
				Assert(IS_EDGE_PATTERN(pf->kind));
			}

			if (pf->kind == EDGE_PATTERN_RIGHT || pf->kind == EDGE_PATTERN_ANY)
			{
				Assert(!IS_EDGE_PATTERN(prev_pf->kind));
				if (pf->src_pf && pf->src_pf != prev_pf)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("an edge cannot connect more than two vertices even in a cyclic pattern"));
				pf->src_pf = prev_pf;
			}
			else if (pf->kind == EDGE_PATTERN_LEFT)
			{
				Assert(!IS_EDGE_PATTERN(prev_pf->kind));
				if (pf->dest_pf && pf->dest_pf != prev_pf)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							errmsg("an edge cannot connect more than two vertices even in a cyclic pattern"));
				pf->dest_pf = prev_pf;
			}
			else
			{
				Assert(pf->kind == VERTEX_PATTERN);
				Assert(IS_EDGE_PATTERN(prev_pf->kind));
			}
		}

		prev_pf = pf;
	}

	/*
	 * Collect list of elements for each path factor. Do this after all the
	 * edge links are setup correctly.
	 */
	foreach_ptr(struct path_factor, pf, path_factors)
		path_elem_lists = lappend(path_elem_lists,
								  get_path_elements_for_path_factor(rte->relid, pf));

	pathqueries = generate_queries_for_path_pattern_recurse(rte, pathqueries,
															NIL, path_elem_lists, 0);
	if (!pathqueries)
		pathqueries = list_make1(generate_query_for_empty_path_pattern(rte));

	return pathqueries;
}

/*
 * Recursive workhorse function of generate_queries_for_path_pattern().
 *
 * `elempos` is the position of the next element being added in the path being
 * built.
 */
static List *
generate_queries_for_path_pattern_recurse(RangeTblEntry *rte, List *pathqueries, List *cur_path, List *path_elem_lists, int elempos)
{
	List	   *path_elems = list_nth_node(List, path_elem_lists, elempos);

	/* Guard against stack overflow due to complex path patterns. */
	check_stack_depth();

	foreach_ptr(struct path_element, pe, path_elems)
	{
		CHECK_FOR_INTERRUPTS();

		/* Update current path being built with current element. */
		cur_path = lappend(cur_path, pe);

		/*
		 * If this is the last element in the path, generate query for the
		 * completed path. Else recurse processing the next element.
		 */
		if (list_length(path_elem_lists) == list_length(cur_path))
		{
			Query	   *pathquery = generate_query_for_graph_path(rte, cur_path);

			Assert(elempos == list_length(path_elem_lists) - 1);
			if (pathquery)
				pathqueries = lappend(pathqueries, pathquery);
		}
		else
			pathqueries = generate_queries_for_path_pattern_recurse(rte, pathqueries,
																	cur_path,
																	path_elem_lists,
																	elempos + 1);
		/* Make way for the next element at the same position. */
		cur_path = list_delete_last(cur_path);
	}

	return pathqueries;
}

/*
 * Construct a query representing given graph path.
 *
 * The query contains:
 *
 * 1. targetlist corresponding to the COLUMNS clause of GRAPH_TABLE clause
 *
 * 2. quals corresponding to the WHERE clause of individual elements, WHERE
 * clause in GRAPH_TABLE clause and quals representing edge-vertex links.
 *
 * 3. fromlist containing all elements in the path
 *
 * The collations of property expressions are obtained from the catalog. The
 * collations of expressions in COLUMNS and WHERE clauses are assigned before
 * rewriting the graph table.  The collations of the edge-vertex link quals are
 * assigned when crafting those quals. Thus everything in the query that requires
 * collation assignment has been taken care of already. No separate collation
 * assignment is required in this function.
 *
 * More details in the prologue of generate_queries_for_path_pattern().
 */
static Query *
generate_query_for_graph_path(RangeTblEntry *rte, List *graph_path)
{
	Query	   *path_query = makeNode(Query);
	List	   *fromlist = NIL;
	List	   *qual_exprs = NIL;
	List	   *vars;

	path_query->commandType = CMD_SELECT;

	foreach_ptr(struct path_element, pe, graph_path)
	{
		struct path_factor *pf = pe->path_factor;
		RangeTblRef *rtr;
		Relation	rel;
		ParseNamespaceItem *pni;

		Assert(pf->kind == VERTEX_PATTERN || IS_EDGE_PATTERN(pf->kind));

		/* Add conditions representing edge connections. */
		if (IS_EDGE_PATTERN(pf->kind))
		{
			struct path_element *src_pe;
			struct path_element *dest_pe;
			Expr	   *edge_qual = NULL;

			Assert(pf->src_pf && pf->dest_pf);
			src_pe = list_nth(graph_path, pf->src_pf->factorpos);
			dest_pe = list_nth(graph_path, pf->dest_pf->factorpos);

			/* Make sure that the links of adjacent vertices are correct. */
			Assert(pf->src_pf == src_pe->path_factor &&
				   pf->dest_pf == dest_pe->path_factor);

			if (src_pe->elemoid == pe->srcvertexid &&
				dest_pe->elemoid == pe->destvertexid)
				edge_qual = makeBoolExpr(AND_EXPR,
										 list_concat(copyObject(pe->src_quals),
													 copyObject(pe->dest_quals)),
										 -1);

			/*
			 * An edge pattern in any direction matches edges in both
			 * directions, try swapping source and destination. When the
			 * source and destination is the same vertex table, quals
			 * corresponding to either direction may get satisfied. Hence OR
			 * the quals corresponding to both the directions.
			 */
			if (pf->kind == EDGE_PATTERN_ANY &&
				dest_pe->elemoid == pe->srcvertexid &&
				src_pe->elemoid == pe->destvertexid)
			{
				List	   *src_quals = copyObject(pe->dest_quals);
				List	   *dest_quals = copyObject(pe->src_quals);
				Expr	   *rev_edge_qual;

				/* Swap the source and destination varnos in the quals. */
				ChangeVarNodes((Node *) dest_quals, pe->path_factor->src_pf->factorpos + 1,
							   pe->path_factor->dest_pf->factorpos + 1, 0);
				ChangeVarNodes((Node *) src_quals, pe->path_factor->dest_pf->factorpos + 1,
							   pe->path_factor->src_pf->factorpos + 1, 0);

				rev_edge_qual = makeBoolExpr(AND_EXPR, list_concat(src_quals, dest_quals), -1);
				if (edge_qual)
					edge_qual = makeBoolExpr(OR_EXPR, list_make2(edge_qual, rev_edge_qual), -1);
				else
					edge_qual = rev_edge_qual;
			}

			/*
			 * If the given edge element does not connect the adjacent vertex
			 * elements in this path, the path is broken. Abandon this path as
			 * it won't return any rows.
			 */
			if (edge_qual == NULL)
				return NULL;

			qual_exprs = lappend(qual_exprs, edge_qual);
		}
		else
			Assert(!pe->src_quals && !pe->dest_quals);

		/*
		 * Create RangeTblEntry for this element table.
		 *
		 * SQL/PGQ standard (Ref. Section 11.19, Access rule 2 and General
		 * rule 4) does not specify whose access privileges to use when
		 * accessing the element tables: property graph owner's or current
		 * user's. It is safer to use current user's privileges to avoid
		 * unprivileged data access through a property graph. This is inline
		 * with the views being security_invoker by default.
		 */
		rel = table_open(pe->reloid, AccessShareLock);
		pni = addRangeTableEntryForRelation(make_parsestate(NULL), rel, AccessShareLock,
											NULL, true, false);
		table_close(rel, NoLock);
		path_query->rtable = lappend(path_query->rtable, pni->p_rte);
		path_query->rteperminfos = lappend(path_query->rteperminfos, pni->p_perminfo);
		pni->p_rte->perminfoindex = list_length(path_query->rteperminfos);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = list_length(path_query->rtable);
		fromlist = lappend(fromlist, rtr);

		/*
		 * Make sure that the assumption mentioned in create_pe_for_element()
		 * holds true; that the elements' RangeTblEntrys are added in the
		 * order in which their respective path factors appear in the list of
		 * path factors representing the path pattern.
		 */
		Assert(pf->factorpos + 1 == rtr->rtindex);

		if (pf->whereClause)
		{
			Node	   *tr;

			tr = replace_property_refs(rte->relid, pf->whereClause, list_make1(pe));

			qual_exprs = lappend(qual_exprs, tr);
		}
	}

	if (rte->graph_pattern->whereClause)
	{
		Node	   *path_quals = replace_property_refs(rte->relid,
													   (Node *) rte->graph_pattern->whereClause,
													   graph_path);

		qual_exprs = lappend(qual_exprs, path_quals);
	}

	path_query->jointree = makeFromExpr(fromlist,
										qual_exprs ? (Node *) makeBoolExpr(AND_EXPR, qual_exprs, -1) : NULL);

	/* Construct query targetlist from COLUMNS specification of GRAPH_TABLE. */
	path_query->targetList = castNode(List,
									  replace_property_refs(rte->relid,
															(Node *) rte->graph_table_columns,
															graph_path));

	/*
	 * Mark the columns being accessed in the path query as requiring SELECT
	 * privilege. Any lateral columns should have been handled when the
	 * corresponding ColumnRefs were transformed. Ignore those here.
	 */
	vars = pull_vars_of_level((Node *) list_make2(qual_exprs, path_query->targetList), 0);
	foreach_node(Var, var, vars)
	{
		RTEPermissionInfo *perminfo = getRTEPermissionInfo(path_query->rteperminfos,
														   rt_fetch(var->varno, path_query->rtable));

		/* Must offset the attnum to fit in a bitmapset */
		perminfo->selectedCols = bms_add_member(perminfo->selectedCols,
												var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	return path_query;
}

/*
 * Construct a query which would not return any rows.
 *
 * More details in the prologue of generate_queries_for_path_pattern().
 */
static Query *
generate_query_for_empty_path_pattern(RangeTblEntry *rte)
{
	Query	   *query = makeNode(Query);

	query->commandType = CMD_SELECT;
	query->rtable = NIL;
	query->rteperminfos = NIL;
	query->jointree = makeFromExpr(NIL, (Node *) makeBoolConst(false, false));

	/*
	 * Even though no rows are returned, the result still projects the same
	 * columns as projected by GRAPH_TABLE clause. Do this by constructing a
	 * target list full of NULL values.
	 */
	foreach_node(TargetEntry, te, rte->graph_table_columns)
	{
		Node	   *nte = (Node *) te->expr;

		te->expr = (Expr *) makeNullConst(exprType(nte), exprTypmod(nte), exprCollation(nte));
		query->targetList = lappend(query->targetList, te);
	}

	return query;
}

/*
 * Construct a query which is UNION of given path queries.
 *
 * The UNION query derives collations of its targetlist entries from the
 * corresponding targetlist entries of the path queries. The targetlists of path
 * queries being UNION'ed already have collations assigned.  No separate
 * collation assignment required in this function.
 *
 * The function destroys given pathqueries list while constructing
 * SetOperationStmt recursively. Hence the function always returns with
 * `pathqueries` set to NIL.
 */
static Query *
generate_union_from_pathqueries(List **pathqueries)
{
	List	   *rtable = NIL;
	Query	   *sampleQuery = linitial_node(Query, *pathqueries);
	SetOperationStmt *sostmt;
	Query	   *union_query;
	int			resno;
	ListCell   *lctl,
			   *lct,
			   *lcm,
			   *lcc;

	Assert(list_length(*pathqueries) > 0);

	/* If there's only one pathquery, no need to construct a UNION query. */
	if (list_length(*pathqueries) == 1)
	{
		*pathqueries = NIL;
		return sampleQuery;
	}

	sostmt = castNode(SetOperationStmt,
					  generate_setop_from_pathqueries(*pathqueries, &rtable, NULL));

	/* Encapsulate the set operation statement into a Query. */
	union_query = makeNode(Query);
	union_query->commandType = CMD_SELECT;
	union_query->rtable = rtable;
	union_query->setOperations = (Node *) sostmt;
	union_query->rteperminfos = NIL;
	union_query->jointree = makeFromExpr(NIL, NULL);

	/*
	 * Generate dummy targetlist for outer query using column names from one
	 * of the queries and common datatypes/collations of topmost set
	 * operation.  It shouldn't matter which query. Also it shouldn't matter
	 * which RT index is used as varno in the target list entries, as long as
	 * it corresponds to a real RT entry; else funny things may happen when
	 * the tree is mashed by rule rewriting. So we use 1 since there's always
	 * one RT entry at least.
	 */
	Assert(rt_fetch(1, rtable));
	union_query->targetList = NULL;
	resno = 1;
	forfour(lct, sostmt->colTypes,
			lcm, sostmt->colTypmods,
			lcc, sostmt->colCollations,
			lctl, sampleQuery->targetList)
	{
		Oid			colType = lfirst_oid(lct);
		int32		colTypmod = lfirst_int(lcm);
		Oid			colCollation = lfirst_oid(lcc);
		TargetEntry *sample_tle = (TargetEntry *) lfirst(lctl);
		char	   *colName;
		TargetEntry *tle;
		Var		   *var;

		Assert(!sample_tle->resjunk);
		colName = pstrdup(sample_tle->resname);
		var = makeVar(1, sample_tle->resno, colType, colTypmod, colCollation, 0);
		var->location = exprLocation((Node *) sample_tle->expr);
		tle = makeTargetEntry((Expr *) var, (AttrNumber) resno++, colName, false);
		union_query->targetList = lappend(union_query->targetList, tle);
	}

	*pathqueries = NIL;
	return union_query;
}

/*
 * Construct a query which is UNION of all the given path queries.
 *
 * The function destroys given pathqueries list while constructing
 * SetOperationStmt recursively.
 */
static Node *
generate_setop_from_pathqueries(List *pathqueries, List **rtable, List **targetlist)
{
	SetOperationStmt *sostmt;
	Query	   *lquery;
	Node	   *rarg;
	RangeTblRef *lrtr = makeNode(RangeTblRef);
	List	   *rtargetlist;
	ParseNamespaceItem *pni;

	/* Guard against stack overflow due to many path queries. */
	check_stack_depth();

	/* Recursion termination condition. */
	if (list_length(pathqueries) == 0)
	{
		*targetlist = NIL;
		return NULL;
	}

	lquery = linitial_node(Query, pathqueries);

	/*
	 * Each path query will become a subquery of the UNION statement. So any
	 * Vars that already refer outside the path query must be adjusted for
	 * additional query level.
	 */
	IncrementVarSublevelsUp((Node *) lquery, 1, 1);

	pni = addRangeTableEntryForSubquery(make_parsestate(NULL), lquery, NULL,
										false, false);
	*rtable = lappend(*rtable, pni->p_rte);
	lrtr->rtindex = list_length(*rtable);
	rarg = generate_setop_from_pathqueries(list_delete_first(pathqueries), rtable, &rtargetlist);
	if (rarg == NULL)
	{
		/*
		 * No further path queries in the list. Convert the last query into a
		 * RangeTblRef as expected by SetOperationStmt. Extract a list of the
		 * non-junk TLEs for upper-level processing.
		 */
		if (targetlist)
		{
			*targetlist = NIL;
			foreach_node(TargetEntry, tle, lquery->targetList)
			{
				if (!tle->resjunk)
					*targetlist = lappend(*targetlist, tle);
			}
		}
		return (Node *) lrtr;
	}

	sostmt = makeNode(SetOperationStmt);
	sostmt->op = SETOP_UNION;
	sostmt->all = true;
	sostmt->larg = (Node *) lrtr;
	sostmt->rarg = rarg;
	constructSetOpTargetlist(NULL, sostmt, lquery->targetList, rtargetlist, targetlist, "UNION", false);

	return (Node *) sostmt;
}

/*
 * Construct a path_element object for the graph element given by `elemoid`
 * satisfied by the path factor `pf`.
 *
 * If the type of graph element does not fit the element pattern kind, the
 * function returns NULL.
 */
static struct path_element *
create_pe_for_element(struct path_factor *pf, Oid elemoid)
{
	HeapTuple	eletup = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(elemoid));
	Form_pg_propgraph_element pgeform;
	struct path_element *pe;

	if (!eletup)
		elog(ERROR, "cache lookup failed for property graph element %u", elemoid);
	pgeform = ((Form_pg_propgraph_element) GETSTRUCT(eletup));

	if ((pgeform->pgekind == PGEKIND_VERTEX && pf->kind != VERTEX_PATTERN) ||
		(pgeform->pgekind == PGEKIND_EDGE && !IS_EDGE_PATTERN(pf->kind)))
	{
		ReleaseSysCache(eletup);
		return NULL;
	}

	pe = palloc0_object(struct path_element);
	pe->path_factor = pf;
	pe->elemoid = elemoid;
	pe->reloid = pgeform->pgerelid;

	/*
	 * When a path is converted into a query
	 * (generate_query_for_graph_path()), a RangeTblEntry will be created for
	 * every element in the path.  Fixing rtindexes of RangeTblEntrys here
	 * makes it possible to craft elements' qual expressions only once while
	 * we have access to the catalog entry. Otherwise they need to be crafted
	 * as many times as the number of paths a given element appears in,
	 * fetching catalog entry again each time.  Hence we simply assume
	 * RangeTblEntrys will be created in the same order in which the
	 * corresponding path factors appear in the list of path factors
	 * representing a path pattern. That way their rtindexes will be same as
	 * path_factor::factorpos + 1.
	 */
	if (IS_EDGE_PATTERN(pf->kind))
	{
		pe->srcvertexid = pgeform->pgesrcvertexid;
		pe->destvertexid = pgeform->pgedestvertexid;
		Assert(pf->src_pf && pf->dest_pf);

		pe->src_quals = build_edge_vertex_link_quals(eletup, pf->factorpos + 1, pf->src_pf->factorpos + 1,
													 pe->srcvertexid,
													 Anum_pg_propgraph_element_pgesrckey,
													 Anum_pg_propgraph_element_pgesrcref,
													 Anum_pg_propgraph_element_pgesrceqop);
		pe->dest_quals = build_edge_vertex_link_quals(eletup, pf->factorpos + 1, pf->dest_pf->factorpos + 1,
													  pe->destvertexid,
													  Anum_pg_propgraph_element_pgedestkey,
													  Anum_pg_propgraph_element_pgedestref,
													  Anum_pg_propgraph_element_pgedesteqop);
	}

	ReleaseSysCache(eletup);

	return pe;
}

/*
 * Returns the list of OIDs of graph labels which the given label expression
 * resolves to in the given property graph.
 */
static List *
get_labels_for_expr(Oid propgraphid, Node *labelexpr)
{
	/*
	 * According to section 9.2 "Contextual inference of a set of labels"
	 * subclause 2.a.ii of SQL/PGQ standard, an element pattern which does not
	 * have a label expression is considered to have label expression
	 * equivalent to '%|!%' which is the set of all labels.
	 */
	if (!labelexpr)
		return get_graph_all_label_oids(propgraphid);

	return get_label_oids_for_labelexpr(labelexpr);
}

/*
 * Return a list of all the graph elements that satisfy the graph element pattern
 * represented by the given path_factor `pf`.
 *
 * First we find all the graph labels that satisfy the label expression in path
 * factor. Each label is associated with one or more graph elements.  A union of
 * all such elements satisfies the element pattern. We create one path_element
 * object representing every element whose graph element kind qualifies the
 * element pattern kind. A list of all such path_element objects is returned.
 *
 * Note that we need to report an error for an explicitly specified label which
 * is not associated with any graph element of the required kind. So we have to
 * treat each label separately. Without that requirement we could have collected
 * all the unique elements first and then created path_element objects for them
 * to simplify the code.
 */
static List *
get_path_elements_for_path_factor(Oid propgraphid, struct path_factor *pf)
{
	List	   *label_oids = get_labels_for_expr(propgraphid, pf->labelexpr);
	List	   *elem_oids_seen = NIL;
	List	   *pf_elem_oids = NIL;
	List	   *path_elements = NIL;
	List	   *unresolved_labels = NIL;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;

	/*
	 * A property graph element can be either a vertex or an edge. Other types
	 * of path factors like nested path pattern need to be handled separately
	 * when supported.
	 */
	Assert(pf->kind == VERTEX_PATTERN || IS_EDGE_PATTERN(pf->kind));

	rel = table_open(PropgraphElementLabelRelationId, AccessShareLock);
	foreach_oid(labeloid, label_oids)
	{
		bool		found = false;

		ScanKeyInit(&key[0],
					Anum_pg_propgraph_element_label_pgellabelid,
					BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(labeloid));
		scan = systable_beginscan(rel, PropgraphElementLabelLabelIndexId, true,
								  NULL, 1, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_propgraph_element_label label_elem = (Form_pg_propgraph_element_label) GETSTRUCT(tup);
			Oid			elem_oid = label_elem->pgelelid;

			if (!list_member_oid(elem_oids_seen, elem_oid))
			{
				/*
				 * Create path_element object if the new element qualifies the
				 * element pattern kind.
				 */
				struct path_element *pe = create_pe_for_element(pf, elem_oid);

				if (pe)
				{
					path_elements = lappend(path_elements, pe);

					/* Remember qualified elements. */
					pf_elem_oids = lappend_oid(pf_elem_oids, elem_oid);
					found = true;
				}

				/*
				 * Remember qualified and unqualified elements processed so
				 * far to avoid processing already processed elements again.
				 */
				elem_oids_seen = lappend_oid(elem_oids_seen, label_elem->pgelelid);
			}
			else if (list_member_oid(pf_elem_oids, elem_oid))
			{
				/*
				 * The graph element is known to qualify the given element
				 * pattern. Flag that the current label has at least one
				 * qualified element associated with it.
				 */
				found = true;
			}
		}

		if (!found)
		{
			/*
			 * We did not find any qualified element associated with this
			 * label. The label or its properties can not be associated with
			 * the given element pattern. Throw an error if the label was
			 * explicitly specified in the element pattern. Otherwise remember
			 * it for later use.
			 */
			if (!pf->labelexpr)
				unresolved_labels = lappend_oid(unresolved_labels, labeloid);
			else
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("no property graph element of type \"%s\" has label \"%s\" associated with it in property graph \"%s\"",
								pf->kind == VERTEX_PATTERN ? "vertex" : "edge",
								get_propgraph_label_name(labeloid),
								get_rel_name(propgraphid))));
		}

		systable_endscan(scan);
	}
	table_close(rel, AccessShareLock);

	/*
	 * Remove the labels which were not explicitly mentioned in the label
	 * expression but do not have any qualified elements associated with them.
	 * Properties associated with such labels may not be referenced. See
	 * replace_property_refs_mutator() for more details.
	 */
	pf->labeloids = list_difference_oid(label_oids, unresolved_labels);

	return path_elements;
}

/*
 * Mutating property references into table variables
 */

struct replace_property_refs_context
{
	Oid			propgraphid;
	const List *mappings;
};

static Node *
replace_property_refs_mutator(Node *node, struct replace_property_refs_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Var		   *newvar = copyObject(var);

		/*
		 * If it's already a Var, then it was a lateral reference.  Since we
		 * are in a subquery after the rewrite, we have to increase the level
		 * by one.
		 */
		newvar->varlevelsup++;

		return (Node *) newvar;
	}
	else if (IsA(node, GraphPropertyRef))
	{
		GraphPropertyRef *gpr = (GraphPropertyRef *) node;
		Node	   *n = NULL;
		struct path_element *found_mapping = NULL;
		struct path_factor *mapping_factor = NULL;
		List	   *unrelated_labels = NIL;

		foreach_ptr(struct path_element, m, context->mappings)
		{
			if (m->path_factor->variable && strcmp(gpr->elvarname, m->path_factor->variable) == 0)
			{
				found_mapping = m;
				break;
			}
		}

		/*
		 * transformGraphTablePropertyRef() would not create a
		 * GraphPropertyRef for a variable which is not present in the graph
		 * path pattern.
		 */
		Assert(found_mapping);

		mapping_factor = found_mapping->path_factor;

		/*
		 * Find property definition for given element through any of the
		 * associated labels qualifying the given element pattern.
		 */
		foreach_oid(labeloid, mapping_factor->labeloids)
		{
			Oid			elem_labelid = GetSysCacheOid2(PROPGRAPHELEMENTLABELELEMENTLABEL,
													   Anum_pg_propgraph_element_label_oid,
													   ObjectIdGetDatum(found_mapping->elemoid),
													   ObjectIdGetDatum(labeloid));

			if (OidIsValid(elem_labelid))
			{
				HeapTuple	tup = SearchSysCache2(PROPGRAPHLABELPROP, ObjectIdGetDatum(elem_labelid),
												  ObjectIdGetDatum(gpr->propid));

				if (!tup)
				{
					/*
					 * The label is associated with the given element but it
					 * is not associated with the required property. Check
					 * next label.
					 */
					continue;
				}

				n = stringToNode(TextDatumGetCString(SysCacheGetAttrNotNull(PROPGRAPHLABELPROP,
																			tup, Anum_pg_propgraph_label_property_plpexpr)));
				ChangeVarNodes(n, 1, mapping_factor->factorpos + 1, 0);

				ReleaseSysCache(tup);
			}
			else
			{
				/*
				 * Label is not associated with the element but it may be
				 * associated with the property through some other element.
				 * Save it for later use.
				 */
				unrelated_labels = lappend_oid(unrelated_labels, labeloid);
			}
		}

		/* See if we can resolve the property in some other way. */
		if (!n)
		{
			bool		prop_associated = false;

			foreach_oid(loid, unrelated_labels)
			{
				if (is_property_associated_with_label(loid, gpr->propid))
				{
					prop_associated = true;
					break;
				}
			}

			if (prop_associated)
			{
				/*
				 * The property is associated with at least one of the labels
				 * that satisfy given element pattern. If it's associated with
				 * the given element (through some other label), use
				 * corresponding value expression. Otherwise NULL. Ref.
				 * SQL/PGQ standard section 6.5 Property Reference, General
				 * Rule 2.b.
				 */
				n = get_element_property_expr(found_mapping->elemoid, gpr->propid,
											  mapping_factor->factorpos + 1);

				if (!n)
					n = (Node *) makeNullConst(gpr->typeId, gpr->typmod, gpr->collation);
			}

		}

		if (!n)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("property \"%s\" for element variable \"%s\" not found",
						   get_propgraph_property_name(gpr->propid), mapping_factor->variable));

		return n;
	}

	return expression_tree_mutator(node, replace_property_refs_mutator, context);
}

static Node *
replace_property_refs(Oid propgraphid, Node *node, const List *mappings)
{
	struct replace_property_refs_context context;

	context.mappings = mappings;
	context.propgraphid = propgraphid;

	return replace_property_refs_mutator(node, &context);
}

/*
 * Build join qualification expressions between edge and vertex tables.
 */
static List *
build_edge_vertex_link_quals(HeapTuple edgetup, int edgerti, int refrti, Oid refid, AttrNumber catalog_key_attnum, AttrNumber catalog_ref_attnum, AttrNumber catalog_eqop_attnum)
{
	List	   *quals = NIL;
	Form_pg_propgraph_element pgeform;
	Datum		datum;
	Datum	   *d1,
			   *d2,
			   *d3;
	int			n1,
				n2,
				n3;
	ParseState *pstate = make_parsestate(NULL);
	Oid			refrelid = GetSysCacheOid1(PROPGRAPHELOID, Anum_pg_propgraph_element_pgerelid, ObjectIdGetDatum(refid));

	pgeform = (Form_pg_propgraph_element) GETSTRUCT(edgetup);

	datum = SysCacheGetAttrNotNull(PROPGRAPHELOID, edgetup, catalog_key_attnum);
	deconstruct_array_builtin(DatumGetArrayTypeP(datum), INT2OID, &d1, NULL, &n1);

	datum = SysCacheGetAttrNotNull(PROPGRAPHELOID, edgetup, catalog_ref_attnum);
	deconstruct_array_builtin(DatumGetArrayTypeP(datum), INT2OID, &d2, NULL, &n2);

	datum = SysCacheGetAttrNotNull(PROPGRAPHELOID, edgetup, catalog_eqop_attnum);
	deconstruct_array_builtin(DatumGetArrayTypeP(datum), OIDOID, &d3, NULL, &n3);

	if (n1 != n2)
		elog(ERROR, "array size key (%d) vs ref (%d) mismatch for element ID %u", catalog_key_attnum, catalog_ref_attnum, pgeform->oid);
	if (n1 != n3)
		elog(ERROR, "array size key (%d) vs operator (%d) mismatch for element ID %u", catalog_key_attnum, catalog_eqop_attnum, pgeform->oid);

	for (int i = 0; i < n1; i++)
	{
		AttrNumber	keyattn = DatumGetInt16(d1[i]);
		AttrNumber	refattn = DatumGetInt16(d2[i]);
		Oid			eqop = DatumGetObjectId(d3[i]);
		Var		   *keyvar;
		Var		   *refvar;
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcoll;
		HeapTuple	tup;
		Form_pg_operator opform;
		List	   *args;
		Oid			actual_arg_types[2];
		Oid			declared_arg_types[2];
		OpExpr	   *linkqual;

		get_atttypetypmodcoll(pgeform->pgerelid, keyattn, &atttypid, &atttypmod, &attcoll);
		keyvar = makeVar(edgerti, keyattn, atttypid, atttypmod, attcoll, 0);
		get_atttypetypmodcoll(refrelid, refattn, &atttypid, &atttypmod, &attcoll);
		refvar = makeVar(refrti, refattn, atttypid, atttypmod, attcoll, 0);

		tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(eqop));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u", eqop);
		opform = (Form_pg_operator) GETSTRUCT(tup);
		/* An equality operator is a binary operator returning boolean result. */
		Assert(opform->oprkind == 'b'
			   && RegProcedureIsValid(opform->oprcode)
			   && opform->oprresult == BOOLOID
			   && !get_func_retset(opform->oprcode));

		/*
		 * Prepare operands and cast them to the types required by the
		 * equality operator. Similar to PK/FK quals, referenced vertex key is
		 * used as left operand and referencing edge key is used as right
		 * operand.
		 */
		args = list_make2(refvar, keyvar);
		actual_arg_types[0] = exprType((Node *) refvar);
		actual_arg_types[1] = exprType((Node *) keyvar);
		declared_arg_types[0] = opform->oprleft;
		declared_arg_types[1] = opform->oprright;
		make_fn_arguments(pstate, args, actual_arg_types, declared_arg_types);

		linkqual = makeNode(OpExpr);
		linkqual->opno = opform->oid;
		linkqual->opfuncid = opform->oprcode;
		linkqual->opresulttype = opform->oprresult;
		linkqual->opretset = false;
		/* opcollid and inputcollid will be set by parse_collate.c */
		linkqual->args = args;
		linkqual->location = -1;

		ReleaseSysCache(tup);
		quals = lappend(quals, linkqual);
	}

	assign_expr_collations(pstate, (Node *) quals);

	return quals;
}

/*
 * Check if the given property is associated with the given label.
 *
 * A label projects the same set of properties through every element it is
 * associated with. Find any of the elements and return true if that element is
 * associated with the given property. False otherwise.
 */
static bool
is_property_associated_with_label(Oid labeloid, Oid propoid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	tup;
	bool		associated = false;

	rel = table_open(PropgraphElementLabelRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_element_label_pgellabelid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(labeloid));
	scan = systable_beginscan(rel, PropgraphElementLabelLabelIndexId,
							  true, NULL, 1, key);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_propgraph_element_label ele_label = (Form_pg_propgraph_element_label) GETSTRUCT(tup);

		associated = SearchSysCacheExists2(PROPGRAPHLABELPROP,
										   ObjectIdGetDatum(ele_label->oid), ObjectIdGetDatum(propoid));
	}
	systable_endscan(scan);
	table_close(rel, RowShareLock);

	return associated;
}

/*
 * If given element has the given property associated with it, through any of
 * the associated labels, return value expression of the property. Otherwise
 * NULL.
 */
Node *
get_element_property_expr(Oid elemoid, Oid propoid, int rtindex)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	labeltup;
	Node	   *n = NULL;

	rel = table_open(PropgraphElementLabelRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_propgraph_element_label_pgelelid,
				BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(elemoid));
	scan = systable_beginscan(rel, PropgraphElementLabelElementLabelIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid(labeltup = systable_getnext(scan)))
	{
		Form_pg_propgraph_element_label ele_label = (Form_pg_propgraph_element_label) GETSTRUCT(labeltup);

		HeapTuple	proptup = SearchSysCache2(PROPGRAPHLABELPROP,
											  ObjectIdGetDatum(ele_label->oid), ObjectIdGetDatum(propoid));

		if (!proptup)
			continue;
		n = stringToNode(TextDatumGetCString(SysCacheGetAttrNotNull(PROPGRAPHLABELPROP,
																	proptup, Anum_pg_propgraph_label_property_plpexpr)));
		ChangeVarNodes(n, 1, rtindex, 0);

		ReleaseSysCache(proptup);
		break;
	}
	systable_endscan(scan);
	table_close(rel, RowShareLock);

	return n;
}

/* -------------------------------------------------------------------------
 * Native (planner-owned) per-path decomposition of a graph pattern.
 *
 * The pattern is decomposed by enumerating, for each non-quantified element
 * pattern, one concrete graph element per branch (exactly like the rewrite
 * fallback), while each quantified (variable-length) hop is kept as an
 * internal RTE_GRAPH_TABLE that the planner turns into a GraphScan node.
 * The branches are UNION ALL-ed, which moves every label disjunction to the
 * branch level and gives each GraphScan concrete, well-typed seed and
 * terminal elements (so the terminal binding can be a normal relational
 * join, and fixed hops can follow the scan as ordinary joins).
 * -------------------------------------------------------------------------
 */

/*
 * Description of one quantified (variable-length) edge element pattern.
 */
typedef struct native_vle_factor
{
	GraphElementPattern *edge_gep;	/* the edge element pattern */
	List	   *edge_element_oids;	/* edge element OIDs matching the label */
	List	   *array_props;	/* GraphPropertyRef* (VLE edge-list refs) */
	int			min_depth;		/* quantifier lower bound */
	int			max_depth;		/* quantifier upper bound, -1 = unbounded */
}			native_vle_factor;

/* Per-branch binding of a VLE factor (edge-var list refs). */
typedef struct native_vle_bind
{
	const char *varname;		/* the quantified edge variable */
	int			gs_rti;			/* RT index of the internal graph RTE */
	int			array_first;	/* first array output attno on the graph RTE */
	List	   *array_props;	/* the factor's GraphPropertyRef* list */
	Node	   *seed_quals;		/* ghost seed element's WHERE clause (the seed
								 * key equality with the previous segment) */
}			native_vle_bind;

/* Binding of a concrete element variable in a branch. */
typedef struct native_bind
{
	const char *varname;
	Oid			elemoid;
	int			rti;
}			native_bind;

/* State for the branch enumeration and assembly. */
typedef struct native_decomp
{
	RangeTblEntry *rte;			/* the user's graph RTE */
	List	   *elem_lists;		/* per factor: List of struct path_element
								 * (NIL for a VLE factor) */
	List	   *vle_factors;	/* per factor: native_vle_factor* or NULL */
	int			nfactors;
	List	   *branch_queries; /* resulting per-branch Queries */
}			native_decomp;

/* Context for resolving property references within a branch. */
typedef struct native_prop_ctx
{
	List	   *binds;			/* List of native_bind */
	List	   *vle_binds;		/* List of native_vle_bind */
}			native_prop_ctx;

/*
 * Return the key columns (attnum/type/typmod/collation) of the given graph
 * element, read from the given key column array of pg_propgraph_element
 * (pgekey for a vertex element, pgesrckey/pgedestkey for an edge element).
 */
List *
get_graph_element_key_columns(Oid elemoid, int key_attnum)
{
	List	   *result = NIL;
	HeapTuple	eletup;
	Form_pg_propgraph_element pgeform;
	Datum		datum;
	Datum	   *d;
	int			n;
	int			i;

	eletup = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(elemoid));
	if (!HeapTupleIsValid(eletup))
		elog(ERROR, "cache lookup failed for property graph element %u", elemoid);
	pgeform = (Form_pg_propgraph_element) GETSTRUCT(eletup);

	datum = SysCacheGetAttrNotNull(PROPGRAPHELOID, eletup, key_attnum);
	deconstruct_array_builtin(DatumGetArrayTypeP(datum), INT2OID, &d, NULL, &n);

	for (i = 0; i < n; i++)
	{
		GraphElementKeyCol *kc = palloc_object(GraphElementKeyCol);

		kc->attnum = DatumGetInt16(d[i]);
		get_atttypetypmodcoll(pgeform->pgerelid, kc->attnum,
							  &kc->typid, &kc->typmod, &kc->collation);
		result = lappend(result, kc);
	}

	ReleaseSysCache(eletup);

	return result;
}

/*
 * Look up the backing table and the vertex element references of a graph
 * element (pgerelid / pgesrcvertexid / pgedestvertexid).  Shared by the
 * native planner and the native executor.
 */
void
get_graph_element_identity(Oid elemoid, Oid *relid, Oid *srcvertex,
						   Oid *dstvertex)
{
	HeapTuple	eletup;
	Form_pg_propgraph_element pgeform;

	eletup = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(elemoid));
	if (!HeapTupleIsValid(eletup))
		elog(ERROR, "cache lookup failed for property graph element %u", elemoid);
	pgeform = (Form_pg_propgraph_element) GETSTRUCT(eletup);

	*relid = pgeform->pgerelid;
	*srcvertex = pgeform->pgesrcvertexid;
	*dstvertex = pgeform->pgedestvertexid;

	ReleaseSysCache(eletup);
}

/*
 * Return an equality operator suitable for the given datatype, using the
 * type's default (btree) equality operator.
 */
Oid
key_equality_operator(Oid typid)
{
	TypeCacheEntry *tc = lookup_type_cache(typid, TYPECACHE_EQ_OPR);

	if (tc->eq_opr == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("no equality operator for graph key type %s",
						format_type_be(typid))));

	return tc->eq_opr;
}

/*
 * Build an equality OpExpr between two same-typed Vars using the type's
 * equality operator.  Collations are fixed up by the caller.
 */
static Expr *
make_key_equality(Node *left, Node *right)
{
	Oid			eqtype = exprType(left);
	Oid			eqop = key_equality_operator(eqtype);
	OpExpr	   *op;

	Assert(eqtype == exprType(right));

	op = makeNode(OpExpr);
	op->opno = eqop;
	op->opfuncid = get_opcode(eqop);
	op->opresulttype = get_op_rettype(eqop);
	op->opretset = false;
	op->args = list_make2(left, right);
	op->location = -1;

	return (Expr *) op;
}

/*
 * Walker accumulating GraphPropertyRef nodes found in an expression tree.
 */
static bool
collect_graph_property_ref_walker(Node *node, List **refs)
{
	if (node == NULL)
		return false;
	if (IsA(node, GraphPropertyRef))
	{
		*refs = lappend(*refs, node);
		return false;
	}
	return expression_tree_walker(node, collect_graph_property_ref_walker,
								  refs);
}

/*
 * Collect the VLE edge-list (array) property references of the given edge
 * variable from the COLUMNS and the graph-level WHERE clause.
 */
List *
get_vle_array_props(RangeTblEntry *rte, const char *edge_var)
{
	List	   *result = NIL;
	List	   *all = NIL;
	ListCell   *lc;

	/*
	 * An anonymous edge pattern (no explicit edge variable) cannot be
	 * referenced in the COLUMNS or the graph-level WHERE clause, so it can
	 * have no VLE edge-list (array) properties.  Bail out rather than
	 * comparing property reference names against a NULL edge variable below.
	 */
	if (edge_var == NULL)
		return NIL;

	foreach(lc, rte->graph_table_columns)
	{
		TargetEntry *te = lfirst_node(TargetEntry, lc);

		all = lappend(all, (Node *) te->expr);
	}
	if (rte->graph_pattern->whereClause)
		all = lappend(all, (Node *) rte->graph_pattern->whereClause);

	foreach(lc, all)
	{
		List	   *refs = NIL;

		(void) collect_graph_property_ref_walker((Node *) lfirst(lc), &refs);
		foreach_ptr(GraphPropertyRef, gpr, refs)
		{
			if (gpr->vle_list && gpr->elvarname &&
				strcmp(gpr->elvarname, edge_var) == 0)
			{
				bool		seen = false;

				foreach_ptr(GraphPropertyRef, prev, result)
				{
					if (prev->propid == gpr->propid)
					{
						seen = true;
						break;
					}
				}
				if (!seen)
					result = lappend(result, gpr);
			}
		}
	}

	return result;
}

/*
 * Walker acquiring locks on the relations referenced by sublinks found in
 * RLS policy quals.  Mirrors acquireLocksOnSubLinks() in rewriteHandler.c:
 * policy quals are added post-parsing, so the relations they reference must
 * be locked here rather than by the parser.
 */
static bool
native_rls_lock_sublinks(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query	   *subquery = (Query *) node;
		ListCell   *lc;

		foreach(lc, subquery->rtable)
		{
			RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

			if (rte->rtekind == RTE_RELATION)
				LockRelationOid(rte->relid, AccessShareLock);
		}

		return query_tree_walker(subquery, native_rls_lock_sublinks, context,
								 QTW_IGNORE_RC_SUBQUERIES);
	}

	return expression_tree_walker(node, native_rls_lock_sublinks, context);
}

/*
 * Apply row-level security policies to the backing relation RTEs of an
 * internally built query.
 *
 * The rewriter's fireRIRrules() performs this step for parsed queries, but
 * the decomposed internal queries are built inside the planner and never
 * pass through the rewriter, so without this their RTEs would carry no
 * securityQuals and RLS would be silently bypassed.  Mirrors the RLS loop
 * of fireRIRrules(), recursing into join subqueries (branch queries are
 * wrapped as subquery RTEs of the UNION).
 */
void
native_apply_rls_to_query(Query *query)
{
	int			rt_index = 0;
	ListCell   *lc;

	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);
		List	   *securityQuals = NIL;
		List	   *withCheckOptions = NIL;
		bool		hasRowSecurity = false;
		bool		hasSubLinks = false;

		rt_index++;

		/* Recurse into wrapped branch (or other subquery) queries. */
		if (rte->rtekind == RTE_SUBQUERY)
		{
			native_apply_rls_to_query(rte->subquery);
			continue;
		}

		/* Only plain relations can have RLS policies. */
		if (rte->rtekind != RTE_RELATION ||
			(rte->relkind != RELKIND_RELATION &&
			 rte->relkind != RELKIND_PARTITIONED_TABLE))
			continue;

		get_row_security_policies(query, rte, rt_index,
								  &securityQuals, &withCheckOptions,
								  &hasRowSecurity, &hasSubLinks);

		if (securityQuals != NIL)
		{
			if (hasSubLinks)
			{
				/* Lock relations referenced by the policy quals. */
				(void) native_rls_lock_sublinks((Node *) securityQuals, NULL);
			}

			/*
			 * Add the new security barrier quals ahead of any pre-existing
			 * security quals, exactly as fireRIRrules() does.
			 */
			rte->securityQuals = list_concat(securityQuals,
											 rte->securityQuals);
		}

		/*
		 * The decomposed queries are SELECT-only, so no WITH CHECK OPTIONS
		 * can apply; hasRowSecurity still matters for the plancache
		 * (dependsOnRLS).
		 */
		if (hasRowSecurity)
			query->hasRowSecurity = true;
		if (hasSubLinks)
			query->hasSubLinks = true;
	}
}

/*
 * Build, for one branch, the internal RTE_GRAPH_TABLE representing the
 * quantified (variable-length) hop described by vf, with concrete ghost
 * seed (source element 'srcpe' at 'src_rti') and concrete ghost terminal
 * ('termpe' at 'term_rti').  The RTE is appended to 'branch'; the terminal
 * (external join) quals are appended to *term_quals.  Returns the RT index
 * of the new RTE.
 *
 * For zero-hop quantifiers ({0,...}), the effective minimum depth is raised
 * to 1 in branches whose terminal element differs from the seed element:
 * a zero-length path ends at the seed vertex itself, which can only satisfy
 * the (concrete) terminal element if the two elements are the same.
 */
static int
native_build_vle_rte(RangeTblEntry *rte, native_vle_factor * vf,
					 struct path_element *srcpe, int src_rti,
					 struct path_element *termpe, int term_rti,
					 List **term_quals, Query *branch)
{
	Oid			graphid = rte->relid;
	List	   *src_keys;
	List	   *term_keys;
	int			nseed;
	int			seed_first = 1;
	int			term_first;
	int			eff_min;
	List	   *columns = NIL;
	List	   *colnames = NIL;
	List	   *seed_quals = NIL;
	RangeTblEntry *gs_rte;
	GraphPattern *gp;
	GraphElementPattern *pd;
	GraphElementPattern *edge_gep;
	GraphElementPattern *td;
	List	   *path_term;
	RTEPermissionInfo *perminfo;
	int			gs_rti;
	int			colno = 0;
	ListCell   *lc;

	src_keys = get_graph_element_key_columns(srcpe->elemoid,
											 Anum_pg_propgraph_element_pgekey);
	term_keys = get_graph_element_key_columns(termpe->elemoid,
											  Anum_pg_propgraph_element_pgekey);
	nseed = list_length(src_keys);

	eff_min = vf->min_depth;
	if (eff_min == 0 && srcpe->elemoid != termpe->elemoid)
		eff_min = 1;

	term_first = seed_first + nseed;

	/* The RT index of the new RTE: next in the branch's rtable. */
	gs_rti = list_length(branch->rtable) + 1;

	/* Output columns: seed key, terminal key, then the edge-list arrays. */
	foreach(lc, src_keys)
	{
		GraphElementKeyCol *kc = lfirst(lc);

		colno++;
		columns = lappend(columns,
						  makeTargetEntry((Expr *) makeVar(gs_rti, colno,
														   kc->typid, kc->typmod,
														   kc->collation, 0),
										  colno, pstrdup("gs_seed"), false));
		colnames = lappend(colnames, makeString(pstrdup("gs_seed")));
	}

	foreach(lc, term_keys)
	{
		GraphElementKeyCol *kc = lfirst(lc);

		colno++;
		columns = lappend(columns,
						  makeTargetEntry((Expr *) makeVar(gs_rti, colno,
														   kc->typid, kc->typmod,
														   kc->collation, 0),
										  colno, pstrdup("gs_term"), false));
		colnames = lappend(colnames, makeString(pstrdup("gs_term")));
	}

	foreach_node(GraphPropertyRef, gpr, vf->array_props)
	{
		colno++;
		columns = lappend(columns,
						  makeTargetEntry((Expr *) makeVar(gs_rti, colno,
														   gpr->typeId, gpr->typmod,
														   gpr->collation, 0),
										  colno, pstrdup("gs_arr"), false));
		colnames = lappend(colnames, makeString(pstrdup("gs_arr")));
	}

	/*
	 * Ghost seed element: its key is exposed as the first columns, and the
	 * seed qual (pd key = previous segment key) lives in the seed element's
	 * WHERE clause so the planner treats the scan as parameterized by the
	 * previous segment.
	 */
	{
		int			k = 0;

		foreach(lc, src_keys)
		{
			GraphElementKeyCol *kc = lfirst(lc);

			seed_quals = lappend(seed_quals,
								 make_key_equality((Node *) makeVar(gs_rti,
																	seed_first + k,
																	kc->typid,
																	kc->typmod,
																	kc->collation, 0),
												   (Node *) makeVar(src_rti,
																	kc->attnum,
																	kc->typid,
																	kc->typmod,
																	kc->collation, 0)));
			k++;
		}
	}

	/* Terminal (external join) quals: terminal key = gs terminal key. */
	{
		int			k = 0;

		foreach(lc, term_keys)
		{
			GraphElementKeyCol *kc = lfirst(lc);

			*term_quals = lappend(*term_quals,
								  make_key_equality((Node *) makeVar(term_rti,
																	 kc->attnum,
																	 kc->typid,
																	 kc->typmod,
																	 kc->collation, 0),
													(Node *) makeVar(gs_rti,
																	 term_first + k,
																	 kc->typid,
																	 kc->typmod,
																	 kc->collation, 0)));
			k++;
		}
	}

	pd = makeNode(GraphElementPattern);
	pd->kind = VERTEX_PATTERN;
	pd->variable = NULL;
	pd->labelexpr = NULL;
	pd->whereClause = (Node *) makeBoolExpr(AND_EXPR, seed_quals, -1);
	pd->quantifier = NULL;
	pd->location = -1;

	edge_gep = copyObject(vf->edge_gep);
	edge_gep->quantifier = list_make2_int(eff_min, vf->max_depth);

	td = makeNode(GraphElementPattern);
	td->kind = VERTEX_PATTERN;
	td->variable = NULL;
	td->labelexpr = NULL;
	td->whereClause = NULL;
	td->quantifier = NULL;
	td->location = -1;

	path_term = list_make3(pd, edge_gep, td);

	gp = makeNode(GraphPattern);
	gp->path_pattern_list = list_make1(path_term);
	gp->whereClause = NULL;

	gs_rte = makeNode(RangeTblEntry);
	gs_rte->rtekind = RTE_GRAPH_TABLE;
	gs_rte->relid = graphid;
	gs_rte->relkind = RELKIND_PROPGRAPH;
	gs_rte->graph_pattern = gp;
	gs_rte->graph_table_columns = columns;
	gs_rte->eref = makeAlias(pstrdup("graph_scan"), colnames);
	gs_rte->rellockmode = AccessShareLock;
	gs_rte->lateral = true;
	gs_rte->is_internal_graph = true;
	gs_rte->graph_seed_elem_oid = srcpe->elemoid;
	gs_rte->graph_vle_props = vf->array_props;

	perminfo = addRTEPermissionInfo(&branch->rteperminfos, gs_rte);
	perminfo->requiredPerms = ACL_SELECT;

	branch->rtable = lappend(branch->rtable, gs_rte);

	/* Fix up collations of the freshly built key quals. */
	{
		ParseState *pstate = make_parsestate(NULL);

		assign_expr_collations(pstate, (Node *) seed_quals);
		assign_expr_collations(pstate, (Node *) *term_quals);
	}

	return gs_rti;
}

/*
 * Mutator resolving GraphPropertyRef nodes against the concrete elements of
 * a branch and against the branch's internal graph RTEs (VLE edge-list
 * refs).  Mirrors replace_property_refs_mutator() for the concrete case.
 */
static Node *
native_replace_property_refs_mutator(Node *node, native_prop_ctx * ctx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Var		   *newvar = copyObject(var);

		/*
		 * If it's already a Var, it was a lateral reference; the branch is
		 * wrapped by the UNION, so raise the level by one.
		 */
		newvar->varlevelsup++;
		return (Node *) newvar;
	}
	else if (IsA(node, GraphPropertyRef))
	{
		GraphPropertyRef *gpr = (GraphPropertyRef *) node;

		/* VLE edge-list (array) reference. */
		if (gpr->vle_list)
		{
			foreach_ptr(native_vle_bind, vb, ctx->vle_binds)
			{
				int			prop = 0;

				if (vb->varname &&
					strcmp(vb->varname, gpr->elvarname) == 0)
				{
					foreach_ptr(GraphPropertyRef, ap, vb->array_props)
					{
						if (ap->propid == gpr->propid)
						{
							return (Node *) makeVar(vb->gs_rti,
													vb->array_first + prop,
													gpr->typeId, gpr->typmod,
													gpr->collation, 0);
						}
						prop++;
					}
					elog(ERROR, "graph VLE edge property not found in scan columns");
				}
			}
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("property \"%s\" for element variable \"%s\" not found",
							get_propgraph_property_name(gpr->propid),
							gpr->elvarname)));
		}

		/* Ordinary reference to a concrete element of the branch. */
		foreach_ptr(native_bind, bind, ctx->binds)
		{
			if (bind->varname && strcmp(bind->varname, gpr->elvarname) == 0)
			{
				Node	   *n;

				n = get_element_property_expr(bind->elemoid, gpr->propid,
											  bind->rti);
				if (!n)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("property \"%s\" for element variable \"%s\" not found",
									get_propgraph_property_name(gpr->propid),
									gpr->elvarname)));
				return n;
			}
		}

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("element variable \"%s\" not found", gpr->elvarname)));
	}

	return expression_tree_mutator(node, native_replace_property_refs_mutator,
								   ctx);
}

/*
 * Construct the Query for one fully-bound branch.  Returns NULL if the
 * combination is inconsistent (fixed edge-vertex links don't line up).
 */
static Query *
native_query_for_branch(native_decomp * dc, List *elems, List *vles)
{
	RangeTblEntry *rte = dc->rte;
	Query	   *path_query = makeNode(Query);
	List	   *fromlist = NIL;
	List	   *qual_exprs = NIL;
	List	   *binds = NIL;
	List	   *vle_binds = NIL;
	native_prop_ctx ctx;
	List	   *vars;
	int			i;
	ListCell   *lc;

	path_query->commandType = CMD_SELECT;

	/*
	 * Pass 1: add one RTE per factor, in pattern order.  Concrete elements
	 * become relation RTEs; VLE factors become internal graph RTEs.  With no
	 * same-variable merging, RT index of factor i is i+1.
	 */
	i = 0;
	foreach(lc, elems)
	{
		struct path_element *pe = lfirst(lc);
		native_vle_factor *vf = list_nth(vles, i);
		int			rti = list_length(path_query->rtable) + 1;
		RangeTblRef *rtr;

		Assert(rti == i + 1);

		if (vf != NULL)
		{
			struct path_element *srcpe = list_nth(elems, i - 1);
			struct path_element *termpe = list_nth(elems, i + 1);
			List	   *term_quals = NIL;
			native_vle_bind *vb;
			int			gs_rti;

			/* zero-hop: gs may not traverse; seed/term elements differ */
			gs_rti = native_build_vle_rte(rte, vf, srcpe, i, termpe, i + 2,
										  &term_quals, path_query);
			Assert(gs_rti == rti);
			qual_exprs = list_concat(qual_exprs, term_quals);

			vb = palloc_object(native_vle_bind);
			vb->varname = vf->edge_gep->variable;
			vb->gs_rti = gs_rti;
			vb->array_first = 1
				+ list_length(get_graph_element_key_columns(srcpe->elemoid,
															Anum_pg_propgraph_element_pgekey))
				+ list_length(get_graph_element_key_columns(termpe->elemoid,
															Anum_pg_propgraph_element_pgekey));
			vb->array_props = vf->array_props;
			{
				RangeTblEntry *gs_rte =
					list_nth(path_query->rtable, gs_rti - 1);
				GraphElementPattern *pd;

				pd = linitial_node(GraphElementPattern,
								   linitial(gs_rte->graph_pattern->path_pattern_list));
				vb->seed_quals = copyObject(pd->whereClause);
			}
			vle_binds = lappend(vle_binds, vb);
		}
		else
		{
			Relation	rel;
			ParseNamespaceItem *pni;
			native_bind *nb;

			rel = table_open(pe->reloid, AccessShareLock);
			pni = addRangeTableEntryForRelation(make_parsestate(NULL), rel,
												AccessShareLock,
												NULL, true, false);
			table_close(rel, NoLock);
			path_query->rtable = lappend(path_query->rtable, pni->p_rte);
			path_query->rteperminfos = lappend(path_query->rteperminfos,
											   pni->p_perminfo);
			pni->p_rte->perminfoindex = list_length(path_query->rteperminfos);

			nb = palloc_object(native_bind);
			nb->varname = pe->path_factor->variable;
			nb->elemoid = pe->elemoid;
			nb->rti = rti;
			binds = lappend(binds, nb);
		}

		rtr = makeNode(RangeTblRef);
		rtr->rtindex = rti;
		fromlist = lappend(fromlist, rtr);
		i++;
	}

	/* Pass 2: fixed edge links, element WHEREs, graph-level WHERE. */
	i = 0;
	foreach(lc, elems)
	{
		struct path_element *pe = lfirst(lc);

		if (pe == NULL)
		{
			/*
			 * VLE factor: keep the ghost seed's key equality with the
			 * previous segment so the scan is parameterized by it.
			 */
			foreach_ptr(native_vle_bind, vb, vle_binds)
			{
				if (vb->gs_rti == i + 1)
				{
					if (vb->seed_quals)
						qual_exprs = lappend(qual_exprs, vb->seed_quals);
					break;
				}
			}
		}
		else if (IS_EDGE_PATTERN(pe->path_factor->kind))
		{
			struct path_element *src_pe = list_nth(elems, i - 1);
			struct path_element *dest_pe = list_nth(elems, i + 1);
			Expr	   *edge_qual = NULL;

			if (src_pe->elemoid == pe->srcvertexid &&
				dest_pe->elemoid == pe->destvertexid)
				edge_qual = makeBoolExpr(AND_EXPR,
										 list_concat(copyObject(pe->src_quals),
													 copyObject(pe->dest_quals)),
										 -1);

			if (pe->path_factor->kind == EDGE_PATTERN_ANY &&
				dest_pe->elemoid == pe->srcvertexid &&
				src_pe->elemoid == pe->destvertexid)
			{
				List	   *src_quals = copyObject(pe->dest_quals);
				List	   *dest_quals = copyObject(pe->src_quals);
				Expr	   *rev_edge_qual;

				ChangeVarNodes((Node *) dest_quals, i, i + 2, 0);
				ChangeVarNodes((Node *) src_quals, i + 2, i, 0);
				rev_edge_qual = makeBoolExpr(AND_EXPR,
											 list_concat(src_quals, dest_quals),
											 -1);
				if (edge_qual)
					edge_qual = makeBoolExpr(OR_EXPR,
											 list_make2(edge_qual, rev_edge_qual),
											 -1);
				else
					edge_qual = rev_edge_qual;
			}

			if (edge_qual == NULL)
				return NULL;

			qual_exprs = lappend(qual_exprs, edge_qual);
		}

		if (pe && pe->path_factor->whereClause)
			qual_exprs = lappend(qual_exprs,
								 replace_property_refs(rte->relid,
													   pe->path_factor->whereClause,
													   list_make1(pe)));

		i++;
	}

	ctx.binds = binds;
	ctx.vle_binds = vle_binds;

	if (rte->graph_pattern->whereClause)
		qual_exprs = lappend(qual_exprs,
							 native_replace_property_refs_mutator(copyObject((Node *) rte->graph_pattern->whereClause),
																  &ctx));

	path_query->jointree = makeFromExpr(fromlist,
										qual_exprs ? (Node *) makeBoolExpr(AND_EXPR, qual_exprs, -1) : NULL);

	/* Construct the branch targetlist from the COLUMNS specification. */
	path_query->targetList = castNode(List,
									  native_replace_property_refs_mutator(copyObject((Node *) rte->graph_table_columns),
																		   &ctx));

	/*
	 * Mark the columns being accessed in the branch query as requiring SELECT
	 * privilege on the backing element tables.
	 */
	vars = pull_vars_of_level((Node *) list_make2(qual_exprs,
												  path_query->targetList), 0);
	foreach_node(Var, var, vars)
	{
		RTEPermissionInfo *perminfo;

		Assert(IsA(rt_fetch(var->varno, path_query->rtable), RangeTblEntry));
		perminfo = getRTEPermissionInfo(path_query->rteperminfos,
										rt_fetch(var->varno, path_query->rtable));
		perminfo->selectedCols = bms_add_member(perminfo->selectedCols,
												var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * Backing element tables can carry RLS policies: handled by the native
	 * planner in set_graph_pathlist(), before subquery_planner.
	 */

	return path_query;
}

/*
 * Recursively enumerate concrete elements for the non-quantified factors,
 * descending into every VLE factor without a choice.
 */
static void
native_queries_recurse(native_decomp * dc, int facpos, List *elems, List *vles)
{
	ListCell   *lc;

	check_stack_depth();

	if (facpos == dc->nfactors)
	{
		Query	   *path_query = native_query_for_branch(dc, elems, vles);

		if (path_query)
			dc->branch_queries = lappend(dc->branch_queries, path_query);
		return;
	}

	if (list_nth(dc->vle_factors, facpos) != NULL)
	{
		native_vle_factor *vf = list_nth(dc->vle_factors, facpos);

		native_queries_recurse(dc, facpos + 1,
							   lappend(list_copy(elems), NULL),
							   lappend(list_copy(vles), vf));
	}
	else
	{
		foreach(lc, list_nth(dc->elem_lists, facpos))
		{
			struct path_element *pe = lfirst(lc);

			native_queries_recurse(dc, facpos + 1,
								   lappend(list_copy(elems), pe),
								   lappend(list_copy(vles), NULL));
		}
	}
}

/*
 * Return the OIDs of the elements described by the given list of resolved
 * path elements (shared by the edge/vertex element lookups below).
 */
static List *
path_element_oids(List *pes)
{
	List	   *result = NIL;

	foreach_ptr(struct path_element, pe, pes)
		result = lappend_oid(result, pe->elemoid);

	return result;
}

/*
 * Return the OIDs of the edge elements matching the given edge element
 * pattern in the given property graph.  Used by the native planner to build
 * the GraphScan's inner (1-hop) expansion.
 */
List *
get_graph_edge_element_oids(Oid propgraphid, GraphElementPattern *gep)
{
	struct path_factor *src_pf;
	struct path_factor *edge_pf;
	struct path_factor *dest_pf;

	Assert(IS_EDGE_PATTERN(gep->kind));

	/*
	 * Element resolution keeps the edge factor's adjacent vertex factors (to
	 * build the source/destination key quals), so provide a minimal ghost
	 * vertex-edge-vertex path.
	 */
	src_pf = palloc0_object(struct path_factor);
	src_pf->factorpos = 0;
	src_pf->kind = VERTEX_PATTERN;

	dest_pf = palloc0_object(struct path_factor);
	dest_pf->factorpos = 2;
	dest_pf->kind = VERTEX_PATTERN;

	edge_pf = palloc0_object(struct path_factor);
	edge_pf->factorpos = 1;
	edge_pf->kind = gep->kind;
	edge_pf->labelexpr = gep->labelexpr;
	edge_pf->variable = gep->variable;
	edge_pf->whereClause = gep->whereClause;
	edge_pf->src_pf = src_pf;
	edge_pf->dest_pf = dest_pf;

	return path_element_oids(get_path_elements_for_path_factor(propgraphid,
															   edge_pf));
}

/*
 * Return the OIDs of the vertex elements matching the given vertex element
 * pattern in the given property graph.  Used by the native planner to
 * estimate the row count of a GraphScan (the terminal pattern's label set
 * determines which vertex tables may end a walk).
 */
List *
get_graph_vertex_element_oids(Oid propgraphid, GraphElementPattern *gep)
{
	struct path_factor *pf;

	Assert(gep->kind == VERTEX_PATTERN);

	pf = palloc0_object(struct path_factor);
	pf->factorpos = 0;
	pf->kind = VERTEX_PATTERN;
	pf->labelexpr = gep->labelexpr;
	pf->variable = gep->variable;
	pf->whereClause = gep->whereClause;

	return path_element_oids(get_path_elements_for_path_factor(propgraphid,
															   pf));
}

/*
 * Decompose a GRAPH_TABLE clause into a Query for native execution.
 *
 * All quantified (variable-length) hops are kept as internal
 * RTE_GRAPH_TABLEs (to be planned as GraphScan nodes); everything else is
 * decomposed into relational JOINs over the backing element tables, one
 * UNION ALL branch per concrete element combination (resolving all label
 * disjunction at the branch level).
 */
Query *
decomposeGraphNative(RangeTblEntry *rte)
{
	GraphPattern *gp = rte->graph_pattern;
	List	   *path_pattern;
	native_decomp dc;
	List	   *factors = NIL;
	List	   *elem_lists = NIL;
	List	   *vle_factors = NIL;
	int			factorpos = 0;
	Query	   *result;

	Assert(list_length(gp->path_pattern_list) == 1);
	path_pattern = linitial(gp->path_pattern_list);

	/*
	 * Build one path factor per element pattern.  Reuse of a variable across
	 * multiple element patterns is not supported yet (Phase F).
	 */
	foreach_node(GraphElementPattern, gep, path_pattern)
	{
		struct path_factor *pf;

		foreach_ptr(struct path_factor, other, factors)
		{
			if (other->variable && gep->variable &&
				strcmp(other->variable, gep->variable) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("reuse of element variable \"%s\" is not yet supported by the native executor",
								gep->variable)));
		}

		pf = palloc0_object(struct path_factor);
		pf->factorpos = factorpos;
		pf->kind = gep->kind;
		pf->variable = gep->variable;
		pf->labelexpr = gep->labelexpr;
		pf->whereClause = gep->whereClause;
		factors = lappend(factors, pf);
		factorpos++;
	}

	/* Link edges to their adjacent vertex factors. */
	foreach_ptr(struct path_factor, pf, factors)
	{
		if (IS_EDGE_PATTERN(pf->kind))
		{
			pf->src_pf = list_nth(factors, pf->factorpos - 1);
			pf->dest_pf = list_nth(factors, pf->factorpos + 1);
		}
	}

	/* Resolve elements per factor; mark the quantified edge factors. */
	{
		foreach_ptr(struct path_factor, pf, factors)
		{
			GraphElementPattern *gep = list_nth(path_pattern, pf->factorpos);

			if (IS_EDGE_PATTERN(pf->kind) && gep->quantifier != NULL)
			{
				native_vle_factor *vf = palloc0_object(native_vle_factor);
				List	   *edes;

				vf->edge_gep = gep;
				vf->min_depth = linitial_int(gep->quantifier);
				vf->max_depth = lsecond_int(gep->quantifier);
				vf->array_props = get_vle_array_props(rte, gep->variable);
				edes = get_path_elements_for_path_factor(rte->relid, pf);
				foreach_ptr(struct path_element, pe, edes)
					vf->edge_element_oids = lappend_oid(vf->edge_element_oids,
														pe->elemoid);

				elem_lists = lappend(elem_lists, NIL);
				vle_factors = lappend(vle_factors, vf);
			}
			else
			{
				elem_lists = lappend(elem_lists,
									 get_path_elements_for_path_factor(rte->relid,
																	   pf));
				vle_factors = lappend(vle_factors, NULL);
			}
		}
	}

	dc.rte = rte;
	dc.elem_lists = elem_lists;
	dc.vle_factors = vle_factors;
	dc.nfactors = list_length(factors);
	dc.branch_queries = NIL;

	native_queries_recurse(&dc, 0, NIL, NIL);

	if (dc.branch_queries == NIL)
		result = generate_query_for_empty_path_pattern(rte);
	else
		result = generate_union_from_pathqueries(&dc.branch_queries);

	return result;
}
