/*-------------------------------------------------------------------------
 *
 * vci_planner.c
 *	  Plan rewrite routine(sequential only)
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_planner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"
#include "utils/syscache.h"

#include "vci.h"
#include "vci_columns_data.h"

#include "vci_mem.h"
#include "vci_executor.h"
#include "vci_utils.h"
#include "vci_planner.h"
#include "vci_supported_oid.h"

/*
 *		rt_fetch
 *
 * NB: this will crash and burn if handed an out-of-range RT index
 */
#define rt_fetch(rangetable_index, rangetable) \
	((RangeTblEntry *) list_nth(rangetable, (rangetable_index)-1))

/*
 *		getrelid
 *
 *		Given the range index of a relation, return the corresponding
 *		relation OID.  Note that InvalidOid will be returned if the
 *		RTE is for a non-relation-type RTE.
 */
#define getrelid(rangeindex,rangetable) \
	(rt_fetch(rangeindex, rangetable)->relid)

/**
 * Used to pass auxiliary information about the table to vci_can_rewrite_custom_scan().
 */
typedef struct
{
	/** reloid of table to be selected for rewrite */
	Oid			reloid;

	/** oid of selected VCI index. InvalidOid if not rewriteable. */
	Oid			indexOid;

	/** Copy reltuples of selected table */
	double		estimate_tuples;

	/** Bitmap of referenced column (attribute). NULL if not rewriteable. */
	Bitmapset  *attrs_used;
} vci_table_info_t;

/**
 *  Used to search plan tree with vci_gather_used_attrs() and vci_gather_one_used_attr(),
 *  and record attributes references in tables specified by scanrelid.
 */
typedef struct
{
	Index		scanrelid;

	Bitmapset  *attrs_used;
} vci_gather_used_attrs_t;

/**
 *  Search plan tree with vci_renumber_attrs() and vci_renumber_on_attr(), and rewrite attribute number.
 *  Used to replace varattno in Var.
 */
typedef struct
{
	Index		scanrelid;

	/** New attribute number map. newattno = attr_map[oldattno] */
	AttrNumber *attr_map;
} vci_renumber_attrs_t;

typedef struct
{
	Plan	   *father_plan;
	Plan	   *gather_plan;
} father_gather_plans;

static bool vci_optimize_phase1(PlannedStmt *plannedstmt, vci_rewrite_plan_context_t *rp_context, int eflags);
static bool vci_rewrite_plan_tree_mutator(Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed);
static bool vci_rewrite_plan_node(Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed);
static bool vci_rewrite_scan_node_via_column_store(Plan **plan_p, Plan *parent, void *context, bool *changed);
static bool vci_insert_material_node_mutator(Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed);
static VciSort *vci_create_custom_sort(Sort *sortnode, AttrNumber scan_plan_no);
static VciAgg *vci_create_custom_agg(Agg *aggnode, AttrNumber scan_plan_no, bool suppress_vp);
static List *vci_reconstruct_qualification(Scan *scannode);
static bool vci_can_rewrite_custom_scan(Scan *scannode, List *targetlist, List *qual, Plan *parent, vci_table_info_t *result);
static Bitmapset *vci_gather_used_attrs_in_plan(Plan *plan, Index scanrelid);
static bool vci_gather_used_attrs(Node *node, void *context);
static void vci_gather_one_used_attr(AttrNumber *attr_p, void *context);
static void vci_minimize_tlist_of_scan(Scan *scannode, Plan *parent, Index parent_refer_relid, Bitmapset *attrs_used_from_parent);
static VciScan *vci_create_custom_scan_via_column_store(Scan *scannode, const vci_table_info_t *table_info, List *tlist, List *qual, bool suppress_vp);

static bool vci_contain_inapplicable_expr_walker(Node *node, void *context);
static bool vci_contain_nestloop_param_expr_walker(Node *node, void *context);
static bool vci_renumber_attrs(Node *node, void *context);
static void vci_renumber_one_attr(AttrNumber *attr_p, void *context);
static bool vci_tlist_consists_of_only_simple_vars(List *tlist, Index scanrelid);

static AttrNumber vci_satisfies_vci_join(vci_rewrite_plan_context_t *rp_context, Join *join);

static bool vci_is_supported_operation(Oid oid);
static bool vci_is_not_user_defined_type(Oid oid);

static void vci_update_plan_tree(PlannedStmt *plannedstmt);
static List *vci_update_target_list(Plan *plan, Plan *gather_plan);

static bool vci_update_plan_walker(Plan *plan, void *plans);

/**
 * Attempt to rewrite plan and return the rewritten planned stmt is successful.
 *
 * @param[in] src             original planned stmt
 * @param[in] eflags          flag to be passed to ExecInitNode
 * @param[in] snapshot        snapshot
 *
 * @retval non NULL plan after rewrite
 * @retval NULL    rewrite failed
 */
PlannedStmt *
vci_generate_custom_plan(PlannedStmt *src, int eflags, Snapshot snapshot)
{
	int			nParamExec;
	bool		changed,
				dummy;
	bool		isGather = false;
	PlannedStmt *target;
	vci_rewrite_plan_context_t rp_context;

	vci_register_applicable_udf(snapshot);

	target = copyObjectImpl(src);

	/*
	 * Initialize plan rewrite information
	 */
	memset(&rp_context, 0, sizeof(rp_context));

	rp_context.plannedstmt = target;
	rp_context.max_subplan_attrs = list_length(target->subplans) + 1;
	rp_context.subplan_attr_map = palloc0_array(vci_subplan_attr_t, rp_context.max_subplan_attrs);
	rp_context.subplan_order_array = palloc_array(int, rp_context.max_subplan_attrs);
	rp_context.max_plan_attrs = 16;
	rp_context.plan_attr_map = palloc0_array(vci_plan_attr_t, rp_context.max_plan_attrs);
	rp_context.last_plan_no = 0;
	nParamExec = list_length(target->paramExecTypes);
	rp_context.param_exec_attr_map = palloc0_array(vci_param_exec_attr_t, nParamExec);

	for (int i = 0; i < rp_context.max_subplan_attrs; i++)
		rp_context.subplan_order_array[i] = i;

	/*
	 * Preparing for analysis
	 */
	if (vci_preanalyze_plan_tree(target, &rp_context, eflags, &isGather))
	{
		elog(DEBUG1, "Not suitable plan");
		return NULL;
	}

	/* Adjust plan tree by moving oss gather plan */
	if (isGather)
		vci_update_plan_tree(target);

	/*
	 * Phase 1: Basic VCI plan rewrite
	 */
	changed = vci_optimize_phase1(target, &rp_context, eflags);

	if (!changed)
	{
		elog(DEBUG1, "No plan to be rewritten");
		return NULL;
	}

	/*
	 * VCI plan node do not support backward scan an mark/restore, so insert
	 * Material node if eflag needs them.
	 */
	vci_plannedstmt_tree_mutator(target, vci_insert_material_node_mutator, vci_register_plan_id, &rp_context, eflags, &dummy);

	/* Disable community parallelism */
	/* target->parallelModeNeeded=0; */

	elog(DEBUG1, "Rewrite plan tree");

	return target;
}

