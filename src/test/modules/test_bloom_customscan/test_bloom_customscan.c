/*-------------------------------------------------------------------------
 *
 * test_bloom_customscan.c
 *		Minimal CustomScan provider that consumes a hash-join Bloom filter.
 *
 * This is a test-only stand-in for a storage-level scan provider.  When enabled, it
 * installs a set_rel_pathlist_hook that offers a CustomPath for plain heap
 * base relations, advertising CUSTOMPATH_SUPPORT_BLOOM_FILTERS.  That opt-in
 * is what lets the planner (a) generate filter-bearing paths for the scan and
 * (b) push a hash-join Bloom filter down to it.
 *
 * The scan itself is an ordinary heap sequential scan; the only interesting
 * part is that its per-tuple loop probes the pushed-down combined filter with
 * ExecBloomFilters() -- exactly what a stock scan does inside ExecScanExtended,
 * but here done by the provider itself -- and, for a multi-key join, checks
 * that the per-key filters were populated on the producer side.
 *
 * A few SQL-callable functions expose counters so the regression test can
 * assert that the filter was actually probed and rejected tuples, and that
 * per-key filters were built, without depending on timing or EXPLAIN output.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	  src/test/modules/test_bloom_customscan/test_bloom_customscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* GUC: when off, the hook does nothing and plans look exactly like upstream. */
static bool test_bloom_cs_enabled = false;

/* Observation counters, reset/read from SQL. */
static uint64 test_bloom_cs_scanned = 0;
static uint64 test_bloom_cs_rejected = 0;
static bool test_bloom_cs_perkey_seen = false;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/* Execution state, embedding CustomScanState as its first field. */
typedef struct BloomCSScanState
{
	CustomScanState css;
	TableScanDesc scandesc;
	bool		inspected;		/* have we checked per-key filters yet? */
}			BloomCSScanState;

/* forward declarations */
static Plan *bloom_cs_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
									   CustomPath *best_path, List *tlist,
									   List *clauses, List *custom_plans);
static Node *bloom_cs_create_scan_state(CustomScan *cscan);
static void bloom_cs_begin_scan(CustomScanState *node, EState *estate,
								int eflags);
static TupleTableSlot *bloom_cs_exec_scan(CustomScanState *node);
static void bloom_cs_end_scan(CustomScanState *node);
static void bloom_cs_rescan(CustomScanState *node);

static const CustomPathMethods bloom_cs_path_methods = {
	.CustomName = "TestBloomCustomScan",
	.PlanCustomPath = bloom_cs_plan_custom_path,
};

static const CustomScanMethods bloom_cs_scan_methods = {
	.CustomName = "TestBloomCustomScan",
	.CreateCustomScanState = bloom_cs_create_scan_state,
};

static const CustomExecMethods bloom_cs_exec_methods = {
	.CustomName = "TestBloomCustomScan",
	.BeginCustomScan = bloom_cs_begin_scan,
	.ExecCustomScan = bloom_cs_exec_scan,
	.EndCustomScan = bloom_cs_end_scan,
	.ReScanCustomScan = bloom_cs_rescan,
};

/*
 * set_rel_pathlist_hook: offer a bloom-filter-capable CustomPath for plain
 * heap base relations.  We copy the cost of the existing sequential scan path
 * so join costing stays sane; the test forces the custom scan to be chosen
 * with "SET enable_seqscan = off" (the CustomPath keeps disabled_nodes = 0).
 */
static void
bloom_cs_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
						  RangeTblEntry *rte)
{
	CustomPath *cpath;
	Cost		startup_cost = 0;
	Cost		total_cost = 0;
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!test_bloom_cs_enabled)
		return;

	/* Only plain, ordinary base tables. */
	if (rel->reloptkind != RELOPT_BASEREL)
		return;
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
		return;

	/* Borrow the sequential scan's cost estimate. */
	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		if (path->pathtype == T_SeqScan && path->param_info == NULL)
		{
			startup_cost = path->startup_cost;
			total_cost = path->total_cost;
			break;
		}
	}

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.parallel_aware = false;
	cpath->path.parallel_safe = false;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = rel->rows;
	cpath->path.startup_cost = startup_cost;
	cpath->path.total_cost = total_cost;
	cpath->path.pathkeys = NIL;

	cpath->flags = CUSTOMPATH_SUPPORT_BLOOM_FILTERS;
	cpath->custom_paths = NIL;
	cpath->custom_private = NIL;
	cpath->methods = &bloom_cs_path_methods;

	add_path(rel, (Path *) cpath);
}

static Plan *
bloom_cs_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->flags = best_path->flags;
	cscan->methods = &bloom_cs_scan_methods;
	cscan->custom_plans = custom_plans;

	/* A base-relation scan: leave custom_scan_tlist NIL, use rel's rowtype. */
	cscan->scan.scanrelid = rel->relid;
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);

	return &cscan->scan.plan;
}

