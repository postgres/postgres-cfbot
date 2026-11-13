/*-------------------------------------------------------------------------
 * vci_executor.h
 *	  Definitions and declarations about executor modules
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_executor.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_EXECUTOR_H
#define VCI_EXECUTOR_H

#include "postgres.h"

#include "access/htup.h"
#include "access/tupdesc.h"
#include "executor/execdesc.h"
#include "executor/execExpr.h"
#include "executor/instrument.h"
#include "nodes/bitmapset.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/pathnodes.h"
#include "storage/buffile.h"

#include "vci_fetch.h"

struct VciAgg;
struct VciAggState;

/*
 * MemoryContext size used during query execution
 */
#define VCI_ALLOCSET_DEFAULT_MINSIZE	(0)
#define VCI_ALLOCSET_DEFAULT_INITSIZE	(  8 * 1024 * 1024)
#define VCI_ALLOCSET_DEFAULT_MAXSIZE	(512 * 1024 * 1024)

/**
 * Maximum number of fetch rows specified in vci_CSCreateFetchContext()
 */
#define VCI_NUM_ROWS_READ_AT_ONCE		(32 * 1024)

/**
 * Maximum number of rows to fetch at one time specified in vci_CSGetSkipFromVirtualTuples()
 */
#define VCI_MAX_FETCHING_ROWS			(128)

/**
 * Number of slot to allocate for Skip List
 */
#define VCI_MAX_SKIP_LIST_SLOTS			(VCI_MAX_FETCHING_ROWS + 1)

/**
 * Initial number of element in plan_info_map[]
 */
#define VCI_INIT_PLAN_INFO_ENTRIES		(16)

struct ExplainState;
struct VciScanState;
struct VciVPContext;
struct VciVPNode;
struct VciScalarArrayOpExprHashTable;

/**
 * Column store fetch management information per VCI Scan
 *
 * @note This struct is instantiated on SMC
 */
typedef struct
{
	/**
	 * The fetch context created by vci_CSCreateFetchContext() in the backend
	 * is recorded in the fetch_context member of VciScanState as the master.
	 * However, it is also recorded in this member variable so that it can be referenced from parallel workers.
	 */
	vci_CSFetchContext fetch_context;

	/**
	 * Pointer to VCI Scan State for reading VCI index (referenced only when abort)
	 * Used only on backend side. Reading it on the parallel worker side results in dangling pointer
	 */
	struct VciScanState *scanstate;

} vci_fetch_placeholder_t;

/**
 * Column store fetch management information for each VCI index
 *
 *
 * @note This struct instance is taken on SMC.
 */
typedef struct
{
	Oid			indexoid;		/* OID of VCI index */
	Bitmapset  *attr_used;		/* Bitmap indicating the column position
								 * referenced in VCI index */
	int			num_fetches;	/* Number of VCI Scan that refer to VCI index
								 * of indexoid */

	vci_CSQueryContext query_context;	/* Column Store Query Context */
	vci_local_ros_t *volatile local_ros;	/* Pointer to Local ROS */

	vci_fetch_placeholder_t *fetch_ph_table;	/* Pointer to
												 * vci_fetch_placeholder_t
												 * array struct. Number of
												 * element in the array is
												 * num_fetches */

} vci_index_placeholder_t;

/**
 * Data struct that records the correspondence between Plan State and Plan in query
 *
 * - Plan is on SMC and is common between backend processes and parallel workers.
 * - Plan State refers to data in the local memory of the backend process.
 *
 * @note This struct instance is taken on SMC.
 */
typedef struct
{
	Plan	   *plan;			/* plan (on SMC) */
	PlanState  *planstate;		/* PlanState on backend side */
	Instrumentation instrument; /* Instrumentation for aggregating
								 * Instrumentation of parallel workers during
								 * parallel execution */
} vci_plan_info_t;

/**
 * Column store fetch management information for each query
 */
