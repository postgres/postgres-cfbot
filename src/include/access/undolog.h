/*-------------------------------------------------------------------------
 *
 * undolog.h
 *	  PostgreSQL UNDO log manager -- WAL-integrated version
 *
 * With UNDO-in-WAL, UNDO records are stored in the standard WAL stream
 * as XLOG_UNDO_BATCH records.  The separate base/undo/ segment files
 * and direct I/O path have been removed.  This header retains:
 *
 * - UndoRecPtr encoding macros (still used for addressing)
 * - Shared memory structures (UndoLogControl, UndoLogSharedData)
 * - GUC parameter declarations
 * - Functions for shmem init, discard, and checkpoint
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undolog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDOLOG_H
#define UNDOLOG_H

#include "access/transam.h"
#include "access/undodefs.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "port/atomics.h"
#include "port/pg_crc32c.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

/*
 * UndoRecPtr: 64-bit pointer to UNDO record
 *
 * Format:
 *   Bits 0-39:  Offset within log (40 bits = 1TB per log)
 *   Bits 40-63: Log number (24 bits = 16M logs)
 *
 * The actual UndoRecPtr typedef and InvalidUndoRecPtr are in undodefs.h
 * to avoid circular include dependencies.
 */

/* Extract log number and offset from UndoRecPtr */
#define UndoRecPtrGetLogNo(ptr) ((uint32) (((uint64) (ptr)) >> 40))
#define UndoRecPtrGetOffset(ptr) (((uint64) (ptr)) & 0xFFFFFFFFFFULL)

/* Construct UndoRecPtr from log number and offset */
#define MakeUndoRecPtr(logno, offset) \
	((((uint64) (logno)) << 40) | ((uint64) (offset)))

/*
 * Legacy define -- no longer used (UNDO records are in WAL, not segment
 * files).  Retained for any code that still references it at compile time.
 */
#define UNDO_LOG_SEGMENT_SIZE (1024 * 1024 * 1024)

/* Maximum number of concurrent UNDO logs */
#define MAX_UNDO_LOGS 100

/*
 * UndoLogState: Lifecycle state of an UNDO log slot
 *
 * With UNDO-in-WAL, the segment lifecycle is simplified -- these states
 * are retained for shared memory structure compatibility but the
 * ACTIVE->SEALED->DISCARDABLE rotation no longer occurs.
 */
typedef enum UndoLogState
{
	UNDO_LOG_FREE = 0,			/* Slot available */
	UNDO_LOG_ACTIVE,			/* Accepting writes */
	UNDO_LOG_SEALED,			/* No more writes */
	UNDO_LOG_DISCARDABLE		/* All records discarded */
} UndoLogState;

/*
 * UndoLogControl: Shared memory control structure for one UNDO log
 */
typedef struct UndoLogControl
{
	uint32		log_number;		/* Log number */
	pg_atomic_uint64 insert_ptr;	/* Next insertion point (atomic) */
	UndoRecPtr	discard_ptr;	/* Can discard older than this */
	TransactionId oldest_xid;	/* Oldest transaction needing this log */
	LWLock		lock;			/* Protects metadata (NOT insert_ptr) */
	bool		in_use;			/* Is this log slot active? */
	UndoLogState state;			/* Current lifecycle state */
	pg_atomic_uint64 seal_ptr;	/* insert_ptr frozen at seal time */
	TimestampTz sealed_time;	/* When this log was sealed */
} UndoLogControl;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
StaticAssertDecl(offsetof(UndoLogControl, insert_ptr) % 8 == 0,
				 "UndoLogControl.insert_ptr must be eight-byte aligned");
StaticAssertDecl(offsetof(UndoLogControl, seal_ptr) % 8 == 0,
				 "UndoLogControl.seal_ptr must be eight-byte aligned");
StaticAssertDecl(sizeof(UndoLogControl) % 8 == 0,
				 "UndoLogControl array elements must be eight-byte aligned");
#endif

/*
 * UndoLogSharedData: Shared memory for all UNDO logs
 *
 * Note: backend_undo_lsns is a flexible array member; the struct must be
 * allocated with room for MaxBackends entries.  Use UndoLogShmemSize() to
 * get the correct allocation size.
 */
