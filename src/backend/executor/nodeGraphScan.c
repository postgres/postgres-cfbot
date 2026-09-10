/*-------------------------------------------------------------------------
 *
 * nodeGraphScan.c
 *	  Routines to handle graph scan nodes.
 *
 * A GraphScan evaluates one quantified (variable-length) hop of a graph
 * pattern with a depth-first search.  The scan is a parameterized inner of
 * its enclosing join: each outer row provides a seed vertex (as nestloop
 * params, see GraphScan.seed_params).  From that seed the executor walks
 * the edge elements of the hop, one depth level at a time, by driving
 * per-depth copies of the planned 1-hop expansion (the inner plan, a UNION
 * ALL of the matching edge element tables).
 *
 * Each depth frame owns its own PlanState copy of the inner plan, so a
 * frame's scan cursor is independent and backtracking simply resumes the
 * parent frame's cursor (no bookkeeping needed).
 *
 * An edge is traversable from the current vertex iff the edge element's
 * source vertex element matches the current vertex's element and each source
 * key column equals the current vertex's key value (compared with the
 * default equality operator of the source key column datatype).  No src-key
 * filter is pushed into the inner plan, so heterogeneous source key widths
 * and element sets are handled uniformly (see build_graphscan_inner_query).
 *
 * Identified rows are emitted as (seed keys, terminal keys, VLE edge-list
 * arrays); the arrays contain the property values accumulated over the
 * path's edges, in traversal order (empty for a zero-length path).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGraphScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_propgraph_element.h"
#include "executor/executor.h"
#include "executor/nodeGraphScan.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "rewrite/rewriteGraphTable.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static TupleTableSlot *ExecGraphScan(PlanState *pstate);
static void build_arms(GraphScanState * node, GraphScan * plan);
static void build_arm_keys(List *keys, int *nkeys, FmgrInfo **eq, Oid **colls);
static bool graph_fetch_seed(GraphScanState * node, GraphScan * plan);
static bool graph_next(GraphScanState * node, GraphScan * plan);
static bool graph_step(GraphScanState * node, GraphScan * plan,
					   GraphDepthFrameData * fr);
static int	graph_try_edge(GraphScanState * node, GraphScan * plan,
						   GraphDepthFrameData * fr, TupleTableSlot *eslot,
						   Oid *newelem, int *newnkeys, Datum *newvid,
						   bool *newnull, Datum *eprops, bool *epropsnull);
static bool edge_key_matches(GraphScanState * node, GraphDepthFrameData * fr,
							 TupleTableSlot *eslot, GraphScanArmData * arm,
							 bool issrc);
static int	graph_find_arm(GraphScanState * node, Oid relid);
static void graph_push(GraphScanState * node, Oid newelem, int newnkeys,
					   Datum *newvid, bool *newnull, Datum *eprops,
					   bool *epropsnull);
static void graph_backtrack(GraphScanState * node);
static void graph_reset(GraphScanState * node);
static void graph_build_row(GraphScanState * node, TupleTableSlot *slot);
static TupleTableSlot *graph_emit_row(GraphScanState *node, TupleTableSlot *slot);
static Datum graph_build_edge_array(GraphScanState * node, TupleTableSlot *slot,
									int pi);

/* Result values of graph_try_edge */
#define GRAPH_EDGE_NONE 0		/* not traversable */
#define GRAPH_EDGE_FORWARD 1	/* traversable in the forward direction */
#define GRAPH_EDGE_FORWARD 1	/* forward traversal */

/*
 * Compile, per edge element arm, the metadata needed to match edges against
 * the current vertex: element ids, source/destination key column positions
 * within the arm's output row, and default equality functions.
 */
