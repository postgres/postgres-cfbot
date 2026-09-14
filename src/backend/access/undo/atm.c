/*-------------------------------------------------------------------------
 *
 * atm.c
 *	  Aborted Transaction Map: aborted transactions whose UNDO is not applied
 *
 * The ATM is a shared-memory data structure mapping TransactionId to UNDO
 * chain metadata for aborted transactions. It enables:
 *
 *   1. Background Logical Revert: the Logical Revert worker scans the
 *      sLog for entries where revert_complete == false and applies their
 *      UNDO chains asynchronously.
 *
 *   2. Instant abort: at transaction abort time, the backend writes an
 *      ATM entry (sLog + WAL) instead of performing synchronous
 *      rollback, making ROLLBACK O(1).
 *
 * Implementation: All ATM functions are thin wrappers around the
 * transaction sLog (access/slog.h), a shared radix tree.
 *
 * WAL: ATMAddAborted() emits XLOG_ATM_ABORT; ATMForget() emits
 * XLOG_ATM_FORGET. During recovery, atm_redo() replays these without
 * re-emitting WAL.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/atm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/atm.h"
#include "access/slog.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "common/file_utils.h"
#include "pgstat.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/wait_event.h"

/*
 * On-disk ATM state file.
 *
 * The Aborted Transaction Map is a shared-memory-only structure otherwise
 * reconstructed only by replaying XLOG_ATM_ABORT / XLOG_ATM_FORGET during the
 * redo pass.  Because a checkpoint may advance the redo pointer PAST an
 * un-forgotten XLOG_ATM_ABORT, redo alone can miss aborts and silently lose a
 * guaranteed rollback.  To close that window we persist the map at each
 * checkpoint (CheckPointATM) and reload it at startup before redo
 * (ATMReloadFromCheckpoint), exactly mirroring how CheckPointTwoPhase /
 * restoreTwoPhaseData keep prepared-xact state that predates the redo point.
 *
 * The file is a single flat file in the data directory: a header, then a
 * packed array of records, then a CRC over header+records.  It is written to
 * a temporary name and durably renamed into place, so a torn write never
 * corrupts a previously good file.
 */
#define ATM_STATE_FILE		"pg_undo_atm"
#define ATM_STATE_TMP_FILE	"pg_undo_atm.tmp"
#define ATM_STATE_MAGIC		0x41544D34	/* "ATM4" */

typedef struct AtmStateHeader
{
	uint32		magic;
	uint32		count;			/* number of AtmStateRecord that follow */
} AtmStateHeader;

typedef struct AtmStateRecord
{
	TransactionId xid;
	Oid			reloid;
	Oid			dboid;
	XLogRecPtr	last_batch_lsn;
	XLogRecPtr	stop_lsn;
	XLogRecPtr	first_batch_lsn;
	bool		revert_complete;
	bool		fileops_owned;
} AtmStateRecord;

/* Internal helpers that skip WAL emission (used during redo) */
static bool ATMAddAbortedInternal(TransactionId xid, Oid dboid, Oid reloid,
								  XLogRecPtr last_batch_lsn,
								  XLogRecPtr stop_lsn,
								  XLogRecPtr first_batch_lsn, bool fileops_owned);
static void ATMForgetInternal(TransactionId xid);

/*
 * ATMShmemSize
 *		Calculate shared memory space needed for the ATM.
 *
 * The ATM is now backed by sLog, which manages its own shared memory.
 * ATM itself needs no additional shared memory.
 */
Size
ATMShmemSize(void)
{
	return 0;
}

/*
 * ATMShmemInit
 *		Initialize ATM shared memory (no-op, sLog handles it).
 */
void
ATMShmemInit(void)
{
	/* sLog initialization is done separately via SLogShmemInit() */
}

/*
 * ATMEnsureCreated
 *		Create the ATM's shared memory if nobody has yet.  Call it where an
 *		ERROR is still harmless, before a transaction may have to abort into
 *		the ATM: the abort path does not allocate it (SLogTxnInsert).
 */
void
ATMEnsureCreated(void)
{
	SLogTxnEnsureCreated();
}

