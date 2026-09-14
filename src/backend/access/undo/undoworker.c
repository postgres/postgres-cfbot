/*-------------------------------------------------------------------------
 *
 * undoworker.c
 *	  UNDO worker background process implementation
 *
 * The UNDO worker periodically discards old UNDO records that are no
 * longer needed by any active transaction. This is essential for
 * preventing unbounded growth of UNDO logs.
 *
 * The worker also advances the undo_discard_horizon, allowing WAL
 * segments containing fully-discarded UNDO batches to be recycled.
 *
 * Design based on ZHeap's UNDO worker and PostgreSQL's autovacuum
 * launcher patterns.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undoworker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <setjmp.h>
#include <unistd.h>

#include "access/undolog.h"
#include "access/undorecord.h"
#include "access/undormgr.h"
#include "access/undoworker.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/aio_subsys.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

/* Shared memory state */
static UndoWorkerShmemData *UndoWorkerShmem = NULL;

/* Adaptive sleep: use shorter interval when sealed logs are pending */
#define UNDO_WORKER_FAST_NAPTIME_MS		200

/* Forward declarations */
static void undo_worker_sighup(SIGNAL_ARGS);
static void undo_worker_sigterm(SIGNAL_ARGS);
static void perform_undo_discard(void);

/*
 * UndoWorkerShmemSize - Calculate shared memory needed
 */
Size
UndoWorkerShmemSize(void)
{
	return sizeof(UndoWorkerShmemData);
}

/*
 * UndoWorkerShmemInit - Initialize shared memory
 */
void
UndoWorkerShmemInit(void)
{
	bool		found;

	UndoWorkerShmem = (UndoWorkerShmemData *)
		ShmemInitStruct("UNDO Worker Data",
						UndoWorkerShmemSize(),
						&found);

	if (!found)
	{
		LWLockInitialize(&UndoWorkerShmem->lock,
						 LWTRANCHE_UNDO_LOG);

		pg_atomic_init_u64(&UndoWorkerShmem->last_discard_time, 0);
		UndoWorkerShmem->oldest_xid_checked = InvalidTransactionId;
		UndoWorkerShmem->last_discard_ptr = InvalidUndoRecPtr;
		UndoWorkerShmem->naptime_ms = undo_worker_naptime;
		UndoWorkerShmem->shutdown_requested = false;

		/* Rotation coordination fields */
		UndoWorkerShmem->worker_proc = INVALID_PROC_NUMBER;
		pg_atomic_init_u32(&UndoWorkerShmem->sealed_log_count, 0);
	}
}

/*
 * undo_worker_sighup - SIGHUP handler
 */
static void
undo_worker_sighup(SIGNAL_ARGS)
{
	(void) postgres_signal_arg; /* unused */
	ConfigReloadPending = true;
	SetLatch(MyLatch);
}

/*
 * undo_worker_sigterm - SIGTERM handler
 */
static void
undo_worker_sigterm(SIGNAL_ARGS)
{
	(void) postgres_signal_arg; /* unused */
	UndoWorkerShmem->shutdown_requested = true;
	SetLatch(MyLatch);
}

/*
 * WakeUndoDiscardWorker
 *		Wake the UNDO discard worker via its latch.
 *
 * Follows the WAL writer wakeup pattern: read the worker's ProcNumber
 * and set its latch to interrupt the WaitLatch sleep.  Safe to call
 * from any backend, including during allocation pressure.
 */
void
WakeUndoDiscardWorker(void)
{
	ProcNumber	proc;

	if (UndoWorkerShmem == NULL)
		return;

	proc = UndoWorkerShmem->worker_proc;
	if (proc != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(proc)->procLatch);
}

/*
 * UndoWorkerGetOldestXid - Get oldest transaction still needing UNDO
 *
 * Returns the oldest transaction ID that is still active across all
 * databases.  Any UNDO records created by transactions older than this
 * can be safely discarded, because those transactions have already
 * committed or aborted and their UNDO is no longer needed.
 *
 * We use GetOldestActiveTransactionId() from procarray.c which properly
 * acquires ProcArrayLock and scans all backends.  We pass allDbs=true
 * because UNDO logs are not per-database -- a single UNDO log may
 * contain records for multiple databases.
 *
 * Returns InvalidTransactionId if there are no active transactions,
 * meaning all UNDO records can potentially be discarded (subject to
 * retention policy).
 */