static void
build_arms(GraphScanState * node, GraphScan * plan)
{
	int			nprops = node->nprops;

	node->arms = palloc0(sizeof(GraphScanArmData) * node->narms);

	for (int a = 0; a < node->narms; a++)
	{
		Oid			elemoid = list_nth_oid(plan->edge_element_oids, a);
		HeapTuple	etup;
		Form_pg_propgraph_element pge;
		GraphScanArmData *arm = &node->arms[a];

		etup = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(elemoid));
		if (!HeapTupleIsValid(etup))
			elog(ERROR, "cache lookup failed for property graph element %u", elemoid);
		pge = (Form_pg_propgraph_element) GETSTRUCT(etup);

		arm->arm_relid = pge->pgerelid;
		arm->arm_srcvertex = pge->pgesrcvertexid;
		arm->arm_dstvertex = pge->pgedestvertexid;
		arm->arm_src_first = nprops + 2;
		arm->arm_dst_first = nprops + 2 + node->max_nsrc;

		build_arm_keys(get_graph_element_key_columns(elemoid,
													 Anum_pg_propgraph_element_pgesrckey),
					   &arm->arm_nsrc, &arm->arm_srceq, &arm->arm_srccoll);
		build_arm_keys(get_graph_element_key_columns(elemoid,
													 Anum_pg_propgraph_element_pgedestkey),
					   &arm->arm_ndst, &arm->arm_dsteq, &arm->arm_dstcoll);

		ReleaseSysCache(etup);
	}
}

/*
 * Compile the default equality functions and collations for one side
 * (source or destination) of an edge element arm, from the key columns read
 * by get_graph_element_key_columns().
 */
static void
build_arm_keys(List *keys, int *nkeys, FmgrInfo **eq, Oid **colls)
{
	int			i = 0;

	*nkeys = list_length(keys);
	*eq = palloc(sizeof(FmgrInfo) * Max(list_length(keys), 1));
	*colls = palloc(sizeof(Oid) * Max(list_length(keys), 1));

	foreach_ptr(GraphElementKeyCol, kc, keys)
	{
		Oid			eqop = key_equality_operator(kc->typid);

		fmgr_info(get_opcode(eqop), &(*eq)[i]);
		(*colls)[i] = kc->collation;
		i++;
	}
}

/*
 * Fetch the next seed vertex from the enclosing nestloop (via the seed
 * PARAM_EXEC slots) and start a new depth-0 frame.  Returns false when there
 * is no (usable) seed; the scan is then exhausted.
 */
static bool
graph_fetch_seed(GraphScanState * node, GraphScan * plan)
{
	EState	   *estate = node->ss.ps.state;
	GraphDepthFrameData *fr = &node->frames[0];
	ListCell   *lc;
	int			k = 0;
	bool		hasnull = false;

	graph_reset(node);

	/* restart every depth's inner scan for the new seed */
	for (int d = 0; d < node->ndepths; d++)
		if (node->frames[d].inner_state != NULL)
			ExecReScan(node->frames[d].inner_state);

	fr->vid_elem = node->seed_elem;
	fr->vid_nkeys = list_length(node->seed_params);
	foreach(lc, node->seed_params)
	{
		Node	   *item = (Node *) lfirst(lc);
		Datum		value;
		bool		isnull;

		if (IsA(item, Param))
		{
			/* bound by the enclosing nestloop (see GraphScan.seed_params) */
			ParamExecData *prm =
				&estate->es_param_exec_vals[((Param *) item)->paramid];

			value = prm->value;
			isnull = prm->isnull;
		}
		else
		{
			/* constant-bound seed column (see GraphScan.seed_params) */
			Const	   *con = castNode(Const, item);

			value = con->constvalue;
			isnull = con->constisnull;
		}

		fr->vid[k] = value;
		fr->vidnull[k] = isnull;
		if (isnull)
			hasnull = true;
		k++;
	}

	/* a NULL seed matches nothing (SQL NULL semantics) */
	if (hasnull)
		return false;

	node->cur_depth = 0;
	node->seed_emitted = false;
	return true;
}

/*
 * Try to advance the traversal one edge from the current (innermost) frame,
 * backtracking when a frame is exhausted.  Returns false when the current
 * seed is exhausted (caller must fetch a new seed).
 */
static bool
graph_next(GraphScanState * node, GraphScan * plan)
{
	for (;;)
	{
		GraphDepthFrameData *fr = &node->frames[node->cur_depth];

		if (graph_step(node, plan, fr))
		{
			/* descended one edge; emit whenever the new depth is deep enough */
			if (node->cur_depth >= node->min_depth)
				return true;
			continue;			/* not deep enough yet; descend further */
		}

		/* this frame is exhausted: backtrack */
		if (node->cur_depth <= 0)
		{
			node->cur_depth = -1;	/* need a new seed */
			return false;
		}
		graph_backtrack(node);
	}
}

/*
 * Pull the next traversable edge from the given frame's inner scan.  Returns
 * true if a new depth was pushed onto the stack.
 */
