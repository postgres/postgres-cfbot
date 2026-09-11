/*-------------------------------------------------------------------------
 *
 * rewriteGraphTable.h
 *		Support for rewriting GRAPH_TABLE clauses.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/rewrite/rewriteGraphTable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEGRAPHTABLE_H
#define REWRITEGRAPHTABLE_H

#include "nodes/parsenodes.h"

extern Query *rewriteGraphTable(Query *parsetree, int rt_index);

/*
 * Build the relational Query representing the graph pattern described by the
 * given RTE_GRAPH_TABLE (joins of the backing element tables, UNIONed for
 * label disjunction).  Used by the rewrite fallback and by the native
 * planner for the unquantified parts of a pattern.
 */
extern Query *decomposeGraphTable(RangeTblEntry *rte);

/*
 * Build the native Query for the given RTE_GRAPH_TABLE: every quantified
 * (variable-length) hop is kept as an internal RTE_GRAPH_TABLE (planned as a
 * GraphScan node), everything else is decomposed into relational JOINs over
 * the backing element tables.  Used by the native planner.
 */
extern Query *decomposeGraphNative(RangeTblEntry *rte);

/*
 * Return a property expression for the given graph element and property,
 * resolving to a column (or expression) of the element's table, with Vars
 * referencing rtindex.  NULL if the element does not carry the property.
 */
extern Node *get_element_property_expr(Oid elemoid, Oid propoid, int rtindex);

/*
 * Return the OIDs of the edge elements matching the given edge element
 * pattern in the given property graph.  Used by the native planner to build
 * the GraphScan's inner (1-hop) expansion.
 */
extern List *get_graph_edge_element_oids(Oid propgraphid,
										 GraphElementPattern *gep);
extern List *get_graph_vertex_element_oids(Oid propgraphid,
										   GraphElementPattern *gep);

/*
 * One key column of a graph element: the element table's column (attnum)
 * plus its type/typmod/collation, used to build key equality expressions.
 */
typedef struct GraphElementKeyCol
{
	AttrNumber	attnum;
	Oid			typid;
	int32		typmod;
	Oid			collation;
}			GraphElementKeyCol;

/*
 * Return the key columns (attnum/type/typmod/collation) of the given graph
 * element, read from the given key column array of pg_propgraph_element
 * (pgekey for a vertex element, pgesrckey/pgedestkey for an edge element).
 * Shared by the rewrite, the native planner and the native executor so the
 * catalog layout is decoded in one place.
 */
extern List *get_graph_element_key_columns(Oid elemoid, int key_attnum);

/*
 * Return an equality operator suitable for a graph key datatype: the type's
 * default equality operator.  Key values are compared with this operator,
 * both by the filters pushed into the GraphScan's inner (1-hop) expansion
 * and by the GraphScan executor itself.
 */
extern Oid	key_equality_operator(Oid typid);

/*
 * Return the VLE edge-list (array) property references (GraphPropertyRef
 * nodes) of the given (quantified) edge variable, as referenced from the
 * COLUMNS and the graph-level WHERE clause of the given graph RTE.
 */
extern List *get_vle_array_props(RangeTblEntry *rte, const char *edge_var);

/*
 * Apply row-level security policies to the backing relation RTEs of an
 * internally built query.  The rewriter's fireRIRrules() does this for
 * parsed queries; the decomposed internal query never passes through the
 * rewriter, so its RTEs would otherwise carry no securityQuals and RLS
 * would be silently bypassed.
 */
extern void native_apply_rls_to_query(Query *query);

#endif							/* REWRITEGRAPHTABLE_H */
