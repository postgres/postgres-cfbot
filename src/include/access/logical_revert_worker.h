/*-------------------------------------------------------------------------
 *
 * logical_revert_worker.h
 *	  Background worker for timer-driven Logical Revert via ATM scan
 *
 * The Logical Revert worker periodically scans the ATM (Aborted Transaction
 * Map) for entries whose UNDO chains have not yet been applied, opens the
 * target relation, applies the UNDO chain via the per-AM apply callback,
 * marks the ATM entry as reverted, emits an XLOG_ATM_FORGET WAL record, and
 * removes the entry from the ATM.
 *
 * This worker is timer-driven (periodic scan) rather than event-driven
 * (queue-based).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/logical_revert_worker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICAL_REVERT_WORKER_H
#define LOGICAL_REVERT_WORKER_H

#include "postgres.h"

/* Shared memory sizing and initialization */
extern Size LogicalRevertShmemSize(void);
extern void LogicalRevertShmemInit(void);

/* Worker entry points */
extern void LogicalRevertWorkerMain(Datum main_arg);

/* Launch a logical revert worker for a specific database */
extern void StartLogicalRevertWorker(Oid dboid);
extern void LogicalRevertLauncherMain(Datum main_arg);
extern void LogicalRevertLauncherRegister(void);

/* GUC parameters */
extern int	logical_revert_naptime;
extern int	max_logical_revert_workers;

#endif							/* LOGICAL_REVERT_WORKER_H */
