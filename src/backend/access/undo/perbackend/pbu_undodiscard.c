/*-------------------------------------------------------------------------
 *
 * undodiscard.c
 *	  discard undo records
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undodiscard.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/perbackend/pbu_compat.h"
#include "access/perbackend/pbu_helpers.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "access/perbackend/pbu_undolog.h"
#include "access/perbackend/pbu_undodiscard.h"
#include "access/perbackend/pbu_undorequest.h"
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "utils/resowner.h"

/*
 * Discard the undo for the given log
 *
 * Search the undo log, get the start record for each transaction until we get
 * the transaction with xid >= xmin or an invalid xid.  Then call undolog
 * routine to discard upto that point and update the memory structure for the
 * log slot.  We set the hibernate flag if we do not have any undo data that
 * can be discarded, this flag is passed to the discard worker wherein it
 * determines if system is idle and it should sleep for sometime.
 *
 * Return the oldest xid remaining in this undo log (which should be >= xmin,
 * since we'll discard everything older).  Return InvalidTransactionId if the
 * undo log is empty.
 */
static FullTransactionId
UndoDiscardOneLog(UndoLogControl *log, TransactionId xmin, bool *hibernate)
{
	UndoRecPtr	undo_recptr,
				next_insert;
	UndoRecPtr	next_urecptr = InvalidUndoRecPtr;
	UnpackedUndoRecord *uur = NULL;
	bool		need_discard = false;
	bool		log_complete = false;
	TransactionId undoxid = InvalidTransactionId;
	TransactionId latest_discardxid = InvalidTransactionId;
	uint32		epoch = 0;

	/* There might not be any undo log and hibernation might be needed. */
	*hibernate = true;

	/*
	 * A discard pointer past the insert pointer means the log's state is not
	 * one WAL replay can have produced from a consistent log: there is no
	 * record at the discard pointer to read.  Leave such a log alone rather
	 * than walk it.
	 */
	LWLockAcquire(&log->mutex, LW_SHARED);
	if (log->meta.discard > log->meta.insert)
	{
		UndoLogOffset discard = log->meta.discard;
		UndoLogOffset insert = log->meta.insert;

		LWLockRelease(&log->mutex);
		if (!log->discard_corrupt_reported)
		{
			log->discard_corrupt_reported = true;
			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("undo log %u has discard pointer " UndoRecPtrFormat " past its insert pointer " UndoRecPtrFormat ", not discarding it",
							log->logno,
							(unsigned long long) MakeUndoRecPtr(log->logno, discard),
							(unsigned long long) MakeUndoRecPtr(log->logno, insert))));
		}
		return InvalidFullTransactionId;
	}
	LWLockRelease(&log->mutex);

	if (UndoRecPtrIsValid(log->oldest_data))
		undo_recptr = log->oldest_data;
	else
		undo_recptr = UndoLogGetFirstValidRecord(log->logno);

	StartTransactionCommand();

	/* Loop until we run out of discardable transactions in the given log. */
	do
	{
		bool		pending_abort = false;

		/*
		 * A log with many transactions takes a while to walk: let a shutdown
		 * request end the pass.  The next pass starts again from the shared
		 * oldest_data, which only moves forward.
		 */
		CHECK_FOR_INTERRUPTS();
		if (ShutdownRequestPending)
			break;

		next_insert = UndoLogGetNextInsertPtr(log->logno, InvalidTransactionId);

		if (next_insert == undo_recptr)
		{
			/*
			 * The caller of this function must have ensured that there is
			 * something to discard.
			 */
			Assert(undo_recptr != log->oldest_data);

			/* Indicate that we have processed all the log. */
			log_complete = true;
		}
		else
		{
			/* Fetch the undo record for given undo_recptr. */
			uur = UndoFetchRecord(undo_recptr, InvalidBlockNumber,
								  InvalidOffsetNumber, InvalidTransactionId,
								  NULL, NULL);

			if (uur != NULL)
			{
				/*
				 * Add the aborted transaction to the rollback request queues.
				 *
				 * If the undo actions for the aborted transaction is already
				 * applied then continue discarding the undo log, otherwise,
				 * discard till current point and stop processing this undo
				 * log.
				 *
				 * We can ignore the abort for transactions whose
				 * corresponding database doesn't exist.
				 *
				 * Note: We've added the transaction-in-progress check to
				 * avoid xids of in-progress autovacuum.  Note that, while
				 * calculating xmin, we ignore the autovacuum xids in
				 * DiscardWorkerMain, so an aborted-looking xid below xmin may
				 * belong to an autovacuum that is still running. Counting
				 * autovacuum xids in xmin would avoid that, but a
				 * long-running autovacuum would then hold back discard.
				 */
				if (!TransactionIdDidCommit(uur->uur_xid) &&
					!TransactionIdIsInProgress(uur->uur_xid) &&
					TransactionIdPrecedes(uur->uur_xid, xmin) &&
					uur->uur_progress == 0 &&
					dbid_exists(uur->uur_dbid))
				{
					FullTransactionId full_xid;

					full_xid = FullTransactionIdFromEpochAndXid(uur->uur_xidepoch,
																uur->uur_xid);
					(void) RegisterRollbackReq(InvalidUndoRecPtr,
											   undo_recptr,
											   uur->uur_dbid,
											   full_xid);

					pending_abort = true;
				}

				next_urecptr = uur->uur_next;
				undoxid = uur->uur_xid;
				epoch = uur->uur_xidepoch;

				UndoRecordRelease(uur);
				uur = NULL;
			}
		}

		/*
		 * We can discard upto this point when one of following conditions is
		 * met: (a) the next transaction is not all-visible. (b) there is no
		 * more log to process. (c) the transaction undo in current log is
		 * finished. (d) there is a pending abort.
		 */
		if ((TransactionIdIsValid(undoxid) &&
			 TransactionIdFollowsOrEquals(undoxid, xmin)) ||
			next_urecptr == InvalidUndoRecPtr ||
			log_complete ||
			UndoRecPtrGetLogNo(next_urecptr) != log->logno ||
			pending_abort)
		{
			/* Hey, I got some undo log to discard, can not hibernate now. */
			*hibernate = false;

			/*
			 * If the transaction id is smaller than the xmin, it means this
			 * must be the last transaction in this undo log, so we need to
			 * get the last insert point in this undo log and discard till
			 * that point.
			 *
			 * Also, if the transaction has pending abort, stop discarding
			 * further.
			 */
			if (TransactionIdPrecedes(undoxid, xmin) && !pending_abort)
			{
				UndoRecPtr	blk_next_insert = InvalidUndoRecPtr;

				/*
				 * If the more undo has been inserted since last we checked,
				 * then we can process that as well.
				 */
				blk_next_insert = UndoLogGetNextInsertPtr(log->logno, undoxid);

				/*
				 * Another transaction has attached to the log since we read
				 * the record: its undo follows, and the next pass will find
				 * it.  End this pass here; retrying now would reread the same
				 * state and never get further.
				 */
				if (!UndoRecPtrIsValid(blk_next_insert))
					break;

				undo_recptr = blk_next_insert;
				need_discard = true;
				epoch = 0;
				latest_discardxid = undoxid;
				undoxid = InvalidTransactionId;
			}

			/* Update the shared memory state. */
			LWLockAcquire(&log->discard_lock, LW_EXCLUSIVE);

			/*
			 * If no more pending undo logs then set the oldest transaction to
			 * InvalidTransactionId.
			 */
			if (log_complete)
			{
				log->oldest_xid = InvalidTransactionId;
				log->oldest_xidepoch = 0;
			}
			else
			{
				log->oldest_xid = undoxid;
				log->oldest_xidepoch = epoch;
			}

			log->oldest_data = undo_recptr;

			LWLockRelease(&log->discard_lock);

			if (need_discard)
			{
				LWLockAcquire(&log->discard_update_lock, LW_EXCLUSIVE);
				PbuUndoLogDiscard(undo_recptr, latest_discardxid);
				LWLockRelease(&log->discard_update_lock);
			}

			break;
		}

		/*
		 * This transaction is smaller than the xmin so lets jump to the next
		 * transaction.
		 */
		undo_recptr = next_urecptr;
		latest_discardxid = undoxid;

		Assert(uur == NULL);

		need_discard = true;
	} while (true);

	CommitTransactionCommand();

	return FullTransactionIdFromEpochAndXid(epoch, undoxid);
}