static bool
graph_step(GraphScanState * node, GraphScan * plan,
		   GraphDepthFrameData * fr)
{
	TupleTableSlot *eslot;
	Oid			newelem;
	int			newnkeys;
	Datum	   *newvid = node->tmp_vid;
	bool	   *newnull = node->tmp_vidnull;
	Datum	   *newprops = node->tmp_props;
	bool	   *newpropsnull = node->tmp_propsnull;

	CHECK_FOR_INTERRUPTS();

	if (fr->inner_state == NULL)
		return false;

	/*
	 * The current frame already sits at (or beyond) the effective maximum
	 * depth: no further descent is allowed.  Its single row was emitted when
	 * this depth was pushed; further calls just exhaust the frame.
	 */
	if (node->cur_depth >= node->max_depth)
		return false;

	for (;;)
	{
		int			res;

		eslot = ExecProcNode(fr->inner_state);
		if (TupIsNull(eslot))
			return false;

		res = graph_try_edge(node, plan, fr, eslot, &newelem, &newnkeys, newvid,
							 newnull, newprops, newpropsnull);
		if (res != GRAPH_EDGE_NONE)
		{
			graph_push(node, newelem, newnkeys, newvid, newnull, newprops,
					   newpropsnull);
			return true;
		}
	}
}

/*
 * Check whether a candidate edge (one row of the inner plan) is traversable
 * from the current vertex, according to the hop's direction, and if so fill
 * the next vertex plus the edge's VLE property values.
 *
 * Returns GRAPH_EDGE_NONE / _FORWARD / _BOTH (the latter for an undirected
 * non-loop edge, whose reverse traversal is also valid and is deferred).
 */
static int
graph_try_edge(GraphScanState * node, GraphScan * plan,
			   GraphDepthFrameData * fr, TupleTableSlot *eslot,
			   Oid *newelem, int *newnkeys, Datum *newvid,
			   bool *newnull, Datum *eprops, bool *epropsnull)
{
	Oid			tbl;
	bool		isnull;
	int			armno;
	GraphScanArmData *arm;
	int			nprops = node->nprops;

	/* identify the edge element by its table OID */
	tbl = DatumGetObjectId(slot_getattr(eslot, nprops + 2, &isnull));
	armno = graph_find_arm(node, tbl);
	if (armno < 0)
		elog(ERROR, "graph scan encountered unknown edge element table %u", tbl);
	arm = &node->arms[armno];

	/* VLE property values of the edge (may be filtered out below) */
	for (int i = 0; i < nprops; i++)
	{
		eprops[i] = slot_getattr(eslot, i + 1, &epropsnull[i]);
	}

	switch (plan->direction)
	{
		case GRAPH_DIR_INCOMING:
			if (edge_key_matches(node, fr, eslot, arm, false))
			{
				*newelem = arm->arm_srcvertex;
				*newnkeys = arm->arm_nsrc;
				for (int i = 0; i < arm->arm_nsrc; i++)
					newvid[i] =
						slot_getattr(eslot, arm->arm_src_first + i + 1, &newnull[i]);
				return GRAPH_EDGE_FORWARD;
			}
			break;

		case GRAPH_DIR_UNDIRECTED:

			/*
			 * An undirected edge is traversable from the current vertex when
			 * it matches either endpoint: as the source it yields the
			 * destination as next vertex, as the destination it yields the
			 * source (the reverse traversal, generated here rather than with
			 * a deferred mechanism).
			 */
			if (edge_key_matches(node, fr, eslot, arm, true))
			{
				*newelem = arm->arm_dstvertex;
				*newnkeys = arm->arm_ndst;
				for (int i = 0; i < arm->arm_ndst; i++)
					newvid[i] =
						slot_getattr(eslot, arm->arm_dst_first + i + 1, &newnull[i]);
				return GRAPH_EDGE_FORWARD;
			}
			if (edge_key_matches(node, fr, eslot, arm, false))
			{
				*newelem = arm->arm_srcvertex;
				*newnkeys = arm->arm_nsrc;
				for (int i = 0; i < arm->arm_nsrc; i++)
					newvid[i] =
						slot_getattr(eslot, arm->arm_src_first + i + 1, &newnull[i]);
				return GRAPH_EDGE_FORWARD;
			}
			break;

		default:				/* GRAPH_DIR_OUTGOING */
			if (edge_key_matches(node, fr, eslot, arm, true))
			{
				*newelem = arm->arm_dstvertex;
				*newnkeys = arm->arm_ndst;
				for (int i = 0; i < arm->arm_ndst; i++)
					newvid[i] =
						slot_getattr(eslot, arm->arm_dst_first + i + 1, &newnull[i]);
				return GRAPH_EDGE_FORWARD;
			}
			break;
	}

	return GRAPH_EDGE_NONE;
}

