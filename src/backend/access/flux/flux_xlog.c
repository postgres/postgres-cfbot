/*-------------------------------------------------------------------------
 *
 * flux_xlog.c
 *	  FLUX WAL (Write-Ahead Logging) implementation
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_xlog.c
 *
 * NOTES
 *	  This implements WAL logging for FLUX operations, providing
 *	  UNDO/REDO functionality for crash recovery. Unlike heap,
 *	  FLUX uses in-place updates with before/after images.
 *
 *	  PANIC policy during redo
 *	  ------------------------
 *	  The per-opcode redo helpers below use elog(PANIC, ...) for any
 *	  invariant violation detected during WAL replay.  This is
 *	  deliberate: a mismatch between the WAL stream and the on-disk
 *	  state (truncated overflow payload, failure to add a tuple the
 *	  forward path just wrote, a page full the forward path just
 *	  defragmented, an unknown opcode) is not a recoverable condition.
 *	  Downgrading these sites to ERROR would promote silent divergence
 *	  between the primary and a standby, or between the on-disk
 *	  heap state and the WAL record that described it; PANIC forces
 *	  a postmaster-wide restart and, in the standby case, marks the
 *	  standby inconsistent.  Each PANIC site is therefore guarded by
 *	  logic that only fires on actually-corrupt input; fixing a PANIC
 *	  that fires in practice is a correctness bug in the forward
 *	  path, not a reason to soften the guard.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/heaptoast.h"
#include "catalog/pg_class.h"
#include "utils/datum.h"
#include "access/flux.h"
#include "access/flux_xlog.h"
#include "access/bufmask.h"
#include "catalog/catalog.h"
#include "storage/standby.h"
#include "access/flux_pbu.h"
#include "access/slog.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogrecord.h"
#include "access/xlogutils.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "miscadmin.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

/*
 * FluxXLogMaybeAppendLogicalTuple
 *		Append a heap-format image of `rtup` to the in-progress WAL record
 *		if `rel` is logically logged.  Returns true and sets
 *		FLUX_WAL_LOGICAL_TUPLE in `*flags` if the image was appended.
 *
 *	The heap image is what logical decoding consumes.  Physical REDO
 *	reads the FLUX-format tuple that precedes this region.  By writing
 *	both, we avoid the need for decode.c to call RelidByRelfilenumber /
 *	RelationIdGetRelation, which are unsafe before SetupHistoricSnapshot.
 *
 *	The image is appended at the END of the main WAL data channel with
 *	the length trailing the bytes:
 *
 *		... [heap bytes] [uint32 heap_len]
 *
 *	So the decoder can read heap_len from (end-4) and back up heap_len
 *	bytes to find the heap payload, regardless of what precedes it in
 *	the record (which may vary with compression / cross-page).
 *
 *	For UPDATE we append two back-to-back trailers (old then new); see
 *	FluxXLogUpdate.
 */
void
FluxXLogPrepareLogicalImage(Relation rel, FluxTuple rtup,
							FluxLogicalImage *img)
{
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	heaptup;

	img->data = NULL;
	img->len = 0;

	if (rel == NULL || rtup == NULL || !RelationIsLogicallyLogged(rel))
		return;

	tupdesc = RelationGetDescr(rel);
	values = (Datum *) palloc(tupdesc->natts * sizeof(Datum));
	isnull = (bool *) palloc(tupdesc->natts * sizeof(bool));

	FluxDeformTuple(rel, rtup, tupdesc, values, isnull);
	for (int i = rtup->t_data->t_natts; i < tupdesc->natts; i++)
		values[i] = getmissingattr(tupdesc, i + 1, &isnull[i]);
	heaptup = heap_form_tuple(tupdesc, values, isnull);

	/*
	 * Copy the heap tuple body into an image buffer that outlives the
	 * critical section.  XLogRegisterData() only records pointers, so the
	 * bytes must remain valid until XLogInsert() reads them.
	 */
	img->len = (uint32) heaptup->t_len;
	img->data = (char *) palloc(img->len);
	memcpy(img->data, heaptup->t_data, img->len);

	heap_freetuple(heaptup);
	pfree(values);
	pfree(isnull);
}

/*
 * Build the old replica identity, independently of the physical undo image.
 * As in heap's ExtractReplicaIdentity, log FULL unconditionally, otherwise
 * only the key, and for UPDATE only if it changed or contains external data.
 * Flatten external values here: decoding cannot fetch them from TOAST.
 * A zero-length image denotes no old identity, but still has a WAL trailer.
 */
void
FluxXLogPrepareReplicaIdentity(Relation rel, FluxTuple oldtup,
							   FluxTuple newtup, FluxLogicalImage *img)
{
	TupleDesc	desc = RelationGetDescr(rel);
	char		replident = rel->rd_rel->relreplident;
	HeapTupleData tuple;
	HeapTuple	key;
	Bitmapset  *idattrs;
	Datum		values[MaxHeapAttributeNumber];
	Datum		newvalues[MaxHeapAttributeNumber];
	bool		nulls[MaxHeapAttributeNumber];
	bool		newnulls[MaxHeapAttributeNumber];
	bool		required = (newtup == NULL);

	FluxXLogPrepareLogicalImage(rel, oldtup, img);
	if (img->data == NULL)
		return;

	if (replident == REPLICA_IDENTITY_NOTHING)
	{
		img->len = 0;
		return;
	}

	tuple.t_len = img->len;
	tuple.t_data = (HeapTupleHeader) img->data;
	if (replident == REPLICA_IDENTITY_FULL)
		key = &tuple;
	else
	{
		idattrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_IDENTITY_KEY);
		heap_deform_tuple(&tuple, desc, values, nulls);
		if (newtup != NULL)
			FluxDeformTuple(rel, newtup, desc, newvalues, newnulls);

		for (int i = 0; i < desc->natts; i++)
		{
			CompactAttribute *att = TupleDescCompactAttr(desc, i);

			if (!bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber,
							   idattrs))
			{
				nulls[i] = true;
				continue;
			}
			Assert(!nulls[i]);
			if (newtup != NULL &&
				(newnulls[i] ||
				 !datumIsEqual(values[i], newvalues[i], att->attbyval, att->attlen) ||
				 (att->attlen == -1 && VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))))
				required = true;
		}
		if (!required || bms_is_empty(idattrs))
		{
			bms_free(idattrs);
			img->len = 0;
			return;
		}
		bms_free(idattrs);
		key = heap_form_tuple(desc, values, nulls);
	}

	if (HeapTupleHasExternal(key))
	{
		HeapTuple	flat = toast_flatten_tuple(key, desc);

		if (key != &tuple)
			heap_freetuple(key);
		key = flat;
	}
	if (key != &tuple)
	{
		pfree(img->data);
		img->len = key->t_len;
		img->data = palloc(img->len);
		memcpy(img->data, key->t_data, img->len);
		heap_freetuple(key);
	}
}

void
FluxXLogReleaseLogicalImage(FluxLogicalImage *img)
{
	if (img->data != NULL)
	{
		pfree(img->data);
		img->data = NULL;
	}
	img->len = 0;
}

/*
 * FluxXLogRegisterLogicalImage
 *		Register a previously prepared heap-format image onto the in-progress
 *		WAL record.  Allocation-free: safe to call inside a critical section.
 *		Appends "[heap bytes][uint32 heap_len]" to the main data channel and
 *		sets FLUX_WAL_LOGICAL_TUPLE in *flags.  No-op when img is NULL or
 *		empty (relation not logically logged).
 */
void
FluxXLogRegisterLogicalImage(FluxLogicalImage *img, uint16 *flags)
{
	if (img == NULL || img->data == NULL)
		return;

	if (img->len > 0)
		XLogRegisterData(img->data, img->len);
	XLogRegisterData((char *) &img->len, sizeof(uint32));

	*flags |= FLUX_WAL_LOGICAL_TUPLE;
}

/* ----------------------------------------------------------------
 *					WAL Record Logging Functions
 * ----------------------------------------------------------------
 */

/*
 * Log a tuple insert operation.
 */
XLogRecPtr
FluxXLogInsert(Relation rel, Buffer buffer, OffsetNumber offnum,
			   FluxTuple tuple, uint64 commit_ts,
			   FluxOverflowBuffers *overflow_buffers,
			   FluxLogicalImage *logical_img,
			   bool force_page_image)
{
	xl_flux_insert xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);
	uint8		info = XLOG_FLUX_INSERT;
	uint8		main_buf_flags = REGBUF_STANDARD;
	int			i;
	int			flux_undo_block_id = 1; /* next free WAL block id after main */
	xl_flux_overflow_write ovf_xlrecs[MAX_OVERFLOW_BUFFERS];

	/*
	 * The multi-insert (COPY/bulk) path adds many tuples to one page but
	 * emits a single INSERT record describing only the first tuple.  Under
	 * full_page_writes the page's initial touch captures the whole page, so
	 * redo restores every tuple.  With full_page_writes off there is no such
	 * image, and the unlogged tuples vanish on crash recovery, leaving later
	 * CAS_UPDATE redo to PANIC on a missing item.  Force a full-page image so
	 * the batch is crash-safe regardless of full_page_writes.
	 */
	if (force_page_image)
		main_buf_flags |= REGBUF_FORCE_IMAGE;

	/* Fill in the insert record */
	xlrec.relid = RelationGetRelid(rel);
	xlrec.offnum = offnum;
	xlrec.flags = 0;
	xlrec.tuple_len = tuple->t_len;
	xlrec.commit_ts = commit_ts;

	/*
	 * NOTE: XLogEnsureRecordSpace() has already been called by the caller
	 * (before entering the critical section) to pre-allocate space for the
	 * main buffer plus all overflow buffers.
	 */
	XLogBeginInsert();

	/* Register buffer FIRST, before any data */
	XLogRegisterBuffer(0, buffer, main_buf_flags);

	/*
	 * Register all overflow buffers (buffers 1..N) for atomic WAL logging.
	 * This ensures the main tuple and all overflow records are restored
	 * together during crash recovery, preventing orphaned overflow pages.
	 *
	 * IMPORTANT: Due to spatial locality optimization, multiple overflow
	 * records may reside on the same page. We must register each unique
	 * buffer only once, but register data for all overflow records.
	 *
	 * For each overflow buffer, we need to include: 1. The offset where the
	 * record should be placed 2. The actual overflow record data
	 */
	if (overflow_buffers != NULL)
	{
		int			registered_block_id = 1;	/* Start after main buffer */
		Buffer		registered_buffers[MAX_OVERFLOW_BUFFERS];
		int			registered_buffer_ids[MAX_OVERFLOW_BUFFERS];
		int			num_registered = 0;

		/*
		 * NOTE: ovf_xlrecs[] is declared at function scope (not here) so that
		 * the pointers registered via XLogRegisterBufData() remain valid
		 * until XLogInsert() is called after this block ends.
		 */

		/* Ensure we don't exceed PostgreSQL's hard limit */
		if (overflow_buffers->count > XLR_MAX_BLOCK_ID)
			elog(ERROR, "too many overflow records: %d (max %d)",
				 overflow_buffers->count, XLR_MAX_BLOCK_ID);

		for (i = 0; i < overflow_buffers->count; i++)
		{
			FluxOverflowBuffer *ovb = &overflow_buffers->buffers[i];
			int			block_id = -1;
			int			j;

			/*
			 * First check if this overflow buffer is the SAME as the main
			 * buffer. This can happen when spatial locality places an
			 * overflow record on the same page as the main tuple. In this
			 * case, reuse block_id=0.
			 */
			if (ovb->buffer == buffer)
			{
				block_id = 0;
				xlrec.flags |= FLUX_WAL_HAS_OVERFLOW_BLK0;
			}
			else
			{
				/*
				 * Check if this buffer was already registered among overflow
				 * buffers (spatial locality: multiple overflow records on
				 * same page). If so, reuse its block_id instead of
				 * registering again.
				 */
				for (j = 0; j < num_registered; j++)
				{
					if (registered_buffers[j] == ovb->buffer)
					{
						block_id = registered_buffer_ids[j];
						break;
					}
				}

				/* If not registered yet, register it now */
				if (block_id < 0)
				{
					block_id = registered_block_id++;

					/*
					 * Force a full-page image for overflow buffers. See
					 * FluxXLogUpdate for the rationale.
					 */
					XLogRegisterBuffer(block_id, ovb->buffer,
									   REGBUF_STANDARD | REGBUF_FORCE_IMAGE);

					/* Track this buffer so we don't register it again */
					registered_buffers[num_registered] = ovb->buffer;
					registered_buffer_ids[num_registered] = block_id;
					num_registered++;
				}
			}

			/*
			 * Create a proper xl_flux_overflow_write header with the offset.
			 * This tells the redo handler where to place the record. Each
			 * header is stored in a dedicated array slot so the pointer
			 * passed to XLogRegisterBufData remains valid until XLogInsert.
			 */
			ovf_xlrecs[i].offnum = ovb->offset;
			ovf_xlrecs[i].flags = ovb->flags;
			ovf_xlrecs[i].data_len = ovb->record_len;
			ovf_xlrecs[i].commit_ts = commit_ts;

			/* Register the header first, then the data */
			XLogRegisterBufData(block_id, (char *) &ovf_xlrecs[i], sizeof(xl_flux_overflow_write));
			XLogRegisterBufData(block_id, ovb->record_data, ovb->record_len);
		}

		/* Undo pages (if any) fold in after the last overflow block. */
		flux_undo_block_id = registered_block_id;
	}

	/* Now register the main data */
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_insert));
	XLogRegisterData((char *) tuple->t_data, tuple->t_len);
	FluxVMRegisterClear(&xlrec.flags);

	FluxXLogRegisterLogicalImage(logical_img, &xlrec.flags);

	/*
	 * Fold the per-backend UNDO record onto THIS forward-op WAL record: the
	 * undo pages become block references and the undo record's own bytes ride
	 * as their block data, so flux_redo() re-runs the undo insert.  The undo
	 * bytes reach disk on the same record as the forward FLUX mutation, which
	 * is what makes it crash-safe.  Caller prepared+inserted the undo record
	 * before opening the crit section; see flux_pbu.c.
	 */
	if (FluxPbuUndoPending())
		FluxPbuRegisterUndoBuffers((uint8) flux_undo_block_id);

	recptr = XLogInsert(RM_FLUX_ID, info);

	/* Set LSN on main page */
	PageSetLSN(page, recptr);

	/*
	 * Set LSN on all overflow pages. Due to spatial locality, some buffers
	 * may appear multiple times in overflow_buffers. PageSetLSN is idempotent
	 * (setting the same LSN multiple times is safe), so we can just iterate
	 * through all entries without checking for duplicates.
	 */
	if (overflow_buffers != NULL)
	{
		for (i = 0; i < overflow_buffers->count; i++)
		{
			Page		ovpage = BufferGetPage(overflow_buffers->buffers[i].buffer);

			PageSetLSN(ovpage, recptr);
		}
	}

	return recptr;
}