typedef struct vci_query_context
{
	/**
	 * Memory context for allocatin gmemory related to Column Store Fetch
	 *
	 * - Expect it to be SMC
	 * - vci_query_context also be instatiated in mcontext
	 */
	MemoryContext mcontext;

	/**
	 * Used to use contention when writing data in the vci_query_context
	 * struct from a parallel worker.
	 */
	LWLock	   *lock;

	/**
	 * Number of VCI index referenced in query
	 */
	int			num_indexes;

	/**
	 * Array into placeholder for VCI index referenced in this query
	 * The size is num_indexes.
	 */
	vci_index_placeholder_t *index_ph_table;

	/**
	 * If stops in the middle of custom plan execution
	 */
	bool		has_stopped;

	/**
	 * planned stmt rewritten into VCI plan
	 */
	PlannedStmt *plannedstmt;

	/**
	 * Original planned stmt before rewrite.
	 * Used when custom plan execution is canceled.
	 */
	PlannedStmt *origplannedstmt;

	/**
	 * Maximum number of elements allocated for plan_info_map[]
	 */
	int			max_plan_info_entries;

	/**
	 * Array containing all Plan and PlanState pairs on backend side
	 * Accessed by plan_info_map[plan->plan_no - 1]
	 *
	 * Used to find PlanState corresponding to plan in
	 * vci_exec_set_param_plan_as_proxy().
	 */
	vci_plan_info_t *plan_info_map;

} vci_query_context_t;

/**
 * Pointer to column store fetch management object for each query
 */
extern vci_query_context_t *vci_query_context;

/* ----------------
 *   Vector processing
 * ----------------
 */

/**
 * ExprState number in VciVPContext
 */
typedef unsigned int vci_vp_item_id;

/**
 * Templete of function pointer for Vector Processing
 */
typedef void (*VciVPExecOp_func) (Expr *expression, struct VciVPNode *vpnode, struct VciVPContext *vpcontext, ExprContext *econtext, int max_slots);

/**
 * Vector Processing's node
 *
 * Converted from Expression state node.
 */
typedef struct VciVPNode
{
	VciVPExecOp_func evalfunc;	/* Function to process this VP node */
	Expr	   *expr;
	int			len_args;		/* Max number of elements in arg_items[] */
	vci_vp_item_id *arg_items;	/* Item number of the child VP node of this VP
								 * node */

	Datum	   *itemValue;		/* Datum array that records this VP node
								 * process result. Number of element is
								 * allocated VCI_MAX_FETCHING_ROWS. */
	bool	   *itemIsNull;		/* bool array that records this VP node
								 * process result. Number of element is
								 * allocated VCI_MAX_FETCHING_ROWS. */
	uint16	   *skip_list;		/* Skip list usued during this VP node process */

	/** Auxiliary information for some VP node types*/
	union
	{
		/** Original skip list configured on the control VP node */
		struct
		{
			uint16	   *orig_skip_list;
		}			init;

		/** Used as storage location for intermediate data during processing of VP nodes based on T_CoerceToDomain */
		struct
		{
			Oid			resulttype;
			char	   *name;
		}			coerce_to_domain;

		struct
		{
			int			paramid;	/* numeric ID for parameter */
			Oid			paramtype;	/* OID of parameter's datatype */
			Plan	   *vci_parent_plan;
		}			param;

		struct
		{
			FmgrInfo   *finfo;	/* function's lookup data */
			FunctionCallInfo fcinfo_data;	/* arguments etc */
			/* faster to access without additional indirection: */
			PGFunction	fn_addr;	/* actual call address */
			int			nargs;	/* number of arguments */
		}			func;

		struct
		{
			/* element_type/typlen/typbyval/typalign are filled at runtime */
			Oid			element_type;	/* InvalidOid if not yet filled */
			bool		useOr;	/* use OR or AND semantics? */
			int16		typlen; /* array element type storage info */
			bool		typbyval;
			char		typalign;
			FmgrInfo   *finfo;	/* function's lookup data */
			FunctionCallInfo fcinfo_data;	/* arguments etc */
			/* faster to access without additional indirection: */
			PGFunction	fn_addr;	/* actual call address */
		}			scalararrayop;

		struct
		{
			bool		has_nulls;
			struct VciScalarArrayOpExprHashTable *elements_tab;
			FmgrInfo   *finfo;	/* function's lookup data */
			FunctionCallInfo fcinfo_data;	/* arguments etc */
			/* faster to access without additional indirection: */
			PGFunction	fn_addr;	/* actual call address */
			FmgrInfo   *hash_finfo; /* function's lookup data */
			FunctionCallInfo hash_fcinfo_data;	/* arguments etc */
			/* faster to access without additional indirection: */
			PGFunction	hash_fn_addr;	/* actual call address */
		}			hashedscalararrayop;

		struct
		{
			/* lookup and call info for source type's output function */
			FmgrInfo   *finfo_out;
			/* lookup and call info for result type's input function */
			FmgrInfo   *finfo_in;
			Oid			typioparam;

			/*
			 * Below ones used in OSS are not required for VCI as these
			 * information will be filled by InitFunctionCallInfoData in eval
			 * execute function itself FunctionCallInfo fcinfo_data_out;
			 * FunctionCallInfo fcinfo_data_in;
			 */
		}			iocoerce;

	}			data;
} VciVPNode;

