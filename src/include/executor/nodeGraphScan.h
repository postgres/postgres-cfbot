/*-------------------------------------------------------------------------
 *
 * nodeGraphScan.h
 *	  prototypes for nodeGraphScan.c
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeGraphScan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEGRAPHSCAN_H
#define NODEGRAPHSCAN_H

#include "nodes/execnodes.h"

typedef struct FmgrInfo FmgrInfo;

extern GraphScanState * ExecInitGraphScan(GraphScan * node, EState *estate, int eflags);
extern void ExecEndGraphScan(GraphScanState * node);
extern void ExecReScanGraphScan(GraphScanState * node);

/*
 * One compiled edge element arm of the GraphScan's inner 1-hop expansion.
 * The inner plan is a UNION ALL (or a single relation) over the arms; each
 * row carries, in this order:
 *
 *	  0 .. nprops-1                 VLE edge-list property values
 *	  nprops                       the edge's ctid (TIDOID)
 *	  nprops+1                     the edge element table OID
 *	  nprops+2 .. +max_nsrc-1      the edge's source key columns (padded)
 *	  nprops+2+max_nsrc .. +max_ndst-1  the edge's destination key columns
 *
 * An edge is traversable from the current vertex when the arm's source
 * vertex element equals the current vertex's element and, given that, each
 * source key column equals the current vertex's key value (compared with
 * the default equality operator of the source key column's datatype).
 * arm_src_first / arm_dst_first are 0-based row offsets of the groups.
 */
typedef struct GraphScanArmData
{
	Oid			arm_relid;		/* edge element table */
	Oid			arm_srcvertex;	/* source vertex element */
	Oid			arm_dstvertex;	/* destination vertex element */

	int			arm_nsrc;		/* source key width */
	int			arm_ndst;		/* destination key width */

	int			arm_src_first;	/* 0-based row offset of the src key group */
	int			arm_dst_first;	/* 0-based row offset of the dst key group */

	FmgrInfo   *arm_srceq;		/* [arm_nsrc] default equality fmgr */
	Oid		   *arm_srccoll;	/* [arm_nsrc] column collations */
	FmgrInfo   *arm_dsteq;		/* [arm_ndst] default equality fmgr */
	Oid		   *arm_dstcoll;	/* [arm_ndst] column collations */
}			GraphScanArmData;

/*
 * One depth frame of the DFS: a copy of the inner 1-hop expansion plan,
 * plus the vertex reached at this depth and the VLE property values of the
 * edge that led here.
 *
 * frames[0] holds the seed vertex (no edge); frames[d] (d >= 1) holds the
 * vertex reached after traversing the d-th edge of the current path.  The
 * inner scan state of a frame acts as a cursor: it is only started (rescan)
 * when the frame is first pushed, and never again, so backtracking resumes
 * the parent's scan exactly where it left off.
 */
typedef struct GraphDepthFrameData
{
	PlanState  *inner_state;	/* own copy of the inner 1-hop expansion */

	/*
	 * True until the frame's inner scan has been (re)started for the current
	 * vertex: the executor (re)initializes or rescans it the first time the
	 * frame is stepped after a push or a new seed, when the current-vertex
	 * PARAM_EXEC parameters are bound.  Parameterized index scans only
	 * re-evaluate their scan keys on (re)scan, so restarting like this is
	 * what keeps them in sync with the vertex.
	 */
	bool		need_init;

	Oid			vid_elem;		/* vertex element of the current vertex */
	int			vid_nkeys;		/* key width of the current vertex */
	Datum	   *vid;			/* current vertex key values */
	bool	   *vidnull;

	Datum	   *edge_props;		/* [nprops] VLE property values of the edge
								 * into this frame (frame 0: unused) */
	bool	   *edge_propsnull;

}			GraphDepthFrameData;

#endif							/* NODEGRAPHSCAN_H */
