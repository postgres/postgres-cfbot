/*-------------------------------------------------------------------------
 *
 * undoaction.c
 *	  execute undo actions
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undoaction.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/perbackend/pbu_compat.h"

#include "access/table.h"
#include "access/perbackend/pbu_undoaction_xlog.h"
#include "access/perbackend/pbu_undolog.h"
#include "access/perbackend/pbu_undorequest.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlog_internal.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/smgr.h"
#include "utils/relfilenumbermap.h"
#include "utils/injection_point.h"
#include "utils/syscache.h"
#include "utils/memutils.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "access/perbackend/pbu_undodiscard.h"
#include "access/undormgr.h"

/*
 * PrefetchUndoPages - Prefetch undo pages
 *
 * Prefetch undo pages, if prefetch_pages are behind prefetch_target
 */
static void
PrefetchUndoPages(RelFileNode rnode, int prefetch_target, int *prefetch_pages,
				  BlockNumber to_blkno, BlockNumber from_blkno,
				  char persistence)
{
	int			nprefetch;
	BlockNumber startblock;
	BlockNumber lastprefetched;

	/* Calculate last prefetched page in the previous iteration. */
	lastprefetched = from_blkno - *prefetch_pages;

	/* We have already prefetched all the pages of the transaction's undo. */
	if (lastprefetched <= to_blkno)
		return;

	/* Calculate number of blocks to be prefetched. */
	nprefetch =
		Min(prefetch_target - *prefetch_pages, lastprefetched - to_blkno);

	/* Where to start prefetch. */
	startblock = lastprefetched - nprefetch;

	while (nprefetch--)
	{
		PrefetchSharedBuffer(smgropen(rnode, INVALID_PROC_NUMBER),
							 MAIN_FORKNUM, startblock++);
		(*prefetch_pages)++;
	}
}

/*
 * UndoRecordBulkFetch  - Read undo records in bulk
 *
 * Read undo records between from_urecptr and to_urecptr until we exhaust the
 * the memory size specified by undo_apply_size.  If we could not read all the
 * records till to_urecptr then the caller should consume current set of records
 * and call this function again.
 *
 * from_urecptr		- Where to start fetching the undo records.  If we can not
 *					  read all the records because of memory limit then this
 *					  will be set to the previous undo record pointer from where
 *					  we need to start fetching on next call. Otherwise it will
 *					  be set to InvalidUndoRecPtr.
 * to_urecptr		- Last undo record pointer to be fetched.
 * undo_apply_size	- Memory segment limit to collect undo records.
 * nrecords			- Number of undo records read.
 * one_page			- Caller is applying undo only for one block not for
 *					  complete transaction.  If this is set true then instead of
 *					  following transaction undo chain using prevlen we will
 *					  follow the block prev chain of the block so that we can
 *					  avoid reading many unnecessary undo records of the
 *					  transaction.
 */
UndoRecInfo *
UndoRecordBulkFetch(UndoRecPtr *from_urecptr, UndoRecPtr to_urecptr,
					Size undo_apply_size, int *nrecords, bool one_page)
{
	RelFileNode rnode;
	UndoRecPtr	urecptr,
				prev_urec_ptr;
	BlockNumber blkno;
	BlockNumber to_blkno;
	Buffer		buffer = InvalidBuffer;
	UnpackedUndoRecord *uur = NULL;
	UndoRecInfo *urp_array;
	int			urp_array_size = 1024;
	int			urp_index = 0;
	int			prefetch_target = 0;
	int			prefetch_pages = 0;
	Size		total_size = 0;
	TransactionId xid = InvalidTransactionId;

	/*
	 * In one_page mode we are fetching undo only for one page instead of
	 * fetching all the undo of the transaction.  Basically, we are fetching
	 * interleaved undo records.  So it does not make sense to do any prefetch
	 * in that case.
	 */
	if (!one_page)
		prefetch_target = target_prefetch_pages;

	/*
	 * Allocate initial memory to hold the undo record info, we can expand it
	 * if needed.
	 */
	urp_array = (UndoRecInfo *) palloc(sizeof(UndoRecInfo) * urp_array_size);
	urecptr = *from_urecptr;

	prev_urec_ptr = InvalidUndoRecPtr;
	*from_urecptr = InvalidUndoRecPtr;

	/* Read undo chain backward until we reach to the first undo record.  */
	do
	{
		BlockNumber from_blkno;
		UndoLogControl *log;
		UndoPersistence persistence;
		int			size;
		int			logno;

		logno = UndoRecPtrGetLogNo(urecptr);
		log = UndoLogGet(logno);

		/*
		 * The log is gone (already discarded, or urecptr is stale -- e.g. a
		 * slot a concurrent transaction reset, whose log number decodes to
		 * 0).  UndoLogGet() returns NULL rather than an uninitialised
		 * UndoLogControl; every other caller treats that as "the record is
		 * gone".  Stop the walk and return the records read so far (each is
		 * already in urp_array; uur must not be freed here).
		 */
		if (log == NULL)
			break;

		persistence = log->meta.persistence;

		UndoRecPtrAssignRelFileNode(rnode, urecptr);
		to_blkno = UndoRecPtrGetBlockNum(to_urecptr);
		from_blkno = UndoRecPtrGetBlockNum(urecptr);

		/* Allocate memory for next undo record. */
		uur = palloc0(sizeof(UnpackedUndoRecord));

		/*
		 * If next undo record pointer to be fetched is not on the same block
		 * then release the old buffer and reduce the prefetch_pages count by
		 * one as we have consumed one page. Otherwise, just set the old
		 * buffer into the new undo record so that UndoGetOneRecord don't read
		 * the buffer again.
		 */
		blkno = UndoRecPtrGetBlockNum(urecptr);
		if (!UndoRecPtrIsValid(prev_urec_ptr) ||
			UndoRecPtrGetLogNo(prev_urec_ptr) != logno ||
			UndoRecPtrGetBlockNum(prev_urec_ptr) != blkno)
		{
			/* Release the previous buffer */
			if (BufferIsValid(buffer))
			{
				UnlockReleaseBuffer(buffer);
				buffer = InvalidBuffer;
			}

			if (prefetch_pages > 0)
				prefetch_pages--;
		}
		else
			uur->uur_buffer = buffer;

		/*
		 * If prefetch_pages are half of the prefetch_target then it's time to
		 * prefetch again.
		 */
		if (prefetch_pages < prefetch_target / 2)
			PrefetchUndoPages(rnode, prefetch_target, &prefetch_pages, to_blkno,
							  from_blkno, persistence);

		/*
		 * In one_page mode it's possible that the undo of the transaction
		 * might have been applied by worker and undo got discarded. Prevent
		 * discard worker from discarding undo data while we are reading it.
		 * See detail comment in UndoFetchRecord.  In normal mode we are
		 * holding transaction undo action lock so it can not be discarded.
		 */
		if (one_page)
		{
			LWLockAcquire(&log->discard_lock, LW_SHARED);

			/* uur holds no data yet; the buffer is released below. */
			if (!UndoRecordIsValid(urecptr))
			{
				pfree(uur);
				break;
			}

			/* Read the undo record. */
			UndoGetOneRecord(uur, urecptr, rnode, persistence, true);
			LWLockRelease(&log->discard_lock);
		}
		else
			UndoGetOneRecord(uur, urecptr, rnode, persistence, true);

		/*
		 * Remember the buffer, so that next time we can call UndoGetOneRecord
		 * with the same buffer if we are reading the undo from the same
		 * buffer.
		 */
		buffer = uur->uur_buffer;
		uur->uur_buffer = InvalidBuffer;

		/*
		 * As soon as the transaction id is changed we can stop fetching the
		 * undo record.  Ideally, to_urecptr should control this but while
		 * reading undo only for a page we don't know what is the end undo
		 * record pointer for the transaction.
		 */
		if (one_page)
		{
			if (!TransactionIdIsValid(xid))
				xid = uur->uur_xid;
			else if (xid != uur->uur_xid)
			{
				/* Not returned, so free it; the buffer is released below. */
				UndoRecordRelease(uur);
				break;
			}
		}

		/* Remember the previous undo record pointer. */
		prev_urec_ptr = urecptr;

		/*
		 * Calculate the previous undo record pointer of the transaction.  If
		 * we are reading undo only for a page then follow the blkprev chain
		 * of the page.  Otherwise, calculate the previous undo record pointer
		 * using transaction's current undo record pointer and the prevlen.
		 */
		if (one_page)
			urecptr = uur->uur_blkprev;
		else if (prev_urec_ptr == to_urecptr || uur->uur_info & UREC_INFO_TRANSACTION)
			urecptr = InvalidUndoRecPtr;
		else
			urecptr = UndoGetPrevUndoRecptr(prev_urec_ptr, uur->uur_prevurp,
											&buffer);

		/* We have consumed all elements of the urp_array so expand its size. */
		if (urp_index >= urp_array_size)
		{
			urp_array_size *= 2;
			urp_array =
				repalloc(urp_array, sizeof(UndoRecInfo) * urp_array_size);
		}

		/* Add entry in the urp_array */
		urp_array[urp_index].index = urp_index;
		urp_array[urp_index].urp = prev_urec_ptr;
		urp_array[urp_index].uur = uur;
		urp_index++;

		/* We have fetched all the undo records for the transaction. */
		if (!UndoRecPtrIsValid(urecptr) || (prev_urec_ptr == to_urecptr))
			break;

		/*
		 * Including current record, if we have crossed the memory limit then
		 * stop processing more records.  Remember to set the from_urecptr so
		 * that on next call we can resume fetching undo records where we left
		 * it.
		 */
		size = UnpackedUndoRecordSize(uur);
		total_size += size;

		if (total_size >= undo_apply_size)
		{
			*from_urecptr = urecptr;
			break;
		}
	} while (true);

	/* Release the last buffer. */
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	*nrecords = urp_index;

	return urp_array;
}