/**
 * Vector processing context
 *
 * Converted from Expression tree.
 */
typedef struct VciVPContext
{
	vci_vp_item_id num_item;	/* Currently assigned maximum item number */
	vci_vp_item_id max_item;	/* Maximum number of nodes reserved by VP
								 * context */
	VciVPNode  *itemNode;		/* Array of VP node */

	Datum	   *resultValue;	/* Array of Datum that is the final result
								 * when VP context is processed */
	bool	   *resultIsNull;	/* Array of bool that is the final result when
								 * VP context is processed */

	vci_vp_item_id caseValue;	/* Temporarily records caseValue during
								 * execution of VciExecEvalVectorProcessing() */
	vci_vp_item_id domainValue; /* Temporarily records domainValue during
								 * execution of VciExecEvalVectorProcessing() */

} VciVPContext;

extern void VciExecEvalVectorProcessing(VciVPContext *vpcontext, ExprContext *econtext, int max_slots);
extern VciVPContext *VciBuildVectorProcessing(Expr *node, PlanState *parent, ExprContext *econtext, uint16 *skip_list);

/* ----------------
 *   Projection information for VCI
 * ----------------
 */

/**
 * Data struct that records how each target in the target list was processed in VciProjectionInfo
 */
typedef struct VciProjectionInfoSlot
{
	bool		is_simple_var;

	union
	{
		/* Record here if is_simple_var is true */
		struct
		{
			Index		relid;	/* Copy varno value of Var */
			AttrNumber	attno;	/* Copy varattno value of Var */
		}			simple_var;

		/* Record here if is_simple_var is false */
		struct
		{
			int			expr_id;	/* Converted to pi_vp_tle_array[expr_id]
									 * in VciProjectionInfo */
		}			expr;
	}			data;
} VciProjectionInfoSlot;

/**
 * ProjectionInfo for VCI
 *
 * The exprlist in ProjectionInfo is an array of VciVPContext pointers for vector processing.
 *
 * @note The ProjectionInfo type in PostgreSQL and the VciProjectionInfo type in VCI are almost identical,
 *       but the former loses information about which position in the original target list the simple_var and pi_targetlist were
 *       created from, while the latter manages this information using pi_slotMap.
 */
