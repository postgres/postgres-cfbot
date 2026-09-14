/*-------------------------------------------------------------------------
 *
 * flux_undo.c
 *	  FLUX UNDO resource manager
 *
 * FLUX writes one UNDO record per tuple UPDATE and DELETE via the
 * shared UNDO-in-WAL infrastructure.  INSERT emits no UNDO record --
 * an inserted tuple has no before-image and its rollback is simply its
 * inserter (t_xmin) never committing.
 * Records carry rmid UNDO_RMID_FLUX and an info subtype
 * (FLUX_UNDO_UPDATE / DELETE / UPDATE_DELTA); rollback is
 * driven by undoapply.c which dispatches to flux_undo_apply() based on
 * rmid.
 *
 * Visibility of aborted rows does not depend on the undo having been
 * applied: a reader treats a writer that did not commit as invisible
 * (CLOG, FluxWriterVisibleToSnapshot) and walks past its versions in the
 * UNDO chain, and a writer that runs into such a version reverts it first
 * (FluxRevertAbortedVersions).  Applying the undo restores the before-images
 * on the page.  The apply is idempotent: it only restores a record that is
 * still the tuple's current version (t_verptr == the record's pointer).
 *
 * Crash safety is provided by emitting an xl_undo_apply CLR record
 * (XLOG_UNDO_APPLY_RECORD / RM_UNDO_ID) for every page modification.
 * The CLR carries the new tuple image (or the LP-state change) and is
 * replayed idempotently by the generic undo_xlog.c redo handler;
 * FLUX does not need its own redo routine for the undo-apply path.
 *
 * The callback mirrors heapam_undo.c's control flow:
 *
 *   1. Defer while in crash recovery or inside a transaction's abort
 *      path (BumpContext makes relation_close/pfree unsafe); the
 *      logical-revert worker re-drives the record from a clean top-
 *      level memory context.
 *   2. try_relation_open() the target.  If the relation was dropped or
 *      truncated past the target block, there is nothing left to undo
 *      (UNDO_APPLY_SUCCESS) -- unless the aborting transaction itself is
 *      still running (inline abort, ROLLBACK TO SAVEPOINT): its own DROP or
 *      TRUNCATE may still be rolled back, so return UNDO_APPLY_SKIPPED and
 *      leave the record to be applied after that (see flux_undo_gone).
 *   3. Dispatch on info to a page-modification branch, emit a CLR,
 *      mark the buffer dirty, release locks, close.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_undo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/flux.h"
#include "access/flux_pbu.h"
#include "access/flux_undo.h"
#include "access/relation.h"
#include "access/slog.h"
#include "access/table.h"
#include "access/undo_xlog.h"
#include "access/undormgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/injection_point.h"
#include "utils/rel.h"
#include "utils/relcache.h"


static UndoApplyResult flux_undo_apply(uint8 rmid, uint16 info,
									   TransactionId xid, Oid reloid,
									   const char *payload, Size payload_len,
									   UndoRecPtr urec_ptr);
static void flux_undo_desc(StringInfo buf, uint8 rmid, uint16 info,
						   const char *payload, Size payload_len);


/* The FLUX UNDO RM registration entry */
static const UndoRmgrData flux_undo_rmgr = {
	.rm_name = "flux",
	.rm_undo = flux_undo_apply,
	.rm_desc = flux_undo_desc,
};


/*
 * FluxUndoRmgrInit
 *		Register the FLUX UNDO resource manager.
 *
 * Called from InitializeUndoSubsystem() at postmaster startup, alongside
 * HeapUndoRmgrInit() and NbtreeUndoRmgrInit().
 */
void
FluxUndoRmgrInit(void)
{
	RegisterUndoRmgr(UNDO_RMID_FLUX, &flux_undo_rmgr);
}


/*
 * emit_flux_undo_clr
 *		Emit an XLOG_UNDO_APPLY_RECORD CLR for the page modification
 *		just performed.  Must be called inside the critical section,
 *		before END_CRIT_SECTION / UnlockReleaseBuffer.
 *
 * tuple_data is the image to replay into the target slot on redo
 * (NULL for LP_UNUSED cases).  tuple_len must match the on-page slot
 * length that should be installed.
 */