/*
 * undo_record_comparator
 *
 * qsort comparator to handle undo record for applying undo actions of the
 * transaction.
 */
static int
undo_record_comparator(const void *left, const void *right)
{
	UnpackedUndoRecord *luur = ((UndoRecInfo *) left)->uur;
	UnpackedUndoRecord *ruur = ((UndoRecInfo *) right)->uur;

	if (luur->uur_rmid < ruur->uur_rmid)
		return -1;
	else if (luur->uur_rmid > ruur->uur_rmid)
		return 1;
	else if (luur->uur_reloid < ruur->uur_reloid)
		return -1;
	else if (luur->uur_reloid > ruur->uur_reloid)
		return 1;
	else if (luur->uur_block == ruur->uur_block)
	{
		/*
		 * If records are for the same block then maintain their existing
		 * order by comparing their index in the array.  Because for single
		 * block we need to maintain the order for applying undo action.
		 */
		if (((UndoRecInfo *) left)->index < ((UndoRecInfo *) right)->index)
			return -1;
		else
			return 1;
	}
	else if (luur->uur_block < ruur->uur_block)
		return -1;
	else
		return 1;
}

/*
 * PbuMarkRollbackDone
 *		Record that the rollback of the transaction whose undo chain starts at
 *		start_urec_ptr is complete, and forget its rollback request.
 *
 * The transaction header's progress marker makes a later retry a no-op and
 * lets the discard worker discard the transaction's undo.
 *
 * If nothing was prepared there is no transaction header to mark: the undo
 * log is temporary (discarded at transaction end) or already gone.  In that
 * case skip the update and its CLR entirely -- temp undo is not WAL-logged,
 * and UndoRecordUpdateTransInfo() would be reading an xact_urec_info[] slot
 * that was never filled in.
 *
 * Only permanent undo logs are WAL-logged.  The contents of an unlogged log
 * never reach the WAL, so redo would find no record to patch; its header is
 * updated in place, like any unlogged page.
 */
void
PbuMarkRollbackDone(FullTransactionId full_xid, UndoRecPtr start_urec_ptr)
{
	if (PrepareUpdateUndoActionProgress(NULL, start_urec_ptr, 1))
	{
		UndoLogControl *log = UndoLogGet(UndoRecPtrGetLogNo(start_urec_ptr));

		START_CRIT_SECTION();

		/* Update the progress in the transaction header. */
		UndoRecordUpdateTransInfo(0);

		/* WAL log the undo apply progress. */
		if (log->meta.persistence == UNDO_PERMANENT)
		{
			XLogRecPtr	lsn;
			xl_undoapply_progress xlrec;

			xlrec.urec_ptr = start_urec_ptr;
			xlrec.progress = 1;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xlrec));

			RegisterUndoLogBuffers(2);
			lsn = XLogInsert(RM_UNDOACTION_ID, XLOG_UNDO_APPLY_PROGRESS);
			UndoLogBuffersSetLSN(lsn);
		}

		END_CRIT_SECTION();
	}
	UnlockReleaseUndoBuffers();

	RollbackHTRemoveEntry(full_xid, start_urec_ptr);
}

/*
 * execute_undo_actions - Execute the undo actions
 *
 * xid - Transaction id that is getting rolled back.
 * from_urecptr - undo record pointer from where to start applying undo action.
 * to_urecptr	- undo record pointer upto which point apply undo action.
 * nopartial	- true if rollback is for complete transaction.
 *
 * Returns true if every record in the requested range was applied, and false
 * if at least one record was skipped (UNDO_APPLY_SKIPPED) or failed
 * (UNDO_APPLY_ERROR), so the transaction's rollback is NOT complete.  A false
 * return obliges the caller to arrange for the remaining work to be done
 * later: the AM apply callbacks skip unconditionally while InRecovery is set
 * (they need syscache/relcache and the index AMs, which the startup process
 * does not have), so the crash-recovery driver must re-queue the transaction
 * for a post-recovery worker rather than treat the chain as reverted.  See
 * PbuPerformUndoRecovery().
 *
 * A return of true after an already-applied transaction header short-circuit
 * (uur_progress != 0) is correct: the rollback is complete, just not by us.
 *
 * Interrupts are checked between batches and between pages.  Each record is
 * applied, and WAL-logged, on its own, and the transaction header is marked
 * only once all of them are, so an interrupted rollback can be started again
 * from the beginning: the records already applied are found to be no longer
 * current and skipped.  Where the caller holds interrupts (AbortTransaction,
 * ROLLBACK PREPARED) the checks do nothing; the startup process has no
 * interrupt that ends it here.  The other callers keep the rollback request
 * for another attempt: the undo apply worker, PbuApplyPendingUndo().
 */
