/*-------------------------------------------------------------------------
 *
 * undo_xlog.h
 *	  UNDO resource manager WAL record definitions
 *
 * This file contains the WAL record format definitions for UNDO log
 * operations. These records are logged by the RM_UNDO_ID resource manager.
 *
 * Record types:
 *   XLOG_UNDO_ALLOCATE       - Log UNDO space allocation
 *   XLOG_UNDO_DISCARD        - Log UNDO record discard
 *   XLOG_UNDO_EXTEND         - Log UNDO log file extension
 *   XLOG_UNDO_APPLY_RECORD   - CLR: Log physical UNDO application to a page
 *
 * The XLOG_UNDO_APPLY_RECORD type is a Compensation Log Record (CLR).
 * CLRs record the fact that an UNDO operation was applied to a page
 * during transaction rollback.  This ensures crash safety: if we crash
 * during rollback, the already-applied UNDO operations are preserved
 * via WAL replay of the CLR's full page image.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undo_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDO_XLOG_H
#define UNDO_XLOG_H

#include "access/transam.h"
#include "access/xlogdefs.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/block.h"
#include "storage/off.h"
#include "storage/relfilelocator.h"

/*
 * UndoRecPtr type definition.  We use undodefs.h which is lightweight
 * and can be included in both frontend and backend code.  If undodefs.h
 * has already been included (via undolog.h or directly), this is a no-op.
 */
#include "access/undodefs.h"

/*
 * WAL record types for UNDO operations
 *
 * These are the info codes for UNDO WAL records. The low 4 bits are used
 * for operation type, leaving the upper 4 bits for flags.
 */
#define XLOG_UNDO_ALLOCATE			0x00	/* Allocate UNDO log space
											 * (legacy) */
#define XLOG_UNDO_DISCARD			0x10	/* Discard old UNDO records */
#define XLOG_UNDO_EXTEND			0x20	/* Extend UNDO log file (legacy) */
#define XLOG_UNDO_APPLY_RECORD		0x30	/* CLR: UNDO applied to page */
#define XLOG_UNDO_ROTATE			0x40	/* Seal old log, activate new
											 * (legacy) */
#define XLOG_UNDO_PAGE_WRITE		0x50	/* Write UNDO data to a page
											 * (legacy) */
#define XLOG_UNDO_BATCH				0x60	/* Batched UNDO records in WAL */

/*
 * xl_undo_allocate - WAL record for UNDO space allocation
 *
 * Logged when a backend allocates space in an UNDO log for writing
 * UNDO records. This ensures crash recovery can reconstruct the
 * insert pointer state.
 */
typedef struct xl_undo_allocate
{
	UndoRecPtr	start_ptr;		/* Starting position of allocation */
	uint32		length;			/* Length of allocation in bytes */
	TransactionId xid;			/* Transaction that allocated this space */
	uint32		log_number;		/* Log number (extracted from start_ptr) */
} xl_undo_allocate;

#define SizeOfUndoAllocate	(offsetof(xl_undo_allocate, log_number) + sizeof(uint32))

/*
 * xl_undo_discard - WAL record for UNDO discard operation
 *
 * Logged when the UNDO worker discards old UNDO records that are no
 * longer needed by any active transaction. This allows space to be
 * reclaimed.
 */
typedef struct xl_undo_discard
{
	UndoRecPtr	discard_ptr;	/* New discard pointer (oldest still needed) */
	uint32		log_number;		/* Which log is being discarded */
	TransactionId oldest_xid;	/* Oldest XID still needing UNDO */
} xl_undo_discard;

#define SizeOfUndoDiscard	(offsetof(xl_undo_discard, oldest_xid) + sizeof(TransactionId))

/*
 * xl_undo_extend - WAL record for UNDO log file extension
 *
 * Logged when an UNDO log file is extended to accommodate more UNDO
 * records. This ensures the file size is correctly restored during
 * crash recovery.
 */
typedef struct xl_undo_extend
{
	uint32		log_number;		/* Which log is being extended */
	uint64		new_size;		/* New size of log file in bytes */
} xl_undo_extend;

#define SizeOfUndoExtend	(offsetof(xl_undo_extend, new_size) + sizeof(uint64))

