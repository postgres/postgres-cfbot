/*-------------------------------------------------------------------------
 *
 * undoworker.h
 *	  Exports from undoworker.c.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undoworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PBU_UNDOWORKER_H
#define PBU_UNDOWORKER_H

/* GUC options */
/* undo worker sleep time between rounds */
extern int	UndoWorkerDelay;

extern Size UndoLauncherShmemSize(void);
extern void UndoLauncherShmemRequest(void);
extern void UndoLauncherShmemInit(void);
extern void UndoLauncherRegister(void);
extern void PbuEnsureUndoLauncher(void);
extern void UndoLauncherMain(Datum main_arg);
pg_noreturn extern void PbuUndoWorkerMain(Datum main_arg);
extern void WakeupUndoWorker(Oid dbid);

#endif							/* PBU_UNDOWORKER_H */
