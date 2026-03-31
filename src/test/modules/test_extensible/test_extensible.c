/*-------------------------------------------------------------------------
 *
 * test_extensible.c
 *		Test correctness of extensible node and custom scan registration
 *		functions. For more details see "Writing a Custom Scan Provider" in
 *		the documentation.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_extensible/test_extensible.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/readfuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

/* Name of the test table that triggers our CustomScan injection */
#define TEST_TABLE_NAME		"test_extensible_tbl"

/*
 * TestExtNode - an ExtensibleNode subtype carrying our planning data.
 */
typedef struct TestExtNode
{
	ExtensibleNode base;		/* must be first */
	Oid			relid;			/* OID of the relation being scanned */
	int			repeat_count;	/* how many times to return each scanned row */
}			TestExtNode;

#define TEST_EXT_NODE_NAME	"TestExtNode"
#define TEST_CUSTOM_SCAN_NAME	"TestCustomScan"

/*
 * ExtensibleNodeMethods callbacks
 *
 * We never call these ourselves. The generic node routines dispatch to them
 * via GetExtensibleNodeMethods() whenever they meet a T_ExtensibleNode:
 * copyObject() calls nodeCopy, equal() calls nodeEqual, nodeToString() calls
 * nodeOut and stringToNode() calls nodeRead. Note that nodeOut and nodeRead
 * have to agree on the set and the order of the serialized fields.
 */

static void
test_ext_node_copy(ExtensibleNode *newnode, const ExtensibleNode *oldnode)
{
	((TestExtNode *) newnode)->relid = ((const TestExtNode *) oldnode)->relid;
	((TestExtNode *) newnode)->repeat_count =
		((const TestExtNode *) oldnode)->repeat_count;
}

static bool
test_ext_node_equal(const ExtensibleNode *a, const ExtensibleNode *b)
{
	return ((const TestExtNode *) a)->relid ==
		((const TestExtNode *) b)->relid &&
		((const TestExtNode *) a)->repeat_count ==
		((const TestExtNode *) b)->repeat_count;
}

static void
test_ext_node_out(StringInfo str, const ExtensibleNode *node)
{
	appendStringInfo(str, " :relid %u", ((const TestExtNode *) node)->relid);
	appendStringInfo(str, " :repeat_count %d",
					 ((const TestExtNode *) node)->repeat_count);
}

static void
test_ext_node_read(ExtensibleNode *node)
{
	TestExtNode *tnode = (TestExtNode *) node;
	const char *token;
	int			length;

	token = pg_strtok(&length); /* skip :relid */
	token = pg_strtok(&length); /* get value */
	tnode->relid = atooid(token);

	token = pg_strtok(&length); /* skip :repeat_count */
	token = pg_strtok(&length); /* get value */
	tnode->repeat_count = atoi(token);
}

static const ExtensibleNodeMethods test_ext_node_methods =
{
	.extnodename = TEST_EXT_NODE_NAME,
	.node_size = sizeof(TestExtNode),
	.nodeCopy = test_ext_node_copy,
	.nodeEqual = test_ext_node_equal,
	.nodeOut = test_ext_node_out,
	.nodeRead = test_ext_node_read,
};

/*
 * TestCustomScanState - execution state for the custom scan
 */
typedef struct TestCustomScanState
{
	CustomScanState css;		/* must be first */
	TableScanDesc scandesc;
	int			repeat_count;	/* repeat_count from TestExtNode */
	int			repeats_left;	/* how many more times to return current row */
}			TestCustomScanState;

/*
 * Executor callbacks
 */

/*
 * Retrieve our private planning data from a CustomScan node. This is the place
 * where the ExtensibleNode crosses from the plan tree into the executor.
 */
static TestExtNode *
test_get_ext_node(CustomScan *cscan)
{
	TestExtNode *tnode;

	Assert(list_length(cscan->custom_private) == 1);
	tnode = (TestExtNode *) linitial(cscan->custom_private);
	Assert(IsA(tnode, ExtensibleNode));
	Assert(strcmp(tnode->base.extnodename, TEST_EXT_NODE_NAME) == 0);

	return tnode;
}