bool
execute_undo_actions(FullTransactionId full_xid, UndoRecPtr from_urecptr,
					 UndoRecPtr to_urecptr, bool nopartial)
{
	UnpackedUndoRecord *uur = NULL;
	UndoRecInfo *urp_array;
	UndoRecPtr	urec_ptr;
	ForkNumber	prev_fork = InvalidForkNumber;
	BlockNumber prev_block = InvalidBlockNumber;
	Size		undo_apply_size = (Size) maintenance_work_mem * 1024;
	bool		all_applied = true;
	TransactionId xid PG_USED_FOR_ASSERTS_ONLY = XidFromFullTransactionId(full_xid);

	/* 'from' and 'to' pointers must be valid. */
	Assert(from_urecptr != InvalidUndoRecPtr);
	Assert(to_urecptr != InvalidUndoRecPtr);

	urec_ptr = from_urecptr;

	if (nopartial)
	{
		/*
		 * It is important here to fetch the latest undo record and validate
		 * if the actions are already executed.  The reason is that it is
		 * possible that discard worker or backend might try to execute the
		 * rollback request which is already executed.  For ex., after discard
		 * worker fetches the record and found that this transaction need to
		 * be rolledback, backend might concurrently execute the actions and
		 * remove the request from rollback hash table. The similar problem
		 * can happen if the discard worker first pushes the request, the undo
		 * worker processed it and backend tries to process it some later
		 * point.
		 */
		uur = UndoFetchRecord(to_urecptr, InvalidBlockNumber, InvalidOffsetNumber,
							  InvalidTransactionId, NULL, NULL);

		/* already processed. */
		if (uur == NULL)
			return true;

		/*
		 * We don't need to execute the undo actions if they are already
		 * executed.
		 */
		if (uur->uur_progress != 0)
		{
			UndoRecordRelease(uur);
			return true;
		}

		Assert(xid == uur->uur_xid);

		UndoRecordRelease(uur);
		uur = NULL;
	}

	/*
	 * Fetch the multiple undo records which can fit into uur_segment; sort
	 * them in order of reloid and block number then apply them together
	 * page-wise. Repeat this until we get invalid undo record pointer.
	 */
	do
	{
		int			prev_rmid = -1;
		Oid			prev_reloid = InvalidOid;
		bool		blk_chain_complete;
		int			i;
		int			nrecords;
		int			last_index = 0;
		int			prefetch_pages = 0;

		/*
		 * If urec_ptr is not valid means we have complete all undo actions
		 * for this transaction, otherwise we need to fetch the next batch of
		 * the undo records.
		 */
		if (!UndoRecPtrIsValid(urec_ptr))
			break;

		/*
		 * A rollback can take a long time.  Let a shutdown, terminate or
		 * cancel request interrupt it between batches and between pages,
		 * where this holds no buffer or lock of its own.
		 */
		CHECK_FOR_INTERRUPTS();

		/* Lets tests slow down or hold a rollback between two batches. */
		INJECTION_POINT("pbu-undo-apply-batch", NULL);

		/*
		 * Fetch multiple undo record in bulk.  This will return the array of
		 * undo record which will holds undo record pointers and the pointers
		 * to the actual unpacked undo record.   This will also update the
		 * number of undo records it has copied in the urp_array.  Also, for
		 * prefetching the target block ahead of applying undo actions it will
		 * update undo_blkinfo which will contains the information of the data
		 * blocks for which undo actions are going to applied for this undo
		 * record batch.
		 */
		urp_array = UndoRecordBulkFetch(&urec_ptr, to_urecptr, undo_apply_size,
										&nrecords, false);
		if (nrecords == 0)
		{
			pfree(urp_array);
			break;
		}

		Assert(TransactionIdEquals(xid, urp_array[0].uur->uur_xid));

		/* Sort the undo record array in order of target blocks. */
		qsort((void *) urp_array, nrecords, sizeof(UndoRecInfo),
			  undo_record_comparator);

		if (nopartial && !UndoRecPtrIsValid(urec_ptr))
			blk_chain_complete = true;
		else
			blk_chain_complete = false;

		/*
		 * Now we have urp_array which is sorted in the block order so
		 * traverse this array and apply the undo action block by block.
		 */
		for (i = last_index; i < nrecords; i++)
		{
			UnpackedUndoRecord *cur_uur = urp_array[i].uur;

			/*
			 * If this undo is not for the same block then apply all undo
			 * actions for the previous block.
			 */
			if (prev_rmid >= 0 &&
				(prev_rmid != cur_uur->uur_rmid ||
				 prev_reloid != cur_uur->uur_reloid ||
				 prev_fork != cur_uur->uur_fork ||
				 prev_block != cur_uur->uur_block))
			{
				CHECK_FOR_INTERRUPTS();
				if (!execute_undo_actions_page(urp_array, last_index, i - 1,
											   prev_reloid, full_xid, prev_block,
											   blk_chain_complete))
					all_applied = false;
				last_index = i;

				/* We have consumed one prefetched page. */
				if (prefetch_pages > 0)
					prefetch_pages--;
			}

			prev_rmid = cur_uur->uur_rmid;
			prev_reloid = cur_uur->uur_reloid;
			prev_fork = cur_uur->uur_fork;
			prev_block = cur_uur->uur_block;
		}

		/* Apply the last set of the actions. */
		CHECK_FOR_INTERRUPTS();
		if (!execute_undo_actions_page(urp_array, last_index, i - 1,
									   prev_reloid, full_xid, prev_block,
									   blk_chain_complete))
			all_applied = false;

		/* Free all undo records. */
		for (i = 0; i < nrecords; i++)
			UndoRecordRelease(urp_array[i].uur);

		/*
		 * Free urp array and undo_blkinfo array for the current batch of undo
		 * records.
		 */
		pfree(urp_array);
	} while (true);

	/*
	 * Set undo action apply progress as completed in the transaction header
	 * if this is a main transaction.
	 */
	if (nopartial)
	{
		/*
		 * During crash recovery (the ARIES undo phase), the whole transaction
		 * chain is applied once, before WAL insertion is re-enabled and
		 * before the shared rollback hash table is populated for this xid.
		 * The transaction-header progress marker and its CLR
		 * (XLOG_UNDO_APPLY_PROGRESS) exist to make an interrupted LIVE abort
		 * idempotent and to coordinate with the discard worker; neither
		 * applies here.  Attempting the progress update in recovery is also
		 * unsafe: it would take the InRecovery undo-buffer read path (no
		 * exclusive buffer lock) and then MarkBufferDirty, tripping the
		 * buffer-lock assert, and it would emit WAL and touch the RollbackHT
		 * that has no entry for this xid.  So in recovery we skip the
		 * progress/CLR bookkeeping entirely; the per-record apply above
		 * already reverted the change, and the end-of-recovery checkpoint
		 * provides durability.
		 *
		 * Note that in recovery the per-record apply above has typically NOT
		 * reverted anything: the AM callbacks return UNDO_APPLY_SKIPPED while
		 * InRecovery is set, which all_applied has recorded.  Marking
		 * progress here would therefore be a lie as well as unsafe; the
		 * caller uses our false return to re-queue the transaction for
		 * post-recovery apply, which then does reach the progress/CLR
		 * bookkeeping below.
		 */
		if (InRecovery)
			return all_applied;

		/*
		 * Do not mark the transaction header as fully applied when some
		 * record was skipped or failed: the marker is what makes a later
		 * retry a no-op, so setting it now would strand the un-reverted
		 * remainder. The caller decides whether to re-queue.
		 */
		if (!all_applied)
			return false;

		Assert(TransactionIdIsValid(xid));
		PbuMarkRollbackDone(full_xid, to_urecptr);
	}

	return all_applied;
}