/*
 * Log a batched multi-tuple insert (COPY/bulk load) into one page.
 *
 * Unlike FluxXLogInsert, this records EVERY tuple body in the batch rather
 * than relying on a forced full-page image, so the batch replays correctly
 * regardless of full_page_writes.  No overflow buffers are involved: tuples
 * that need overflow are routed to the single-insert path by the caller.
 *
 * WAL main-data layout:
 *   [xl_flux_multi_insert header]
 *   ntuples * [xl_flux_multi_insert_tuple header][tuple t_data body]
 *   ntuples * [logical image bytes][uint32 len]   (only when logically logged)
 */
XLogRecPtr
FluxXLogMultiInsert(Relation rel, Buffer buffer,
					OffsetNumber *offnums, FluxTuple *tuples,
					int ntuples, uint64 commit_ts,
					FluxLogicalImage *logical_imgs)
{
	xl_flux_multi_insert xlrec;

	/*
	 * Serialize the whole per-tuple region into one scratch buffer and
	 * register it as a single rdata chunk.  XLogRegisterData() is limited to
	 * XLR_NORMAL_RDATAS (20) slots, and XLogEnsureRecordSpace() cannot grow
	 * that inside a critical section, so registering two chunks per tuple
	 * would overflow the slot array once a page packs more than a handful of
	 * rows. Mirrors heap_multi_insert()'s single-scratch-buffer pattern; the
	 * on-WAL bytes are identical to per-tuple registration because redo and
	 * logical decoding read the region as one contiguous XLogRecGetData()
	 * stream.
	 *
	 * The region is bounded by BLCKSZ: every tuple consumed
	 * sizeof(ItemIdData) + MAXALIGN(t_len) on the page, so the sum of
	 * SizeOfFluxMultiInsertTuple + t_len across the batch is strictly less
	 * than one page.
	 */
	PGAlignedBlock scratch;
	char	   *scratchptr = scratch.data;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);
	uint8		info = XLOG_FLUX_MULTI_INSERT;
	int			i;

	Assert(ntuples > 0);
	Assert(ntuples <= MaxOffsetNumber);

	xlrec.relid = RelationGetRelid(rel);
	xlrec.ntuples = (uint16) ntuples;
	xlrec.flags = 0;
	xlrec.commit_ts = commit_ts;

	for (i = 0; i < ntuples; i++)
	{
		xl_flux_multi_insert_tuple *tuphdr;

		/*
		 * SHORTALIGN each per-tuple header so its uint16 fields land on an
		 * even offset, mirroring heap_multi_insert().  Readers (redo and
		 * logical decode) advance with the identical SHORTALIGN, so the
		 * on-WAL stride stays byte-for-byte in lockstep.
		 */
		scratchptr = (char *) SHORTALIGN(scratchptr);
		tuphdr = (xl_flux_multi_insert_tuple *) scratchptr;

		tuphdr->offnum = offnums[i];
		tuphdr->datalen = (uint16) tuples[i]->t_len;
		scratchptr += SizeOfFluxMultiInsertTuple;

		memcpy(scratchptr, (char *) tuples[i]->t_data, tuples[i]->t_len);
		scratchptr += tuples[i]->t_len;
	}
	Assert((scratchptr - scratch.data) < BLCKSZ);

	XLogBeginInsert();

	/*
	 * Register the page WITHOUT forcing a full-page image.  Crash safety
	 * comes from logging every tuple body above, not from an FPI.
	 */
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	XLogRegisterData((char *) &xlrec, SizeOfFluxMultiInsert);
	FluxVMRegisterClear(&xlrec.flags);
	XLogRegisterData(scratch.data, (uint32) (scratchptr - scratch.data));

	/*
	 * Append one logical-decoding image per tuple (when logically logged).
	 * The caller pre-serialized all images into one contiguous blob before
	 * entering the critical section (logical_imgs[0].data), so a single
	 * registration covers the whole region and keeps the rdata slot count
	 * constant.
	 */
	if (logical_imgs != NULL && logical_imgs[0].data != NULL)
	{
		XLogRegisterData(logical_imgs[0].data, logical_imgs[0].len);
		xlrec.flags |= FLUX_WAL_LOGICAL_TUPLE;
	}

	recptr = XLogInsert(RM_FLUX_ID, info);

	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * Log a tuple update operation (in-place with before/after images)
 */
XLogRecPtr
FluxXLogUpdate(Relation rel, Buffer buffer, OffsetNumber offnum,
			   FluxTuple old_tuple, FluxTuple new_tuple,
			   uint64 old_commit_ts, uint64 new_commit_ts,
			   FluxOverflowBuffers *overflow_buffers,
			   Buffer new_buffer,
			   FluxLogicalImage *old_img,
			   FluxLogicalImage *new_img,
			   bool no_logical)
{
	xl_flux_update xlrec;
	xl_flux_prefix_suffix ps;

	/* Registered by pointer: must stay live until XLogInsert(). */
	xl_flux_overflow_write ovf_xlrecs[MAX_OVERFLOW_BUFFERS];

	XLogRecPtr	recptr;
	uint8		info = XLOG_FLUX_UPDATE_INPLACE;
	int			i;
	int			flux_undo_block_id = 1; /* next free WAL block id after main */
	bool		is_cross_page = (BufferIsValid(new_buffer) &&
								 new_buffer != buffer);

	/* Fill in the update record */
	xlrec.offnum = offnum;
	xlrec.flags = no_logical ? FLUX_WAL_NO_LOGICAL : 0;
	xlrec.old_commit_ts = old_commit_ts;
	xlrec.new_commit_ts = new_commit_ts;
	xlrec.old_tuple_len = (uint16) old_tuple->t_len;
	xlrec.new_tuple_len = (uint16) new_tuple->t_len;
	xlrec.dst_block_id = 0;
	memset(xlrec.pad, 0, sizeof(xlrec.pad));

	if (is_cross_page)
		xlrec.flags |= FLUX_WAL_CROSS_PAGE;

	/*
	 * NOTE: XLogEnsureRecordSpace() has already been called by the caller
	 * (before entering the critical section) to pre-allocate space for the
	 * main buffer plus all overflow buffers.
	 */
	XLogBeginInsert();

	/*
	 * Register the source buffer (block 0).
	 *
	 * A same-page update rewrites the tuple in place: redo overwrites the
	 * slot with the new tuple data (or patches the prefix/suffix diff) for a
	 * same-size update, and for a GROWING update redo replays the identical
	 * PageIndexTupleOverwrite the primary performed (see
	 * flux_xlog_update_inplace_redo).  That primitive is keyed purely by
	 * (offnum, new size) and reproduces the primary page byte-for-byte under
	 * any recovery ordering, so growth no longer needs a forced full-page
	 * image.  Removing that forced FPI is the dominant WAL-amplification win
	 * under OLTP: previously every in-place update whose tuple grew even a
	 * few bytes (e.g. a numeric field widening) logged a full 8 KB page.
	 *
	 * For cross-page updates the redo handler marks the old tuple UPDATED
	 * directly (see FLUX_WAL_CROSS_PAGE handling in flux_redo) and the new
	 * tuple is restored from the destination page's image.
	 */
	{
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	}

	/*
	 * Register all overflow buffers (buffers 1..N) for atomic WAL logging.
	 * This ensures the main tuple UPDATE and all overflow records are
	 * restored together during crash recovery, preventing orphaned overflow
	 * pages.
	 *
	 * IMPORTANT: Due to spatial locality optimization, multiple overflow
	 * records may reside on the same page. We must register each unique
	 * buffer only once, but register data for all overflow records.
	 */
	{
		int			next_block_id = 1;	/* Start after main buffer (block 0) */

		if (overflow_buffers != NULL)
		{
			Buffer		registered_buffers[MAX_OVERFLOW_BUFFERS];
			int			registered_buffer_ids[MAX_OVERFLOW_BUFFERS];
			int			num_registered = 0;

			/* Ensure we don't exceed PostgreSQL's hard limit */
			if (overflow_buffers->count > XLR_MAX_BLOCK_ID)
				elog(ERROR, "too many overflow records: %d (max %d)",
					 overflow_buffers->count, XLR_MAX_BLOCK_ID);

			for (i = 0; i < overflow_buffers->count; i++)
			{
				FluxOverflowBuffer *ovb = &overflow_buffers->buffers[i];
				int			block_id = -1;
				int			j;

				/*
				 * First check if this overflow buffer is the SAME as the main
				 * buffer.  This can happen when spatial locality places an
				 * overflow record on the same page as the main tuple. In this
				 * case, reuse block_id=0.
				 */
				if (ovb->buffer == buffer)
				{
					block_id = 0;
					xlrec.flags |= FLUX_WAL_HAS_OVERFLOW_BLK0;
				}
				else
				{
					/*
					 * Check if this buffer was already registered among
					 * overflow buffers (spatial locality: multiple overflow
					 * records on same page). If so, reuse its block_id
					 * instead of registering again.
					 */
					for (j = 0; j < num_registered; j++)
					{
						if (registered_buffers[j] == ovb->buffer)
						{
							block_id = registered_buffer_ids[j];
							break;
						}
					}

					/* If not registered yet, register it now */
					if (block_id < 0)
					{
						block_id = next_block_id++;

						/*
						 * Force a full-page image for overflow buffers. The
						 * redo handler for overflow pages reconstructs items
						 * using PageAddItem, but the page layout can differ
						 * from the primary when the page already contains
						 * items from prior operations (e.g., free space
						 * fragmentation, item alignment). Using
						 * REGBUF_FORCE_IMAGE guarantees the page is restored
						 * exactly as the primary had it.
						 */
						XLogRegisterBuffer((uint8) block_id, ovb->buffer,
										   REGBUF_STANDARD | REGBUF_FORCE_IMAGE);

						/* Track this buffer so we don't register it again */
						registered_buffers[num_registered] = ovb->buffer;
						registered_buffer_ids[num_registered] = block_id;
						num_registered++;
					}
				}

				/*
				 * Header first, then the record, as the insert path logs
				 * them: update redo parses [xl_flux_overflow_write][data] and
				 * places the record at the logged offnum.
				 */
				ovf_xlrecs[i].offnum = ovb->offset;
				ovf_xlrecs[i].flags = ovb->flags;
				ovf_xlrecs[i].data_len = ovb->record_len;
				ovf_xlrecs[i].commit_ts = new_commit_ts;
				XLogRegisterBufData((uint8) block_id, (char *) &ovf_xlrecs[i],
									sizeof(xl_flux_overflow_write));
				XLogRegisterBufData((uint8) block_id, ovb->record_data,
									ovb->record_len);
			}
		}

		/*
		 * For cross-page out-of-place updates, register the destination
		 * buffer so both pages are crash-safe.  Force a full-page image so
		 * redo simply restores the page without needing replay logic.
		 */
		if (is_cross_page)
		{
			xlrec.dst_block_id = (uint8) next_block_id;
			XLogRegisterBuffer((uint8) next_block_id, new_buffer,
							   REGBUF_STANDARD | REGBUF_FORCE_IMAGE);
			next_block_id++;
		}

		/* Undo pages (if any) fold in after the last forward block. */
		flux_undo_block_id = next_block_id;
	}

	/* Now register the main data */
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_update));
	FluxVMRegisterClear(&xlrec.flags);

	/*
	 * Log only new tuple for REDO.  Old tuple data is stored exclusively in
	 * the shared UNDO log (UNDO_RMID_FLUX record written via the per-backend
	 * UNDO engine) and is not needed during WAL replay:
	 *
	 * - Same-size/shrinking updates: redo overwrites the slot in place using
	 * only the new tuple data. - Growing updates: REGBUF_FORCE_IMAGE is set
	 * above, so redo restores the page from a full-page image and never
	 * enters BLK_NEEDS_REDO.
	 *
	 * Prefix/suffix compression: For same-size in-place updates, we compute
	 * the common prefix and suffix between old and new tuple data.  If the
	 * savings exceed sizeof(xl_flux_prefix_suffix) (4 bytes), we log only the
	 * changed bytes plus a small header.  The redo handler reconstructs the
	 * full new tuple from the existing page data + diff.
	 *
	 * This is only safe for same-size updates without cross-page moves.
	 * Growing updates use REGBUF_FORCE_IMAGE and never enter BLK_NEEDS_REDO.
	 */
	if (!is_cross_page &&
		old_tuple->t_len == new_tuple->t_len &&
		new_tuple->t_len > 0)
	{
		char	   *oldp = (char *) old_tuple->t_data;
		char	   *newp = (char *) new_tuple->t_data;
		int			len = new_tuple->t_len;
		int			difflen;

		/* Compute common prefix */
		for (ps.prefixlen = 0; ps.prefixlen < len; ps.prefixlen++)
			if (oldp[ps.prefixlen] != newp[ps.prefixlen])
				break;

		/* Compute common suffix (don't overlap with prefix) */
		for (ps.suffixlen = 0;
			 ps.suffixlen < len - ps.prefixlen;
			 ps.suffixlen++)
			if (oldp[len - 1 - ps.suffixlen] != newp[len - 1 - ps.suffixlen])
				break;

		difflen = len - ps.prefixlen - ps.suffixlen;

		/*
		 * Use compression only if the savings exceed the header overhead. The
		 * header is 4 bytes (two uint16s), so we need the prefix + suffix to
		 * save more than that.
		 */
		if (ps.prefixlen + ps.suffixlen > (int) sizeof(xl_flux_prefix_suffix) &&
			difflen >= 0)
		{
			xlrec.flags |= FLUX_WAL_PREFIX_SUFFIX;
			XLogRegisterData((char *) &ps, sizeof(xl_flux_prefix_suffix));
			if (difflen > 0)
				XLogRegisterData(newp + ps.prefixlen, difflen);
		}
		else
		{
			/* Not worth compressing, log full new tuple */
			XLogRegisterData((char *) new_tuple->t_data, new_tuple->t_len);
		}
	}
	else
	{
		/* Cross-page or size-changing: log full new tuple */
		XLogRegisterData((char *) new_tuple->t_data, new_tuple->t_len);
	}

	/*
	 * Append heap-format images of old + new tuples for logical decoding.
	 * Order: old first, then new.  Flag is set uniformly on both or neither.
	 * The images were prepared by the caller before the critical section.
	 */
	{
		uint16		tmpflag = 0;

		FluxXLogRegisterLogicalImage(old_img, &tmpflag);
		FluxXLogRegisterLogicalImage(new_img, &tmpflag);
		xlrec.flags |= tmpflag;
	}

	/*
	 * Fold the per-backend UNDO record onto this forward UPDATE record (undo
	 * pages as block references carrying the undo record's bytes; redo
	 * re-runs the insert).  Undo blocks start after the main page, all
	 * overflow buffers, and the cross-page destination buffer.
	 */
	if (FluxPbuUndoPending())
		FluxPbuRegisterUndoBuffers((uint8) flux_undo_block_id);

	recptr = XLogInsert(RM_FLUX_ID, info);

	/*
	 * Every page the record changes carries its LSN, so that none reaches
	 * disk before the record does (as FluxXLogInsert does).
	 */
	PageSetLSN(BufferGetPage(buffer), recptr);
	if (is_cross_page)
		PageSetLSN(BufferGetPage(new_buffer), recptr);
	if (overflow_buffers != NULL)
	{
		for (i = 0; i < overflow_buffers->count; i++)
			PageSetLSN(BufferGetPage(overflow_buffers->buffers[i].buffer),
					   recptr);
	}

	return recptr;
}