typedef struct VciProjectionInfo
{
	/* instructions to evaluate projection */
	ExprState	pi_state;
	TargetEntry **pi_tle_array; /* Array of expression state tree under
								 * TargetEntry that was converted */
	VciVPContext **pi_vp_tle_array; /* Array of VP context */
	int			pi_tle_array_len;	/* Maximum number of element of
									 * pi_vp_tle_array[] */
	ExprContext *pi_exprContext;	/* Execute context for executing this
									 * VciProjectionInfo */
	TupleTableSlot *pi_slot;	/* TupleTableSlot that contains this
								 * VciProjectionInfo result */
	bool		pi_directMap;
	int			pi_numSimpleVars;	/* Number of Simple Vars */
	int		   *pi_varSlotOffsets;	/* Pointer of mapping information used by
									 * Simple Vars */
	int		   *pi_varNumbers;	/* Pointer of mapping information used by
								 * Simple Vars */
	int		   *pi_varOutputCols;	/* Pointer of mapping information used by
									 * Simple Vars */
	VciProjectionInfoSlot *pi_slotMap;	/* Map information that records
										 * whether each target list was
										 * converted to SimpleVar or VP
										 * context. */
	int			pi_lastInnerVar;
	int			pi_lastOuterVar;
	int			pi_lastScanVar;
} VciProjectionInfo;

/* ----------------
 *   VCI Scan/Sort/Agg Common Definitions
 * ----------------
 */

/*
 * Macros specified in flags of CustomScan and CustomScanState
 */
#define VCI_CUSTOMPLAN_MASK		(0x00F0)
#define VCI_CUSTOMPLAN_SCAN		(0x0010)
#define VCI_CUSTOMPLAN_SORT		(0x0020)
#define VCI_CUSTOMPLAN_AGG		(0x0030)
#define VCI_CUSTOMPLAN_GATHER	(0x0060)

/**
 * VCI based Plan node
 */
typedef struct VciPlan
{
	CustomScan	cscan;			/* Base class CustomScan */

	/*
	 * The following parameters are set by the (sequential) scheduler.
	 */
	int			preset_eflags;	/* eflags precalculated for parallel
								 * scheduling */

	AttrNumber	scan_plan_no;	/* Plan Number for VCI Scan that becomes a
								 * partitioned table */

	/** Cache of vci_search_scan() result */
	struct VciScan *scan_cached;

	/** Plan to be rewritten. Become NULL when copyObject() is called */
	Plan	   *orig_plan;
} VciPlan;

/**
 * VCI based Plan State node
 */
typedef struct VciPlanState
{
	CustomScanState css;		/* Base class CustomScanState */

	/** Cache of vci_search_scan_state() result */
	struct VciScanState *scanstate_cached;

} VciPlanState;

/**
 * VCI Scan node
 */
typedef struct VciScan
{
	VciPlan		vci;			/* Base class VCI Plan */

	VciScanMode scan_mode;

	Index		scanrelid;		/* relid of table to be scanned */
	Oid			reloid;			/* OID of table to be scanned */
	Oid			indexoid;		/* OID of VCi index that actually reads data */
	Bitmapset  *attr_used;		/* Bitmap of column (attribute) to scans */
	int			num_attr_used;	/* Number of scan column */
	bool		is_all_simple_vars; /* Target list is configured with
									 * SimpleVar */
	double		estimate_tuples;	/* Estimated number of rows in the scanned
									 * table	*/
	bool		is_subextent_grain; /* Execute sub-extent fine-grained
									 * parallelization or not */
	Index		index_ph_id;	/* index_ph_table[index_ph_id-1] of
								 * vci_query_context_t */
	Index		fetch_ph_id;	/* index_ph_table[index_ph_id-1].fetch_ph_table[fetch_ph_id-1]
								 * of vci_query_context_t */
} VciScan;

/**
 * VCI Scan State node
 */
