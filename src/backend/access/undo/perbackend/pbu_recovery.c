/*-------------------------------------------------------------------------
 *
 * pbu_recovery.c
 *	  per-backend UNDO crash-recovery apply driver.
 *
 * After WAL redo reaches consistency, the ARIES-style undo phase must roll
 * back transactions that produced per-backend UNDO but did not commit before
 * the crash.  StartupUndoLogs() has already rebuilt the UndoLogControl banks
 * (insert/discard pointers) from the pg_undo/<redo> checkpoint snapshot, and
 * redo of the RM_UNDOLOG_ID records advanced them and rebuilt the xid->logno
 * map; the undo-page contents themselves were made durable by riding on their
 * producer's WAL record (RegisterUndoLogBuffers) and were replayed into the
 * shared buffers.  So the recovered undo logs contain, in insertion order, the
 * transaction-header chain: each transaction's first record carries a
 * UndoRecordTransaction header with the owning xid, its database, an
 * apply-progress marker, and uur_next -- the start location of the next
 * transaction.
 *
 * PbuPerformUndoRecovery() walks that chain across all active undo logs and,
 * for every LOSER transaction (wrote undo, did not commit, and is not a
 * prepared 2PC transaction awaiting explicit resolution), reverse-applies its
 * undo via execute_undo_actions() -- the same apply path used on live abort
 * (PbuAtAbort_ApplyUndo).  execute_undo_actions() is idempotent: it consults
 * the transaction header's uur_progress marker and returns early if the undo
 * was already (fully) applied, so re-processing a chain (e.g. reached again
 * from a second log) is harmless.
 *
 * IN-FLIGHT LOSERS AND DEFERRED COMPLETION
 *
 * The inline apply cannot finish the job for a table or index AM.  Every AM
 * undo callback (flux_undo_apply and the nbtree/hash callbacks) returns
 * UNDO_APPLY_SKIPPED unconditionally while InRecovery is set: reverting a row
 * needs the relcache/syscache to open the relation, and reversing the index
 * side of an in-place indexed-column UPDATE needs the index AMs -- neither of
 * which the startup process has (calling the index AMs from it crashes).  So
 * for a transaction that was an IN-FLIGHT LOSER at the crash instant -- its
 * ROLLBACK had not been applied, and a CHECKPOINT had already made its
 * post-update pages durable -- the inline apply reverts nothing at all.
 *
 * That outcome used to be indistinguishable from success, so such a transaction
 * was silently abandoned in its post-update state: the heap kept the
 * uncommitted value and the indexes kept the uncommitted key.
 *
 * execute_undo_actions() now reports whether every record was applied, and a
 * transaction it could not finish is registered as a rollback request --
 * PbuRegisterRecoveredRollbackReq() -- in the same shared rollback hash table
 * and xid/size queues a live abort uses.  After recovery the undo launcher
 * starts an undo apply worker for that database, which connects and calls
 * execute_undo_actions() with relcache, syscache and the index AMs available:
 * the same code path, and the same end state, as a live ROLLBACK.
 *
 * The chain also stays put until the deferred rollback has consumed it: the
 * discard worker stops at the first transaction in a log that did not commit
 * and whose transaction header does not yet record the undo as applied
 * (uur_progress == 0; see UndoDiscardOneLog), and the progress marker is only
 * set once the worker has applied every record.
 *
 * Between the end of recovery and the worker's pass, a loser transaction's rows
 * are still physically present with their uncommitted values, but they are not
 * visible: FLUX resolves visibility through the status of the writing xid,
 * and that xid did not commit.  Readers therefore see
 * the pre-update row throughout.  The deferred apply restores the before-image
 * physically and repairs the index entries.
 *
 * If no undo apply worker can run (single-user mode) the request cannot be
 * drained, so we report a WARNING naming the transaction instead of claiming a
 * rollback that will not happen.
 *
 * Prepared (2PC) transactions are skipped here: their per-backend undo
 * location is persisted in the 2PC state file (see twophase.c) and applied by
 * pbu_twophase_postabort() on ROLLBACK PREPARED, or discarded on COMMIT
 * PREPARED.
 *
 * This runs before WAL insertion is re-enabled, matching the cluster-wide
 * engine's PerformUndoRecovery(); the apply itself writes no new WAL (durability of the
 * revert is provided by the end-of-recovery checkpoint), and the apply
 * callbacks that need syscache (e.g. nbtree) defer to the post-recovery undo
 * apply worker via UNDO_APPLY_SKIPPED (they check InRecovery).  FILEOPS undo
 * apply is path-based and completes synchronously here.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/perbackend/pbu_recovery.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/injection_point.h"

#include "access/perbackend/pbu_compat.h"
#include "access/perbackend/pbu_undoinsert.h"
#include "access/perbackend/pbu_undolog.h"
#include "access/perbackend/pbu_undorecord.h"
#include "access/perbackend/pbu_undorequest.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "utils/memutils.h"

/*
 * pbu_undoworker.h cannot be included here (its types collide with the ones
 * pbu_compat.h pulls in), so declare the symbol we need from it.
 */
