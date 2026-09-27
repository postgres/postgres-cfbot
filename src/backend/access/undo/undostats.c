/*-------------------------------------------------------------------------
 *
 * undostats.c
 *	  UNDO log statistics collection and reporting
 *
 * The cluster-wide UNDO engine keeps its records in the WAL stream, so what
 * there is to monitor is what holds that WAL back: in-flight transactions
 * that wrote UNDO, aborted transactions waiting for the logical revert
 * worker, and prepared transactions.  pg_stat_get_undo_retention() reports
 * them, using the same sources KeepLogSeg() does (UndoGetOldestBatchLSN).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undostats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/slog.h"
#include "access/twophase.h"
#include "access/undolog.h"
#include "access/undostats.h"
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/tuplestore.h"

#define UNDO_RETENTION_COLS 6

static void
undo_retention_row(ReturnSetInfo *rsinfo, XLogRecPtr cur, const char *holder,
				   int pid, TransactionId xid, Oid dboid, XLogRecPtr lsn)
{
	Datum		values[UNDO_RETENTION_COLS];
	bool		nulls[UNDO_RETENTION_COLS] = {0};

	values[0] = CStringGetTextDatum(holder);
	values[1] = Int32GetDatum(pid);
	nulls[1] = (pid == 0);
	values[2] = TransactionIdGetDatum(xid);
	nulls[2] = !TransactionIdIsValid(xid);
	values[3] = ObjectIdGetDatum(dboid);
	nulls[3] = !OidIsValid(dboid);
	values[4] = LSNGetDatum(lsn);
	values[5] = Int64GetDatum(cur > lsn ? (int64) (cur - lsn) : 0);

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
}

/*
 * pg_stat_get_undo_retention - What is keeping UNDO WAL from being recycled
 *
 * One row per holder, with the oldest WAL position it needs and how much WAL
 * that retains:
 *   holder 'backend'  - an in-flight transaction that has written UNDO
 *                       (pid, xid and database of the backend)
 *   holder 'aborted'  - an ATM entry waiting for the logical revert worker
 *   holder 'prepared' - the oldest UNDO batch of all prepared transactions
 *
 * The minimum oldest_lsn over all rows is what UndoGetOldestBatchLSN() feeds
 * into KeepLogSeg().
 */
Datum
pg_stat_get_undo_retention(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	XLogRecPtr	cur;
	XLogRecPtr	lsn;
	SLogTxnEntry *entries;
	int			n;

	InitMaterializedSRF(fcinfo, 0);

	if (UndoLogShared == NULL)
		return (Datum) 0;

	cur = RecoveryInProgress() ? GetXLogReplayRecPtr(NULL) :
		GetXLogInsertRecPtr();

	for (int i = 0; i < MaxBackends; i++)
	{
		PGPROC	   *proc;

		lsn = (XLogRecPtr)
			pg_atomic_read_u64(&UndoLogShared->backend_undo_lsns[i]);
		if (!XLogRecPtrIsValid(lsn))
			continue;
		proc = GetPGProcByNumber(i);
		undo_retention_row(rsinfo, cur, "backend", proc->pid, proc->xid,
						   proc->databaseId, lsn);
	}

	n = SLogTxnSnapshotForCheckpoint(&entries);
	for (int i = 0; i < n; i++)
	{
		if (entries[i].revert_complete)
			continue;
		lsn = XLogRecPtrIsValid(entries[i].first_batch_lsn) ?
			entries[i].first_batch_lsn : entries[i].last_batch_lsn;
		undo_retention_row(rsinfo, cur, "aborted", 0, entries[i].xid,
						   entries[i].dboid, lsn);
	}

	lsn = TwoPhaseGetOldestUndoBatchLSN();
	if (XLogRecPtrIsValid(lsn))
		undo_retention_row(rsinfo, cur, "prepared", 0, InvalidTransactionId,
						   InvalidOid, lsn);

	return (Datum) 0;
}
