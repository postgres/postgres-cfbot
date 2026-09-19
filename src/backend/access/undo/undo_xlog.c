/*-------------------------------------------------------------------------
 *
 * undo_xlog.c
 *	  UNDO resource manager WAL redo routines
 *
 * This module implements the WAL redo callback for the RM_UNDO_ID resource
 * manager, the recovery-time tracking of transactions that wrote UNDO, the
 * crash-recovery UNDO phase, and the reader that fetches UNDO batches back
 * out of WAL.  The records it replays:
 *
 *   XLOG_UNDO_INFLIGHT       - The UNDO chains in flight at a checkpoint.
 *   XLOG_UNDO_BATCH          - A batch of UNDO records.  Redo changes no page;
 *                              it only tracks the batch as the chain head of
 *                              its transaction until the commit or abort.
 *   XLOG_UNDO_APPLY_RECORD   - A CLR (Compensation Log Record): the page
 *                              change one UNDO record made when applied.
 *
 * CLR Redo Strategy
 * -----------------
 * CLRs are physiological: the rm_undo callbacks register the target buffer
 * with REGBUF_STANDARD and describe the change (mark a line pointer LP_DEAD,
 * or restore a before-image).  Redo goes through XLogReadBufferForRedo(), so
 * a full-page image is restored when one was taken, and otherwise the change
 * is replayed when the page LSN shows it is missing.  A CLR does not mark
 * the UNDO record itself as applied; see ApplyOneUndoRecord() for why
 * re-applying UNDO is still safe.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undo_xlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/atm.h"
#include "access/bufmask.h"
#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/undo_xlog.h"
#include "access/undolog.h"
#include "access/undorecord.h"
#include "access/undormgr.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

static void undo_recovery_publish_retention(void);

/*
 * undo_redo - Replay an UNDO WAL record during crash recovery
 *
 * This function handles all UNDO resource manager WAL record types; see
 * the file header for what each one does.
 */
void
undo_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{



		case XLOG_UNDO_APPLY_RECORD:
			{
				/*
				 * Physiological CLR redo: re-apply the exact page
				 * modification that was performed during UNDO application.
				 *
				 * If a full page image is present (BLK_RESTORED), the page is
				 * already correct.  Otherwise (BLK_NEEDS_REDO), replay the
				 * operation from the record: UNDO_CLR_LP_DEAD for index
				 * INSERT undo, UNDO_CLR_HAS_TUPLE to restore a table AM's
				 * before-image.
				 */
				xl_undo_apply *xlrec;
				Buffer		buffer;
				XLogRedoAction action;

				xlrec = (xl_undo_apply *) XLogRecGetData(record);
				action = XLogReadBufferForRedo(record, 0, &buffer);

				switch (action)
				{
					case BLK_RESTORED:
						/* Full page image applied -- nothing more to do */
						break;

					case BLK_DONE:
						/* Page already up-to-date (LSN check) */
						break;

					case BLK_NEEDS_REDO:
						{
							Page		page = BufferGetPage(buffer);

							if (xlrec->clr_flags & UNDO_CLR_LP_DEAD)
							{
								/*
								 * Mark the line pointer LP_DEAD, keeping its
								 * storage.  Used only by nbtree/hash index
								 * INSERT undo, where btree binary search
								 * still reads the dead tuple's key until
								 * VACUUM physically removes it.  Must match
								 * the forward-apply path (ItemIdMarkDead),
								 * which does not zero lp_off/lp_len.
								 */
								ItemId		lp = PageGetItemId(page,
															   xlrec->target_offset);

								if (ItemIdIsNormal(lp))
									ItemIdMarkDead(lp);
							}
							else if (xlrec->clr_flags & UNDO_CLR_HAS_TUPLE)
							{
								/*
								 * Restore a table AM's before-image (FLUX).
								 * The bytes are in the block data registered
								 * with block reference 0 and are opaque here:
								 * they are copied back into the line
								 * pointer's storage verbatim.
								 */
								ItemId		lp = PageGetItemId(page,
															   xlrec->target_offset);

								if (ItemIdIsUsed(lp) && ItemIdHasStorage(lp) &&
									xlrec->tuple_len > 0)
								{
									HeapTupleHeader htup;
									char	   *data;
									Size		datalen;

									data = XLogRecGetBlockData(record, 0,
															   &datalen);
									Assert(data != NULL);
									Assert(datalen >= xlrec->tuple_len);

									ItemIdSetNormal(lp, ItemIdGetOffset(lp),
													xlrec->tuple_len);
									htup = (HeapTupleHeader) PageGetItem(page, lp);
									memcpy(htup, data, xlrec->tuple_len);
								}
							}

							PageSetLSN(page, record->EndRecPtr);
							MarkBufferDirty(buffer);
						}
						break;

					case BLK_NOTFOUND:
						/* Block doesn't exist (truncated?) -- skip */
						break;
				}

				if (BufferIsValid(buffer))
					UnlockReleaseBuffer(buffer);
			}
			break;


		case XLOG_UNDO_BATCH:
			{
				xl_undo_batch *xlrec = (xl_undo_batch *) XLogRecGetData(record);

				/*
				 * During recovery, track this batch for incomplete
				 * transaction detection.  After redo completes, any
				 * transaction that wrote UNDO batches but did not commit will
				 * need its UNDO chain walked for rollback.
				 *
				 * The batch payload (serialized UNDO records) is part of the
				 * WAL record and can be re-read later via XLogReadRecord()
				 * during the undo phase.
				 */
				UndoRecoveryTrackBatch(xlrec->xid, xlrec->dboid,
									   record->ReadRecPtr,
									   xlrec->chain_prev,
									   xlrec->persistence);

				ereport(DEBUG2,
						(errmsg("undo_redo: BATCH xid %u, nrecords %u, "
								"total_len %u, chain_prev %X/%X",
								xlrec->xid, xlrec->nrecords,
								xlrec->total_len,
								LSN_FORMAT_ARGS(xlrec->chain_prev))));
			}
			break;

		case XLOG_UNDO_INFLIGHT:
			{
				xl_undo_inflight *xlrec = (xl_undo_inflight *) XLogRecGetData(record);

				for (uint32 i = 0; i < xlrec->nxacts; i++)
					UndoRecoveryTrackInflight(&xlrec->xacts[i]);
				undo_recovery_publish_retention();
			}
			break;


		default:
			elog(PANIC, "undo_redo: unknown op code %u", info);
	}
}

