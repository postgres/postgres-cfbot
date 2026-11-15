/*-------------------------------------------------------------------------
 *
 * goo.h
 *     prototype for the greedy operator ordering join search
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *     src/include/optimizer/goo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GOO_H
#define GOO_H

#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

extern RelOptInfo *goo_join_search(PlannerInfo *root, int levels_needed,
								   List *initial_rels);

#endif							/* GOO_H */