/*==========================================================================*/
/* Plan rewrite                                                        */
/*==========================================================================*/

/**
 * Basic part of VCI plan rewrite
 *
 * @param[in]     plannedstmt plan
 * @param[in,out] rp_context  Plan rewrite information
 * @param[in]     eflags       flag to be passed to ExecInitNode
 *
 * @return true if rewrite succeed, false if failed
 */
static bool
vci_optimize_phase1(PlannedStmt *plannedstmt, vci_rewrite_plan_context_t *rp_context, int eflags)
{
	bool		changed = false;

	rp_context->forbid_parallel_exec = false;

	if (vci_plannedstmt_tree_mutator_order(plannedstmt, vci_rewrite_plan_tree_mutator, vci_register_plan_id, rp_context,
										   eflags, &changed, rp_context->subplan_order_array))
		return false;

	return changed;
}

/**
 * Rewrite plan subtree starting with plan into VCI plan
 *
 * @param[in,out] plan_p  Pointer to the plan subtree to start rewriting
 * @param[in,out] parent  parent plan node of plan
 * @param[in,out] context additional context
 * @param[in]     eflags  flag to be passed to ExecInitNode
 * @param[out]    changed write true if rewrite is executed
 *
 * @return true when callback function stops cycle, false if cycle is complete
 */
static bool
vci_rewrite_plan_tree_mutator(Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;
	Plan	   *plan;
	bool		saved_forbid_parallel_exec;
	bool		result;

	plan = *plan_p;

	saved_forbid_parallel_exec = rp_context->forbid_parallel_exec;
	rp_context->forbid_parallel_exec = false;

	if (vci_plan_tree_mutator(plan_p, parent, vci_rewrite_plan_tree_mutator, context, eflags, changed))
		return true;

	result = vci_rewrite_plan_node(plan_p, parent, context, eflags, changed);

	if (rp_context->plan_attr_map[plan->plan_no].plan_compat == VCI_PLAN_COMPAT_OK)
		rp_context->plan_attr_map[plan->plan_no].plan_compat = rp_context->forbid_parallel_exec ? VCI_PLAN_COMPAT_UNSUPPORTED_OBJ : VCI_PLAN_COMPAT_OK;
	rp_context->forbid_parallel_exec |= saved_forbid_parallel_exec;

	return result;
}

/**
 * Rewrute plan node of *plan_p with VCI plan
 *
 * @param[in,out] plan_p  Pointer to the plan to start rewriting
 * @param[in,out] parent  parent plan of plan
 * @param[in,out] context additional context
 * @param[in]     eflags  flag to be passed to ExecInitNode
 * @param[out]    changed write true if rewrite is executed
 *
 * @return true when callback function stops cycle, false if cycle is complete
 */
static bool
vci_rewrite_plan_node(Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;
	Plan	   *plan;
	AttrNumber	scan_plan_no = 0;

	plan = *plan_p;

	/* Determine if Vector processing is possible */
	rp_context->suppress_vp = false;

	if (rp_context->plan_attr_map[plan->plan_no].plan_compat != VCI_PLAN_COMPAT_OK)
	{
		rp_context->forbid_parallel_exec = true;
		return false;
	}

	/* Determine if there are any expression that cannot be rewritten in plan */
	if (vci_expression_walker(plan, vci_contain_inapplicable_expr_walker, context))
	{
		rp_context->forbid_parallel_exec = true;
		return false;
	}

	switch (nodeTag(plan))
	{
		default:
			break;

		case T_HashJoin:
			{
				HashJoin   *hjnode = (HashJoin *) plan;

				if (!VciGuc.enable_hashjoin)
					return false;

				scan_plan_no = vci_satisfies_vci_join(rp_context, &hjnode->join);

				if (scan_plan_no == 0)
					return false;

				elog(DEBUG1, "Replace VCI HashJoin");

				*changed = true;

				vci_set_inner_plan_type_and_scan_plan_no(rp_context, plan, VCI_INNER_PLAN_TYPE_HASHJOIN, scan_plan_no);
			}
			break;

		case T_NestLoop:
			{
				NestLoop   *nlnode = (NestLoop *) plan;

				if (!VciGuc.enable_nestloop)
					return false;

				scan_plan_no = vci_satisfies_vci_join(rp_context, &nlnode->join);

				if (scan_plan_no == 0)
					return false;

				elog(DEBUG1, "Replace VCI NestLoop");

				*changed = true;

				vci_set_inner_plan_type_and_scan_plan_no(rp_context, plan, VCI_INNER_PLAN_TYPE_NESTLOOP, scan_plan_no);
			}
			break;

		case T_Sort:
			{
				Sort	   *sortnode = (Sort *) plan;

				if (!VciGuc.enable_sort)
					return false;

				/*
				 * Can only be rewritten when outer is VCI
				 * Scan/HashJoin/NestLoop. VCI Agg cannot be rewritten. Sort
				 * plan nodes are note consecutive, so VCI Sort will not
				 * occur.
				 */
				switch (vci_get_inner_plan_type(rp_context, outerPlan(plan)))
				{
					case VCI_INNER_PLAN_TYPE_SCAN:
					case VCI_INNER_PLAN_TYPE_HASHJOIN:
					case VCI_INNER_PLAN_TYPE_NESTLOOP:
						/* OK */
						scan_plan_no = vci_get_inner_scan_plan_no(rp_context, outerPlan(plan));
						break;
					default:
						return false;
				}

				Assert(scan_plan_no > 0);

				elog(DEBUG1, "Replace VCI Sort");

				*plan_p = (Plan *) vci_create_custom_sort(sortnode, scan_plan_no);
				*changed = true;

				vci_set_inner_plan_type_and_scan_plan_no(rp_context, plan, VCI_INNER_PLAN_TYPE_SORT, scan_plan_no);
			}
			break;

		case T_Agg:
			{
				Agg		   *aggnode = (Agg *) plan;

				switch (aggnode->aggstrategy)
				{
					case AGG_SORTED:
						if (!VciGuc.enable_sortagg)
							return false;
						break;

					case AGG_HASHED:
						if (!VciGuc.enable_hashagg)
							return false;
						break;

					case AGG_PLAIN:
						if (!VciGuc.enable_plainagg)
							return false;
						break;

					default:
						break;	/* LCOV_EXCL_LINE */
				}

				switch (aggnode->aggstrategy)
				{
					case AGG_SORTED:
						if (vci_get_inner_plan_type(rp_context, outerPlan(plan)) != VCI_INNER_PLAN_TYPE_SORT)
							return false;
						/* OK */
						scan_plan_no = vci_get_inner_scan_plan_no(rp_context, outerPlan(plan));
						break;

					case AGG_HASHED:
					case AGG_PLAIN:
						switch (vci_get_inner_plan_type(rp_context, outerPlan(plan)))
						{
							case VCI_INNER_PLAN_TYPE_SCAN:
							case VCI_INNER_PLAN_TYPE_HASHJOIN:
							case VCI_INNER_PLAN_TYPE_NESTLOOP:
								/* OK */
								scan_plan_no = vci_get_inner_scan_plan_no(rp_context, outerPlan(plan));
								break;
							default:
								return false;
						}
						break;

					default:
						break;	/* LCOV_EXCL_LINE */
				}

				Assert(scan_plan_no > 0);

				elog(DEBUG1, "Replace VCI Agg");

				*plan_p = (Plan *) vci_create_custom_agg(aggnode, scan_plan_no, rp_context->suppress_vp);
				*changed = true;

				vci_set_inner_plan_type_and_scan_plan_no(rp_context, plan, VCI_INNER_PLAN_TYPE_AGG, scan_plan_no);
			}
			break;

		case T_SeqScan:
			if (!VciGuc.enable_seqscan)
				return false;
			else
			{
				bool		each_changed = false;

				switch (VciGuc.table_scan_policy)
				{
					case VCI_TABLE_SCAN_POLICY_COLUMN_ONLY:
						if (true == vci_rewrite_scan_node_via_column_store(plan_p, parent, context, &each_changed))
							return false;
						break;

					default:
						break;
				}

				*changed |= each_changed;
			}
			break;

		case T_IndexScan:
			if (!VciGuc.enable_indexscan)
				return false;

			if (((IndexScan *) plan)->indexorderdir != NoMovementScanDirection)
			{
				elog(DEBUG1, "Need sorting rows if indexscan with indexorderdir(%d) is replaced",
					 ((IndexScan *) plan)->indexorderdir);
				return false;
			}
			goto process_scan_like_plan;

		case T_BitmapHeapScan:
			if (!VciGuc.enable_bitmapheapscan)
				return false;

			goto process_scan_like_plan;

	process_scan_like_plan:
			{
				if (VciGuc.table_scan_policy == VCI_TABLE_SCAN_POLICY_COLUMN_ONLY)
					if (true == vci_rewrite_scan_node_via_column_store(plan_p, parent, context, changed))
						return false;
			}
			break;

		case T_CustomScan:
		case T_CustomPlanMarkPos:
			return false;
	}

	return false;
}