/*
 * xl_undo_apply - CLR for physical UNDO application (physiological)
 *
 * This is a Compensation Log Record (CLR) generated when an UNDO record
 * is physically applied to a heap or index page during transaction rollback.
 *
 * Physiological CLR approach:
 *   Instead of storing a full 8KB page image (REGBUF_FORCE_IMAGE), we log
 *   just the operation and its data.  During redo, we re-apply the exact
 *   same page modification.  This reduces WAL volume from ~8KB to
 *   ~100-500 bytes per CLR.
 *
 *   For operations that only change LP state (INSERT undo, HOT_UPDATE kill),
 *   no additional data is needed -- the metadata in xl_undo_apply suffices.
 *
 *   For operations that restore tuple data (DELETE/UPDATE/INPLACE undo),
 *   the tuple data follows the fixed header as registered buffer data.
 *
 *   For full page image operations (DEDUP undo), REGBUF_FORCE_IMAGE is
 *   still used since the entire page is being replaced.
 *
 * CLR flags (in clr_flags):
 *   UNDO_CLR_HAS_TUPLE    - Before-image bytes follow in block data (FLUX)
 *   UNDO_CLR_LP_DEAD      - Mark line pointer LP_DEAD (nbtree/hash INSERT undo)
 */

/* CLR operation flags */
#define UNDO_CLR_HAS_TUPLE		0x0001	/* Before-image in block data */
#define UNDO_CLR_LP_DEAD		0x0004	/* Mark target LP_DEAD */

typedef struct xl_undo_apply
{
	UndoRecPtr	urec_ptr;		/* UNDO record pointer that was applied */
	TransactionId xid;			/* Transaction being rolled back */
	RelFileLocator target_locator;	/* Target relation file locator */
	BlockNumber target_block;	/* Target block number */
	OffsetNumber target_offset; /* Target item offset within page */
	uint16		operation_type; /* UNDO subtype (HEAP_UNDO_INSERT, etc.) */
	uint16		clr_flags;		/* UNDO_CLR_* flags */
	uint32		tuple_len;		/* Restored tuple length (0 if no tuple) */
} xl_undo_apply;

#define SizeOfUndoApply	(offsetof(xl_undo_apply, tuple_len) + sizeof(uint32))

/*
 * xl_undo_page_write - WAL record for UNDO page data write
 *
 * Logged when UNDO data is written to a shared-buffer-managed page.
 * The actual data follows the record header and is also registered
 * via XLogRegisterBufData as buffer-specific data (block reference 0).
 *
 * During redo, the data is memcpy'd into the page at page_offset.
 * If a full page image was stored (REGBUF_STANDARD enables FPI after
 * checkpoints), XLogReadBufferForRedo restores it automatically and
 * no additional replay is needed.
 */
typedef struct xl_undo_page_write
{
	uint32		page_offset;	/* Offset within the page to write at */
	uint32		data_len;		/* Length of data written */
} xl_undo_page_write;

#define SizeOfUndoPageWrite	(offsetof(xl_undo_page_write, data_len) + sizeof(uint32))

/*
 * Rotation trigger reasons for XLOG_UNDO_ROTATE records
 */
#define UNDO_ROTATE_CAPACITY	0x01	/* Rotated due to capacity threshold */
#define UNDO_ROTATE_CHECKPOINT	0x02	/* Rotated at checkpoint boundary */
#define UNDO_ROTATE_PRESSURE	0x03	/* Rotated under allocation pressure */
#define UNDO_ROTATE_MANUAL		0x04	/* Rotated on request (unused) */

/*
 * xl_undo_rotate - WAL record for UNDO log segment rotation
 *
 * Logged when the active UNDO log is sealed and a new one is activated.
 * During recovery, the old log is marked SEALED and the new log is
 * marked ACTIVE, restoring the correct lifecycle state.
 */
typedef struct xl_undo_rotate
{
	uint32		old_log_number; /* Log being sealed (0 if first log) */
	UndoRecPtr	old_seal_ptr;	/* Insert pointer at seal time */
	uint32		new_log_number; /* Newly activated log */
	uint8		trigger;		/* UNDO_ROTATE_* reason */
} xl_undo_rotate;