/*
 * BeginCustomScan is the last thing ExecInitCustomScan() does, so the generic
 * parts of the CustomScanState - the scan relation and the tuple slots among
 * them - are ready by now. See "Custom Scan Execution Callbacks".
 */
static void
test_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
	TestCustomScanState *tstate = (TestCustomScanState *) node;
	TestExtNode *tnode = test_get_ext_node((CustomScan *) node->ss.ps.plan);
	Relation	rel = node->ss.ss_currentRelation;

	Assert(tnode->repeat_count > 0);
	tstate->repeat_count = tnode->repeat_count;
	tstate->repeats_left = 0;

	/* Start a plain sequential table scan */
	tstate->scandesc = table_beginscan(rel, estate->es_snapshot, 0, NULL,
									   SO_NONE);
}

/*
 * Access method for ExecScan(): return the next tuple to be considered, or
 * NULL when the scan is done
 */
static TupleTableSlot *
test_scan_next(ScanState *node)
{
	TestCustomScanState *tstate = (TestCustomScanState *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;

	/*
	 * If the current tuple still has repeats remaining, return it again
	 * without advancing the heap scan. The repeat count comes from the
	 * TestExtNode that was read in BeginCustomScan.
	 */
	if (tstate->repeats_left > 0)
	{
		tstate->repeats_left--;
		return slot;
	}

	/* Fetch the next tuple from the heap */
	if (!table_scan_getnextslot(tstate->scandesc, ForwardScanDirection, slot))
		return NULL;

	/* Schedule (repeat_count - 1) additional returns of this tuple */
	tstate->repeats_left = tstate->repeat_count - 1;
	return slot;
}

/*
 * Recheck method for ExecScan(), used only during EvalPlanQual rechecks. Our
 * tuples come straight from an MVCC-checked table scan and we evaluate no
 * quals of our own, so there is nothing to recheck.
 */
static bool
test_scan_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
test_exec_custom_scan(CustomScanState *node)
{
	/*
	 * The core code applies neither the qual nor the projection to the tuples
	 * we return, so we hand the work over to the generic ExecScan()
	 * machinery, exactly as the built-in scan nodes do
	 */
	return ExecScan(&node->ss, test_scan_next, test_scan_recheck);
}

static void
test_end_custom_scan(CustomScanState *node)
{
	TestCustomScanState *tstate = (TestCustomScanState *) node;

	if (tstate->scandesc)
		table_endscan(tstate->scandesc);
}

/*
 * ReScanCustomScan has to reset our own state as well as the state kept by the
 * ExecScan() machinery
 */
static void
test_rescan_custom_scan(CustomScanState *node)
{
	TestCustomScanState *tstate = (TestCustomScanState *) node;

	tstate->repeats_left = 0;
	table_rescan(tstate->scandesc, NULL);
	ExecScanReScan(&node->ss);
}

static const CustomExecMethods test_custom_exec_methods =
{
	.CustomName = TEST_CUSTOM_SCAN_NAME,
	.BeginCustomScan = test_begin_custom_scan,
	.ExecCustomScan = test_exec_custom_scan,
	.EndCustomScan = test_end_custom_scan,
	.ReScanCustomScan = test_rescan_custom_scan,
};

/*
 * CreateCustomScanState allocates the CustomScanState and is expected
 * to fill in its node tag, its methods and, optionally, slotOps. Everything
 * else is left to ExecInitCustomScan(). See "Custom Scan Plan Callbacks".
 */
static Node *
test_create_custom_scan_state(CustomScan *cscan)
{
	TestCustomScanState *tstate;
	TestExtNode *tnode = test_get_ext_node(cscan);
	Relation	rel;

	tstate = (TestCustomScanState *)
		newNode(sizeof(TestCustomScanState), T_CustomScanState);
	tstate->css.methods = &test_custom_exec_methods;

	/*
	 * Tell ExecInitCustomScan() which kind of tuple slot our ExecCustomScan
	 * callback is going to fill, so that table_scan_getnextslot() can store
	 * tuples in it directly. The answer depends on the table AM, hence
	 * table_slot_callbacks().
	 *
	 * ExecInitCustomScan() opens the scan relation only after this callback
	 * returns, but the slot type has to be known by then. So we open the
	 * relation ourselves. Its OID was passed down from the planner in our
	 * ExtensibleNode. No lock is needed, as the query already holds one on
	 * every relation mentioned in its range table.
	 */
	rel = table_open(tnode->relid, NoLock);
	tstate->css.slotOps = table_slot_callbacks(rel);
	table_close(rel, NoLock);

	return (Node *) tstate;
}

static const CustomScanMethods test_custom_scan_methods =
{
	.CustomName = TEST_CUSTOM_SCAN_NAME,
	.CreateCustomScanState = test_create_custom_scan_state,
};

/*
 * Planner callbacks
 */

/*
 * PlanCustomPath turns our CustomPath into the CustomScan plan node that the
 * executor is going to run. The cost data and custom_relids are copied from
 * the path by the core once we return, so we only have to describe what the
 * scan does.
 */
static Plan *
test_plan_custom_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  struct CustomPath *best_path,
					  List *tlist,
					  List *clauses,
					  List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;

	/*
	 * Unlike the built-in scan nodes, a custom scan provider receives the
	 * restriction clauses as RestrictInfo nodes rather than as bare
	 * expressions. That is deliberate: the provider is free to evaluate some
	 * of the clauses on its own (say, push them down to a remote server), and
	 * the planner data in RestrictInfo helps it decide which ones. The
	 * clauses we want the executor to check have to be reduced to bare
	 * expressions and stored in the plan's qual, as "Creating Custom Scan
	 * Plans" requires the scan to be initialized like any other one.
	 *
	 * The second argument of extract_actual_clauses() selects the regular
	 * clauses rather than the pseudoconstant ones (constant-TRUE clauses are
	 * dropped in either case). Pseudoconstants must not end up in the qual:
	 * they do not depend on the scanned tuple, so the core evaluates them
	 * once in a gating Result node placed above us.
	 */
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;

	/* Our CustomPath has no child paths */
	cscan->custom_plans = custom_plans;
	cscan->custom_exprs = NIL;

	/*
	 * Pass the ExtensibleNode from the path to the plan via custom_private.
	 * This is the recommended pattern for conveying private planning data
	 * from a CustomPath to its corresponding CustomScan.
	 */
	cscan->custom_private = best_path->custom_private;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_relids = NULL;
	cscan->methods = &test_custom_scan_methods;

	return (Plan *) cscan;
}