/**
 * Get bitmap of parameters updated by SubPlan via initPlan called from
 * the given plan.
 */
static bool
vci_rewrite_scan_node_via_column_store(Plan **plan_p, Plan *parent, void *context, bool *changed)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;
	Plan	   *plan = *plan_p;
	Scan	   *scannode = (Scan *) plan;
	vci_table_info_t table_info;
	List	   *tlist = NIL;
	List	   *qual = NIL;
	AttrNumber	scan_plan_no;

	if (rp_context->plan_attr_map[plan->plan_no].preset_eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK))
		return false;

	table_info.reloid = getrelid(scannode->scanrelid, rp_context->plannedstmt->rtable);
	table_info.indexOid = InvalidOid;
	table_info.attrs_used = NULL;

	tlist = scannode->plan.targetlist;
	qual = vci_reconstruct_qualification(scannode);

	scan_plan_no = plan->plan_no;

	if (nodeTag(plan) != T_SeqScan)
	{
		if (expression_tree_walker((Node *) qual, vci_contain_nestloop_param_expr_walker, context))
		{
			elog(DEBUG1, "Scan's qual contains any inapplicable expression");
			return false;
		}
	}

	/*
	 * Determines whether VCI index containes attributes accessed by the
	 * query, and if so returns the OID of the VCI index and bitmapset of
	 * attributes accessed in the query.
	 */
	if (!vci_can_rewrite_custom_scan(scannode, tlist, qual, parent, &table_info))
		return false;

	tlist = scannode->plan.targetlist;

	elog(DEBUG1, "Replace VCI Scan [column store]: convert from %s",
		 VciGetNodeName(nodeTag(plan)));

	*plan_p = (Plan *) vci_create_custom_scan_via_column_store(scannode, &table_info, tlist, qual, rp_context->suppress_vp);
	*changed = true;

	vci_set_inner_plan_type_and_scan_plan_no(rp_context, plan, VCI_INNER_PLAN_TYPE_SCAN, scan_plan_no);

	return false;
}

/**
 * Insert Material node into the tree that has already been rewritten to VCI plan node
 * as necessary.
 *
 * @param[in,out] plan_p  rewritten plan tree
 * @param[in,out] parent  parent plan of plan
 * @param[in,out] context additional context
 * @param[in]     eflags  flag to be passed to ExecInitNode
 * @param[out]    changed write true if rewrite is executed
 *
 * @return true when callback function stop cycle, false if cycle is complete
 *
 * None of the VCI plan nodes support mark/restore, backward scan, or rewind (efficient scan).
 * If they are needed, insert a Materialnode above them to handle.
 */
static bool
vci_insert_material_node_mutator(Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;
	Material   *newplan;
	VciPlan    *targetplan;
	Plan	   *plan;

	plan = *plan_p;

	switch (nodeTag(plan))
	{
		case T_CustomScan:
		case T_CustomPlanMarkPos:
			switch (vci_get_inner_plan_type(rp_context, plan))
			{
				case VCI_INNER_PLAN_TYPE_SORT:

					/*
					 * VCI Sort node does not support EXEC_FLAG_BACKWARD and
					 * EXEC_FLAG_MARK, so insert a Material node between them.
					 *
					 * VCI Sort can be used if only EXEC_FLAG_REWIND
					 */
					if ((eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0)
					{
						targetplan = (VciPlan *) plan;
						goto maybe_need_material_node;
					}
					break;

				case VCI_INNER_PLAN_TYPE_SCAN:
				case VCI_INNER_PLAN_TYPE_AGG:
					Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));
					/* pass through */
					/* pgr0007 */

				default:
					break;
			}
			break;

		case T_Limit:
			if (outerPlan(plan) && (vci_get_inner_plan_type(rp_context, outerPlan(plan)) == VCI_INNER_PLAN_TYPE_SORT))
			{
				if ((eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0)
				{
					targetplan = (VciPlan *) outerPlan(plan);
					goto maybe_need_material_node;
				}
			}
			break;

		default:
			break;
	}

	if (vci_plan_tree_mutator(plan_p, parent, vci_insert_material_node_mutator, context, eflags, changed))
		return true;

	return false;

maybe_need_material_node:
	newplan = makeNode(Material);

	newplan->plan.targetlist = vci_generate_pass_through_target_list(plan->targetlist);
	newplan->plan.qual = NIL;
	newplan->plan.lefttree = plan;
	newplan->plan.plan_no = ++rp_context->last_plan_no;
	vci_expand_plan_attr_map(rp_context);

	copy_plan_costsize(&newplan->plan, plan);

	newplan->plan.extParam = bms_copy(plan->extParam);
	newplan->plan.allParam = bms_copy(plan->allParam);

	newplan->plan.initPlan = plan->initPlan;
	plan->initPlan = NULL;

	*plan_p = (Plan *) newplan;
	*changed = true;

	rp_context->plan_attr_map[targetplan->cscan.scan.plan.plan_no].preset_eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	return false;
}

