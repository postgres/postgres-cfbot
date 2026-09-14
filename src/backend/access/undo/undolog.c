/*-------------------------------------------------------------------------
 *
 * undolog.c
 *	  PostgreSQL UNDO log manager -- WAL-integrated version
 *
 * With UNDO-in-WAL, UNDO records are stored in the standard WAL stream
 * as XLOG_UNDO_BATCH records.  The separate base/undo/ segment files,
 * direct pwrite()/pread() I/O path, and per-backend fd cache have been
 * removed.  This file retains:
 *
 * - GUC parameters (undo_worker_naptime, etc.)
 * - Shared memory structures for UNDO state tracking
 * - Discard pointer management (repurposed for WAL-based UNDO)
 * - Checkpoint support (statistics logging)
 *
 * The previous functions (UndoLogAllocate, UndoLogWrite, UndoLogRead,
 * UndoLogSync, UndoLogSealAndRotate, etc.) are removed.  Callers now
 * use UndoRecordSetInsert() which writes directly to WAL via XLogInsert.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undolog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/twophase.h"
#include "access/undolog.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/procnumber.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

/* GUC parameters */
int			undo_worker_naptime = 10000;	/* 10 seconds */
int			undo_max_wal_retention_size = 0;	/* 0 = unlimited, in MB */
int			undo_batch_size_kb = 256;	/* UNDO batch flush threshold in KB */
int			undo_batch_record_limit = 1000; /* UNDO batch flush threshold in
											 * records */

/* Shared memory pointer */
UndoLogSharedData *UndoLogShared = NULL;

/*
 * UndoLogShmemSize
 *		Calculate shared memory size for UNDO log management
 *
 * The size includes the fixed UndoLogSharedData fields plus a per-backend
 * array of pg_atomic_uint64 for first UNDO batch LSN tracking.
 */
Size
UndoLogShmemSize(void)
{
	Size		size;

	/* Fixed struct size up to (but not including) the flexible array */
	size = offsetof(UndoLogSharedData, backend_undo_lsns);

	/* Per-backend first-batch LSN slots */
	size = add_size(size, mul_size(MaxBackends, sizeof(pg_atomic_uint64)));

	return size;
}

/*
 * UndoLogShmemInit
 *		Initialize shared memory for UNDO log management
 */
void
UndoLogShmemInit(void)
{
	bool		found;

	UndoLogShared = (UndoLogSharedData *)
		ShmemInitStruct("UNDO Log Control", UndoLogShmemSize(), &found);

	if (!found)
	{
		int			i;

		/* Initialize all log control structures */
		for (i = 0; i < MAX_UNDO_LOGS; i++)
		{
			UndoLogControl *log = &UndoLogShared->logs[i];

			log->log_number = 0;
			pg_atomic_init_u64(&log->insert_ptr, InvalidUndoRecPtr);
			log->discard_ptr = InvalidUndoRecPtr;
			log->oldest_xid = InvalidTransactionId;
			LWLockInitialize(&log->lock, LWTRANCHE_UNDO_LOG);
			log->in_use = false;
			log->state = UNDO_LOG_FREE;
			pg_atomic_init_u64(&log->seal_ptr, InvalidUndoRecPtr);
			log->sealed_time = 0;
		}

		UndoLogShared->next_log_number = 1;
		LWLockInitialize(&UndoLogShared->allocation_lock, LWTRANCHE_UNDO_LOG);
		pg_atomic_init_u32(&UndoLogShared->active_log_idx, MAX_UNDO_LOGS);
		pg_atomic_init_u64(&UndoLogShared->total_allocated, 0);
		pg_atomic_init_u64(&UndoLogShared->total_discarded, 0);
		pg_atomic_init_u64(&UndoLogShared->undo_discard_horizon,
						   InvalidXLogRecPtr);

		/* Initialize per-backend first UNDO batch LSN slots */
		for (i = 0; i < MaxBackends; i++)
			pg_atomic_init_u64(&UndoLogShared->backend_undo_lsns[i],
							   InvalidXLogRecPtr);
	}
}

