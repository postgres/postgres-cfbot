/*-------------------------------------------------------------------------
 *
 * flux_pbu.h
 *	  By-prototype interface to the FLUX per-backend UNDO shim (flux_pbu.c).
 *
 * The rest of the FLUX access method (flux_operations.c, flux_mvcc.c,
 * flux_pvs.c, flux_handler.c, flux_xlog.c)
 * cannot include the per-backend UNDO engine headers (they conflict).  It
 * therefore drives the per-backend insert / fetch / redo path through these
 * entry points, which are the only interface flux_pbu.c exposes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/access/flux_pbu.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FLUX_PBU_H
#define FLUX_PBU_H

#include "access/undodefs.h"	/* UndoRecPtr, InvalidUndoRecPtr */
#include "access/xlogdefs.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"		/* Relation */

/*
 * Maximum number of undo pages one FLUX undo record can span, hence the number
 * of block ids a FLUX forward-op WAL record can devote to undo.  A FLUX
 * before-image record can carry a whole tuple and span three pages.  Also
 * allow for the previous transaction header updated by the insert.
 */
#define FLUX_PBU_MAX_UNDO_BLOCKS	4

/*
 * WRITE PROTOCOL (see flux_pbu.c for the full choreography).  One record per
 * DML op:
 *   urp = FluxPbuPrepareInsert()     -- before crit; returns the new t_verptr
 *   FluxPbuInsertUndo()              -- in crit, before the FLUX forward WAL
 *   FluxPbuRegisterUndoBuffers(bid)  -- in crit, after XLogRegisterBuffer/Data
 *                                       and before XLogInsert (attach the undo
 *                                       pages to the FLUX forward record)
 *   FluxPbuFinishUndo(lsn)           -- after XLogInsert (stamp LSN, release)
 * or FluxPbuCancelUndo()             -- release without committing the record
 */
extern UndoRecPtr FluxPbuPrepareInsert(uint16 subtype, Relation rel,
									   ItemPointer tid,
									   const char *tuple_bytes,
									   uint32 tuple_len, char relpersistence,
									   bool reserve_indexes,
									   TransactionId freeze_conflict,
									   CommandId cid);
extern void FluxPbuAmendRevivedList(UndoRecPtr urp, const Oid *oids, uint16 n);
extern void FluxPbuInsertUndo(void);
extern int	FluxPbuRegisterUndoBuffers(uint8 first_block_id);
extern bool FluxPbuUndoPending(void);
extern void FluxPbuFinishUndo(XLogRecPtr lsn);
extern void FluxPbuCancelUndo(void);

/*
 * REDO: re-run the undo insert carried on a FLUX forward-op WAL record, so the
 * undo page content is rebuilt rather than restored from a forced full-page
 * image.  The record argument is the XLogReaderState* (typed void * so callers
 * need not expose the per-backend headers); first_block_id is where the emitter
 * started registering undo buffers.  The undo region locates itself from there
 * (undo blocks are the record's last blocks and the only ones in the reserved
 * undo database), so this is a no-op for records that carry no undo.
 */
extern UndoRecPtr FluxPbuRedoUndo(void *record, uint8 first_block_id);

/*
 * True iff the given block reference of a WAL record is a per-backend UNDO page
 * rather than a FLUX relation page.  The FLUX redo handlers use this to skip the
 * undo blocks folded onto a forward-op record when scanning block ids 1..N for
 * FLUX overflow pages.
 */
extern void FluxPbuResolveRecoveryConflict(void *record);
extern bool FluxPbuBlockIsUndo(void *record, uint8 block_id);

/*
 * READ: version-chain fetch for FluxReconstructVisibleVersion (full payload),
 * header-only xid probe for the lost-update conflict check + ANALYZE proxy,
 * and a discarded-or-not existence probe.
 */
extern bool FluxPbuFetchVersion(UndoRecPtr verptr, TransactionId *out_xid,
								CommandId *out_cid,
								uint16 *out_subtype, char **out_payload,
								Size *out_payload_len);
extern bool FluxPbuFetchXid(UndoRecPtr verptr, TransactionId *out_xid);
extern bool FluxPbuFetchForRevert(UndoRecPtr urp, TransactionId *out_top_xid,
								  uint16 *out_subtype, char **out_payload,
								  Size *out_payload_len);
extern bool FluxPbuRecordExists(UndoRecPtr verptr);

/*
 * Subtransaction (SAVEPOINT) rollback: physically restore the pre-savepoint
 * images by reverse-applying the aborting subxact's per-backend undo records,
 * at SUBXACT_EVENT_ABORT_SUB.  FluxPbuInSubxactApply() lets flux_undo_apply()
 * know to skip its top-xid sLog cleanup during a subxact apply.
 */
extern bool FluxPbuInSubxactApply(void);

#endif							/* FLUX_PBU_H */