/*
 * Log a tuple delete operation
 */
XLogRecPtr
FluxXLogDelete(Relation rel, Buffer buffer, OffsetNumber offnum,
			   FluxTuple tuple, uint64 commit_ts,
			   FluxLogicalImage *logical_img)
{
	xl_flux_delete xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);
	uint8		info = XLOG_FLUX_DELETE;

	/* Fill in the delete record */
	xlrec.offnum = offnum;
	xlrec.flags = 0;
	if (((FluxTupleHeader *) PageGetItem(page, PageGetItemId(page, offnum)))->t_flags &
		FLUX_TUPLE_MOVED_PARTITION)
		xlrec.flags |= FLUX_WAL_MOVED_PARTITION;
	xlrec.tuple_len = tuple->t_len;
	xlrec.commit_ts = commit_ts;

	XLogBeginInsert();

	/* Register buffer FIRST, before any data */
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	/*
	 * Register delete header only -- old tuple data is stored exclusively in
	 * the UNDO log.  The redo handler only needs the offset and commit_ts to
	 * set FLUX_TUPLE_DELETED on the existing tuple.
	 */
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_delete));
	FluxVMRegisterClear(&xlrec.flags);

	/*
	 * Append heap-format image of the deleted tuple for logical decoding.
	 * DELETE's REDO path doesn't need the old tuple image on-page (it just
	 * flips a flag), so this region is strictly for the decode side.  The
	 * image was prepared by the caller before the critical section.
	 */
	FluxXLogRegisterLogicalImage(logical_img, &xlrec.flags);

	/*
	 * Fold the per-backend UNDO record onto this forward DELETE record (undo
	 * pages as block references carrying the undo record's bytes; redo
	 * re-runs the insert).  DELETE registers only block 0, so undo blocks
	 * start at 1.
	 */
	if (FluxPbuUndoPending())
		FluxPbuRegisterUndoBuffers(1);

	recptr = XLogInsert(RM_FLUX_ID, info);

	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * Log page defragmentation
 */
XLogRecPtr
FluxXLogDefrag(Relation rel, Buffer buffer, FluxOffsetMapping *mappings,
			   int nmappings, uint64 commit_ts,
			   TransactionId snapshotConflictHorizon)
{
	xl_flux_defrag xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);
	uint8		info = XLOG_FLUX_DEFRAG;

	/* Fill in the defrag record */
	memset(&xlrec, 0, sizeof(xlrec));
	xlrec.ntuples = nmappings;
	xlrec.commit_ts = commit_ts;
	xlrec.snapshotConflictHorizon = snapshotConflictHorizon;

	/*
	 * Always false for a FLUX relation: FLUX refuses user_catalog_table and
	 * is never a system catalog, so RelationIsAccessibleInLogicalDecoding()
	 * cannot be true here.  The field and the catalog branch it drives in
	 * ResolveRecoveryConflictWithSnapshot() at redo are therefore unreachable
	 * for FLUX records; they are kept so the WAL format and the conflict
	 * resolution stay correct for a genuine catalog relation and so the test
	 * tracks the one place the rule could change.
	 */
	xlrec.isCatalogRel = RelationIsAccessibleInLogicalDecoding(rel);

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_defrag));
	if (nmappings > 0)
		XLogRegisterData((char *) mappings,
						 sizeof(FluxOffsetMapping) * nmappings);

	/*
	 * Force a full-page image.  The caller may have removed dead tuples
	 * (ItemIdSetUnused) before compaction, and those removals are not encoded
	 * in the DEFRAG WAL record.  Without an FPI the redo handler would call
	 * PageRepairFragmentation() on a page that still contains the dead
	 * tuples, producing a page inconsistent with the primary.
	 */
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD | REGBUF_FORCE_IMAGE);

	recptr = XLogInsert(RM_FLUX_ID, info);

	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * Log overflow record write.
 *
 * The caller must already hold an exclusive lock on the buffer and have
 * written the overflow record data to the page.  We log either a new
 * overflow record (header + data) or a link update (header only).
 *
 * buffer:      already-locked buffer containing the overflow record
 * offnum:      offset of the overflow record on the page
 * record_data: pointer to the record data to log (header, or header+data)
 * record_len:  length of data to log
 * flags:       FLUX_OVERFLOW_WAL_NEW_RECORD or FLUX_OVERFLOW_WAL_LINK_UPDATE
 * commit_ts:   commit timestamp
 */
XLogRecPtr
FluxXLogOverflowWrite(Relation rel, Buffer buffer, OffsetNumber offnum,
					  char *record_data, uint32 record_len, uint16 flags,
					  uint64 commit_ts)
{
	xl_flux_overflow_write xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);
	uint8		info = XLOG_FLUX_OVERFLOW_WRITE;

	/* Fill in the overflow write record */
	xlrec.offnum = offnum;
	xlrec.flags = flags;
	xlrec.data_len = record_len;
	xlrec.commit_ts = commit_ts;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_overflow_write));
	XLogRegisterData(record_data, record_len);
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_FLUX_ID, info);

	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * Log attribute compression
 */
XLogRecPtr
FluxXLogCompress(Relation rel, Buffer buffer, OffsetNumber offnum,
				 uint16 attr_num, uint8 comp_type,
				 uint8 comp_level, char *comp_data,
				 uint32 orig_size, uint32 comp_size, uint64 commit_ts)
{
	xl_flux_compress xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);
	uint8		info = XLOG_FLUX_COMPRESS;

	/* Fill in the compress record */
	xlrec.offnum = offnum;
	xlrec.attr_num = attr_num;
	xlrec.comp_type = comp_type;
	xlrec.comp_level = comp_level;
	xlrec.orig_size = orig_size;
	xlrec.comp_size = comp_size;
	xlrec.commit_ts = commit_ts;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_compress));
	XLogRegisterData(comp_data, comp_size);
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_FLUX_ID, info);

	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * Log page initialization
 */
XLogRecPtr
FluxXLogInitPage(Relation rel, Buffer buffer, uint32 flags, uint64 commit_ts)
{
	xl_flux_init_page xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);
	uint8		info = XLOG_FLUX_INIT_PAGE;

	/* Fill in the init page record */
	xlrec.flags = flags;
	xlrec.commit_ts = commit_ts;

	XLogBeginInsert();

	/* Register buffer FIRST, before any data */
	XLogRegisterBuffer(0, buffer, REGBUF_WILL_INIT | REGBUF_STANDARD);

	/* Now register the data */
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_init_page));

	recptr = XLogInsert(RM_FLUX_ID, info);

	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * FluxXLogCasUpdate -- WAL record for same-size CAS in-place update.
 *
 * Logs only the changed byte range within the tuple.  This is the minimal
 * WAL record for the tuple-level CAS fast path where the entire tuple does
 * not need to be logged (same size, only data bytes changed).
 *
 * The caller holds BUFFER_LOCK_SHARE_EXCLUSIVE, which makes it the tuple's
 * only writer.  We do NOT force a full-page image because:
 *   (a) the modification is confined to a single tuple's data bytes, and
 *   (b) the redo handler is idempotent (memcpy of fixed-length data at
 *       a fixed offset within the tuple).
 */