/* ----------------------------------------------------------------
 *	UNDO recovery tracking
 *
 *	During WAL redo, we track which transactions wrote UNDO batches.
 *	When a commit/abort record is redone, the XID is removed.
 *	After redo completes, remaining entries represent incomplete
 *	transactions that need their UNDO chains walked.
 * ----------------------------------------------------------------
 */

/* Hash table entry for tracking incomplete UNDO transactions */
typedef struct UndoRecoveryEntry
{
	TransactionId xid;			/* hash key */
	Oid			dboid;			/* database the transaction ran in */
	XLogRecPtr	last_batch_lsn[NUndoPersistenceLevels]; /* chain heads per
														 * persistence level */

	/*
	 * Oldest batch known to be in each chain: the chain_prev of the oldest
	 * batch redo saw, or that batch itself if it starts the chain.  Redo
	 * starts at the last checkpoint and so may not see the first batches.
	 */
	XLogRecPtr	first_batch_lsn[NUndoPersistenceLevels];
	char		status;			/* in use */
} UndoRecoveryEntry;

/* Simple dynamic array for recovery tracking (used during startup only) */
static UndoRecoveryEntry *undo_recovery_entries = NULL;
static int	undo_recovery_nentries = 0;
static int	undo_recovery_capacity = 0;

/*
 * Safety cap to prevent OOM during recovery.  If more than this many
 * distinct in-flight XIDs are found in WAL at crash time, we stop
 * tracking new ones and log a warning.  The untracked transactions will
 * need manual resolution (e.g. via pg_resetwal or targeted UNDO apply).
 *
 * 1 million entries x ~40 bytes each ~ 40 MB, which is reasonable for
 * a recovery-only allocation.
 */
#define UNDO_RECOVERY_MAX_ENTRIES	1048576

/*
 * undo_recovery_entry - Find or create the recovery entry for xid
 *
 * Returns NULL if the entry would exceed UNDO_RECOVERY_MAX_ENTRIES.
 */
static UndoRecoveryEntry *
undo_recovery_entry(TransactionId xid, Oid dboid)
{
	int			i;
	UndoRecoveryEntry *entry;

	for (i = 0; i < undo_recovery_nentries; i++)
	{
		if (undo_recovery_entries[i].xid == xid)
			return &undo_recovery_entries[i];
	}

	/* Safety cap: refuse to track more XIDs to prevent OOM */
	if (undo_recovery_nentries >= UNDO_RECOVERY_MAX_ENTRIES)
	{
		static bool warned = false;

		if (!warned)
		{
			ereport(WARNING,
					(errmsg("UNDO recovery: reached maximum tracked transaction limit (%d)",
							UNDO_RECOVERY_MAX_ENTRIES),
					 errhint("Transactions beyond this limit will not be automatically rolled back. "
							 "Manual intervention may be required after recovery completes.")));
			warned = true;
		}
		return NULL;
	}

	if (undo_recovery_nentries >= undo_recovery_capacity)
	{
		int			new_capacity = (undo_recovery_capacity == 0) ? 64 :
			undo_recovery_capacity * 2;
		UndoRecoveryEntry *new_entries;

		/* Clamp doubling to not exceed the safety cap */
		if (new_capacity > UNDO_RECOVERY_MAX_ENTRIES)
			new_capacity = UNDO_RECOVERY_MAX_ENTRIES;

		if (undo_recovery_entries == NULL)
		{
			new_entries = (UndoRecoveryEntry *)
				palloc0(sizeof(UndoRecoveryEntry) * new_capacity);
		}
		else
		{
			new_entries = (UndoRecoveryEntry *)
				repalloc(undo_recovery_entries,
						 sizeof(UndoRecoveryEntry) * new_capacity);
			memset(&new_entries[undo_recovery_capacity], 0,
				   sizeof(UndoRecoveryEntry) * (new_capacity - undo_recovery_capacity));
		}
		undo_recovery_entries = new_entries;
		undo_recovery_capacity = new_capacity;
	}

	entry = &undo_recovery_entries[undo_recovery_nentries++];
	entry->xid = xid;
	entry->dboid = dboid;
	for (i = 0; i < NUndoPersistenceLevels; i++)
	{
		entry->last_batch_lsn[i] = InvalidXLogRecPtr;
		entry->first_batch_lsn[i] = InvalidXLogRecPtr;
	}
	return entry;
}

