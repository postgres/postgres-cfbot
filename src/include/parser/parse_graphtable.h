/*-------------------------------------------------------------------------
 *
 * parse_graphtable.h
 *		parsing of GRAPH_TABLE
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_graphtable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_GRAPHTABLE_H
#define PARSE_GRAPHTABLE_H

#include "nodes/pg_list.h"
#include "parser/parse_node.h"

extern Node *transformGraphTablePropertyRef(ParseState *pstate, ColumnRef *cref);

extern Node *transformGraphPattern(ParseState *pstate, GraphPattern *graph_pattern);

/*
 * Collect the OIDs of the labels referenced by a label expression (a single
 * GraphLabelRef or a BoolExpr OR of GraphLabelRef nodes).  Returns NIL if
 * labelexpr is NULL or references no labels.  A NULL label expression on an
 * element pattern means the pattern matches every label of the graph; call
 * get_graph_all_label_oids() for that set.
 */
extern List *get_label_oids_for_labelexpr(Node *labelexpr);

/*
 * Return the OIDs of all labels belonging to the given property graph.
 * Used for a graph element pattern without a label expression, which
 * matches every label of the graph (SQL/PGQ "%|!%" semantics).
 */
extern List *get_graph_all_label_oids(Oid propgraphid);

/*
 * Map a graph element pattern kind to the element-kind character ('v' for
 * vertex, 'e' for edge) and the human-readable class name ("vertex"/"edge").
 * Returns false for pattern kinds that do not denote a vertex or edge (e.g.
 * PAREN_EXPR), leaving the outputs untouched.  kind_str may be NULL if the
 * caller only needs the character.  Used by the label-kind validator.
 */
extern bool graph_element_kind_info(GraphElementPatternKind kind,
									char *element_kind, const char **kind_str);

/*
 * Verify that every label explicitly referenced by the label expressions in
 * the given graph pattern is associated with some element of the required
 * kind (vertex or edge); error otherwise.  Called at parse time; DDL made
 * after parsing (cached plans) is handled by the planner-side element
 * resolution.
 */
extern void validate_graph_element_label_kinds(GraphPattern *pattern,
											   Oid graph_oid);

#endif							/* PARSE_GRAPHTABLE_H */
