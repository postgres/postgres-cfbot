/*-------------------------------------------------------------------------
 *
 * vci_planner.h
 *    Data struct definitions needed for analysis to rewrite plans
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_planner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_PLANNER_H
#define VCI_PLANNER_H

#include "access/attnum.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

/**
 * Types for internal use only by planners
 *
 * NestLoop and HashJoin do not actually replace VCI Plan.
 * So only record the possibility of including NestLoop and HashJoin
 * in the parallel plan group
 */
typedef enum
{
	VCI_INNER_PLAN_TYPE_NONE = 0,
	VCI_INNER_PLAN_TYPE_SCAN,
	VCI_INNER_PLAN_TYPE_SORT,
	VCI_INNER_PLAN_TYPE_AGG,
	VCI_INNER_PLAN_TYPE_HASHJOIN,
	VCI_INNER_PLAN_TYPE_NESTLOOP,
	VCI_INNER_PLAN_TYPE_REDIST,
} vci_inner_plan_type_t;

/**
 * Whether plan node is suitable for VCi execution
 */
typedef enum
{
	VCI_PLAN_COMPAT_OK = 0,
	VCI_PLAN_COMPAT_FORBID_TYPE,	/* VCI execution prohibited type */
	VCI_PLAN_COMPAT_UNSUPPORTED_OBJ,
} vci_plan_compat_t;

typedef struct
{
	/**
	 * VCI Plan Type
	 */
	vci_inner_plan_type_t plan_type;

	AttrNumber	scan_plan_no;

	int			preset_eflags;

	Bitmapset  *def_param_ids;
	Bitmapset  *use_param_ids;

	vci_plan_compat_t plan_compat;

} vci_plan_attr_t;

typedef enum
{
	VCI_PARAM_EXEC_UNKNOWN = 0,

	VCI_PARAM_EXEC_NESTLOOP,

	VCI_PARAM_EXEC_INITPLAN,

	VCI_PARAM_EXEC_SUBPLAN,
} vci_param_exec_type_t;

typedef struct
{
	vci_param_exec_type_t type;
	Bitmapset  *def_plan_nos;
	int			num_def_plans;
	Bitmapset  *use_plan_nos;
	int			num_use_plans;
	int			plan_id;
} vci_param_exec_attr_t;

typedef enum
{
	VCI_SUBPLAN_UNKNOWN = 0,
	VCI_SUBPLAN_INITPLAN,
	VCI_SUBPLAN_SUBPLAN,
} vci_subplan_type_t;

typedef struct
{
	Plan	   *topmostplan;	/** Topmost Plan */
	vci_subplan_type_t type;
	Bitmapset  *plan_ids;

	bool		has_analyzed_parallel;
} vci_subplan_attr_t;

typedef struct
{
	PlannedStmt *plannedstmt;

	EState	   *estate;

	vci_subplan_attr_t *subplan_attr_map;

	int			max_subplan_attrs;

	int		   *subplan_order_array;

	vci_plan_attr_t *plan_attr_map;

	int			max_plan_attrs;

	AttrNumber	last_plan_no;

	vci_param_exec_attr_t *param_exec_attr_map;

	int			current_plan_id;

	AttrNumber	current_plan_no;

	bool		forbid_parallel_exec;

	bool		suppress_vp;

	struct
	{
		List	   *main_plan_list;

		Bitmapset  *plan_group;

		Bitmapset  *correlated_subplans;

		Bitmapset  *local_param_ids;
	}			parallel;

} vci_rewrite_plan_context_t;

extern bool vci_preanalyze_plan_tree(PlannedStmt *target, vci_rewrite_plan_context_t *rp_context, int eflags, bool *isGather);
extern void vci_register_plan_id(Plan *plan, int plan_id, void *context);
extern void vci_expand_plan_attr_map(vci_rewrite_plan_context_t *rp_context);
extern vci_inner_plan_type_t vci_get_inner_plan_type(vci_rewrite_plan_context_t *context, const Plan *plan);
extern AttrNumber vci_get_inner_scan_plan_no(vci_rewrite_plan_context_t *context, const Plan *plan);
extern void vci_set_inner_plan_type_and_scan_plan_no(vci_rewrite_plan_context_t *context, Plan *plan, vci_inner_plan_type_t plan_type, AttrNumber scan_plan_no);

#endif							/* VCI_PLANNER_H */