extern void PbuEnsureUndoLauncher(void);

/*
 * On-disk 2PC record for a per-backend UNDO chain.  Persisted at EndPrepare()
 * and consumed on COMMIT/ROLLBACK PREPARED.  The owning full xid is supplied
 * to the callbacks separately (TwoPhaseCallback fxid arg), so it is not stored
 * here.
 *
 * Both ends of the chain are needed: the apply walks backward from the latest
 * record to the first one.
 */
typedef struct PbuTwoPhaseUndoRecord
{
	uint64		start_urec_ptr; /* first UndoRecPtr of this xact */
	uint64		latest_urec_ptr;	/* most recent UndoRecPtr of this xact */
	Oid			dbid;			/* database the undo belongs to */
} PbuTwoPhaseUndoRecord;

#define PBU_2PC_UNDO		0x0001

static bool PbuRegisterRecoveredRollbackReq(FullTransactionId full_xid,
											UndoRecPtr start_urec_ptr,
											UndoRecPtr end_urec_ptr,
											Oid dbid, TransactionId xid);

/*
 * PbuRegisterRecoveredRollbackReq
 *		Queue a loser transaction whose undo could not be applied inline for
 *		completion by the undo apply worker after recovery finishes.
 *
 * Returns true if the request was queued (or was already queued by an earlier
 * pass over the same chain), false if there is no way to drain it, in which
 * case the caller must not count the transaction as rolled back.
 *
 * RegisterRollbackReq() enters the request in the shared rollback hash table
 * and pushes it onto the xid and size queues.  Two properties matter here:
 *
 *  - The hash-table entry makes the discard worker leave this transaction's
 *    undo alone, so the chain survives until the deferred rollback consumes it.
 *  - The queue entry is what the undo launcher polls; it runs with
 *    bgw_start_time = BgWorkerStart_RecoveryFinished, so it begins draining
 *    exactly when the system opens for writes, which is the earliest moment the
 *    apply can legally run.
 *
 * We pass end_urec_ptr explicitly instead of letting RegisterRollbackReq derive
 * it.  The derivation (FindUndoEndLocationAndSize) resolves the chain end from
 * the log's live insert pointer and asserts the transaction is the log's current
 * owner; for a chain recovered from a crashed log neither holds, whereas our
 * caller has just computed the exact boundary during the chain walk.  Note the
 * convention: end_urec_ptr is the transaction's LATEST undo record (inclusive),
 * which is what UndoWorkerPerformRequest() hands to execute_undo_actions() as
 * its 'from' pointer -- not the exclusive end of the chain's address range.
 *
 * RegisterRollbackReq() returns false in single-user mode (!IsUnderPostmaster),
 * where its normal contract is "the calling backend should apply the undo
 * itself".  The startup process must not do that (no relcache, no index AMs),
 * and in single-user mode there is no worker either, so that case is reported to
 * the caller as un-drainable rather than silently dropped.
 */