TransactionId
UndoWorkerGetOldestXid(void)
{
	TransactionId oldest_xid;

	/*
	 * Don't attempt the scan during recovery -- the UNDO worker should not be
	 * running in that case, but guard defensively.
	 */
	if (RecoveryInProgress())
		return InvalidTransactionId;

	/*
	 * GetOldestActiveTransactionId scans ProcArray under ProcArrayLock
	 * (LW_SHARED) and returns the smallest XID among all active backends. We
	 * pass inCommitOnly=false (we want all active XIDs, not just those in
	 * commit critical section) and allDbs=true (UNDO spans all databases).
	 */
	oldest_xid = GetOldestActiveTransactionId(false, true);

	return oldest_xid;
}

/*
 * perform_undo_discard - Main discard logic
 *
 * Two passes:
 *   First pass: Update discard pointers for all in-use logs based on
 *            the oldest active transaction ID.
 *   Second pass: Scan SEALED/DISCARDABLE logs and manage lifecycle
 *            transitions: SEALED -> DISCARDABLE -> FREE.
 */
static void
perform_undo_discard(void)
{
	TransactionId oldest_xid;
	UndoRecPtr	oldest_undo_ptr;
	TimestampTz current_time;
	int			i;
	int			freed_count = 0;

	/* Get oldest active transaction */
	oldest_xid = UndoWorkerGetOldestXid();

	if (!TransactionIdIsValid(oldest_xid))
	{
		/* No active transactions, can discard all UNDO */
		oldest_xid = ReadNextTransactionId();
	}

	current_time = GetCurrentTimestamp();

	/*
	 * Scan per-backend UNDO batch LSN slots and clear any that belong to dead
	 * backends.  A backend that was SIGKILLed (or otherwise exited without
	 * calling AtProcExit) will leave its slot occupied, which pins the WAL
	 * discard horizon indefinitely.  We detect dead backends by checking
	 * ProcGlobal->allProcs[i].pid == 0, which indicates the slot is not in
	 * use by a live process (pid 0 also indicates prepared-xact dummy
	 * PGPROCs, but those do not write UNDO data).
	 */
	for (i = 0; i < MaxBackends; i++)
	{
		XLogRecPtr	slot_lsn;

		slot_lsn = (XLogRecPtr)
			pg_atomic_read_u64(&UndoLogShared->backend_undo_lsns[i]);

		if (!XLogRecPtrIsValid(slot_lsn))
			continue;

		if (GetPGProcByNumber(i)->pid == 0)
		{
			pg_atomic_write_u64(&UndoLogShared->backend_undo_lsns[i],
								(uint64) InvalidXLogRecPtr);
			ereport(DEBUG2,
					(errmsg("UNDO worker: cleared stale batch LSN for dead backend slot %d", i)));
		}
	}

	/*
	 * First pass: For each UNDO log, determine what can be discarded.  We
	 * need to respect the retention_time setting to allow point-in-time
	 * recovery.
	 */
	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (!log->in_use)
			continue;

		/*
		 * Calculate the oldest UNDO pointer that must be retained. This is
		 * based on: 1. The oldest active transaction 2. The retention time
		 * setting
		 */
		LWLockAcquire(&log->lock, LW_SHARED);

		if (TransactionIdIsValid(log->oldest_xid) &&
			TransactionIdPrecedes(log->oldest_xid, oldest_xid))
		{
			/* This log has UNDO that can be discarded */
			oldest_undo_ptr = pg_atomic_read_u64(&log->insert_ptr);

			LWLockRelease(&log->lock);

			/* Update discard pointer */
			UndoLogDiscard(oldest_undo_ptr);

			/* Update cumulative discard counter */
			pg_atomic_fetch_add_u64(&UndoLogShared->total_discarded,
									UndoRecPtrGetOffset(oldest_undo_ptr));

			ereport(DEBUG2,
					(errmsg("UNDO worker: discarded log %u up to %llu",
							log->log_number,
							(unsigned long long) oldest_undo_ptr)));
		}
		else
		{
			LWLockRelease(&log->lock);
		}
	}

	/*
	 * Second pass: Manage lifecycle transitions for SEALED and DISCARDABLE
	 * logs.
	 *
	 * SEALED logs whose discard_ptr >= seal_ptr have had all their records
	 * discarded and can transition to DISCARDABLE.  DISCARDABLE logs can have
	 * their slot freed and segment file deleted.
	 */
	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (!log->in_use)
			continue;

		LWLockAcquire(&log->lock, LW_EXCLUSIVE);

		if (log->state == UNDO_LOG_SEALED)
		{
			UndoRecPtr	seal = pg_atomic_read_u64(&log->seal_ptr);
			UndoRecPtr	discard = log->discard_ptr;

			if (UndoRecPtrIsValid(seal) &&
				UndoRecPtrGetOffset(discard) >= UndoRecPtrGetOffset(seal))
			{
				/* All records discarded -- transition to DISCARDABLE */
				log->state = UNDO_LOG_DISCARDABLE;
				ereport(DEBUG1,
						(errmsg("UNDO worker: log %u transitioned to DISCARDABLE",
								log->log_number)));
			}
		}

		if (log->state == UNDO_LOG_DISCARDABLE)
		{
			uint32		log_number = log->log_number;

			/* Free the slot */
			log->in_use = false;
			log->state = UNDO_LOG_FREE;
			log->log_number = 0;
			pg_atomic_write_u64(&log->insert_ptr, InvalidUndoRecPtr);
			log->discard_ptr = InvalidUndoRecPtr;
			log->oldest_xid = InvalidTransactionId;
			pg_atomic_write_u64(&log->seal_ptr, InvalidUndoRecPtr);
			log->sealed_time = 0;

			LWLockRelease(&log->lock);

			/* Delete the segment file outside the lock */
			UndoLogDeleteSegmentFile(log_number);

			/* Decrement sealed log count */
			pg_atomic_fetch_sub_u32(&UndoWorkerShmem->sealed_log_count, 1);

			freed_count++;
			continue;
		}

		LWLockRelease(&log->lock);
	}

	if (freed_count > 0)
		ereport(LOG,
				(errmsg("UNDO worker: freed %d discardable log segment(s)",
						freed_count)));

	/*
	 * Advance the WAL discard horizon so KeepLogSeg() can allow recycling of
	 * WAL segments no longer needed for UNDO rollback.
	 *
	 * UndoGetOldestBatchLSN() scans per-backend slots and returns the minimum
	 * first-batch LSN across all active transactions that have written UNDO
	 * data.  WAL before this LSN cannot be recycled.
	 *
	 * If no backend has in-flight UNDO data the function returns
	 * InvalidXLogRecPtr, meaning there is no UNDO-imposed WAL retention
	 * requirement.  We do not call UndoSetDiscardHorizon in that case because
	 * an invalid horizon is already the "no constraint" sentinel.
	 */
	{
		XLogRecPtr	new_horizon = UndoGetOldestBatchLSN();

		if (XLogRecPtrIsValid(new_horizon))
			UndoSetDiscardHorizon(new_horizon);

		/*
		 * If undo_max_wal_retention_size is set, warn when the retained WAL
		 * distance between the current write position and the UNDO discard
		 * horizon exceeds the configured limit.  This helps operators detect
		 * long-running transactions that prevent WAL recycling.
		 */
		if (undo_max_wal_retention_size > 0 && XLogRecPtrIsValid(new_horizon))
		{
			XLogRecPtr	write_ptr = GetXLogWriteRecPtr();

			if (write_ptr > new_horizon)
			{
				uint64		retained_mb = (write_ptr - new_horizon) >> 20;

				if (retained_mb > (uint64) undo_max_wal_retention_size)
					ereport(WARNING,
							(errmsg("UNDO WAL retention (%lu MB) exceeds undo_max_wal_retention_size (%d MB)",
									(unsigned long) retained_mb, undo_max_wal_retention_size),
							 errhint("Investigate long-running transactions or increase undo_max_wal_retention_size.")));
			}
		}
	}

	/* Record this discard operation */
	LWLockAcquire(&UndoWorkerShmem->lock, LW_EXCLUSIVE);
	pg_atomic_write_u64(&UndoWorkerShmem->last_discard_time,
						(uint64) current_time);
	UndoWorkerShmem->oldest_xid_checked = oldest_xid;
	LWLockRelease(&UndoWorkerShmem->lock);
}

