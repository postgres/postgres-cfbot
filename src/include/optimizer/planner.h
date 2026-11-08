/*-------------------------------------------------------------------------
 *
 * planner.h
 *	  prototypes for planner.c.
 *
 * Note that the primary entry points for planner.c are declared in
 * optimizer/optimizer.h, because they're intended to be called from
 * non-planner code.  Declarations here are meant for use by other
 * planner modules.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/planner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_H
#define PLANNER_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"


typedef struct ExplainState ExplainState;	/* defined in explain_state.h */

/* Hook for plugins to get control in planner() */
typedef PlannedStmt *(*planner_hook_type) (Query *parse,
										   const char *query_string,
										   int cursorOptions,
										   ParamListInfo boundParams,
										   ExplainState *es);
extern PGDLLIMPORT planner_hook_type planner_hook;

/* Hook for plugins to get control after PlannerGlobal is initialized */
typedef void (*planner_setup_hook_type) (PlannerGlobal *glob, Query *parse,
										 const char *query_string,
										 int cursorOptions,
										 double *tuple_fraction,
										 ExplainState *es);
extern PGDLLIMPORT planner_setup_hook_type planner_setup_hook;

/* Hook for plugins to get control before PlannerGlobal is discarded */
typedef void (*planner_shutdown_hook_type) (PlannerGlobal *glob, Query *parse,
											const char *query_string,
											PlannedStmt *pstmt);
extern PGDLLIMPORT planner_shutdown_hook_type planner_shutdown_hook;

/*
 * Hook for plugins to get control when grouping_planner() plans upper rels.
 * New stages may be added, so plugins must ignore stages they do not handle.
 * input_rel may be NULL for set-operation stages, which have no finalized
 * input relation; output_rel is always non-NULL and receives paths.  extra
 * is stage-specific and is currently NULL for UPPERREL_PARTIAL_SETOP.
 *
 * UPPERREL_PARTIAL_SETOP is invoked only when a partial set-operation
 * de-duplication path is being considered, and can occur more than once in
 * a query with multiple non-flattened set operations.  Its output_rel
 * initially contains partial paths; a plugin can add partial paths (and may
 * add complete paths, which are also considered).  The corresponding
 * upper_targets[] entry is not initialized.
 */
typedef void (*create_upper_paths_hook_type) (PlannerInfo *root,
											  UpperRelationKind stage,
											  RelOptInfo *input_rel,
											  RelOptInfo *output_rel,
											  void *extra);
extern PGDLLIMPORT create_upper_paths_hook_type create_upper_paths_hook;


extern PlannedStmt *standard_planner(Query *parse, const char *query_string,
									 int cursorOptions,
									 ParamListInfo boundParams,
									 ExplainState *es);

extern PlannerInfo *subquery_planner(PlannerGlobal *glob, Query *parse,
									 char *plan_name,
									 PlannerInfo *parent_root,
									 PlannerInfo *alternative_root,
									 bool hasRecursion, double tuple_fraction,
									 SetOperationStmt *setops);

extern RowMarkType select_rowmark_type(RangeTblEntry *rte,
									   LockClauseStrength strength);

extern bool limit_needed(Query *parse);

extern void mark_partial_aggref(Aggref *agg, AggSplit aggsplit);

extern Path *get_cheapest_fractional_path(RelOptInfo *rel,
										  double tuple_fraction);


extern RelOptInfo *create_unique_paths(PlannerInfo *root, RelOptInfo *rel,
									   SpecialJoinInfo *sjinfo);

/*
 * Shared de-duplication support (used by DISTINCT and set operations).
 *
 * These helpers cover only the mechanics of the de-duplication path shapes.
 * Callers remain responsible for deciding whether de-duplication is possible,
 * for estimating the number of groups, and for the applicable strategy
 * policy: DISTINCT estimates groups from its input relation, while set
 * operations derive them from their children.  DISTINCT also hard-disables
 * hashing when enable_hashagg is off and alternatives exist, whereas set
 * operations leave it as a cost penalty, consistent with their other hash
 * paths.
 */
extern Path *create_dedup_hashagg_path(PlannerInfo *root, RelOptInfo *rel,
									   Path *subpath, PathTarget *target,
									   List *group_clause,
									   double num_groups);
extern Path *create_dedup_limit_path(PlannerInfo *root, RelOptInfo *rel,
									 Path *subpath);
extern RelOptInfo *create_partial_dedup_rel(PlannerInfo *root,
											UpperRelationKind kind,
											RelOptInfo *source_rel,
											Relids relids,
											PathTarget *target);
extern void add_partial_dedup_hash_path(PlannerInfo *root,
										RelOptInfo *partial_rel,
										Path *partial_input,
										List *group_clause,
										double partial_groups,
										bool allow_hash);
extern void finish_partial_dedup_rel(PlannerInfo *root,
									 RelOptInfo *source_rel,
									 RelOptInfo *hook_input_rel,
									 RelOptInfo *partial_rel,
									 UpperRelationKind kind,
									 bool preserve_ordering);
extern void add_final_dedup_hash_paths(PlannerInfo *root,
									   RelOptInfo *input_rel,
									   RelOptInfo *final_rel,
									   List *group_clause,
									   double num_groups);

extern char *choose_plan_name(PlannerGlobal *glob, const char *name,
							  bool always_number);

#endif							/* PLANNER_H */