/*
 * PbuAtAbort_ApplyUndo
 *		Roll back the current transaction's per-backend UNDO at abort.
 *
 * Called from AbortTransaction() (xact.c) in the inline-undo-apply window,
 * alongside (but independent of) the cluster-wide engine's AtAbort_XactUndo().
 * The two engines coexist: this path only fires when the per-backend engine
 * produced UNDO for this transaction, which is recorded separately on the
 * TransactionState (pbuUndoStartPtr) via SetCurrentUndoLocation().  FLUX
 * writes per-backend UNDO; a transaction that touched no FLUX table has an
 * invalid pbuUndoStartPtr and returns immediately.
 *
 * The rollback is registered in the shared rollback hash table so the discard
 * worker will not race us.  If the request cannot be pushed to a background
 * undo worker (e.g. single-user mode, or workers disabled), the backend
 * applies its own UNDO synchronously via execute_undo_actions().
 */
static void PbuRememberPendingUndo(FullTransactionId full_xid,
								   UndoRecPtr from_urec_ptr,
								   UndoRecPtr start_urec_ptr, bool temp);

void
PbuAtAbort_ApplyUndo(void)
{
	FullTransactionId full_xid;
	bool		any = false;

	for (int level = 0; level < UndoPersistenceLevels; level++)
	{
		if (GetCurrentTransactionPbuUndoStartAt(level) != 0)
			any = true;
	}

	/* Nothing to do if this transaction produced no per-backend UNDO. */
	if (!any)
		return;

	full_xid = GetTopFullTransactionIdIfAny();
	if (!FullTransactionIdIsValid(full_xid))
		return;

	/*
	 * Release any buffer content locks this transaction still holds before
	 * applying UNDO.
	 *
	 * A statement that errors out mid-update can unwind while still holding a
	 * buffer content lock -- e.g. flux_tuple_update_inplace() calls
	 * CheckForSerializableConflictIn() under the tuple's exclusive buffer
	 * lock, and that ereports on an SSI failure.  Resource-owner cleanup only
	 * releases those locks later, in ResourceOwnerRelease(RESOURCE_RELEASE_
	 * LOCKS), which runs AFTER this abort-time UNDO application.  The UNDO
	 * appliers re-lock the very same buffer (flux_undo_apply ->
	 * BufferLockAcquire), so without this the backend deadlocks against its
	 * own held lock and the abort never completes (observed as a permanently
	 * hung backend after "could not serialize access ..." in the total-cash
	 * isolation test).
	 *
	 * This mirrors what the cluster-wide engine already does around inline
	 * UNDO application in xactundo.c.  We are in an error-recovery path, so
	 * dropping content locks here is safe -- no live statement can still be
	 * relying on them.
	 */
	BufferLockReleaseAll();

	/*
	 * The transaction has one chain in each persistence level's undo log it
	 * wrote to.  Each is a separate rollback request, keyed by its start.
	 */
	for (int level = 0; level < UndoPersistenceLevels; level++)
	{
		UndoRecPtr	start_urec_ptr;
		UndoRecPtr	from_urec_ptr;
		bool		pushed;

		start_urec_ptr = (UndoRecPtr) GetCurrentTransactionPbuUndoStartAt(level);
		if (!UndoRecPtrIsValid(start_urec_ptr))
			continue;
		from_urec_ptr = (UndoRecPtr) GetCurrentTransactionPbuUndoLatestAt(level);
		if (!UndoRecPtrIsValid(from_urec_ptr))
			from_urec_ptr = start_urec_ptr;

		/*
		 * Register the rollback request.  RegisterRollbackReq computes the
		 * chain's end pointer from start_urec_ptr, records the request so the
		 * discard worker skips it, and returns true if it was pushed to a
		 * background undo worker.  It returns false in single-user mode and
		 * for temporary undo.
		 */
		pushed = RegisterRollbackReq(InvalidUndoRecPtr, start_urec_ptr,
									 MyDatabaseId, full_xid);

		/*
		 * If the request was not handed to a background worker, this backend
		 * must apply the UNDO itself, synchronously, before the transaction
		 * finishes aborting.  from = latest record, to = chain start,
		 * nopartial = true (whole transaction).
		 */
		if (!pushed &&
			!execute_undo_actions(full_xid, from_urec_ptr, start_urec_ptr,
								  true))
			PbuRememberPendingUndo(full_xid, from_urec_ptr, start_urec_ptr,
								   level == UNDO_TEMP);
	}
}

/*
 * Rollbacks that PbuAtAbort_ApplyUndo() could not complete, for
 * PbuApplyPendingUndo(); allocated in TopMemoryContext.
 */
typedef struct PbuPendingUndo
{
	FullTransactionId full_xid;
	UndoRecPtr	from_urec_ptr;
	UndoRecPtr	start_urec_ptr;
	bool		temp;
	int			nfailures;
} PbuPendingUndo;

static List *pbu_pending_undo = NIL;
static bool pbu_applying_pending_undo = false;

/* The rollbacks PbuApplyPendingUndo() is working on; see PbuPendingUndoAtExit. */
static List *pbu_applying_undo = NIL;

/* Attempts PbuApplyPendingUndo() makes at a rollback before giving up. */
#define PBU_PENDING_UNDO_MAX_ATTEMPTS	3