static void
emit_flux_undo_clr(Relation rel, Buffer buffer, UndoRecPtr urec_ptr,
				   TransactionId xid, BlockNumber blkno, OffsetNumber offnum,
				   uint16 info, uint16 clr_flags,
				   const char *tuple_data, uint32 tuple_len)
{
	xl_undo_apply xlrec;
	XLogRecPtr	lsn;

	if (!RelationNeedsWAL(rel))
	{
		/*
		 * Unlogged / temp relations need no CLR: they do not survive a crash,
		 * so replay idempotency is irrelevant.
		 */
		PageSetLSN(BufferGetPage(buffer), GetXLogInsertRecPtr());
		return;
	}

	/*
	 * During crash recovery, UNDO is reverse-applied by PerformUndoRecovery()
	 * BEFORE WAL insertion is enabled (LocalSetXLogInsertAllowed runs later
	 * in StartupXLOG).  Emitting a CLR here would call XLogInsert ->
	 * GetWALInsertionTimeLine and trip its RECOVERY_STATE_DONE assertion (and
	 * is unnecessary): the physical restore is made durable by the
	 * end-of-recovery checkpoint, and re-application is prevented by the page
	 * LSN just as in the WAL-logged path.  So in recovery, set the page LSN
	 * to the current replay position and skip the CLR record.
	 */
	if (!XLogInsertAllowed())
	{
		TimeLineID	replayTLI;

		PageSetLSN(BufferGetPage(buffer), GetXLogReplayRecPtr(&replayTLI));
		return;
	}

	xlrec.urec_ptr = urec_ptr;
	xlrec.xid = xid;
	xlrec.target_locator = rel->rd_locator;
	xlrec.target_block = blkno;
	xlrec.target_offset = offnum;
	xlrec.operation_type = info;
	xlrec.clr_flags = clr_flags;
	xlrec.tuple_len = tuple_len;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfUndoApply);
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	if ((clr_flags & UNDO_CLR_HAS_TUPLE) && tuple_data != NULL && tuple_len > 0)
		XLogRegisterBufData(0, tuple_data, tuple_len);

	lsn = XLogInsert(RM_UNDO_ID, XLOG_UNDO_APPLY_RECORD);
	PageSetLSN(BufferGetPage(buffer), lsn);
}


/*
 * apply_flux_undo_restore_tuple
 *		Shared helper for UPDATE, DELETE and DELTA_UPDATE undo: overwrite
 *		the current on-disk tuple with an in-memory before-image.  The
 *		caller is responsible for preparing the before-image (direct
 *		copy for DELETE/UPDATE, reverse-diff reconstruction for
 *		DELTA_UPDATE).
 *
 * For DELETE undo the before-image already carries the pre-delete
 * header, so the FLUX_TUPLE_DELETED bit will be cleared as a
 * side-effect of the overwrite.
 *
 * The before-image always fits: an in-place UPDATE never shrinks the slot
 * (a smaller new image is widened to the slot length, a larger one grows
 * it), so a record's before-image is never longer than the slot of the
 * version it produced, and only that current version is ever restored (see
 * flux_undo_record_is_current).  If it does not fit anyway, the restore is
 * refused and reported as a failure so the rollback request is kept, rather
 * than silently left half done.
 */
static bool
apply_flux_undo_restore_tuple(Relation rel, Buffer buffer, OffsetNumber offnum,
							  BlockNumber blkno, UndoRecPtr urec_ptr,
							  TransactionId xid, uint16 info,
							  const char *old_image, uint32 old_len)
{
	Page		page = BufferGetPage(buffer);
	ItemId		lp;
	char	   *slot;

	Assert(old_image != NULL && old_len > 0);

	lp = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(lp))
	{
		ereport(DEBUG2,
				(errmsg_internal("FLUX UNDO: item (%u, %u) no longer normal, skipping",
								 blkno, offnum)));
		return false;
	}

	if (ItemIdGetLength(lp) < old_len ||
		IS_INJECTION_POINT_ATTACHED("flux-undo-restore-no-room"))
	{
		ereport(WARNING,
				(errmsg_internal("FLUX UNDO: current slot at (%u, %u) is smaller "
								 "than before-image (%u < %u); cannot restore",
								 blkno, offnum,
								 (unsigned) ItemIdGetLength(lp),
								 (unsigned) old_len)));
		return false;
	}

	slot = (char *) PageGetItem(page, lp);

	START_CRIT_SECTION();

	memcpy(slot, old_image, old_len);
	if (ItemIdGetLength(lp) != old_len)
		ItemIdSetNormal(lp, ItemIdGetOffset(lp), old_len);

	MarkBufferDirty(buffer);

	emit_flux_undo_clr(rel, buffer, urec_ptr, xid, blkno, offnum,
					   info, UNDO_CLR_HAS_TUPLE,
					   old_image, old_len);

	END_CRIT_SECTION();
	return true;
}