typedef struct VciScanState
{
	VciPlanState vci;			/* Base class VCI Plan State */

	bool		is_subextent_grain; /* Execute sub-extent fine-grained
									 * parallelization or not */

	/*
	 * Column store fetch setting
	 */
	vci_CSFetchContext fetch_context;	/* Columnar fetch context (master) */
	vci_CSFetchContext local_fetch_context; /* Columnar fetch context (locale
											 * of each process) */
	vci_extent_status_t *extent_status; /* extent information */
	vci_virtual_tuples_t *vector_set;	/* vector set */

	AttrNumber	last_attr;		/* Biggest Attr Number */
	int		   *attr_map;		/* Map that substracts column store fetch id
								 * from Attr Number */

	int32		first_extent_id;	/* Extent number that starts reading */
	int32		last_extent_id; /* Extent number that finishes reading
								 * (exclusive) */
	int64		first_crid;		/* CRID that starts read */
	int64		last_crid;		/* CRID that finishes read (exclusive) */

	/*
	 * The following are read and written during column store fetch execution.
	 */

	/**
	 * true when the first column store fetch is executed
	 *
	 * Set to false before executing column store fetch
	 */
	bool		first_fetch;

	VciFetchPos pos;			/* Current column store fetch location */
	VciFetchPos mark;			/* Column store fetch location recorded in
								 * mergr */

	VciVPContext *vp_qual;		/* VP context converted from qual  */

	VciProjectionInfo *vps_ProjInfo;	/* when generating oputput with non-VP */

	/*
	 * The result of vector processing will be recorded in
	 * result_values[resind][i] and result_isnull[resind][i]. With resind is
	 * order of target list and i is number in vector
	 */
	Datum	  **result_values;	/** Process result after Vector processing (value information) */
	bool	  **result_isnull;	/** Process result after Vector processing (NULL information) */

	/**
	 * Number of Vector processing context
	 */
	int			num_vp_targets;

	/**
	 * Arrays to pointer to Vector processing context
	 */
	VciVPContext **vp_targets;

	/***
	 * true when parallel worker receives NULL
	 */
	bool		scan_done;

} VciScanState;

/**
 * VCI Sort node
 */
typedef struct VciSort
{
	VciPlan		vci;			/* Base class VCI Plan */

	int			numCols;		/* number of sort-key columns */
	AttrNumber *sortColIdx;		/* their indexes in the target list */
	Oid		   *sortOperators;	/* OIDs of operators to sort them by */
	Oid		   *collations;		/* OIDs of collations */
	bool	   *nullsFirst;		/* NULLS FIRST/LAST directions */
} VciSort;

/**
 * VCI Sort State node
 */
typedef struct VciSortState
{
	VciPlanState vci;			/* Base class VCI Plan State */

	bool		randomAccess;	/* need random access to sort output? */
	bool		bounded;		/* is the result set bounded? */
	int64		bound;			/* if bounded, how many tuples are needed */
	bool		sort_Done;		/* sort completed yet? */
	bool		bounded_Done;	/* value of bounded we did the sort with */
	int64		bound_Done;		/* value of bound we did the sort with */
	void	   *tuplesortstate; /* private state of tuplesort.c */

	ScanDirection saved_dir;	/* area to store estate->es_direction */
} VciSortState;

/**
 * VCI Agg node
 */
typedef struct VciAgg
{
	VciPlan		vci;			/* base class VCI Plan State */

	AggStrategy aggstrategy;
	int			numCols;		/* number of grouping columns */
	AttrNumber *grpColIdx;		/* their indexes in the target list */
	Oid		   *grpOperators;	/* equality operators to compare with */
	Oid		   *grpCollations;
	int64		numGroups;		/* estimated number of groups in input */
} VciAgg;

typedef struct VciAggStatePerAggData *VciAggStatePerAgg;
typedef struct VciAggStatePerGroupData *VciAggStatePerGroup;

/**
 * VCI Agg State node
 */