static const CustomPathMethods test_custom_path_methods =
{
	.CustomName = TEST_CUSTOM_SCAN_NAME,
	.PlanCustomPath = test_plan_custom_path,
};

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

static void
test_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
					  Index rti, RangeTblEntry *rte)
{
	CustomPath *cpath;
	TestExtNode *tnode;
	char	   *relname;

	/* Let previous hooks run first */
	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	/* Only handle plain base relations (ordinary tables) */
	if (rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION)
		return;

	/*
	 * Only inject our CustomPath for the specific marker table. This prevents
	 * interference with system-catalog scans.
	 */
	relname = get_rel_name(rte->relid);
	if (relname == NULL || strcmp(relname, TEST_TABLE_NAME) != 0)
		return;

	/*
	 * Build a TestExtNode carrying the data our executor callbacks are going
	 * to need
	 */
	tnode = (TestExtNode *) newNode(sizeof(TestExtNode), T_ExtensibleNode);
	tnode->base.extnodename = TEST_EXT_NODE_NAME;
	tnode->relid = rte->relid;
	tnode->repeat_count = 2;	/* each row will be returned twice */

	/*
	 * Build the CustomPath. Claiming zero cost makes the planner prefer our
	 * path over the sequential scan, which keeps the test deterministic. A
	 * real provider would estimate the cost honestly and would also report
	 * the number of rows it actually returns, which in our case is
	 * repeat_count times the number of rows in the table.
	 */
	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.rows = rel->rows;
	cpath->path.startup_cost = 0;
	cpath->path.total_cost = 0;
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_private = list_make1(tnode);
	cpath->methods = &test_custom_path_methods;

	add_path(rel, (Path *) cpath);
}