/*
 * UndoRecoveryTrackBatch - Record an UNDO batch during WAL redo
 *
 * Called from the XLOG_UNDO_BATCH redo handler to track which
 * transactions have UNDO data that may need rollback.
 */
void
UndoRecoveryTrackBatch(TransactionId xid, Oid dboid, XLogRecPtr batch_lsn,
					   XLogRecPtr chain_prev,
					   UndoPersistenceLevel persistence)
{
	UndoRecoveryEntry *entry;

	if (!TransactionIdIsValid(xid))
		return;

	entry = undo_recovery_entry(xid, dboid);
	if (entry == NULL)
		return;

	/* Update the chain head and the oldest known batch of this level */
	if (persistence < NUndoPersistenceLevels)
	{
		XLogRecPtr	first = XLogRecPtrIsValid(chain_prev) ? chain_prev :
			batch_lsn;

		entry->last_batch_lsn[persistence] = batch_lsn;
		if (!XLogRecPtrIsValid(entry->first_batch_lsn[persistence]) ||
			first < entry->first_batch_lsn[persistence])
			entry->first_batch_lsn[persistence] = first;
	}
}

/*
 * UndoRecoveryTrackInflight - Merge a checkpoint's in-flight chain
 *
 * Called from the XLOG_UNDO_INFLIGHT redo handler.  The checkpointer read
 * the chain after the redo pointer, so redo may already have seen newer
 * batches of it: keep the newest head and the oldest first batch.  A
 * transaction whose outcome clog already knows finished between the read and
 * the record; it is not a loser.
 */
void
UndoRecoveryTrackInflight(const xl_undo_inflight_xact *x)
{
	UndoRecoveryEntry *entry;
	bool		any = false;

	if (!TransactionIdIsValid(x->xid))
		return;
	for (int i = 0; i < NUndoPersistenceLevels; i++)
		any |= XLogRecPtrIsValid(x->last_batch_lsn[i]);
	if (!any || TransactionIdDidCommit(x->xid) || TransactionIdDidAbort(x->xid))
		return;

	entry = undo_recovery_entry(x->xid, x->dboid);
	if (entry == NULL)
		return;

	for (int i = 0; i < NUndoPersistenceLevels; i++)
	{
		if (XLogRecPtrIsValid(x->last_batch_lsn[i]) &&
			x->last_batch_lsn[i] > entry->last_batch_lsn[i])
			entry->last_batch_lsn[i] = x->last_batch_lsn[i];
		if (XLogRecPtrIsValid(x->first_batch_lsn[i]) &&
			(!XLogRecPtrIsValid(entry->first_batch_lsn[i]) ||
			 x->first_batch_lsn[i] < entry->first_batch_lsn[i]))
			entry->first_batch_lsn[i] = x->first_batch_lsn[i];
	}
}

/*
 * undo_recovery_publish_retention - Keep the WAL of the tracked chains
 *
 * On a standby nothing else retains it: its restartpoints would recycle the
 * segments that promotion's UNDO phase has to read.
 */
static void
undo_recovery_publish_retention(void)
{
	XLogRecPtr	oldest = InvalidXLogRecPtr;

	for (int i = 0; i < undo_recovery_nentries; i++)
	{
		UndoRecoveryEntry *e = &undo_recovery_entries[i];

		if (!TransactionIdIsValid(e->xid))
			continue;
		for (int j = 0; j < NUndoPersistenceLevels; j++)
		{
			XLogRecPtr	lsn = XLogRecPtrIsValid(e->first_batch_lsn[j]) ?
				e->first_batch_lsn[j] : e->last_batch_lsn[j];

			if (XLogRecPtrIsValid(lsn) &&
				(!XLogRecPtrIsValid(oldest) || lsn < oldest))
				oldest = lsn;
		}
	}
	UndoSetRecoveryBatchLSN(oldest);
}