/*
 * Discard the undo for all the transactions whose xid is smaller than
 * oldestXmin
 */
void
UndoDiscard(TransactionId oldestXmin, bool *hibernate)
{
	FullTransactionId oldestXidHavingUndo;
	UndoLogControl *log = NULL;
	uint32		epoch;

	/*
	 * If all the undo logs are discarded, then oldestXidHavingUndo should be
	 * oldestXmin.  As of now, we don't allow more than 2 billion xids in the
	 * system, so we can rely on the epoch retrieved with GetEpochForXid.
	 */
	epoch = GetEpochForXid(oldestXmin);
	oldestXidHavingUndo = FullTransactionIdFromEpochAndXid(epoch, oldestXmin);

	/*
	 * Iterate through all the active logs and one-by-one try to discard the
	 * transactions that are old enough to matter.
	 *
	 * Note: Ideally we can arrange undo logs so that we can efficiently find
	 * those with oldest_xid < oldestXmin, but for now we'll just scan all of
	 * them.
	 */
	while ((log = UndoLogNext(log)))
	{
		FullTransactionId oldest_xid = InvalidFullTransactionId;

		/*
		 * If the log is already discarded, then we are done.  It is important
		 * to first check this to ensure that tablespace containing this log
		 * doesn't get dropped concurrently.
		 */
		LWLockAcquire(&log->mutex, LW_SHARED);
		if (log->meta.discard == log->meta.insert)
		{
			LWLockRelease(&log->mutex);
			continue;
		}
		LWLockRelease(&log->mutex);

		/* We can't process temporary undo logs. */
		if (log->meta.persistence == UNDO_TEMP)
			continue;

		/*
		 * If the first xid of the undo log is smaller than the xmin the try
		 * to discard the undo log.
		 */
		if (!TransactionIdIsValid(log->oldest_xid) ||
			TransactionIdPrecedes(log->oldest_xid, oldestXmin))
		{
			/* Process the undo log. */
			oldest_xid = UndoDiscardOneLog(log, oldestXmin, hibernate);
		}

		/* If oldestXidHavingUndo is not yet initialized, initialize it. */
		if (!FullTransactionIdIsValid(oldestXidHavingUndo))
			oldestXidHavingUndo = oldest_xid;
		else if (FullTransactionIdIsValid(oldest_xid) &&
				 FullTransactionIdPrecedes(oldest_xid, oldestXidHavingUndo))
			oldestXidHavingUndo = oldest_xid;
	}

	/*
	 * Update the oldestXidWithEpochHavingUndo in the shared memory.
	 *
	 * Note: In future if multiple worker can perform discard then we may need
	 * to use compare and swap for updating the shared memory value.
	 */
	if (FullTransactionIdIsValid(oldestXidHavingUndo))
		pg_atomic_write_u64(UndoOldestXidHavingUndo_pbu,
							U64FromFullTransactionId(oldestXidHavingUndo));
}

/*
 * To discard all the logs. Particularly required in single user mode.
 * At the commit time, discard all the undo logs.
 */
void
UndoLogDiscardAll(void)
{
	UndoLogControl *log = NULL;

	Assert(!IsUnderPostmaster);

	while ((log = UndoLogNext(log)))
	{
		/*
		 * Process the undo log.  No locks are required for discard, since
		 * this called only in single-user mode. Similarly, no transaction id
		 * is required here because WAL-logging the xid till where the undo is
		 * discarded will not be required for single user mode.
		 */
		PbuUndoLogDiscard(MakeUndoRecPtr(log->logno, log->meta.insert),
						  InvalidTransactionId);
	}

}
