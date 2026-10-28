/*-------------------------------------------------------------------------
 *
 * nodeSamplescan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeSamplescan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODESAMPLESCAN_H
#define NODESAMPLESCAN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"

extern SampleScanState *ExecInitSampleScan(SampleScan *node, EState *estate, int eflags);
extern void ExecEndSampleScan(SampleScanState *node);
extern void ExecReScanSampleScan(SampleScanState *node);

/* instrument support */
extern void ExecSampleScanInstrumentEstimate(SampleScanState *node,
											 ParallelContext *pcxt);
extern void ExecSampleScanInstrumentInitDSM(SampleScanState *node,
											ParallelContext *pcxt);
extern void ExecSampleScanInstrumentInitWorker(SampleScanState *node,
											   ParallelWorkerContext *pwcxt);
extern void ExecSampleScanRetrieveInstrumentation(SampleScanState *node);

#endif							/* NODESAMPLESCAN_H */