/*
 * flux_undo_gone
 *		Result for a record whose relation or block no longer exists.
 *
 * Once the aborting transaction xid is over, its relation is dropped or
 * truncated for good, by another transaction that committed: nothing is left
 * to undo.  While xid is still running -- its own inline abort, or ROLLBACK
 * TO SAVEPOINT -- the DROP or TRUNCATE may be its own, about to be rolled
 * back with it, so the record must be tried again afterwards.
 */
static UndoApplyResult
flux_undo_gone(TransactionId xid)
{
	return TransactionIdEquals(xid, GetTopTransactionIdIfAny()) ?
		UNDO_APPLY_SKIPPED : UNDO_APPLY_SUCCESS;
}


/*
 * flux_undo_record_is_current
 *		Does the tuple still carry the version this UNDO record produced?
 *
 * Every UPDATE and DELETE stamps its record's pointer into the tuple's
 * t_verptr, and the before-image it restores carries the previous pointer.
 * So the record is current iff t_verptr still equals urec_ptr, as in
 * OrioleDB (tuphdr.undoLocation == item).
 *
 * A record that is not current was already undone (ROLLBACK TO SAVEPOINT,
 * the inline abort, a worker, or a writer that reverted it), and the tuple
 * may since hold a version another transaction committed.  Restoring the
 * before-image again would destroy that version.
 */
static bool
flux_undo_record_is_current(Page page, OffsetNumber offnum,
							UndoRecPtr urec_ptr)
{
	ItemId		lp;

	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		return false;
	lp = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(lp))
		return false;
	return ((FluxTupleHeader *) PageGetItem(page, lp))->t_verptr == urec_ptr;
}


/*
 * flux_undo_index_first
 *		Reverse the index changes of a rolled-back in-place indexed-column
 *		UPDATE: reverse the recorded insertion or revival and clear the mark on
 *		(k_old, TID).
 *
 * This runs before the tuple is restored.  The other order loses the index
 * reversal when it fails (evaluating an index expression can ERROR): the
 * record is no longer current once the tuple is restored, so no retry ever
 * gets back to it, and the (k_new, TID) entry stays live for good.  Done
 * first, a failure leaves the record current and the whole record is
 * retried; the index reversal is idempotent (btdeletemark, btundomark), so redoing it is
 * harmless.
 *
 * Index work must not run under a table content lock (AB-BA with bottom-up
 * deletion), so the buffer lock is released around it, keeping the pin.
 * Returns false, with *result set, if the tuple must not be restored now:
 * UNDO_APPLY_SKIPPED if the index reversal has to wait for the aborting
 * transaction's catalog changes to be rolled back (see
 * flux_inplace_index_undo), UNDO_APPLY_SUCCESS if, once relocked, the record
 * is no longer current: someone else (a helper, the owner, the worker)
 * undid it meanwhile.
 */
static bool
flux_undo_index_first(Relation rel, Buffer buffer, OffsetNumber offnum,
					  ItemPointer tid, UndoRecPtr urec_ptr,
					  const char *old_image, Size old_len,
					  const char *indexes, uint16 nindexes, TransactionId xid,
					  UndoApplyResult *result)
{
	Page		page = BufferGetPage(buffer);
	ItemId		lp = PageGetItemId(page, offnum);
	Size		new_len = ItemIdGetLength(lp);
	FluxTupleHeader *new_hdr = palloc(new_len);
	FluxTupleHeader *old_hdr = palloc(old_len);

	/* Aligned copies: the index code reads them as FluxTupleHeaders. */
	memcpy(new_hdr, PageGetItem(page, lp), new_len);
	memcpy(old_hdr, old_image, old_len);

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	*result = flux_inplace_index_undo(rel, tid, new_hdr, new_len, old_hdr,
									  old_len, indexes, nindexes, xid) ?
		UNDO_APPLY_SUCCESS : UNDO_APPLY_SKIPPED;
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	pfree(new_hdr);
	pfree(old_hdr);

	return *result == UNDO_APPLY_SUCCESS &&
		flux_undo_record_is_current(page, offnum, urec_ptr);
}


