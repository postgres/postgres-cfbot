/*-------------------------------------------------------------------------
 *
 * execRPR.h
 *	  prototypes for execRPR.c (NFA-based Row Pattern Recognition engine)
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/execRPR.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECRPR_H
#define EXECRPR_H

#include "nodes/execnodes.h"

/* NFA context management */
extern RPRNFAContext *ExecRPRStartContext(WindowAggState *winstate,
										  int64 startPos);
extern RPRNFAContext *ExecRPRGetHeadContext(WindowAggState *winstate,
											int64 pos);
extern void ExecRPRFreeContext(WindowAggState *winstate, RPRNFAContext *ctx);

/* NFA processing */
extern void ExecRPRProcessRow(WindowAggState *winstate, int64 currentPos,
							  bool hasLimitedFrame, int64 frameOffset);
extern void ExecRPRCleanupDeadContexts(WindowAggState *winstate,
									   RPRNFAContext *excludeCtx);
extern void ExecRPRFinalizeAllContexts(WindowAggState *winstate, int64 lastPos);

/* NFA statistics */
extern void ExecRPRRecordContextSuccess(WindowAggState *winstate,
										int64 matchLen);
extern void ExecRPRRecordContextFailure(WindowAggState *winstate,
										int64 failedLen);

#endif							/* EXECRPR_H */