/**
 * Create VCI Sort node
 */
static VciSort *
vci_create_custom_sort(Sort *sortnode, AttrNumber scan_plan_no)
{
	VciSort    *sort;

	sort = palloc0_object(VciSort);

	sort->vci.cscan.scan.plan = sortnode->plan;
	sort->vci.cscan.scan.plan.type = T_CustomPlanMarkPos;	/* Mark restore support */
	sort->vci.cscan.flags = VCI_CUSTOMPLAN_SORT | CUSTOMPATH_SUPPORT_BACKWARD_SCAN | CUSTOMPATH_SUPPORT_MARK_RESTORE;
	sort->vci.cscan.custom_relids = NULL;
	sort->vci.cscan.methods = &vci_sort_scan_methods;

	sort->vci.scan_plan_no = scan_plan_no;
	sort->vci.orig_plan = (Plan *) sortnode;

	sort->numCols = sortnode->numCols;
	sort->sortColIdx = sortnode->sortColIdx;
	sort->sortOperators = sortnode->sortOperators;
	sort->collations = sortnode->collations;
	sort->nullsFirst = sortnode->nullsFirst;

	return sort;
}

/**
 * Create VCI Agg node
 */
static VciAgg *
vci_create_custom_agg(Agg *aggnode, AttrNumber scan_plan_no, bool suppress_vp)
{
	VciAgg	   *agg;

	agg = palloc0_object(VciAgg);

	agg->vci.cscan.scan.plan = aggnode->plan;
	agg->vci.cscan.scan.plan.type = T_CustomScan;	/* Not mark restore
													 * support */
	agg->vci.cscan.flags = VCI_CUSTOMPLAN_AGG;
	agg->vci.cscan.custom_relids = NULL;

	switch (aggnode->aggstrategy)
	{
		case AGG_HASHED:
			agg->vci.cscan.methods = &vci_hashagg_scan_methods;
			break;

		case AGG_SORTED:
			agg->vci.cscan.methods = &vci_groupagg_scan_methods;
			break;

		case AGG_PLAIN:
			agg->vci.cscan.methods = &vci_agg_scan_methods;
			break;

		default:
			break;				/* LCOV_EXCL_LINE */
	}

	agg->vci.scan_plan_no = scan_plan_no;
	agg->vci.orig_plan = (Plan *) aggnode;

	agg->aggstrategy = aggnode->aggstrategy;
	agg->numCols = aggnode->numCols;
	agg->grpColIdx = aggnode->grpColIdx;
	agg->grpOperators = aggnode->grpOperators;
	agg->grpCollations = aggnode->grpCollations;
	agg->numGroups = aggnode->numGroups;

	return agg;
}

/**
 * Determine if Scan plan node can be rewritten to VCI Scan
 *
 * Checks that there is a VCI index with all columns (attributes) to be read
 * from the table. If there is more than one matching VCI index, the earlier one
 * in the index list is used.
 *
 * @param[in]     scanrelid    relid of target table
 * @param[in]     reloid       oid of target table
 * @param[in]     targetlist   targetlist of target Scan plan
 * @param[in]     qual         qual of target Scan plan
 * @param[in]     parent       Parent plan node of target Scan plan
 * @param[in,out] table_info   Input information about targe table and returns information obtained within this function
 *
 * @retval true if rewriteable, false if not
 */
static bool
vci_can_rewrite_custom_scan(Scan *scannode, List *targetlist, List *qual, Plan *parent, vci_table_info_t *table_info)
{
	Index		scanrelid;
	vci_gather_used_attrs_t gcontext;
	int			orig_natts,
				opt_natts;
	Relation	tableRel;
	double		estimate_tuples;
	Oid			foundVciIndexOid = InvalidOid;
	Bitmapset  *attrs_used = NULL;
	bool		do_minimize_tlist = false;
	Bitmapset  *attrs_used_from_parent = NULL;
	List	   *indexoidlist = NIL;
	ListCell   *indexoidscan;
	Index		parent_refer_relid = 0;

	scanrelid = scannode->scanrelid;

	gcontext.scanrelid = scanrelid;
	gcontext.attrs_used = NULL;

	if (expression_tree_walker((Node *) qual, vci_gather_used_attrs, &gcontext) ||
		expression_tree_walker((Node *) targetlist, vci_gather_used_attrs, &gcontext))
		return false;

	attrs_used = gcontext.attrs_used;

	orig_natts = opt_natts = bms_num_members(attrs_used);

	if (orig_natts == 0)
		return false;

	if (parent)
	{
		if ((Plan *) scannode == outerPlan(parent))
			parent_refer_relid = OUTER_VAR;
		else if ((Plan *) scannode == innerPlan(parent))
			parent_refer_relid = INNER_VAR;
	}

	/*
	 * To improve the read performance of SeqScan, PostgreSQL may sort the
	 * target list according to the order of columns in the heap tuple,
	 * including columns that are not actually referenced by the upper node.
	 *
	 * In a columnar system, such optimizations are harmful, so optimizations
	 * are needed to stop reading unnecessary columns.
	 *
	 * First, calculate the columns that are truly referenced from the upper
	 * node.
	 *
	 * This optimization only looks at the next higher node. Hash does not
	 * work because it works in conjunction with HashJoin, which is even
	 * higher up.
	 */
	if ((parent_refer_relid != 0) && vci_tlist_consists_of_only_simple_vars(targetlist, scanrelid))
	{
		switch (nodeTag(parent))
		{
			case T_Agg:
			case T_Group:
			case T_HashJoin:
			case T_MergeJoin:
			case T_NestLoop:
				attrs_used_from_parent = vci_gather_used_attrs_in_plan(parent, parent_refer_relid);
				do_minimize_tlist = true;
				break;
			default:
				break;
		}
	}

	if (do_minimize_tlist)
	{
		Bitmapset  *new_attrs_used;

		gcontext.scanrelid = scanrelid;
		gcontext.attrs_used = NULL;

		expression_tree_walker((Node *) qual, vci_gather_used_attrs, &gcontext);

		new_attrs_used = bms_add_members(gcontext.attrs_used, attrs_used_from_parent);

		/*
		 * Compare the attributes referenced by Scan with the attributes
		 * referenced by the WHERE clause and the attributes referenced by the
		 * parent node.
		 */
		if (bms_equal(attrs_used, new_attrs_used))
		{
			bms_free(new_attrs_used);
			bms_free(attrs_used_from_parent);

			attrs_used_from_parent = NULL;
			do_minimize_tlist = false;
		}
		else
		{
			bms_free(attrs_used);

			attrs_used = new_attrs_used;

			opt_natts = bms_num_members(attrs_used);
		}
	}

	/*
	 * Lock table for index calculation
	 */
	tableRel = table_open(table_info->reloid, AccessShareLock);

	estimate_tuples = (double) Max(tableRel->rd_rel->reltuples, 0);

	elog(DEBUG1, "vci index: target table \"%s\"(oid=%u) tuples(rows=%.0f,extents=%u)",
		 NameStr(tableRel->rd_rel->relname), table_info->reloid,
		 estimate_tuples, (int) (estimate_tuples / VCI_NUM_ROWS_IN_EXTENT));

	if (estimate_tuples < (double) VciGuc.table_rows_threshold)
	{
		elog(DEBUG1, "vci index: target table \"%s\"(oid=%u) is too few rows. threshold=%d",
			 NameStr(tableRel->rd_rel->relname), table_info->reloid, VciGuc.table_rows_threshold);

		goto done;
	}

	/*
	 * Find the VCI index from the indexes existing in the table and check
	 * whether the table contains attrs_used.
	 */
	indexoidlist = RelationGetIndexList(tableRel);

	foreach(indexoidscan, indexoidlist)
	{
		Relation	indexRel;
		Oid			indexOid;

		indexOid = lfirst_oid(indexoidscan);
		indexRel = index_open(indexOid, AccessShareLock);

		if (isVciIndexRelation(indexRel))
		{
			Form_pg_index indexStruct = indexRel->rd_index;
			Bitmapset  *attrs_indexed;

			/*
			 * If the index is valid, but cannot yet be used, ignore it. (See
			 * L.190 src/backend/optimizer/util/plancat.c) See
			 * src/backend/access/heap/README.HOT for discussion.
			 */
			if (indexStruct->indcheckxmin &&
				!TransactionIdPrecedes(HeapTupleHeaderGetXmin(indexRel->rd_indextuple->t_data),
									   TransactionXmin))
			{
				index_close(indexRel, AccessShareLock);
				continue;
			}

			attrs_indexed = vci_MakeIndexedColumnBitmap(indexOid,
														CurrentMemoryContext,
														AccessShareLock);

			if (bms_is_subset(attrs_used, attrs_indexed))
			{
				elog(DEBUG1, "vci index: adopt index \"%s\"(oid=%u)",
					 NameStr(indexRel->rd_rel->relname), indexOid);

				foundVciIndexOid = indexOid;
			}
			else
			{
				int			num,
							x;

				elog(DEBUG1, "vci index: don't match index \"%s\"(oid=%u)",
					 NameStr(indexRel->rd_rel->relname), indexOid);

				num = bms_num_members(attrs_used);
				x = 1;
				while (num > 0)
				{
					if (bms_is_member(x, attrs_used))
					{
						elog(DEBUG1, "\tattrnum = %d%s", x, bms_is_member(x, attrs_indexed) ? " x" : "");
						num--;
					}
					x++;
				}
			}

			bms_free(attrs_indexed);
		}

		index_close(indexRel, AccessShareLock);

		if (OidIsValid(foundVciIndexOid))
			break;
	}

	list_free(indexoidlist);

done:
	table_close(tableRel, AccessShareLock);

	if (OidIsValid(foundVciIndexOid))
	{
		if (do_minimize_tlist)
		{
			elog(DEBUG1, "vci index: minimize targetlist %d -> %d handing over %s",
				 orig_natts, opt_natts, VciGetNodeName(nodeTag(parent)));

			vci_minimize_tlist_of_scan(scannode, parent, parent_refer_relid, attrs_used_from_parent);
			bms_free(attrs_used_from_parent);
		}

		table_info->indexOid = foundVciIndexOid;
		table_info->estimate_tuples = estimate_tuples;
		table_info->attrs_used = attrs_used;
	}
	else
	{
		bms_free(attrs_used);
	}

	return OidIsValid(foundVciIndexOid);
}