/*
 * Compare the current vertex key values against the given side (source or
 * destination) key columns of a candidate edge.  The arm's source (resp.
 * destination) vertex element must equal the current vertex's element.
 */
static bool
edge_key_matches(GraphScanState * node, GraphDepthFrameData * fr,
				 TupleTableSlot *eslot, GraphScanArmData * arm,
				 bool issrc)
{
	int			n;
	int			first;
	Oid			elem;
	int			i;

	if (issrc)
	{
		n = arm->arm_nsrc;
		first = arm->arm_src_first;
		elem = arm->arm_srcvertex;
	}
	else
	{
		n = arm->arm_ndst;
		first = arm->arm_dst_first;
		elem = arm->arm_dstvertex;
	}

	if (fr->vid_elem != elem)
		return false;
	if (fr->vid_nkeys != n)
		return false;

	for (i = 0; i < n; i++)
	{
		Datum		edatum;
		bool		eisnull;
		FmgrInfo   *eq;
		Oid			eqcoll;

		edatum = slot_getattr(eslot, first + i + 1, &eisnull);
		if (eisnull || fr->vidnull[i])
			return false;

		if (issrc)
		{
			eq = &arm->arm_srceq[i];
			eqcoll = arm->arm_srccoll[i];
		}
		else
		{
			eq = &arm->arm_dsteq[i];
			eqcoll = arm->arm_dstcoll[i];
		}

		if (!DatumGetBool(FunctionCall2Coll(eq, eqcoll, edatum, fr->vid[i])))
			return false;
	}
	return true;
}

/* Arm index whose edge element table matches relid, or -1. */
static int
graph_find_arm(GraphScanState * node, Oid relid)
{
	for (int a = 0; a < node->narms; a++)
		if (node->arms[a].arm_relid == relid)
			return a;
	return -1;
}

/*
 * Push a new depth frame for a traversed edge.  Enforces max_graph_stack_depth
 * via a shared counter on the EState (summed over all active GraphScans).
 */
static void
graph_push(GraphScanState * node, Oid newelem, int newnkeys,
		   Datum *newvid, bool *newnull, Datum *eprops,
		   bool *epropsnull)
{
	int			d = node->cur_depth + 1;
	GraphDepthFrameData *nfr = &node->frames[d];
	EState	   *estate = node->ss.ps.state;

	estate->es_graph_stack_depth++;
	if (estate->es_graph_stack_depth > max_graph_stack_depth)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("exceeded maximum graph traversal depth"),
						errhint("Increase max_graph_stack_depth and retry, or try "
								"to remove the infinite loop")));

	nfr->vid_elem = newelem;
	nfr->vid_nkeys = newnkeys;
	memcpy(nfr->vid, newvid, sizeof(Datum) * newnkeys);
	memcpy(nfr->vidnull, newnull, sizeof(bool) * newnkeys);
	if (eprops != NULL)
	{
		memcpy(nfr->edge_props, eprops, sizeof(Datum) * node->nprops);
		memcpy(nfr->edge_propsnull, epropsnull, sizeof(bool) * node->nprops);
	}
	node->cur_depth = d;
}

/* Pop the innermost depth frame (called only for depth > 0). */
static void
graph_backtrack(GraphScanState * node)
{
	node->ss.ps.state->es_graph_stack_depth--;
	Assert(node->ss.ps.state->es_graph_stack_depth >= 0);
	node->cur_depth--;
}

/* Pop all active frames; the scan becomes ready for a new seed. */
static void
graph_reset(GraphScanState * node)
{
	for (; node->cur_depth > 0; node->cur_depth--)
	{
		node->ss.ps.state->es_graph_stack_depth--;
		(void) 0;
	}
	Assert(node->ss.ps.state->es_graph_stack_depth >= 0);
	node->cur_depth = -1;
	node->seed_emitted = false;
}