/*
 * UndoRecoveryRemoveXid - Remove an XID from recovery tracking
 *
 * Called when a commit or abort record is redone during recovery.
 * Committed transactions don't need UNDO rollback.  Before an abort record
 * is written, generic UNDO is either applied or registered in the ATM.
 * FILEOPS inverses are embedded in the abort record and applied by redo.
 * The ATM retains any outstanding generic rollback independently.
 */
void
UndoRecoveryRemoveXid(TransactionId xid)
{
	int			i;

	if (!TransactionIdIsValid(xid))
		return;

	for (i = 0; i < undo_recovery_nentries; i++)
	{
		if (undo_recovery_entries[i].xid == xid)
		{
			/* Mark as removed by zeroing XID */
			undo_recovery_entries[i].xid = InvalidTransactionId;
			break;
		}
	}
}

/*
 * UndoRecoveryNeeded - Check if there are incomplete transactions needing UNDO
 *
 * Returns true if any tracked transactions remain after redo is complete.
 */
bool
UndoRecoveryNeeded(void)
{
	int			i;

	for (i = 0; i < undo_recovery_nentries; i++)
	{
		if (TransactionIdIsValid(undo_recovery_entries[i].xid))
			return true;
	}

	return false;
}

/*
 * DeferredUndoXact - Transaction deferred for async UNDO processing
 *
 * During crash recovery, if syscache isn't available, we skip UNDO application
 * and defer the transaction for later processing by the logical revert worker.
 */
typedef struct DeferredUndoXact
{
	TransactionId xid;
	Oid			dboid;
	XLogRecPtr	last_batch_lsn;
	XLogRecPtr	first_batch_lsn;	/* oldest batch: WAL to retain */
	bool		fileops_owned;	/* startup completed the FILEOPS inverses */
	struct DeferredUndoXact *next;
} DeferredUndoXact;

static DeferredUndoXact *deferred_undo_xacts = NULL;

/*
 * PerformUndoRecovery - Walk and apply UNDO chains for incomplete transactions
 *
 * This is the ARIES-style undo phase, called after the redo loop completes.
 * For each incomplete transaction that wrote UNDO batches, we walk the
 * UNDO chain backward and apply each record via the RM dispatch table.
 *
 * CLRs are generated during this phase to ensure idempotency in case of
 * a crash during the undo phase itself.
 *
 * If UNDO application is skipped (e.g., due to syscache not being available),
 * the transaction is tracked for deferred processing after recovery completes.
 *
 * Return true if any FILEOPS inverse succeeded.  Before admitting writers,
 * recovery must checkpoint those effects and any deferred ownership, so another
 * crash cannot re-track the loser and undo a committed reuse of its paths.
 */