/*
 * A backend that exits with a rollback still pending -- terminated while it
 * was applying it, or disconnected after an attempt failed -- hands it to the
 * undo apply workers.  Its request is in the rollback hash table, but only
 * this backend was ever going to apply it: drop the entry, and the discard
 * worker registers the transaction again and queues it for a worker, as for
 * any aborted transaction whose rollback is not done.  Temporary undo has no
 * entry, and goes with the backend's temporary tables.
 */
static void
PbuPendingUndoAtExit(int code, Datum arg)
{
	List	   *lists[] = {pbu_applying_undo, pbu_pending_undo};

	/* In single-user mode no request was registered (RegisterRollbackReq). */
	if (!IsUnderPostmaster)
		return;

	for (int i = 0; i < lengthof(lists); i++)
	{
		ListCell   *lc;

		foreach(lc, lists[i])
		{
			PbuPendingUndo *p = (PbuPendingUndo *) lfirst(lc);

			if (p != NULL && !p->temp)
				RollbackHTRemoveEntry(p->full_xid, p->start_urec_ptr);
		}
	}
}

static void
PbuRememberPendingUndo(FullTransactionId full_xid, UndoRecPtr from_urec_ptr,
					   UndoRecPtr start_urec_ptr, bool temp)
{
	static bool exit_registered = false;
	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	PbuPendingUndo *p = palloc0_object(PbuPendingUndo);

	if (!exit_registered)
	{
		before_shmem_exit(PbuPendingUndoAtExit, (Datum) 0);
		exit_registered = true;
	}

	p->full_xid = full_xid;
	p->from_urec_ptr = from_urec_ptr;
	p->start_urec_ptr = start_urec_ptr;
	p->temp = temp;
	pbu_pending_undo = lappend(pbu_pending_undo, p);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * PbuApplyPendingUndo
 *		Finish the rollbacks that had to wait for the end of the transaction.
 *
 * The inline rollback in AbortTransaction() runs before the transaction's
 * catalog changes are rolled back.  If the transaction itself truncated,
 * rewrote or dropped a relation its undo records name, or dropped or rebuilt
 * an index they change, the relcache does not show that relation or file yet,
 * and the access method leaves the record unapplied (UNDO_APPLY_SKIPPED).
 * Once the transaction is over the catalog is as it was before it, so apply
 * the chain again, in a transaction of its own, as an undo apply worker
 * would.  Records already applied are found no longer current and skipped.
 *
 * No worker can do this for temporary undo, which only this backend can
 * read, nor in single-user mode.  Until it is done, readers walk past the
 * aborted versions and writers revert them first, as for any rollback left
 * to a worker; PbuTempUndoPending() keeps the temporary undo they need.
 *
 * Called with no transaction open: before the next one starts and before
 * the backend reports ready for query.  An error is reported as a WARNING
 * and the rollback is tried again next time, a few times at most.  Then it
 * is given up: what is left stays in place, for writers to revert while the
 * undo lasts, as an undo worker leaves a request it gives up on.  A query
 * cancel interrupts the rollback too, but is not counted as a failed
 * attempt; if the backend is terminated, the undo apply workers finish the
 * rollback (PbuPendingUndoAtExit).
 */
void
PbuApplyPendingUndo(void)
{
	List	   *pending;
	ListCell   *lc;

	if (pbu_pending_undo == NIL || pbu_applying_pending_undo ||
		IsTransactionOrTransactionBlock())
		return;

	pbu_applying_pending_undo = true;
	pending = pbu_applying_undo = pbu_pending_undo;
	pbu_pending_undo = NIL;

	foreach(lc, pending)
	{
		PbuPendingUndo *p = (PbuPendingUndo *) lfirst(lc);
		MemoryContext oldcxt = CurrentMemoryContext;
		bool		done = false;
		bool		canceled = false;

		PG_TRY();
		{
			StartTransactionCommand();
			/* Lets tests hold the backend here, where it can be interrupted. */
			INJECTION_POINT("pbu-apply-pending-undo", NULL);
			done = execute_undo_actions(p->full_xid, p->from_urec_ptr,
										p->start_urec_ptr, true);
			CommitTransactionCommand();
		}
		PG_CATCH();
		{
			ErrorData  *edata;

			MemoryContextSwitchTo(oldcxt);
			edata = CopyErrorData();
			FlushErrorState();
			AbortOutOfAnyTransaction();
			canceled = edata->sqlerrcode == ERRCODE_QUERY_CANCELED;
			ereport(WARNING,
					(errmsg("could not finish rolling back transaction %llu: %s",
							(unsigned long long) U64FromFullTransactionId(p->full_xid),
							edata->message)));
			FreeErrorData(edata);
		}
		PG_END_TRY();
		MemoryContextSwitchTo(oldcxt);

		/* It is on one list or the other, never both. */
		lfirst(lc) = NULL;
		if (done)
			pfree(p);
		else if (canceled || ++p->nfailures < PBU_PENDING_UNDO_MAX_ATTEMPTS)
		{
			MemoryContextSwitchTo(TopMemoryContext);
			pbu_pending_undo = lappend(pbu_pending_undo, p);
			MemoryContextSwitchTo(oldcxt);
		}
		else
		{
			ereport(WARNING,
					(errmsg("giving up on rolling back transaction %llu",
							(unsigned long long) U64FromFullTransactionId(p->full_xid)),
					 errdetail("Some of its per-backend undo could not be applied; those changes stay in place.")));
			pfree(p);
		}
	}
	pbu_applying_undo = NIL;
	list_free(pending);
	pbu_applying_pending_undo = false;
}

/*
 * Must this backend keep its temporary undo?  See PbuApplyPendingUndo().
 */
bool
PbuTempUndoPending(void)
{
	ListCell   *lc;

	if (pbu_applying_pending_undo)
		return true;
	foreach(lc, pbu_pending_undo)
	{
		if (((PbuPendingUndo *) lfirst(lc))->temp)
			return true;
	}
	return false;
}

/*
 * MarkUndoRecordApplied
 *		Mark one undo record as already applied.
 *
 * The record's rmid is replaced by UNDO_RMID_INVALID.  The rmid is the first
 * byte of the record, which always lies on the page that urp addresses.  The
 * record's other contents, including the trailing length that the backward
 * chain walk uses, are left alone, so the chain stays walkable and a reader
 * following a version pointer into it still finds the same bytes.
 */
static void
MarkUndoRecordApplied(UndoRecPtr urp)
{
	UndoLogControl *log = UndoLogGet(UndoRecPtrGetLogNo(urp));
	RelFileNode rnode;
	Buffer		buffer;
	Page		page;

	if (log == NULL)
		return;					/* discarded: nothing left to apply */

	UndoRecPtrAssignRelFileNode(rnode, urp);
	buffer = ReadBufferWithoutRelcache(rnode, UndoLogForkNum,
									   UndoRecPtrGetBlockNum(urp),
									   RBM_NORMAL, NULL,
									   UndoPersistenceIsPermanent(log->meta.persistence));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);

	START_CRIT_SECTION();

	((char *) page)[UndoRecPtrGetPageOffset(urp) +
					offsetof(UndoRecordHeader, urec_rmid)] = UNDO_RMID_INVALID;
	MarkBufferDirty(buffer);

	/* Only permanent undo logs are WAL-logged; see execute_undo_actions(). */
	if (log->meta.persistence == UNDO_PERMANENT)
	{
		xl_undoapply_subxact xlrec;

		xlrec.urec_ptr = urp;
		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));
		XLogRegisterBuffer(0, buffer, 0);
		PageSetLSN(page, XLogInsert(RM_UNDOACTION_ID, XLOG_UNDO_APPLY_SUBXACT));
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

