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

extern GraphScanState * ExecInitGraphScan(GraphScan * node, EState *estate, int eflags);
extern void ExecEndGraphScan(GraphScanState * node);
extern void ExecReScanGraphScan(GraphScanState * node);

#endif							/* NODEGRAPHSCAN_H */