/*
 * ATMGetLastBatchLSN
 *		Retrieve the WAL LSN of the last UNDO batch for an aborted transaction.
 *
 * Returns true if found, storing the LSN in *lsn_out.
 */
bool
ATMGetLastBatchLSN(TransactionId xid, XLogRecPtr *lsn_out)
{
	return SLogTxnLookupByXid(xid, lsn_out);
}

/*
 * ATMAddAbortedInternal
 *		Add an entry to the ATM without emitting WAL.
 *
 * For redo, crash-recovery UNDO, and restoring the checkpointed state.  An
 * ATM too full for the entry raises ERROR, failing startup rather than
 * dropping a rollback; it never returns false.
 */
static bool
ATMAddAbortedInternal(TransactionId xid, Oid dboid, Oid reloid,
					  XLogRecPtr last_batch_lsn, XLogRecPtr stop_lsn,
					  XLogRecPtr first_batch_lsn, bool fileops_owned)
{
	return SLogTxnInsert(xid, reloid, dboid, last_batch_lsn, stop_lsn,
						 first_batch_lsn, fileops_owned, false);
}

/*
 * ATMAddAborted
 *		Record an aborted transaction in the ATM with WAL logging.
 *
 * last_batch_lsn is the head of the UNDO chain, where the revert worker
 * starts; first_batch_lsn is its oldest batch, from which on WAL is retained
 * for as long as the entry exists (ATMGetOldestUnrevertedLSN).  The caller's
 * own retention (the per-backend slot, or the 2PC state) may be released once
 * this returns true.
 *
 * fileops_owned is true for online aborts: their abort WAL record carries
 * the FILEOPS inverses, which a later generic walk must not apply again.
 * Startup also passes true after completing a loser's FILEOPS inverses.
 * Preserve this ownership in both WAL and checkpoint state, including subxacts
 * whose records carry the top-level xid rather than the aborting subxid.
 *
 * Called from the abort path. Returns false if the sLog is full,
 * signaling the caller to fall back to synchronous rollback.
 */
bool
ATMAddAborted(TransactionId xid, Oid dboid, XLogRecPtr last_batch_lsn,
			  XLogRecPtr first_batch_lsn, bool fileops_owned)
{
	return ATMAddAbortedBounded(xid, dboid, last_batch_lsn, InvalidXLogRecPtr,
								first_batch_lsn, fileops_owned);
}

/*
 * ATMAddAbortedBounded
 *		Like ATMAddAborted(), but the revert worker applies only the batches
 *		strictly newer than stop_lsn (cf. ApplyUndoChainFromWALBounded).
 *
 * For a rolled-back subtransaction whose inline apply failed: the batches at
 * or below stop_lsn belong to its parent, which may yet commit.  The caller
 * passes the subtransaction's own xid, which no other entry can use.
 */
bool
ATMAddAbortedBounded(TransactionId xid, Oid dboid, XLogRecPtr last_batch_lsn,
					 XLogRecPtr stop_lsn, XLogRecPtr first_batch_lsn, bool fileops_owned)
{
	xl_atm_abort xlrec;

	/*
	 * Update shared memory first.  This runs while aborting, so it must not
	 * fail: if the ATM is full, report that without writing WAL, and the
	 * caller keeps its own WAL retention.  Writing the record first would
	 * leave redo with an entry the primary lacks.  The entry is visible
	 * before its record, but the revert worker skips it until the transaction
	 * is no longer in progress, which comes after the abort record that
	 * follows this one.
	 */
	if (!SLogTxnInsert(xid, InvalidOid, dboid, last_batch_lsn, stop_lsn,
					   first_batch_lsn, fileops_owned, true))
		return false;

	xlrec.xid = xid;
	xlrec.last_batch_lsn = last_batch_lsn;
	xlrec.stop_lsn = stop_lsn;
	xlrec.first_batch_lsn = first_batch_lsn;
	xlrec.fileops_owned = fileops_owned;
	xlrec.dboid = dboid;
	xlrec.reloid = InvalidOid;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfXlAtmAbort);
	XLogInsert(RM_ATM_ID, XLOG_ATM_ABORT);

	return true;
}

