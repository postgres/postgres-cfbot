/*-------------------------------------------------------------------------
 *
 * atm.h
 *	  Aborted Transaction Map for CTR (Constant-Time Recovery)
 *
 * The ATM is a shared-memory structure that tracks aborted transactions
 * whose UNDO chains have not yet been applied (Logical
 * Revert). It drives the background Logical Revert worker.
 *
 * The ATM is now backed by the sLog (Secondary Log) shared-memory hash
 * tables defined in access/slog.h.  All ATM functions are thin wrappers
 * around sLog operations, preserving the existing API and WAL format.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/atm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ATM_H
#define ATM_H

#include "access/atm_xlog.h"
#include "access/transam.h"
#include "datatype/timestamp.h"
#include "storage/lwlock.h"

/* Shared memory sizing and initialization */
extern Size ATMShmemSize(void);
extern void ATMShmemInit(void);

/* Core API */
extern bool ATMGetLastBatchLSN(TransactionId xid, XLogRecPtr *lsn_out);
extern bool ATMAddAborted(TransactionId xid, Oid dboid,
						  XLogRecPtr last_batch_lsn,
						  XLogRecPtr first_batch_lsn);
extern bool ATMAddAbortedBounded(TransactionId xid, Oid dboid,
								 XLogRecPtr last_batch_lsn,
								 XLogRecPtr stop_lsn,
								 XLogRecPtr first_batch_lsn);
extern void ATMAddAbortedNoWAL(TransactionId xid, Oid dboid,
							   XLogRecPtr last_batch_lsn,
							   XLogRecPtr first_batch_lsn);
extern void ATMForget(TransactionId xid);
extern void ATMMarkReverted(TransactionId xid);

/* Iteration for Logical Revert worker */
extern bool ATMGetNextUnreverted(Oid dboid, TransactionId after_xid,
								 TransactionId *xid_out,
								 XLogRecPtr *lsn_out, XLogRecPtr *stop_lsn_out);
extern int	ATMCollectUnrevertedDatabases(Oid *dboids, int max_dboids);

/* WAL retention: oldest batch LSN across unreverted entries */
extern XLogRecPtr ATMGetOldestUnrevertedLSN(void);

/* Recovery support */
extern void ATMRecoveryFinalize(void);

/*
 * Crash-safety: persist the ATM at each checkpoint and reload it at startup
 * before the redo pass.  The ATM lives only in shared memory and is otherwise
 * reconstructed solely by replaying XLOG_ATM_ABORT / XLOG_ATM_FORGET during
 * redo; a checkpoint that advances the redo pointer past an un-forgotten
 * XLOG_ATM_ABORT would make crash recovery miss that abort, silently losing a
 * guaranteed rollback.  CheckPointATM() durably snapshots the map; startup
 * calls ATMReloadFromCheckpoint() before redo so atm_redo's XLOG_ATM_FORGET
 * replays can correctly remove entries forgotten after the checkpoint.
 */
extern void CheckPointATM(void);
extern void ATMReloadFromCheckpoint(void);

#endif							/* ATM_H */