XLogRecPtr
FluxXLogCasUpdate(Relation rel, Buffer buffer, OffsetNumber offnum,
				  uint16 data_offset, uint16 data_len,
				  const char *new_data, uint64 new_commit_ts)
{
	xl_flux_cas_update xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);

	xlrec.offnum = offnum;
	xlrec.flags = RelationIsLogicallyLogged(rel) ? FLUX_WAL_LOGICAL_REL : 0;
	xlrec.data_offset = data_offset;
	xlrec.data_len = data_len;
	xlrec.new_commit_ts = new_commit_ts;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_cas_update));
	XLogRegisterData(new_data, data_len);
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_CAS_UPDATE);
	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * FluxXLogCasUpdateUndo -- FOLD variant of FluxXLogCasUpdate.
 *
 * Emits ONE combined WAL record carrying both the main-fork redo byte-diff
 * (block 0, identical to FluxXLogCasUpdate) and the per-backend UNDO pages
 * (block 1+, via FluxPbuRegisterUndoBuffers, which also attaches the undo
 * record's own bytes as their block data).  The caller has already written the
 * undo record into its pinned undo buffers (FluxPbuInsertUndo) and holds them
 * locked; this function logs the change in one XLogInsert, stamps the data page
 * LSN, and returns the record LSN so the caller can release the undo buffers
 * (FluxPbuFinishUndo).  A CAS update is same-size and touches no overflow
 * pages, so the undo blocks always start at block 1.
 */
XLogRecPtr
FluxXLogCasUpdateUndo(Relation rel, Buffer buffer, OffsetNumber offnum,
					  uint16 data_offset, uint16 data_len,
					  const char *new_data, uint64 new_commit_ts)
{
	xl_flux_cas_update_undo xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);

	/* redo half (block 0) */
	xlrec.offnum = offnum;
	xlrec.flags = RelationIsLogicallyLogged(rel) ? FLUX_WAL_LOGICAL_REL : 0;
	xlrec.data_offset = data_offset;
	xlrec.data_len = data_len;
	xlrec.new_commit_ts = new_commit_ts;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, SizeOfFluxCasUpdateUndo);
	XLogRegisterData(new_data, data_len);

	/* block 0: main-fork page, redo byte-diff */
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);

	/* block 1+: per-backend undo pages, carrying the undo record's bytes */
	FluxPbuRegisterUndoBuffers(1);

	recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_CAS_UPDATE_UNDO);

	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * FluxXLogWriteDict -- WAL-log a compression-dictionary fork page.
 *
 * The dictionary fork uses a non-standard page layout that the redo path
 * cannot rebuild from a logical delta, so we register the page as a forced
 * full-page image and the redo handler restores it verbatim.  The caller
 * must already hold the buffer locked and have dirtied it inside the same
 * critical section.
 */
XLogRecPtr
FluxXLogWriteDict(Relation rel, Buffer buffer)
{
	xl_flux_write_dict xlrec;
	XLogRecPtr	recptr;
	Page		page = BufferGetPage(buffer);

	xlrec.blkno = BufferGetBlockNumber(buffer);

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xl_flux_write_dict));
	XLogRegisterBuffer(0, buffer, REGBUF_FORCE_IMAGE);

	recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_WRITE_DICT);
	PageSetLSN(page, recptr);

	return recptr;
}

/*
 * REDO function for FLUX WAL records
 */
/* ----------------------------------------------------------------
 *		Per-opcode REDO handlers.
 *
 *	flux_redo() is the thin dispatcher; the real work for each
 *	XLOG_FLUX_* opcode lives in a dedicated static helper below.
 * ----------------------------------------------------------------
 */
static void flux_xlog_insert_redo(XLogReaderState *record);
static void flux_xlog_multi_insert_redo(XLogReaderState *record);
static void flux_xlog_update_inplace_redo(XLogReaderState *record);
static void flux_xlog_delete_redo(XLogReaderState *record);
static void flux_xlog_defrag_redo(XLogReaderState *record);
static void flux_xlog_overflow_write_redo(XLogReaderState *record);
static void flux_xlog_compress_redo(XLogReaderState *record);
static void flux_xlog_init_page_redo(XLogReaderState *record);
static void flux_xlog_vm_set_redo(XLogReaderState *record);
static void flux_xlog_vm_clear_redo(XLogReaderState *record);
static void flux_xlog_lock_redo(XLogReaderState *record);
static void flux_xlog_supersede_redo(XLogReaderState *record);

/*
 * Replay the visibility map clear of an INSERT, UPDATE or DELETE record, see
 * FluxVMClearForChange.  Done before the heap page, as heap does.
 */
static void
flux_redo_vm_clear(XLogReaderState *record)
{
	BlockNumber heapBlk;
	Buffer		vmbuf;

	if (!XLogRecHasBlockRef(record, FLUX_VM_BLOCK_ID))
		return;
	XLogRecGetBlockTag(record, 0, NULL, NULL, &heapBlk);
	if (XLogReadBufferForRedoExtended(record, FLUX_VM_BLOCK_ID,
									  RBM_ZERO_ON_ERROR, false,
									  &vmbuf) == BLK_NEEDS_REDO)
	{
		Page		vmpage = BufferGetPage(vmbuf);

		if (PageIsNew(vmpage))
			PageInit(vmpage, BLCKSZ, 0);
		(void) FluxVMClearMapBits(vmpage, heapBlk);
		PageSetLSN(vmpage, record->EndRecPtr);
		MarkBufferDirty(vmbuf);
	}
	if (BufferIsValid(vmbuf))
		UnlockReleaseBuffer(vmbuf);
}

/*
 * flux_xlog_insert_redo
 *		REDO handler for XLOG_FLUX_INSERT.
 */
static void
flux_xlog_insert_redo(XLogReaderState *record)
{
	RelFileLocator rlocator;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);

	{
		xl_flux_insert *xlrec = (xl_flux_insert *) XLogRecGetData(record);
		char	   *tuple_data = (char *) xlrec + sizeof(xl_flux_insert);
		FluxTupleHeader *tuple_hdr = (FluxTupleHeader *) tuple_data;
		XLogRedoAction action;

		flux_redo_vm_clear(record);
		action = XLogReadBufferForRedo(record, 0, &buffer);

		if (action == BLK_NEEDS_REDO)
		{
			FluxPageOpaque phdr;
			OffsetNumber inserted_offnum;
			char	   *ovf_data;
			Size		ovf_len;

			page = BufferGetPage(buffer);

			/*
			 * XLogInitBufferForRedo does standard PageInit for new pages, but
			 * doesn't set up FLUX opaque space. Initialize it here if needed.
			 */
			if (PageIsNew(page))
			{
				FluxInitPage(page, BufferGetPageSize(buffer));
			}

			/*
			 * CRITICAL: During normal operation, overflow records are
			 * inserted BEFORE the main tuple (via FluxStoreOverflowColumn
			 * called from FluxFormTuple, then FluxPageAddTuple for main).
			 * This means overflow records get lower offsets (1, 2, 3...) and
			 * the main tuple gets a higher offset (4, ...).
			 *
			 * We MUST replay in the same order. If there are overflow records
			 * on block_id=0 (same page as main tuple due to spatial
			 * locality), replay them FIRST before the main tuple.
			 */
			ovf_data = XLogRecGetBlockData(record, 0, &ovf_len);
			if (ovf_data != NULL && ovf_len > 0 &&
				(xlrec->flags & FLUX_WAL_HAS_OVERFLOW_BLK0))
			{
				char	   *ovf_ptr = ovf_data;
				Size		ovf_remaining = ovf_len;

				/*
				 * Block 0 has overflow data. Parse and replay all overflow
				 * records on this block before the main tuple. Each overflow
				 * record has format: [xl_flux_overflow_write header][actual
				 * record data]
				 */
				while (ovf_remaining > sizeof(xl_flux_overflow_write))
				{
					xl_flux_overflow_write *ovf_xlrec = (xl_flux_overflow_write *) ovf_ptr;
					char	   *actual_data = ovf_ptr + sizeof(xl_flux_overflow_write);
					Size		actual_len = ovf_xlrec->data_len;
					OffsetNumber ovf_offnum;

					if (ovf_remaining < sizeof(xl_flux_overflow_write) + actual_len)
						elog(PANIC, "FLUX INSERT redo: corrupt overflow data on block 0: "
							 "ovf_remaining=%zu, sizeof(hdr)=%zu, data_len=%u, "
							 "total_len=%zu, offnum=%u, flags=%u",
							 ovf_remaining, sizeof(xl_flux_overflow_write),
							 (unsigned) actual_len, ovf_len,
							 (unsigned) ovf_xlrec->offnum,
							 (unsigned) ovf_xlrec->flags);

					/*
					 * Use InvalidOffsetNumber to let PageAddItem choose the
					 * next available offset. This ensures sequential offsets
					 * matching the original insertion order.
					 */
					ovf_offnum = PageAddItem(page, actual_data, actual_len,
											 InvalidOffsetNumber, false, false);
					if (ovf_offnum == InvalidOffsetNumber)
						elog(PANIC, "FLUX INSERT redo: failed to add overflow record on block %u",
							 blkno);

					/* Advance to next overflow record in the block data */
					ovf_ptr += sizeof(xl_flux_overflow_write) + actual_len;
					ovf_remaining -= sizeof(xl_flux_overflow_write) + actual_len;
				}
			}

			/*
			 * A record without a tuple body of the advertised length is
			 * restored from its full-page image and cannot reach here.
			 */
			if (xlrec->tuple_len == 0 ||
				XLogRecGetDataLen(record) <
				sizeof(xl_flux_insert) + xlrec->tuple_len)
				elog(PANIC, "FLUX INSERT redo: record on block %u lacks a tuple body",
					 blkno);

			/*
			 * Now replay the main tuple at the EXACT offset the primary used
			 * (xlrec->offnum).  FluxPageAddTuple on the primary can reuse a
			 * specific freed/unused line pointer (after a prior DELETE on the
			 * page), so letting PageAddItem pick the next sequential slot on
			 * redo would place the tuple at a different offset and produce a
			 * page that differs from the primary's -- caught by
			 * wal_consistency_checking as an "inconsistent page", and a
			 * latent standby/replica divergence.  overwrite=true fills a
			 * reused dead/unused line pointer in place; for a plain append
			 * (offnum == max+1) it behaves like a normal add.  Any block-0
			 * overflow records added above already occupy their own (lower)
			 * offsets, which the primary's offnum accounts for.
			 */
			inserted_offnum = PageAddItem(page, tuple_hdr, xlrec->tuple_len,
										  xlrec->offnum, true, true);
			if (inserted_offnum == InvalidOffsetNumber)
				elog(PANIC, "FLUX INSERT redo: failed to add tuple");

			/*
			 * Fix the tuple's t_ctid to point to itself at the correct
			 * location. During normal operation, this is set in
			 * flux_tuple_insert after we know the final TID. During redo, we
			 * must fix it here.
			 *
			 * Defensive: validate the ItemId is LP_NORMAL *and* has storage
			 * before dereferencing via PageGetItem.  PageGetItem asserts
			 * ItemIdHasStorage (lp_len != 0), which ItemIdIsNormal does not
			 * imply: PageAddItem can produce an LP_NORMAL line pointer with
			 * lp_len == 0 for a zero-length payload, and after crash recovery
			 * involving the UNDO revert worker the slot could be in an
			 * unexpected state.  Skipping the t_ctid fixup for a zero-storage
			 * item is safe (there is no tuple body to point at).
			 */
			{
				ItemId		itemid = PageGetItemId(page, inserted_offnum);

				if (ItemIdIsNormal(itemid) && ItemIdHasStorage(itemid))
				{
					FluxTupleHeader *inserted_hdr =
						(FluxTupleHeader *) PageGetItem(page, itemid);

					/* blkno was already fetched at function entry */
					ItemPointerSet(&inserted_hdr->t_ctid, blkno, inserted_offnum);
				}
			}

			/*
			 * Update page header.  CRITICAL: Must replicate the exact logic
			 * from FluxPageAddTuple() so the page matches the Full Page
			 * Write.  FluxPageAddTuple sets the FLUX_PAGE_DEFRAG_NEEDED flag
			 * based on fragmentation heuristics, so we must do the same here.
			 */
			phdr = FluxPageGetOpaque(page);
			FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), xlrec->commit_ts));

			/*
			 * Mark page for defragmentation if fragmented. This matches the
			 * logic in FluxPageAddTuple() at flux_tuple.c:513-517.
			 */
			if (PageGetFreeSpace(page) >= xlrec->tuple_len * 2 &&
				PageGetMaxOffsetNumber(page) > FirstOffsetNumber + 5)
			{
				FluxPageSetFlag(phdr, FLUX_PAGE_DEFRAG_NEEDED);
			}

			if (xlrec->flags & FLUX_WAL_ALL_VISIBLE_CLEARED)
				PageClearAllVisible(page);
			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}

		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);

		/*
		 * Process overflow buffers on separate pages (buffers 1..N). Each
		 * overflow buffer contains an overflow record that was registered
		 * with XLogRegisterBufData during WAL logging.
		 *
		 * Note: Overflow records on block_id=0 were already handled above
		 * before the main tuple to preserve insertion order.
		 */
		for (int ovf_idx = 1; ovf_idx < XLR_MAX_BLOCK_ID; ovf_idx++)
		{
			Buffer		ovf_buffer;
			Page		ovf_page;
			XLogRedoAction ovf_action;

			if (!XLogRecHasBlockRef(record, ovf_idx))
				break;			/* No more overflow buffers */

			/*
			 * Skip the per-backend UNDO pages folded onto this record.  They
			 * are not FLUX overflow pages: replaying them as such would
			 * FluxInitPage over the undo page and parse undo bytes as
			 * overflow records, destroying the before-image rollback needs.
			 * FluxPbuRedoUndo() below rebuilds them from the record's undo
			 * region.
			 */
			if (FluxPbuBlockIsUndo(record, (uint8) ovf_idx))
				continue;

			ovf_action = XLogReadBufferForRedo(record, (uint8) ovf_idx, &ovf_buffer);
			if (ovf_action == BLK_NEEDS_REDO)
			{
				char	   *ovf_data;
				Size		ovf_len;

				ovf_page = BufferGetPage(ovf_buffer);

				/* Initialize as FLUX page if new */
				if (PageIsNew(ovf_page))
				{
					FluxInitPage(ovf_page, BufferGetPageSize(ovf_buffer));
				}

				/* Get the overflow record data from WAL */
				ovf_data = XLogRecGetBlockData(record, (uint8) ovf_idx, &ovf_len);
				if (ovf_data != NULL && ovf_len > 0)
				{
					char	   *ovf_ptr = ovf_data;
					Size		ovf_remaining = ovf_len;

					/*
					 * Parse and replay all overflow records on this block.
					 * Multiple overflow records may be on the same page due
					 * to spatial locality optimization.
					 */
					while (ovf_remaining > sizeof(xl_flux_overflow_write))
					{
						xl_flux_overflow_write *ovf_xlrec = (xl_flux_overflow_write *) ovf_ptr;
						char	   *actual_data = ovf_ptr + sizeof(xl_flux_overflow_write);
						Size		actual_len = ovf_xlrec->data_len;
						OffsetNumber ovf_offnum;

						if (ovf_remaining < sizeof(xl_flux_overflow_write) + actual_len)
							elog(PANIC, "FLUX INSERT redo: corrupt overflow data on block %u",
								 BufferGetBlockNumber(ovf_buffer));

						/*
						 * Use the specific offset from WAL record. Overflow
						 * pointers reference these offsets. Use the specific
						 * offset from WAL record. Overflow pointers reference
						 * these offsets.
						 */
						ovf_offnum = PageAddItem(ovf_page, actual_data, actual_len,
												 ovf_xlrec->offnum, false, false);
						if (ovf_offnum == InvalidOffsetNumber)
							elog(PANIC, "FLUX INSERT redo: failed to add overflow record on block %u",
								 BufferGetBlockNumber(ovf_buffer));

						/* Advance to next overflow record */
						ovf_ptr += sizeof(xl_flux_overflow_write) + actual_len;
						ovf_remaining -= sizeof(xl_flux_overflow_write) + actual_len;
					}
				}

				PageSetLSN(ovf_page, record->EndRecPtr);
				MarkBufferDirty(ovf_buffer);
			}
			if (BufferIsValid(ovf_buffer))
				UnlockReleaseBuffer(ovf_buffer);
		}

		/*
		 * Re-run the per-backend UNDO insert folded onto this record,
		 * rebuilding the undo page content from the undo region rather than
		 * from a forced full-page image.  After this,
		 * PbuPerformUndoRecovery() can read the undo records back to roll
		 * losers back.  No-op if this INSERT carried no undo (unlogged/temp
		 * relations).
		 */
		FluxPbuRedoUndo(record, 1);
	}
}