/*
 * Patch a reserved part of the current writer's payload without changing the
 * record size.  The caller must own the transaction, so discard and rollback
 * cannot race this change.  All affected pages are locked before changing any
 * bytes, and a single WAL record makes a cross-page patch atomic at replay.
 */
void
PbuAmendPayload(UndoRecPtr urp, Size offset, const char *data, Size len)
{
	UndoLogControl *log = UndoLogGet(UndoRecPtrGetLogNo(urp));
	UnpackedUndoRecord *urec;
	RelFileNode rnode;
	UndoLogOffset pos;
	Buffer		buffers[UINT16_MAX / UndoLogUsableBytesPerPage + 2];
	uint16		offsets[lengthof(buffers)];
	uint16		lengths[lengthof(buffers)];
	Size		done = 0;
	int			nbufs = 0;
	int			i;

	if (log == NULL || len == 0 || len > UINT16_MAX)
		elog(ERROR, "invalid undo payload amendment");
	urec = UndoFetchRecord(urp, InvalidBlockNumber, InvalidOffsetNumber,
						   InvalidTransactionId, NULL, NULL);
	if (urec == NULL)
		elog(ERROR, "undo record missing during payload amendment");
	if (!TransactionIdIsCurrentTransactionId(urec->uur_xid) ||
		offset > urec->uur_payload.len || len > urec->uur_payload.len - offset)
		elog(ERROR, "undo payload amendment outside owned record");

	/* Expected size includes tuple bytes and the trailing record length. */
	offset += UndoRecordExpectedSize(urec) - urec->uur_payload.len -
		urec->uur_tuple.len - sizeof(uint16);
	UndoRecordRelease(urec);
	pos = UndoLogOffsetPlusUsableBytes(UndoRecPtrGetOffset(urp), offset);
	UndoRecPtrAssignRelFileNode(rnode, urp);
	XLogEnsureRecordSpace(lengthof(buffers), 2 * lengthof(buffers));

	while (done < len)
	{
		offsets[nbufs] = pos % BLCKSZ;
		lengths[nbufs] = Min(len - done, BLCKSZ - offsets[nbufs]);
		buffers[nbufs] = ReadBufferWithoutRelcache(rnode, UndoLogForkNum,
												   pos / BLCKSZ, RBM_NORMAL, NULL,
												   UndoPersistenceIsPermanent(log->meta.persistence));
		LockBuffer(buffers[nbufs], BUFFER_LOCK_EXCLUSIVE);
		done += lengths[nbufs];
		pos = UndoLogOffsetPlusUsableBytes(pos, lengths[nbufs]);
		nbufs++;
	}

	START_CRIT_SECTION();
	done = 0;
	if (log->meta.persistence == UNDO_PERMANENT)
		XLogBeginInsert();
	for (i = 0; i < nbufs; i++)
	{
		memcpy((char *) BufferGetPage(buffers[i]) + offsets[i],
			   data + done, lengths[i]);
		MarkBufferDirty(buffers[i]);
		if (log->meta.persistence == UNDO_PERMANENT)
		{
			XLogRegisterBuffer(i, buffers[i], 0);
			XLogRegisterBufData(i, (char *) &offsets[i], sizeof(uint16));
			XLogRegisterBufData(i, (char *) data + done, lengths[i]);
		}
		done += lengths[i];
	}
	if (log->meta.persistence == UNDO_PERMANENT)
	{
		XLogRecPtr	lsn = XLogInsert(RM_UNDOACTION_ID, XLOG_UNDO_AMEND_RECORD);

		for (i = 0; i < nbufs; i++)
			PageSetLSN(BufferGetPage(buffers[i]), lsn);
	}
	END_CRIT_SECTION();
	for (i = 0; i < nbufs; i++)
		UnlockReleaseBuffer(buffers[i]);
}

/* Set while PbuApplySubxactUndo() runs; see PbuInSubxactApply(). */
static bool pbu_in_subxact_apply = false;

/* Set while PbuRetrySubxactUndo() runs; see PbuInSubxactRetry(). */
static bool pbu_in_subxact_retry = false;

/*
 * The top-level transaction in which ROLLBACK TO SAVEPOINT left an undo
 * record unapplied, if any.  See PbuAtPreCommit_Undo().
 */
static FullTransactionId pbu_subxact_undo_incomplete;

/*
 * Records of the aborting subtransaction that PbuApplySubxactUndo() could not
 * apply yet, for PbuRetrySubxactUndo(); allocated in TopTransactionContext of
 * the transaction pbu_retry_fxid.
 */
static FullTransactionId pbu_retry_fxid;
static UndoRecPtr *pbu_retry_urps;
static int	pbu_retry_n;
static int	pbu_retry_size;

static void
PbuRememberSubxactRetry(FullTransactionId full_xid, UndoRecPtr urp)
{
	if (!FullTransactionIdEquals(pbu_retry_fxid, full_xid))
	{
		pbu_retry_fxid = full_xid;
		pbu_retry_size = 16;
		pbu_retry_urps = MemoryContextAlloc(TopTransactionContext,
											pbu_retry_size * sizeof(UndoRecPtr));
		pbu_retry_n = 0;
	}
	else if (pbu_retry_n == pbu_retry_size)
	{
		pbu_retry_size *= 2;
		pbu_retry_urps = repalloc(pbu_retry_urps,
								  pbu_retry_size * sizeof(UndoRecPtr));
	}
	pbu_retry_urps[pbu_retry_n++] = urp;
}

/* Is anything left for PbuRetrySubxactUndo()? */
bool
PbuSubxactUndoRetryPending(void)
{
	FullTransactionId full_xid = GetTopFullTransactionIdIfAny();

	return FullTransactionIdIsValid(full_xid) &&
		FullTransactionIdEquals(full_xid, pbu_retry_fxid) && pbu_retry_n > 0;
}

/*
 * PbuInSubxactApply
 *		Is the undo being applied for ROLLBACK TO SAVEPOINT?
 *
 * The access methods' undo callbacks drop the top transaction's sLog marker
 * of a restored tuple once its rollback is done.  For a subtransaction that
 * would drop markers other (sibling, parent) subtransactions still need; the
 * access methods' SUBXACT_EVENT_ABORT_SUB handlers clean up per
 * subtransaction instead.
 */
bool
PbuInSubxactApply(void)
{
	return pbu_in_subxact_apply;
}