bool
PerformUndoRecovery(void)
{
	int			i,
				j;
	int			total_xacts = 0;
	int			total_records = 0;
	int			pending_xacts = 0;
	int			deferred_xacts = 0;
	bool		fileops_applied = false;

	/* Count pending transactions for the opening log message. */
	for (i = 0; i < undo_recovery_nentries; i++)
	{
		if (TransactionIdIsValid(undo_recovery_entries[i].xid))
			pending_xacts++;
	}

	if (pending_xacts > 0)
		ereport(LOG,
				(errmsg("UNDO recovery: %d incomplete transaction(s) to roll back",
						pending_xacts)));

	for (i = 0; i < undo_recovery_nentries; i++)
	{
		UndoRecoveryEntry *entry = &undo_recovery_entries[i];
		bool		any_skipped = false;
		bool		fileops_owned = true;
		XLogRecPtr	perm_first = InvalidXLogRecPtr;

		if (!TransactionIdIsValid(entry->xid))
			continue;

		/*
		 * Skip prepared transactions. Prepared (2PC) transactions must remain
		 * in the prepared state after crash recovery, not be automatically
		 * rolled back. They will be explicitly committed or rolled back later
		 * via COMMIT PREPARED or ROLLBACK PREPARED.
		 *
		 * During recovery, RecoveryTransactionIdIsPrepared() checks the
		 * in-memory prepared transaction state reconstructed from WAL replay.
		 */
		if (RecoveryTransactionIdIsPrepared(entry->xid))
		{
			ereport(LOG,
					(errmsg("UNDO recovery: skipping prepared transaction %u "
							"(will remain in prepared state)",
							entry->xid)));
			continue;
		}

		total_xacts++;

		ereport(LOG,
				(errmsg("UNDO recovery: rolling back transaction %u",
						entry->xid)));

		/*
		 * Walk each persistence level's UNDO chain independently. This
		 * mirrors the normal abort path in AtAbort_XactUndo().
		 *
		 * TEMP and UNLOGGED levels are skipped during crash recovery: - TEMP:
		 * temporary tables are destroyed on server restart, so there is
		 * nothing to roll back and the pages no longer exist. - UNLOGGED:
		 * unlogged table files are reset to empty on crash recovery
		 * (initfork), making any prior UNDO application wrong.
		 */
		for (j = 0; j < NUndoPersistenceLevels; j++)
		{
			XLogRecPtr	batch_lsn = entry->last_batch_lsn[j];

			if (j == UNDOPERSISTENCE_TEMP || j == UNDOPERSISTENCE_UNLOGGED)
				continue;

			if (XLogRecPtrIsValid(batch_lsn))
			{
				UndoChainWalkResult res;

				/*
				 * Same walk, and so the same newest-first order, as online
				 * rollback.  A batch that cannot be read (fsync=off, or WAL
				 * recycled too early) ends the walk with a WARNING rather
				 * than a PANIC, which would make the cluster unrecoverable;
				 * whatever it covered is left for VACUUM.
				 */
				(void) UndoWalkChain(batch_lsn, InvalidXLogRecPtr, &res);
				total_records += res.applied + res.skipped + res.failed;
				fileops_owned &= res.complete && !res.fileops_failed;
				fileops_applied |= res.fileops_applied;

				/*
				 * A record skipped here (an index AM needs the syscache,
				 * which the startup process does not have) or one that failed
				 * is handed to the logical revert worker, which walks the
				 * whole chain again.  FILEOPS cannot distinguish a new file
				 * at the same path, so the ATM must remember when startup
				 * completed all its inverses.  Index callbacks skip their
				 * records there too (see UndoApplyInline).
				 */
				if (!res.complete || res.skipped > 0 || res.failed > 0)
					any_skipped = true;

				/*
				 * The deferred entry must retain WAL from the chain's first
				 * batch.  A complete walk read it (oldest_batch).  One that
				 * stopped early did not, but the batch it could not read and
				 * the oldest batch redo knows of are no newer than the ones
				 * it did read; take the oldest of all.
				 */
				if (j == UNDOPERSISTENCE_PERMANENT)
				{
					perm_first = entry->first_batch_lsn[j];
					if (XLogRecPtrIsValid(res.oldest_batch) &&
						res.oldest_batch < perm_first)
						perm_first = res.oldest_batch;
					if (!res.complete &&
						XLogRecPtrIsValid(res.unread_batch) &&
						res.unread_batch < perm_first)
						perm_first = res.unread_batch;
				}
			}
		}

		/*
		 * If any UNDO records were skipped (e.g., due to syscache not being
		 * initialized during early recovery), track this transaction for
		 * deferred processing.  ATMAddAborted() writes WAL, which isn't
		 * allowed yet, so we put the entry into the shared ATM without WAL
		 * now and remember it on an in-memory list that
		 * FlushDeferredUndoXacts() WAL-logs once recovery is over.
		 *
		 * The entry has to be in the ATM now, not later: the end-of-recovery
		 * checkpoint runs before FlushDeferredUndoXacts(), and only the ATM
		 * keeps that checkpoint from recycling the WAL that holds this chain.
		 *
		 * Use the permanent persistence level's last_batch_lsn for tracking.
		 * TEMP and UNLOGGED are skipped during crash recovery anyway.
		 */
		if (any_skipped)
		{
			XLogRecPtr	perm_lsn = entry->last_batch_lsn[UNDOPERSISTENCE_PERMANENT];

			if (XLogRecPtrIsValid(perm_lsn))
			{
				DeferredUndoXact *deferred = (DeferredUndoXact *)
					palloc(sizeof(DeferredUndoXact));

				/*
				 * The database comes from the WAL, not MyDatabaseId: the
				 * startup process is connected to no database, and the revert
				 * worker that will pick this up only serves entries of its
				 * own database.
				 */
				deferred->xid = entry->xid;
				deferred->dboid = entry->dboid;
				deferred->last_batch_lsn = perm_lsn;
				deferred->first_batch_lsn = perm_first;
				deferred->fileops_owned = fileops_owned;
				deferred->next = deferred_undo_xacts;
				deferred_undo_xacts = deferred;

				ATMAddAbortedNoWAL(deferred->xid, deferred->dboid, perm_lsn,
								   perm_first, fileops_owned);

				deferred_xacts++;
				ereport(LOG,
						(errmsg("UNDO recovery: transaction %u deferred to "
								"logical revert worker (syscache not ready)",
								entry->xid)));
			}
		}
	}

	if (total_xacts > 0)
	{
		if (deferred_xacts > 0)
			ereport(LOG,
					(errmsg("UNDO recovery complete: %d transactions processed, "
							"%d records applied, %d transactions deferred to "
							"logical revert worker",
							total_xacts, total_records, deferred_xacts)));
		else
			ereport(LOG,
					(errmsg("UNDO recovery complete: %d transactions rolled back, "
							"%d records applied",
							total_xacts, total_records)));
	}

	/* Free tracking data */
	if (undo_recovery_entries != NULL)
	{
		pfree(undo_recovery_entries);
		undo_recovery_entries = NULL;
	}
	undo_recovery_nentries = 0;
	undo_recovery_capacity = 0;

	return fileops_applied;
}