static bool
PbuRegisterRecoveredRollbackReq(FullTransactionId full_xid,
								UndoRecPtr start_urec_ptr,
								UndoRecPtr end_urec_ptr,
								Oid dbid, TransactionId xid)
{
	/*
	 * A dropped database leaves nothing to revert: the relations are gone, so
	 * the undo has no target.  (RegisterRollbackReq asserts on InvalidOid.)
	 */
	if (!OidIsValid(dbid))
		return true;

	if (!IsUnderPostmaster)
	{
		ereport(WARNING,
				(errmsg("per-backend UNDO recovery: rollback of transaction %u "
						"cannot be completed in single-user mode", xid),
				 errdetail("The transaction did not commit, so its rows are not "
						   "visible, but restoring their before-images and "
						   "repairing index entries needs an undo apply worker."),
				 errhint("Restart the server normally to let the undo apply "
						 "worker complete this rollback.")));
		return false;
	}

	if (!RollbackHTIsFull())
		(void) RegisterRollbackReq(end_urec_ptr, start_urec_ptr, dbid, full_xid);
	else
	{
		ereport(WARNING,
				(errmsg("per-backend UNDO recovery: rollback request table is "
						"full; rollback of transaction %u is still outstanding",
						xid)));
		return false;
	}

	/*
	 * The queued request is drained by an undo apply worker, started by the
	 * undo launcher once recovery has finished.  Warn if the configuration
	 * leaves no launcher to do that.
	 */
	PbuEnsureUndoLauncher();

	ereport(LOG,
			(errmsg("per-backend UNDO recovery: queued rollback of transaction "
					"%u for the undo apply worker", xid)));

	return true;
}

/*
 * PbuPerformUndoRecovery
 *		Reverse-apply the per-backend UNDO of loser transactions at crash
 *		recovery.
 *
 * Called from the recovery driver (PerformWalRecovery in xlogrecovery.c) after
 * the redo loop finishes, alongside the cluster-wide engine's
 * PerformUndoRecovery().
 * Returns the number of transactions whose undo was applied or queued for
 * deferred completion.
 *
 * A transaction with table or index undo cannot be reverted here (the AM
 * callbacks all skip while InRecovery); it is queued for the undo apply worker
 * instead.  See the file header.
 */