/*
 * flux_xlog_multi_insert_redo
 *		REDO handler for XLOG_FLUX_MULTI_INSERT.
 *
 * Replays a page-at-a-time batch insert.  The WAL main data is:
 *		[xl_flux_multi_insert header]
 *		ntuples * { [xl_flux_multi_insert_tuple header][tuple t_data body] }
 *		ntuples * { [logical image bytes][uint32 len] }  (only if logical)
 *
 * Unlike single INSERT, the batch path never spills to overflow (large
 * tuples route to the single-insert path), so there is no overflow replay.
 */
static void
flux_xlog_multi_insert_redo(XLogReaderState *record)
{
	RelFileLocator rlocator;
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	XLogRecGetBlockTag(record, 0, &rlocator, NULL, &blkno);

	{
		xl_flux_multi_insert *xlrec = (xl_flux_multi_insert *) XLogRecGetData(record);
		int			ntuples = xlrec->ntuples;
		char	   *cursor = (char *) xlrec + SizeOfFluxMultiInsert;
		XLogRedoAction action;
		int			i;

		flux_redo_vm_clear(record);
		action = XLogReadBufferForRedo(record, 0, &buffer);

		if (action == BLK_NEEDS_REDO)
		{
			FluxPageOpaque phdr;
			Size		max_body = 0;

			page = BufferGetPage(buffer);

			if (PageIsNew(page))
				FluxInitPage(page, BufferGetPageSize(buffer));

			for (i = 0; i < ntuples; i++)
			{
				xl_flux_multi_insert_tuple *thdr;
				char	   *body;
				Size		datalen;
				OffsetNumber inserted_offnum;

				cursor = (char *) SHORTALIGN(cursor);
				thdr = (xl_flux_multi_insert_tuple *) cursor;
				body = cursor + SizeOfFluxMultiInsertTuple;
				datalen = thdr->datalen;

				cursor = body + datalen;

				if (datalen == 0)
					elog(PANIC, "FLUX MULTI_INSERT redo: zero-length tuple %d on block %u",
						 i, blkno);

				/*
				 * Use InvalidOffsetNumber so PageAddItem packs sequentially,
				 * matching the original batch insertion order.
				 */
				inserted_offnum = PageAddItem(page, body, datalen,
											  InvalidOffsetNumber, false, true);
				if (inserted_offnum == InvalidOffsetNumber)
					elog(PANIC, "FLUX MULTI_INSERT redo: failed to add tuple %d on block %u",
						 i, blkno);

				if (datalen > max_body)
					max_body = datalen;

				/*
				 * Fix the tuple's t_ctid to point at itself.  Guard the
				 * PageGetItem dereference on ItemIdHasStorage as the single
				 * INSERT redo path does.
				 */
				{
					ItemId		itemid = PageGetItemId(page, inserted_offnum);

					if (ItemIdIsNormal(itemid) && ItemIdHasStorage(itemid))
					{
						FluxTupleHeader *inserted_hdr =
							(FluxTupleHeader *) PageGetItem(page, itemid);

						ItemPointerSet(&inserted_hdr->t_ctid, blkno, inserted_offnum);
					}
				}
			}

			phdr = FluxPageGetOpaque(page);
			FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), xlrec->commit_ts));

			/*
			 * Mark the page for defragmentation if fragmented, matching
			 * FluxPageAddTuple().  Use the largest tuple body in the batch as
			 * the heuristic threshold.
			 */
			if (max_body > 0 &&
				PageGetFreeSpace(page) >= max_body * 2 &&
				PageGetMaxOffsetNumber(page) > FirstOffsetNumber + 5)
			{
				FluxPageSetFlag(phdr, FLUX_PAGE_DEFRAG_NEEDED);
			}

			if (xlrec->flags & FLUX_WAL_ALL_VISIBLE_CLEARED)
				PageClearAllVisible(page);
			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}

		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

/*
 * flux_xlog_update_inplace_redo
 *		REDO handler for XLOG_FLUX_UPDATE_INPLACE.
 */