/*
 * flux_undo_apply_locked
 *		Reverse-apply one FLUX UNDO record to its tuple.
 *
 * The caller holds an exclusive lock on the record's buffer.  It is held
 * again on return, but for a rolled-back indexed-key UPDATE it is released
 * meanwhile (flux_undo_index_first), so the caller must re-read the page.
 *
 * Returns UNDO_APPLY_SUCCESS if the record was applied or had already been
 * undone (nothing to do), and UNDO_APPLY_ERROR if it is current but could
 * not be applied, so that the rollback request is kept instead of being
 * marked complete.  Shared by flux_undo_apply (rollback by the owner or an
 * undo worker) and FluxRevertAbortedVersions (a writer reverting an aborted
 * transaction's version it ran into).
 */
UndoApplyResult
flux_undo_apply_locked(Relation rel, Buffer buffer, uint16 info,
					   TransactionId xid, const char *payload,
					   Size payload_len, UndoRecPtr urec_ptr)
{
	FluxUndoPayloadHeader hdr;
	const char *image_bytes;
	Size		image_len;
	BlockNumber blkno;
	OffsetNumber offnum;
	char	   *delta_recon = NULL;
	const char *indexes = NULL;
	uint16		nindexes = 0;
	UndoApplyResult result = UNDO_APPLY_ERROR;

	Assert(payload_len >= SizeOfFluxUndoPayloadHeader);
	memcpy(&hdr, payload, SizeOfFluxUndoPayloadHeader);
	image_bytes = payload + SizeOfFluxUndoPayloadHeader;
	image_len = hdr.tuple_len;
	if (image_len > payload_len - SizeOfFluxUndoPayloadHeader)
		elog(ERROR, "truncated FLUX undo image");
	if (hdr.flags & FLUX_UNDO_FLAG_INDEX_LIST)
	{
		const char *area = image_bytes + image_len;

		if (hdr.pad < sizeof(uint16) ||
			SizeOfFluxUndoPayloadHeader + image_len + hdr.pad != payload_len)
			elog(ERROR, "invalid FLUX undo index list");
		memcpy(&nindexes, area, sizeof(nindexes));
		if (sizeof(uint16) + nindexes * sizeof(FluxUndoIndex) > hdr.pad)
			elog(ERROR, "truncated FLUX undo index list");
		indexes = area + sizeof(uint16);
	}
	blkno = ItemPointerGetBlockNumber(&hdr.tid);
	offnum = ItemPointerGetOffsetNumber(&hdr.tid);
	Assert(BufferGetBlockNumber(buffer) == blkno);

	if (!flux_undo_record_is_current(BufferGetPage(buffer), offnum,
									 urec_ptr))
	{
		ereport(DEBUG2,
				(errmsg_internal("FLUX UNDO: record at %llu for (%u,%u) was "
								 "already undone, skipping",
								 (unsigned long long) urec_ptr,
								 blkno, offnum)));
		return UNDO_APPLY_SUCCESS;
	}

	/*
	 * A delta record carries only the changed middle of the old image.
	 * Rebuild the full old image from the current on-page bytes first; the
	 * restore, the index reversal and the CLR all need the full image.  The
	 * record is current, so the page holds the image the delta was taken
	 * against.
	 */
	if (info == FLUX_UNDO_UPDATE_DELTA)
	{
		Page		pg = BufferGetPage(buffer);
		ItemId		lp = PageGetItemId(pg, offnum);
		char	   *delta_aligned;

		if (!(hdr.flags & FLUX_UNDO_FLAG_HAS_TUPLE) ||
			image_len < SizeOfFluxUndoDelta)
		{
			ereport(WARNING,
					(errmsg_internal("FLUX UNDO UPDATE_DELTA: truncated delta "
									 "(%zu bytes) at %llu",
									 image_len,
									 (unsigned long long) urec_ptr)));
			return UNDO_APPLY_ERROR;
		}

		/*
		 * The payload sits at an arbitrary offset in the undo page, so copy
		 * the delta to an aligned buffer before reading it as a struct.
		 */
		delta_aligned = (char *) palloc(image_len);
		memcpy(delta_aligned, image_bytes, image_len);
		delta_recon = (char *) palloc(((FluxUndoDelta *) delta_aligned)->old_len);
		if (!FluxUndoDeltaReconstruct((FluxUndoDelta *) delta_aligned, image_len,
									  (char *) PageGetItem(pg, lp),
									  ItemIdGetLength(lp), delta_recon))
		{
			ereport(WARNING,
					(errmsg_internal("FLUX UNDO UPDATE_DELTA: reverse-diff "
									 "failed at %llu; leaving tuple in place",
									 (unsigned long long) urec_ptr)));
			pfree(delta_aligned);
			pfree(delta_recon);
			return UNDO_APPLY_ERROR;
		}
		image_bytes = delta_recon;
		image_len = ((FluxUndoDelta *) delta_aligned)->old_len;
		info = FLUX_UNDO_UPDATE;
		pfree(delta_aligned);
	}

	switch (info)
	{
		case FLUX_UNDO_UPDATE:
		case FLUX_UNDO_DELETE:
			if (!(hdr.flags & FLUX_UNDO_FLAG_HAS_TUPLE) || image_len == 0)
			{
				ereport(WARNING,
						(errmsg_internal("FLUX UNDO %s: missing before-image at %llu",
										 info == FLUX_UNDO_UPDATE ? "UPDATE" : "DELETE",
										 (unsigned long long) urec_ptr)));
				break;
			}

			/*
			 * For an in-place indexed-column UPDATE the index changes must be
			 * reversed too, before the tuple: see flux_undo_index_first.
			 */
			if (info == FLUX_UNDO_UPDATE &&
				RelationSupportsDeleteMarking(rel) &&
				nindexes > 0 &&
				!flux_undo_index_first(rel, buffer, offnum, &hdr.tid,
									   urec_ptr, image_bytes, image_len,
									   indexes, nindexes, xid, &result))
				break;

			if (apply_flux_undo_restore_tuple(rel, buffer, offnum, blkno,
											  urec_ptr, xid, info,
											  image_bytes, (uint32) image_len))
				result = UNDO_APPLY_SUCCESS;
			break;

		default:
			ereport(WARNING,
					(errmsg_internal("FLUX UNDO: unknown subtype 0x%x at %llu",
									 info, (unsigned long long) urec_ptr)));
			break;
	}

	if (delta_recon != NULL)
		pfree(delta_recon);
	return result;
}