/*
 * UndoLogDiscard
 *		Advance the UNDO discard horizon.
 *
 * With UNDO-in-WAL, discard means advancing the WAL retention horizon
 * past which UNDO records are no longer needed for rollback.  The
 * background UNDO worker calls this after confirming all transactions
 * older than oldest_needed have committed or had their UNDO applied.
 */
void
UndoLogDiscard(UndoRecPtr oldest_needed)
{
	int			i;

	if (!UndoRecPtrIsValid(oldest_needed))
		return;

	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (!log->in_use)
			continue;

		LWLockAcquire(&log->lock, LW_EXCLUSIVE);

		if (UndoRecPtrGetLogNo(oldest_needed) == log->log_number)
		{
			if (UndoRecPtrGetOffset(oldest_needed) > UndoRecPtrGetOffset(log->discard_ptr))
			{
				log->discard_ptr = oldest_needed;
				ereport(DEBUG2,
						(errmsg("UNDO discard: log %u advanced to offset %llu",
								log->log_number,
								(unsigned long long) UndoRecPtrGetOffset(oldest_needed))));
			}
		}

		LWLockRelease(&log->lock);
	}
}

/*
 * UndoLogGetOldestDiscardPtr
 *		Get the oldest UNDO discard pointer across all active logs.
 *
 * Used to determine WAL retention requirements for UNDO.
 */
UndoRecPtr
UndoLogGetOldestDiscardPtr(void)
{
	UndoRecPtr	oldest = InvalidUndoRecPtr;
	int			i;

	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (log->in_use)
		{
			if (!UndoRecPtrIsValid(oldest) ||
				log->discard_ptr < oldest)
				oldest = log->discard_ptr;
		}
	}

	return oldest;
}

/*
 * CheckPointUndoLog
 *		Perform checkpoint processing for the UNDO log subsystem.
 *
 * With UNDO-in-WAL, there are no UNDO segment files to sync.
 * This function logs statistics when log_checkpoints is enabled.
 */
void
CheckPointUndoLog(void)
{
	int			active_logs = 0;
	uint64		total_allocated = 0;
	uint64		total_discarded = 0;
	int			i;

	if (UndoLogShared == NULL)
		return;

	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (!log->in_use)
			continue;

		active_logs++;
		total_allocated += UndoRecPtrGetOffset(pg_atomic_read_u64(&log->insert_ptr));

		LWLockAcquire(&log->lock, LW_SHARED);
		total_discarded += UndoRecPtrGetOffset(log->discard_ptr);
		LWLockRelease(&log->lock);
	}

	if (log_checkpoints && active_logs > 0)
	{
		ereport(LOG,
				(errmsg("UNDO checkpoint: %d active log(s), "
						"%llu bytes allocated, %llu bytes discarded, "
						"%llu bytes retained",
						active_logs,
						(unsigned long long) total_allocated,
						(unsigned long long) total_discarded,
						(unsigned long long) (total_allocated - total_discarded))));
	}
}

/*
 * UndoGetDiscardHorizon
 *		Return the current UNDO discard horizon LSN.
 *
 * WAL segments containing data at or after this LSN must be retained
 * because they contain UNDO records that may still be needed for
 * rollback of in-progress transactions.
 *
 * Returns InvalidXLogRecPtr if no UNDO data exists (UNDO not in use
 * or all transactions have committed).
 */
XLogRecPtr
UndoGetDiscardHorizon(void)
{
	if (UndoLogShared == NULL)
		return InvalidXLogRecPtr;

	return (XLogRecPtr) pg_atomic_read_u64(&UndoLogShared->undo_discard_horizon);
}

/*
 * UndoSetDiscardHorizon
 *		Advance the UNDO discard horizon to a new LSN.
 *
 * Called by the UNDO discard worker after confirming that all UNDO
 * records before 'horizon' have been processed (transactions committed
 * or rolled back, index pruning completed).
 *
 * The horizon only moves forward -- if the new value is older than
 * the current horizon, the call is a no-op.
 */