/*
 * SQL-callable test functions
 */
PG_FUNCTION_INFO_V1(test_get_extensible_node_methods);
PG_FUNCTION_INFO_V1(test_get_custom_scan_methods);
PG_FUNCTION_INFO_V1(test_ext_node_callbacks);

/*
 * test_get_extensible_node_methods(name text, missing_ok bool)
 *
 * Thin wrapper around GetExtensibleNodeMethods(). Returns the registered
 * extnodename, or NULL when missing_ok = true and the name is not found.
 * Raises ERROR when missing_ok = false and the name is not found.
 */
Datum
test_get_extensible_node_methods(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		missing_ok = PG_GETARG_BOOL(1);
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(name, missing_ok);
	if (methods == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(methods->extnodename));
}

/*
 * test_get_custom_scan_methods(name text, missing_ok bool)
 *
 * Thin wrapper around GetCustomScanMethods(). Returns the registered
 * CustomName, or NULL when missing_ok = true and the name is not found.
 * Raises ERROR when missing_ok = false and the name is not found.
 */
Datum
test_get_custom_scan_methods(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		missing_ok = PG_GETARG_BOOL(1);
	const CustomScanMethods *methods;

	methods = GetCustomScanMethods(name, missing_ok);
	if (methods == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(methods->CustomName));
}

/*
 * test_ext_node_callbacks(relid oid, repeat_count int)
 *
 * Exercises all four ExtensibleNodeMethods callbacks, each of which is reached
 * through GetExtensibleNodeMethods(): nodeCopy via copyObject(), nodeEqual via
 * equal(), nodeOut via nodeToString() and nodeRead via stringToNode().
 *
 * Returns the serialized form of the node. Reports an ERROR if any of the
 * callbacks misbehaves.
 */
Datum
test_ext_node_callbacks(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int			repeat_count = PG_GETARG_INT32(1);
	TestExtNode *tnode;
	TestExtNode *copy;
	TestExtNode *restored;
	char	   *str;

	tnode = (TestExtNode *) newNode(sizeof(TestExtNode), T_ExtensibleNode);
	tnode->base.extnodename = TEST_EXT_NODE_NAME;
	tnode->relid = relid;
	tnode->repeat_count = repeat_count;

	/* nodeCopy + nodeEqual */
	copy = copyObject(tnode);
	if (!equal(tnode, copy))
		elog(ERROR, "copy of " TEST_EXT_NODE_NAME " is not equal to the original");

	/* nodeEqual must also be able to tell two different nodes apart */
	copy->repeat_count++;
	if (equal(tnode, copy))
		elog(ERROR, "nodes with different repeat_count compare as equal");

	/* nodeOut + nodeRead */
	str = nodeToString(tnode);
	restored = (TestExtNode *) stringToNode(str);
	if (!equal(tnode, restored))
		elog(ERROR, "deserialized " TEST_EXT_NODE_NAME " is not equal to the original");

	PG_RETURN_TEXT_P(cstring_to_text(str));
}

/*
 * Module initialization
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("cannot load \"%s\" after startup",
						"test_extensible"),
				 errdetail("\"%s\" must be loaded with "
						   "\"shared_preload_libraries\".",
						   "test_extensible")));

	/*
	 * Register the custom scan methods. Every backend that plans or executes
	 * such a scan has to do this, because the plan tree refers to the methods
	 * by name only; that includes parallel workers, which read the plan back
	 * with stringToNode(). Hence the shared_preload_libraries requirement.
	 */
	RegisterCustomScanMethods(&test_custom_scan_methods);

	/* Register the extensible node type */
	RegisterExtensibleNodeMethods(&test_ext_node_methods);

	/* Install the path-list hook to inject CustomPaths for the test table */
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = test_set_rel_pathlist;
}
