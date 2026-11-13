/*-------------------------------------------------------------------------
 *
 * vci_fetch_column_store.c
 *	  Routine to fetch from column store
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/executor/vci_fetch_column_store.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"		/* for IsolationIsSerializable */
#include "access/xlog.h"		/* for RecoveryInProgress() */
#include "access/xlogrecovery.h"
#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "executor/executor.h"	/* for EXEC_FLAG_BACKWARD */
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "storage/ipc.h"		/* for before_shmem_exit() */
#include "storage/lwlock.h"
#include "tcop/pquery.h"		/* for ActivePortal */
#include "utils/cash.h"
#include "utils/date.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "vci.h"

#include "vci_executor.h"
#include "vci_fetch.h"
#include "vci_mem.h"
#include <stdint.h>

#if (!defined(WIN32))

/* C99 says to define __STDC_LIMIT_MACROS before including stdint.h,
 * if you want the limit (max/min) macros for int types.
 */
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS 1
#endif

#include <inttypes.h>

#else
typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;

/* Limits of integral types. */
#ifndef INT8_MIN
#define INT8_MIN               (-128)
#endif
#ifndef INT16_MIN
#define INT16_MIN              (-32767-1)
#endif
#ifndef INT32_MIN
#define INT32_MIN              (-2147483647-1)
#endif
#ifndef INT8_MAX
#define INT8_MAX               (127)
#endif
#ifndef INT16_MAX
#define INT16_MAX              (32767)
#endif
#ifndef INT32_MAX
#define INT32_MAX              (2147483647)
#endif
#ifndef UINT8_MAX
#define UINT8_MAX              (255U)
#endif
#ifndef UINT16_MAX
#define UINT16_MAX             (65535U)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX             (4294967295U)
#endif

#endif							/* ! C99 */
/**
 * Used to search for VCI Scan in the plan tree with search_vci_scan_walker().
 */
typedef struct
{
	List	   *scan_list;		/* Record discovered VciScan in a list */
} vci_search_vci_scan_context_t;

/**
 * Store data struct for each query
 */
vci_query_context_t *vci_query_context;

static void initialize_query_context(PlannedStmt *target, MemoryContext smccontext);
static bool search_vci_scan_walker(Plan *plan, void *context);
static void aggregate_attr_used(List *scan_list);
static void output_local_ros_size(vci_CSQueryContext query_context);
static void enter_standby_query(void);
static void exit_standby_query(void);
static void prepare_query_contexts(bool recoveryInProgress, bool estimatingLocalROSSize);
static bool estimate_and_check_localROS_size(void);
static void shutdown_standby_query(int code, Datum arg);
static bool create_all_queries_context_for_fetching_column_store(QueryDesc *queryDesc, int eflags);
static void create_attr_map(VciScanState *scanstate, VciScan *scan, int *num_attrs_p, AttrNumber **attrNumArray_p);
static void initialize_one_fetch_context_for_fetching_column_store(VciScanState *scanstate, vci_index_placeholder_t *index_ph);

static bool is_running_standby_query;
static bool shutdown_standby_query_registered;

/**
 * Initialize query context required for column store fetch
 *
 * Attempt to rewrite plan for each query, and if successful, initialize
 * resources necessary to execute custom plan.
 *
 * @param[in,out] queryDesc query description to be rewritten
 * @param[in]     eflags    Execution flag
 *
 * @note If plan rewrite is succesful, per-query SMC is constructed, and the
 *       rewritten plan is stored in queryDesc->plannedstmt.
 *       Also, vci_query_context_t will be generated in vci_query_context.
 */