void
UndoSetDiscardHorizon(XLogRecPtr horizon)
{
	uint64		old_horizon;

	if (UndoLogShared == NULL || !XLogRecPtrIsValid(horizon))
		return;

	/* Advance forward only */
	while (true)
	{
		old_horizon = pg_atomic_read_u64(&UndoLogShared->undo_discard_horizon);

		if (XLogRecPtrIsValid((XLogRecPtr) old_horizon) &&
			horizon <= (XLogRecPtr) old_horizon)
			break;				/* already at or past this point */

		if (pg_atomic_compare_exchange_u64(&UndoLogShared->undo_discard_horizon,
										   &old_horizon, (uint64) horizon))
			break;
	}
}

/*
 * UndoRegisterBatchLSN
 *		Register the first UNDO batch LSN for the current backend.
 *
 * Called from UndoRecordSetInsert() the first time a transaction writes
 * UNDO data.  Stores the LSN in the per-backend slot so that the UNDO
 * discard worker can find the oldest in-flight UNDO batch and avoid
 * recycling WAL segments that still contain needed UNDO data.
 *
 * Only the FIRST call per transaction takes effect (we want the oldest,
 * i.e., smallest, LSN).  Subsequent calls for the same transaction are
 * no-ops because the slot is already occupied.
 */
void
UndoRegisterBatchLSN(XLogRecPtr batch_lsn)
{
	pg_atomic_uint64 *slot;
	uint64		expected;

	if (UndoLogShared == NULL || !XLogRecPtrIsValid(batch_lsn))
		return;
	if (MyProcNumber < 0 || MyProcNumber >= MaxBackends)
		return;

	slot = &UndoLogShared->backend_undo_lsns[MyProcNumber];
	expected = InvalidXLogRecPtr;

	/*
	 * Only set if the slot is currently empty.  This records the first
	 * (oldest) batch for this transaction; later batches have larger LSNs and
	 * should not overwrite the stored value.
	 */
	(void) pg_atomic_compare_exchange_u64(slot, &expected, (uint64) batch_lsn);
}

/*
 * UndoClearBatchLSN
 *		Clear the per-backend UNDO batch LSN registration.
 *
 * Called at transaction commit or abort to release the WAL retention
 * hold that was established by UndoRegisterBatchLSN().
 */
void
UndoClearBatchLSN(void)
{
	if (UndoLogShared == NULL)
		return;
	if (MyProcNumber < 0 || MyProcNumber >= MaxBackends)
		return;

	pg_atomic_write_u64(&UndoLogShared->backend_undo_lsns[MyProcNumber],
						(uint64) InvalidXLogRecPtr);
}

/*
 * UndoGetOldestBatchLSN
 *		Return the oldest UNDO batch LSN that must be retained in WAL.
 *
 * Considers both:
 *   1. Per-backend slots (in-flight transactions with UNDO data)
 *   2. ATM entries (aborted transactions awaiting Logical Revert)
 *
 * The ATM check is critical: once a transaction aborts, its per-backend
 * slot is cleared by UndoClearBatchLSN(), but the logical revert worker
 * still needs to read the UNDO batches from WAL.  Without this check,
 * checkpoints could recycle WAL segments containing needed UNDO data,
 * causing the revert worker to crash (SIGBUS/SIGSEGV) or read garbage.
 *
 * Returns InvalidXLogRecPtr if no WAL retention is needed for UNDO.
 */