typedef struct UndoLogSharedData
{
	UndoLogControl logs[MAX_UNDO_LOGS];
	uint32		next_log_number;
	LWLock		allocation_lock;
	pg_atomic_uint32 active_log_idx;
	pg_atomic_uint64 total_allocated;
	pg_atomic_uint64 total_discarded;

	/*
	 * UNDO discard horizon: the oldest XLogRecPtr of an XLOG_UNDO_BATCH
	 * record that is still needed for rollback or index pruning.  WAL
	 * segments containing data at or after this LSN must be retained. Updated
	 * by the UNDO discard worker as transactions complete and their UNDO
	 * records are no longer needed.
	 */
	pg_atomic_uint64 undo_discard_horizon;

	/*
	 * Per-backend first UNDO batch LSN.
	 *
	 * Each active backend stores the XLogRecPtr of its first XLOG_UNDO_BATCH
	 * record here when it writes UNDO data for a transaction.  Cleared at
	 * commit or abort.  The UNDO discard worker scans this array to find the
	 * global minimum, which becomes the new undo_discard_horizon, preventing
	 * WAL recycling past the oldest in-flight UNDO batch.
	 *
	 * Indexed by MyProcNumber (0-based, range [0, MaxBackends)).
	 *
	 * Must be last field -- UndoLogShmemSize() uses
	 * offsetof(UndoLogSharedData, backend_undo_lsns).
	 */
	pg_atomic_uint64 backend_undo_lsns[FLEXIBLE_ARRAY_MEMBER];
} UndoLogSharedData;

#ifndef PG_HAVE_ATOMIC_U64_SIMULATION
StaticAssertDecl(offsetof(UndoLogSharedData, logs) % 8 == 0,
				 "UndoLogSharedData.logs must be eight-byte aligned");
StaticAssertDecl(offsetof(UndoLogSharedData, total_allocated) % 8 == 0,
				 "UndoLogSharedData.total_allocated must be eight-byte aligned");
StaticAssertDecl(offsetof(UndoLogSharedData, total_discarded) % 8 == 0,
				 "UndoLogSharedData.total_discarded must be eight-byte aligned");
StaticAssertDecl(offsetof(UndoLogSharedData, undo_discard_horizon) % 8 == 0,
				 "UndoLogSharedData.undo_discard_horizon must be eight-byte aligned");
StaticAssertDecl(offsetof(UndoLogSharedData, backend_undo_lsns) % 8 == 0,
				 "UndoLogSharedData.backend_undo_lsns must be eight-byte aligned");
#endif

StaticAssertDecl(sizeof(XLogRecPtr) == sizeof(uint64),
				 "XLogRecPtr must be 64 bits for UNDO per-backend atomic LSN slots to be correct");

/* Global shared memory pointer (set during startup) */
extern UndoLogSharedData *UndoLogShared;

/* GUC parameters */
/*
 * Note: UNDO records are embedded in WAL (no separate segment files).
 * UNDO_LOG_SEGMENT_SIZE and MAX_UNDO_LOGS are legacy defines retained
 * for compile-time compatibility.
 */
extern int	undo_worker_naptime;
extern int	undo_rollback_overflow_size;
extern int	undo_max_wal_retention_size;
extern int	undo_batch_size_kb;
extern int	undo_batch_record_limit;

/*
 * Shared memory initialization
 */
extern Size UndoLogShmemSize(void);
extern void UndoLogShmemRequest(void);
extern void UndoLogShmemInit(void);

/*
 * Discard, retention, and checkpoint
 */
extern void UndoLogDiscard(UndoRecPtr oldest_needed);
extern UndoRecPtr UndoLogGetOldestDiscardPtr(void);
extern void CheckPointUndoLog(void);

/* WAL retention for UNDO: get/set the discard horizon */
extern XLogRecPtr UndoGetDiscardHorizon(void);
extern void UndoSetDiscardHorizon(XLogRecPtr horizon);

/* Per-backend UNDO batch LSN registration for WAL retention */
extern void UndoRegisterBatchLSN(XLogRecPtr batch_lsn);
extern void UndoClearBatchLSN(void);
extern XLogRecPtr UndoGetOldestBatchLSN(void);

/*
 * Utility functions
 */
extern UndoRecPtr UndoLogGetInsertPtr(uint32 log_number);
extern UndoRecPtr UndoLogGetDiscardPtr(uint32 log_number);
extern char *UndoLogPath(uint32 log_number, char *path);

/*
 * Legacy no-op stubs -- retained for callers not yet fully updated.
 * These are all no-ops in the UNDO-in-WAL architecture.
 */
extern void UndoLogSync(void);
extern void UndoLogCloseFiles(void);
extern void ExtendUndoLogFile(uint32 log_number, uint64 new_size);
extern void ExtendUndoLogSmgrFile(uint32 log_number, uint64 logical_end);
extern UndoRecPtr UndoFlushGetMaxWritePtr(void);
extern void UndoFlushResetMaxWritePtr(void);
extern void UndoLogSealAndRotate(uint8 trigger);
extern void UndoLogDeleteSegmentFile(uint32 log_number);
extern bool UndoLogTryPressureDiscard(void);

#endif							/* UNDOLOG_H */