typedef struct VciAggState
{
	VciPlanState vci;			/* Base class VCI Plan State */

	bool		enable_vp;		/* Is vector processing possible or not */

	VciProjectionInfo *vps_ProjInfo;	/* ProjectionInfo when generating Agg
										 * State output */

	List	   *aggs;			/* all Aggref nodes in targetlist & quals */
	int			numaggs;		/* length of list (could be zero!) */
	Oid		   *eqfuncoids;		/* per-grouping-field equality fn oids */
	ExprState **eqfunctions;	/* expression returning equality */
	FmgrInfo   *hashfunctions;	/* per-grouping-field hash fns */
	VciAggStatePerAgg peragg;	/* per-Aggref information */
	MemoryContext hash_metacxt; /* memory for hash table bucket array */
	MemoryContext hash_tuplescxt;	/* memory for hash table tuples */
	MemoryContext aggcontext;	/* memory context for long-lived data */
	ExprContext *tmpcontext;	/* econtext for input expressions */
	bool		agg_done;		/* indicates completion of Agg scan */
	/* these fields are used in AGG_PLAIN and AGG_SORTED modes: */
	VciAggStatePerGroup pergroup;	/* per-Aggref-per-group working state */
	HeapTuple	grp_firstTuple; /* copy of first tuple of current group */
	/* these fields are used in AGG_HASHED mode: */
	TupleTableSlot *hashslot;	/* slot for loading hash table */
	TupleHashTable hashtable;	/* hash table with one entry per group */
	int			last_hash_column;
	int		   *hash_needed;	/* array of columns needed in hash table */
	int			num_hash_needed;	/* number of columns needed in hash table */
	Datum	  **hash_input_values;	/* array of pointers to datum vector for
									 * each hash key */
	bool	  **hash_input_isnull;	/* array of pointers to null vector for
									 * each ehash key */
	bool		table_filled;	/* hash table filled yet? */
	TupleHashIterator hashiter; /* for iterating through hash table */

	/*
	 * aggregation function changes its behaviour by checking AggState
	 * Therefore, ExecEvalExpr() shows dummy AggState, not VciAggState
	 */
	AggState   *pseudo_aggstate;

	/**
	 * Record VciAggHashEntry before copying to SMC, in case of parallel worker
	 * encounter out-of-memory error in SMC.
	 * Usually set to NULL.
	 */
	volatile TupleHashEntry saved_entry;

	/**
	 * Similar to saved_entry, but only records the first HeapTuple of
	 * each group in plain/sorted aggregation
	 * Usually set to NULL.
	 */
	volatile HeapTuple saved_grp_firstTuple;

} VciAggState;

typedef void (*VciAdvanceAggref_Func) (VciAggState *, int, VciAggStatePerGroup *, int);

extern VciAdvanceAggref_Func VciGetSpecialAdvanceAggrefFunc(VciAggStatePerAgg peraggstate);

/* ----------------
 *   VCI Gather information
 * ----------------
 */

typedef struct VciGather
{
	VciPlan		vci;

} VciGather;

typedef struct VciGatherState
{
	VciPlanState vci;
} VciGatherState;

/* ----------------
 *   VCI Var State
 * ----------------
 */
/**
 * Var expression state for VCI
 *
 * Normally, Var expression is converted to ExprState exression state in ExecInitNode(),
 * but in VCI, additional information is required, so a dedicated class is created.
 */
typedef struct VciVarState
{
	ExprState	xprstate;		/* Base class VCI Plan State */
	VciScanState *scanstate;	/* Pointer to VciScanState from which Var will
								 * load data */
} VciVarState;

/**
 * Param expression state for VCI
 *
 * Normally, Param expression is converted to ExprState exression state in ExecInitNode(),
 * but in VCI, additional information is required, so a dedicated class is created.
 */
typedef struct VciParamState
{
	ExprState	xprstate;		/* Base class VCI Plan State */
	Plan	   *plan;			/* th plan to hold this Param */

} VciParamState;

