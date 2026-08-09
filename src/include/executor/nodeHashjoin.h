/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.h
 *	  prototypes for nodeHashjoin.c
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeHashjoin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEHASHJOIN_H
#define NODEHASHJOIN_H

#include "access/parallel.h"
#include "nodes/execnodes.h"
#include "storage/buffile.h"

extern HashJoinState *ExecInitHashJoin(HashJoin *node, EState *estate, int eflags);
extern void ExecEndHashJoin(HashJoinState *node);
extern void ExecReScanHashJoin(HashJoinState *node);
extern void ExecShutdownHashJoin(HashJoinState *node);
extern void ExecHashJoinEstimate(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinInitializeDSM(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinReInitializeDSM(HashJoinState *state, ParallelContext *pcxt);
extern void ExecHashJoinInitializeWorker(HashJoinState *state,
										 ParallelWorkerContext *pwcxt);

extern void ExecHashJoinSaveTuple(MinimalTuple tuple, uint32 hashvalue,
								  BufFile **fileptr, HashJoinTable hashtable);

/*
 * Bloom filter pushdown producer-side helper (see nodeHashjoin.c).
 *
 * ExecBloomFilters and ExecInitBloomFilters live in executor.h so that
 * scan nodes can call them from ExecInit without having to pull in
 * hashjoin internals.
 */
extern void ExecRegisterBloomFilterProducer(HashJoinState *hjstate);

#endif							/* NODEHASHJOIN_H */