/*
 * UndoWorkerMain - Main loop for UNDO worker
 *
 * This is the entry point for the UNDO worker background process.
 * It runs continuously, waking periodically to discard old UNDO.
 *
 * Uses adaptive sleep: when sealed logs are pending cleanup, the worker
 * wakes more frequently (200ms) to process them promptly.  Otherwise
 * it uses the configured undo_worker_naptime.
 */
void
UndoWorkerMain(Datum main_arg)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext undo_worker_context;

	(void) main_arg;			/* unused */

	/* Establish signal handlers */
	pqsignal(SIGHUP, undo_worker_sighup);
	pqsignal(SIGTERM, undo_worker_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/*
	 * Connect with no specific database so the worker is a stats-reporting
	 * backend (visible in pg_stat_activity as 'undo worker') while not
	 * pinning any database against DROP.  Discard operates on cluster-wide
	 * UNDO logs, so it needs no per-database catalog access.
	 */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	/* Register our ProcNumber for latch-based wakeup by other backends */
	UndoWorkerShmem->worker_proc = MyProcNumber;

	/* Initialize worker state */
	ereport(LOG,
			(errmsg("UNDO worker started")));

	/*
	 * Create a memory context for the worker. This will be reset after each
	 * iteration and during error recovery.
	 */
	undo_worker_context = AllocSetContextCreate(TopMemoryContext,
												"UNDO Worker",
												ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(undo_worker_context);

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * Unlike the autovacuum worker, this is a long-lived background process
	 * that must survive transient errors (e.g. an ERROR raised while
	 * discarding UNDO or cleaning up retained sLog before-images).  Letting
	 * the error propagate would terminate the worker; the postmaster would
	 * restart it, but any in-flight discard progress would be abandoned and
	 * discard could stall.  Instead we recover in place, mirroring the
	 * autovacuum launcher.
	 *
	 * We use sigsetjmp(..., 1) so the prevailing signal mask is restored on
	 * longjmp; signals other than SIGQUIT stay blocked until we exit.  The
	 * HOLD_INTERRUPTS() call is still required because InterruptPending might
	 * already be set.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevents interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * Abort the current transaction in order to recover, but only if one
		 * is actually in progress.  perform_undo_discard() and the sLog
		 * cleanup operate on shared memory and do not normally open a
		 * transaction; guarding avoids a spurious "AbortCurrentTransaction
		 * when not in transaction" path.
		 */
		if (IsTransactionState())
			AbortCurrentTransaction();

		/* Release any other resources we might still be holding. */
		LWLockReleaseAll();
		pgstat_report_wait_end();
		pgaio_error_cleanup();
		UnlockBuffers();
		if (AuxProcessResourceOwner)
			ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/* Return to the worker context and clear ErrorContext. */
		MemoryContextSwitchTo(undo_worker_context);
		FlushErrorState();

		/* Flush any leaked data in the worker context. */
		MemoryContextReset(undo_worker_context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  We don't want to be
		 * filling the error logs as fast as we can.
		 */
		pg_usleep(1000000L);
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Main loop: wake up periodically and discard old UNDO
	 */
	while (!UndoWorkerShmem->shutdown_requested)
	{
		int			rc;
		long		naptime;
		uint32		sealed_count;

		/* Process any pending configuration changes */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);

			/* Update naptime from GUC */
			UndoWorkerShmem->naptime_ms = undo_worker_naptime;
		}

		CHECK_FOR_INTERRUPTS();

		INJECTION_POINT("undo-worker-before-discard", NULL);

		/* Perform UNDO discard */
		perform_undo_discard();

		INJECTION_POINT("undo-worker-after-discard", NULL);

		/*
		 * Adaptive sleep: use a shorter interval when sealed logs are pending
		 * cleanup, similar to the WAL writer's adaptive sleep.
		 */
		sealed_count = pg_atomic_read_u32(&UndoWorkerShmem->sealed_log_count);
		if (sealed_count > 0)
			naptime = UNDO_WORKER_FAST_NAPTIME_MS;
		else
			naptime = UndoWorkerShmem->naptime_ms;

		/* Sleep until next iteration, latch set, or signal */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   naptime,
					   WAIT_EVENT_UNDO_WORKER_MAIN);

		ResetLatch(MyLatch);

		/* Emergency bailout if postmaster died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);
	}

	/* Clear our ProcNumber before exiting */
	UndoWorkerShmem->worker_proc = INVALID_PROC_NUMBER;

	/* Normal shutdown */
	ereport(LOG,
			(errmsg("UNDO worker shutting down")));

	proc_exit(0);
}

/*
 * UndoWorkerRegister - Register the UNDO worker at server start
 *
 * This is called from postmaster during server initialization.
 */
void
UndoWorkerRegister(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(BackgroundWorker));

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 10;	/* Restart after 10 seconds if crashed */

	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "UndoWorkerMain");
	snprintf(worker.bgw_name, BGW_MAXLEN, "undo worker");
	snprintf(worker.bgw_type, BGW_MAXLEN, "undo worker");

	RegisterBackgroundWorker(&worker);
}

/*
 * UndoWorkerRequestShutdown - Request worker to shut down
 */
void
UndoWorkerRequestShutdown(void)
{
	if (UndoWorkerShmem != NULL)
	{
		LWLockAcquire(&UndoWorkerShmem->lock, LW_EXCLUSIVE);
		UndoWorkerShmem->shutdown_requested = true;
		LWLockRelease(&UndoWorkerShmem->lock);
	}
}