/**
 * Determine if the given target list consists only of Simple Vars
 * referencing a single input tuple.
 */
static bool
vci_tlist_consists_of_only_simple_vars(List *tlist, Index scanrelid)
{
	ListCell   *tl;
	Index		attno = 1;

	foreach(tl, tlist)
	{
		TargetEntry *tle;
		Var		   *var;

		tle = (TargetEntry *) lfirst(tl);

		if (!tle->expr || !IsA(tle->expr, Var))
			return false;

		var = (Var *) tle->expr;

		if (var->varno != scanrelid)
			return false;

		if (var->varattno != attno)
			return false;

		attno++;
	}

	return true;
}

/**
 * Collect the bitmap of attributes referenced as scanrelid within the specified plan node.
 */
static Bitmapset *
vci_gather_used_attrs_in_plan(Plan *plan, Index scanrelid)
{
	vci_gather_used_attrs_t gcontext;

	gcontext.scanrelid = scanrelid;
	gcontext.attrs_used = NULL;

	if (vci_expression_and_colid_walker(plan, vci_gather_used_attrs, vci_gather_one_used_attr, &gcontext))
	{
		bms_free(gcontext.attrs_used);
		return NULL;
	}

	return gcontext.attrs_used;
}

/**
 * Scan Var node in the VCI Scan node and obtain the attrno of attributes
 * that require data supply from the VCI index.
 */
static bool
vci_gather_used_attrs(Node *node, void *context)
{
	vci_gather_used_attrs_t *gcontext = (vci_gather_used_attrs_t *) context;

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				if (gcontext->scanrelid != var->varno)
					return false;

				gcontext->attrs_used = bms_add_member(gcontext->attrs_used, var->varattno);
			}
			return false;

		default:
			break;
	}

	return expression_tree_walker(node, vci_gather_used_attrs, context);
}

/**
 * Records to vci_gather_used_attrs_t because it is *attr_p attribute going to be referenced
 */
static void
vci_gather_one_used_attr(AttrNumber *attr_p, void *context)
{
	vci_gather_used_attrs_t *gcontext = (vci_gather_used_attrs_t *) context;

	Assert(*attr_p > 0);

	gcontext->attrs_used = bms_add_member(gcontext->attrs_used, *attr_p);
}

/**
 * Delete nodes in targetlist of Scan node that are not referenced by higher-level nodes.
 * At the same time, change the attno of outer var or inner var within higher-leve nodes.
 */
static void
vci_minimize_tlist_of_scan(Scan *scannode, Plan *parent, Index parent_refer_relid, Bitmapset *attrs_used_from_parent)
{
	vci_renumber_attrs_t rcontext;
	AttrNumber	last_attr;
	int			j;
	AttrNumber	resno;
	List	   *tlist;
	List	   *new_tlist = NIL;
	ListCell   *lc;

	tlist = scannode->plan.targetlist;

	last_attr = list_length(tlist);

	rcontext.scanrelid = parent_refer_relid;
	rcontext.attr_map = palloc0_array(AttrNumber, (last_attr + 1));

	j = 1;
	for (int i = 1; i <= last_attr; i++)
		if (bms_is_member(i, attrs_used_from_parent))
			rcontext.attr_map[i] = j++;

	if (vci_expression_and_colid_walker(parent, vci_renumber_attrs, vci_renumber_one_attr, &rcontext))
		elog(ERROR, "planner failed to minimize tlist of scan");

	resno = 1;
	new_tlist = NIL;
	foreach(lc, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		Assert(IsA(tle, TargetEntry));

		if (rcontext.attr_map[tle->resno] > 0)
		{
			tle->resno = resno++;
			new_tlist = lappend(new_tlist, tle);
		}
	}

	pfree(rcontext.attr_map);

	scannode->plan.targetlist = new_tlist;
}