extern CustomScanMethods vci_scan_scan_methods;
extern CustomExecMethods vci_scan_exec_column_store_methods;
extern CustomScanMethods vci_sort_scan_methods;
extern CustomExecMethods vci_sort_exec_methods;
extern CustomScanMethods vci_agg_scan_methods;
extern CustomExecMethods vci_agg_exec_methods;
extern CustomScanMethods vci_hashagg_scan_methods;
extern CustomExecMethods vci_hashagg_exec_methods;
extern CustomScanMethods vci_groupagg_scan_methods;
extern CustomExecMethods vci_groupagg_exec_methods;
extern CustomScanMethods vci_gather_scan_methods;
extern CustomExecMethods vci_gather_exec_methods;

/* ----------------
 *   vci_executor.c
 * ----------------
 */

/**
 * Enum that specifies how Var is handled in ExecInitNode()
 */
typedef enum vci_initexpr
{
	VCI_INIT_EXPR_NONE,

	/** Var converts to ExprState like original */
	VCI_INIT_EXPR_NORMAL,

	/** Var converts to VciVarState */
	VCI_INIT_EXPR_FETCHING_COLUMN_STORE,

	/** Var converts to VciVarState, but Aggref and later convert to ExpState like original */
	VCI_INIT_EXPR_FETCHING_COLUMN_STORE_AGGREF,
} vci_initexpr_t;

extern ExprState *VciExecInitQual(List *qual, PlanState *parent, vci_initexpr_t inittype);
extern TupleTableSlot *VciExecProject(VciProjectionInfo *projInfo);

extern VciProjectionInfo *VciExecBuildProjectionInfo(List *targetList,
													 ExprContext *econtext,
													 TupleTableSlot *slot,
													 PlanState *parent,
													 TupleDesc inputDesc);

/* ----------------
 *   vci_planner.c
 * ----------------
 */
extern bool vci_is_supported_jointype(JoinType jointype);

/* ----------------
 *   vci_plan.c
 * ----------------
 */

extern bool vci_is_custom_plan(Plan *plan);
extern int	vci_get_vci_plan_type(Plan *plan);
extern void vci_copy_plan(VciPlan *dest, const VciPlan *src);
extern struct VciScan *vci_search_scan(VciPlan *);
extern struct VciScanState *vci_search_scan_state(VciPlanState *);
extern List *vci_generate_pass_through_target_list(List *targetlist);

/* ----------------
 *   vci_plan_func.c
 * ----------------
 */

struct QueryDesc;

/**
 * Callback to notify plan_id before analyzing topmost plan
 * (top of main plan tree and each subplan tree) in vci_plannedstmt_tree_walker()
 * or vci_plannedstmt_tree_mutator() analyze.
 */
typedef void (*vci_topmost_plan_cb_t) (Plan *, int plan_id, void *context);

/**
 * Template for a function pointer passed as a callback to a mutator routine that rewrites a plan.
 */
typedef bool (*vci_mutator_t) (Plan **plan_p, Plan *parent, void *context, int eflags, bool *changed);

extern PGDLLEXPORT bool vci_plannedstmt_tree_walker(PlannedStmt *plannedstmt, bool (*walker) (Plan *, void *), vci_topmost_plan_cb_t topmostplan, void *context);
extern PGDLLEXPORT bool vci_plan_tree_walker(Plan *plan, bool (*walker) (Plan *, void *), void *context);
extern bool vci_expression_walker(Plan *plan, bool (*walker) (Node *, void *), void *context);
extern bool vci_expression_and_colid_walker(Plan *plan, bool (*walker) (Node *, void *), void (*attr_cb) (AttrNumber *, void *), void *context);
extern bool vci_expression_and_initplan_walker(Plan *plan, bool (*walker) (Node *, void *), bool (*walker_initplan) (Node *, void *), void *context);