/*
 * flux_aborted_writer
 *		The xid whose aborted, not-yet-reverted change is the tuple's current
 *		version, or InvalidTransactionId.
 *
 * A DELETED tuple's current change is its delete (t_xmax); an in-place
 * UPDATED tuple's current change is the update (t_xmin).  Both stamped their
 * UNDO record into t_verptr.  The change is aborted when its xid neither
 * committed nor is still running.  An INSERT writes no UNDO (the row is
 * hidden by its aborted t_xmin alone), so a never-updated tuple is never
 * reported.
 */
static TransactionId
flux_aborted_writer(FluxTupleHeader *tup)
{
	TransactionId xid;
	uint16		hint;

	if (!UndoRecPtrIsValid(tup->t_verptr))
		return InvalidTransactionId;

	if (tup->t_flags & FLUX_TUPLE_DELETED)
	{
		xid = FluxTupleGetXmax(tup);
		hint = FLUX_TUPLE_XMAX_COMMITTED;

		/*
		 * A killed speculative insertion (flux_tuple_complete_speculative,
		 * succeeded = false) is DELETED by its own inserter and writes no
		 * UNDO: nothing to revert.  FLUX_TUPLE_SPECULATIVE says so, as it is
		 * cleared only when the insertion is confirmed.  t_xmax == t_xmin
		 * does not: a transaction that UPDATEs a row in place and then
		 * DELETEs it leaves the same pair, with two UNDO records to apply.
		 */
		if (tup->t_flags & FLUX_TUPLE_SPECULATIVE)
			return InvalidTransactionId;
	}
	else if (tup->t_flags & FLUX_TUPLE_UPDATED)
	{
		xid = FluxTupleGetXmin(tup);
		hint = FLUX_TUPLE_XMIN_COMMITTED;
	}
	else
		return InvalidTransactionId;

	if ((tup->t_flags & hint) || !TransactionIdIsNormal(xid) ||
		TransactionIdIsCurrentTransactionId(xid))
		return InvalidTransactionId;

	/*
	 * Committed is the common answer, so ask CLOG first.  A transaction can
	 * commit between the two remaining tests, hence the second DidCommit.
	 */
	if (TransactionIdDidCommit(xid) ||
		TransactionIdIsInProgress(xid) ||
		TransactionIdDidCommit(xid))
		return InvalidTransactionId;

	return xid;
}