/*
 * ATMAddAbortedNoWAL
 *		Record an aborted transaction in the ATM before WAL can be written.
 *
 * For the crash-recovery UNDO phase, which finds loser transactions while
 * still in recovery.  The entry pins its UNDO WAL from this point on: the
 * end-of-recovery checkpoint's KeepLogSeg() sees it through
 * ATMGetOldestUnrevertedLSN(), and CheckPointATM() persists it.  The caller
 * must WAL-log it with ATMAddAborted() once WAL is writable, so standbys get
 * it too.  A recovery insert replaces any ownership marker from an online
 * abort that crashed before writing its abort record.
 */
void
ATMAddAbortedNoWAL(TransactionId xid, Oid dboid, XLogRecPtr last_batch_lsn,
				   XLogRecPtr first_batch_lsn, bool fileops_owned)
{
	(void) ATMAddAbortedInternal(xid, dboid, InvalidOid, last_batch_lsn,
								 InvalidXLogRecPtr, first_batch_lsn, fileops_owned);
}

/*
 * ATMForgetInternal
 *		Remove ATM entries for a transaction without emitting WAL.
 */
static void
ATMForgetInternal(TransactionId xid)
{
	SLogTxnRemoveByXid(xid);
}

/*
 * ATMForget
 *		Remove ATM entries after Logical Revert has completed.
 *
 * Emits a WAL record so that the removal survives recovery.
 */
void
ATMForget(TransactionId xid)
{
	xl_atm_forget xlrec;

	/* Write WAL first */
	xlrec.xid = xid;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfXlAtmForget);
	XLogInsert(RM_ATM_ID, XLOG_ATM_FORGET);

	/* Now update shared memory */
	ATMForgetInternal(xid);
}

/*
 * ATMMarkReverted
 *		Mark an ATM entry's revert as complete.
 *
 * The entry is kept in the ATM (for visibility checks) until ATMForget()
 * is called after the Logical Revert worker confirms all effects are gone.
 */
void
ATMMarkReverted(TransactionId xid)
{
	SLogTxnMarkReverted(xid);
}

/*
 * ATMGetNextUnreverted
 *		Find the next ATM entry of database dboid that hasn't been reverted
 *		yet, after after_xid (InvalidTransactionId: from the start).
 *
 * Used by the Logical Revert background worker to find work.
 *
 * Returns true if an unreverted entry was found, filling in the output
 * parameters.
 */
bool
ATMGetNextUnreverted(Oid dboid, TransactionId after_xid,
					 TransactionId *xid_out,
					 XLogRecPtr *lsn_out, XLogRecPtr *stop_lsn_out)
{
	return SLogTxnGetNextUnreverted(dboid, after_xid, xid_out, lsn_out,
									stop_lsn_out);
}

/*
 * ATMCollectUnrevertedDatabases
 *		Collect the distinct database OIDs that have unreverted ATM entries.
 *
 * Returns the count; fills dboids[] up to max_dboids.  Used by the logical
 * revert launcher to spawn workers only for databases that have
 * aborted-transaction UNDO to apply.
 */
int
ATMCollectUnrevertedDatabases(Oid *dboids, int max_dboids)
{
	return SLogTxnCollectUnrevertedDatabases(dboids, max_dboids);
}

/*
 * ATMGetOldestUnrevertedLSN
 *		Return the oldest UNDO WAL position any unreverted ATM entry needs,
 *		i.e. the oldest first_batch_lsn (the whole chain must stay readable).
 *
 * Used by the WAL retention logic to prevent recycling WAL segments that
 * still contain UNDO batches needed by the logical revert worker.
 * Returns InvalidXLogRecPtr if no unreverted entries exist.
 */
XLogRecPtr
ATMGetOldestUnrevertedLSN(void)
{
	return SLogTxnGetOldestUnrevertedLSN();
}

/*
 * ATMRecoveryFinalize
 *		Called at the end of recovery to log the ATM state.
 *
 * After WAL redo has reconstructed the ATM via sLog, this logs the number
 * of unreverted entries so the DBA can see how much Logical Revert work
 * remains, and the oldest unreverted last_batch_lsn -- the LSN that pins
 * UNDO WAL against recycling (ATMGetOldestUnrevertedLSN ->
 * UndoGetOldestBatchLSN -> KeepLogSeg) until the logical revert worker
 * forgets the entry.  Emitting the LSN here, before any worker runs, gives
 * an observable proof that the retention floor survived the crash.
 */
