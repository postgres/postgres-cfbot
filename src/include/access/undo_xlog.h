/*-------------------------------------------------------------------------
 *
 * undo_xlog.h
 *	  UNDO resource manager WAL record definitions
 *
 * This file contains the WAL record format definitions for UNDO log
 * operations. These records are logged by the RM_UNDO_ID resource manager.
 *
 * Record types:
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
 * These are the info codes for UNDO WAL records. The upper 4 bits identify
 * the operation; the low 4 bits are reserved for generic WAL flags.
 */
#define XLOG_UNDO_APPLY_RECORD		0x30	/* CLR: UNDO applied to page */
#define XLOG_UNDO_BATCH				0x60	/* Batched UNDO records in WAL */
#define XLOG_UNDO_INFLIGHT			0x70	/* UNDO chains in flight at a
											 * checkpoint */

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
	Oid			dboid;			/* Database the transaction runs in; recovery
								 * hands loser chains to that database's
								 * logical revert worker */
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
 * xl_undo_inflight - UNDO chains of transactions in flight at a checkpoint
 *
 * Written by every online checkpoint after its redo pointer.  Crash recovery
 * starts redo there and so replays none of the batches written before it; a
 * transaction whose batches all precede the redo pointer is known to
 * recovery only through this record.
 */
typedef struct xl_undo_inflight_xact
{
	TransactionId xid;			/* top-level transaction */
	Oid			dboid;			/* database it runs in */
	XLogRecPtr	first_batch_lsn[NUndoPersistenceLevels];
	XLogRecPtr	last_batch_lsn[NUndoPersistenceLevels];
} xl_undo_inflight_xact;

typedef struct xl_undo_inflight
{
	uint32		nxacts;
	xl_undo_inflight_xact xacts[FLEXIBLE_ARRAY_MEMBER];
} xl_undo_inflight;

#define SizeOfUndoInflight offsetof(xl_undo_inflight, xacts)

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
extern void undo_mask(char *pagedata, BlockNumber blkno);

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
extern UndoBatchData *UndoReadBatchFromWAL(XLogRecPtr batch_lsn,
										   bool *malformed);
extern void UndoFreeBatchData(UndoBatchData *batch);
extern void UndoResetBatchReader(void);

/*
 * Recovery UNDO phase support.
 *
 * During WAL redo, XLOG_UNDO_BATCH records are tracked so that after
 * redo completes, incomplete transactions can be identified and their
 * UNDO chains walked for rollback.
 */
extern void UndoRecoveryTrackBatch(TransactionId xid, Oid dboid,
								   XLogRecPtr batch_lsn,
								   XLogRecPtr chain_prev,
								   UndoPersistenceLevel persistence);
extern void UndoRecoveryRemoveXid(TransactionId xid);
extern void UndoRecoveryTrackInflight(const xl_undo_inflight_xact *x);
extern bool UndoRecoveryNeeded(void);
extern void PerformUndoRecovery(void);
extern void FlushDeferredUndoXacts(void);

#endif							/* UNDO_XLOG_H */