/*
 * FluxTupleHasAbortedVersion
 *		True if the tuple's current version belongs to an aborted transaction
 *		whose UNDO has not been applied yet.
 */
bool
FluxTupleHasAbortedVersion(FluxTupleHeader *tup)
{
	return TransactionIdIsValid(flux_aborted_writer(tup));
}

/*
 * FluxRevertAbortedVersions
 *		"Helping": revert the aborted, not-yet-reverted versions at the top
 *		of a tuple's version chain before a writer acts on it.
 *
 * FLUX updates in place, so a transaction that aborted without its UNDO
 * being applied yet (a rollback handed to an undo apply worker, or a crash
 * loser) leaves its images on the page.  Readers walk past them (see
 * FluxWriterVisibleToSnapshot), but a writer works on the page version, and
 * overwriting an aborted image would leave it in the version chain of the
 * new version for good.  So the writer reverts it first, as OrioleDB does
 * (btree/modify.c), instead of failing or waiting for the worker.
 *
 * The caller holds an exclusive lock on buffer, which holds the tuple at
 * offnum.  Each step reuses the rollback apply (flux_undo_apply_locked),
 * with its CLR, and leaves t_verptr at the previous record, so the owner's
 * or the worker's later apply of the same record finds it no longer current
 * and skips it.  That apply releases the lock around the index reversal of
 * an in-place key UPDATE, so the tuple is re-read after every step.
 *
 * Returns true if anything was reverted; the caller must then re-read the
 * tuple.
 */
bool
FluxRevertAbortedVersions(Relation rel, Buffer buffer, OffsetNumber offnum)
{
	Page		page = BufferGetPage(buffer);
	bool		reverted = false;

	for (;;)
	{
		ItemId		lp;
		FluxTupleHeader *tup;
		TransactionId xid;
		TransactionId top_xid;
		uint16		subtype;
		char	   *payload;
		Size		payload_len;
		FluxUndoPayloadHeader phdr;
		UndoRecPtr	urp;
		bool		deleted;

		if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
			break;
		lp = PageGetItemId(page, offnum);
		if (!ItemIdIsNormal(lp))
			break;
		tup = (FluxTupleHeader *) PageGetItem(page, lp);

		xid = flux_aborted_writer(tup);
		if (!TransactionIdIsValid(xid))
			break;

		urp = tup->t_verptr;
		deleted = (tup->t_flags & FLUX_TUPLE_DELETED) != 0;
		if (!FluxPbuFetchForRevert(urp, &top_xid, &subtype, &payload,
								   &payload_len))
			break;

		/* The record must be the one that produced this version. */
		memcpy(&phdr, payload, SizeOfFluxUndoPayloadHeader);
		if (!TransactionIdEquals(phdr.writer_xid, xid) ||
			ItemPointerGetBlockNumber(&phdr.tid) != BufferGetBlockNumber(buffer) ||
			ItemPointerGetOffsetNumber(&phdr.tid) != offnum ||
			deleted != (subtype == FLUX_UNDO_DELETE))
		{
			elog(WARNING,
				 "FLUX: UNDO record at %llu does not match aborted version of (%u,%u)",
				 (unsigned long long) urp, BufferGetBlockNumber(buffer), offnum);
			pfree(payload);
			break;
		}

		if (flux_undo_apply_locked(rel, buffer, subtype, top_xid, payload,
								   payload_len, urp) != UNDO_APPLY_SUCCESS)
		{
			pfree(payload);
			break;
		}
		pfree(payload);

		/* Stop if the restore did not happen (e.g. the slot shrank). */
		lp = PageGetItemId(page, offnum);
		if (!ItemIdIsNormal(lp) ||
			((FluxTupleHeader *) PageGetItem(page, lp))->t_verptr == urp)
			break;

		reverted = true;
	}

	return reverted;
}