static TupleTableSlot *
ExecGraphScan(PlanState *pstate)
{
	GraphScanState *node = castNode(GraphScanState, pstate);
	GraphScan  *plan = castNode(GraphScan, pstate->plan);
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (node->cur_depth < 0)
		{
			/*
			 * Fetch a seed vertex from the enclosing nestloop (via the seed
			 * PARAM_EXEC slots), but only once per (re)scan: after the
			 * current seed has been fully traversed, keep returning NULL
			 * until ExecReScan() re-arms us for the next outer row.
			 */
			if (!node->need_seed)
				return NULL;
			node->need_seed = false;
			if (!graph_fetch_seed(node, plan))
				return NULL;
		}

		/* emit the zero-hop seed row first, when applicable */
		if (!node->seed_emitted && node->min_depth == 0)
		{
			TupleTableSlot *res;

			node->seed_emitted = true;
			res = graph_emit_row(node, slot);
			if (res != NULL)
				return res;
			/* else drop and keep traversing */
		}

		/* try to descend along another edge (or backtrack) */
		if (graph_next(node, plan))
		{
			TupleTableSlot *res = graph_emit_row(node, slot);

			if (res != NULL)
				return res;
		}
	}
}

/*
 * Fill the scan's output slot for the path currently on the stack: seed
 * keys (frame 0), terminal keys (innermost frame), and VLE edge-list arrays.
 */