static void
flux_xlog_update_inplace_redo(XLogReaderState *record)
{
	Buffer		buffer;
	Page		page;

	{
		xl_flux_update *xlrec = (xl_flux_update *) XLogRecGetData(record);
		XLogRedoAction action;

		/*
		 * WAL record layout depends on FLUX_WAL_PREFIX_SUFFIX flag:
		 *
		 * Without prefix/suffix: [xl_flux_update][full new tuple data] With
		 * prefix/suffix:    [xl_flux_update][xl_flux_prefix_suffix][diff
		 * bytes]
		 *
		 * Old tuple data is in the UNDO log exclusively.
		 */
		char	   *after_header = (char *) xlrec + sizeof(xl_flux_update);
		bool		use_prefix_suffix = (xlrec->flags & FLUX_WAL_PREFIX_SUFFIX) != 0;
		xl_flux_prefix_suffix ps_info = {0, 0};
		char	   *diff_data = NULL;
		char	   *new_tuple_data = NULL;
		FluxTupleHeader *new_tuple_hdr = NULL;
		ItemId		itemid;
		FluxPageOpaque phdr;

		if (use_prefix_suffix)
		{
			memcpy(&ps_info, after_header, sizeof(xl_flux_prefix_suffix));
			diff_data = after_header + sizeof(xl_flux_prefix_suffix);
		}
		else
		{
			new_tuple_data = after_header;
			new_tuple_hdr = (FluxTupleHeader *) new_tuple_data;
		}

		flux_redo_vm_clear(record);
		action = XLogReadBufferForRedo(record, 0, &buffer);

		if (action == BLK_NEEDS_REDO)
		{
			char	   *blk0_ovf_data;
			Size		blk0_ovf_len;

			page = BufferGetPage(buffer);

			/*
			 * XLogInitBufferForRedo does standard PageInit for new pages, but
			 * doesn't set up FLUX opaque space. Initialize it here if needed.
			 */
			if (PageIsNew(page))
			{
				FluxInitPage(page, BufferGetPageSize(buffer));
			}

			/*
			 * Process overflow records on block 0 BEFORE the main tuple,
			 * matching the original insertion order.  During normal
			 * operation, overflow records stored on the same page as the main
			 * tuple (spatial locality) get lower offsets.  We must replay
			 * them first so the main tuple ends up at the correct offset.
			 */
			blk0_ovf_data = XLogRecGetBlockData(record, 0, &blk0_ovf_len);
			if (blk0_ovf_data != NULL && blk0_ovf_len > 0 &&
				(xlrec->flags & FLUX_WAL_HAS_OVERFLOW_BLK0))
			{
				char	   *ovf_ptr = blk0_ovf_data;
				Size		ovf_remaining = blk0_ovf_len;

				while (ovf_remaining > sizeof(xl_flux_overflow_write))
				{
					xl_flux_overflow_write *blk0_ovf_xlrec =
						(xl_flux_overflow_write *) ovf_ptr;
					char	   *actual_data = ovf_ptr + sizeof(xl_flux_overflow_write);
					Size		actual_len = blk0_ovf_xlrec->data_len;
					OffsetNumber ovf_offnum;

					if (ovf_remaining < sizeof(xl_flux_overflow_write) + actual_len)
						elog(PANIC, "FLUX UPDATE redo: corrupt overflow data on block 0");

					ovf_offnum = PageAddItem(page, actual_data, actual_len,
											 blk0_ovf_xlrec->offnum, true, false);
					if (ovf_offnum == InvalidOffsetNumber)
						elog(PANIC, "FLUX UPDATE redo: failed to add overflow record on block 0");

					ovf_ptr += sizeof(xl_flux_overflow_write) + actual_len;
					ovf_remaining -= sizeof(xl_flux_overflow_write) + actual_len;
				}
			}

			/*
			 * Apply the update.  BLK_NEEDS_REDO is only returned when the
			 * page LSN < record LSN (no FPI).
			 *
			 * For cross-page out-of-place updates, the new tuple lives on the
			 * destination page (restored from its FPI).  Here we just mark
			 * the old tuple as UPDATED so visibility checks filter it
			 * correctly.
			 *
			 * Same-page out-of-place updates always force an FPI (see
			 * FluxXLogUpdate), so they get BLK_RESTORED and never reach this
			 * code path.
			 */
			itemid = PageGetItemId(page, xlrec->offnum);
			if (xlrec->offnum <= PageGetMaxOffsetNumber(page) &&
				ItemIdIsNormal(itemid) && ItemIdHasStorage(itemid))
			{
				FluxTupleHeader *existing_tuple =
					(FluxTupleHeader *) PageGetItem(page, itemid);

				if (xlrec->flags & FLUX_WAL_CROSS_PAGE)
				{
					/*
					 * Cross-page out-of-place update: mark the old tuple as
					 * UPDATED.  The new version is on the destination page
					 * restored from its FPI.
					 */
					existing_tuple->t_flags |= FLUX_TUPLE_UPDATED;
					existing_tuple->t_flags &= ~FLUX_TUPLE_UNCOMMITTED;
					existing_tuple->t_commit_ts = xlrec->new_commit_ts;

					/*
					 * The row moved to another page, so this stub is not the
					 * live version any more.  Mark KEYS_UPDATED so a KeyShare
					 * probe can never take the key-intact skip against it.
					 * Erring towards SET only costs a wait; a clear bit here
					 * would be an FK-integrity hole (see
					 * FLUX_TUPLE_KEYS_UPDATED in flux.h).
					 */
					existing_tuple->t_flags |= FLUX_TUPLE_KEYS_UPDATED;
				}
				else
				{
					Size		existing_len = ItemIdGetLength(itemid);

					if (use_prefix_suffix)
					{
						/*
						 * Prefix/suffix compressed update: reconstruct new
						 * tuple by patching the diff bytes into the existing
						 * tuple data on the page.
						 *
						 * The emitter computes prefix/suffix/difflen against
						 * new_tuple->t_len (a same-size update only), so redo
						 * must anchor on xlrec->new_tuple_len -- NOT the
						 * on-page slot length.  A prior non-shrinking shrink
						 * update can leave the slot larger than the tuple's
						 * logical length (see flux_operations.c), so using
						 * existing_len here would over-copy past the diff
						 * bytes and corrupt the tuple's suffix.
						 */
						int			difflen = (int) xlrec->new_tuple_len -
							ps_info.prefixlen - ps_info.suffixlen;

						if (difflen < 0 ||
							ps_info.prefixlen + ps_info.suffixlen > xlrec->new_tuple_len)
							elog(PANIC, "FLUX UPDATE REDO: invalid prefix/suffix "
								 "(prefix=%u, suffix=%u, tuple_len=%u)",
								 ps_info.prefixlen, ps_info.suffixlen,
								 xlrec->new_tuple_len);

						if (difflen > 0)
							memcpy((char *) existing_tuple + ps_info.prefixlen,
								   diff_data, difflen);
					}
					else if (xlrec->new_tuple_len <= existing_len)
					{
						/*
						 * Full new tuple: overwrite in place.  Do NOT shrink
						 * the line pointer; the write path keeps the original
						 * slot length so the undo before-image can be
						 * restored into it (see flux_operations.c).  Redo
						 * must produce the identical slot length or WAL
						 * consistency checking
						 * (wal_consistency_checking=flux) will diverge.
						 */
						memcpy(existing_tuple, new_tuple_hdr, xlrec->new_tuple_len);
					}
					else
					{
						/*
						 * Growing in-place update: the new tuple is larger
						 * than the current slot.  The primary grew it with
						 * PageIndexTupleOverwrite at this same offnum; replay
						 * the identical primitive here.  It is keyed purely
						 * by (offnum, new size) -- it shifts only the tuple
						 * data physically preceding this tuple and fixes the
						 * affected line-pointer offsets -- so it reproduces
						 * the primary's page byte-for-byte regardless of
						 * where the other tuples' data sits, which is why
						 * growing updates no longer need a forced full-page
						 * image (see FluxXLogUpdate). The full new tuple is
						 * carried verbatim in the record (the size-changing
						 * branch logs it uncompressed).
						 */
						if (!PageIndexTupleOverwrite(page, xlrec->offnum,
													 (const char *) new_tuple_hdr,
													 xlrec->new_tuple_len))
							elog(PANIC, "FLUX UPDATE REDO: failed to overwrite-grow "
								 "tuple at offnum %u (new_len %u)",
								 xlrec->offnum, xlrec->new_tuple_len);
					}
				}
			}
			else
				elog(PANIC, "FLUX UPDATE redo: invalid lp at offset %u", xlrec->offnum);

			/* Update page header */
			phdr = FluxPageGetOpaque(page);
			FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), xlrec->new_commit_ts));

			if (xlrec->flags & FLUX_WAL_ALL_VISIBLE_CLEARED)
				PageClearAllVisible(page);
			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);

		/*
		 * Process overflow buffers on separate pages (buffers 1..N) for
		 * UPDATE.  Multiple overflow records may share a single block due to
		 * spatial locality, so we loop through all records within each
		 * block's data (matching INSERT redo).
		 */
		for (int ovf_idx = 1; ovf_idx < XLR_MAX_BLOCK_ID; ovf_idx++)
		{
			Buffer		ovf_buffer;
			Page		ovf_page;
			XLogRedoAction ovf_action;

			if (!XLogRecHasBlockRef(record, ovf_idx))
				break;			/* No more overflow buffers */

			/*
			 * Skip the per-backend UNDO pages folded onto this record (see
			 * the same guard in flux_xlog_insert_redo).  The cross-page
			 * destination buffer is a FLUX page and is handled by this loop
			 * as before.
			 */
			if (FluxPbuBlockIsUndo(record, (uint8) ovf_idx))
				continue;

			ovf_action = XLogReadBufferForRedo(record, (uint8) ovf_idx, &ovf_buffer);
			if (ovf_action == BLK_NEEDS_REDO)
			{
				char	   *ovf_data;
				Size		ovf_len;

				ovf_page = BufferGetPage(ovf_buffer);

				/* Initialize as FLUX page if new */
				if (PageIsNew(ovf_page))
				{
					FluxInitPage(ovf_page, BufferGetPageSize(ovf_buffer));
				}

				/* Get the overflow record data from WAL */
				ovf_data = XLogRecGetBlockData(record, (uint8) ovf_idx, &ovf_len);
				if (ovf_data != NULL && ovf_len > 0)
				{
					char	   *ovf_ptr = ovf_data;
					Size		ovf_remaining = ovf_len;

					/*
					 * Parse and replay all overflow records on this block.
					 * Multiple overflow records may share a page due to
					 * spatial locality.
					 */
					while (ovf_remaining > sizeof(xl_flux_overflow_write))
					{
						xl_flux_overflow_write *ovf_xlrec2 =
							(xl_flux_overflow_write *) ovf_ptr;
						char	   *actual_data = ovf_ptr + sizeof(xl_flux_overflow_write);
						Size		actual_len = ovf_xlrec2->data_len;
						OffsetNumber ovf_offnum;

						if (ovf_remaining < sizeof(xl_flux_overflow_write) + actual_len)
							elog(PANIC, "FLUX UPDATE redo: corrupt overflow data on block %u",
								 BufferGetBlockNumber(ovf_buffer));

						/*
						 * Place the record at the offset the primary used,
						 * with overwrite=true, exactly as insert redo does.
						 */
						ovf_offnum = PageAddItem(ovf_page, actual_data, actual_len,
												 ovf_xlrec2->offnum, true, false);
						if (ovf_offnum == InvalidOffsetNumber)
							elog(PANIC, "FLUX UPDATE redo: failed to add overflow record on block %u",
								 BufferGetBlockNumber(ovf_buffer));

						ovf_ptr += sizeof(xl_flux_overflow_write) + actual_len;
						ovf_remaining -= sizeof(xl_flux_overflow_write) + actual_len;
					}
				}

				PageSetLSN(ovf_page, record->EndRecPtr);
				MarkBufferDirty(ovf_buffer);
			}
			if (BufferIsValid(ovf_buffer))
				UnlockReleaseBuffer(ovf_buffer);
		}

		/*
		 * Re-run the per-backend UNDO insert folded onto this record (see
		 * flux_xlog_insert_redo).  This is the before-image that makes an
		 * in-place UPDATE reversible, so it must be rebuilt before the undo
		 * recovery driver walks the log.
		 */
		FluxPbuRedoUndo(record, 1);
	}
}

/*
 * flux_xlog_delete_redo
 *		REDO handler for XLOG_FLUX_DELETE.
 */
static void
flux_xlog_delete_redo(XLogReaderState *record)
{
	Buffer		buffer;
	Page		page;

	{
		xl_flux_delete *xlrec = (xl_flux_delete *) XLogRecGetData(record);
		XLogRedoAction action;
		ItemId		itemid;
		FluxPageOpaque phdr;
		UndoRecPtr	del_urp;

		/*
		 * Re-run the per-backend UNDO insert folded onto this record first,
		 * rebuilding the DELETE's before-image so it survives the crash and
		 * can be reverse-applied by per-backend recovery.  It yields the
		 * record's pointer, which the delete stamps into t_verptr.
		 */
		del_urp = FluxPbuRedoUndo(record, 1);

		/*
		 * WAL record contains only the delete header (offset + commit_ts).
		 * Old tuple data is stored exclusively in the UNDO log for
		 * transaction rollback and is not needed here.
		 */

		flux_redo_vm_clear(record);
		action = XLogReadBufferForRedo(record, 0, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			/*
			 * XLogInitBufferForRedo does standard PageInit for new pages, but
			 * doesn't set up FLUX opaque space. Initialize it here if needed.
			 */
			if (PageIsNew(page))
			{
				FluxInitPage(page, BufferGetPageSize(buffer));
			}

			/* REDO: Mark tuple as deleted */
			itemid = PageGetItemId(page, xlrec->offnum);
			if (xlrec->offnum > PageGetMaxOffsetNumber(page) ||
				!ItemIdIsNormal(itemid) || !ItemIdHasStorage(itemid))
				elog(PANIC, "FLUX DELETE redo: invalid lp at offset %u", xlrec->offnum);
			{
				FluxTupleHeader *tuple =
					(FluxTupleHeader *) PageGetItem(page, itemid);

				/*
				 * What flux_tuple_delete_internal does to the tuple, but for
				 * the UNCOMMITTED hint, which commit clears without WAL.
				 * xlrec->commit_ts carries the deleter's xid.
				 */
				tuple->t_flags |= FLUX_TUPLE_DELETED;
				tuple->t_flags &= ~FLUX_TUPLE_XMAX_COMMITTED;
				FluxTupleSetXmax(tuple, (TransactionId) xlrec->commit_ts);
				tuple->t_flags &= ~(FLUX_TUPLE_MOVED_PARTITION |
									FLUX_TUPLE_SUPERSEDED | FLUX_TUPLE_KEY_INTACT);
				if (xlrec->flags & FLUX_WAL_MOVED_PARTITION)
					tuple->t_flags |= FLUX_TUPLE_MOVED_PARTITION;
				if (UndoRecPtrIsValid(del_urp))
					tuple->t_verptr = del_urp;
				ItemPointerSet(&tuple->t_ctid,
							   BufferGetBlockNumber(buffer), xlrec->offnum);
			}

			/* Update page header */
			phdr = FluxPageGetOpaque(page);
			FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), xlrec->commit_ts));
			FluxPageSetFlag(phdr, FLUX_PAGE_DEFRAG_NEEDED);

			if (xlrec->flags & FLUX_WAL_ALL_VISIBLE_CLEARED)
				PageClearAllVisible(page);
			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

/*
 * flux_xlog_defrag_redo
 *		REDO handler for XLOG_FLUX_DEFRAG.
 */
