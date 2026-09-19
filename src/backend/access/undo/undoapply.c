/*-------------------------------------------------------------------------
 *
 * undoapply.c
 *	  Generic UNDO record application during transaction rollback
 *
 * When a transaction aborts, this module walks the UNDO chain backward
 * from the most recent record to the first.  For each record, it
 * dispatches to the appropriate resource manager's rm_undo callback
 * based on the urec_rmid field in the record header.
 *
 * This module is AM-agnostic: it contains no AM- or subsystem-specific
 * code.  All such UNDO application logic lives in the respective resource
 * managers, each registered via RegisterUndoRmgr() (see access/undormgr.h).
 *
 * The dispatch pattern is analogous to WAL resource managers: each RM
 * registers its callbacks via RegisterUndoRmgr(), and this module
 * routes UNDO records to the correct handler.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undoapply.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/undolog.h"
#include "access/undorecord.h"
#include "access/undormgr.h"
#include "access/undo_xlog.h"
#include "miscadmin.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

/* See undormgr.h */
bool		UndoApplyInline = false;

/*
 * ApplyOneUndoRecord - Apply a single UNDO record via RM dispatch
 *
 * Dispatches to the appropriate resource manager's rm_undo callback and
 * returns its result.  A record of an unknown resource manager is an error:
 * it cannot be applied here.
 */
static UndoApplyResult
ApplyOneUndoRecord(UndoRecordHeader *header, char *payload,
				   UndoRecPtr urec_ptr)
{
	const UndoRmgrData *rmgr;
	UndoApplyResult result;

	/*
	 * Idempotency design note:
	 *
	 * UNDO records are immutable once written to WAL and carry no per-record
	 * applied marker, and nothing records how far a chain walk got.  A chain
	 * can therefore be walked again after a crash part-way through, or by the
	 * revert worker after a partial inline apply, so every rm_undo must be
	 * idempotent.  They are so semantically, not by page LSN: they read the
	 * target page with ReadBuffer (not XLogReadBufferForRedo) and act only if
	 * the page still shows the effect being undone.  FILEOPS acts only on an
	 * object that still shows the operation's effect, recorded in the
	 * payload.  nbtree and hash apply their records only inline
	 * (UndoApplyInline) and skip them on every later walk, because after the
	 * abort is visible the heap TID that identifies their entry can belong to
	 * a committed row.  A CLR (XLOG_UNDO_APPLY_RECORD) only makes the page
	 * change itself crash-safe through ordinary redo; it does not mark the
	 * UNDO record as applied.
	 */

	/*
	 * Look up the resource manager for this record.
	 */
	rmgr = GetUndoRmgr(header->urec_rmid);
	if (rmgr == NULL)
	{
		ereport(WARNING,
				(errmsg("UNDO rollback: unknown RM ID %u for record at %llu, skipping",
						header->urec_rmid,
						(unsigned long long) urec_ptr)));
		return UNDO_APPLY_ERROR;
	}

	/*
	 * Dispatch to the RM's undo-apply callback.  The callback is responsible
	 * for all AM-specific work: opening relations, locking buffers, modifying
	 * pages, generating CLRs, and releasing resources.
	 */
	result = rmgr->rm_undo(header->urec_rmid,
						   header->urec_info,
						   header->urec_xid,
						   header->urec_reloid,
						   payload,
						   header->urec_payload_len,
						   urec_ptr);

	if (result == UNDO_APPLY_SUCCESS)
		ereport(DEBUG2,
				(errmsg("UNDO rollback: applied %s record at %llu",
						rmgr->rm_name,
						(unsigned long long) urec_ptr)));
	else if (result == UNDO_APPLY_SKIPPED)
		ereport(DEBUG2,
				(errmsg("UNDO rollback: skipped %s record at %llu",
						rmgr->rm_name,
						(unsigned long long) urec_ptr)));
	else
		ereport(WARNING,
				(errmsg("UNDO rollback: error applying %s record at %llu",
						rmgr->rm_name,
						(unsigned long long) urec_ptr)));

	return result;
}

/*
 * ApplyUndoChainFromWAL - Walk and apply an UNDO chain from WAL
 *
 * Reads XLOG_UNDO_BATCH WAL records via UndoReadBatchFromWAL() and
 * iterates through the serialized records within each batch.
 *
 * The chain is walked backward via the xl_undo_batch.chain_prev LSN
 * from the most recent batch to the first.  Within each batch, records
 * are applied in reverse order (newest to oldest) as required by
 * ARIES-style rollback semantics.  This is achieved by first scanning
 * forward through the serialized records to collect their start offsets,
 * then iterating the collected offsets in reverse to apply each record.
 */
bool
ApplyUndoChainFromWAL(XLogRecPtr last_batch_lsn)
{
	return ApplyUndoChainFromWALBounded(last_batch_lsn, InvalidXLogRecPtr);
}

