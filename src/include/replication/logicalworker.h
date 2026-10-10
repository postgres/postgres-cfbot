/*-------------------------------------------------------------------------
 *
 * logicalworker.h
 *	  Exports for logical replication workers.
 *
 * Portions Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALWORKER_H
#define LOGICALWORKER_H

#include <signal.h>

extern PGDLLIMPORT volatile sig_atomic_t ParallelApplyMessagePending;

extern void ApplyWorkerMain(Datum main_arg);
extern void ParallelApplyWorkerMain(Datum main_arg);
extern void TableSyncWorkerMain(Datum main_arg);
extern void SequenceSyncWorkerMain(Datum main_arg);

extern bool IsLogicalWorker(void);
extern bool IsLogicalParallelApplyWorker(void);

/*
 * Accessor for the cached hot_indexed_on_apply mode of the current apply
 * worker's subscription.  Returns a LOGICALREP_HOT_INDEXED_* code (see
 * catalog/pg_subscription.h).  Non-apply processes always see
 * LOGICALREP_HOT_INDEXED_OFF.
 */
extern char GetHotIndexedApplyMode(void);

extern void HandleParallelApplyMessageInterrupt(void);
extern void ProcessParallelApplyMessages(void);

extern void LogicalRepWorkersWakeupAtCommit(Oid subid);

extern void AtEOXact_LogicalRepWorkers(bool isCommit);

#endif							/* LOGICALWORKER_H */