/**
 * Renumber attribute numbers in the subtree under the specified expression node.
 */
static bool
vci_renumber_attrs(Node *node, void *context)
{
	vci_renumber_attrs_t *rcontext = (vci_renumber_attrs_t *) context;

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				if (rcontext->scanrelid != var->varno)
					return false;

				if (var->varattno <= InvalidAttrNumber)
					return true;

				var->varattno = rcontext->attr_map[var->varattno];
			}
			return false;

		default:
			break;
	}

	return expression_tree_walker(node, vci_renumber_attrs, context);
}

/**
 * Renumber attribute number located at the position of attr_p
 */
static void
vci_renumber_one_attr(AttrNumber *attr_p, void *context)
{
	vci_renumber_attrs_t *rcontext = (vci_renumber_attrs_t *) context;

	Assert(*attr_p > 0);

	*attr_p = rcontext->attr_map[*attr_p];
}

/**
 * Combine qual when Scan derived node returned to SeqScan node
 */
static List *
vci_reconstruct_qualification(Scan *scannode)
{
	List	   *qual = scannode->plan.qual;

	switch (nodeTag(scannode))
	{
		case T_SeqScan:
			qual = list_copy(qual);
			break;

		case T_IndexScan:
			qual = list_copy(qual);
			if (((IndexScan *) scannode)->indexqualorig)
				qual = list_concat(qual, ((IndexScan *) scannode)->indexqualorig);
			break;

		case T_BitmapHeapScan:
			qual = list_copy(qual);
			if (((BitmapHeapScan *) scannode)->bitmapqualorig)
				qual = list_concat(qual, ((BitmapHeapScan *) scannode)->bitmapqualorig);
			break;

		default:
			Assert(0);
			break;
	}

	return qual;
}

/**
 * Create VCI Scan node
 */
static VciScan *
vci_create_custom_scan_via_column_store(Scan *scannode, const vci_table_info_t *table_info, List *tlist, List *qual, bool suppress_vp)
{
	VciScan    *scan;

	scan = palloc0_object(VciScan);

	scan->vci.cscan.scan.plan = scannode->plan;
	scan->vci.cscan.scan.plan.parallel_aware = false;
	scan->vci.cscan.scan.plan.type = T_CustomPlanMarkPos;

	scan->vci.cscan.scan.plan.targetlist = tlist;
	scan->vci.cscan.scan.plan.qual = qual;

	scan->vci.cscan.scan.scanrelid = scannode->scanrelid;

	scan->vci.cscan.flags = VCI_CUSTOMPLAN_SCAN | CUSTOMPATH_SUPPORT_MARK_RESTORE;
	scan->vci.cscan.custom_relids = bms_make_singleton(scannode->scanrelid);
	scan->vci.cscan.methods = &vci_scan_scan_methods;

	scan->vci.scan_plan_no = scan->vci.cscan.scan.plan.plan_no;
	scan->vci.orig_plan = (Plan *) scannode;

	scan->scan_mode = VCI_SCAN_MODE_COLUMN_STORE;
	scan->scanrelid = scannode->scanrelid;
	scan->reloid = table_info->reloid;
	scan->indexoid = table_info->indexOid;
	scan->attr_used = table_info->attrs_used;
	scan->num_attr_used = bms_num_members(table_info->attrs_used);
	scan->estimate_tuples = table_info->estimate_tuples;
	scan->is_all_simple_vars = vci_tlist_consists_of_only_simple_vars(tlist, scannode->scanrelid);

	return scan;
}

/**
 * Return true when expression node that cannot be executed in custom plan is detected,
 * false if they are all custom plan applicable
 */
