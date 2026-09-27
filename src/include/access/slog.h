/*-------------------------------------------------------------------------
 *
 * slog.h
 *	  Secondary Log (sLog): shared state of the UNDO subsystem that is not WAL
 *
 * The transaction sLog (slog.c) is the Aborted Transaction Map (ATM): the
 * aborted transactions whose UNDO has not been applied yet, in a radix tree
 * keyed by (xid, reloid) packed into a uint64, so that one xid's entries are
 * contiguous.  Protected by a single LWLock (modifications are infrequent).
 *
 * The tuple sLog (slog_tuple.c) is per-tuple state of UNDO-based table AMs:
 * who holds row locks on their tuples.  It keys on physical tuple identity
 * (relid, relnumber, tid) and knows neither any AM's page format nor either
 * UNDO engine.
 *
 * WAL: Transaction sLog reuses existing RM_ATM_ID records.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/slog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLOG_H
#define SLOG_H

#include "access/transam.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "nodes/lockoptions.h"
#include "storage/itemptr.h"
#include "storage/lwlock.h"
#include "storage/relfilelocator.h"
#include "utils/dsa.h"

/* ----------------------------------------------------------------
 * Transaction sLog structures
 *
 * SLogTxnEntry is used as the public output type for lookups.
 * Internally, the ATM radix tree stores only the data fields; the key
 * (xid, reloid) is implicit in the tree path.
 * ----------------------------------------------------------------
 */

/*
 * SLogTxnEntry - Public output structure for transaction lookups.
 * Callers receive copies of this via SLogTxnLookup().
 */
typedef struct SLogTxnEntry
{
	TransactionId xid;
	Oid			reloid;
	XLogRecPtr	last_batch_lsn; /* LSN of last UNDO batch for this xid */
	XLogRecPtr	stop_lsn;		/* apply only batches newer than this, or
								 * InvalidXLogRecPtr for the whole chain */
	XLogRecPtr	first_batch_lsn;	/* oldest batch of the chain: pins WAL */
	Oid			dboid;			/* database OID */
	TimestampTz abort_time;		/* when transaction aborted */
	bool		revert_complete;	/* has Logical Revert finished? */
	bool		fileops_owned;	/* FILEOPS completed or owned by abort WAL */
} SLogTxnEntry;

/* ----------------------------------------------------------------
 * Shared state
 *
 * The transaction radix tree is allocated in shared memory; its internal
 * structures are opaque to callers.  The SLogSharedState is defined in
 * slog.c.
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 * API: Shared memory
 * ----------------------------------------------------------------
 */
extern Size SLogShmemSize(void);
extern void SLogShmemRequest(void);
extern void SLogShmemInit(void);

/* ----------------------------------------------------------------
 * API: Transaction sLog
 * ----------------------------------------------------------------
 */
extern bool SLogTxnInsert(TransactionId xid, Oid reloid, Oid dboid,
						  XLogRecPtr last_batch_lsn, XLogRecPtr stop_lsn,
						  XLogRecPtr first_batch_lsn, bool fileops_owned,
						  bool check_space);
extern bool SLogTxnLookup(TransactionId xid, Oid reloid,
						  SLogTxnEntry *entry_out);
extern bool SLogTxnLookupByXid(TransactionId xid, XLogRecPtr *lsn_out);
extern void SLogTxnRemove(TransactionId xid, Oid reloid);
extern void SLogTxnRemoveByXid(TransactionId xid);
extern void SLogTxnMarkReverted(TransactionId xid);
extern bool SLogTxnGetNextUnreverted(Oid dboid, TransactionId after_xid,
									 TransactionId *xid_out,
									 XLogRecPtr *lsn_out,
									 XLogRecPtr *stop_lsn_out);
extern int	SLogTxnCollectUnrevertedDatabases(Oid *dboids, int max_dboids);
extern XLogRecPtr SLogTxnGetOldestUnrevertedLSN(void);
extern int	SLogTxnSnapshotForCheckpoint(SLogTxnEntry **entries_out);
extern void SLogRecoveryFinalize(int *total_out, int *unreverted_out);

/* Create the ATM before a transaction may need to abort into it */
extern void SLogTxnEnsureCreated(void);

/* GUC: maximum DSA size (in MB) */
extern int	slog_dsa_max_size_mb;

/* ----------------------------------------------------------------
 * API: Tuple sLog (slog_tuple.c)
 * ----------------------------------------------------------------
 */

/*
 * Row locks on tuples of UNDO-based table AMs, in on-demand shared memory.
 * The AM calls SLogTupleLockAcquire under the tuple's buffer lock before it
 * changes the page, and SLogTupleLockReleaseAll at top-level commit or
 * abort.  Subtransaction end needs nothing: a lock whose subtransaction
 * aborted is dead.
 */
typedef void (*SLogTupleLockCallback) (Oid relid, RelFileNumber relnumber,
									   ItemPointer tid, LockTupleMode mode,
									   void *arg);
extern void SLogTupleLockAcquire(Oid relid, RelFileNumber relnumber,
								 ItemPointer tid, LockTupleMode mode);
extern bool SLogTupleLockHeldByMe(Oid relid, RelFileNumber relnumber,
								  ItemPointer tid, LockTupleMode *mode);
extern TransactionId SLogTupleLockConflictXid(Oid relid,
											  RelFileNumber relnumber,
											  ItemPointer tid,
											  LockTupleMode mode);
extern void SLogTupleLockReleaseAll(void);
extern void SLogTupleLockForEachMine(SLogTupleLockCallback cb, void *arg);
extern void SLogTupleLockAtPrepare(FullTransactionId fxid, bool any_live);
extern void SLogTupleLockRestore(FullTransactionId fxid, Oid relid,
								 RelFileNumber relnumber, ItemPointer tid,
								 LockTupleMode mode);
extern void SLogTupleLockReleasePrepared(FullTransactionId fxid);
extern void SLogTupleForgetStorage(Oid relid, RelFileNumber relnumber);

#endif							/* SLOG_H */