int
PbuPerformUndoRecovery(void)
{
	UndoLogControl *log = NULL;
	int			applied = 0;
	int			deferred = 0;
	int			abandoned = 0;
	MemoryContext oldctx;
	MemoryContext workctx;

	/*
	 * Do everything in a scratch context so the per-record pallocs from the
	 * chain walk / bulk fetch don't accumulate in whatever context recovery
	 * left us in.
	 */
	workctx = AllocSetContextCreate(CurrentMemoryContext,
									"PbuUndoRecovery",
									ALLOCSET_DEFAULT_SIZES);
	oldctx = MemoryContextSwitchTo(workctx);

	/*
	 * Visit every undo log currently known to the engine.  For each log with
	 * live (non-discarded) data, walk its transaction-header chain from the
	 * oldest valid record forward and roll back the losers.
	 */
	while ((log = UndoLogNext(log)) != NULL)
	{
		UndoRecPtr	cur;
		UndoLogNumber logno = log->logno;

		/*
		 * Unlogged and temporary undo did not survive the crash: its pages
		 * were never WAL-logged, and the tables it belongs to are being
		 * reset. StartupXLOG() empties those logs
		 * (ResetNonPermanentUndoLogs).
		 */
		if (log->meta.persistence != UNDO_PERMANENT)
			continue;

		/* First non-discarded record in this log, if any. */
		cur = UndoLogGetFirstValidRecord(logno);

		while (UndoRecPtrIsValid(cur))
		{
			UnpackedUndoRecord *uur;
			FullTransactionId full_xid;
			TransactionId xid;
			Oid			dbid;
			UndoRecPtr	txn_start = cur;
			UndoRecPtr	next_txn;

			CHECK_FOR_INTERRUPTS();

			/*
			 * Only follow the chain within a log while the pointer stays in a
			 * live (allocated, non-discarded) region.  A pointer into an
			 * already-discarded range means that transaction's undo was fully
			 * applied and discarded before the crash.
			 */
			if (UndoLogIsDiscarded(cur))
				break;

			uur = UndoFetchRecord(txn_start, InvalidBlockNumber,
								  InvalidOffsetNumber, InvalidTransactionId,
								  NULL, NULL);
			if (uur == NULL)
				break;			/* discarded/rewound concurrently */

			/*
			 * The first record of a transaction in a log carries the
			 * transaction header (xid, dbid, uur_next).  If this record is
			 * not a transaction header we have lost the chain anchor; stop
			 * walking this log.
			 */
			if (!(uur->uur_info & UREC_INFO_TRANSACTION))
			{
				UndoRecordRelease(uur);
				break;
			}

			xid = uur->uur_xid;
			dbid = uur->uur_dbid;
			full_xid = FullTransactionIdFromEpochAndXid(uur->uur_xidepoch, xid);
			next_txn = uur->uur_next;
			UndoRecordRelease(uur);
			uur = NULL;

			/*
			 * Decide whether this transaction is a loser that must be rolled
			 * back.  Committed transactions keep their effects.  Prepared 2PC
			 * transactions stay prepared (their undo is owned by the 2PC
			 * state and applied on ROLLBACK PREPARED).  Everything else -- an
			 * in-flight transaction that crashed, or one already marked
			 * aborted but not yet fully undone -- is a loser.
			 */
			if (TransactionIdIsValid(xid) &&
				!TransactionIdDidCommit(xid) &&
				!RecoveryTransactionIdIsPrepared(xid))
			{
				UndoRecPtr	from_ptr;
				UndoRecPtr	end_ptr;

				ereport(LOG,
						(errmsg("per-backend UNDO recovery: rolling back "
								"transaction %u (log %d)", xid, logno)));

				/*
				 * Compute the LATEST (newest) undo record of this
				 * transaction: execute_undo_actions()/UndoRecordBulkFetch
				 * walk the chain BACKWARD from `from` (latest) down to `to`
				 * (txn_start), so we must start at the last record, not the
				 * first.  The transaction's undo occupies [txn_start, end):
				 * end is the next transaction's start when it is in this same
				 * log, otherwise the log's current insert pointer.  The
				 * latest record pointer is the record immediately before
				 * `end`.
				 */
				if (UndoRecPtrIsValid(next_txn) &&
					UndoRecPtrGetLogNo(next_txn) == logno)
					end_ptr = next_txn;
				else
				{
					UndoLogOffset next_insert;

					next_insert = (UndoLogOffset) MakeUndoRecPtr(logno,
																 log->meta.insert);

					/*
					 * If the insert pointer sits at the very start of a page
					 * (just past the block header), step back over the header
					 * so UndoGetPrevUndoRecptr finds the last record on the
					 * previous page (mirrors FindUndoEndLocationAndSize).
					 */
					if (UndoRecPtrGetPageOffset(next_insert) == UndoLogBlockHeaderSize)
						next_insert -= UndoLogBlockHeaderSize;
					end_ptr = next_insert;
				}

				from_ptr = UndoGetPrevUndoRecptr(end_ptr, InvalidUndoRecPtr, NULL);
				if (!UndoRecPtrIsValid(from_ptr))
					from_ptr = txn_start;

				/*
				 * Reverse-apply the whole chain for this transaction inline,
				 * and queue what the inline apply could not finish.
				 *
				 * execute_undo_actions(from=latest, to=txn_start) walks the
				 * chain backward from the newest record to the transaction
				 * start, applying each record's compensating action in LIFO
				 * order; nopartial = true (complete transaction).  It returns
				 * false if any record came back UNDO_APPLY_SKIPPED -- which
				 * is what every AM callback does while InRecovery -- or
				 * errored, meaning the transaction's heap and index effects
				 * are still in place and the rollback has to be completed
				 * after recovery.
				 */

				/*
				 * Like redo, an ERROR here must fail startup and retry next
				 * time.
				 */
				INJECTION_POINT("pbu-recovery-error", NULL);
				if (execute_undo_actions(full_xid, from_ptr, txn_start, true))
					applied++;
				else if (PbuRegisterRecoveredRollbackReq(full_xid, txn_start,
														 from_ptr, dbid, xid))
					deferred++;
				else
					abandoned++;
			}

			/*
			 * Advance to the next transaction in insertion order.  Stop if
			 * there is none, if it loops back, or if it leaves the set of
			 * active logs we are iterating (it will be reached when we visit
			 * that log).
			 */
			if (!UndoRecPtrIsValid(next_txn) || next_txn == cur)
				break;
			if (UndoRecPtrGetLogNo(next_txn) != logno)
				break;
			cur = next_txn;

			/* Keep scratch memory bounded across a long chain. */
			MemoryContextReset(workctx);
			MemoryContextSwitchTo(workctx);
		}
	}

	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(workctx);

	if (applied > 0)
		ereport(LOG,
				(errmsg("per-backend UNDO recovery: rolled back %d "
						"transaction(s)", applied)));
	if (deferred > 0)
		ereport(LOG,
				(errmsg("per-backend UNDO recovery: queued %d transaction(s) for "
						"rollback by the undo apply worker", deferred)));
	if (abandoned > 0)
		ereport(WARNING,
				(errmsg("per-backend UNDO recovery: %d transaction(s) could not "
						"be rolled back and could not be queued", abandoned)));

	return applied + deferred;
}