static bool
vci_contain_inapplicable_expr_walker(Node *node, void *context)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;

	Assert(context);

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

				/* varattno == InvalidAttrNumber means it's a whole-row Var */
				if (var->varattno == InvalidAttrNumber)
					return true;

				/*
				 * varattno < InvalidAttrNumber means it's a system-defined
				 * attribute
				 */
				else if (var->varattno < InvalidAttrNumber)
					return true;
			}
			break;

		case T_FuncExpr:
			{
				FuncExpr   *expr = (FuncExpr *) node;

				if (expr->funcretset)
				{
					elog(DEBUG1, "FuncExpr contains returning-set function");
					return true;
				}

				if (expr->funcvariadic)
				{
					elog(DEBUG1, "FuncExpr contains funcvariadic");
					return true;
				}

				if (!vci_is_supported_function(expr->funcid))
				{
					elog(DEBUG1, "FuncExpr contains not-supported function: oid=%d", expr->funcid);
					return true;
				}

				if (!vci_is_not_user_defined_type(expr->funcresulttype))
				{
					elog(DEBUG1, "FuncExpr contains user defined type: oid=%d", expr->funcresulttype);
					return true;
				}

				/*
				 * Always returns true here to create vci_runs_in_plan()
				 * result. Overwrite to function.
				 */
				if (expr->funcid == vci_special_udf_info.vci_runs_in_plan_funcoid)
					expr->funcid = vci_special_udf_info.vci_always_return_true_funcoid;
			}
			break;

		case T_OpExpr:
		case T_DistinctExpr:	/* struct-equivalent to OpExpr */
		case T_NullIfExpr:		/* struct-equivalent to OpExpr */
			{
				OpExpr	   *expr = (OpExpr *) node;

				if (expr->opretset)
				{
					elog(DEBUG1, "%s contains returning-set function", VciGetNodeName(nodeTag(node)));
					return true;
				}

				if (!vci_is_supported_operation(expr->opfuncid))
				{
					elog(DEBUG1, "%s contains not-supported operation: oid=%d", VciGetNodeName(nodeTag(node)), expr->opfuncid);
					return true;
				}

				if (!vci_is_not_user_defined_type(expr->opresulttype))
				{
					elog(DEBUG1, "%s contains user defined type: oid=%d", VciGetNodeName(nodeTag(node)), expr->opresulttype);
					return true;
				}
			}
			break;

		case T_Param:
			{
				Param	   *param = (Param *) node;
				int			paramid = param->paramid;

				/* Not support PARAM_EXTERN or PARAM_SUBLINK */
				if (param->paramkind != PARAM_EXEC)
				{
					elog(DEBUG1, "Param contains extern or sublink");
					return true;
				}

				/*
				 * Check Param defined or referenced by multiple plan node
				 */
				switch (rp_context->param_exec_attr_map[paramid].type)
				{
					case VCI_PARAM_EXEC_NESTLOOP:
						/* VCI compatible, for calls via NestLoop */
						break;

					case VCI_PARAM_EXEC_INITPLAN:
					case VCI_PARAM_EXEC_SUBPLAN:

						/*
						 * not VCI compatible, for calls via initPlan or
						 * SubPlan
						 */
						if (rp_context->param_exec_attr_map[paramid].num_def_plans > 1)
						{
							elog(DEBUG1, "Param contains multi defining plans");
							return true;
						}

						if (rp_context->param_exec_attr_map[paramid].num_use_plans > 1)
						{
							elog(DEBUG1, "Param contains multi referencing plans");
							return true;
						}
						break;

						/* LCOV_EXCL_START */
					default:

						/*
						 * Commenting out below code as there is possibility
						 * to reach here when optimizer optimizes the plan to
						 * remove subplan node itself. E.g: Create view V1 as
						 * SELECT *, (SELECT d FROM t11 WHERE t11.a = t1.a
						 * LIMIT 1) AS d FROM t1 WHERE a > 5; and run  SELECT *
						 * FROM v1 where  a=3;
						 */
						/* elog(PANIC, "Should not reach here."); */
						break;
						/* LCOV_EXCL_STOP */
				}
			}
			break;

		case T_Const:
		case T_List:
			break;

		case T_Aggref:
			{
				Aggref	   *aggref = (Aggref *) node;

				/* Not support ordered-set or hypothetical */
				if (aggref->aggkind != AGGKIND_NORMAL)
				{
					elog(DEBUG1, "Aggref contains %c", aggref->aggkind);
					return true;
				}

				/* Not support polymorphic and variadic aggregation */
				if (aggref->aggvariadic)
				{
					elog(DEBUG1, "Aggref contains variadic aggregation");
					return true;
				}

				/* Not support FILTER expression */
				if (aggref->aggfilter != NULL)
				{
					elog(DEBUG1, "Aggref contains FILTER expression");
					return true;
				}

				/* Not support DISTINCT */
				if (aggref->aggdistinct != NIL)
				{
					elog(DEBUG1, "Aggref contains DISTINCT");
					return true;
				}

				/* Not support ORDER BY */
				if (aggref->aggorder != NIL)
				{
					elog(DEBUG1, "Aggref contains ORDER BY");
					return true;
				}

				/* Not support user-defined aggregation */
				if (!vci_is_supported_aggregation(aggref))
					return true;
			}
			break;

		case T_ScalarArrayOpExpr:
			break;

		case T_BoolExpr:
			break;

		case T_RelabelType:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
			break;
		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;

				if (ntest->argisrow)
				{
					elog(DEBUG1, "NullTest contains row-format");
					return true;
				}
			}
			break;

		case T_BooleanTest:
		case T_TargetEntry:
			break;

		case T_CoerceViaIO:
			break;

		case T_CaseExpr:
		case T_CaseTestExpr:
			break;

		case T_SubPlan:
			return true;

		case T_ArrayExpr:
		case T_ArrayCoerceExpr:
		case T_ConvertRowtypeExpr:
		case T_RowExpr:
		case T_RowCompareExpr:
		case T_SubscriptingRef:
		case T_WindowFunc:
		case T_XmlExpr:
		case T_WindowClause:
		case T_CommonTableExpr:
		case T_FieldSelect:
		case T_FieldStore:
		case T_RangeTblFunction:
		case T_AlternativeSubPlan:
		case T_SetOperationStmt:
		case T_AppendRelInfo:
		case T_WithCheckOption: /* nserting/updating an auto-updatable view */
		case T_CurrentOfExpr:	/* CURRENT OF cursor_name */
		case T_CoerceToDomain:
		case T_CoerceToDomainValue:
		case T_GroupingFunc:
		case T_SQLValueFunction:
		case T_NextValueExpr:
			return true;

		case T_Query:
		case T_FromExpr:
		case T_JoinExpr:
		case T_PlaceHolderVar:
		case T_PlaceHolderInfo:
		case T_CollateExpr:
		case T_SubLink:
		case T_RangeTblRef:
		case T_SortGroupClause:
		case T_NamedArgExpr:
		case T_SetToDefault:	/* a DEFAULT marker in an INSERT or UPDATE
								 * command */
			return true;		/* LCOV_EXCL_LINE */

		default:
			/* LCOV_EXCL_START */
			elog(ERROR, "unrecognized node type: %s(%d)",
				 VciGetNodeName(nodeTag(node)), (int) nodeTag(node));
			break;
			/* LCOV_EXCL_STOP */
	}

	return expression_tree_walker(node, vci_contain_inapplicable_expr_walker, context);
}

/**
 * Returns true if it references Param defined in NestLoop
 */
static bool
vci_contain_nestloop_param_expr_walker(Node *node, void *context)
{
	vci_rewrite_plan_context_t *rp_context = (vci_rewrite_plan_context_t *) context;

	if (node == NULL)
		return false;

	if (nodeTag(node) == T_Param)
	{
		Param	   *param = (Param *) node;
		int			paramid = param->paramid;

		if (rp_context->param_exec_attr_map[paramid].type == VCI_PARAM_EXEC_NESTLOOP)
		{
			elog(DEBUG1, "Param contains non-permitted paramId");
			return true;
		}
	}

	return expression_tree_walker(node, vci_contain_nestloop_param_expr_walker, context);
}

/*==========================================================================*/
/* Determine if function/typeis supported in VCI */
/*==========================================================================*/

/**
 * Determine if Join is supported
 *
 * @param[in] jointype Join type
 * @return true if supported, false if not
 */
bool
vci_is_supported_jointype(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
		case JOIN_ANTI:
		case JOIN_LEFT:
			return true;

		case JOIN_RIGHT:
		case JOIN_FULL:
		default:
			return false;
	}
}

/**
 * Determine whether Join plan node can be incorporated into parallel plan group.
 *
 * @param[in]     rp_context Join type
 * @param[in]     join       Join type
 *
 * @return 0 if cannot be incorporated, return plan_no of the VCI Scan that will
 *         result in partitioned table
 */
static AttrNumber
vci_satisfies_vci_join(vci_rewrite_plan_context_t *rp_context, Join *join)
{
	Plan	   *outer,
			   *inner;

	if (!vci_is_supported_jointype(join->jointype))
		return 0;

	outer = outerPlan(join);
	inner = innerPlan(join);

	if (rp_context->plan_attr_map[outer->plan_no].plan_compat != VCI_PLAN_COMPAT_OK)
	{
		elog(DEBUG1, "Join's outer subtree contains not-parallel-executable plannode");
		return 0;
	}

	if (rp_context->plan_attr_map[inner->plan_no].plan_compat != VCI_PLAN_COMPAT_OK)
	{
		elog(DEBUG1, "Join's inner subtree contains not-parallel-executable plannode");
		return 0;
	}

	/*
	 * Check if outer can be used as partitioned table
	 *
	 * Rewriteable only when VCI Scan/HashJoin/NestLoop. Not rewriteable for
	 * VCI Sort/VCI Agg.
	 */
	switch (vci_get_inner_plan_type(rp_context, outer))
	{
		case VCI_INNER_PLAN_TYPE_SCAN:
		case VCI_INNER_PLAN_TYPE_HASHJOIN:
		case VCI_INNER_PLAN_TYPE_NESTLOOP:
			/* OK */
			return vci_get_inner_scan_plan_no(rp_context, outer);

		default:
			break;
	}

	/*
	 * If outer cannot be used as partitioned table, try to use inner.
	 * However, inner-side is generally unsuitable for partitioned table, so
	 * stricter restrictions are imposed than on outer.
	 */

	if (nodeTag(inner) == T_Hash)
		inner = outerPlan(inner);
	else
		return 0;

	if ((inner == NULL) || (join->jointype != JOIN_INNER))
		return 0;

	if (inner->plan_rows < (double) VciGuc.table_rows_threshold)
		return 0;

	/*
	 * outer-side should be less than threshold
	 *
	 * This restriction is imposed because performance deteriorates when a
	 * partitioned table is established on the inner-side when the outer side
	 * is too large.
	 */
	if ((double) VciGuc.table_rows_threshold <= outer->plan_rows)
		return 0;

	if ((vci_get_inner_plan_type(rp_context, inner) == VCI_INNER_PLAN_TYPE_SCAN) &&
		(inner->allParam == NULL))
	{
		switch (nodeTag(outer))
		{
			case T_SeqScan:
			case T_BitmapHeapScan:

			case T_IndexScan:
				/* OK */
				return vci_get_inner_scan_plan_no(rp_context, inner);

			default:
				break;
		}
	}

	return 0;
}