#define SizeOfUndoRotate	(offsetof(xl_undo_rotate, trigger) + sizeof(uint8))

/*
 * xl_undo_batch - WAL record for batched UNDO data (XLOG_UNDO_BATCH)
 *
 * This record type replaces the old pwrite()-to-segment-file path.
 * All UNDO records for a batch are serialized into a single WAL record.
 * The batch payload contains concatenated UndoRecordHeader+payload pairs
 * in their exact serialized format.
 *
 * The chain_prev field links this batch to the previous batch for the
 * same transaction.  During rollback, the UNDO chain is walked backward
 * by reading WAL records at successive chain_prev LSNs.
 *
 * Coalescing: The existing UndoRecordSet mechanism batches records
 * (flush at 256KB or 1000 records).  This batch becomes one WAL record.
 * A 1000-row INSERT produces ~1 WAL record containing 1000 UNDO records.
 */
typedef struct xl_undo_batch
{
	TransactionId xid;			/* Owning transaction */
	XLogRecPtr	chain_prev;		/* LSN of previous batch for this xact
								 * (InvalidXLogRecPtr if first batch) */
	uint32		nrecords;		/* Number of UNDO records in batch */
	uint32		total_len;		/* Total bytes of serialized UNDO data */
	Oid			primary_reloid; /* Relation OID (optimization for
								 * single-relation batches) */
	UndoPersistenceLevel persistence;	/* Persistence level of this batch */
	/* Followed by total_len bytes of serialized UndoRecordHeader+payload */
} xl_undo_batch;

#define SizeOfUndoBatch (offsetof(xl_undo_batch, persistence) + sizeof(UndoPersistenceLevel))

/*
 * xl_undo_chain_state - UNDO chain state for prepared transactions
 *
 * Saved in the two-phase state file during PREPARE TRANSACTION, so the
 * UNDO chain can be restored during COMMIT/ROLLBACK PREPARED.
 */
typedef struct xl_undo_chain_state
{
	UndoRecPtr	firstUndoPtr;	/* First UNDO record in transaction chain */
	UndoRecPtr	currentUndoPtr; /* Most recent UNDO record in chain */
} xl_undo_chain_state;

/* Function declarations for WAL operations */
extern void undo_redo(XLogReaderState *record);
extern void undo_desc(StringInfo buf, XLogReaderState *record);
extern const char *undo_identify(uint8 info);

/* Two-phase commit support */
extern void undo_twophase_recover(FullTransactionId fxid, uint16 info,
								  void *recdata, uint32 len);
extern void undo_twophase_postcommit(FullTransactionId fxid, uint16 info,
									 void *recdata, uint32 len);
extern void undo_twophase_postabort(FullTransactionId fxid, uint16 info,
									void *recdata, uint32 len);

/*
 * UNDO batch reading from WAL for rollback and recovery.
 *
 * UndoReadBatchFromWAL reads a single XLOG_UNDO_BATCH record at the
 * given LSN and returns the header plus a pointer to the payload data.
 * The caller must pfree the returned data when done.
 */
typedef struct UndoBatchData
{
	xl_undo_batch header;		/* Batch header */
	char	   *payload;		/* Serialized UNDO records (palloc'd) */
	Size		payload_len;	/* Length of payload */
} UndoBatchData;

extern bool UndoValidateBatchLSN(XLogRecPtr batch_lsn);
extern UndoBatchData *UndoReadBatchFromWAL(XLogRecPtr batch_lsn);
extern void UndoFreeBatchData(UndoBatchData *batch);
extern void UndoResetBatchReader(void);

/*
 * Recovery UNDO phase support.
 *
 * During WAL redo, XLOG_UNDO_BATCH records are tracked so that after
 * redo completes, incomplete transactions can be identified and their
 * UNDO chains walked for rollback.
 */
extern void UndoRecoveryTrackBatch(TransactionId xid, XLogRecPtr batch_lsn,
								   XLogRecPtr chain_prev,
								   UndoPersistenceLevel persistence);
extern void UndoRecoveryRemoveXid(TransactionId xid);
extern bool UndoRecoveryNeeded(void);
extern void PerformUndoRecovery(void);
extern void FlushDeferredUndoXacts(void);

#endif							/* UNDO_XLOG_H */