static void
graph_build_row(GraphScanState * node, TupleTableSlot *slot)
{
	GraphScan  *plan = castNode(GraphScan, node->ss.ps.plan);
	GraphDepthFrameData *seedfr = &node->frames[0];
	GraphDepthFrameData *endfr = &node->frames[node->cur_depth];
	int			amp;
	MemoryContext oldcxt;

	ExecClearTuple(slot);

	amp = 0;
	foreach_int(attno, plan->seed_key_cols)
	{
		slot->tts_values[attno - 1] = seedfr->vid[amp];
		slot->tts_isnull[attno - 1] = seedfr->vidnull[amp];
		amp++;
	}

	amp = 0;
	foreach_int(attno, plan->terminal_key_cols)
	{
		slot->tts_values[attno - 1] = endfr->vid[amp];
		slot->tts_isnull[attno - 1] = endfr->vidnull[amp];
		amp++;
	}

	/* Build the VLE edge-list arrays in the per-tuple context. */
	oldcxt =
		MemoryContextSwitchTo(node->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	amp = 0;
	foreach_int(attno, plan->edge_list_cols)
	{
		slot->tts_values[attno - 1] = graph_build_edge_array(node, slot, amp);
		slot->tts_isnull[attno - 1] = false;
		amp++;
	}
	MemoryContextSwitchTo(oldcxt);

	ExecStoreVirtualTuple(slot);
}

/*
 * Build and emit the current path's row: fill the scan slot, then apply the
 * scan qual and projection.  Returns the resulting table slot, or NULL when
 * the row failed the qual (the caller must keep traversing).
 */
static TupleTableSlot *
graph_emit_row(GraphScanState *node, TupleTableSlot *slot)
{
	graph_build_row(node, slot);
	node->ss.ps.ps_ExprContext->ecxt_scantuple = slot;
	if (node->ss.ps.qual == NULL ||
		ExecQual(node->ss.ps.qual, node->ss.ps.ps_ExprContext))
	{
		if (node->ss.ps.ps_ProjInfo != NULL)
			return ExecProject(node->ss.ps.ps_ProjInfo);
		return slot;
	}
	return NULL;
}

/*
 * Build the VLE edge-list array for property pi: the property's value over
 * every traversed edge of the current path, in traversal order; an empty
 * array when the path has no edges.
 */
static Datum
graph_build_edge_array(GraphScanState * node, TupleTableSlot *slot,
					   int pi)
{
	GraphScan  *plan = castNode(GraphScan, node->ss.ps.plan);
	Oid			arrtype;
	Oid			elemtype;
	ArrayBuildState *astate = NULL;

	arrtype = TupleDescAttr(slot->tts_tupleDescriptor,
							list_nth_int(plan->edge_list_cols, pi) - 1)
		->atttypid;
	elemtype = get_element_type(arrtype);
	if (elemtype == InvalidOid)
		elog(ERROR, "graph scan edge-list column %d is not an array", pi + 1);

	for (int d = 1; d <= node->cur_depth; d++)
	{
		GraphDepthFrameData *fr = &node->frames[d];

		astate =
			accumArrayResult(astate, fr->edge_props[pi], fr->edge_propsnull[pi],
							 elemtype, CurrentMemoryContext);
	}

	if (astate == NULL)
		return PointerGetDatum(construct_empty_array(elemtype));

	return makeArrayResult(astate, CurrentMemoryContext);
}

GraphScanState *
ExecInitGraphScan(GraphScan * node, EState *estate, int eflags)
{
	GraphScanState *scanstate;
	int			maxwidth;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * GraphScan should not have any "normal" children
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(GraphScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecGraphScan;

	ExecAssignExprContext(estate, &scanstate->ss.ps);

	scanstate->min_depth = node->min_depth;
	scanstate->max_depth = node->max_depth;
	scanstate->seed_elem = node->seed_elem_oid;
	scanstate->nprops = list_length(node->edge_list_cols);
	scanstate->max_nsrc = node->max_nsrc;
	scanstate->max_ndst = node->max_ndst;
	scanstate->seed_params = node->seed_params;
	scanstate->narms = list_length(node->edge_element_oids);
	scanstate->cur_depth = -1;
	scanstate->need_seed = true;
	scanstate->seed_emitted = false;

	/*
	 * Effective maximum depth.  Explicit bounds are honored; unbounded (or
	 * absurdly large) ones are clamped to max_graph_stack_depth + 1 so that
	 * the traversal-depth guard below fires instead of looping forever.
	 */
	if (node->max_depth < 0 || node->max_depth > max_graph_stack_depth)
		scanstate->max_depth = max_graph_stack_depth + 1;
	scanstate->ndepths = scanstate->max_depth + 1;
	scanstate->frames = palloc0(sizeof(GraphDepthFrameData) * scanstate->ndepths);

	/* Compile the per-arm edge element metadata. */
	build_arms(scanstate, node);

	/*
	 * Initialize the scan slot.  There is no heap relation to describe it, so
	 * build a virtual slot whose descriptor comes from the scan's own
	 * targetlist; the result slot is built from the targetlist by
	 * ExecInitResultTypeTL.
	 */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  ExecTypeFromTL(node->graph_columns), &TTSOpsVirtual, 0);
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * Build the depth frames: every frame owns a copy of the inner (1-hop)
	 * expansion plan so that each frame's scan cursor is independent.
	 */
	maxwidth = Max(list_length(node->seed_params),
				   Max(node->max_nsrc, node->max_ndst));
	scanstate->tmp_vid = palloc(sizeof(Datum) * Max(maxwidth, 1));
	scanstate->tmp_vidnull = palloc(sizeof(bool) * Max(maxwidth, 1));
	scanstate->tmp_props = palloc(sizeof(Datum) * Max(scanstate->nprops, 1));
	scanstate->tmp_propsnull = palloc(sizeof(bool) * Max(scanstate->nprops, 1));

	for (int d = 0; d < scanstate->ndepths; d++)
	{
		GraphDepthFrameData *fr = &scanstate->frames[d];

		fr->vid = palloc(sizeof(Datum) * Max(maxwidth, 1));
		fr->vidnull = palloc(sizeof(bool) * Max(maxwidth, 1));
		fr->edge_props = palloc(sizeof(Datum) * Max(scanstate->nprops, 1));
		fr->edge_propsnull = palloc(sizeof(bool) * Max(scanstate->nprops, 1));

		if (node->inner_plan != NULL)
			fr->inner_state =
				ExecInitNode(copyObject(node->inner_plan), estate, eflags);
		else
			fr->inner_state = NULL;
	}

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	return scanstate;
}

void
ExecEndGraphScan(GraphScanState * node)
{
	graph_reset(node);

	for (int d = 0; d < node->ndepths; d++)
		if (node->frames[d].inner_state != NULL)
			ExecEndNode(node->frames[d].inner_state);
}

void
ExecReScanGraphScan(GraphScanState * node)
{
	graph_reset(node);
	node->need_seed = true;

	if (node->ss.ps.chgParam != NULL)
	{
		for (int d = 0; d < node->ndepths; d++)
			if (node->frames[d].inner_state != NULL)
				UpdateChangedParamSet(node->frames[d].inner_state,
									  node->ss.ps.chgParam);
	}
}