extern bool vci_plannedstmt_tree_mutator(PlannedStmt *plannedstmt, vci_mutator_t mutator, vci_topmost_plan_cb_t topmostplan, void *context, int eflags, bool *changed);
extern bool vci_plannedstmt_tree_mutator_order(PlannedStmt *plannedstmt, vci_mutator_t mutator, vci_topmost_plan_cb_t topmostplan, void *context, int eflags, bool *changed, int *subplan_order);
extern bool vci_plan_tree_mutator(Plan **plan_p, Plan *parent, vci_mutator_t mutator, void *context, int eflags, bool *changed);

/* ----------------
 *   vci_scan.c
 * ----------------
 */
extern TupleTableSlot *VciExecProcScanTuple(VciScanState *node);
extern int	VciExecProcScanVector(VciScanState *scanstate);

/* ----------------
 *   vci_sort.c
 * ----------------
 */
struct Tuplesortstate;

extern struct Tuplesortstate *vci_sort_exec_top_half(VciSortState *sortstate);
extern void vci_sort_perform_sort(VciSortState *sortstate);

/* ----------------
 *   vci_agg.c
 * ----------------
 */

extern void vci_agg_fill_hash_table(VciAggState *aggstate);
extern TupleTableSlot *vci_agg_retrieve_hash_table(VciAggState *aggstate);
extern TupleHashEntry vci_agg_find_group_from_hash_table(VciAggState *aggstate);
extern void vci_initialize_aggregates(VciAggState *aggstate,
									  VciAggStatePerAgg peragg,
									  VciAggStatePerGroup pergroup);
extern void vci_finalize_aggregate(VciAggState *aggstate, VciAggStatePerAgg peraggstate, VciAggStatePerGroup pergroupstate, Datum *resultVal, bool *resultIsNull);
extern void vci_advance_aggregates(VciAggState *aggstate, VciAggStatePerGroup pergroup);

/* ----------------
 *   vci_aggmergetranstype.c
 * ----------------
 */

/**
 * Template for function pointer for copying Datum
 */
typedef Datum (*VciCopyDatumFunc) (Datum, bool, int);

extern bool vci_is_supported_aggregation(Aggref *aggref);

/* ----------------
 *   vci_gather.c
 * ----------------
 */

/* ----------------
 *   vci_param.c
 * ----------------
 */
extern void VciExecEvalParamExec(ExprState *exprstate, ExprEvalStep *op, ExprContext *econtext);

/* ----------------
 *   Column store fetching (vci_fetch_column_store.c)
 * ----------------
 */
extern void vci_initialize_query_context(QueryDesc *queryDesc, int eflags);
extern void vci_finalize_query_context(void);
extern void vci_free_query_context(void);
extern bool vci_is_processing_custom_plan(void);

extern void vci_create_one_fetch_context_for_fetching_column_store(VciScanState *scanstate, ExprContext *econtext);
extern void vci_clone_one_fetch_context_for_fetching_column_store(VciScanState *scanstate);
extern void vci_destroy_one_fetch_context_for_fetching_column_store(VciScanState *scanstate);

extern void vci_set_starting_position_for_fetching_column_store(VciScanState *scanstate, int64 crid, int size);

extern bool vci_fill_vector_set_from_column_store(VciScanState *scanstate);
extern void vci_mark_pos_vector_set_from_column_store(VciScanState *scanstate);
extern void vci_restr_pos_vector_set_from_column_store(VciScanState *scanstate);
extern void vci_step_next_tuple_from_column_store(VciScanState *scanstate);
extern void vci_finish_vector_set_from_column_store(VciScanState *scanstate);

extern void VciExecTargetListWithVectorProcessing(VciScanState *scanstate, ExprContext *econtext, int max_slots);
extern void VciExecEvalScalarVarFromColumnStore(ExprState *exprstate, ExprEvalStep *op, ExprContext *econtext);

/* ----------------
 *   vci_planner.c
 * ----------------
 */
extern PlannedStmt *vci_generate_custom_plan(PlannedStmt *src, int eflags, Snapshot snapshot);

#endif							/* VCI_EXECUTOR_H */