void
vci_initialize_query_context(QueryDesc *queryDesc, int eflags)
{
	PlannedStmt *orig_stmt;
	PlannedStmt *target;
	MemoryContext tmpcontext;
	MemoryContext oldcontext;
	MemoryContext smccontext;

	/*
	 * When a previous query was failed, vci_query_context may be a dangling
	 * pointer. We'll only set vci_query_context to NULL but mustn't access
	 * the memory content pointed by vci_query_context.
	 */
	vci_query_context = NULL;

	/*
	 * In standalone mode or bootstrap mode, disable VCI execution.
	 */
	if (!IsPostmasterEnvironment)
		return;

	if (!VciGuc.enable)
		return;

	orig_stmt = queryDesc->plannedstmt;

	/*
	 * Custom plan is only for SELECT command. For other commands, plan tree
	 * rewrite won't be performed.
	 */
	if (orig_stmt->commandType != CMD_SELECT)
		return;

	/*
	 * Stop if isolation level is serializable
	 */
	if (IsolationIsSerializable())
		return;

	/*
	 * Stop if full_page_writes is off
	 */
	if (!fullPageWrites)
		return;

	/*
	 * Stop if WITH HOLD is specified in DECLARE command
	 */
	if (ActivePortal &&
		(ActivePortal->cursorOptions & (CURSOR_OPT_HOLD)))
		return;

	/*
	 * Stop if SCROLL is specified in DECLARE command or SCROLL/NO SCROLL is
	 * not specified but SCROLL effect is applied internally
	 */
	if (eflags & EXEC_FLAG_BACKWARD)
		return;

	/*
	 * Stop if plan cost estimate is less than threshold
	 */
	if ((orig_stmt->planTree == NULL) ||
		(orig_stmt->planTree->total_cost < (Cost) VciGuc.cost_threshold))
		return;

	elog(DEBUG1, "Call vci_initialize_query_context()");

	tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "VCI Temporary Rewrite Plan",
									   ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(tmpcontext);

	target = vci_generate_custom_plan(orig_stmt, eflags, queryDesc->snapshot);

	MemoryContextSwitchTo(oldcontext);

	if (!target)
		goto done;

	smccontext = AllocSetContextCreate(TopTransactionContext, "VCI Query",
									   ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(smccontext);

	/* To rewrite plan, memory is allocated in tmpcontext, but move it to SMC */
	target = copyObjectImpl(target);

	initialize_query_context(target, smccontext);

	if (create_all_queries_context_for_fetching_column_store(queryDesc, eflags))
	{
		/* Environment build is succesful, so rewrite the plan */
		queryDesc->plannedstmt = target;
	}
	else
	{
		/* Environment build failed */
		MemoryContextSwitchTo(oldcontext);
		vci_finalize_query_context();

		goto done;
	}

	vci_query_context->plannedstmt = target;

	vci_query_context->max_plan_info_entries = VCI_INIT_PLAN_INFO_ENTRIES;
	vci_query_context->plan_info_map = palloc0_array(vci_plan_info_t, VCI_INIT_PLAN_INFO_ENTRIES);

	vci_query_context->lock = VciShmemAddr->vci_query_context_lock;

	MemoryContextSwitchTo(oldcontext);

done:
	MemoryContextDelete(tmpcontext);
}

static void
initialize_query_context(PlannedStmt *target, MemoryContext smccontext)
{
	vci_search_vci_scan_context_t scontext;

	vci_query_context = palloc0_object(vci_query_context_t);

	vci_query_context->mcontext = smccontext;

	scontext.scan_list = NIL;
	vci_plannedstmt_tree_walker(target, search_vci_scan_walker, NULL, &scontext);
	aggregate_attr_used(scontext.scan_list);
	list_free(scontext.scan_list);
}

static bool
search_vci_scan_walker(Plan *plan, void *context)
{
	vci_search_vci_scan_context_t *scontext;

	scontext = (vci_search_vci_scan_context_t *) context;

	if (plan && (IsA(plan, CustomScan) || IsA(plan, CustomPlanMarkPos)))
	{
		uint32		plan_type = ((CustomScan *) plan)->flags & VCI_CUSTOMPLAN_MASK;

		if (plan_type == VCI_CUSTOMPLAN_SCAN)
		{
			VciScan    *scan = (VciScan *) plan;

			if (scan->scan_mode == VCI_SCAN_MODE_COLUMN_STORE)
			{
				scontext->scan_list = lappend(scontext->scan_list, plan);
				return false;
			}
		}
	}

	return vci_plan_tree_walker(plan, search_vci_scan_walker, context);
}

/**
 * If there are multiple VCI Scan in the same table, OR of the referenced attributes is taken.
 * This is required to pass to vci_CSCreateQueryContext().
 */
static void
aggregate_attr_used(List *scan_list)
{
	int			i;
	int			uniq_vci_indexes = 0;
	List	   *uniq_oid_list = NIL;
	ListCell   *outer,
			   *inner;

	/*
	 * Calculate number of unique VCI indexes referenced from query
	 */
	foreach(outer, scan_list)
	{
		bool		match = false;
		VciScan    *scan = (VciScan *) lfirst(outer);

		foreach(inner, uniq_oid_list)
		{
			if (scan->indexoid == lfirst_oid(inner))
			{
				match = true;
				break;
			}
		}

		if (match)
			continue;
		else
		{
			uniq_oid_list = lappend_oid(uniq_oid_list, scan->indexoid);
			uniq_vci_indexes++;
		}
	}

	elog(DEBUG1, "# of unique VCI indexes = %d", uniq_vci_indexes);

	/* uniq_vci_indexes can be 0 */

	vci_query_context->num_indexes = uniq_vci_indexes;
	vci_query_context->index_ph_table = palloc0_array(vci_index_placeholder_t, uniq_vci_indexes);

	i = 0;
	foreach(outer, uniq_oid_list)
		vci_query_context->index_ph_table[i++].indexoid = lfirst_oid(outer);

	list_free(uniq_oid_list);
	uniq_oid_list = NIL;

	for (i = 0; i < uniq_vci_indexes; i++)
	{
		vci_index_placeholder_t *index_ph;

		index_ph = &vci_query_context->index_ph_table[i];

		foreach(outer, scan_list)
		{
			VciScan    *scan = (VciScan *) lfirst(outer);

			if (scan->indexoid == index_ph->indexoid)
			{
				index_ph->attr_used = bms_add_members(index_ph->attr_used,
													  scan->attr_used);

				scan->index_ph_id = i + 1;
				scan->fetch_ph_id = ++index_ph->num_fetches;
			}
		}

		index_ph->fetch_ph_table = palloc0_array(vci_fetch_placeholder_t, index_ph->num_fetches);
	}
}

/**
 * Free query context for VCI execution
 *
 * Release if query context is secured.
 * Free if local SMC of backend process is secured.
 */
void
vci_free_query_context(void)
{
	if (vci_query_context)
	{
		MemoryContextDelete(vci_query_context->mcontext);

		vci_query_context = NULL;
	}
}

/**
 * Determine whether custom plan that performs column store fetch is being executed
 *
 * @retval true  Executing custom plan
 * @retval false Not executing custom plan (including interruptions)
 */
bool
vci_is_processing_custom_plan(void)
{
	if (vci_query_context == NULL)
		return false;

	return !vci_query_context->has_stopped;
}

/**
 * @description output the Data WOS size and Whiteout WOS size on log.
 */
static void
output_local_ros_size(vci_CSQueryContext query_context)
{
	elog(DEBUG1,
		 "A local ROS creation for VCI %d failed: Data WOS size = %ld, Whiteout WOS size = %ld",
		 query_context->main_relation_oid,
		 (long) query_context->num_data_wos_entries, (long) query_context->num_whiteout_wos_entries);
}

/**
 * Create data necessary for column store fetch.
 * Call before executor runs.
 */
static bool
create_all_queries_context_for_fetching_column_store(QueryDesc *queryDesc, int eflags)
{
	bool		result = true;
	bool		recoveryInProgress;

	/*
	 * In standby server query, ShareUpdateExclusiveLock lock cannot be
	 * performed during Local ROS creation. Instead, stop streaming
	 * replication WAL replay.
	 *
	 * Multiple queries simultaneously creating Local ROS are counted by
	 * num_standby_exec_queries. Restart WAL replay at the end of last query.
	 */
	recoveryInProgress = RecoveryInProgress();

	if (recoveryInProgress)
		enter_standby_query();

	prepare_query_contexts(recoveryInProgress, true);
	if (!estimate_and_check_localROS_size())
		goto error;

	for (int i = 0; i < vci_query_context->num_indexes; i++)
	{
		vci_index_placeholder_t *index_ph;

		index_ph = &vci_query_context->index_ph_table[i];

		vci_CSDestroyQueryContext(index_ph->query_context);
		index_ph->query_context = NULL;
	}
	prepare_query_contexts(recoveryInProgress, false);
	if (!estimate_and_check_localROS_size())
		goto error;

	/*
	 * Create Local ROS
	 */
	PG_TRY();
	{
		for (int i = 0; i < vci_query_context->num_indexes; i++)
		{
			vci_index_placeholder_t *index_ph;

			index_ph = &vci_query_context->index_ph_table[i];

			index_ph->local_ros = vci_CSGenerateLocalRos(index_ph->query_context);

			Assert(index_ph->local_ros);
		}
	}
	PG_CATCH();
	{
		if (geterrcode() == ERRCODE_OUT_OF_MEMORY)
		{
			/*
			 * Cancel VCI execution if there is an error due to insufficient
			 * memory during Local ROS generation.
			 */
			if (VciGuc.log_query)
				elog(WARNING, "out of memory during local ROS generation");

			for (int i = 0; i < vci_query_context->num_indexes; i++)
			{
				vci_index_placeholder_t *index_ph;
				vci_id_t	vciid;

				index_ph = &vci_query_context->index_ph_table[i];

				vciid.oid = index_ph->indexoid;
				vciid.dbid = MyDatabaseId;

				vci_SetForceNextWosRosConvFlag(&vciid, true);

				if (index_ph->query_context)
				{
					output_local_ros_size(index_ph->query_context);
					vci_CSDestroyQueryContext(index_ph->query_context);
					index_ph->query_context = NULL;
				}
			}

			FlushErrorState();

			result = false;
		}
		else
		{
			if (recoveryInProgress)
				exit_standby_query();

			PG_RE_THROW();
		}
	}
	PG_END_TRY();

	if (recoveryInProgress)
		exit_standby_query();

	return result;

error:
	for (int i = 0; i < vci_query_context->num_indexes; i++)
	{
		vci_index_placeholder_t *index_ph;
		vci_id_t	vciid;

		index_ph = &vci_query_context->index_ph_table[i];

		vciid.oid = index_ph->indexoid;
		vciid.dbid = MyDatabaseId;

		vci_SetForceNextWosRosConvFlag(&vciid, true);
		output_local_ros_size(index_ph->query_context);

		vci_CSDestroyQueryContext(index_ph->query_context);
		index_ph->query_context = NULL;
	}

	if (recoveryInProgress)
		exit_standby_query();

	return false;
}

static void
enter_standby_query(void)
{
	LWLockAcquire(VciShmemAddr->standby_exec_loc, LW_EXCLUSIVE);
	if (VciShmemAddr->num_standby_exec_queries == 0)
		SetVciRecoveryPause();
	VciShmemAddr->num_standby_exec_queries++;
	LWLockRelease(VciShmemAddr->standby_exec_loc);

	if (!shutdown_standby_query_registered)
	{
		before_shmem_exit(shutdown_standby_query, 0);
		shutdown_standby_query_registered = true;
	}

	is_running_standby_query = true;
}

static void
exit_standby_query(void)
{
	is_running_standby_query = false;

	LWLockAcquire(VciShmemAddr->standby_exec_loc, LW_EXCLUSIVE);
	VciShmemAddr->num_standby_exec_queries--;
	if (VciShmemAddr->num_standby_exec_queries == 0)
		SetRecoveryPause(false);
	LWLockRelease(VciShmemAddr->standby_exec_loc);
}

static void
shutdown_standby_query(int code, Datum arg)
{
	if (!is_running_standby_query)
		return;

	is_running_standby_query = false;

	LWLockAcquire(VciShmemAddr->standby_exec_loc, LW_EXCLUSIVE);
	VciShmemAddr->num_standby_exec_queries--;
	if (VciShmemAddr->num_standby_exec_queries == 0)
		SetRecoveryPause(false);
	LWLockRelease(VciShmemAddr->standby_exec_loc);
}

/**
 * @description allocate query_contexts for VCIs.
 * @param[in] recoveryInProgress true if recovery is in progress.
 * @param[in] estimatingLocalROSSize true if estimating a local ROS size.
 */
static void
prepare_query_contexts(bool recoveryInProgress, bool estimatingLocalROSSize)
{
	for (int i = 0; i < vci_query_context->num_indexes; i++)
	{
		int			j,
					k,
					num_attrs;
		AttrNumber *attrNumArray;
		vci_index_placeholder_t *index_ph;
		vci_id_t	vciid;

		index_ph = &vci_query_context->index_ph_table[i];

		num_attrs = bms_num_members(index_ph->attr_used);

		attrNumArray = palloc_array(AttrNumber, num_attrs);

		j = k = 0;
		do
		{
			if (bms_is_member(k, index_ph->attr_used))
				attrNumArray[j++] = k;

			k++;
		} while (j < num_attrs);

		/* update memory entry */
		vciid.oid = index_ph->indexoid;
		vciid.dbid = MyDatabaseId;

		vci_TouchMemoryEntry(&vciid,
							 get_rel_tablespace(index_ph->indexoid));

		index_ph->query_context = vci_CSCreateQueryContext(
														   index_ph->indexoid,
														   num_attrs,	/* Numbe of read columns */
														   attrNumArray,	/* Array of read columns */
														   vci_query_context->mcontext, /* SMC */
														   recoveryInProgress,
														   estimatingLocalROSSize);

		Assert(index_ph->query_context);

		pfree(attrNumArray);
	}
}

/**
 * @description estimate the size of local ROS
 * @return true if estimated local ROS size is smaller than upperbounds.
 */
static bool
estimate_and_check_localROS_size()
{
	Size		total_local_ros_size = 0;

	/*
	 * Estimate Local ROS size
	 */
	for (int i = 0; i < vci_query_context->num_indexes; i++)
	{
		Size		local_ros_size;

		local_ros_size = vci_CSEstimateLocalRosSize(vci_query_context->index_ph_table[i].query_context);

		if (local_ros_size == (Size) -1)
		{
			if (VciGuc.log_query)
				elog(WARNING, "too many rows in Data WOS");

			return false;
		}

		total_local_ros_size += local_ros_size;
	}

	if (VciGuc.max_local_ros_size * UINT64CONST(1024) < total_local_ros_size)
	{
		if (VciGuc.log_query)
			elog(WARNING, "could not use VCI: local ROS size (%zu) exceeds vci.max_local_ros (%zu)",
				 total_local_ros_size, (Size) VciGuc.max_local_ros_size * UINT64CONST(1024));

		return false;
	}

	return true;
}

/**
 * Finalize query context requited for column store fetch
 *
 * @note Object pointed to by vci_query_context is collected.
 */
void
vci_finalize_query_context(void)
{
	Assert(vci_query_context);

	elog(DEBUG1, "Call vci_finalize_query_context()");

	for (int i = vci_query_context->num_indexes - 1; i >= 0; i--)
	{
		if (vci_query_context->index_ph_table[i].local_ros)
		{
			vci_CSDestroyLocalRos(vci_query_context->index_ph_table[i].local_ros);
			vci_query_context->index_ph_table[i].local_ros = NULL;
		}

		if (vci_query_context->index_ph_table[i].query_context)
		{
			vci_CSDestroyQueryContext(vci_query_context->index_ph_table[i].query_context);
			vci_query_context->index_ph_table[i].query_context = NULL;
		}
	}
	vci_free_query_context();
	vci_query_context = NULL;
}

/**
 * VCI Scan assigned serial number only to attributes read from the table and creates map.
 *
 * @param[out] scanstate      VCI Scan state to be output
 * @param[in]  scan           Original VCI Scan
 * @param[out] num_attrs_p    Number of attributes to read
 * @param[out] attrNumArray_p Map of original attribute number in table -> serial numbers for attributes to be read
 *
 * @todo The order of function arguments are unnatural
 */
static void
create_attr_map(VciScanState *scanstate, VciScan *scan, int *num_attrs_p, AttrNumber **attrNumArray_p)
{
	int			top_attr,
				attr_index,
				num_attrs;
	AttrNumber *attrNumArray;

	num_attrs = bms_num_members(scan->attr_used);

	attrNumArray = palloc_array(AttrNumber, num_attrs);

	top_attr = 1;				/* AttrNumber starts from 1 */
	attr_index = 0;

	do
	{
		if (bms_is_member(top_attr, scan->attr_used))
			attrNumArray[attr_index++] = top_attr;

		top_attr++;
	} while (attr_index < num_attrs);

	/* Record the biggest AttrNumber */
	scanstate->last_attr = top_attr - 1;

	/*
	 * Create a map of column number returned by column store fetch from
	 * AttrNumber so that searched can be performed from Var.
	 */
	scanstate->attr_map = palloc0_array(int, (scanstate->last_attr + 1));

	for (int i = 0; i < num_attrs; i++)
		/* Add 1 to the index number so that 0 indicates an invalid value */
		scanstate->attr_map[attrNumArray[i]] = i + 1;

	*num_attrs_p = num_attrs;
	*attrNumArray_p = attrNumArray;
}

/**
 * Create data required for a specific VCI Scan to perform column store fetch.
 *
 * @param[in,out] scanstate Pointer to VCI Scan
 * @param[in,out] econtext  expression context needed for execution
 *
 * @note Call only once in ExecInit for VCI Scan.
 */
void
vci_create_one_fetch_context_for_fetching_column_store(VciScanState *scanstate, ExprContext *econtext)
{
	VciScan    *scan = (VciScan *) scanstate->vci.css.ss.ps.plan;
	vci_index_placeholder_t *index_ph;
	vci_fetch_placeholder_t *fetch_ph;
	int			num_attrs;
	AttrNumber *attrNumArray;

	elog(DEBUG1, "Call vci_create_one_fetch_context_for_fetching_column_store()");

	create_attr_map(scanstate, scan, &num_attrs, &attrNumArray);

	Assert((1 <= scan->index_ph_id) && (scan->index_ph_id <= vci_query_context->num_indexes));

	index_ph = &vci_query_context->index_ph_table[scan->index_ph_id - 1];

	scanstate->fetch_context
		= vci_CSCreateFetchContext(index_ph->query_context,
								   VCI_NUM_ROWS_READ_AT_ONCE,
								   num_attrs,
								   attrNumArray,
								   true,	/* column store */
								   false,	/* Do not return TID vector */
								   true /* Returns CRID */ );

	Assert(scanstate->fetch_context);

	pfree(attrNumArray);

	initialize_one_fetch_context_for_fetching_column_store(scanstate, index_ph);

	/* Record status in shared memory area */
	Assert((1 <= scan->fetch_ph_id) && (scan->fetch_ph_id <= index_ph->num_fetches));

	fetch_ph = &index_ph->fetch_ph_table[scan->fetch_ph_id - 1];

	fetch_ph->fetch_context = scanstate->fetch_context;
	fetch_ph->scanstate = scanstate;
}

/**
 * Parallel background worker copies data necessary for VCI Scan to perform
 * column store fetch.
 *
 * @param[in,out] scanstate Pointer to VCI Scan
 *
 * @note This is for parallel background worker
 */
void
vci_clone_one_fetch_context_for_fetching_column_store(VciScanState *scanstate)
{
	VciScan    *scan = (VciScan *) scanstate->vci.css.ss.ps.plan;
	vci_index_placeholder_t *index_ph;
	int			num_attrs;
	AttrNumber *attrNumArray;

	Assert((1 <= scan->index_ph_id) && (scan->index_ph_id <= vci_query_context->num_indexes));

	index_ph = &vci_query_context->index_ph_table[scan->index_ph_id - 1];

	/* Copy fecth context created on backend */
	scanstate->fetch_context = index_ph->fetch_ph_table[scan->fetch_ph_id - 1].fetch_context;

	create_attr_map(scanstate, scan, &num_attrs, &attrNumArray);

	pfree(attrNumArray);

	initialize_one_fetch_context_for_fetching_column_store(scanstate, index_ph);

	/*
	 * first_extent_id, last_extent_id, first_fetch of scanstate of VCI Scan
	 * to be scanned in parallel are reset when the task is received.
	 */
}

static void
initialize_one_fetch_context_for_fetching_column_store(VciScanState *scanstate, vci_index_placeholder_t *index_ph)
{
	scanstate->local_fetch_context
		= vci_CSLocalizeFetchContext(scanstate->fetch_context,
									 CurrentMemoryContext);

	scanstate->extent_status
		= vci_CSCreateCheckExtent(scanstate->local_fetch_context);

	Assert(scanstate->extent_status);

	scanstate->vector_set
		= vci_CSCreateVirtualTuples(scanstate->local_fetch_context);

	Assert(scanstate->vector_set);

	/* Start scanning from the negative extent id if Local ROS exists */
	scanstate->first_extent_id = -index_ph->query_context->num_local_ros_extents;
	scanstate->last_extent_id = index_ph->query_context->num_ros_extents;
	scanstate->first_crid = (int64) scanstate->first_extent_id * VCI_NUM_ROWS_IN_EXTENT;
	scanstate->last_crid = (int64) scanstate->last_extent_id * VCI_NUM_ROWS_IN_EXTENT;
	scanstate->first_fetch = false;
}

/**
 * Destroy data required for specific VCI Scan to execute column store fetch.
 *
 * @param[in,out] scanstate Pointer to VCI Scan
 *
 * @note Call only once in ExecEnd for VCI Scan
 */
void
vci_destroy_one_fetch_context_for_fetching_column_store(VciScanState *scanstate)
{
	elog(DEBUG1, "Call vci_destroy_one_fetch_context_for_fetching_column_store()");

	pfree(scanstate->attr_map);
	scanstate->attr_map = NULL;

	vci_CSDestroyVirtualTuples(scanstate->vector_set);
	scanstate->vector_set = NULL;

	vci_CSDestroyCheckExtent(scanstate->extent_status);
	scanstate->extent_status = NULL;

	vci_CSDestroyFetchContext(scanstate->local_fetch_context);
	scanstate->local_fetch_context = NULL;

	vci_CSDestroyFetchContext(scanstate->fetch_context);
	scanstate->fetch_context = NULL;
}

/**
 * Specify column store read start position to VCI Scan
 *
 * @param[in, out] scanstate   Pointer to VCI Scan
 * @param[in]      crid_statrt Read start CRID
 * @param[in]      size        Number of rows to read at a time
 *                             (VCI_NUM_ROWS_IN_EXTENT or less)
 */
void
vci_set_starting_position_for_fetching_column_store(VciScanState *scanstate, int64 crid_start, int size)
{
	int64		crid_end = crid_start + size;
	int32		extent_id;

	/*
	 * Dividing by VCI_NUM_ROWS_IN_EXTENT doesn't work when crid_start is
	 * negative, so bit shift.
	 */
	extent_id = crid_start >> VCI_CRID_ROW_ID_BIT_WIDTH;

	Assert(crid_end <= (int64) (extent_id + 1) * VCI_NUM_ROWS_IN_EXTENT);

	scanstate->first_extent_id = extent_id;
	scanstate->last_extent_id = extent_id + 1;
	scanstate->first_crid = crid_start;
	scanstate->last_crid = crid_end;

	scanstate->first_fetch = false;
}

/**
 * Read vector from column store fetches in VCI Scan.
 * If there are unread lines in the vector, do nothing.
 *
 * @param[in, out] scanstate   Pointer to VCI Scan
 *
 * @retval false Read all rows in column store
 * @retval true  One or more lines remain to be read
 *
 * @note Before calling this function, initialize settings
 *       such as vci_reset_vector_set_from_column_store() and
 *       vci_set_starting_position_for_fetching_column_store().
 */
bool
vci_fill_vector_set_from_column_store(VciScanState *scanstate)
{
	if (!scanstate->first_fetch)
	{
		int64		crid_start;
		int64		crid_end;
		int64		vector_end;
		vci_extent_status_t *status;
		vci_virtual_tuples_t *vector_set;
		uint16	   *skip_list;

		scanstate->first_fetch = true;

		scanstate->pos.current_extent_id = scanstate->first_extent_id;

		crid_start = scanstate->first_crid;
		crid_end = scanstate->last_crid;

		/* Check first extent */
		status = scanstate->extent_status;

		vci_CSCheckExtent(status,
						  scanstate->local_fetch_context,
						  scanstate->pos.current_extent_id,
						  false);

		if (!status->existence || !status->visible)
			goto start;

		crid_end = Min(crid_end, crid_start + status->num_rows);

		/* Read first vector */
		vector_end = (crid_start + VCI_MAX_FETCHING_ROWS) & ~(VCI_MAX_FETCHING_ROWS - 1);

		if (crid_end < vector_end)
			vector_end = crid_end;

		vector_set = scanstate->vector_set;

		scanstate->pos.fetch_starting_crid = crid_start;
		scanstate->pos.num_fetched_rows =
			vci_CSFetchVirtualTuples(vector_set, crid_start, vector_end - crid_start);

		if (scanstate->pos.num_fetched_rows < 1)
			elog(ERROR, "vci_CSFetchVirtualTuples returns %d num_fetched_rows(crid=" INT64_FORMAT ")",
				 scanstate->pos.num_fetched_rows, crid_start);

		scanstate->pos.offset_in_extent = (crid_start & (VCI_NUM_ROWS_IN_EXTENT - 1)) + scanstate->pos.num_fetched_rows;
		scanstate->pos.num_rows_in_extent = ((crid_end - 1) & (VCI_NUM_ROWS_IN_EXTENT - 1)) + 1;

		skip_list = vci_CSGetSkipFromVirtualTuples(vector_set);
		scanstate->pos.current_row = skip_list[0];
	}

start:
	CHECK_FOR_INTERRUPTS();

	if (scanstate->pos.current_row < scanstate->pos.num_fetched_rows)
		/* Can read fetched vectors */
		return true;

	if (scanstate->pos.offset_in_extent < scanstate->pos.num_rows_in_extent)
	{
		/* Read the next vector in the same extent */
		vci_virtual_tuples_t *vector_set;
		int64		crid_start;
		uint16	   *skip_list;

		vector_set = scanstate->vector_set;

		crid_start = (int64) scanstate->pos.current_extent_id * VCI_NUM_ROWS_IN_EXTENT
			+ scanstate->pos.offset_in_extent;

		scanstate->pos.fetch_starting_crid = crid_start;
		scanstate->pos.num_fetched_rows =
			vci_CSFetchVirtualTuples(vector_set, crid_start, VCI_MAX_FETCHING_ROWS);

		if (scanstate->pos.num_fetched_rows < 1)
			elog(ERROR, "vci_CSFetchVirtualTuples returns %d num_fetched_rows(crid=" INT64_FORMAT ")",
				 scanstate->pos.num_fetched_rows, crid_start);

		Assert(vector_set->num_rows > 0);

		scanstate->pos.offset_in_extent += VCI_MAX_FETCHING_ROWS;

		skip_list = vci_CSGetSkipFromVirtualTuples(vector_set);
		scanstate->pos.current_row = skip_list[0];

		goto start;
	}

	/* read next extent */
	while (scanstate->pos.current_extent_id + 1 < scanstate->last_extent_id)
	{
		vci_extent_status_t *status = scanstate->extent_status;
		int64		extent_start;
		int64		extent_end;

		scanstate->pos.current_extent_id++;

		vci_CSCheckExtent(status,
						  scanstate->local_fetch_context,
						  scanstate->pos.current_extent_id,
						  false);

		if (status->existence && status->visible)
		{
			extent_start = (int64) scanstate->pos.current_extent_id * VCI_NUM_ROWS_IN_EXTENT;
			extent_end = Min(extent_start + status->num_rows, scanstate->last_crid);

			scanstate->pos.offset_in_extent = 0;
			scanstate->pos.num_rows_in_extent = extent_end - extent_start;

			goto start;
		}
	}

	/* Finished read all extent */
	return false;
}

/**
 * Temporarily record the read position of VCI Scan column store.
 *
 * @param[in, out] scanstate   Pointer to VCI Scan
 */
void
vci_mark_pos_vector_set_from_column_store(VciScanState *scanstate)
{
	scanstate->mark = scanstate->pos;
}

/**
 * Return read position of VCI Scan column store to the marked position,
 * and read data again.
 *
 * @param[in, out] scanstate   Pointer to VCI Scan
 */
void
vci_restr_pos_vector_set_from_column_store(VciScanState *scanstate)
{
	/* read next vector in the same extent */
	vci_virtual_tuples_t *vector_set;
	int64		crid_start;

	/* return to marked position */
	scanstate->pos = scanstate->mark;

	/* Re-read extent */
	vector_set = scanstate->vector_set;

	crid_start = (int64) scanstate->pos.current_extent_id * VCI_NUM_ROWS_IN_EXTENT
		+ (scanstate->pos.offset_in_extent - VCI_MAX_FETCHING_ROWS);

	scanstate->pos.fetch_starting_crid = crid_start;
	scanstate->pos.num_fetched_rows =
		vci_CSFetchVirtualTuples(vector_set, crid_start, VCI_MAX_FETCHING_ROWS);

	Assert(vector_set->num_rows > 0);
}

/**
 * When reading 1 row of vector loaded by vci_fill_vector_set_from_column_store(),
 * set the row to be read next to pointer.
 *
 * @param[in, out] scanstate   Pointer to VCI Scan
 */
void
vci_step_next_tuple_from_column_store(VciScanState *scanstate)
{
	vci_virtual_tuples_t *vector_set;
	uint16	   *skip_list;

	vector_set = scanstate->vector_set;
	skip_list = vci_CSGetSkipFromVirtualTuples(vector_set);

	scanstate->pos.current_row += skip_list[scanstate->pos.current_row + 1] + 1;
}

/**
 * Set lines of loaded vector to read
 *
 * @param[in, out] scanstate   Pointer to VCI Scan
 */
void
vci_finish_vector_set_from_column_store(VciScanState *scanstate)
{
	scanstate->pos.current_row = scanstate->pos.num_fetched_rows;
}

/**
 * Execute vector process corresponding to target list of VCI Scan
 *
 * @param[in,out] scanstate Pointer to VCI Scan
 * @param[in,out] econtext  expression context required for execution
 * @param[in]     max_slots max length of this vector
 */
void
VciExecTargetListWithVectorProcessing(VciScanState *scanstate, ExprContext *econtext, int max_slots)
{
	for (int i = 0; i < scanstate->num_vp_targets; i++)
		VciExecEvalVectorProcessing(scanstate->vp_targets[i], econtext, max_slots);
}

/**
 * Evaluation function for Var when performing column store fetch
 *
 * @param[in,out] exprstate expression state tree of Var (VciVarState type)
 * @param[in,out] econtext  expression context required for execution
 * @param[out]    isNull    Return NULL/NOT NULL information of evaluation result of Var
 * @param[out]    isDone    Return state when multiple lines are retruned. Always ExprSingleResult in VciParamState.
 *
 * @return Return evaluation result data of Var
 *
 * @note Called from VciExecInitExpr()
 */
void
VciExecEvalScalarVarFromColumnStore(ExprState *exprstate, ExprEvalStep *op, ExprContext *econtext)
{
	vci_virtual_tuples_column_info_t *data_vector;
	int			index;
	int			null_bit_id;

	PlanState  *parent;
	VciScanState *scanstate;
	int			attnum;

	attnum = op->d.var.attnum;
	parent = op->d.var.vci_parent_planstate;
	scanstate = vci_search_scan_state((VciPlanState *) parent);

	/* The actual index number is the value minus 1. 0 is invalid. */
	index = scanstate->attr_map[attnum] - 1;

	Assert(index >= 0);
	Assert(index < scanstate->vector_set->num_columns);

	data_vector = &scanstate->vector_set->column_info[index];

	null_bit_id = data_vector->null_bit_id;

	if (null_bit_id >= 0)
		*op->resnull = vci_CSGetIsNullOfVirtualTupleColumnar(scanstate->vector_set, index)[scanstate->pos.current_row];

	*op->resvalue = vci_CSGetValuesOfVirtualTupleColumnar(scanstate->vector_set, index)[scanstate->pos.current_row];

}