/*
 * PbuInSubxactRetry
 *		Is the undo being applied by PbuRetrySubxactUndo()?
 *
 * Then the subtransaction's catalog changes are rolled back, so the relcache
 * maps every relation to the file the parent sees.  An undo callback that
 * found its relation mapped to another file during the walk can return
 * UNDO_APPLY_SKIPPED there and rely on the retry seeing the right one.
 */
bool
PbuInSubxactRetry(void)
{
	return pbu_in_subxact_retry;
}

/*
 * PbuApplySubxactUndo
 *		ROLLBACK TO SAVEPOINT: reverse-apply the aborting subtransaction's
 *		per-backend undo records of one undo resource manager.
 *
 * Called before the subtransaction abort record, so that access methods
 * can perform their subtransaction-scoped cleanup afterwards.
 * CurrentTransactionState is still the aborting subtransaction,
 * so the ...At() brackets hold exactly its records, one chain per persistence
 * level it wrote to.  Every level is applied: a change left in place is kept
 * by a COMMIT, as an aborted subtransaction's image that readers can walk past
 * only until the transaction's undo is discarded.
 *
 * Each record that was applied is also marked so.  The records stay in the transaction's
 * undo chain, between records of the parent, so a later rollback of the
 * parent or of the whole transaction would walk over them and apply them a
 * second time.  By then the subtransaction's row locks are gone and another
 * transaction may have updated the same row and committed; re-applying the
 * before-image would overwrite that committed value.  Every applier skips
 * marked records: execute_undo_actions_page() here, which serves the aborting
 * backend, the undo apply workers, crash recovery and ROLLBACK PREPARED, and
 * this walk, which dispatches records by resource manager.
 *
 * zheap solved the same problem by rewinding the undo insert pointer to the
 * start of the subtransaction.  That would let a later write overwrite
 * records that a concurrent reader may still be reaching through a tuple's
 * version pointer, so we mark instead of rewinding.
 *
 * A record that could not be applied is left unmarked and remembered for
 * PbuRetrySubxactUndo(): typically the subtransaction itself truncated or
 * dropped the record's relation, which makes it look empty or gone until
 * AbortSubTransaction() has rolled back the subtransaction's catalog changes.
 * The older records of the same relation are remembered with it, unapplied:
 * records of one row must be applied newest first, and an older one applied
 * now would find its row still carrying the newer, unapplied change.
 *
 * This runs inside AbortSubTransaction(), so an ERROR here (for instance an
 * I/O error reading an undo page) is an error during abort.
 */
static void
PbuApplySubxactUndoInternal(int rmid)
{
	FullTransactionId full_xid = GetTopFullTransactionIdIfAny();
	TransactionId xid;
	int			saved_trans_state;
	List	   *deferred = NIL;

	if (!FullTransactionIdIsValid(full_xid))
		return;
	xid = XidFromFullTransactionId(full_xid);

	/*
	 * We are in TRANS_ABORT, but relcache and locks are still live: present
	 * TRANS_INPROGRESS for the relation opens the appliers perform.
	 */
	saved_trans_state = EnterInlineUndoApplyState();
	pbu_in_subxact_apply = true;
	PG_TRY();
	{
		for (int level = 0; level < UndoPersistenceLevels; level++)
		{
			UndoRecPtr	start = (UndoRecPtr) GetCurrentTransactionPbuUndoStartAt(level);
			UndoRecPtr	urp = (UndoRecPtr) GetCurrentTransactionPbuUndoLatestAt(level);

			if (!UndoRecPtrIsValid(start))
				continue;

			while (UndoRecPtrIsValid(urp))
			{
				UnpackedUndoRecord *uur;
				UndoRecPtr	prev;
				Buffer		buffer = InvalidBuffer;
				bool		applied = false;

				uur = UndoFetchRecord(urp, InvalidBlockNumber,
									  InvalidOffsetNumber,
									  InvalidTransactionId, NULL, NULL);
				if (uur == NULL)
					break;		/* discarded: nothing left to apply */

				if (rmid < 0 || uur->uur_rmid == rmid)
				{
					const UndoRmgrData *rmgr = GetUndoRmgr(uur->uur_rmid);

					applied = !list_member_oid(deferred, uur->uur_reloid) &&
						rmgr != NULL && rmgr->rm_undo != NULL &&
						rmgr->rm_undo(uur->uur_rmid, uur->uur_type, xid,
									  uur->uur_reloid,
									  uur->uur_payload.data,
									  uur->uur_payload.len, urp) ==
						UNDO_APPLY_SUCCESS;
					if (!applied)
					{
						PbuRememberSubxactRetry(full_xid, urp);
						deferred = list_append_unique_oid(deferred,
														  uur->uur_reloid);
					}
				}

				if (urp == start ||
					((uur->uur_info & UREC_INFO_TRANSACTION) &&
					 !UndoRecPtrIsValid(uur->uur_prevurp)))
					prev = InvalidUndoRecPtr;
				else
					prev = UndoGetPrevUndoRecptr(urp, uur->uur_prevurp,
												 &buffer);

				UndoRecordRelease(uur);
				if (BufferIsValid(buffer))
					UnlockReleaseBuffer(buffer);

				if (applied)
					MarkUndoRecordApplied(urp);
				urp = prev;
			}
		}
	}
	PG_FINALLY();
	{
		pbu_in_subxact_apply = false;
		LeaveInlineUndoApplyState(saved_trans_state);
	}
	PG_END_TRY();
	list_free(deferred);
}

/* Apply one resource manager's subtransaction records. */
void
PbuApplySubxactUndo(uint8 rmid)
{
	PbuApplySubxactUndoInternal(rmid);
}

/*
 * Apply all subtransaction records in chain order, before its abort record
 * can drop their target files.  Dispatch each record to its resource manager.
 */
void
PbuApplySubxactUndoAll(void)
{
	PbuApplySubxactUndoInternal(-1);
}

/*
 * PbuRetrySubxactUndo
 *		Apply the records PbuApplySubxactUndo() could not.
 *
 * Called by AbortSubTransaction() once the subtransaction's relcache and
 * catalog changes are rolled back, with the parent made the current
 * transaction, so relations look as the parent sees them.  If a record's
 * relation or block is still gone, nothing is left to undo: the relation was
 * created in the subtransaction, or is gone for good.  A record that fails
 * here stays unmarked, in the chain the parent takes over, and the
 * transaction can no longer commit; see PbuAtPreCommit_Undo().
 */