/*
 * FlushDeferredUndoXacts - WAL-log the transactions deferred to the ATM
 *
 * Called after recovery completes (when InRedo is false).  PerformUndoRecovery()
 * already put each deferred transaction into the Aborted Transaction Map
 * without WAL; this writes the XLOG_ATM_ABORT records, including ownership of
 * the completed FILEOPS inverses, so standbys learn about them too.
 * The logical revert worker of each entry's database processes them
 * asynchronously.
 *
 * This must be called after InRedo is set to false because ATMAddAborted()
 * writes WAL, which is not allowed during recovery.
 */
void
FlushDeferredUndoXacts(void)
{
	DeferredUndoXact *deferred;
	int			count = 0;

	if (deferred_undo_xacts == NULL)
		return;

	ereport(LOG,
			(errmsg("flushing deferred UNDO transactions to ATM")));

	/* Walk the list and add each transaction to the ATM */
	while (deferred_undo_xacts != NULL)
	{
		deferred = deferred_undo_xacts;
		deferred_undo_xacts = deferred->next;

		ATMAddAborted(deferred->xid, deferred->dboid, deferred->last_batch_lsn,
					  deferred->first_batch_lsn, deferred->fileops_owned);
		count++;

		pfree(deferred);
	}

	if (count > 0)
		ereport(LOG,
				(errmsg("added %d deferred transaction(s) to ATM for async UNDO processing",
						count)));
}

/* ----------------------------------------------------------------
 *	UNDO batch reading from WAL
 * ----------------------------------------------------------------
 */

/*
 * UndoReadBatchFromWAL - Read a single XLOG_UNDO_BATCH record from WAL
 *
 * Uses XLogReader to read the WAL record at the given LSN.
 * Returns a palloc'd UndoBatchData containing the header and a copy
 * of the payload.  The caller must pfree via UndoFreeBatchData().
 *
 * Returns NULL if the record cannot be read or is not an UNDO batch.  In the
 * latter case, a record that was read but is not a well-formed UNDO batch,
 * *malformed is set: reading it again will not help.
 */
/*
 * Module-level cached XLogReader for UndoReadBatchFromWAL.
 * Allocated once and reused across calls to avoid per-batch
 * open/close overhead on WAL segment files during rollback.
 */
static XLogReaderState *undo_batch_reader = NULL;
static XLogReaderRoutine undo_batch_reader_routine = {
	.page_read = read_local_xlog_page,
	.segment_open = wal_segment_open,
	.segment_close = wal_segment_close,
};

/*
 * UndoValidateBatchLSN
 *		Quick check that the WAL record at batch_lsn is a valid UNDO source.
 *
 * Returns true if the record is a standalone RM_UNDO_ID batch.  Returns
 * false for any other unrecognized record type.  Used by the inline UNDO
 * path to avoid calling ApplyUndoChainFromWAL on a batch_lsn that points to
 * the wrong record type.
 */
bool
UndoValidateBatchLSN(XLogRecPtr batch_lsn)
{
	XLogRecord *record_hdr;
	char	   *errormsg = NULL;
	uint8		rmid;
	MemoryContext old_ctx;
	bool		result = false;

	if (!XLogRecPtrIsValid(batch_lsn))
		return false;

	/*
	 * Perform the entire read under TopMemoryContext.  The cached reader and,
	 * critically, its lazily-allocated decode_buffer (xlogreader.c) are
	 * palloc'd in CurrentMemoryContext.  Callers such as the inline-abort
	 * path run with a transient context that is deleted right after;
	 * allocating the reader state there would leave the static
	 * undo_batch_reader (and its decode buffer) dangling, crashing the next
	 * abort.  TopMemoryContext makes the cache truly persistent.
	 */
	old_ctx = MemoryContextSwitchTo(TopMemoryContext);

	if (undo_batch_reader == NULL)
	{
		undo_batch_reader = XLogReaderAllocate(wal_segment_size, NULL,
											   &undo_batch_reader_routine, NULL);
		if (undo_batch_reader == NULL)
		{
			MemoryContextSwitchTo(old_ctx);
			return false;
		}
	}

	XLogBeginRead(undo_batch_reader, batch_lsn);
	record_hdr = XLogReadRecord(undo_batch_reader, &errormsg);
	if (record_hdr == NULL)
	{
		MemoryContextSwitchTo(old_ctx);
		return false;
	}

	rmid = XLogRecGetRmid(undo_batch_reader);

	/* Only a standalone UNDO batch is a valid inline UNDO source */
	if (rmid == RM_UNDO_ID)
		result = true;

	MemoryContextSwitchTo(old_ctx);
	return result;
}

