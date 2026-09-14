/*-------------------------------------------------------------------------
 *
 * undoworker.h
 *	  UNDO worker background process
 *
 * The UNDO worker is a background process that periodically scans active
 * transactions and discards UNDO records that are no longer needed.
 * This reclaims space in UNDO logs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undoworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDOWORKER_H
#define UNDOWORKER_H

#include "access/transam.h"
#include "access/undolog.h"
#include "fmgr.h"
#include "storage/lwlock.h"
#include "storage/procnumber.h"
#include "storage/shmem.h"

/*
 * UndoWorkerShmemData - Shared memory for UNDO worker coordination
 *
 * This structure tracks the state of UNDO discard operations and
 * coordinates between the worker and other backends.
 */
typedef struct UndoWorkerShmemData
{
	LWLock		lock;			/* Protects this structure */

	pg_atomic_uint64 last_discard_time; /* Last discard operation time */
	TransactionId oldest_xid_checked;	/* Last XID used for discard */
	UndoRecPtr	last_discard_ptr;	/* Last UNDO pointer discarded */

	int			naptime_ms;		/* Current sleep time in ms */
	bool		shutdown_requested; /* Worker should exit */

	/* Rotation coordination fields */
	ProcNumber	worker_proc;	/* For latch-based wakeup */
	pg_atomic_uint32 sealed_log_count;	/* Number of SEALED logs pending */
} UndoWorkerShmemData;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
StaticAssertDecl(offsetof(UndoWorkerShmemData, last_discard_time) % 8 == 0,
				 "UndoWorkerShmemData.last_discard_time must be eight-byte aligned");
#endif

/* GUC parameters */
extern int	undo_worker_naptime;

/* Shared memory functions */
extern Size UndoWorkerShmemSize(void);
extern void UndoWorkerShmemInit(void);

/* Worker lifecycle functions */
pg_noreturn extern void UndoWorkerMain(Datum main_arg);
extern void UndoWorkerRegister(void);

/* Utility functions */
extern TransactionId UndoWorkerGetOldestXid(void);
extern void UndoWorkerRequestShutdown(void);
extern void WakeUndoDiscardWorker(void);

#endif							/* UNDOWORKER_H */