/*
 * ApplyUndoChainFromWALBounded - Apply an UNDO chain, stopping at a boundary
 *
 * Identical to ApplyUndoChainFromWAL() except that the chain walk halts before
 * processing any batch whose LSN is at or below stop_at_lsn.  This applies only
 * the batches strictly newer than stop_at_lsn, which is what subtransaction
 * abort needs: stop_at_lsn is the parent's saved chain head, so the parent's
 * (and earlier subtransactions') batches are left intact while the aborting
 * subtransaction's batches are reverted.
 *
 * Pass InvalidXLogRecPtr as stop_at_lsn to walk the entire chain (top-level
 * rollback / recovery semantics).
 *
 * Returns true only if the whole chain was walked and no record failed
 * (UNDO_APPLY_ERROR, or an unknown resource manager).  A record an RM skipped
 * because its target is gone (UNDO_APPLY_SKIPPED) counts as done.  On false
 * the caller must hand the chain to the logical revert worker rather than
 * treat it as applied; every rm_undo is idempotent, so re-walking the records
 * that did apply is harmless.
 */
bool
ApplyUndoChainFromWALBounded(XLogRecPtr last_batch_lsn, XLogRecPtr stop_at_lsn)
{
	UndoChainWalkResult res;

	if (!XLogRecPtrIsValid(last_batch_lsn))
		return false;

	return ApplyUndoChain(last_batch_lsn, stop_at_lsn, &res) == UNDO_CHAIN_DONE;
}

/*
 * ApplyUndoChain - Apply an UNDO chain and report how it went
 *
 * UndoWalkChain() plus a summary log line.  Callers that must tell a chain
 * they could not read (worth retrying) from records that failed to apply
 * (not worth retrying) use this instead of ApplyUndoChainFromWALBounded().
 */
UndoChainOutcome
ApplyUndoChain(XLogRecPtr last_batch_lsn, XLogRecPtr stop_at_lsn,
			   UndoChainWalkResult *res)
{
	UndoChainOutcome outcome;

	outcome = UndoWalkChain(last_batch_lsn, stop_at_lsn, res);

	if (outcome != UNDO_CHAIN_DONE)
		ereport(WARNING,
				(errmsg("UNDO rollback from WAL: %d batches, %d records applied, "
						"%d skipped, %d failed%s",
						res->batches, res->applied, res->skipped, res->failed,
						res->complete ? "" : ", chain not fully read")));
	else
		ereport(DEBUG1,
				(errmsg("UNDO rollback from WAL: %d batches, %d records applied, "
						"%d skipped",
						res->batches, res->applied, res->skipped)));

	return outcome;
}

/*
 * UndoWalkChain - The chain walk shared by every UNDO apply path
 *
 * Walks the chain backward from last_batch_lsn via chain_prev, newest batch
 * first, stopping before any batch at or below stop_at_lsn (pass
 * InvalidXLogRecPtr for the whole chain).  Within each batch the records are
 * applied newest first too: they are serialized oldest first, so a forward
 * scan collects their start offsets and the offsets are then visited in
 * reverse.  Each record goes to its resource manager via ApplyOneUndoRecord().
 *
 * Online rollback (ApplyUndoChainFromWALBounded) and the crash-recovery UNDO
 * phase (PerformUndoRecovery) both use this, so they apply in the same order.
 *
 * *res receives the per-result record counts and whether the whole chain
 * could be walked: an unreadable batch, a malformed record, or a chain_prev
 * that does not point strictly backward ends the walk with complete = false
 * (and unread_batch set; malformed tells the last two, and a batch that is not
 * a well-formed UNDO batch, from a read failure), after a WARNING.  Nothing
 * is thrown for those, so a damaged chain cannot take the server down from
 * inside an abort or from the startup process.  The return value summarizes it; see UndoChainOutcome.
 */