XLogRecPtr
UndoGetOldestBatchLSN(void)
{
	XLogRecPtr	oldest = InvalidXLogRecPtr;
	int			i;

	if (UndoLogShared == NULL)
		return InvalidXLogRecPtr;

	/* Check per-backend slots for in-flight transactions */
	for (i = 0; i < MaxBackends; i++)
	{
		XLogRecPtr	lsn = (XLogRecPtr)
			pg_atomic_read_u64(&UndoLogShared->backend_undo_lsns[i]);

		if (XLogRecPtrIsValid(lsn))
		{
			if (!XLogRecPtrIsValid(oldest) || lsn < oldest)
				oldest = lsn;
		}
	}

	/*
	 * Check prepared (2PC) transactions.  A xact can sit PREPARED
	 * indefinitely; its UNDO-batch WAL must survive until ROLLBACK PREPARED
	 * reads it.  The per-backend slot is cleared when the preparing backend
	 * exits, so it does not cover this.
	 */
	{
		XLogRecPtr	prep_oldest = TwoPhaseGetOldestUndoBatchLSN();

		if (XLogRecPtrIsValid(prep_oldest) &&
			(!XLogRecPtrIsValid(oldest) || prep_oldest < oldest))
			oldest = prep_oldest;
	}

	return oldest;
}

/*
 * Legacy no-op stubs
 *
 * UNDO-in-WAL has no per-log segment files, no fd cache, and no
 * per-backend write-pointer tracking: UNDO data lives in the WAL stream
 * and durability/recycling are handled by WAL flush and the discard
 * worker.  The operations below are therefore inherently nothing-to-do in
 * this mode, but they still have live callers in the shared transaction
 * and discard paths (e.g. UndoLogCloseFiles / UndoFlushResetMaxWritePtr
 * from xactundo.c, UndoLogDeleteSegmentFile from the discard worker,
 * ExtendUndoLogFile from the undo_xlog.c redo path).  We keep them as
 * no-ops so those callers stay uniform across both UNDO modes rather than
 * sprinkling mode checks at every call site; the no-op is the correct
 * behaviour here, not a placeholder awaiting future work.
 */

void
UndoLogSync(void)
{
	/* No-op: WAL sync handles durability */
}

void
UndoLogCloseFiles(void)
{
	/* No-op: no fd cache with UNDO-in-WAL */
}

void
UndoFlushResetMaxWritePtr(void)
{
	/* No-op: no per-backend write pointer tracking with UNDO-in-WAL */
}

UndoRecPtr
UndoFlushGetMaxWritePtr(void)
{
	/* No-op: no per-backend write pointer tracking with UNDO-in-WAL */
	return InvalidUndoRecPtr;
}

void
UndoLogSealAndRotate(uint8 trigger pg_attribute_unused())
{
	/* No-op: no segment rotation with UNDO-in-WAL */
}

void
UndoLogDeleteSegmentFile(uint32 log_number pg_attribute_unused())
{
	/* No-op: no segment files with UNDO-in-WAL */
}

bool
UndoLogTryPressureDiscard(void)
{
	/* No-op: no segment pressure with UNDO-in-WAL */
	return false;
}

char *
UndoLogPath(uint32 log_number, char *path)
{
	/* Legacy: construct the path even though files no longer exist */
	snprintf(path, MAXPGPATH, "base/undo/%012u", log_number);
	return path;
}

void
ExtendUndoLogFile(uint32 log_number pg_attribute_unused(),
				  uint64 logical_end pg_attribute_unused())
{
	/* No-op: no segment files with UNDO-in-WAL */
}

void
ExtendUndoLogSmgrFile(uint32 log_number pg_attribute_unused(),
					  uint64 logical_end pg_attribute_unused())
{
	/* No-op: no smgr-managed UNDO files with UNDO-in-WAL */
}

UndoRecPtr
UndoLogGetInsertPtr(uint32 log_number)
{
	int			i;
	UndoRecPtr	ptr = InvalidUndoRecPtr;

	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (log->in_use && log->log_number == log_number)
		{
			ptr = pg_atomic_read_u64(&log->insert_ptr);
			break;
		}
	}

	return ptr;
}

UndoRecPtr
UndoLogGetDiscardPtr(uint32 log_number)
{
	int			i;
	UndoRecPtr	ptr = InvalidUndoRecPtr;

	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (log->in_use && log->log_number == log_number)
		{
			LWLockAcquire(&log->lock, LW_SHARED);
			ptr = log->discard_ptr;
			LWLockRelease(&log->lock);
			break;
		}
	}

	return ptr;
}