UndoBatchData *
UndoReadBatchFromWAL(XLogRecPtr batch_lsn, bool *malformed)
{
	XLogRecord *record_hdr;
	char	   *errormsg = NULL;
	UndoBatchData *result;
	xl_undo_batch *xlrec;
	char	   *record_data;
	Size		record_len;
	Size		payload_offset;

	*malformed = false;

	if (!XLogRecPtrIsValid(batch_lsn))
		return NULL;

	/* Test hook: simulate a batch that cannot be read right now. */
	if (IS_INJECTION_POINT_ATTACHED("undo-read-batch-fail"))
	{
		ereport(WARNING,
				(errmsg("UNDO batch at %X/%X: read failed (injection point)",
						LSN_FORMAT_ARGS(batch_lsn))));
		return NULL;
	}

	/*
	 * Safety check: verify the WAL segment containing this LSN has not been
	 * recycled by a checkpoint.  If the LSN is behind the current redo
	 * pointer and the segment file doesn't exist, reading would cause SIGBUS
	 * (signal 10: Bus error) or SIGSEGV.  Return NULL gracefully instead.
	 *
	 * Compare against GetRedoRecPtr(); if our target is well behind the redo
	 * pointer AND behind the last checkpoint's redo location, the segment may
	 * have been recycled.
	 */
	{
		XLogRecPtr	redo_ptr = GetRedoRecPtr();
		XLogSegNo	target_segno;
		char		path[MAXPGPATH];
		TimeLineID	tli;

		if (batch_lsn < redo_ptr)
		{
			/*
			 * Determine the timeline for the segment path.
			 * GetWALInsertionTimeLine() is only valid once recovery is DONE
			 * (it asserts RECOVERY_STATE_DONE); during the crash-recovery
			 * undo phase we must instead use the replay timeline.
			 * RecoveryInProgress() distinguishes the two callers
			 * (crash-recovery undo phase vs. online abort / logical revert
			 * worker).
			 */
			if (RecoveryInProgress())
			{
				(void) GetXLogReplayRecPtr(&tli);
				if (tli == 0)
					tli = 1;	/* no replay position yet: default to TLI 1 */
			}
			else
				tli = GetWALInsertionTimeLine();

			XLByteToSeg(batch_lsn, target_segno, wal_segment_size);
			XLogFilePath(path, tli, target_segno,
						 wal_segment_size);

			if (access(path, F_OK) != 0)
			{
				ereport(WARNING,
						(errmsg("UNDO batch at %X/%X: WAL segment \"%s\" no longer "
								"exists (recycled by checkpoint); skipping rollback",
								LSN_FORMAT_ARGS(batch_lsn), path)));
				return NULL;
			}
		}
	}

	/*
	 * Allocate the reader and perform the read under TopMemoryContext.  Both
	 * the cached reader and its lazily-allocated decode_buffer (xlogreader.c)
	 * are palloc'd in CurrentMemoryContext; if that is a transient caller
	 * context (e.g. the inline-abort context) it gets deleted, leaving the
	 * static undo_batch_reader and its decode buffer dangling and crashing
	 * the next abort.  We switch back to the caller's context before
	 * allocating the returned UndoBatchData, which the caller owns.  See the
	 * matching comment in UndoValidateBatchLSN.
	 */
	{
		MemoryContext read_ctx = MemoryContextSwitchTo(TopMemoryContext);

		if (undo_batch_reader == NULL)
		{
			undo_batch_reader = XLogReaderAllocate(wal_segment_size, NULL,
												   &undo_batch_reader_routine, NULL);
			if (undo_batch_reader == NULL)
			{
				MemoryContextSwitchTo(read_ctx);
				ereport(WARNING,
						(errmsg("could not allocate XLogReader for UNDO batch read")));
				return NULL;
			}
		}

		/* Position the reader at the target LSN, then read */
		XLogBeginRead(undo_batch_reader, batch_lsn);
		record_hdr = XLogReadRecord(undo_batch_reader, &errormsg);
		MemoryContextSwitchTo(read_ctx);
	}
	if (record_hdr == NULL)
	{
		if (errormsg)
			ereport(WARNING,
					(errmsg("could not read WAL record at %X/%X: %s",
							LSN_FORMAT_ARGS(batch_lsn), errormsg)));
		return NULL;
	}

	/*
	 * Determine record format: a standalone XLOG_UNDO_BATCH record carrying
	 * the UNDO payload.
	 */
	record_data = XLogRecGetData(undo_batch_reader);
	record_len = XLogRecGetDataLen(undo_batch_reader);

	if (XLogRecGetRmid(undo_batch_reader) == RM_UNDO_ID &&
		(XLogRecGetInfo(undo_batch_reader) & ~XLR_INFO_MASK) == XLOG_UNDO_BATCH)
	{
		/* Standalone XLOG_UNDO_BATCH record */
		if (record_len < SizeOfUndoBatch)
		{
			ereport(WARNING,
					(errmsg("UNDO batch record at %X/%X too short: %zu bytes",
							LSN_FORMAT_ARGS(batch_lsn), record_len)));
			*malformed = true;
			return NULL;
		}

		xlrec = (xl_undo_batch *) record_data;
		payload_offset = SizeOfUndoBatch;
	}
	else
	{
		ereport(WARNING,
				(errmsg("WAL record at %X/%X is not an UNDO batch (rmid=%u, info=0x%02x)",
						LSN_FORMAT_ARGS(batch_lsn),
						XLogRecGetRmid(undo_batch_reader),
						XLogRecGetInfo(undo_batch_reader) & ~XLR_INFO_MASK)));
		*malformed = true;
		return NULL;
	}

	/*
	 * Validate that the claimed payload length fits within the WAL record. A
	 * mismatch here means the WAL segment was recycled and overwritten (the
	 * LSN now points to a different record), or the record is corrupt.
	 * Without this check, the memcpy below would read past the XLogReader's
	 * internal buffer, potentially accessing unmapped memory
	 * (SIGBUS/SIGSEGV).
	 */
	if (payload_offset + (Size) xlrec->total_len > record_len)
	{
		ereport(WARNING,
				(errmsg("UNDO batch at %X/%X: payload length %u exceeds "
						"record data (offset %zu, record_len %zu)",
						LSN_FORMAT_ARGS(batch_lsn),
						xlrec->total_len, payload_offset, record_len)));
		*malformed = true;
		return NULL;
	}

	/*
	 * Allocate UndoBatchData. We use palloc (CurrentMemoryContext) because
	 * this structure is only needed until ApplyUndoChainFromWAL processes the
	 * batch. We intentionally do NOT pfree in UndoFreeBatchData() because
	 * calling pfree on BumpContext memory would ERROR. The memory will be
	 * reclaimed when the current memory context is reset.
	 */
	result = (UndoBatchData *) palloc(sizeof(UndoBatchData));
	memcpy(&result->header, xlrec, SizeOfUndoBatch);
	result->payload_len = (Size) xlrec->total_len;
	if (result->payload_len > 0)
	{
		result->payload = (char *) palloc(result->payload_len);
		memcpy(result->payload, record_data + payload_offset,
			   result->payload_len);
	}
	else
	{
		result->payload = NULL;
	}

	/* Do not free reader -- it is cached for reuse. */
	return result;
}