void
ATMRecoveryFinalize(void)
{
	int			total = 0;
	int			unreverted = 0;

	SLogRecoveryFinalize(&total, &unreverted);

	if (total > 0)
		elog(LOG, "ATM recovery complete: %d entries, %d unreverted, "
			 "oldest unreverted LSN %X/%X",
			 total, unreverted,
			 LSN_FORMAT_ARGS(ATMGetOldestUnrevertedLSN()));
}

/*
 * atm_redo
 *		WAL redo handler for ATM resource manager.
 */
void
atm_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_ATM_ABORT:
			{
				xl_atm_abort *xlrec =
					(xl_atm_abort *) XLogRecGetData(record);

				ATMAddAbortedInternal(xlrec->xid, xlrec->dboid,
									  xlrec->reloid, xlrec->last_batch_lsn,
									  xlrec->stop_lsn,
									  xlrec->first_batch_lsn, xlrec->fileops_owned);
			}
			break;

		case XLOG_ATM_FORGET:
			{
				xl_atm_forget *xlrec =
					(xl_atm_forget *) XLogRecGetData(record);

				ATMForgetInternal(xlrec->xid);
			}
			break;

		default:
			elog(PANIC, "atm_redo: unknown op code %u", info);
			break;
	}
}

/*
 * CheckPointATM
 *		Durably persist the Aborted Transaction Map at a checkpoint.
 *
 * Writes every current ATM entry (xid, reloid, dboid, last_batch_lsn,
 * revert_complete) to a single flat file, via a temporary file that is
 * durably renamed into place, so the map survives a crash even when the
 * checkpoint's redo pointer advances past the entries' XLOG_ATM_ABORT
 * records.  Called from CheckPointGuts(), outside any critical section
 * (palloc and file I/O are therefore safe), alongside CheckPointTwoPhase.
 *
 * We snapshot ALL entries, not only those with an XLOG_ATM_ABORT preceding
 * the redo point: an entry whose abort record follows the redo point will be
 * re-added by redo, and ATMAddAbortedInternal is idempotent on (xid, reloid)
 * (SLogTxnInsert preserves its chain and progress), so double-add across
 * the checkpoint boundary converges to a single entry.  Persisting the whole
 * map is simpler than partitioning it by redo point and is cheap because the
 * ATM only holds un-forgotten aborts.
 */
void
CheckPointATM(void)
{
	SLogTxnEntry *entries;
	int			count;
	AtmStateHeader hdr;
	pg_crc32c	crc;
	int			fd;
	int			i;

	count = SLogTxnSnapshotForCheckpoint(&entries);

	/*
	 * Always (re)write the file, even when empty, so a stale file from a
	 * checkpoint that had entries is replaced by an authoritative empty one.
	 * An empty file (count == 0) records "no un-forgotten aborts as of this
	 * redo point".
	 */
	hdr.magic = ATM_STATE_MAGIC;
	hdr.count = (uint32) count;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &hdr, sizeof(hdr));

	fd = OpenTransientFile(ATM_STATE_TMP_FILE,
						   O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						ATM_STATE_TMP_FILE)));

	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_WRITE);
	errno = 0;
	if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr))
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						ATM_STATE_TMP_FILE)));
	}

	for (i = 0; i < count; i++)
	{
		AtmStateRecord rec;

		rec.xid = entries[i].xid;
		rec.reloid = entries[i].reloid;
		rec.dboid = entries[i].dboid;
		rec.last_batch_lsn = entries[i].last_batch_lsn;
		rec.stop_lsn = entries[i].stop_lsn;
		rec.first_batch_lsn = entries[i].first_batch_lsn;
		rec.fileops_owned = entries[i].fileops_owned;
		rec.revert_complete = entries[i].revert_complete;

		COMP_CRC32C(crc, &rec, sizeof(rec));

		errno = 0;
		if (write(fd, &rec, sizeof(rec)) != sizeof(rec))
		{
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							ATM_STATE_TMP_FILE)));
		}
	}

	FIN_CRC32C(crc);
	errno = 0;
	if (write(fd, &crc, sizeof(crc)) != sizeof(crc))
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						ATM_STATE_TMP_FILE)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						ATM_STATE_TMP_FILE)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						ATM_STATE_TMP_FILE)));

	/* Atomically replace the live file, fsyncing both file and directory. */
	durable_rename(ATM_STATE_TMP_FILE, ATM_STATE_FILE, ERROR);

	if (entries != NULL)
		pfree(entries);
}