/*
 * Size of the last relation storage flux_undo_apply() checked a target block
 * against, so that rolling back many records of one relation does not probe
 * the relation's size once per record.  A relation's size is found by
 * seeking its last segment file, and when the relcache entry does not survive
 * between records (RELCACHE_FORCE_RELEASE, or simply invalidations) the smgr
 * is reopened and every segment is visited again: probing per record made a
 * large rollback cost O(records x segments).
 *
 * The cached size is only ever used to skip the probe for a block below it,
 * and only while a backend rolls back its own transaction (the record's xid
 * is this backend's top transaction), which still holds the RowExclusiveLock
 * its own changes took: relation locks are not released before the
 * transaction ends, so nothing else can
 * shrink the relation (VACUUM's truncation and TRUNCATE need
 * AccessExclusiveLock), and the transaction's own TRUNCATE or rewrite gives
 * the relation a new relfilenumber, which the cache is keyed on.  The relation
 * can grow meanwhile; a block at or past the cached size re-reads the size.
 * A deferred apply (an undo apply worker, the logical revert worker) holds no
 * lock between records and always probes.
 */
static RelFileLocator flux_undo_size_locator;
static BlockNumber flux_undo_size_nblocks = InvalidBlockNumber;
static TransactionId flux_undo_size_xid = InvalidTransactionId;

static bool
flux_undo_block_exists(Relation rel, BlockNumber blkno, TransactionId xid)
{
	/* The cache is valid for one transaction's rollback only. */
	if (!TransactionIdIsValid(xid) ||
		!TransactionIdEquals(xid, GetTopTransactionIdIfAny()))
		return blkno < RelationGetNumberOfBlocks(rel);
	if (xid != flux_undo_size_xid)
	{
		flux_undo_size_xid = xid;
		flux_undo_size_nblocks = InvalidBlockNumber;
	}
	if (flux_undo_size_nblocks == InvalidBlockNumber ||
		!RelFileLocatorEquals(flux_undo_size_locator, rel->rd_locator) ||
		blkno >= flux_undo_size_nblocks)
	{
		flux_undo_size_locator = rel->rd_locator;
		flux_undo_size_nblocks = RelationGetNumberOfBlocks(rel);
		INJECTION_POINT("flux-undo-size-probe", NULL);
	}
	return blkno < flux_undo_size_nblocks;
}

/*
 * flux_undo_apply
 *		Apply a single FLUX UNDO record.
 *
 * Dispatched by the per-backend UNDO engine (execute_undo_actions_page and
 * PbuApplySubxactUndoAll) for records tagged UNDO_RMID_FLUX.  urec_ptr is
 * the record's own location.  The apply is idempotent: a record that is no
 * longer current (see flux_undo_record_is_current) is skipped and reported as
 * done.
 */