UndoChainOutcome
UndoWalkChain(XLogRecPtr last_batch_lsn, XLogRecPtr stop_at_lsn,
			  UndoChainWalkResult *res)
{
	XLogRecPtr	batch_lsn;

	memset(res, 0, sizeof(*res));
	res->complete = true;

	ereport(DEBUG1,
			(errmsg("applying UNDO chain from WAL starting at %X/%X "
					"(stop boundary %X/%X)",
					LSN_FORMAT_ARGS(last_batch_lsn),
					LSN_FORMAT_ARGS(stop_at_lsn))));

	batch_lsn = last_batch_lsn;

	while (XLogRecPtrIsValid(batch_lsn))
	{
		UndoBatchData *batch;
		char	   *pos;
		char	   *end;

		/*
		 * Bounded walk: stop once we reach a batch that belongs to the parent
		 * (or an earlier subtransaction).  chain_prev LSNs decrease strictly
		 * as we walk backward, so the first batch at or below the boundary
		 * marks the start of the region we must preserve.
		 */
		if (XLogRecPtrIsValid(stop_at_lsn) && batch_lsn <= stop_at_lsn)
			break;

		INJECTION_POINT("undo-apply-before-batch", NULL);

		batch = UndoReadBatchFromWAL(batch_lsn, &res->malformed);
		if (batch == NULL)
		{
			ereport(WARNING,
					(errmsg("UNDO rollback: could not read batch at %X/%X, "
							"stopping chain walk",
							LSN_FORMAT_ARGS(batch_lsn))));
			res->complete = false;
			res->unread_batch = batch_lsn;
			break;
		}

		res->batches++;
		res->oldest_batch = batch_lsn;

		/*
		 * Walk through records within this batch in reverse order.
		 *
		 * ARIES requires that UNDO records within a batch be applied
		 * newest-first (reverse of serialization order).  We first scan
		 * forward to collect pointers to each record start, then iterate the
		 * collected pointers in reverse to apply them.
		 */
		pos = batch->payload;
		end = pos + batch->payload_len;

		{
			char	  **record_starts;
			int			nrecords_in_batch = 0;
			int			max_records = 1024;
			int			i;
			MemoryContext batchctx;
			MemoryContext oldctx;

			/*
			 * record_starts must grow to hold EVERY record in the batch --
			 * silently truncating rollback is a data-integrity bug (the
			 * dropped records are never applied, so a partially-applied
			 * in-place UPDATE/DELETE is left on the page).  We therefore
			 * repalloc() to double the array whenever it fills.
			 *
			 * The caller's CurrentMemoryContext may be a BumpContext
			 * (executor abort path), which supports neither repalloc() nor
			 * pfree().  So we do all growth in a private AllocSet child
			 * context and delete it whole at the end of the batch --
			 * context-agnostic, leak-free, and unbounded.
			 */
			batchctx = AllocSetContextCreate(CurrentMemoryContext,
											 "UNDO batch record_starts",
											 ALLOCSET_DEFAULT_SIZES);
			oldctx = MemoryContextSwitchTo(batchctx);

			record_starts = (char **) palloc(max_records * sizeof(char *));

			/* First pass: collect record start pointers by scanning forward */
			while (pos < end)
			{
				UndoRecordHeader hdr;

				if ((Size) (end - pos) < SizeOfUndoRecordHeader)
				{
					ereport(WARNING,
							(errmsg("UNDO rollback: truncated record in batch at %X/%X",
									LSN_FORMAT_ARGS(batch_lsn))));
					res->complete = false;
					res->malformed = true;
					res->unread_batch = batch_lsn;
					break;
				}

				memcpy(&hdr, pos, SizeOfUndoRecordHeader);

				if (hdr.urec_len < SizeOfUndoRecordHeader ||
					(Size) (end - pos) < hdr.urec_len)
				{
					ereport(WARNING,
							(errmsg("UNDO rollback: invalid record size %u in batch at %X/%X",
									hdr.urec_len, LSN_FORMAT_ARGS(batch_lsn))));
					res->complete = false;
					res->malformed = true;
					res->unread_batch = batch_lsn;
					break;
				}

				/*
				 * Grow the array (double it) rather than truncate the
				 * rollback
				 */
				if (nrecords_in_batch >= max_records)
				{
					max_records *= 2;
					record_starts = (char **) repalloc(record_starts,
													   max_records * sizeof(char *));
				}

				record_starts[nrecords_in_batch++] = pos;
				pos += hdr.urec_len;
			}

			/*
			 * Second pass: apply records in reverse order (newest first).
			 * Even if there was a scan error, apply whatever records we
			 * successfully collected.
			 */
			for (i = nrecords_in_batch - 1; i >= 0; i--)
			{
				UndoRecordHeader header;
				char	   *payload = NULL;

				memcpy(&header, record_starts[i], SizeOfUndoRecordHeader);

				if (header.urec_payload_len > 0)
					payload = record_starts[i] + SizeOfUndoRecordHeader;

				switch (ApplyOneUndoRecord(&header, payload, InvalidUndoRecPtr))
				{
					case UNDO_APPLY_SUCCESS:
						res->applied++;
						break;
					case UNDO_APPLY_SKIPPED:
						res->skipped++;
						break;
					case UNDO_APPLY_ERROR:
						res->failed++;
						break;
				}
			}

			MemoryContextSwitchTo(oldctx);
			MemoryContextDelete(batchctx);
		}

		INJECTION_POINT("undo-apply-after-batch", NULL);

		/*
		 * Follow the chain to the previous batch.  chain_prev must point
		 * strictly backward (or be invalid, at the start of the chain);
		 * anything else is a corrupt chain that would loop forever.
		 */
		{
			XLogRecPtr	next_lsn = batch->header.chain_prev;

			UndoFreeBatchData(batch);
			if (XLogRecPtrIsValid(next_lsn) && next_lsn >= batch_lsn)
			{
				ereport(WARNING,
						(errmsg("UNDO rollback: chain_prev %X/%X of batch at %X/%X "
								"does not point backward; stopping chain walk",
								LSN_FORMAT_ARGS(next_lsn),
								LSN_FORMAT_ARGS(batch_lsn))));
				res->complete = false;
				res->malformed = true;
				res->unread_batch = next_lsn;
				break;
			}
			batch_lsn = next_lsn;
		}
	}

	if (!res->complete)
		return res->malformed ? UNDO_CHAIN_MALFORMED : UNDO_CHAIN_INCOMPLETE;
	return res->failed > 0 ? UNDO_CHAIN_RECORD_ERROR : UNDO_CHAIN_DONE;
}