void
PbuRetrySubxactUndo(void)
{
	FullTransactionId full_xid = GetTopFullTransactionIdIfAny();
	TransactionId xid = XidFromFullTransactionId(full_xid);
	int			n;

	if (!PbuSubxactUndoRetryPending())
		return;
	n = pbu_retry_n;
	pbu_retry_n = 0;

	pbu_in_subxact_apply = true;
	pbu_in_subxact_retry = true;
	PG_TRY();
	{
		/* Same order as the walk: newest first. */
		for (int i = 0; i < n; i++)
		{
			UndoRecPtr	urp = pbu_retry_urps[i];
			UnpackedUndoRecord *uur;
			const UndoRmgrData *rmgr;
			UndoApplyResult result = UNDO_APPLY_ERROR;

			uur = UndoFetchRecord(urp, InvalidBlockNumber, InvalidOffsetNumber,
								  InvalidTransactionId, NULL, NULL);
			if (uur == NULL)
				continue;
			/* An outer subtransaction's walk may already have applied it. */
			if (uur->uur_rmid == UNDO_RMID_INVALID)
			{
				UndoRecordRelease(uur);
				continue;
			}
			rmgr = GetUndoRmgr(uur->uur_rmid);
			if (rmgr != NULL && rmgr->rm_undo != NULL)
				result = rmgr->rm_undo(uur->uur_rmid, uur->uur_type, xid,
									   uur->uur_reloid,
									   uur->uur_payload.data,
									   uur->uur_payload.len, urp);
			UndoRecordRelease(uur);

			if (result == UNDO_APPLY_ERROR)
				pbu_subxact_undo_incomplete = full_xid;
			else
				MarkUndoRecordApplied(urp);
		}
	}
	PG_FINALLY();
	{
		pbu_in_subxact_apply = false;
		pbu_in_subxact_retry = false;
	}
	PG_END_TRY();
}

/*
 * PbuAtPreCommit_Undo
 *		Refuse to commit or prepare a transaction in which ROLLBACK TO
 *		SAVEPOINT left per-backend undo unapplied.
 *
 * Committing would keep the rolled-back change on the page as the image of an
 * aborted subtransaction.  Readers walk past it to the committed version only
 * while the undo exists; once it is discarded, that version is lost.  Rolling
 * back instead applies the record with the rest of the transaction.
 */
void
PbuAtPreCommit_Undo(void)
{
	FullTransactionId full_xid = GetTopFullTransactionIdIfAny();

	if (PbuSubxactUndoRetryPending() ||
		(FullTransactionIdIsValid(full_xid) &&
		 FullTransactionIdEquals(full_xid, pbu_subxact_undo_incomplete)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("cannot commit a transaction whose rollback to a savepoint was incomplete"),
				 errdetail("An undo record of a rolled-back subtransaction could not be applied.")));
}

/*
 * execute_undo_actions_page - Execute the undo actions for a page
 *
 *	urp_array - array of undo records (along with their location) for which undo
 *				action needs to be applied.
 *	first_idx - index in the urp_array of the first undo action to be applied
 *	last_idx  - index in the urp_array of the first undo action to be applied
 *	reloid	- OID of relation on which undo actions needs to be applied.
 *	blkno	- block number on which undo actions needs to be applied.
 *						 blk_chain_complete - indicates whether the undo chain for block is
 *						 complete.
 *
 *	returns true if every record in [first_idx, last_idx] was applied, false if
 *	any record was skipped (UNDO_APPLY_SKIPPED) or errored.  A false return means
 *	the rollback of those records is still outstanding; the caller must not treat
 *	the transaction as reverted.  Unlike the previous behaviour, an
 *	UNDO_APPLY_ERROR does not stop the loop: the remaining records of the batch
 *	are still attempted (they may target unrelated pages), and the false return
 *	carries the incompleteness up.
 */
bool
execute_undo_actions_page(UndoRecInfo *urp_array, int first_idx, int last_idx,
						  Oid reloid, FullTransactionId full_xid, BlockNumber blkno,
						  bool blk_chain_complete)
{
	int			i;
	bool		all_applied = true;
	TransactionId xid = XidFromFullTransactionId(full_xid);

	Assert(urp_array != NULL);

	(void) reloid;				/* per-record uur_reloid is used instead */
	(void) blkno;
	(void) blk_chain_complete;

	/*
	 * Dispatch each record to the owning AM's undo-apply callback via the
	 * pluggable UNDO resource-manager API (access/undormgr.h).  Every AM or
	 * subsystem that writes per-backend UNDO registers an UndoRmgrData with
	 * an rm_undo callback keyed by the rmid stored in each record's header;
	 * the callback opens the target relation, reapplies the compensating
	 * change to the page and emits its own CLR WAL.
	 *
	 * The record subtype the callback switches on lives in uur_type, NOT
	 * uur_info (the latter is the UREC_INFO_* structural flag bits); we
	 * therefore pass rec->uur_type as the callback's "info" argument. Passing
	 * uur_info here would be a dispatch bug.
	 */
	for (i = first_idx; i <= last_idx; i++)
	{
		UnpackedUndoRecord *rec = urp_array[i].uur;
		const UndoRmgrData *undormgr;
		const char *payload;
		Size		payload_len;
		UndoApplyResult result;

		/* Already applied by ROLLBACK TO SAVEPOINT. */
		if (rec->uur_rmid == UNDO_RMID_INVALID)
			continue;

		undormgr = GetUndoRmgr(rec->uur_rmid);
		if (undormgr == NULL || undormgr->rm_undo == NULL)
		{
			/*
			 * No registered handler for this rmid.  Every producer registers
			 * its rmgr at startup, so this should not happen.  The record's
			 * change is still in place, so the transaction is not reverted:
			 * report that rather than counting the record as applied.
			 */
			ereport(WARNING,
					(errmsg("no per-backend UNDO handler for resource manager %u; "
							"undo record at " UndoRecPtrFormat " not applied",
							rec->uur_rmid,
							(unsigned long long) urp_array[i].urp)));
			all_applied = false;
			continue;
		}

		payload = rec->uur_payload.data;
		payload_len = rec->uur_payload.len;

		/* Lets tests make a record fail to apply without an error. */
		if (IS_INJECTION_POINT_ATTACHED("pbu-undo-apply-skip"))
			result = UNDO_APPLY_SKIPPED;
		else
			result = undormgr->rm_undo(rec->uur_rmid,
									   rec->uur_type,
									   xid,
									   rec->uur_reloid,
									   payload,
									   payload_len,
									   urp_array[i].urp);

		/*
		 * Both SKIPPED and ERROR leave this record's change in place, so the
		 * transaction is not fully reverted either way.  Report that to the
		 * caller instead of silently returning success.
		 *
		 * SKIPPED is the interesting case for crash recovery: every AM undo
		 * callback returns it unconditionally while InRecovery is set, so a
		 * whole loser transaction comes back SKIPPED from the ARIES undo
		 * phase and must be re-queued (PbuPerformUndoRecovery).  Outside
		 * recovery, a record whose relation or block is gone for good counts
		 * as applied (the AM callback returns SUCCESS); SKIPPED then means
		 * the record must be tried again later, e.g. because the aborting
		 * transaction's own DROP or TRUNCATE has not been rolled back yet
		 * (PbuApplyPendingUndo).  An undo apply worker retries an incomplete
		 * request a bounded number of times (UNDO_WORKER_MAX_ATTEMPTS).
		 */
		if (result != UNDO_APPLY_SUCCESS)
			all_applied = false;
	}

	return all_applied;
}