static void
flux_xlog_defrag_redo(XLogReaderState *record)
{
	Buffer		buffer;
	Page		page;

	{
		xl_flux_defrag *xlrec = (xl_flux_defrag *) XLogRecGetData(record);
		XLogRedoAction action;

		FluxPageOpaque phdr;

		/*
		 * The tuples this record removes may still be visible to queries on a
		 * hot standby: resolve that before the page changes under them.
		 */
		if (InHotStandby &&
			TransactionIdIsValid(xlrec->snapshotConflictHorizon))
		{
			RelFileLocator locator;

			XLogRecGetBlockTag(record, 0, &locator, NULL, NULL);
			ResolveRecoveryConflictWithSnapshot(xlrec->snapshotConflictHorizon,
												xlrec->isCatalogRel,
												locator);
		}

		/*
		 * Compaction moves tuples, so like heap's prune replay take a cleanup
		 * lock: no standby backend may hold a pin on the page meanwhile.
		 */
		action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true,
											   &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			/*
			 * XLogInitBufferForRedo does standard PageInit for new pages, but
			 * doesn't set up FLUX opaque space. Initialize it here if needed.
			 */
			if (PageIsNew(page))
			{
				FluxInitPage(page, BufferGetPageSize(buffer));
			}

			/* Defragment the page */
			PageRepairFragmentation(page);

			/* Update page header */
			phdr = FluxPageGetOpaque(page);
			FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), xlrec->commit_ts));
			FluxPageClearFlag(phdr, FLUX_PAGE_DEFRAG_NEEDED);

			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

/*
 * flux_xlog_overflow_write_redo
 *		REDO handler for XLOG_FLUX_OVERFLOW_WRITE.
 */
static void
flux_xlog_overflow_write_redo(XLogReaderState *record)
{
	Buffer		buffer;
	Page		page;

	{
		xl_flux_overflow_write *xlrec =
			(xl_flux_overflow_write *) XLogRecGetData(record);
		char	   *record_data = (char *) xlrec + sizeof(xl_flux_overflow_write);
		XLogRedoAction action;

		action = XLogReadBufferForRedo(record, 0, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			/* Initialize as normal FLUX page if needed */
			if (PageIsNew(page))
			{
				FluxInitPage(page, BufferGetPageSize(buffer));
			}

			if (xlrec->flags & FLUX_OVERFLOW_WAL_LINK_UPDATE)
			{
				/*
				 * Link update: overwrite the existing overflow record header
				 * at the specified offset with updated chain pointers.
				 */
				ItemId		itemid;

				itemid = PageGetItemId(page, xlrec->offnum);
				if (xlrec->offnum > PageGetMaxOffsetNumber(page) ||
					!ItemIdIsNormal(itemid) || !ItemIdHasStorage(itemid))
					elog(PANIC, "FLUX OVERFLOW_WRITE redo: invalid lp at offset %u", xlrec->offnum);
				{
					FluxOverflowRecordHeader *existing_hdr =
						(FluxOverflowRecordHeader *) PageGetItem(page, itemid);

					memcpy(existing_hdr, record_data,
						   sizeof(FluxOverflowRecordHeader));
				}
			}
			else
			{
				/*
				 * New overflow record: the logged data is the complete record
				 * (FluxOverflowRecordHeader + chunk data). Add it to the page
				 * at the specified offset.
				 */
				OffsetNumber offnum;

				offnum = PageAddItem(page, record_data, xlrec->data_len,
									 xlrec->offnum, false, false);
				if (offnum == InvalidOffsetNumber)
					elog(ERROR, "failed to add overflow record to page during redo");
			}

			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

/*
 * flux_xlog_compress_redo
 *		REDO handler for XLOG_FLUX_COMPRESS.
 */
static void
flux_xlog_compress_redo(XLogReaderState *record)
{
	Buffer		buffer;
	Page		page;

	{
		xl_flux_compress *xlrec = (xl_flux_compress *) XLogRecGetData(record);
		XLogRedoAction action;

		ItemId		itemid;
		FluxPageOpaque phdr;

		action = XLogReadBufferForRedo(record, 0, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			page = BufferGetPage(buffer);

			/*
			 * XLogInitBufferForRedo does standard PageInit for new pages, but
			 * doesn't set up FLUX opaque space. Initialize it here if needed.
			 */
			if (PageIsNew(page))
			{
				FluxInitPage(page, BufferGetPageSize(buffer));
			}

			/* Apply compression to the tuple attribute */
			itemid = PageGetItemId(page, xlrec->offnum);
			if (xlrec->offnum > PageGetMaxOffsetNumber(page) ||
				!ItemIdIsNormal(itemid) || !ItemIdHasStorage(itemid))
				elog(PANIC, "FLUX COMPRESS redo: invalid lp at offset %u", xlrec->offnum);
			{
				FluxTupleHeader *tuple =
					(FluxTupleHeader *) PageGetItem(page, itemid);

				/* Mark tuple as compressed */
				tuple->t_flags |= FLUX_TUPLE_COMPRESSED;
				tuple->t_infomask |= FLUX_INFOMASK_COMPRESSED;
				tuple->t_commit_ts = xlrec->commit_ts;
			}

			/* Update page header */
			phdr = FluxPageGetOpaque(page);
			FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), xlrec->commit_ts));

			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

/*
 * flux_xlog_init_page_redo
 *		REDO handler for XLOG_FLUX_INIT_PAGE.
 */
static void
flux_xlog_init_page_redo(XLogReaderState *record)
{
	Buffer		buffer;
	Page		page;

	{
		xl_flux_init_page *xlrec = (xl_flux_init_page *) XLogRecGetData(record);
		XLogRedoAction action;

		action = XLogReadBufferForRedoExtended(record, 0, RBM_ZERO_AND_LOCK, false, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			FluxPageOpaque phdr;

			page = BufferGetPage(buffer);

			/* Initialize page with FLUX opaque space */
			FluxInitPage(page, BufferGetPageSize(buffer));

			/* Override commit_ts and flags from WAL record */
			phdr = FluxPageGetOpaque(page);
			phdr->pd_commit_ts_and_flags = ((uint64) (xlrec->commit_ts) & FLUX_PAGE_TS_MASK) | (uint64) (xlrec->flags);

			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

/*
 * flux_xlog_vm_set_redo
 *		REDO handler for XLOG_FLUX_VM_SET.
 */
static void
flux_xlog_vm_set_redo(XLogReaderState *record)
{
	{
		xl_flux_vm_set *xlrec = (xl_flux_vm_set *) XLogRecGetData(record);
		Buffer		vmBuf;
		Page		vmPage;
		uint32		mapByte;
		uint8		mapOffset;
		uint8	   *map;

		RelFileLocator locator;

		XLogRecGetBlockTag(record, 0, &locator, NULL, NULL);
		if (InHotStandby)
			ResolveRecoveryConflictWithSnapshot(xlrec->conflict_xid, false, locator);

		/*
		 * Block 0 is the heap buffer, registered with REGBUF_NO_CHANGE.  We
		 * don't need to redo it since the heap page is not modified by VM
		 * operations.
		 */

		/* Redo VM buffer (block 1) */
		if (XLogReadBufferForRedoExtended(record, 1, RBM_ZERO_ON_ERROR, false, &vmBuf) == BLK_NEEDS_REDO)
		{
			vmPage = BufferGetPage(vmBuf);
			if (PageIsNew(vmPage))
				PageInit(vmPage, BLCKSZ, 0);

			/* Calculate the VM byte and offset for this heap block */
			mapByte = (xlrec->heapBlk % ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData)) * 4)) / 4;
			mapOffset = (xlrec->heapBlk % ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData)) * 4)) % 4;

			map = (uint8 *) PageGetContents(vmPage);
			map[mapByte] |= (xlrec->flags << (mapOffset * 2));

			PageSetLSN(vmPage, record->EndRecPtr);
			MarkBufferDirty(vmBuf);
		}
		if (BufferIsValid(vmBuf))
			UnlockReleaseBuffer(vmBuf);
	}
}

/*
 * flux_xlog_vm_clear_redo
 *		REDO handler for XLOG_FLUX_VM_CLEAR.
 */
static void
flux_xlog_vm_clear_redo(XLogReaderState *record)
{
	{
		xl_flux_vm_clear *xlrec = (xl_flux_vm_clear *) XLogRecGetData(record);
		Buffer		vmBuf;
		Page		vmPage;
		uint32		mapByte;
		uint8		mapOffset;
		uint8	   *map;

		/*
		 * Block 0 is the heap buffer, registered with REGBUF_NO_CHANGE --
		 * skip it.
		 */

		/* Redo VM buffer (block 1) */
		if (XLogReadBufferForRedo(record, 1, &vmBuf) == BLK_NEEDS_REDO)
		{
			vmPage = BufferGetPage(vmBuf);

			/* Calculate the VM byte and offset for this heap block */
			mapByte = (xlrec->heapBlk % ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData)) * 4)) / 4;
			mapOffset = (xlrec->heapBlk % ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData)) * 4)) % 4;

			map = (uint8 *) PageGetContents(vmPage);
			map[mapByte] &= ~(xlrec->flags << (mapOffset * 2));

			PageSetLSN(vmPage, record->EndRecPtr);
			MarkBufferDirty(vmBuf);
		}
		if (BufferIsValid(vmBuf))
			UnlockReleaseBuffer(vmBuf);
	}
}

/*
 * flux_xlog_lock_redo
 *		REDO handler for XLOG_FLUX_LOCK.
 */
static void
flux_xlog_lock_redo(XLogReaderState *record)
{
	Buffer		buffer;
	Page		page;

	{
		xl_flux_lock *xlrec = (xl_flux_lock *) XLogRecGetData(record);
		XLogRedoAction action;

		action = XLogReadBufferForRedo(record, 0, &buffer);
		if (action == BLK_NEEDS_REDO)
		{
			ItemId		itemid;

			page = BufferGetPage(buffer);

			if (PageIsNew(page))
			{
				FluxInitPage(page, BufferGetPageSize(buffer));
			}

			itemid = PageGetItemId(page, xlrec->offnum);
			if (xlrec->offnum > PageGetMaxOffsetNumber(page) ||
				!ItemIdIsNormal(itemid) || !ItemIdHasStorage(itemid))
				elog(PANIC, "FLUX LOCK redo: invalid lp at offset %u", xlrec->offnum);
			{
				FluxTupleHeader *tuple =
					(FluxTupleHeader *) PageGetItem(page, itemid);

				/* Apply the lock state from the WAL record */
				tuple->t_infomask = xlrec->infomask;
				tuple->t_flags |= FLUX_TUPLE_LOCKED;
			}

			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(buffer);
		}
		if (BufferIsValid(buffer))
			UnlockReleaseBuffer(buffer);
	}
}

/*
 * flux_xlog_supersede_redo
 *		REDO handler for XLOG_FLUX_SUPERSEDE: link a moved row's old version
 *		to its new TID (see flux_link_superseded).
 */
static void
flux_xlog_supersede_redo(XLogReaderState *record)
{
	xl_flux_supersede *xlrec = (xl_flux_supersede *) XLogRecGetData(record);
	Buffer		buffer;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		ItemId		itemid = PageGetItemId(page, xlrec->offnum);

		if (xlrec->offnum > PageGetMaxOffsetNumber(page) ||
			!ItemIdIsNormal(itemid) || !ItemIdHasStorage(itemid))
			elog(PANIC, "FLUX SUPERSEDE redo: invalid lp at offset %u", xlrec->offnum);
		{
			FluxTupleHeader *tuple = (FluxTupleHeader *) PageGetItem(page, itemid);

			tuple->t_flags |= FLUX_TUPLE_SUPERSEDED;
			if (xlrec->flags & FLUX_WAL_KEY_INTACT)
				tuple->t_flags |= FLUX_TUPLE_KEY_INTACT;
			tuple->t_ctid = xlrec->new_tid;
		}
		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * flux_xlog_cas_update_redo
 *		REDO handler for XLOG_FLUX_CAS_UPDATE.
 *
 * Patches a contiguous byte range within a tuple on the page.  The record
 * carries only the changed bytes (data_offset..data_offset+data_len) and
 * the new commit timestamp.  Idempotent memcpy; safe for replay.
 */
static void
flux_xlog_cas_update_redo(XLogReaderState *record)
{
	xl_flux_cas_update *xlrec = (xl_flux_cas_update *) XLogRecGetData(record);
	char	   *new_data = ((char *) xlrec) + sizeof(xl_flux_cas_update);
	Buffer		buffer;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		ItemId		itemid;
		FluxTupleHeader *tuple;

		itemid = PageGetItemId(page, xlrec->offnum);
		if (!ItemIdIsNormal(itemid) || !ItemIdHasStorage(itemid))
			elog(PANIC, "FLUX CAS_UPDATE redo: invalid item at offset %u",
				 xlrec->offnum);

		tuple = (FluxTupleHeader *) PageGetItem(page, itemid);

		/* Patch the changed data bytes */
		memcpy(((char *) tuple) + xlrec->data_offset, new_data, xlrec->data_len);

		/* Update commit timestamp */
		tuple->t_commit_ts = xlrec->new_commit_ts;

		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * flux_xlog_cas_update_undo_redo
 *		REDO handler for XLOG_FLUX_CAS_UPDATE_UNDO (FOLD variant).
 *
 * Replays from one record:
 *   block 0  - main-fork page: patch the tuple data bytes, set commit ts
 *              (identical to flux_xlog_cas_update_redo).
 *   block 1+ - per-backend UNDO page(s): the undo insert is re-run from the
 *              record's undo region (FluxPbuRedoUndo) so the folded undo
 *              survives the crash and can be reverse-applied by per-backend
 *              recovery.
 *
 * Both are idempotent; safe for replay.
 */
static void
flux_xlog_cas_update_undo_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_flux_cas_update_undo *xlrec =
		(xl_flux_cas_update_undo *) XLogRecGetData(record);
	char	   *new_data = ((char *) xlrec) + SizeOfFluxCasUpdateUndo;
	Buffer		buffer;

	/* ---- block 0: main-fork redo byte-diff ---- */
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buffer);
		ItemId		itemid;
		FluxTupleHeader *tuple;

		itemid = PageGetItemId(page, xlrec->offnum);
		if (!ItemIdIsNormal(itemid) || !ItemIdHasStorage(itemid))
			elog(PANIC, "FLUX CAS_UPDATE_UNDO redo: invalid item at offset %u",
				 xlrec->offnum);

		tuple = (FluxTupleHeader *) PageGetItem(page, itemid);

		memcpy(((char *) tuple) + xlrec->data_offset, new_data, xlrec->data_len);
		tuple->t_commit_ts = xlrec->new_commit_ts;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	/* ---- block 1+: per-backend UNDO, re-run the undo insert ---- */
	FluxPbuRedoUndo(record, 1);
}