/*
 * ATMReloadFromCheckpoint
 *		Reconstruct the ATM from the checkpoint state file, before redo.
 *
 * Called from StartupXLOG at the same point as restoreTwoPhaseData(), i.e.
 * BEFORE the redo pass.  Each persisted entry is re-inserted into the map
 * without emitting WAL (ATMAddAbortedInternal), preserving its
 * revert_complete flag.  Reloading before redo is essential: an
 * XLOG_ATM_FORGET replayed after the checkpoint must be able to remove an
 * entry that WAS persisted, and an XLOG_ATM_ABORT replayed after the
 * checkpoint re-adds idempotently on top of what we loaded.
 *
 * A missing file (fresh initdb, or a cluster that never checkpointed the
 * ATM) is not an error: it means "no persisted entries".  A file that fails
 * its CRC or magic check is treated as absent with a warning; every entry it
 * could have held is still WAL-durable via XLOG_ATM_ABORT, and any whose
 * abort record precedes the redo point would then be lost -- but a corrupt
 * ATM state file implies a torn write that durable_rename is designed to
 * prevent, so this is a belt-and-suspenders path, not an expected one.
 */
void
ATMReloadFromCheckpoint(void)
{
	int			fd;
	AtmStateHeader hdr;
	pg_crc32c	crc,
				file_crc;
	AtmStateRecord *records = NULL;
	uint32		i;
	ssize_t		nread;

	fd = OpenTransientFile(ATM_STATE_FILE, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return;				/* no persisted state -- nothing to reload */
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", ATM_STATE_FILE)));
	}

	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_READ);
	nread = read(fd, &hdr, sizeof(hdr));
	if (nread != sizeof(hdr) || hdr.magic != ATM_STATE_MAGIC)
	{
		pgstat_report_wait_end();
		CloseTransientFile(fd);
		ereport(WARNING,
				(errmsg("ignoring ATM state file \"%s\" with invalid header",
						ATM_STATE_FILE)));
		return;
	}

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &hdr, sizeof(hdr));

	if (hdr.count > 0)
	{
		Size		bytes = (Size) hdr.count * sizeof(AtmStateRecord);

		records = (AtmStateRecord *) palloc(bytes);
		nread = read(fd, records, bytes);
		if (nread != (ssize_t) bytes)
		{
			pgstat_report_wait_end();
			CloseTransientFile(fd);
			pfree(records);
			ereport(WARNING,
					(errmsg("ignoring truncated ATM state file \"%s\"",
							ATM_STATE_FILE)));
			return;
		}
		COMP_CRC32C(crc, records, bytes);
	}

	nread = read(fd, &file_crc, sizeof(file_crc));
	pgstat_report_wait_end();
	CloseTransientFile(fd);

	FIN_CRC32C(crc);
	if (nread != sizeof(file_crc) || !EQ_CRC32C(crc, file_crc))
	{
		if (records != NULL)
			pfree(records);
		ereport(WARNING,
				(errmsg("ignoring ATM state file \"%s\" with bad checksum",
						ATM_STATE_FILE)));
		return;
	}

	for (i = 0; i < hdr.count; i++)
	{
		ATMAddAbortedInternal(records[i].xid, records[i].dboid,
							  records[i].reloid, records[i].last_batch_lsn,
							  records[i].stop_lsn,
							  records[i].first_batch_lsn, records[i].fileops_owned);
		if (records[i].revert_complete)
			ATMMarkReverted(records[i].xid);
	}

	if (records != NULL)
		pfree(records);

	if (hdr.count > 0)
		elog(LOG, "ATM reloaded %u entries from checkpoint state file",
			 hdr.count);
}