static UndoApplyResult
flux_undo_apply(uint8 rmid, uint16 info, TransactionId xid, Oid reloid,
				const char *payload, Size payload_len, UndoRecPtr urec_ptr)
{
	FluxUndoPayloadHeader hdr;
	Relation	rel;
	Buffer		buffer;
	BlockNumber blkno;
	UndoApplyResult result;

	Assert(rmid == UNDO_RMID_FLUX);

	/*
	 * Defer during crash recovery (syscache may not be initialised) or during
	 * an aborting transaction (BumpContext makes relation_close() and pfree()
	 * unsafe).  The logical-revert worker will re-drive the record from a
	 * clean memory context.
	 */
	if (InRecovery || IsAbortedTransactionBlockState())
	{
		ereport(DEBUG2,
				(errmsg_internal("FLUX UNDO: deferring xid %u record at %llu "
								 "(in recovery or abort path)",
								 xid,
								 (unsigned long long) urec_ptr)));
		return UNDO_APPLY_SKIPPED;
	}

	/* Decode the common payload header */
	if (payload_len < SizeOfFluxUndoPayloadHeader)
	{
		ereport(WARNING,
				(errmsg_internal("FLUX UNDO: payload too short (%zu bytes) "
								 "for record at %llu",
								 payload_len,
								 (unsigned long long) urec_ptr)));
		return UNDO_APPLY_ERROR;
	}
	memcpy(&hdr, payload, SizeOfFluxUndoPayloadHeader);
	blkno = ItemPointerGetBlockNumber(&hdr.tid);

	/* Open the relation; skip if dropped */
	rel = try_relation_open(reloid, RowExclusiveLock);
	if (rel == NULL)
	{
		ereport(DEBUG2,
				(errmsg_internal("FLUX UNDO: relation %u no longer exists, "
								 "skipping record at %llu",
								 reloid,
								 (unsigned long long) urec_ptr)));
		return flux_undo_gone(xid);
	}

	/*
	 * The record was written against one physical file of the relation.  If
	 * the relation now has a different one (TRUNCATE, or a rewrite such as
	 * ALTER TABLE ... TYPE), the record does not apply here: while the
	 * aborting transaction is still running the old file may be about to
	 * become current again (its own TRUNCATE/rewrite is rolled back with it),
	 * so the record is retried after the relcache revert; once it is over the
	 * old file is gone for good.
	 */
	if (hdr.relnumber != rel->rd_locator.relNumber)
	{
		ereport(DEBUG2,
				(errmsg_internal("FLUX UNDO: record at %llu is for file %u of "
								 "relation %u, which now uses file %u",
								 (unsigned long long) urec_ptr,
								 hdr.relnumber, reloid,
								 rel->rd_locator.relNumber)));
		relation_close(rel, RowExclusiveLock);
		return flux_undo_gone(xid);
	}

	/* Skip if the target block was truncated away */
	if (!flux_undo_block_exists(rel, blkno, xid))
	{
		ereport(DEBUG2,
				(errmsg_internal("FLUX UNDO: block %u beyond end of "
								 "relation %u, skipping",
								 blkno, reloid)));
		relation_close(rel, RowExclusiveLock);
		return flux_undo_gone(xid);
	}

	buffer = ReadBuffer(rel, blkno);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	result = flux_undo_apply_locked(rel, buffer, info, xid, payload,
									payload_len, urec_ptr);

	UnlockReleaseBuffer(buffer);
	relation_close(rel, RowExclusiveLock);

	return result;
}


/*
 * flux_undo_desc
 *		Describe a FLUX UNDO record for pg_waldump / debug logging.
 */
static void
flux_undo_desc(StringInfo buf, uint8 rmid, uint16 info,
			   const char *payload, Size payload_len)
{
	const char *subtype;
	FluxUndoPayloadHeader hdr;

	switch (info)
	{
		case FLUX_UNDO_UPDATE:
			subtype = "UPDATE";
			break;
		case FLUX_UNDO_DELETE:
			subtype = "DELETE";
			break;
		case FLUX_UNDO_UPDATE_DELTA:
			subtype = "UPDATE_DELTA";
			break;
		default:
			subtype = "UNKNOWN";
			break;
	}

	if (payload_len >= SizeOfFluxUndoPayloadHeader)
	{
		memcpy(&hdr, payload, SizeOfFluxUndoPayloadHeader);
		appendStringInfo(buf,
						 "%s rel=%u tid=(%u,%u) tuple_len=%u flags=0x%x",
						 subtype,
						 hdr.relnumber,
						 ItemPointerGetBlockNumber(&hdr.tid),
						 ItemPointerGetOffsetNumber(&hdr.tid),
						 hdr.tuple_len,
						 hdr.flags);

		if (info == FLUX_UNDO_UPDATE_DELTA &&
			payload_len >= SizeOfFluxUndoPayloadHeader + SizeOfFluxUndoDelta)
		{
			FluxUndoDelta delta;

			memcpy(&delta, payload + SizeOfFluxUndoPayloadHeader,
				   SizeOfFluxUndoDelta);
			appendStringInfo(buf,
							 " delta old_len=%u prefix=%u suffix=%u middle=%u",
							 delta.old_len, delta.prefixlen, delta.suffixlen,
							 delta.old_len - delta.prefixlen - delta.suffixlen);
		}
	}
	else
	{
		appendStringInfo(buf, "%s (truncated payload, %zu bytes)",
						 subtype, payload_len);
	}
}