/*
 * UndoFreeBatchData - Release a UndoBatchData structure
 *
 * This is a no-op function. We don't actually pfree the batch or payload
 * because they were allocated with palloc() from CurrentMemoryContext, which
 * may be a BumpContext. Calling pfree on BumpContext memory would ERROR.
 * The memory will be automatically reclaimed when the current memory context
 * is reset (e.g., at end of query, transaction, or subtransaction).
 *
 * This function exists to maintain API compatibility and to serve as a
 * clear marker in the code where batch data is no longer needed.
 */
void
UndoFreeBatchData(UndoBatchData *batch)
{
	/* Intentionally empty - memory reclaimed by context reset */
	(void) batch;
}

/*
 * UndoResetBatchReader - Free and NULL the cached WAL reader.
 *
 * Must be called after a PG_CATCH that could leave the static reader in
 * an inconsistent state (stale segment FD, partial read buffer, etc.).
 * The next call to UndoReadBatchFromWAL will reallocate a fresh reader.
 */
void
UndoResetBatchReader(void)
{
	if (undo_batch_reader != NULL)
	{
		XLogReaderFree(undo_batch_reader);
		undo_batch_reader = NULL;
	}
}

/*
 * undo_mask - mask a page changed by an Undo record before consistency checks
 *
 * Only XLOG_UNDO_APPLY_RECORD registers a block.  Its target is a page of the
 * table AM or index AM that applied the UNDO record, so hand the page to that
 * AM's mask function, telling them apart by the special space the way
 * pg_filedump does: hash pages end their 16-byte special space with
 * HASHO_PAGE_ID, which no btree cycle ID can equal (MAX_BT_CYCLE_ID).
 */
void
undo_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	Size		special = PageGetSpecialSize(page);

	if (special == MAXALIGN(sizeof(HashPageOpaqueData)) &&
		HashPageGetOpaque(page)->hasho_page_id == HASHO_PAGE_ID)
		hash_mask(pagedata, blkno);
	else if (special == MAXALIGN(sizeof(BTPageOpaqueData)))
		btree_mask(pagedata, blkno);
	else
	{
		mask_page_lsn_and_checksum(page);
		mask_page_hint_bits(page);
		mask_unused_space(page);
	}
}