/**
 * Determine whether the given oid is an operation supported by VCI
 */
static bool
vci_is_supported_operation(Oid oid)
{
	return oid < FirstNormalObjectId;
}

/**
 * Determine whether the given oid is not user defined type
 */
static bool
vci_is_not_user_defined_type(Oid oid)
{
	return oid < FirstNormalObjectId;
}

/*==========================================================================*/
/* Register map of Plan on SMC and Plan State on backend */
/*==========================================================================*/

/*==========================================================================*/
/* Implementation of PG function to check VCI execution */
/*==========================================================================*/

PG_FUNCTION_INFO_V1(vci_runs_in_query);
PG_FUNCTION_INFO_V1(vci_runs_in_plan);
PG_FUNCTION_INFO_V1(vci_always_return_true);

/**
 * PG function that returns whether query is being executed by VCI
 *
 * @param[in] PG_FUNCTION_ARGS  Pointer to data struct passed to PG function
 * @return true if VCI is runnning, false if not
 */
Datum
vci_runs_in_query(PG_FUNCTION_ARGS)
{
	return BoolGetDatum(vci_is_processing_custom_plan());
}

/**
 * PG function that returns whether the plan node containing this function call is VCI plan node
 *
 * @param[in] PG_FUNCTION_ARGS  Pointer to data struct passed to PG function
 * @return always false
 */
Datum
vci_runs_in_plan(PG_FUNCTION_ARGS)
{
	return BoolGetDatum(false);
}

/**
 * Function that always returns true
 *
 * @param[in] PG_FUNCTION_ARGS  Pointer to data struct passed to PG function
 * @return always true
 *
 * The vci_runs_in_plan function in the query is overridden by this function,
 * which always returns true if the plan rewrite determines that a VCI plan node is connected.
 */
Datum
vci_always_return_true(PG_FUNCTION_ARGS)
{
	return BoolGetDatum(true);
}

/*
 * This function is used to update the plan tree by removing
 * the gather plan from the tree and adjust the targetlist
 * in custom_vci_plan based on the partial_plan and gather_plan.
 */
static void
vci_update_plan_tree(PlannedStmt *plannedstmt)
{
	Plan	   *plan = NULL;
	List	   *newsubplans = NIL;

	father_gather_plans plans;

	memset(&plans, 0, sizeof(father_gather_plans));

	if (plannedstmt->planTree)
	{
		plan = plannedstmt->planTree;

		if (nodeTag(plan) == T_Gather || nodeTag(plan) == T_GatherMerge)
		{
			plannedstmt->planTree = plan->lefttree;
			plans.gather_plan = plan;

			/*
			 * The targetlist of the Gather/GatherMerge node and the
			 * underlying node should be the same (this is enforced in
			 * preanalyze_plan_tree_mutator()). However, the
			 * Gather/GatherMerge node may have additional information that
			 * needs to be retained (by the underlying node) once it is
			 * removed.
			 */
			vci_update_target_list(plannedstmt->planTree, plan);
		}
		plans.father_plan = plan;
		vci_plan_tree_walker(plan, vci_update_plan_walker, &plans);

	}

	if (plannedstmt->subplans)
	{
		ListCell   *l;

		foreach(l, plannedstmt->subplans)
		{
			Plan	   *subplan = (Plan *) lfirst(l);

			if (subplan == NULL)
				continue;

			plans.father_plan = subplan;
			if (nodeTag(subplan) == T_Gather || nodeTag(subplan) == T_GatherMerge)
			{
				plans.gather_plan = subplan;
				subplan = subplan->lefttree;
			}
			newsubplans = lappend(newsubplans, subplan);
			vci_plan_tree_walker(subplan, vci_update_plan_walker, &plans);
		}
		plannedstmt->subplans = newsubplans;
	}

}
static bool
vci_update_plan_walker(Plan *plan, void *plans)
{
	father_gather_plans *fg_plans = (father_gather_plans *) plans;
	father_gather_plans fg_plans_local;

	if (plan == NULL)
		return false;
	/* Go through the every plan here */
	if (nodeTag(plan) == T_Gather || nodeTag(plan) == T_GatherMerge)
	{
		if (fg_plans->father_plan->lefttree == plan)
		{
			fg_plans->father_plan->lefttree = plan->lefttree;
		}
		else if (fg_plans->father_plan->righttree == plan)
		{
			fg_plans->father_plan->righttree = plan->lefttree;
		}
		else
		{
			/*
			 * Not expected scenario, All other cases should already mark that
			 * VCI is not possible.
			 */
			elog(ERROR, "The plan must be either left or right child of parent.");
		}

		fg_plans->gather_plan = plan;
	}
	else if (nodeTag(plan) == T_CustomPlanMarkPos && fg_plans->gather_plan)
	{
		plan->plan_rows = fg_plans->gather_plan->plan_rows;
		plan->parallel_aware = 0;
		vci_update_target_list(plan, fg_plans->gather_plan);
	}

	fg_plans_local.gather_plan = fg_plans->gather_plan;
	fg_plans_local.father_plan = plan;

	return vci_plan_tree_walker(plan, vci_update_plan_walker, &fg_plans_local);

}

/*
 * If vci_scan is created based on partial scan, some fields will be updated
 * by the targetlist in gather_plan. This function is used to do this job.
 *
 */
static List *
vci_update_target_list(Plan *plan, Plan *gather_plan)
{
	ListCell   *cell1,
			   *cell2;

	forboth(cell1, plan->targetlist, cell2, gather_plan->targetlist)
	{
		TargetEntry *te1 = (TargetEntry *) lfirst(cell1);
		TargetEntry *te2 = (TargetEntry *) lfirst(cell2);

		te1->resname = te2->resname;
	}

	return plan->targetlist;
}