/*
 * flux_xlog_write_dict_redo
 *		REDO handler for XLOG_FLUX_WRITE_DICT.
 *
 * The record always carries a forced full-page image of the dictionary-fork
 * block, so XLogReadBufferForRedo restores the page directly and there is no
 * delta to replay.  We never reach BLK_NEEDS_REDO without the FPI present;
 * the handler simply restores and releases the buffer.
 */
static void
flux_xlog_write_dict_redo(XLogReaderState *record)
{
	Buffer		buffer;

	/*
	 * With REGBUF_FORCE_IMAGE the page is reconstructed from the FPI by
	 * XLogReadBufferForRedo, which returns BLK_RESTORED.  BLK_NEEDS_REDO is
	 * not expected, but if it ever occurs there is nothing to apply.
	 */
	(void) XLogReadBufferForRedo(record, 0, &buffer);
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

static void
flux_xlog_freeze_redo(XLogReaderState *record)
{
	xl_flux_freeze_page *xlrec = (xl_flux_freeze_page *) XLogRecGetData(record);
	Buffer		buffer;
	RelFileLocator locator;

	XLogRecGetBlockTag(record, 0, &locator, NULL, NULL);
	if (InHotStandby)
		ResolveRecoveryConflictWithSnapshot(xlrec->conflict_xid,
											xlrec->is_catalog_rel, locator);
	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Size		len;
		xl_flux_freeze_tuple *plans =
			(xl_flux_freeze_tuple *) XLogRecGetBlockData(record, 0, &len);

		if (len != xlrec->nplans * sizeof(*plans))
			elog(PANIC, "invalid FLUX freeze plan length");
		FluxExecuteFreeze(buffer, plans, xlrec->nplans);
		PageSetLSN(BufferGetPage(buffer), record->EndRecPtr);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * flux_redo
 *		Thin dispatcher for all XLOG_FLUX_* opcodes.  Each case is
 *	delegated to a dedicated per-opcode static helper above.
 */
void
flux_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;


	FluxPbuResolveRecoveryConflict(record);

	switch (info)
	{
		case XLOG_FLUX_FREEZE_PAGE:
			flux_xlog_freeze_redo(record);
			break;
		case XLOG_FLUX_INSERT:
			flux_xlog_insert_redo(record);
			break;

		case XLOG_FLUX_UPDATE_INPLACE:
			flux_xlog_update_inplace_redo(record);
			break;

		case XLOG_FLUX_DELETE:
			flux_xlog_delete_redo(record);
			break;

		case XLOG_FLUX_DEFRAG:
			flux_xlog_defrag_redo(record);
			break;

		case XLOG_FLUX_OVERFLOW_WRITE:
			flux_xlog_overflow_write_redo(record);
			break;

		case XLOG_FLUX_COMPRESS:
			flux_xlog_compress_redo(record);
			break;

		case XLOG_FLUX_INIT_PAGE:
			flux_xlog_init_page_redo(record);
			break;


		case XLOG_FLUX_VM_SET:
			flux_xlog_vm_set_redo(record);
			break;

		case XLOG_FLUX_VM_CLEAR:
			flux_xlog_vm_clear_redo(record);
			break;

		case XLOG_FLUX_LOCK:
			flux_xlog_lock_redo(record);
			break;

		case XLOG_FLUX_CAS_UPDATE:
			flux_xlog_cas_update_redo(record);
			break;

		case XLOG_FLUX_CAS_UPDATE_UNDO:
			flux_xlog_cas_update_undo_redo(record);
			break;

		case XLOG_FLUX_WRITE_DICT:
			flux_xlog_write_dict_redo(record);
			break;

		case XLOG_FLUX_MULTI_INSERT:
			flux_xlog_multi_insert_redo(record);
			break;

		case XLOG_FLUX_SUPERSEDE:
			flux_xlog_supersede_redo(record);
			break;

		default:
			elog(PANIC, "flux_redo: unknown op code %u", info);
	}
}


/* Execute the exact same prepared changes on primary and standby. */
void
FluxExecuteFreeze(Buffer buffer, xl_flux_freeze_tuple *plans, int nplans)
{
	Page		page = BufferGetPage(buffer);

	for (int i = 0; i < nplans; i++)
	{
		ItemId		item = PageGetItemId(page, plans[i].offset);
		FluxTupleHeader *tuple = (FluxTupleHeader *) PageGetItem(page, item);

		Assert(ItemIdIsNormal(item));
		if (plans[i].actions & FLUX_FREEZE_XMIN)
		{
			FluxTupleSetXmin(tuple, FrozenTransactionId);
			tuple->t_flags |= FLUX_TUPLE_XMIN_COMMITTED;
		}
		if (plans[i].actions & FLUX_FREEZE_XMAX)
		{
			FluxTupleSetXmax(tuple, InvalidTransactionId);
			tuple->t_flags &= ~(FLUX_TUPLE_DELETED | FLUX_TUPLE_UPDATED |
								FLUX_TUPLE_XMAX_COMMITTED |
								FLUX_TUPLE_MOVED_PARTITION |
								FLUX_TUPLE_SUPERSEDED | FLUX_TUPLE_KEY_INTACT);
		}
		if (plans[i].actions & FLUX_FREEZE_DETACH)
		{
			tuple->t_verptr = InvalidUndoRecPtr;
			tuple->t_flags &= ~(FLUX_TUPLE_HAS_VERSION_PTR | FLUX_TUPLE_UNCOMMITTED);
		}
	}
}

XLogRecPtr
FluxXLogFreeze(Relation rel, Buffer buffer, xl_flux_freeze_tuple *plans,
			   int nplans, TransactionId conflict_xid)
{
	xl_flux_freeze_page xlrec = {0};

	xlrec.conflict_xid = conflict_xid;
	xlrec.nplans = nplans;

	/*
	 * Always false for a FLUX relation: FLUX refuses user_catalog_table and
	 * is never a system catalog, so RelationIsAccessibleInLogicalDecoding()
	 * cannot be true here.  The field and the catalog branch it drives in
	 * ResolveRecoveryConflictWithSnapshot() at redo are therefore unreachable
	 * for FLUX records; they are kept so the WAL format and the conflict
	 * resolution stay correct for a genuine catalog relation and so the test
	 * tracks the one place the rule could change.
	 */
	xlrec.is_catalog_rel = RelationIsAccessibleInLogicalDecoding(rel);
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xlrec));
	XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
	XLogRegisterBufData(0, (char *) plans, nplans * sizeof(*plans));
	return XLogInsert(RM_FLUX_ID, XLOG_FLUX_FREEZE_PAGE);
}

/*
 * Mask function for FLUX pages (for consistency checking)
 */
void
flux_mask(char *page, BlockNumber blkno)
{
	Page		flux_page = (Page) page;
	FluxPageOpaque phdr;
	bool		is_overflow;
	OffsetNumber offnum;
	OffsetNumber maxoff;

	mask_page_lsn_and_checksum(flux_page);

	mask_page_hint_bits(flux_page);
	mask_unused_space(flux_page);

	/*
	 * Dictionary-fork pages (XLOG_FLUX_WRITE_DICT full-page images) have no
	 * opaque special area and no line-pointer array; they are initialized
	 * with PageInit(page, BLCKSZ, 0).  FluxPageGetOpaque would read 8 bytes
	 * past the page end and the line-pointer loop below would mis-mask dict
	 * payload, producing spurious "inconsistent pages" failures under
	 * wal_consistency_checking.  Detect them by their zero special size and
	 * leave the (already LSN/checksum/hint-masked) page untouched.
	 */
	if (PageGetSpecialSize(flux_page) != MAXALIGN(sizeof(FluxPageOpaqueData)))
		return;

	phdr = FluxPageGetOpaque(flux_page);

	/* Check page type before masking flags */
	is_overflow = (phdr->pd_commit_ts_and_flags & FLUX_PAGE_OVERFLOW) != 0;

	/*
	 * Mask the entire packed commit_ts_and_flags field.
	 *
	 * The timestamp uses Max(existing, new) during redo which can produce a
	 * different value if the page was concurrently modified.  Heuristic flags
	 * (e.g., FLUX_PAGE_DEFRAG_NEEDED) may be set by redo but not by the
	 * original operation, or vice versa.
	 */
	phdr->pd_commit_ts_and_flags = 0;

	/*
	 * Ignore the alignment padding after an item whose length is not
	 * MAXALIGNed, as heap_mask does.  PageAddItem copies only the item's
	 * length, so the padding keeps whatever the page's free space held: on
	 * the primary, bytes of tuples that an earlier compaction moved away; on
	 * a standby that restored the page from a full-page image, the zeroed
	 * hole.  This applies to overflow records as much as to tuples.
	 */
	maxoff = PageGetMaxOffsetNumber(flux_page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		ItemId		itemid = PageGetItemId(flux_page, offnum);

		if (ItemIdHasStorage(itemid))
		{
			int			len = ItemIdGetLength(itemid);
			int			padlen = MAXALIGN(len) - len;

			if (padlen > 0)
				memset((char *) PageGetItem(flux_page, itemid) + len,
					   MASK_MARKER, padlen);
		}
	}

	/*
	 * Overflow pages contain FluxOverflowRecordHeader items, not regular
	 * tuples.  Their contents are fully determined by the WAL data, so no
	 * per-item masking is needed.
	 */
	if (is_overflow)
		return;

	/*
	 * Mask tuple-level fields that function as hint bits and are not
	 * faithfully reproduced by WAL redo.  The redo handlers only set the
	 * minimal fields needed for correctness (t_flags, t_commit_ts);
	 * transactional fields like infomask bits are set on the primary but not
	 * replayed.
	 */
	maxoff = PageGetMaxOffsetNumber(flux_page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		ItemId		itemid = PageGetItemId(flux_page, offnum);
		FluxTupleHeader *tuple_hdr;

		if (!ItemIdIsNormal(itemid) || !ItemIdHasStorage(itemid))
			continue;

		tuple_hdr = (FluxTupleHeader *) PageGetItem(flux_page, itemid);
		tuple_hdr->t_infomask = 0;

		/*
		 * xmin and verptr are deliberately NOT masked: freezing is WAL state.
		 * Nor are FLUX_TUPLE_MOVED_PARTITION and FLUX_TUPLE_SUPERSEDED, which
		 * only WAL-logged changes set or clear, nor the forward TID that
		 * XLOG_FLUX_SUPERSEDE stores in a superseded version's t_ctid.
		 */
		tuple_hdr->t_flags &= FLUX_TUPLE_MOVED_PARTITION | FLUX_TUPLE_SUPERSEDED |
			FLUX_TUPLE_KEY_INTACT;
		tuple_hdr->t_commit_ts = 0;
		if (!(tuple_hdr->t_flags & FLUX_TUPLE_SUPERSEDED))
			ItemPointerSetInvalid(&tuple_hdr->t_ctid);
	}
}