/*
 * PbuAtPrepare_Undo
 *		Persist the current transaction's per-backend UNDO chain bounds into
 *		the 2PC state file, one record per persistence level's undo log.
 *
 * Called from EndPrepare() (twophase.c), between StartPrepare() and the END
 * sentinel, where RegisterTwoPhaseRecord() is valid.  If this transaction
 * produced per-backend UNDO (its TransactionState carries a valid
 * pbuUndoStartPtr set by SetCurrentUndoLocation), record the first and the
 * latest record locations and the database so COMMIT/ROLLBACK PREPARED can
 * find the chain.  No-op otherwise.
 *
 * Mirrors the cluster-wide engine's automatic 2PC capture (gxact->undo_batch_lsn set
 * in MarkAsPreparing), but for the per-backend engine and via an explicit
 * two-phase record so the location survives a crash in the GID state file.
 */
void
PbuAtPrepare_Undo(void)
{
	/* One record per persistence level's chain; none if no per-backend UNDO. */
	for (int level = 0; level < UndoPersistenceLevels; level++)
	{
		UndoRecPtr	start_urec_ptr;
		PbuTwoPhaseUndoRecord rec;

		start_urec_ptr = (UndoRecPtr) GetCurrentTransactionPbuUndoStartAt(level);
		if (!UndoRecPtrIsValid(start_urec_ptr))
			continue;

		rec.start_urec_ptr = (uint64) start_urec_ptr;
		rec.latest_urec_ptr = GetCurrentTransactionPbuUndoLatestAt(level);
		rec.dbid = MyDatabaseId;

		RegisterTwoPhaseRecord(TWOPHASE_RM_PERBACKEND_ID, PBU_2PC_UNDO,
							   &rec, sizeof(PbuTwoPhaseUndoRecord));
	}
}

/*
 * pbu_twophase_postabort
 *		ROLLBACK PREPARED resolution for a per-backend UNDO chain head.
 *
 * Reverse-apply the prepared transaction's per-backend undo.  Reached from the
 * finishing backend's own live transaction (ROLLBACK PREPARED), including
 * after a crash+restart where the 2PC state file was reloaded, so the apply
 * runs with syscache available (path-based FILEOPS undo, and any future
 * relation-based undo, both work here).
 *
 * fxid is the prepared transaction's full xid, supplied by the 2PC dispatch.
 * execute_undo_actions() is idempotent (checks uur_progress), so a retry after
 * a crash mid-rollback is safe.
 */
void
pbu_twophase_postabort(FullTransactionId fxid, uint16 info,
					   void *recdata, uint32 len)
{
	PbuTwoPhaseUndoRecord *rec = (PbuTwoPhaseUndoRecord *) recdata;
	UndoRecPtr	start_urec_ptr;
	UndoRecPtr	latest_urec_ptr;

	Assert(info == PBU_2PC_UNDO);
	Assert(len == sizeof(PbuTwoPhaseUndoRecord));

	start_urec_ptr = (UndoRecPtr) rec->start_urec_ptr;
	if (!UndoRecPtrIsValid(start_urec_ptr))
		return;

	latest_urec_ptr = (UndoRecPtr) rec->latest_urec_ptr;
	if (!UndoRecPtrIsValid(latest_urec_ptr))
		latest_urec_ptr = start_urec_ptr;

	/*
	 * execute_undo_actions() walks the chain backward from 'from' and stops
	 * after 'to', so 'from' must be the latest record and 'to' the first.
	 * nopartial = true: this is the complete transaction.
	 */
	execute_undo_actions(fxid, latest_urec_ptr, start_urec_ptr, true);
}

/*
 * pbu_twophase_postcommit
 *		COMMIT PREPARED resolution for a per-backend UNDO chain head.
 *
 * The transaction committed, so its per-backend undo is not applied; the undo
 * pages age out and are discarded by the normal discard machinery.  Nothing to
 * do here.
 */
void
pbu_twophase_postcommit(FullTransactionId fxid, uint16 info,
						void *recdata, uint32 len)
{
	(void) fxid;
	(void) info;
	(void) recdata;
	(void) len;
}