static Node *
bloom_cs_create_scan_state(CustomScan *cscan)
{
	BloomCSScanState *bcss = (BloomCSScanState *) newNode(sizeof(BloomCSScanState),
														  T_CustomScanState);

	bcss->css.methods = &bloom_cs_exec_methods;
	bcss->scandesc = NULL;
	bcss->inspected = false;

	/*
	 * We scan a heap table with table_scan_getnextslot(), which stores
	 * on-disk heap tuples, so the scan tuple slot must be a buffer-heap slot
	 * rather than the default virtual slot.  ExecInitCustomScan() reads
	 * css.slotOps when building ss_ScanTupleSlot (before BeginCustomScan
	 * runs), so it has to be set here.
	 */
	bcss->css.slotOps = &TTSOpsBufferHeapTuple;

	return (Node *) bcss;
}

static void
bloom_cs_begin_scan(CustomScanState *node, EState *estate, int eflags)
{
	BloomCSScanState *bcss = (BloomCSScanState *) node;

	/*
	 * ExecInitCustomScan already opened the scan relation (scanrelid > 0) and
	 * called ExecInitBloomFilters(), so node->ss.ps.bloom_filters is set up.
	 * We just need a table scan descriptor.
	 */
	if (!(eflags & (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_EXPLAIN_GENERIC)))
		bcss->scandesc = table_beginscan(node->ss.ss_currentRelation,
										 estate->es_snapshot, 0, NULL, 0);
}

/*
 * On the first tuple (by which point an eager producer has built its hash
 * table and filters), record whether the per-key filters were populated.
 */
static void
bloom_cs_maybe_inspect_perkey(BloomCSScanState * bcss)
{
	ListCell   *lc;

	if (bcss->inspected)
		return;

	foreach(lc, bcss->css.ss.ps.bloom_filters)
	{
		BloomFilterState *bfs = (BloomFilterState *) lfirst(lc);
		HashJoinState *producer = bfs->producer;
		HashState  *hashNode;

		if (producer == NULL)
			continue;
		hashNode = castNode(HashState, innerPlanState(&producer->js.ps));

		/* Hash table (and filters) not built yet; try again next tuple. */
		if (hashNode->bloom_filter == NULL)
			return;

		if (hashNode->want_perkey_bloom &&
			hashNode->perkey_nfilters > 0 &&
			hashNode->perkey_filters != NULL)
			test_bloom_cs_perkey_seen = true;
	}

	bcss->inspected = true;
}

static TupleTableSlot *
bloom_cs_exec_scan(CustomScanState *node)
{
	BloomCSScanState *bcss = (BloomCSScanState *) node;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	TupleTableSlot *scanslot = node->ss.ss_ScanTupleSlot;
	ExprState  *qual = node->ss.ps.qual;
	ProjectionInfo *proj = node->ss.ps.ps_ProjInfo;

	bloom_cs_maybe_inspect_perkey(bcss);

	for (;;)
	{
		ResetExprContext(econtext);

		if (!table_scan_getnextslot(bcss->scandesc, ForwardScanDirection,
									scanslot))
		{
			if (proj)
				return ExecClearTuple(proj->pi_state.resultslot);
			return ExecClearTuple(scanslot);
		}

		test_bloom_cs_scanned++;
		econtext->ecxt_scantuple = scanslot;

		/*
		 * Probe the pushed-down combined Bloom filter, just as a stock scan
		 * would inside ExecScanExtended.  A conclusive miss lets us drop the
		 * tuple without evaluating the qual or projection.
		 */
		if (!ExecBloomFilters(node->ss.ps.bloom_filters, econtext))
		{
			test_bloom_cs_rejected++;
			continue;
		}

		if (qual != NULL && !ExecQual(qual, econtext))
			continue;

		if (proj != NULL)
			return ExecProject(proj);

		return scanslot;
	}
}

static void
bloom_cs_end_scan(CustomScanState *node)
{
	BloomCSScanState *bcss = (BloomCSScanState *) node;

	if (bcss->scandesc != NULL)
		table_endscan(bcss->scandesc);
}

static void
bloom_cs_rescan(CustomScanState *node)
{
	BloomCSScanState *bcss = (BloomCSScanState *) node;

	bcss->inspected = false;
	if (bcss->scandesc != NULL)
		table_rescan(bcss->scandesc, NULL);
}

/* ------------------------------------------------------------------------
 * SQL-callable observation helpers
 * ------------------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(test_bloom_cs_reset);
PG_FUNCTION_INFO_V1(test_bloom_cs_scanned_rows);
PG_FUNCTION_INFO_V1(test_bloom_cs_rejected_rows);
PG_FUNCTION_INFO_V1(test_bloom_cs_perkey_built);

Datum
test_bloom_cs_reset(PG_FUNCTION_ARGS)
{
	test_bloom_cs_scanned = 0;
	test_bloom_cs_rejected = 0;
	test_bloom_cs_perkey_seen = false;
	PG_RETURN_VOID();
}

Datum
test_bloom_cs_scanned_rows(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) test_bloom_cs_scanned);
}

Datum
test_bloom_cs_rejected_rows(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) test_bloom_cs_rejected);
}

Datum
test_bloom_cs_perkey_built(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(test_bloom_cs_perkey_seen);
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("test_bloom_customscan.enable",
							 "Offer a bloom-filter-capable CustomScan for heap tables.",
							 NULL,
							 &test_bloom_cs_enabled,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("test_bloom_customscan");

	RegisterCustomScanMethods(&bloom_cs_scan_methods);

	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = bloom_cs_set_rel_pathlist;
}
