/*-------------------------------------------------------------------------
 *
 * xactundo.h
 *	  Transaction-level undo management
 *
 * This module manages per-transaction undo record sets. It maintains
 * up to NUndoPersistenceLevels (3) record sets per transaction -- one
 * for each persistence level (permanent, unlogged, temporary). This
 * design follows the EDB undo-record-set branch architecture where
 * undo records for different persistence levels are kept separate.
 *
 * Code that wants to write transactional undo should interface with
 * these functions rather than manipulating UndoRecordSet directly.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xactundo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACTUNDO_H
#define XACTUNDO_H

#include "access/undodefs.h"
#include "access/undorecord.h"
#include "access/xlogdefs.h"

/*
 * XactUndoContext - Context for a single undo insertion within a transaction.
 *
 * Created by PrepareXactUndoData(), consumed by InsertXactUndoData()
 * and cleaned up by CleanupXactUndoInsertion(). The plevel tracks which
 * persistence-level record set this insertion belongs to.
 */
typedef struct XactUndoContext
{
	UndoPersistenceLevel plevel;
	UndoRecordSet *uset;		/* borrowed reference, do not free */
} XactUndoContext;

/* Shared memory initialization */
extern Size XactUndoShmemSize(void);
extern void XactUndoShmemInit(void);

/* Per-backend initialization */
extern void InitializeXactUndo(void);

/*
 * Undo insertion API for any AM or subsystem.
 *
 * PrepareXactUndoData: Find or create the appropriate per-persistence-level
 *   UndoRecordSet for the current transaction and prepare it for a new
 *   record. Returns the UndoRecPtr where the record will be written.
 *
 *   Parameters are AM-agnostic: the caller provides an RM ID, RM-specific
 *   info flags, a relation OID, and an opaque payload.
 *
 * InsertXactUndoData: Actually write the record data into the undo log.
 *
 * CleanupXactUndoInsertion: Release any resources held by the context.
 */
extern UndoRecPtr PrepareXactUndoData(XactUndoContext *ctx,
									  char persistence,
									  uint8 rmid,
									  uint16 info,
									  Oid reloid,
									  const char *payload,
									  Size payload_len);
extern UndoRecPtr PrepareXactUndoDataParts(XactUndoContext *ctx,
										   char persistence,
										   uint8 rmid,
										   uint16 info,
										   Oid reloid,
										   const char *part1,
										   Size part1_len,
										   const char *part2,
										   Size part2_len);
extern void InsertXactUndoData(XactUndoContext *ctx);
extern void CleanupXactUndoInsertion(XactUndoContext *ctx);

/*
 * DeferXactUndoData: accumulate this record instead of writing it now.
 *
 * The alternative to InsertXactUndoData() for callers that emit many small
 * records per statement (the index AMs: one per index entry).  The record is
 * already in ctx->uset's buffer after PrepareXactUndoData[Parts]; deferring
 * simply declines to flush that buffer, so the next record lands in the same
 * batch and all of them share one XLOG_UNDO_BATCH record instead of getting one
 * each.  This is what the UndoRecordSet was built for -- see the batching note
 * at the top of undoinsert.c.
 *
 * Deferral is only safe because every path that can consume the chain flushes
 * first: XactUndoFlushPending() is called at the top of AtAbort_XactUndo(),
 * AtSubAbort_XactUndo(), AtCommit_XactUndo() and AtPrepare_XactUndo(), and on
 * crossing the size/record thresholds below.  A deferred record is therefore
 * never the reason a rollback finds nothing to apply.
 *
 * Returns true if the record is still pending (nothing written), false if the
 * threshold forced an immediate flush.  Callers may ignore the result.
 */
extern bool DeferXactUndoData(XactUndoContext *ctx);

/*
 * XactUndoFlushPending: write any deferred records as one batch.
 *
 * Safe to call when nothing is pending (no-op).  Must NOT be called from a
 * critical section: it emits a WAL record.
 */
extern void XactUndoFlushPending(void);

/* True if any deferred record is waiting to be written. */
extern bool XactUndoHasPendingData(void);

/* Transaction lifecycle hooks */
extern void AtCommit_XactUndo(void);
extern void AtAbort_XactUndo(void);
extern void AtPrepare_XactUndo(void);
extern void AtSubCommit_XactUndo(int level);
extern void AtSubAbort_XactUndo(int level);
extern void AtProcExit_XactUndo(void);

extern bool XactUndoHasUnrecoverableUndo(void);

/* Undo chain traversal for rollback */
extern UndoRecPtr GetCurrentXactUndoRecPtr(UndoPersistenceLevel plevel);
extern XLogRecPtr GetCurrentXactLastBatchLSN(UndoPersistenceLevel plevel);
extern void XActUndoUpdateLastBatchLSN(XLogRecPtr lsn,
									   UndoPersistenceLevel plevel);

#endif							/* XACTUNDO_H */
