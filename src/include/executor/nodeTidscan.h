/*-------------------------------------------------------------------------
 *
 * nodeTidscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeTidscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODETIDSCAN_H
#define NODETIDSCAN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern TidScanState *ExecInitTidScan(TidScan *node, EState *estate, int eflags);
extern void ExecEndTidScan(TidScanState *node);
extern void ExecReScanTidScan(TidScanState *node);

/* instrument support */
extern void ExecTidScanInstrumentEstimate(TidScanState *node,
										  ParallelContext *pcxt);
extern void ExecTidScanInstrumentInitDSM(TidScanState *node,
										 ParallelContext *pcxt);
extern void ExecTidScanInstrumentInitWorker(TidScanState *node,
											ParallelWorkerContext *pwcxt);
extern void ExecTidScanRetrieveInstrumentation(TidScanState *node);

#endif							/* NODETIDSCAN_H */
