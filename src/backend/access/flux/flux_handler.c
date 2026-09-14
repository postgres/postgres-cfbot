/*-------------------------------------------------------------------------
 *
 * flux_handler.c
 *	  FLUX table access method handler
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_handler.c
 *
 * NOTES
 *	  This file implements the FLUX table access method, which provides
 *	  time-based MVCC with in-place updates, overflow pages for large
 *	  attributes, compression, and advanced space management.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/detoast.h"
#include "access/flux.h"
#include "access/flux_pbu.h"
#include "access/flux_undo.h"
#include "access/slog.h"
#include "access/flux_xlog.h"
#include "access/tableam.h"
#include "access/tableam_indexscan.h"
#include "access/tsmapi.h"
#include "access/multixact.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "nodes/tidbitmap.h"
#include "utils/backend_progress.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * A FLUX bitmap index scan feeds FLUX TIDs into tbm_add_tuples(), whose
 * per-page bitmaps hold TBM_MAX_TUPLES_PER_PAGE offsets.
 */
StaticAssertDecl(MaxFluxItemsPerPage <= TBM_MAX_TUPLES_PER_PAGE,
				 "TBM_MAX_TUPLES_PER_PAGE must cover MaxFluxItemsPerPage");

/* Forward declarations */
static bool flux_index_fetch_tuple(IndexScanFluxData *scan,
								   ItemPointer tid,
								   Snapshot snapshot,
								   TupleTableSlot *tts,
								   bool *call_again, bool *all_dead);

static void flux_prepare_pagescan(FluxScanDesc scan, Buffer buffer);
static BlockNumber flux_scan_stream_read_next(ReadStream *stream,
											  void *callback_private_data,
											  void *per_buffer_data);
static BlockNumber flux_bitmap_stream_read_next(ReadStream *stream,
												void *callback_private_data,
												void *per_buffer_data);
static bool flux_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream);
static bool flux_scan_analyze_next_tuple(TableScanDesc scan,
										 double *liverows, double *deadrows,
										 TupleTableSlot *slot);
static void flux_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
								   ItemPointer maxtid);
static bool flux_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
										   TupleTableSlot *slot);
static bool flux_scan_bitmap_next_tuple(TableScanDesc scan,
										TupleTableSlot *slot,
										bool *recheck,
										uint64 *lossy_pages,
										uint64 *exact_pages);

/* Include operations from other modules */
extern void flux_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
							  uint32 options, BulkInsertState bistate);
extern TM_Result flux_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
								   uint32 options, Snapshot snapshot, Snapshot crosscheck,
								   bool wait, TM_FailureData *tmfd);
extern TM_Result flux_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
								   CommandId cid, uint32 options,
								   Snapshot snapshot, Snapshot crosscheck,
								   bool wait, TM_FailureData *tmfd,
								   LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes);
extern void flux_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
							  CommandId cid, uint32 options, BulkInsertState bistate);
extern void flux_relation_vacuum(Relation onerel, const VacuumParams *params,
								 BufferAccessStrategy bstrategy);

/*
 * Read stream callback for sequential scan prefetching.
 *
 * Returns the next block number to read ahead for the sequential scan.
 * The read_stream infrastructure will prefetch these blocks asynchronously,
 * reducing I/O wait time for cold data.
 *
 * Uses rs_prefetch_block (separate from rs_cblock) to track the prefetch
 * position independently of the scan's current position.
 */
static BlockNumber
flux_scan_stream_read_next(ReadStream *stream,
						   void *callback_private_data,
						   void *per_buffer_data)
{
	FluxScanDesc scan = (FluxScanDesc) callback_private_data;
	BlockNumber block;

	block = scan->rs_prefetch_block;
	if (block < scan->rs_startblock || block >= scan->rs_endblock)
		return InvalidBlockNumber;

	scan->rs_prefetch_block = block + scan->rs_dir;
	return block;
}

/*
 * Read stream callback for bitmap heap scans.
 *
 * Pulls the next block from the TBM iterator and hands it to the read
 * stream so upcoming bitmap pages are prefetched asynchronously, matching
 * the HEAP bitmapheap_stream_read_next() behaviour.  The TBMIterateResult
 * for each block is stashed in per_buffer_data so the consumer can read
 * lossy/recheck flags and the exact tuple offsets without re-iterating.
 */
static BlockNumber
flux_bitmap_stream_read_next(ReadStream *stream,
							 void *callback_private_data,
							 void *per_buffer_data)
{
	FluxScanDesc scan = (FluxScanDesc) callback_private_data;
	TableScanDesc sscan = &scan->rs_base;
	TBMIterateResult *tbmres = per_buffer_data;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/* no more entries in the bitmap */
		if (!tbm_iterate(&sscan->st.rs_tbmiterator, tbmres))
			return InvalidBlockNumber;

		/*
		 * Ignore any claimed entries past what we think is the end of the
		 * relation.  It may have been extended after the start of our scan.
		 * Skip this optimization under SERIALIZABLE, where all
		 * index-reachable tuples must be examined for conflict detection.
		 */
		if (!IsolationIsSerializable() &&
			tbmres->blockno >= scan->rs_nblocks)
			continue;

		return tbmres->blockno;
	}

	Assert(false);
	return InvalidBlockNumber;
}

/*
 * ------------------------------------------------------------------------
 * Slot related callbacks for FLUX AM
 * ------------------------------------------------------------------------
 */

/*
 * Return slot implementation suitable for storing FLUX tuples
 */
static const TupleTableSlotOps *
flux_slot_callbacks(Relation relation)
{
	(void) relation;
	return &TTSOpsFluxTuple;
}

/*
 * ------------------------------------------------------------------------
 * Table scan callbacks for FLUX AM
 * ------------------------------------------------------------------------
 */

/*
 * Start a scan of the FLUX relation
 */
static TableScanDesc
flux_scan_begin(Relation relation, Snapshot snapshot,
				int nkeys, ScanKey key,
				ParallelTableScanDesc pscan,
				uint32 flags)
{
	FluxScanDesc scan;


	scan = (FluxScanDesc) palloc0(sizeof(FluxScanDescData));

	scan->rs_base.rs_rd = relation;
	scan->rs_base.rs_snapshot = snapshot;
	scan->rs_base.rs_nkeys = nkeys;
	scan->rs_base.rs_key = key;
	scan->rs_base.rs_flags = flags;
	scan->rs_base.rs_parallel = pscan;

	scan->rs_cbuf = InvalidBuffer;
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_nblocks = RelationGetNumberOfBlocks(relation);
	scan->rs_startblock = 0;
	scan->rs_endblock = scan->rs_nblocks;
	scan->rs_dir = ForwardScanDirection;
	scan->rs_coffset = FirstOffsetNumber;
	scan->rs_cindex = InvalidOffsetNumber;
	scan->rs_inited = false;
	scan->rs_ntuples = 0;
	scan->rs_vistuples = NULL;
	scan->rs_vm_buffer = InvalidBuffer;
	scan->rs_vm_blockno = InvalidBlockNumber;
	scan->rs_keep_deleted = (flags & SO_RETURN_DELETED) != 0;

	/*
	 * SSI: a seqscan (or samplescan) reads the whole relation, so under
	 * SERIALIZABLE it must take a relation-level SIREAD predicate lock,
	 * exactly as heap_beginscan does.  Per-tuple PredicateLockTID() in the
	 * page path only covers tuples that are actually returned, so without
	 * this a seqscan-based read left no trace for a concurrent writer to
	 * conflict against and FLUX silently missed write-skew anomalies that
	 * heap catches ("could not serialize access due to read/write
	 * dependencies among transactions").
	 *
	 * Predicate locking is a no-op outside SERIALIZABLE; the Assert mirrors
	 * heap's, so a missing snapshot is still caught in every isolation level.
	 */
	if (scan->rs_base.rs_flags & (SO_TYPE_SEQSCAN | SO_TYPE_SAMPLESCAN))
	{
		Assert(snapshot);
		PredicateLockRelation(relation, snapshot);
	}

	/* Count a sequential scan, as heap's initscan does. */
	if (scan->rs_base.rs_flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(relation);

	/* Allocate parallel scan worker data if doing a parallel scan */
	if (pscan != NULL)
		scan->rs_parallelworkerdata = palloc_object(ParallelBlockTableScanWorkerData);
	else
		scan->rs_parallelworkerdata = NULL;

	/* Set up MVCC bookkeeping timestamps (visibility uses xmin/xmax + CLOG) */
	scan->rs_snapshot_ts = GetCurrentTimestamp();
	scan->rs_xact_ts = GetCurrentTimestamp();

	/*
	 * Initialize read stream for sequential prefetching (non-parallel only).
	 * The read stream uses the kernel readahead and our callback to prefetch
	 * upcoming pages, reducing I/O wait time for cold sequential scans.
	 * Parallel scans use their own block coordination, so skip the stream.
	 */
	scan->rs_prefetch_block = 0;
	if (flags & SO_TYPE_BITMAPSCAN)
	{
		/*
		 * Bitmap scans drive the stream from the TBM iterator.  The iterator
		 * is attached by the executor after beginscan, so the callback only
		 * runs once read_stream_next_buffer() is first called.  Per-buffer
		 * data carries the TBMIterateResult for each prefetched block.
		 */
		scan->rs_read_stream = read_stream_begin_relation(READ_STREAM_DEFAULT,
														  NULL, /* bstrategy */
														  relation,
														  MAIN_FORKNUM,
														  flux_bitmap_stream_read_next,
														  scan,
														  sizeof(TBMIterateResult));
	}
	else if (pscan == NULL && scan->rs_nblocks > 0)
	{
		scan->rs_read_stream = read_stream_begin_relation(READ_STREAM_SEQUENTIAL |
														  READ_STREAM_USE_BATCHING,
														  NULL, /* bstrategy */
														  relation,
														  MAIN_FORKNUM,
														  flux_scan_stream_read_next,
														  scan,
														  0);
	}
	else
	{
		scan->rs_read_stream = NULL;
	}

	return (TableScanDesc) scan;
}

/*
 * End the scan and release resources
 */
static void
flux_scan_end(TableScanDesc sscan)
{
	FluxScanDesc scan = (FluxScanDesc) sscan;

	/*
	 * FLUX does not compress attributes, so there is no compression
	 * dictionary to retrain at ANALYZE scan end.
	 */

	/* End read stream before releasing buffers */
	if (scan->rs_read_stream != NULL)
	{
		read_stream_end(scan->rs_read_stream);
		scan->rs_read_stream = NULL;
	}

	/* Release buffer if held */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/* Release cached visibility map buffer */
	if (BufferIsValid(scan->rs_vm_buffer))
	{
		ReleaseBuffer(scan->rs_vm_buffer);
		scan->rs_vm_buffer = InvalidBuffer;
	}

	if (scan->rs_vistuples)
		pfree(scan->rs_vistuples);

	if (scan->rs_parallelworkerdata != NULL)
		pfree(scan->rs_parallelworkerdata);

	/*
	 * Unregister the snapshot if this scan owns it (SO_TEMP_SNAPSHOT).
	 * Without this, catalog scans and parallel worker scans leak snapshot
	 * references, causing "resource was not closed" warnings.
	 */
	if (scan->rs_base.rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_base.rs_snapshot);

	pfree(scan);
}

/*
 * Restart a relation scan
 */
static void
flux_scan_rescan(TableScanDesc sscan, ScanKey key,
				 bool set_params, bool allow_strat,
				 bool allow_sync, bool allow_pagemode)
{
	FluxScanDesc scan = (FluxScanDesc) sscan;

	/* Release current buffer */
	if (BufferIsValid(scan->rs_cbuf))
	{
		ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}

	/* Release cached VM buffer on rescan (relation may have changed) */
	if (BufferIsValid(scan->rs_vm_buffer))
	{
		ReleaseBuffer(scan->rs_vm_buffer);
		scan->rs_vm_buffer = InvalidBuffer;
		scan->rs_vm_blockno = InvalidBlockNumber;
	}

	/* Reset read stream for rescan */
	if (scan->rs_read_stream != NULL)
	{
		read_stream_reset(scan->rs_read_stream);
		scan->rs_prefetch_block = 0;
	}

	/* Reset scan position to start of relation */
	scan->rs_cblock = InvalidBlockNumber;
	scan->rs_nblocks = RelationGetNumberOfBlocks(sscan->rs_rd);
	scan->rs_startblock = 0;
	scan->rs_endblock = scan->rs_nblocks;
	scan->rs_dir = ForwardScanDirection;
	scan->rs_cindex = 0;
	scan->rs_coffset = FirstOffsetNumber;
	scan->rs_inited = false;
	scan->rs_ntuples = 0;

	/* Update scan key if provided */
	scan->rs_base.rs_nkeys = key ? scan->rs_base.rs_nkeys : 0;
	scan->rs_base.rs_key = key;

	/* A rescan is another sequential scan, as in heap's initscan. */
	if (sscan->rs_flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(sscan->rs_rd);
}

/*
 * Get the next block to scan.
 *
 * For serial scans, simply advances sequentially.  For parallel scans,
 * coordinates with other workers via table_block_parallelscan_nextpage()
 * so that each block is scanned by exactly one worker.
 *
 * Returns the next block number, or InvalidBlockNumber when finished.
 */
static BlockNumber
flux_scan_nextblock(FluxScanDesc scan)
{
	BlockNumber nblocks = scan->rs_nblocks;

	if (nblocks == 0)
		return InvalidBlockNumber;

	if (scan->rs_base.rs_parallel != NULL)
	{
		ParallelBlockTableScanDesc pbscan =
			(ParallelBlockTableScanDesc) scan->rs_base.rs_parallel;

		/* Initialize parallel worker state on first call */
		if (!scan->rs_inited)
		{
			table_block_parallelscan_startblock_init(scan->rs_base.rs_rd,
													 scan->rs_parallelworkerdata,
													 pbscan,
													 scan->rs_startblock,
													 InvalidBlockNumber);
			scan->rs_inited = true;
		}

		return table_block_parallelscan_nextpage(scan->rs_base.rs_rd,
												 scan->rs_parallelworkerdata,
												 pbscan);
	}
	else
	{
		/* Serial scan: advance sequentially within the scan's bounds */
		if (scan->rs_cblock == InvalidBlockNumber)
			return scan->rs_startblock < scan->rs_endblock ?
				scan->rs_startblock : InvalidBlockNumber;

		if (scan->rs_cblock + 1 >= scan->rs_endblock)
			return InvalidBlockNumber;

		return scan->rs_cblock + 1;
	}
}

/*
 * SSI bookkeeping for a read that examined the on-page image hdr of the row
 * at tid, as heap does for each tuple it examines: a conflict out to the
 * image's writer or remover (FluxCheckForSerializableConflictOut), and a
 * SIREAD lock on the TID if the read serves a version of this row, the page
 * image (visible) or one rebuilt from UNDO.  The UNDO walk reports the
 * writers it steps over itself (FluxReconstructVisibleVersion).  No-op
 * outside SERIALIZABLE.
 */
static void
flux_ssi_read(Relation rel, ItemPointer tid, FluxTupleHeader *hdr,
			  Buffer buffer, Snapshot snapshot, bool visible, bool served)
{
	if (snapshot == NULL || !IsolationIsSerializable())
		return;
	FluxCheckForSerializableConflictOut(visible, rel, hdr, buffer, snapshot);
	if (served)
		PredicateLockTID(rel, tid, snapshot,
						 visible ? FluxTupleGetXmin(hdr) : InvalidTransactionId);
}

/*
 * flux_prepare_pagescan -- collect visible tuple offsets for page-mode scan
 *
 * Called once per page.  Locks share-exclusive, checks visibility for all
 * tuples, collects the candidate offsets into scan->rs_vistuples[], then
 * unlocks.  The buffer remains pinned via scan->rs_cbuf.
 *
 * This is the FLUX equivalent of heapgetpage(), with one difference: the
 * offsets are only candidates.  FLUX updates in place, so
 * flux_scan_getnextslot re-locks the page for each candidate, decides
 * visibility again and copies the tuple (see FluxSlotStoreTuple).
 *
 * The visibility map optimization (1D) is integrated here: if the page is
 * marked all-visible, per-tuple visibility checks are skipped entirely.
 */
static void
flux_prepare_pagescan(FluxScanDesc scan, Buffer buffer)
{
	Page		page;
	OffsetNumber maxoff;
	OffsetNumber offnum;
	int			ntup = 0;
	bool		all_visible;

	/* Share-exclusive: excludes the in-place byte-CAS UPDATE, see below. */
	LockBuffer(buffer, BUFFER_LOCK_SHARE_EXCLUSIVE);
	page = BufferGetPage(buffer);

	/* Skip new/empty pages */
	if (PageIsNew(page))
	{
		scan->rs_ntuples = 0;
		scan->rs_cindex = 0;
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		return;
	}

	maxoff = PageGetMaxOffsetNumber(page);

	/* Allocate vistuples array if needed (shared with bitmap scan path) */
	if (scan->rs_vistuples == NULL)
	{
		scan->rs_vistuples = (OffsetNumber *)
			MemoryContextAlloc(TopMemoryContext,
							   MaxOffsetNumber * sizeof(OffsetNumber));
	}

	/*
	 * Check visibility: first check the in-page PD_ALL_VISIBLE flag (zero
	 * cost since the page is already pinned and locked).  Only fall through
	 * to the VM fork if the in-page flag is not set.  Use the cached VM
	 * buffer to avoid per-page ReadBufferExtended overhead.
	 */
	if (PageIsAllVisible(page))
		all_visible = true;
	else
		all_visible = FluxVMCheckCached(scan->rs_base.rs_rd, scan->rs_cblock,
										FLUX_VM_ALL_VISIBLE,
										&scan->rs_vm_buffer,
										&scan->rs_vm_blockno);

	for (offnum = FirstOffsetNumber; offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;
		FluxTupleHeader *tuple_header;
		ItemPointerData item_tid;

		itemid = PageGetItemId(page, offnum);
		if (!ItemIdIsNormal(itemid))
			continue;

		tuple_header = (FluxTupleHeader *) PageGetItem(page, itemid);
		ItemPointerSet(&item_tid, BufferGetBlockNumber(buffer), offnum);

		/* Skip overflow records - they are not tuples */
		if (FluxIsOverflowRecordInline(tuple_header, ItemIdGetLength(itemid)))
			continue;

		/* Dirty scans must also consider pending inserts and deletes. */
		if (scan->rs_base.rs_snapshot != NULL &&
			scan->rs_base.rs_snapshot->snapshot_type == SNAPSHOT_DIRTY)
		{
			scan->rs_vistuples[ntup++] = offnum;
			continue;
		}

		/* Skip speculative tuples not yet confirmed */
		if (tuple_header->t_flags & FLUX_TUPLE_SPECULATIVE)
			continue;

		/*
		 * Check MVCC visibility.  If the page is marked all-visible in the
		 * visibility map, skip the expensive per-tuple check.  Otherwise,
		 * consult the commit timestamp and the sLog for in-progress
		 * transaction state.
		 *
		 * NOTE: Do NOT skip FLUX_TUPLE_DELETED tuples here.  The DELETED flag
		 * is set physically at DELETE time, but the delete may be in-progress
		 * or aborted.  The visibility function correctly consults the sLog to
		 * determine actual delete status.
		 */
		if (!all_visible &&
			scan->rs_base.rs_snapshot &&
			!FluxTupleVisibleToSnapshotDual(tuple_header,
											scan->rs_base.rs_snapshot,
											RelationGetRelid(scan->rs_base.rs_rd),
											buffer, &item_tid))
		{
			if (scan->rs_keep_deleted &&
				!IsMVCCSnapshot(scan->rs_base.rs_snapshot) &&
				(tuple_header->t_flags & FLUX_TUPLE_DELETED))
			{
				scan->rs_vistuples[ntup++] = offnum;
				continue;
			}

			/*
			 * The on-page (newest) version is not visible to our snapshot. If
			 * this tuple was updated in place and still has a version chain
			 * in the UNDO log, an OLDER version may be visible -- keep it as
			 * a candidate so flux_scan_getnextslot can reconstruct and serve
			 * the before-image.  Only truly-invisible tuples (no history, or
			 * history exhausted) are dropped here.
			 */
			if ((tuple_header->t_flags & FLUX_TUPLE_UPDATED) &&
				IsMVCCSnapshot(scan->rs_base.rs_snapshot) &&
				UndoRecPtrIsValid(FluxTupleGetVersionPtr(tuple_header)))
			{
				/*
				 * We will serve an OLDER version of this row, so from SSI's
				 * point of view we are reading a version that the concurrent
				 * in-place updater is superseding -- heap's "visible
				 * RECENTLY_DEAD / DELETE_IN_PROGRESS" case, where it reports
				 * the updater's xid as a conflict out.  This branch used to
				 * `continue` straight past both the conflict-out check and
				 * the per-tuple SIREAD lock, so a reader that saw a
				 * before-image left no rw-dependency at all and write-skew
				 * anomalies heap rejects were silently accepted.
				 */
				ItemPointerSet(&item_tid, BufferGetBlockNumber(buffer), offnum);
				flux_ssi_read(scan->rs_base.rs_rd, &item_tid, tuple_header,
							  buffer, scan->rs_base.rs_snapshot, false, true);

				scan->rs_vistuples[ntup++] = offnum;
				continue;
			}

			/*
			 * Tuple is not visible.  If we're in a serializable transaction,
			 * check for rw-conflict out: a concurrent writer modified this
			 * tuple after our snapshot.
			 */
			ItemPointerSet(&item_tid, BufferGetBlockNumber(buffer), offnum);
			flux_ssi_read(scan->rs_base.rs_rd, &item_tid, tuple_header,
						  buffer, scan->rs_base.rs_snapshot, false, false);
			continue;
		}

		/*
		 * Tuple is visible.  Acquire SIREAD predicate lock for SSI so that
		 * concurrent writers can detect rw-antidependencies on this tuple.
		 *
		 * A VISIBLE tuple can still be a conflict-out source: if a concurrent
		 * transaction is deleting or updating it (visible to us, being
		 * written by them), we have read a version they are superseding,
		 * which is an rw-dependency they must know about.  Heap covers this
		 * by calling HeapCheckForSerializableConflictOut() unconditionally,
		 * which reports the updater's xid in the visible RECENTLY_DEAD /
		 * DELETE_IN_PROGRESS cases.  FLUX previously only reported conflicts
		 * for invisible tuples, so the reader never registered the incoming
		 * rw-conflict and a later writer was not recognised as a pivot --
		 * silently accepting write-skew anomalies heap rejects.
		 */
		ItemPointerSet(&item_tid, BufferGetBlockNumber(buffer), offnum);
		flux_ssi_read(scan->rs_base.rs_rd, &item_tid, tuple_header, buffer,
					  scan->rs_base.rs_snapshot, true, true);

		scan->rs_vistuples[ntup++] = offnum;
	}

	scan->rs_ntuples = ntup;
	scan->rs_cindex = 0;

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
}

/*
 * Reconstruct from UNDO the version of the row at tid that the latest
 * snapshot sees: the newest one written by a committed transaction (or by
 * our own), walking past those of running and aborted ones.  onpage is a
 * stable copy of the page version, of length len.  Returns false if the walk
 * served nothing older than onpage, see FluxReconstructVisibleVersion.
 */
static bool
flux_reconstruct_latest(Relation rel, ItemPointer tid, char *onpage,
						Size len, char **bi_data, int *bi_len)
{
	SnapshotData latest = {0};
	bool		found;

	/*
	 * A private snapshot: GetLatestSnapshot() would overwrite the static one
	 * a caller such as an RI crosscheck may still hold, and errors out in a
	 * parallel worker.
	 */
	latest.snapshot_type = SNAPSHOT_MVCC;
	latest.xip = palloc(GetMaxSnapshotXidCount() * sizeof(TransactionId));
	latest.subxip = palloc(GetMaxSnapshotSubxidCount() * sizeof(TransactionId));
	GetSnapshotData(&latest);
	found = FluxReconstructVisibleVersionEx(rel, tid, onpage, len, &latest,
											bi_data, bi_len, NULL);
	pfree(latest.xip);
	pfree(latest.subxip);
	return found;
}

/*
 * What flux_pending_version found.
 */
typedef enum FluxPendingVersion
{
	FLUX_PENDING_NONE,			/* no aborted version: the page version stands */
	FLUX_PENDING_RECONSTRUCTED, /* slot holds the row the revert leaves */
	FLUX_PENDING_GONE,			/* the revert leaves no row */
} FluxPendingVersion;

/*
 * Is the row at offnum of buffer's page at a version whose writer aborted,
 * with the revert still pending (FluxTupleHasAbortedVersion)?  Readers that
 * look at the page itself rather than through an MVCC snapshot (an index
 * build with SnapshotAny, ANALYZE) must see the row as the revert will leave
 * it, and must not revert it themselves: they hold too weak a table lock to
 * run the revert's index reversal, a parallel worker cannot write, and the
 * index being built is not complete yet.  So reconstruct that row from UNDO:
 * after an aborted UPDATE its last committed version, after an aborted
 * DELETE the row as it was before, and nothing after an aborted INSERT
 * (FLUX_PENDING_GONE).  That reconstructed version is stored into slot
 * (FLUX_PENDING_RECONSTRUCTED).  The page is not changed.
 *
 * The caller holds buffer locked share-exclusive.  So it is on return, but
 * it may have been released meanwhile (the UNDO walk must not run under the
 * content lock), so the caller must re-read the page.
 */
static FluxPendingVersion
flux_pending_version(Relation rel, Buffer buffer, OffsetNumber offnum,
					 TupleTableSlot *slot)
{
	Page		page = BufferGetPage(buffer);
	ItemPointerData tid;

	ItemPointerSet(&tid, BufferGetBlockNumber(buffer), offnum);

	for (;;)
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		Size		len;
		char	   *onpage;
		char	   *bi_data = NULL;
		int			bi_len = 0;
		bool		found;

		if (!ItemIdIsNormal(itemid) ||
			!FluxTupleHasAbortedVersion((FluxTupleHeader *)
										PageGetItem(page, itemid)))
			return FLUX_PENDING_NONE;

		/* Walk the chain from a stable copy, without the lock. */
		len = ItemIdGetLength(itemid);
		onpage = palloc(len);
		memcpy(onpage, PageGetItem(page, itemid), len);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		found = flux_reconstruct_latest(rel, &tid, onpage, len,
										&bi_data, &bi_len);
		LockBuffer(buffer, BUFFER_LOCK_SHARE_EXCLUSIVE);

		/*
		 * The walk stops at the newest committed version.  The one it serves
		 * has an aborted writer only if the chain broke off before that.
		 */
		if (found &&
			!FluxTupleHasAbortedVersion((FluxTupleHeader *) bi_data))
		{
			FluxSlotStoreTuple(slot, (FluxTupleHeader *) bi_data, bi_len,
							   InvalidBuffer);
			pfree(bi_data);
			pfree(onpage);
			slot->tts_tableOid = RelationGetRelid(rel);
			slot->tts_tid = tid;
			return FLUX_PENDING_RECONSTRUCTED;
		}
		if (found)
			pfree(bi_data);

		/*
		 * If the revert ran meanwhile, and its UNDO was discarded after it,
		 * the walk may have broken off for that reason: decide again on what
		 * the page holds now.  If not, the UNDO is still there, as discarding
		 * waits for the revert.  Then a walk that found no committed version
		 * ended at the row's aborted INSERT, and one that did not end at all
		 * broke off on a damaged chain.
		 */
		itemid = PageGetItemId(page, offnum);
		if (ItemIdIsNormal(itemid) && ItemIdGetLength(itemid) == len &&
			memcmp(PageGetItem(page, itemid), onpage, len) == 0)
		{
			pfree(onpage);
			if (found)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("could not reconstruct the committed version of tuple (%u,%u) in relation \"%s\"",
										 ItemPointerGetBlockNumber(&tid),
										 offnum, RelationGetRelationName(rel))));
			return FLUX_PENDING_GONE;
		}
		pfree(onpage);
	}
}

/*
 * The on-page version of the tuple at tid is not visible to snapshot.  If it
 * was updated in place and has a version chain, store into slot the older
 * version the snapshot sees, reconstructed from UNDO, and return true.
 * Called with buffer locked share-exclusive; returns with it unlocked if the
 * tuple has history (the chain walk must not run under the content lock),
 * locked otherwise.  *unlocked says which.
 */
static bool
flux_store_visible_older(Relation rel, Buffer buffer, ItemPointer tid,
						 FluxTupleHeader *hdr, Size len, Snapshot snapshot,
						 TupleTableSlot *slot, bool *unlocked)
{
	char	   *onpage;
	char	   *bi_data = NULL;
	int			bi_len = 0;
	bool		found;

	*unlocked = false;
	if (!(hdr->t_flags & FLUX_TUPLE_UPDATED) || snapshot == NULL ||
		!IsMVCCSnapshot(snapshot) ||
		!UndoRecPtrIsValid(FluxTupleGetVersionPtr(hdr)))
		return false;

	onpage = palloc(len);
	memcpy(onpage, hdr, len);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	*unlocked = true;
	found = FluxReconstructVisibleVersion(rel, tid, onpage, len, snapshot,
										  &bi_data, &bi_len);
	pfree(onpage);
	if (!found)
		return false;
	FluxSlotStoreTuple(slot, (FluxTupleHeader *) bi_data, bi_len, InvalidBuffer);
	pfree(bi_data);
	slot->tts_tableOid = RelationGetRelid(rel);
	slot->tts_tid = *tid;
	return true;
}

/*
 * Get next tuple from scan (page-mode)
 *
 * Uses page-mode scanning: for each page, flux_prepare_pagescan() collects
 * candidate tuple offsets.  This function then takes each candidate under a
 * short share-exclusive lock of the already pinned buffer: one ReadBuffer per
 * page, one lock per returned tuple.
 *
 * For parallel scans, block assignment is coordinated via
 * flux_scan_nextblock() which uses the parallel scan infrastructure.
 */
static bool
flux_scan_getnext(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	FluxScanDesc scan = (FluxScanDesc) sscan;
	int			index;

	Assert(!ScanDirectionIsNoMovement(direction));
	Assert(sscan->rs_parallel == NULL || ScanDirectionIsForward(direction));

	/* Discard read-ahead from the old direction, not the current page. */
	if (scan->rs_read_stream != NULL &&
		(direction != scan->rs_dir || !BufferIsValid(scan->rs_cbuf)))
	{
		read_stream_reset(scan->rs_read_stream);
		scan->rs_prefetch_block = BufferIsValid(scan->rs_cbuf) ?
			scan->rs_cblock + direction :
			(ScanDirectionIsForward(direction) ? scan->rs_startblock :
			 scan->rs_endblock - 1);
	}
	scan->rs_dir = direction;
	index = scan->rs_cindex + direction;

	/* If relation is empty, return false immediately */
	if (scan->rs_startblock >= scan->rs_endblock)
	{
		ExecClearTuple(slot);
		return false;
	}

	for (;;)
	{
		Page		page;

		/*
		 * Try to return the next candidate from the current page.  Unlike
		 * heap's page mode, FLUX cannot serve tuples from the merely pinned
		 * page: an in-place UPDATE may have rewritten a candidate since
		 * flux_prepare_pagescan looked at it.  So re-lock (share-exclusive,
		 * which excludes the byte-CAS writer), decide visibility on the bytes
		 * as they are now and copy them, in one lock hold.  A tuple the
		 * prepare pass found invisible stays invisible to an MVCC snapshot:
		 * an UPDATE since then installs a version that snapshot cannot see,
		 * and the prepare pass already kept every row with history as a
		 * candidate.
		 */
		for (; index >= 0 && index < scan->rs_ntuples; index += direction)
		{
			OffsetNumber offnum;
			ItemId		itemid;
			FluxTupleHeader *tuple_header;
			ItemPointerData self_tid;

			offnum = scan->rs_vistuples[index];
			scan->rs_cindex = index;
			ItemPointerSet(&self_tid, scan->rs_cblock, offnum);

			/* Use the same dirty visibility and xid outputs as index scans. */
			if (sscan->rs_snapshot != NULL &&
				sscan->rs_snapshot->snapshot_type == SNAPSHOT_DIRTY)
			{
				IndexScanFluxData fscan;
				ItemPointerData tid;
				bool		call_again = false;
				bool		found;

				fscan.rel = sscan->rs_rd;
				fscan.buffer = InvalidBuffer;
				fscan.keyless = false;
				fscan.keep_deleted = false;
				ItemPointerSet(&tid, scan->rs_cblock, offnum);
				found = flux_index_fetch_tuple(&fscan, &tid,
											   sscan->rs_snapshot, slot,
											   &call_again, NULL);
				if (BufferIsValid(fscan.buffer))
					ReleaseBuffer(fscan.buffer);
				if (found)
					return true;
				continue;
			}

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE_EXCLUSIVE);
			page = BufferGetPage(scan->rs_cbuf);
			itemid = PageGetItemId(page, offnum);

			if (!ItemIdIsNormal(itemid))
			{
				LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
				continue;
			}

			tuple_header = (FluxTupleHeader *) PageGetItem(page, itemid);

			/*
			 * Before-image substitution for committed in-place UPDATEs.
			 *
			 * The on-page image is the NEWEST version, stamped with the
			 * updater's xmin.  If that updater is not visible to our MVCC
			 * snapshot, we must serve the older version reconstructed from
			 * the UNDO log: FluxReconstructVisibleVersion walks the t_verptr
			 * chain and stops at the version whose producing xid is visible
			 * (XidInMVCCSnapshot).  Visibility is decided purely by the xid
			 * snapshot + CLOG, matching FluxTupleSatisfiesMVCC.
			 *
			 * Determine on-page visibility once; only reconstruct when the
			 * newest version is NOT visible to us.  When it IS visible, serve
			 * on-page bytes directly.
			 */
			if ((tuple_header->t_flags & FLUX_TUPLE_UPDATED) &&
				scan->rs_base.rs_snapshot != NULL &&
				IsMVCCSnapshot(scan->rs_base.rs_snapshot) &&
				UndoRecPtrIsValid(FluxTupleGetVersionPtr(tuple_header)) &&
				!FluxTupleVisibleToSnapshotDual(tuple_header,
												scan->rs_base.rs_snapshot,
												RelationGetRelid(scan->rs_base.rs_rd),
												scan->rs_cbuf, &self_tid))
			{
				char	   *bi_data = NULL;
				int			bi_len = 0;
				ItemPointerData item_tid;
				Size		onpage_len = ItemIdGetLength(itemid);
				char	   *onpage = palloc(onpage_len);
				bool		found;

				ItemPointerSet(&item_tid, scan->rs_cblock, offnum);

				/* Walk the chain from a stable copy, without the lock. */
				memcpy(onpage, tuple_header, onpage_len);
				LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
				found = FluxReconstructVisibleVersion(scan->rs_base.rs_rd,
													  &item_tid, onpage,
													  onpage_len,
													  scan->rs_base.rs_snapshot,
													  &bi_data, &bi_len);
				pfree(onpage);
				if (found)
				{
					FluxTupleHeader *bi_tuple = (FluxTupleHeader *) bi_data;

					FluxSlotStoreTuple(slot, bi_tuple, bi_len, InvalidBuffer);
					pfree(bi_tuple);
					ItemPointerSet(&slot->tts_tid, scan->rs_cblock, offnum);
					return true;
				}

				/*
				 * No visible older version: the row is genuinely invisible to
				 * this snapshot (e.g. it was inserted-then-updated all after
				 * our snapshot).  Skip it.
				 */
				continue;
			}

			/*
			 * Serve the on-page version if this snapshot sees it.  The
			 * prepare pass said so, but the row may have been rewritten
			 * since.
			 */
			if (!(scan->rs_base.rs_snapshot != NULL &&
				  IsMVCCSnapshot(scan->rs_base.rs_snapshot) &&
				  !FluxTupleVisibleToSnapshotDual(tuple_header,
												  scan->rs_base.rs_snapshot,
												  RelationGetRelid(scan->rs_base.rs_rd),
												  scan->rs_cbuf, &self_tid)))
			{
				FluxSlotStoreTuple(slot, tuple_header,
								   ItemIdGetLength(itemid), scan->rs_cbuf);
				LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
				ItemPointerSet(&slot->tts_tid, scan->rs_cblock, offnum);
				return true;
			}
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
		}

		/*
		 * Exhausted all visible tuples on the current page. Release the
		 * buffer pin and advance to the next block.
		 */
		if (BufferIsValid(scan->rs_cbuf))
		{
			ReleaseBuffer(scan->rs_cbuf);
			scan->rs_cbuf = InvalidBuffer;
		}

		/*
		 * Get the next page.  Use the read stream if available (prefetches
		 * upcoming pages for I/O efficiency), otherwise fall back to
		 * ReadBuffer for parallel scans.
		 */
		if (scan->rs_read_stream != NULL)
		{
			scan->rs_cbuf = read_stream_next_buffer(scan->rs_read_stream, NULL);
			if (!BufferIsValid(scan->rs_cbuf))
			{
				scan->rs_cblock = InvalidBlockNumber;
				scan->rs_ntuples = 0;
				ExecClearTuple(slot);
				return false;
			}
			scan->rs_cblock = BufferGetBlockNumber(scan->rs_cbuf);
		}
		else
		{
			BlockNumber block = flux_scan_nextblock(scan);

			if (!BlockNumberIsValid(block))
			{
				scan->rs_cblock = InvalidBlockNumber;
				scan->rs_ntuples = 0;
				ExecClearTuple(slot);
				return false;
			}

			scan->rs_cblock = block;
			scan->rs_cbuf = ReadBuffer(scan->rs_base.rs_rd, scan->rs_cblock);
		}

		/*
		 * Opportunistic cleanup: try to prune dead tuples before scanning.
		 * FluxPagePruneOpt() expects only a pin (no lock) and will
		 * non-blockingly try to get an exclusive lock.
		 */
		FluxPagePruneOpt(scan->rs_base.rs_rd, scan->rs_cbuf);

		/*
		 * Prepare the page scan: collect candidate offsets into
		 * rs_vistuples[].  The buffer stays pinned; each candidate is
		 * re-locked when returned.
		 */
		flux_prepare_pagescan(scan, scan->rs_cbuf);
		index = ScanDirectionIsForward(direction) ? 0 : scan->rs_ntuples - 1;
	}
}

/*
 * Set TID range for TID range scans
 *
 * This restricts the scan to only return tuples within the given TID range.
 * Used by TID range scans (WHERE ctid >= ... AND ctid < ...).
 *
 * Following the heap AM pattern, we store the effective min/max TIDs in the
 * base scan descriptor's st.tidrange fields and configure the scan start
 * position accordingly.
 */
static void
flux_scan_set_tidrange(TableScanDesc sscan, ItemPointer mintid,
					   ItemPointer maxtid)
{
	FluxScanDesc scan = (FluxScanDesc) sscan;
	ItemPointerData highestItem;
	ItemPointerData lowestItem;

	flux_scan_rescan(sscan, NULL, false, false, false, false);
	scan->rs_endblock = 0;
	ItemPointerSetInvalid(&sscan->st.tidrange.rs_mintid);
	ItemPointerSetInvalid(&sscan->st.tidrange.rs_maxtid);
	if (scan->rs_nblocks == 0)
		return;

	ItemPointerSet(&highestItem, scan->rs_nblocks - 1, MaxOffsetNumber);
	ItemPointerSet(&lowestItem, 0, FirstOffsetNumber);
	if (ItemPointerCompare(maxtid, &highestItem) < 0)
		ItemPointerCopy(maxtid, &highestItem);
	if (ItemPointerCompare(mintid, &lowestItem) > 0)
		ItemPointerCopy(mintid, &lowestItem);
	if (ItemPointerCompare(&highestItem, &lowestItem) < 0)
		return;

	scan->rs_startblock = ItemPointerGetBlockNumberNoCheck(&lowestItem);
	scan->rs_endblock = ItemPointerGetBlockNumberNoCheck(&highestItem) + 1;
	ItemPointerCopy(&lowestItem, &sscan->st.tidrange.rs_mintid);
	ItemPointerCopy(&highestItem, &sscan->st.tidrange.rs_maxtid);
}

/*
 * Restrict a scan that has not started to numblocks blocks from startblock
 * (InvalidBlockNumber: to the end of the relation), as heap_setscanlimits.
 * BRIN summarizes one block range this way.
 */
static void
flux_scan_setlimits(TableScanDesc sscan, BlockNumber startblock,
					BlockNumber numblocks)
{
	FluxScanDesc scan = (FluxScanDesc) sscan;

	Assert(!BufferIsValid(scan->rs_cbuf) &&
		   scan->rs_cblock == InvalidBlockNumber);

	/* A parallel scan hands out its own blocks: whole relation only. */
	if (sscan->rs_parallel != NULL)
	{
		Assert(startblock == 0 && numblocks == InvalidBlockNumber);
		return;
	}

	scan->rs_startblock = startblock;
	if (numblocks == InvalidBlockNumber || startblock >= scan->rs_nblocks ||
		numblocks >= scan->rs_nblocks - startblock)
		scan->rs_endblock = scan->rs_nblocks;
	else
		scan->rs_endblock = startblock + numblocks;
}

/* Count each returned row, as heap_getnextslot does. */
static bool
flux_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	if (!flux_scan_getnext(sscan, direction, slot))
		return false;
	pgstat_count_heap_getnext(sscan->rs_rd);
	return true;
}

/* Use the same positioning and visibility rules as a sequential scan. */
static bool
flux_scan_getnextslot_tidrange(TableScanDesc sscan, ScanDirection direction,
							   TupleTableSlot *slot)
{
	ItemPointer mintid = &sscan->st.tidrange.rs_mintid;
	ItemPointer maxtid = &sscan->st.tidrange.rs_maxtid;

	/* A normalized exclusive bound can have offset zero. */
	if (((FluxScanDesc) sscan)->rs_endblock > 0)
	{
		while (flux_scan_getnext(sscan, direction, slot))
		{
			if (ItemPointerCompare(&slot->tts_tid, mintid) >= 0 &&
				ItemPointerCompare(&slot->tts_tid, maxtid) <= 0)
			{
				/* Only rows in range, as heap_getnextslot_tidrange. */
				pgstat_count_heap_getnext(sscan->rs_rd);
				return true;
			}
		}
	}
	ExecClearTuple(slot);
	return false;
}

/*
 * Bitmap heap scan: fetch next tuple from bitmap
 *
 * This is the scan_bitmap_next_tuple callback.  It iterates over TIDs from
 * the TBM (TID bitmap) built by a BitmapIndexScan, fetches and checks
 * visibility for each tuple, and returns visible ones in the slot.
 *
 * For each block indicated by the bitmap:
 * - If the block is "lossy" (the bitmap lost per-tuple precision for this
 *   block), we check every tuple on the page.
 * - If the block is "exact", we only check the specific offsets indicated.
 *
 * The scan descriptor was set up via table_beginscan_bm() and the TBM
 * iterator is stored in scan->st.rs_tbmiterator.
 */
static bool
flux_scan_bitmap_next_tuple(TableScanDesc scan,
							TupleTableSlot *slot,
							bool *recheck,
							uint64 *lossy_pages,
							uint64 *exact_pages)
{
	FluxScanDesc rscan = (FluxScanDesc) scan;

	Assert(rscan->rs_read_stream);

	for (;;)
	{
		void	   *per_buffer_data;
		TBMIterateResult *tbmres;

		/*
		 * If we have tuples remaining from a previously fetched page, try to
		 * return one.
		 */
		if (rscan->rs_ntuples > 0 &&
			rscan->rs_cindex < rscan->rs_ntuples)
		{
			OffsetNumber offnum;
			Page		page;
			ItemId		itemid;
			FluxTupleHeader *tuple_hdr;
			ItemPointerData self_tid;

			offnum = rscan->rs_vistuples[rscan->rs_cindex];
			rscan->rs_cindex++;
			ItemPointerSet(&self_tid, rscan->rs_cblock, offnum);

			/* Re-read the page (we released the lock after visibility check) */
			if (!BufferIsValid(rscan->rs_cbuf))
				rscan->rs_cbuf = ReadBuffer(scan->rs_rd, rscan->rs_cblock);

			/*
			 * Excludes in-place writers while we decide and copy (see
			 * FluxSlotStoreTuple).  The row may have been rewritten since the
			 * page was collected, so visibility is decided again below.
			 */
			LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_SHARE_EXCLUSIVE);
			page = BufferGetPage(rscan->rs_cbuf);

			itemid = PageGetItemId(page, offnum);
			if (!ItemIdIsNormal(itemid))
			{
				LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
				continue;
			}

			tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

			/*
			 * NOTE: Do NOT skip FLUX_TUPLE_DELETED here.  This tuple passed
			 * the visibility check during page scan preparation. The DELETED
			 * flag may be set but the delete could be in-progress or aborted.
			 */

			/*
			 * FLUX updates in place keeping the same TID, so a changed
			 * indexed column leaves a stale (oldkey -> tid) secondary entry
			 * beside the new one.  A bitmap scan keeps only TIDs and discards
			 * the index tuple, so it cannot compare the stored key against
			 * the live tuple. Force the executor to re-evaluate
			 * bitmapqualorig against the live tuple whenever we return a
			 * committed in-place UPDATE: an exact (non-lossy) bitmap would
			 * otherwise trust the stale index entry and return a row whose
			 * live key no longer matches the scan key.
			 */
			if (tuple_hdr->t_flags & FLUX_TUPLE_UPDATED)
				*recheck = true;

			/*
			 * Before-image substitution for in-place UPDATEs, mirroring the
			 * sequential-scan and index-fetch paths.  A snapshot that cannot
			 * see the updater's xmin must observe the before-image, not the
			 * on-page (new) data.  Serve it from the UNDO log;
			 * FluxReconstructVisibleVersion walks the t_verptr chain and
			 * stops at the version whose producing xid is visible. Visibility
			 * is ordinary heap-shaped xmin/xmax + CLOG + snapshot
			 * (FluxTupleSatisfiesMVCC).
			 */
			if ((tuple_hdr->t_flags & FLUX_TUPLE_UPDATED) &&
				scan->rs_snapshot != NULL &&
				IsMVCCSnapshot(scan->rs_snapshot) &&
				UndoRecPtrIsValid(FluxTupleGetVersionPtr(tuple_hdr)) &&
				!FluxTupleVisibleToSnapshotDual(tuple_hdr, scan->rs_snapshot,
												RelationGetRelid(scan->rs_rd),
												rscan->rs_cbuf, &self_tid))
			{
				char	   *bi_data = NULL;
				int			bi_len = 0;
				ItemPointerData item_tid;

				ItemPointerSet(&item_tid, rscan->rs_cblock, offnum);

				/*
				 * Unlike the other scan paths, this walks the chain with the
				 * share-exclusive lock still held (the UNDO reads take their
				 * own locks and release them before returning).
				 */
				if (FluxReconstructVisibleVersion(
												  scan->rs_rd,
												  &item_tid,
												  (const char *) tuple_hdr,
												  ItemIdGetLength(itemid),
												  scan->rs_snapshot,
												  &bi_data, &bi_len))
				{
					FluxTupleHeader *bi_tuple = (FluxTupleHeader *) bi_data;

					FluxSlotStoreTuple(slot, bi_tuple, bi_len, InvalidBuffer);
					pfree(bi_tuple);
					slot->tts_tableOid = RelationGetRelid(scan->rs_rd);
					ItemPointerSet(&slot->tts_tid, rscan->rs_cblock, offnum);
					LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

					return true;
				}

				/* No visible older version: skip this tuple. */
				LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
				continue;
			}

			/* Rewritten since collection into a version we cannot see? */
			if (scan->rs_snapshot != NULL && IsMVCCSnapshot(scan->rs_snapshot) &&
				!FluxTupleVisibleToSnapshotDual(tuple_hdr, scan->rs_snapshot,
												RelationGetRelid(scan->rs_rd),
												rscan->rs_cbuf, &self_tid))
			{
				LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
				continue;
			}

			FluxSlotStoreTuple(slot, tuple_hdr,
							   ItemIdGetLength(itemid), rscan->rs_cbuf);
			slot->tts_tableOid = RelationGetRelid(scan->rs_rd);
			ItemPointerSet(&slot->tts_tid, rscan->rs_cblock, offnum);
			LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			return true;
		}

		/* Release buffer from previous block */
		if (BufferIsValid(rscan->rs_cbuf))
		{
			ReleaseBuffer(rscan->rs_cbuf);
			rscan->rs_cbuf = InvalidBuffer;
		}

		/*
		 * Advance to the next block in the bitmap.  The read stream pulls
		 * blocks from the TBM iterator (via flux_bitmap_stream_read_next),
		 * prefetching upcoming bitmap pages, and hands back the matching
		 * TBMIterateResult in per_buffer_data.  Out-of-range blocks were
		 * already filtered by the callback.
		 */
		rscan->rs_cbuf = read_stream_next_buffer(rscan->rs_read_stream,
												 &per_buffer_data);

		if (!BufferIsValid(rscan->rs_cbuf))
			return false;		/* bitmap exhausted */

		tbmres = per_buffer_data;

		Assert(BlockNumberIsValid(tbmres->blockno));
		Assert(BufferGetBlockNumber(rscan->rs_cbuf) == tbmres->blockno);

		*recheck = tbmres->recheck;

		rscan->rs_cblock = tbmres->blockno;

		LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_SHARE_EXCLUSIVE);
		{
			Page		page = BufferGetPage(rscan->rs_cbuf);
			int			ntup = 0;
			OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

			/* Allocate vistuples array if needed */
			if (rscan->rs_vistuples == NULL)
			{
				rscan->rs_vistuples = (OffsetNumber *)
					MemoryContextAlloc(TopMemoryContext,
									   MaxOffsetNumber * sizeof(OffsetNumber));
			}

			if (!tbmres->lossy)
			{
				/*
				 * Exact page: only examine offsets listed in the bitmap.
				 */
				OffsetNumber offsets[TBM_MAX_TUPLES_PER_PAGE];
				int			noffsets;

				noffsets = tbm_extract_page_tuple(tbmres, offsets,
												  TBM_MAX_TUPLES_PER_PAGE);

				for (int j = 0; j < noffsets; j++)
				{
					OffsetNumber offnum = offsets[j];
					ItemId		itemid;
					FluxTupleHeader *tuple_hdr;

					if (offnum < FirstOffsetNumber || offnum > maxoff)
						continue;

					itemid = PageGetItemId(page, offnum);
					if (!ItemIdIsNormal(itemid))
						continue;

					tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

					/* Skip overflow records */
					if (FluxIsOverflowRecordInline(tuple_hdr, ItemIdGetLength(itemid)))
						continue;

					/* Skip speculative tuples */
					if (tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE)
						continue;

					/*
					 * Check visibility.  Keep an invisible-but-updated tuple
					 * that still has a version chain as a candidate so the
					 * tuple loop can reconstruct and serve the before-image.
					 */
					{
						bool		visible;
						bool		keep;
						ItemPointerData item_tid;

						ItemPointerSet(&item_tid, rscan->rs_cblock, offnum);
						visible = scan->rs_snapshot == NULL ||
							FluxTupleVisibleToSnapshotDual(tuple_hdr, scan->rs_snapshot,
														   RelationGetRelid(scan->rs_rd),
														   rscan->rs_cbuf, &item_tid);
						keep = visible ||
							((tuple_hdr->t_flags & FLUX_TUPLE_UPDATED) &&
							 IsMVCCSnapshot(scan->rs_snapshot) &&
							 UndoRecPtrIsValid(FluxTupleGetVersionPtr(tuple_hdr)));

						/* as heap's bitmap scan, for each tuple examined */
						flux_ssi_read(scan->rs_rd, &item_tid, tuple_hdr,
									  rscan->rs_cbuf, scan->rs_snapshot,
									  visible, keep);
						if (!keep)
							continue;
					}

					rscan->rs_vistuples[ntup++] = offnum;
				}

				(*exact_pages)++;
			}
			else
			{
				/*
				 * Lossy page: examine every tuple on the page.  tbmres->lossy
				 * is true here.
				 */
				OffsetNumber offnum;

				for (offnum = FirstOffsetNumber; offnum <= maxoff;
					 offnum = OffsetNumberNext(offnum))
				{
					ItemId		itemid;
					FluxTupleHeader *tuple_hdr;

					itemid = PageGetItemId(page, offnum);
					if (!ItemIdIsNormal(itemid))
						continue;

					tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

					/* Skip overflow records */
					if (FluxIsOverflowRecordInline(tuple_hdr, ItemIdGetLength(itemid)))
						continue;

					/* Skip speculative tuples */
					if (tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE)
						continue;

					/*
					 * Check visibility.  Keep an invisible-but-updated tuple
					 * that still has a version chain as a candidate so the
					 * tuple loop can reconstruct and serve the before-image.
					 */
					{
						bool		visible;
						bool		keep;
						ItemPointerData item_tid;

						ItemPointerSet(&item_tid, rscan->rs_cblock, offnum);
						visible = scan->rs_snapshot == NULL ||
							FluxTupleVisibleToSnapshotDual(tuple_hdr, scan->rs_snapshot,
														   RelationGetRelid(scan->rs_rd),
														   rscan->rs_cbuf, &item_tid);
						keep = visible ||
							((tuple_hdr->t_flags & FLUX_TUPLE_UPDATED) &&
							 IsMVCCSnapshot(scan->rs_snapshot) &&
							 UndoRecPtrIsValid(FluxTupleGetVersionPtr(tuple_hdr)));

						/* as heap's bitmap scan, for each tuple examined */
						flux_ssi_read(scan->rs_rd, &item_tid, tuple_hdr,
									  rscan->rs_cbuf, scan->rs_snapshot,
									  visible, keep);
						if (!keep)
							continue;
					}

					rscan->rs_vistuples[ntup++] = offnum;
				}

				(*lossy_pages)++;
			}

			rscan->rs_ntuples = ntup;
			rscan->rs_cindex = 0;
		}
		LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

		/* Loop back to return the first visible tuple from this block */
	}
}

/*
 * ------------------------------------------------------------------------
 * Index scan callbacks for FLUX AM
 * ------------------------------------------------------------------------
 */

static bool flux_index_getnext_slot(IndexScanDesc scan,
									ScanDirection direction,
									TupleTableSlot *slot);
static void flux_recheck_release(IndexScanFluxData *fscan);

/*
 * Prepare table-AM state for a FLUX index scan (slot-based interface, added by
 * upstream ddce1da5b1b).  Allocates FLUX's private IndexScanFluxData into
 * scan->xs_table_opaque and resolves the getnext_slot implementation.
 *
 * FLUX enables in-place delete-marking (am_index_delete_marking = true), so a
 * plain index scan can reach a heap TID through an entry whose key no longer
 * matches the visible version (a delete-marked tombstone carrying an OLD key,
 * or a live NEW-key entry while an old-snapshot reader is served the
 * reconstructed before-image).  flux_index_getnext_slot rechecks the entry key
 * against the visible tuple and skips on mismatch, so
 * we ask the index AM to keep the index tuple (xs_want_itup) for the whole
 * scan.  The planner suppresses genuine index-only scans on a delete-marking
 * table (RelationSupportsDeleteMarking in get_relation_info), so xs_want_itup
 * here always means "plain scan that wants xs_itup for the recheck", never a
 * true index-only scan.
 */
static void
flux_index_scan_begin(IndexScanDesc scan, uint32 flags)
{
	IndexScanFluxData *fscan = palloc0_object(IndexScanFluxData);

	fscan->rel = scan->heapRelation;
	fscan->buffer = InvalidBuffer;
	fscan->keep_deleted = (flags & SO_RETURN_DELETED) != 0;
	fscan->scan_cxt = CurrentMemoryContext;

	/*
	 * On a delete-marking table, ask the index AM to keep the index tuple
	 * (xs_itup) so flux_index_getnext_slot can recheck the key against the
	 * visible version.  Use xs_want_itup_recheck, NOT xs_want_itup: this is a
	 * plain (buffer-slot) index scan, never a genuine index-only scan
	 * (planner-suppressed), and table_index_getnext_slot asserts that
	 * xs_want_itup implies a virtual slot.
	 */
	if (RelationSupportsDeleteMarking(scan->heapRelation))
		scan->xs_want_itup_recheck = true;

	scan->xs_getnext_slot = flux_index_getnext_slot;
	scan->xs_table_opaque = fscan;
}

static void
flux_index_scan_reset(IndexScanDesc scan)
{
	IndexScanFluxData *fscan = (IndexScanFluxData *) scan->xs_table_opaque;

	if (BufferIsValid(fscan->buffer))
	{
		ReleaseBuffer(fscan->buffer);
		fscan->buffer = InvalidBuffer;
	}
}

static void
flux_index_scan_end(IndexScanDesc scan)
{
	IndexScanFluxData *fscan = (IndexScanFluxData *) scan->xs_table_opaque;

	if (BufferIsValid(fscan->buffer))
		ReleaseBuffer(fscan->buffer);
	flux_recheck_release(fscan);

	pfree(fscan);
}

/*
 * flux_committed_version
 *		The newest version of a row written by a transaction that committed
 *		(or by our own), reconstructed from UNDO; NULL if there is none (an
 *		INSERT that never committed) or the chain cannot be read.  onpage is
 *		a stable copy of the on-page image; the caller holds no buffer lock.
 *
 * Existence checks (SnapshotDirty, SnapshotSelf, SnapshotNonVacuumable) need
 * it when the newest change is not committed: an aborted transaction's
 * change whose revert is still pending (deferred to an undo apply worker, or
 * a crash loser), and, for the old key of an in-place key UPDATE, a writer
 * that is still running.  A fresh MVCC snapshot sees exactly the committed
 * writers, and FluxReconstructVisibleVersion steps past the others.
 */
static FluxTupleHeader *
flux_committed_version(Relation rel, ItemPointer tid, const char *onpage,
					   Size len, Size *out_len)
{
	SnapshotData latest = {0};
	char	   *bi_data = NULL;
	int			bi_len = 0;

	/* Private: GetLatestSnapshot() would overwrite a caller's snapshot. */
	latest.snapshot_type = SNAPSHOT_MVCC;
	latest.xip = palloc(GetMaxSnapshotXidCount() * sizeof(TransactionId));
	latest.subxip = palloc(GetMaxSnapshotSubxidCount() *
						   sizeof(TransactionId));
	GetSnapshotData(&latest);
	latest.curcid = InvalidCommandId;	/* all of our own work */
	if (!FluxReconstructVisibleVersionEx(rel, tid, onpage, len, &latest,
										 &bi_data, &bi_len, NULL))
		bi_data = NULL;
	pfree(latest.xip);
	pfree(latest.subxip);
	*out_len = bi_len;
	return (FluxTupleHeader *) bi_data;
}

/* flux_committed_version of the tuple at tid, read from the page. */
static FluxTupleHeader *
flux_read_committed_version(Relation rel, ItemPointer tid, Size *out_len)
{
	Buffer		buffer = ReadBuffer(rel, ItemPointerGetBlockNumber(tid));
	OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
	Page		page;
	char	   *onpage = NULL;
	Size		len = 0;
	FluxTupleHeader *cv = NULL;

	LockBuffer(buffer, BUFFER_LOCK_SHARE_EXCLUSIVE);
	page = BufferGetPage(buffer);
	if (offnum >= FirstOffsetNumber && offnum <= PageGetMaxOffsetNumber(page) &&
		ItemIdIsNormal(PageGetItemId(page, offnum)))
	{
		ItemId		lp = PageGetItemId(page, offnum);

		len = ItemIdGetLength(lp);
		onpage = palloc(len);
		memcpy(onpage, PageGetItem(page, lp), len);
	}
	UnlockReleaseBuffer(buffer);
	if (onpage != NULL)
	{
		cv = flux_committed_version(rel, tid, onpage, len, out_len);
		pfree(onpage);
	}
	return cv;
}

/*
 * fetch_tid: TID-based visibility check for constraint enforcement code
 * (_bt_check_unique, unique_key_recheck), the special-purpose table-AM entry
 * point upstream ddce1da5b1b split out of the index-scan machinery.  These
 * callers pass a TID taken from an index but do not run an index scan, so they
 * need no scan state -- just "is any version reachable through this TID visible
 * to `snapshot`?".  FLUX has no HOT chains, so this is a single fetch through
 * the same visibility logic the index scan uses.
 */
static bool
flux_fetch_tid(Relation rel, ItemPointer tid, Snapshot snapshot,
			   bool *all_dead)
{
	IndexScanFluxData fscan;
	TupleTableSlot *slot;
	bool		call_again = false;
	bool		found;

	fscan.rel = rel;
	fscan.buffer = InvalidBuffer;
	fscan.keyless = true;
	fscan.keep_deleted = false;

	slot = table_slot_create(rel, NULL);
	found = flux_index_fetch_tuple(&fscan, tid, snapshot, slot,
								   &call_again, all_dead);
	if (BufferIsValid(fscan.buffer))
		ReleaseBuffer(fscan.buffer);
	ExecDropSingleTupleTableSlot(slot);

	return found;
}

/*
 * The IndexInfo of irel for fscan's delete-marking key recheck, built on the
 * first call and then reused: an index scan rechecks every entry it returns,
 * and building it per row (it copies the index's expression and predicate
 * trees) cost a catalog-tree copy per row and kept it to the end of the
 * query, which ran a large nested loop with an inner index scan out of
 * memory.  An expression index also gets the EState its expressions are
 * evaluated in; a plain-column index needs none (FormIndexDatum only reads
 * the slot).  Everything lives in a context of its own, a child of the one
 * the scan lives in, freed by flux_recheck_release.
 */
static IndexInfo *
flux_recheck_info(IndexScanFluxData *fscan, Relation irel)
{
	if (fscan->recheck_ii == NULL)
	{
		MemoryContext oldcxt;

		if (fscan->recheck_cxt == NULL)
			fscan->recheck_cxt = AllocSetContextCreate(fscan->scan_cxt,
													   "FLUX index recheck",
													   ALLOCSET_SMALL_SIZES);
		oldcxt = MemoryContextSwitchTo(fscan->recheck_cxt);
		fscan->recheck_ii = BuildIndexInfo(irel);
		if (fscan->recheck_ii->ii_Expressions != NIL)
			fscan->recheck_estate = CreateExecutorState();
		MemoryContextSwitchTo(oldcxt);
	}
	return fscan->recheck_ii;
}

/* Free what flux_recheck_info built for fscan, if anything. */
static void
flux_recheck_release(IndexScanFluxData *fscan)
{
	if (fscan->recheck_estate != NULL)
		FreeExecutorState(fscan->recheck_estate);
	if (fscan->recheck_cxt != NULL)
		MemoryContextDelete(fscan->recheck_cxt);
	fscan->recheck_estate = NULL;
	fscan->recheck_ii = NULL;
	fscan->recheck_cxt = NULL;
}

/*
 * Recheck a delete-marking index entry's key against the tuple just fetched
 * into `slot` for the scan's snapshot.
 *
 * On a delete-marking (in-place UPDATE) table an index scan can reach a heap
 * TID through an entry whose key differs from the VISIBLE version's key:
 *   - a delete-marked (tombstone) entry carries the OLD key while a new
 *     snapshot reader sees the NEW version; and
 *   - a LIVE (new-key) entry, while an OLD-snapshot reader is served the
 *     reconstructed before-image (OLD key) by the table AM.
 * Returning the tuple via such an entry would be wrong (a duplicate for a
 * range scan, or a phantom key for an old reader).  Re-form the visible
 * tuple's key for this index and compare to the entry (scan->xs_itup);
 * returns true on mismatch (caller must skip this entry).  The matching
 * version is always reachable through the entry whose key equals the visible
 * key.  Moved here from indexam.c index_fetch_heap() when upstream ddce1da5b1b
 * put the whole index-scan heap-fetch loop under the table AM.
 *
 * This runs for every row an index scan returns, so it allocates nothing
 * that outlives the call: the IndexInfo (and an expression index's EState)
 * is fscan's, built once (flux_recheck_info), and an expression's result is
 * freed with the per-tuple memory on return.
 */
static bool
flux_entry_key_mismatch(IndexScanFluxData *fscan, Relation irel,
						IndexTuple itup, TupleTableSlot *slot)
{
	IndexInfo  *ii = flux_recheck_info(fscan, irel);
	EState	   *estate = fscan->recheck_estate;
	Datum		vis_values[INDEX_MAX_KEYS];
	bool		vis_isnull[INDEX_MAX_KEYS];
	Datum		ent_values[INDEX_MAX_KEYS];
	bool		ent_isnull[INDEX_MAX_KEYS];
	TupleDesc	itupdesc = RelationGetDescr(irel);
	bool		mismatch = false;
	int			i;

	if (estate != NULL)
		GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
	FormIndexDatum(ii, slot, estate, vis_values, vis_isnull);
	index_deform_tuple(itup, itupdesc, ent_values, ent_isnull);

	for (i = 0; i < ii->ii_NumIndexKeyAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(itupdesc, i);

		if (vis_isnull[i] != ent_isnull[i])
		{
			mismatch = true;
			break;
		}
		if (vis_isnull[i])
			continue;
		if (!datum_image_eq(vis_values[i], ent_values[i],
							att->attbyval, att->attlen))
		{
			mismatch = true;
			break;
		}
	}

	if (estate != NULL)
		ResetPerTupleExprContext(estate);

	return mismatch;
}

static bool
flux_index_entry_key_mismatch(IndexScanDesc scan, TupleTableSlot *slot)
{
	return flux_entry_key_mismatch((IndexScanFluxData *) scan->xs_table_opaque,
								   scan->indexRelation, scan->xs_itup, slot);
}

/*
 * The version a dirty snapshot judged at tid (in slot) does not carry the
 * entry's key.  If its writer is still running (the snapshot reports an
 * xmin or xmax to wait for), the entry may be the old key of an in-place
 * key UPDATE in progress: heap would find the old version there, live until
 * the updater commits, and make the caller wait.  So do the same when the
 * committed version carries the entry's key: store it in slot, keep the
 * snapshot's xid, and return true.  fscan holds the recheck state.
 */
static bool
flux_dirty_old_key_match(IndexScanFluxData *fscan, Relation rel,
						 Relation irel, IndexTuple itup, ItemPointer tid,
						 Snapshot snapshot, TupleTableSlot *slot)
{
	FluxTupleHeader *cv;
	Size		cv_len;

	if (snapshot == NULL || snapshot->snapshot_type != SNAPSHOT_DIRTY ||
		(!TransactionIdIsValid(snapshot->xmin) &&
		 !TransactionIdIsValid(snapshot->xmax)))
		return false;
	cv = flux_read_committed_version(rel, tid, &cv_len);
	if (cv == NULL)
		return false;
	FluxSlotStoreTuple(slot, cv, cv_len, InvalidBuffer);
	pfree(cv);
	slot->tts_tableOid = RelationGetRelid(rel);
	slot->tts_tid = *tid;
	if (flux_entry_key_mismatch(fscan, irel, itup, slot))
		return false;

	/*
	 * The running writer is replacing this version, as heap reports for an
	 * old version: xmax, and no speculative token (the version is committed,
	 * so its writer is not a speculative inserter).
	 */
	if (TransactionIdIsValid(snapshot->xmin))
	{
		snapshot->xmax = snapshot->xmin;
		snapshot->xmin = InvalidTransactionId;
		snapshot->speculativeToken = 0;
	}
	return true;
}

/*
 * index_fetch_tid: table_index_fetch_tid() for FLUX, for _bt_check_unique.
 *
 * An in-place key UPDATE leaves the row at its TID under two entries, the
 * delete-marked old key and the live new one, so whether the entry's key is
 * taken depends on which version carries it.  Judge the version the snapshot
 * finds (flux_index_fetch_tuple: the committed one if the newest change is
 * an aborted transaction's with its revert pending), and count it only if it
 * carries the entry's key:
 *
 * - updater running: the new-key entry matches the page image (xmin, the
 *   updater, is reported); the old-key entry matches the committed version
 *   (flux_dirty_old_key_match, the updater reported as xmax).  The caller
 *   waits either way, then checks again.
 * - updater aborted: the committed version carries the old key, which is
 *   taken; the new key is not.
 * - updater committed: the page image carries the new key; the old key is
 *   free.  *all_dead is flux_index_fetch_tuple's per-TID verdict, and the
 *   caller never kills a tombstone with it.
 * - a speculative insertion in progress: its token, as heap reports it.
 *
 * _bt_check_unique calls this for every entry of the key it inserts, so the
 * key recheck's state is freed before returning (flux_recheck_release).
 */
static bool
flux_index_fetch_tid(Relation rel, Relation irel, IndexTuple itup,
					 ItemPointer tid, Snapshot snapshot, bool *all_dead)
{
	IndexScanFluxData fscan = {0};
	TupleTableSlot *slot;
	bool		call_again = false;
	bool		found;

	fscan.rel = rel;
	fscan.buffer = InvalidBuffer;
	fscan.keyless = false;
	fscan.keep_deleted = false;
	fscan.scan_cxt = CurrentMemoryContext;

	slot = table_slot_create(rel, NULL);
	found = flux_index_fetch_tuple(&fscan, tid, snapshot, slot,
								   &call_again, all_dead);
	if (BufferIsValid(fscan.buffer))
		ReleaseBuffer(fscan.buffer);
	if (found && flux_entry_key_mismatch(&fscan, irel, itup, slot))
		found = flux_dirty_old_key_match(&fscan, rel, irel, itup, tid,
										 snapshot, slot);
	flux_recheck_release(&fscan);
	ExecDropSingleTupleTableSlot(slot);

	return found;
}

/*
 * xs_getnext_slot callback for FLUX (slot-based table-AM index scan
 * interface).  This is the single entry point the executor reaches through
 * table_index_getnext_slot(): it drives the index AM (amgettuple via
 * tableam_index_getnext_tid) and, for each TID, fetches and visibility-checks
 * the FLUX tuple into the caller's slot, applying the delete-marking key
 * recheck.  Mirrors heapam_index_getnext_slot's structure, minus HOT chains
 * (FLUX does in-place, stable-TID updates and never sets call_again).
 */
static bool
flux_index_getnext_slot(IndexScanDesc scan, ScanDirection direction,
						TupleTableSlot *slot)
{
	IndexScanFluxData *fscan = (IndexScanFluxData *) scan->xs_table_opaque;

	Assert(TransactionIdIsValid(RecentXmin));

	for (;;)
	{
		bool		call_again = false;
		bool		all_dead = false;

		/* Get the next TID from the index */
		if (!tableam_index_getnext_tid(scan, direction))
			return false;

		Assert(ItemPointerIsValid(&scan->xs_heaptid));

		/* Fetch and visibility-check the FLUX tuple for this TID */
		if (!flux_index_fetch_tuple(fscan, &scan->xs_heaptid,
									scan->xs_snapshot, slot,
									&call_again, &all_dead))
		{
			/*
			 * No visible tuple for this entry.  Tell the index AM to kill its
			 * entry only when the TID is dead to all (never carry a sticky
			 * verdict), and never in recovery.
			 */
			if (all_dead && !scan->xactStartedInRecovery)
				scan->kill_prior_tuple = true;
			continue;			/* try next index entry */
		}

		pgstat_count_heap_fetch(scan->indexRelation);

		/* delete-marking key recheck */
		if (scan->xs_itup != NULL &&
			RelationSupportsDeleteMarking(scan->heapRelation) &&
			flux_index_entry_key_mismatch(scan, slot) &&
			!flux_dirty_old_key_match(fscan, scan->heapRelation,
									  scan->indexRelation, scan->xs_itup,
									  &scan->xs_heaptid, scan->xs_snapshot,
									  slot))
		{
			/*
			 * The entry's key does not describe the visible version.  Do NOT
			 * let this count toward kill_prior_tuple (the entry is not dead
			 * -- an older snapshot legitimately needs it).  Fetch the next
			 * TID.
			 */
			continue;
		}

		return true;
	}

	pg_unreachable();
	return false;
}

/*
 * HeapTupleSatisfiesDirty for a FLUX tuple: is the version live or being
 * inserted, and who is still writing it?  Reports an in-progress inserter in
 * snapshot->xmin (with a speculative insert's token, which t_ctid holds as in
 * heap) and an in-progress deleter in snapshot->xmax, for _bt_check_unique and
 * ON CONFLICT.  Everything is read from the header.  The caller holds a lock
 * on the buffer.
 */
static bool
flux_tuple_satisfies_dirty(FluxTupleHeader *tuple_hdr, Snapshot snapshot,
						   Buffer buffer)
{
	snapshot->xmin = InvalidTransactionId;
	snapshot->xmax = InvalidTransactionId;
	snapshot->speculativeToken = 0;

	/*
	 * A speculative insert that lost its arbiter race is killed by
	 * flux_tuple_complete_speculative(succeeded=false), which marks it
	 * DELETED with t_xmax == t_xmin and leaves it on the page. Such a tuple
	 * can never become live again (its inserter and deleter are the same xid,
	 * so they commit or roll back together), so it is dead to every snapshot
	 * -- the verdict heap_abort_speculative gets by zeroing xmin.  Without
	 * this, the killer's own next arbiter pass finds the tuple, reports its
	 * own top xid as xmin, and XactLockTableWait spins forever on itself
	 * (TransactionIdIsInProgress(own xid) is always true); other backends
	 * would wait on a speculative token already released.
	 */
	if ((tuple_hdr->t_flags & FLUX_TUPLE_DELETED) &&
		TransactionIdIsValid(FluxTupleGetXmin(tuple_hdr)) &&
		TransactionIdEquals(FluxTupleGetXmin(tuple_hdr),
							FluxTupleGetXmax(tuple_hdr)))
	{
		return false;
	}

	/*
	 * Resolve the inserter (t_xmin) unless it is ALREADY known committed via
	 * the FLUX_TUPLE_XMIN_COMMITTED hint.
	 *
	 * We must NOT gate this on FLUX_TUPLE_UNCOMMITTED: that flag is a pure
	 * hint cleared at PRE_COMMIT (flux_finish_page), which runs BEFORE the
	 * CLOG write / ProcArrayEndTransaction.  So an inserter that is still in
	 * progress -- past PRE_COMMIT but not yet committed -- has UNCOMMITTED
	 * already clear, and gating on it would skip resolution and wrongly treat
	 * the row as a committed conflict.  In ON CONFLICT that surfaced as
	 * TM_Invisible -> slot_getsysattr on an empty slot -> Assert.
	 * FLUX_TUPLE_XMIN_COMMITTED, by contrast, is set ONLY from a CLOG read
	 * (below and in FluxSetHintBits), so it can never precede the actual
	 * commit: gating on its absence resolves every not-yet-committed inserter
	 * correctly.  t_xmin is the inserter (heap xmin semantics).
	 */
	if (!(tuple_hdr->t_flags & FLUX_TUPLE_XMIN_COMMITTED))
	{
		TransactionId hint_xid = FluxTupleGetXmin(tuple_hdr);

		if (TransactionIdIsValid(hint_xid) &&
			!TransactionIdIsCurrentTransactionId(hint_xid))
		{
			if (TransactionIdIsInProgress(hint_xid))
			{
				/*
				 * Another transaction is inserting this tuple. Report xmin
				 * for SpeculativeInsertionWait.
				 */
				snapshot->xmin = hint_xid;

				if (tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE)
					snapshot->speculativeToken =
						ItemPointerGetBlockNumber(&tuple_hdr->t_ctid);

				return true;
			}
			else if (!TransactionIdDidCommit(hint_xid))
			{
				/*
				 * Inserter aborted or crashed -- invisible.  Use !DidCommit
				 * rather than DidAbort: a crashed xid reports neither
				 * committed nor aborted, and with no INSERT undo record the
				 * aborted-inserter CLOG verdict is the only rollback signal
				 * (heap does the same).
				 */
				return false;
			}
			else
			{
				/*
				 * Inserter committed.  Cache the CLOG verdict as the
				 * FLUX_TUPLE_XMIN_COMMITTED hint (set ONLY from a CLOG read,
				 * so it can never precede commit) and clear the stale
				 * UNCOMMITTED hint, unless a deleter, which may still be
				 * running, wrote the version last (see flux.h).
				 * BufferSetHintBits16 handles the lock upgrade.
				 */
				BufferSetHintBits16(&tuple_hdr->t_flags,
									(tuple_hdr->t_flags |
									 FLUX_TUPLE_XMIN_COMMITTED) &
									~((tuple_hdr->t_flags & FLUX_TUPLE_DELETED) ?
									  0 : FLUX_TUPLE_UNCOMMITTED),
									buffer);
			}
		}

		/*
		 * Otherwise the insert is our own (an own insert rolled back with its
		 * savepoint has an aborted subtransaction's xid in t_xmin, which is
		 * not current and did not commit), or t_xmin is invalid.  Our own
		 * delete is the DELETED case below.
		 */
	}

	/* Inserting xact is committed (or ours).  Check deletion. */
	if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
	{
		/*
		 * Deletion state is read from the on-page header, not the per-tuple
		 * sLog (per-page conflict tracking no longer writes a shared DELETE
		 * entry).  The deleter is t_xmax; classify it via current-xact /
		 * in-progress / CLOG, exactly as heap's HeapTupleSatisfiesDirty does
		 * for xmax.  This drives _bt_check_unique: an in-progress deleter
		 * sets snapshot->xmax (caller waits), our own or a committed delete
		 * makes the entry dead (visible=false -> not a uniqueness conflict),
		 * and an aborted delete leaves the row live.
		 */
		TransactionId del_xid = FluxTupleGetXmax(tuple_hdr);

		if (!TransactionIdIsValid(del_xid))
		{
			/*
			 * DELETED flag with no deleter (xmax invalid).  The commit-time
			 * page sweep marks a savepoint-rolled-back INSERT as DELETED and
			 * clears UNCOMMITTED but stamps no deleter.  With no INSERT undo
			 * record, the inserter's t_xmin is the only rollback signal: if
			 * it never committed the row was never really there, so it is
			 * invisible.  Only a genuinely committed inserter with a stale
			 * DELETED hint is live.
			 */
			TransactionId ins_xid = FluxTupleGetXmin(tuple_hdr);

			if (TransactionIdIsValid(ins_xid) &&
				!TransactionIdIsCurrentTransactionId(ins_xid) &&
				!TransactionIdIsInProgress(ins_xid) &&
				!TransactionIdDidCommit(ins_xid))
			{
				return false;
			}

			BufferSetHintBits16(&tuple_hdr->t_flags,
								tuple_hdr->t_flags & ~FLUX_TUPLE_DELETED,
								buffer);
			/* fall through -- tuple is live */
		}
		else if (TransactionIdIsCurrentTransactionId(del_xid))
		{
			/* We deleted it ourselves. */
			return false;
		}
		else if (TransactionIdIsInProgress(del_xid))
		{
			/* Deleter still running: report xmax so the caller waits. */
			snapshot->xmax = del_xid;
			return true;
		}
		else if (TransactionIdDidCommit(del_xid))
		{
			/*
			 * Delete committed: the row (and its index entry) is dead.
			 */
			return false;
		}
		else
		{
			/*
			 * Deleter aborted: the delete did not take effect.  Clear the
			 * stale DELETED hint and treat the tuple as live.
			 */
			BufferSetHintBits16(&tuple_hdr->t_flags,
								tuple_hdr->t_flags & ~FLUX_TUPLE_DELETED,
								buffer);
			/* fall through -- tuple is live */
		}
	}

	/*
	 * FLUX_TUPLE_UPDATED alone does not make the tuple an old version: FLUX
	 * updates in place, so an UPDATED tuple is the live current version, kept
	 * marked for before-image serving.  An UPDATE that moves the row to a new
	 * TID deletes the old version, which the DELETED handling above has
	 * decided, and a row lock is not a delete.
	 */
	return true;
}

/*
 * Fetch the FLUX tuple at `tid` into `tts` after a visibility test according
 * to `snapshot`; returns true if a visible version was produced.  Called only
 * from flux_index_getnext_slot (the xs_getnext_slot callback).
 *
 * *call_again is always left false: FLUX does in-place, stable-TID updates, so
 * there is never more than one row version reachable via a single index entry
 * (no HOT-style chain to keep walking).  *all_dead, if non-NULL, is set true
 * only when this specific TID is dead to every snapshot; the caller uses it to
 * drive kill_prior_tuple.  It is strictly PER-TID -- never a sticky scan-level
 * verdict -- or a live entry could be wrongly marked LP_DEAD (silent data
 * loss).
 */
static bool
flux_index_fetch_tuple(IndexScanFluxData *scan,
					   ItemPointer tid,
					   Snapshot snapshot,
					   TupleTableSlot *tts,
					   bool *call_again, bool *all_dead)
{
	Size		tuple_len;
	Relation	rel = scan->rel;
	BlockNumber blkno = ItemPointerGetBlockNumber(tid);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
	Buffer		buffer;
	Page		page;
	ItemId		itemid;
	FluxTupleHeader *tuple_hdr;
	bool		visible = false;

	/*
	 * Initialize output parameters.  all_dead is strictly PER-TID: it must
	 * reflect only whether *this* TID is dead to every snapshot, never a
	 * verdict carried over from a previously-fetched TID in the same scan.
	 * FLUX does in-place updates and never sets call_again, so each TID is
	 * fetched exactly once; a sticky scan-level all_dead would leak an
	 * earlier TID's dead verdict onto a later LIVE TID, making indexam.c set
	 * kill_prior_tuple and _bt_killitems mark the live entry LP_DEAD -- the
	 * silent-data-loss "kill LIVE entries" failure.  So always start false
	 * and set true only in the truly-dead branch below.
	 */
	*call_again = false;
	if (all_dead)
		*all_dead = false;

	/*
	 * FLUX secondary indexes hold plain 6-byte heap-style TIDs.  A
	 * key-changing UPDATE stays in place (stable TID) and delete-marks the
	 * old (k_old, TID) entry, so an entry reached here may carry a key the
	 * served version does not have; flux_index_getnext_slot rechecks the
	 * entry key against the served version.  This function is a plain TID
	 * fetch that checks visibility.
	 */

	/* Clear the slot */
	ExecClearTuple(tts);

	/*
	 * Release any buffer from a previous index_fetch_tuple call, but keep the
	 * pin when the next TID is on the same page, as heap's index fetch does:
	 * an index scan in physical order then visits each page once, which is
	 * what the planner's correlation costing assumes.
	 */
	buffer = ReleaseAndReadBuffer(scan->buffer, rel, blkno);
	scan->buffer = InvalidBuffer;
	LockBuffer(buffer, BUFFER_LOCK_SHARE_EXCLUSIVE);

	page = BufferGetPage(buffer);

	/* Validate offset */
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		UnlockReleaseBuffer(buffer);
		return false;
	}

	/* Get the item */
	itemid = PageGetItemId(page, offnum);

	if (!ItemIdIsNormal(itemid))
	{
		UnlockReleaseBuffer(buffer);
		return false;
	}

	/* Get tuple header */
	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

	/*
	 * An existence check -- a uniqueness or exclusion check (SnapshotDirty,
	 * SnapshotSelf) or SnapshotNonVacuumable -- whose row's newest change
	 * belongs to an aborted transaction with its revert still pending (an
	 * undo apply worker's, or a crash loser's).  The row exists, at the
	 * version before that change: an in-place UPDATE re-stamped t_xmin to the
	 * aborted updater, so judging the page image would call the row an
	 * aborted insert and let a duplicate in.  Judge the committed version
	 * instead, as an MVCC reader does (FluxWriterVisibleToSnapshot).  Never
	 * revert here (flux_help_revert): _bt_check_unique holds a btree buffer
	 * lock, which the revert's index work (flux_undo_index_first) would wait
	 * for.
	 *
	 * The committed version is live: nothing can update or delete a row on
	 * top of an aborted change without reverting it first.  A caller without
	 * the index entry's key (fetch_tid) cannot tell the old key of an aborted
	 * in-place key UPDATE from its new one; for those the entry decides (see
	 * flux_index_fetch_tid), and fetch_tid keeps the old verdict.
	 */
	if (snapshot != NULL && !IsMVCCSnapshot(snapshot) &&
		snapshot->snapshot_type != SNAPSHOT_ANY &&
		FluxTupleHasAbortedVersion(tuple_hdr) &&
		!(scan->keyless && (tuple_hdr->t_flags & FLUX_TUPLE_KEYS_UPDATED)))
	{
		Size		len = ItemIdGetLength(itemid);
		char	   *onpage = palloc(len);
		FluxTupleHeader *cv;
		Size		cv_len;

		memcpy(onpage, tuple_hdr, len);
		UnlockReleaseBuffer(buffer);
		if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
		{
			snapshot->xmin = InvalidTransactionId;
			snapshot->xmax = InvalidTransactionId;
			snapshot->speculativeToken = 0;
		}
		cv = flux_committed_version(rel, tid, onpage, len, &cv_len);
		pfree(onpage);
		if (cv == NULL)
			return false;
		FluxSlotStoreTuple(tts, cv, cv_len, InvalidBuffer);
		pfree(cv);
		tts->tts_tableOid = RelationGetRelid(rel);
		tts->tts_tid = *tid;
		return true;
	}

	/* Check visibility (heap-shaped xmin/xmax) */
	if (snapshot)
	{
		if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
			visible = flux_tuple_satisfies_dirty(tuple_hdr, snapshot, buffer);
		else
		{
			visible = FluxTupleVisibleToSnapshotDual(tuple_hdr, snapshot,
													 RelationGetRelid(rel),
													 buffer, tid);

			/*
			 * The caller judges deleted versions itself (SO_RETURN_DELETED),
			 * except a speculative insertion that lost its race, which is
			 * dead to everyone; the sequential path skips those too.
			 */
			if (!visible && scan->keep_deleted &&
				snapshot->snapshot_type == SNAPSHOT_ANY &&
				(tuple_hdr->t_flags & FLUX_TUPLE_DELETED) &&
				!(tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE))
				visible = true;
		}
	}
	else
	{
		/*
		 * No snapshot means fetch unconditionally (e.g., system catalog
		 * scans, VACUUM FULL table rewrite).  Only truly deleted tuples are
		 * invisible.  UPDATED tuples are live (they contain the current
		 * version of the data after an in-place update).
		 */
		visible = !(tuple_hdr->t_flags & FLUX_TUPLE_DELETED);
	}

	/*
	 * SSI bookkeeping for the index-fetch path, mirroring heap (see
	 * heapam_indexscan.c: HeapCheckForSerializableConflictOut on every tuple
	 * examined, PredicateLockTID on the one served).  Done here, while we
	 * still hold the buffer's share lock.  A version rebuilt from UNDO below
	 * takes its TID lock there, once the walk has found it; the walk reports
	 * the writers it steps over.
	 */
	flux_ssi_read(rel, tid, tuple_hdr, buffer, snapshot, visible, visible);

	if (!visible)
	{
		/*
		 * Index fetch: the on-page (newest) version is not visible to our
		 * snapshot, but if this tuple was updated in place and still has a
		 * version chain in the UNDO log, an OLDER version may be visible.
		 * Reconstruct it and serve the before-image, mirroring the
		 * sequential-scan path.  This is what lets a REPEATABLE READ reader
		 * whose snapshot predates a committed UPDATE still find the row at
		 * its old indexed key.
		 */
		if ((tuple_hdr->t_flags & FLUX_TUPLE_UPDATED) &&
			snapshot != NULL && IsMVCCSnapshot(snapshot) &&
			UndoRecPtrIsValid(FluxTupleGetVersionPtr(tuple_hdr)))
		{
			char	   *bi_data = NULL;
			int			bi_len = 0;
			Size		onpage_len = ItemIdGetLength(itemid);
			char	   *onpage = palloc(onpage_len);
			bool		found;

			/* Walk the chain from a stable copy, without the lock. */
			memcpy(onpage, tuple_hdr, onpage_len);
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			found = FluxReconstructVisibleVersion(rel, tid, onpage, onpage_len,
												  snapshot, &bi_data, &bi_len);
			pfree(onpage);
			if (found)
			{
				FluxTupleHeader *bi_tuple = (FluxTupleHeader *) bi_data;

				PredicateLockTID(rel, tid, snapshot, InvalidTransactionId);
				FluxSlotStoreTuple(tts, bi_tuple, bi_len, InvalidBuffer);
				pfree(bi_tuple);
				tts->tts_tableOid = RelationGetRelid(rel);
				ItemPointerCopy(tid, &tts->tts_tid);
				ReleaseBuffer(buffer);
				return true;
			}
			ReleaseBuffer(buffer);
			return false;
		}

		/*
		 * Set the all_dead hint only if the tuple is committed-deleted AND
		 * its deleter (t_xmax) precedes the oldest-xmin horizon, meaning no
		 * running or future snapshot can still see it.  FluxTupleDeadToAll
		 * does the XID-horizon check (t_xmax committed and < oldest_xmin), as
		 * heap does.
		 *
		 * Setting all_dead prematurely would let the index AM remove the
		 * entry while concurrent transactions still need it.
		 */
		if (FluxTupleDeadToAll(tuple_hdr, FluxGetOldestXminHorizon(rel)))
		{
			if (all_dead)
				*all_dead = true;
		}

		/*
		 * Do NOT set call_again for UPDATED tuples.  For out-of-place updates
		 * the executor inserts new index entries (TU_All), so the index scan
		 * will naturally find the new version through its own index entry.
		 * Setting call_again=true here with the same tid causes an infinite
		 * loop because the caller retries with the unchanged tid parameter.
		 */

		UnlockReleaseBuffer(buffer);
		return false;
	}

	/*
	 * Copy the version we decided on while still holding the share-exclusive
	 * lock: FLUX rewrites tuples in place, so the page bytes may change the
	 * moment we unlock (see FluxSlotStoreTuple).  Everything below works on
	 * the copy, which lives in the slot's context.
	 */
	tuple_len = ItemIdGetLength(itemid);
	{
		FluxTupleHeader *copy = MemoryContextAlloc(tts->tts_mcxt, tuple_len);

		memcpy(copy, tuple_hdr, tuple_len);
		tuple_hdr = copy;
	}
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Before-image substitution for in-place UPDATEs, mirroring the
	 * sequential-scan path (flux_getnextslot) and the fetch-by-TID path
	 * (flux_tuple_fetch_row_version).  If the reader can see the updater's
	 * xmin the on-page value is correct and the callee returns false (serve
	 * on-page); if not, an older visible version is reconstructed from the
	 * UNDO log so a REPEATABLE READ / SERIALIZABLE reader whose snapshot
	 * cannot see the update observes the before-image through any index --
	 * including one whose key the UPDATE never touched (e.g. a primary-key
	 * lookup).  Visibility is ordinary heap-shaped xmin/xmax + CLOG +
	 * snapshot (XidInMVCCSnapshot in the callee).
	 *
	 * Key UPDATEs are in place too, so the before-image reached through an
	 * index entry may carry a different key than the entry;
	 * flux_index_getnext_slot rechecks the entry key against the version
	 * served here.
	 *
	 * The UNCOMMITTED test only saves the walk: the version is visible, so
	 * its t_xmin is, and the walk can only replace a delete the snapshot does
	 * not see by the image before it, with the same values.  A stale hint
	 * (see flux.h) serves the on-page image below instead.
	 */
	if ((tuple_hdr->t_flags & FLUX_TUPLE_UPDATED) &&
		!(tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED) &&
		snapshot != NULL && IsMVCCSnapshot(snapshot))
	{
		char	   *bi_data = NULL;
		int			bi_len = 0;

		if (FluxReconstructVisibleVersion(rel, tid,
										  (const char *) tuple_hdr,
										  tuple_len,
										  snapshot,
										  &bi_data, &bi_len))
		{
			FluxTupleHeader *bi_tuple = (FluxTupleHeader *) bi_data;

			pfree(tuple_hdr);
			FluxSlotStoreTuple(tts, bi_tuple, bi_len, InvalidBuffer);
			pfree(bi_tuple);
			tts->tts_tableOid = RelationGetRelid(rel);
			tts->tts_tid = *tid;

			scan->buffer = buffer;
			return true;
		}
	}

	/*
	 * Tuple is visible.  A committed-DELETED tuple can still be visible here:
	 * an RR/SERIALIZABLE reader whose snapshot predates the delete sees the
	 * row (FluxTupleVisibleToSnapshotDual already returned true because the
	 * deleter xid is not visible to this snapshot).
	 * FluxTupleToSlotWithOverflow refuses a DELETED tuple outright (it can't
	 * tell visible-to-this-snapshot from physically-dead), so serve the
	 * on-page image via FluxSlotStoreTuple -- the same virtual-slot store the
	 * sequential-scan path uses, which has no DELETED reject.  This mirrors
	 * seqscan and fixes a lost-row/wrong-result under REPEATABLE READ via an
	 * index path.
	 */
	if (visible && (tuple_hdr->t_flags & FLUX_TUPLE_DELETED))
	{
		FluxSlotStoreTuple(tts, tuple_hdr, tuple_len, InvalidBuffer);
		pfree(tuple_hdr);
		tts->tts_tableOid = RelationGetRelid(rel);
		tts->tts_tid = *tid;
		scan->buffer = buffer;
		return true;
	}

	/* Tuple is visible - convert to slot (with overflow fetch) */
	if (FluxTupleToSlotWithOverflow(tuple_hdr, tts, rel))
	{
		tts->tts_tableOid = RelationGetRelid(rel);
		tts->tts_tid = *tid;
		/* Slot is already marked valid by FluxTupleToSlotWithOverflow */
	}
	else
	{
		pfree(tuple_hdr);
		ReleaseBuffer(buffer);
		return false;
	}

	/*
	 * FluxTupleToSlotWithOverflow left datums pointing into our copy;
	 * materialize them into slot-owned memory, then drop the copy.
	 */
	ExecMaterializeSlot(tts);
	pfree(tuple_hdr);

	/*
	 * Keep the pin for the next fetch (see ReleaseAndReadBuffer above); the
	 * slot no longer points into the page.  The scan's reset and end release
	 * it.
	 */
	scan->buffer = buffer;

	return true;
}

/*
 * ------------------------------------------------------------------------
 * Tuple manipulation callbacks for FLUX AM
 * ------------------------------------------------------------------------
 */

/*
 * Fetch tuple at given TID
 */
static bool
flux_tuple_fetch_row_version(Relation relation,
							 ItemPointer tid,
							 Snapshot snapshot,
							 TupleTableSlot *slot)
{
	BlockNumber blkno = ItemPointerGetBlockNumber(tid);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
	Buffer		buffer;
	Page		page;
	ItemId		itemid;
	FluxTupleHeader *tuple_hdr;
	bool		visible = false;

	/* Clear the slot */
	ExecClearTuple(slot);

	/* Read the page */
	buffer = ReadBuffer(relation, blkno);

	/*
	 * SnapshotAny is how the executor fetches the latest version of a row it
	 * is about to update or delete (ExecUpdate building the new tuple,
	 * RETURNING OLD, EvalPlanQual).  That version must never be an aborted
	 * transaction's not-yet-reverted image, or the new row would be built on
	 * top of it.  The writer reverts such versions itself before working on
	 * the row (flux_help_revert); do the same here, under an exclusive lock,
	 * so the row we hand back is the one the write will apply to.
	 */
	if (snapshot == SnapshotAny)
	{
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		if (flux_help_revert(relation, buffer, offnum))
			buffer = ReadBuffer(relation, blkno);
		else
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	}

	/*
	 * Excludes in-place writers while we decide and copy
	 * (FluxSlotStoreTuple).
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE_EXCLUSIVE);

	page = BufferGetPage(buffer);

	/* Validate offset */
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		UnlockReleaseBuffer(buffer);
		return false;
	}

	/* Get the item */
	itemid = PageGetItemId(page, offnum);

	if (!ItemIdIsNormal(itemid))
	{
		UnlockReleaseBuffer(buffer);
		return false;
	}

	/* Get tuple header */
	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

	/*
	 * Check visibility (heap-shaped xmin/xmax). Special case: SnapshotAny is
	 * used by DELETE RETURNING to fetch the just-deleted tuple, so we must
	 * allow deleted tuples in that case.
	 */
	if (snapshot && snapshot != SnapshotAny)
	{
		/* For normal snapshots, check visibility */
		visible = FluxTupleVisibleToSnapshotDual(tuple_hdr, snapshot,
												 RelationGetRelid(relation),
												 buffer, tid);

		/* SSI, as heap_fetch */
		flux_ssi_read(relation, tid, tuple_hdr, buffer, snapshot, visible,
					  visible);

		/*
		 * The on-page version is not visible.  An in-place update may have
		 * replaced the version the snapshot sees, as for an index fetch:
		 * reconstruct it from UNDO.  Heap's TID still names that version. Do
		 * NOT additionally check FLUX_TUPLE_DELETED here; the visibility
		 * function already decided whether the delete is visible.
		 */
		if (!visible)
		{
			bool		unlocked;

			if (flux_store_visible_older(relation, buffer, tid, tuple_hdr,
										 ItemIdGetLength(itemid), snapshot,
										 slot, &unlocked))
			{
				ReleaseBuffer(buffer);
				return true;
			}
			if (unlocked)
				ReleaseBuffer(buffer);
			else
				UnlockReleaseBuffer(buffer);
			return false;
		}
	}
	else if (snapshot == SnapshotAny)
	{
		/* SnapshotAny: fetch any tuple, even if deleted (for RETURNING) */
		visible = true;
	}
	else
	{
		/* No snapshot means fetch unconditionally if not deleted */
		if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
		{
			UnlockReleaseBuffer(buffer);
			return false;
		}
		visible = true;
	}

	if (!visible)
	{
		UnlockReleaseBuffer(buffer);
		return false;
	}

	/*
	 * Before-image substitution for committed in-place UPDATEs, mirroring the
	 * sequential-scan path.  An index fetch that lands on a tuple updated in
	 * place by a transaction not visible to our MVCC snapshot must serve the
	 * before-image, not the on-page (new) value -- otherwise an UPDATE driven
	 * by this fetch (e.g. UPDATE ... WHERE pk = const) would recompute on top
	 * of a concurrent committed update and silently lose it.  Serve from the
	 * UNDO log.  Visibility is ordinary heap-shaped xmin/xmax + CLOG +
	 * snapshot (XidInMVCCSnapshot in the callee): if the reader can see the
	 * updater the callee returns false and the on-page value is served.  The
	 * UNCOMMITTED test only saves the walk, as in flux_index_fetch_tuple.
	 */
	if ((tuple_hdr->t_flags & FLUX_TUPLE_UPDATED) &&
		!(tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED) &&
		snapshot != NULL && IsMVCCSnapshot(snapshot))
	{
		char	   *bi_data = NULL;
		int			bi_len = 0;

		/* Walked with the share-exclusive lock held, as in the bitmap path. */
		if (FluxReconstructVisibleVersion(relation, tid,
										  (const char *) tuple_hdr,
										  ItemIdGetLength(itemid),
										  snapshot,
										  &bi_data, &bi_len))
		{
			FluxTupleHeader *bi_tuple = (FluxTupleHeader *) bi_data;

			FluxSlotStoreTuple(slot, bi_tuple, bi_len, InvalidBuffer);
			pfree(bi_tuple);
			slot->tts_tableOid = RelationGetRelid(relation);
			slot->tts_tid = *tid;
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			return true;
		}
	}

	/*
	 * SnapshotAny, and another transaction that is still running has changed
	 * the row in place: serve the latest committed version instead, which is
	 * what heap returns (its TID still holds the version the running
	 * transaction superseded).  The executor fetches the row this way before
	 * it updates it and builds the new row on top of what it got.  If the
	 * other transaction aborts, the update applies that new row without
	 * fetching again, so it must not carry the aborted values, nor report
	 * them as RETURNING OLD.  If the other transaction commits, the update
	 * reports TM_Updated and the executor fetches again.  The latest snapshot
	 * sees every committed writer and none that is still running.
	 */
	if (snapshot == SnapshotAny && UndoRecPtrIsValid(tuple_hdr->t_verptr))
	{
		bool		is_insert;

		if (TransactionIdIsValid(flux_header_dirty_xid(tuple_hdr, &is_insert)) &&
			!is_insert)
		{
			Size		len = ItemIdGetLength(itemid);
			char	   *onpage = palloc(len);
			char	   *bi_data = NULL;
			int			bi_len = 0;

			/* Walk the chain from a stable copy, without the lock. */
			memcpy(onpage, tuple_hdr, len);
			UnlockReleaseBuffer(buffer);
			if (flux_reconstruct_latest(relation, tid, onpage, len,
										&bi_data, &bi_len))
			{
				FluxSlotStoreTuple(slot, (FluxTupleHeader *) bi_data, bi_len,
								   InvalidBuffer);
				pfree(bi_data);
			}
			else				/* it committed meanwhile: as on page */
				FluxSlotStoreTuple(slot, (FluxTupleHeader *) onpage, len,
								   InvalidBuffer);
			pfree(onpage);
			slot->tts_tableOid = RelationGetRelid(relation);
			slot->tts_tid = *tid;
			return true;
		}
	}

	/* Store tuple into slot with buffer pin for safe access */
	FluxSlotStoreTuple(slot, tuple_hdr,
					   ItemIdGetLength(itemid), buffer);
	slot->tts_tableOid = RelationGetRelid(relation);
	slot->tts_tid = *tid;

	/*
	 * FluxSlotStoreTuple copied the tuple.  This function's principal caller
	 * is the executor's "fetch the old tuple before updating it" step, which
	 * then updates that very tuple in place, so the slot must not alias the
	 * page; materializing also detaches its datums from the copy.
	 */
	ExecMaterializeSlot(slot);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);

	return true;
}

/*
 * Check if TID is valid for relation scan
 */
static bool
flux_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(scan->rs_rd);

	return ItemPointerIsValid(tid) &&
		ItemPointerGetBlockNumber(tid) < nblocks;
}

/*
 * Get latest version of tuple (for updates)
 */
static void
flux_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	/*
	 * FLUX updates in place, so the latest version of a row is at its TID,
	 * unless an UPDATE moved the row: follow FLUX_TUPLE_SUPERSEDED links
	 * whose updater committed or is ourselves.
	 */
	for (;;)
	{
		BlockNumber block = ItemPointerGetBlockNumber(tid);
		OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
		Buffer		buffer;
		Page		page;
		bool		follow = false;
		ItemPointerData next;

		if (block >= RelationGetNumberOfBlocks(scan->rs_rd))
			return;
		buffer = ReadBuffer(scan->rs_rd, block);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		if (offnum >= FirstOffsetNumber && offnum <= PageGetMaxOffsetNumber(page))
		{
			ItemId		itemid = PageGetItemId(page, offnum);

			if (ItemIdIsNormal(itemid))
			{
				FluxTupleHeader *tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);
				TransactionId xmax = FluxTupleGetXmax(tuple_hdr);

				follow = (tuple_hdr->t_flags & (FLUX_TUPLE_DELETED | FLUX_TUPLE_SUPERSEDED)) ==
					(FLUX_TUPLE_DELETED | FLUX_TUPLE_SUPERSEDED) &&
					(TransactionIdIsCurrentTransactionId(xmax) ||
					 TransactionIdDidCommit(xmax));
				next = tuple_hdr->t_ctid;
			}
		}
		UnlockReleaseBuffer(buffer);
		if (!follow || ItemPointerEquals(&next, tid))
			return;
		*tid = next;
	}
}

/*
 * Does a version of a row that a later version has replaced satisfy the
 * snapshot?  The version was written by (xmin, cmin), the writer that
 * replaced it by (sup_xid, sup_cid), heap's xmax and cmax of the version.
 * This is HeapTupleSatisfiesSelf/Dirty/MVCC for a tuple with that xmin and
 * xmax: a version replaced in place is still live to a snapshot that does
 * not see its replacement, exactly as heap's old version is.
 */
static bool
flux_superseded_version_satisfies(TransactionId xmin, CommandId cmin,
								  TransactionId sup_xid, CommandId sup_cid,
								  Snapshot snapshot)
{
	bool		mvcc = (snapshot->snapshot_type == SNAPSHOT_MVCC);
	bool		dirty = (snapshot->snapshot_type == SNAPSHOT_DIRTY);

	Assert(mvcc || dirty || snapshot->snapshot_type == SNAPSHOT_SELF);

	if (dirty)
	{
		snapshot->xmin = InvalidTransactionId;
		snapshot->xmax = InvalidTransactionId;
		snapshot->speculativeToken = 0;
	}

	/* The version's writer. */
	if (TransactionIdIsCurrentTransactionId(xmin))
	{
		if (mvcc && cmin >= snapshot->curcid)
			return false;		/* written after the scan started */
	}
	else if (mvcc)
	{
		if (!FluxWriterVisibleToSnapshot(xmin, snapshot, false))
			return false;
	}
	else if (TransactionIdIsInProgress(xmin))
	{
		if (!dirty)
			return false;
		snapshot->xmin = xmin;
		return true;			/* in insertion by other */
	}
	else if (!TransactionIdDidCommit(xmin))
		return false;			/* aborted or crashed */

	/* The writer that replaced it. */
	if (TransactionIdIsCurrentTransactionId(sup_xid))
		return mvcc && sup_cid >= snapshot->curcid; /* replaced after scan
													 * start */
	if (mvcc)
		return !FluxWriterVisibleToSnapshot(sup_xid, snapshot, false);
	if (TransactionIdIsInProgress(sup_xid))
	{
		if (dirty)
			snapshot->xmax = sup_xid;
		return true;
	}
	return !TransactionIdDidCommit(sup_xid);
}

/*
 * Check if tuple satisfies snapshot
 *
 * The version judged is the one in the slot.  FLUX updates in place, so the
 * slot's TID leads to the row's newest version, which may not be the one in
 * the slot: an AFTER trigger's queued image (am_inplace_update_keeps_tid) of
 * a row its own transaction rewrote since, as the RI triggers check a
 * deferred row.  A slot that knows its version's identity (FluxSlotGetVersion)
 * and finds another version on the page is judged as heap judges the old
 * version its TID still names: by its writer and by the writer that replaced
 * it, found by walking the row's version chain.
 */
static bool
flux_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							  Snapshot snapshot)
{
	TransactionId vxmin;
	CommandId	vcmin;
	Buffer		buffer;
	Page		page;
	ItemId		itemid;
	FluxTupleHeader *tuple_hdr;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		visible;

	Assert(TTS_IS_FLUXTUPLE(slot));

	/*
	 * Re-fetch the on-disk tuple header by TID so we can check the real
	 * commit timestamps and transaction status.  The previous implementation
	 * used flux_tuple_from_slot() which fabricated a new tuple with current
	 * timestamps, making the visibility check meaningless.
	 */
	if (!ItemPointerIsValid(&slot->tts_tid))
		return false;

	blkno = ItemPointerGetBlockNumber(&slot->tts_tid);
	offnum = ItemPointerGetOffsetNumber(&slot->tts_tid);

	buffer = ReadBuffer(rel, blkno);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);

	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		UnlockReleaseBuffer(buffer);
		return false;
	}

	itemid = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(itemid))
	{
		UnlockReleaseBuffer(buffer);
		return false;
	}

	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

	/* The slot holds a version the row has moved past: judge that one. */
	if ((snapshot->snapshot_type == SNAPSHOT_SELF ||
		 snapshot->snapshot_type == SNAPSHOT_DIRTY ||
		 snapshot->snapshot_type == SNAPSHOT_MVCC) &&
		FluxSlotGetVersion(slot, &vxmin, &vcmin) &&
		!(TransactionIdEquals(FluxTupleGetXmin(tuple_hdr), vxmin) &&
		  tuple_hdr->t_cmin == vcmin))
	{
		Size		len = ItemIdGetLength(itemid);
		char	   *onpage = palloc(len);
		TransactionId sup_xid;
		CommandId	sup_cid;
		bool		found;

		/* Walk the chain from a stable copy, without the lock. */
		memcpy(onpage, tuple_hdr, len);
		UnlockReleaseBuffer(buffer);
		found = FluxFindVersionSuperseder(rel, &slot->tts_tid, onpage, len,
										  vxmin, vcmin, &sup_xid, &sup_cid);
		pfree(onpage);

		/*
		 * Not in the chain: a version whose UNDO has been discarded was
		 * replaced by a writer every snapshot sees, or the version was an
		 * aborted writer's that has been reverted.  Either way it is dead.
		 */
		if (!found)
		{
			if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
			{
				snapshot->xmin = InvalidTransactionId;
				snapshot->xmax = InvalidTransactionId;
				snapshot->speculativeToken = 0;
			}
			return false;
		}
		return flux_superseded_version_satisfies(vxmin, vcmin,
												 sup_xid, sup_cid, snapshot);
	}

	/*
	 * The row's newest change is an aborted transaction's, with its revert
	 * still pending: for an existence check the row is there, at its
	 * committed version (see flux_index_fetch_tuple).
	 */
	if (!IsMVCCSnapshot(snapshot) && snapshot->snapshot_type != SNAPSHOT_ANY &&
		FluxTupleHasAbortedVersion(tuple_hdr))
	{
		if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
		{
			snapshot->xmin = InvalidTransactionId;
			snapshot->xmax = InvalidTransactionId;
			snapshot->speculativeToken = 0;
		}
		UnlockReleaseBuffer(buffer);
		return true;
	}

	if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
	{
		visible = flux_tuple_satisfies_dirty(tuple_hdr, snapshot, buffer);
		UnlockReleaseBuffer(buffer);
		return visible;
	}

	visible = FluxTupleVisibleToSnapshotDual(tuple_hdr, snapshot,
											 RelationGetRelid(rel), buffer,
											 &slot->tts_tid);

	UnlockReleaseBuffer(buffer);

	return visible;
}

/*
 * Speculative tuple insertion for FLUX
 * This is used for INSERT ... ON CONFLICT operations
 */
static void
flux_tuple_insert_speculative(Relation relation, TupleTableSlot *slot,
							  CommandId cid, uint32 options,
							  BulkInsertState bistate, uint32 specToken)
{
	FluxTuple	tuple;
	Buffer		buf;
	Page		page;
	Size		tuple_size;
	BlockNumber target_block;
	OffsetNumber offnum;
	FluxTupleHeader *tuple_hdr;
	uint64		commit_ts;
	FluxOverflowBuffers overflow_buffers;
	int			i;
	int			natts = RelationGetDescr(relation)->natts;
	Datum	   *toast_values;
	bool	   *toast_isnull;
	ToastAttrInfo *toast_attr;
	ToastTupleContext ttc;
	bool		toasted = false;
	FluxLogicalImage logical_img;
	Buffer		vmbuffer;

	slot_getallattrs(slot);

	/*
	 * TOAST the values exactly as flux_tuple_insert does: a value that
	 * arrives as an on-disk pointer into another relation's TOAST table is
	 * detoasted and stored in ours (or inline), and an oversized value is
	 * compressed or externalized.  If the insertion is later killed,
	 * flux_tuple_complete_speculative deletes these values again.
	 */
	toast_values = palloc_array(Datum, natts);
	toast_isnull = palloc_array(bool, natts);
	toast_attr = palloc_array(ToastAttrInfo, natts);
	memcpy(toast_values, slot->tts_values, natts * sizeof(Datum));
	memcpy(toast_isnull, slot->tts_isnull, natts * sizeof(bool));
	flux_toast_tuple(relation, toast_values, toast_isnull, NULL, NULL,
					 &ttc, toast_attr, &toasted, 0);

	overflow_buffers.count = 0;
	tuple = FluxFormTuple(RelationGetDescr(relation),
						  toast_values, toast_isnull,
						  relation, &overflow_buffers);
	tuple_size = tuple->t_len;

	/* The tuple holds copies of the values; release the temporaries. */
	if (toasted)
		flux_toast_cleanup(&ttc);
	pfree(toast_values);
	pfree(toast_isnull);
	pfree(toast_attr);

	/*
	 * A row whose untoastable part still exceeds a FLUX page cannot be
	 * stored; report it as flux_tuple_insert does (and heap's hio.c), rather
	 * than asking the FSM for an impossible amount of space.
	 */
	if (tuple_size > FLUX_MAX_TUPLE_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu, maximum size %zu",
						tuple_size, (Size) FLUX_MAX_TUPLE_SIZE)));

	/*
	 * Get timestamp BEFORE entering critical section, as this may allocate
	 * memory.
	 */
	commit_ts = FluxGetCommitTimestamp();

	/*
	 * Mark the tuple as uncommitted so that SNAPSHOT_DIRTY callers can detect
	 * in-progress insertions via the sLog.  The sLog entry is registered
	 * after the tuple is placed on the page (below) so that the TID is valid.
	 */
	tuple->t_data->t_flags |= FLUX_TUPLE_UNCOMMITTED;
	tuple->t_data->t_xmin = GetCurrentTransactionId();	/* subxid: heap-shaped,
														 * so savepoint rollback
														 * marks it aborted in
														 * CLOG */
	tuple->t_data->t_cmin = cid;	/* see flux_tuple_insert */

	/*
	 * Use the FSM to find a page with enough free space, or extend the
	 * relation with a properly initialized new page.
	 */
	target_block = FluxGetPageWithFreeSpace(relation, tuple_size);
	if (target_block == InvalidBlockNumber)
	{
		/* Clean up overflow buffers before throwing error */
		for (i = 0; i < overflow_buffers.count; i++)
		{
			UnlockReleaseBuffer(overflow_buffers.buffers[i].buffer);
			pfree(overflow_buffers.buffers[i].record_data);
		}
		FluxFreeTuple(tuple);
		elog(ERROR, "FLUX failed to allocate page for speculative insertion");
	}

	buf = ReadBuffer(relation, target_block);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	/* Verify page has sufficient space */
	if (PageGetHeapFreeSpace(page) < tuple_size)
	{
		/* FSM was stale, update and retry */
		FluxRecordFreeSpace(relation, target_block, PageGetHeapFreeSpace(page));
		UnlockReleaseBuffer(buf);

		target_block = FluxGetPageWithFreeSpace(relation, tuple_size);
		if (target_block == InvalidBlockNumber)
		{
			/* Clean up overflow buffers before throwing error */
			for (i = 0; i < overflow_buffers.count; i++)
			{
				UnlockReleaseBuffer(overflow_buffers.buffers[i].buffer);
				pfree(overflow_buffers.buffers[i].record_data);
			}
			FluxFreeTuple(tuple);
			elog(ERROR, "FLUX failed to allocate page for speculative insertion after retry");
		}

		buf = ReadBuffer(relation, target_block);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		if (PageGetHeapFreeSpace(page) < tuple_size)
		{
			/*
			 * Both FSM attempts returned stale pages.  Extend the relation
			 * as the final fallback to get a guaranteed-empty page.
			 * Update FSM for accuracy on the bad page.
			 */
			FluxRecordFreeSpace(relation, BufferGetBlockNumber(buf),
								PageGetHeapFreeSpace(page));
			UnlockReleaseBuffer(buf);

			/* Race-safe extension (see FluxExtendRelationBuffer). */
			buf = FluxExtendRelationBuffer(relation);
			page = BufferGetPage(buf);
		}
	}

	/*
	 * Try adding tuple before critical section.  If the page is too full (FSM
	 * was optimistic about alignment/line-pointer overhead), extend the
	 * relation and use a fresh page.
	 */
	offnum = PageAddItem(page, tuple->t_data, tuple_size,
						 InvalidOffsetNumber, false, true);
	if (offnum == InvalidOffsetNumber)
	{
		FluxRecordFreeSpace(relation, BufferGetBlockNumber(buf),
							PageGetHeapFreeSpace(page));
		UnlockReleaseBuffer(buf);

		buf = FluxExtendRelationBuffer(relation);
		page = BufferGetPage(buf);

		offnum = PageAddItem(page, tuple->t_data, tuple_size,
							 InvalidOffsetNumber, false, true);
		if (offnum == InvalidOffsetNumber)
			elog(ERROR, "failed to add FLUX tuple to fresh page during "
				 "speculative insert (tuple_size=%zu)", tuple_size);
	}

	/*
	 * Reserve WAL room for the overflow blocks and their header + data chunks
	 * outside the critical section, as the plain insert path does.
	 */
	if (RelationNeedsWAL(relation) && overflow_buffers.count > 0)
		XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID, 3 + MAX_OVERFLOW_BUFFERS * 2);

	FluxXLogPrepareLogicalImage(relation, tuple, &logical_img);

	vmbuffer = FluxVMPinForChange(relation, buf);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	/* Mark as speculative insertion */
	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, PageGetItemId(page, offnum));
	tuple_hdr->t_flags |= FLUX_TUPLE_SPECULATIVE;
	tuple_hdr->t_commit_ts = 0; /* heap-shaped: t_xmax = Invalid */

	/*
	 * Store the speculative insertion token in t_ctid, using the same
	 * encoding as heap: block number holds the token, offset is set to
	 * SpecTokenOffsetNumber so callers can distinguish a token from a real
	 * TID.
	 */
	ItemPointerSet(&tuple_hdr->t_ctid, specToken, SpecTokenOffsetNumber);

	/* Set TID in slot */
	ItemPointerSet(&slot->tts_tid, BufferGetBlockNumber(buf), offnum);

	/*
	 * The page now holds a tuple no snapshot sees yet, so it is not
	 * all-visible (or all-frozen) any more, as for any insert.
	 */
	FluxVMClearForChange(buf, vmbuffer);
	MarkBufferDirty(buf);

	/* WAL logging */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr;
		xl_flux_insert xlrec;
		xl_flux_overflow_write ovf_xlrecs[MAX_OVERFLOW_BUFFERS];
		int			ovf_block_ids[MAX_OVERFLOW_BUFFERS];
		int			ovf_next_block_id = 1;

		xlrec.offnum = offnum;
		xlrec.relid = RelationGetRelid(relation);
		xlrec.flags = FLUX_WAL_SPECULATIVE;
		if (logical_img.data != NULL)
			xlrec.flags |= FLUX_WAL_LOGICAL_TUPLE;
		xlrec.tuple_len = (uint32) tuple_size;
		xlrec.commit_ts = 0;	/* heap-shaped: t_xmax = Invalid */

		/*
		 * Force a full-page image and append the tuple body to the main data
		 * channel (matching FluxXLogInsert's layout, which flux_xlog_insert_
		 * redo parses).  The redo handler restores the tuple from the FPI and
		 * never reaches the PageAddItem path for these records; the body is
		 * carried so logical decoding and any future non-FPI replay see a
		 * well-formed record.  Registering the body as block data instead
		 * (the historical behavior) left tuple_len uninitialized and made
		 * redo read past the record on a BLK_NEEDS_REDO replay.
		 */
		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD | REGBUF_FORCE_IMAGE);
		XLogRegisterData((char *) &xlrec, sizeof(xl_flux_insert));
		XLogRegisterData((char *) tuple->t_data, tuple_size);
		FluxVMRegisterClear(&xlrec.flags);

		/*
		 * Register the overflow pages the way the plain insert path does,
		 * which is what insert redo parses: each page once (records can share
		 * a page), and each record's data preceded by its
		 * xl_flux_overflow_write header.
		 */
		for (i = 0; i < overflow_buffers.count; i++)
		{
			FluxOverflowBuffer *ovb = &overflow_buffers.buffers[i];
			int			j;

			Assert(ovb->buffer != buf);
			ovf_block_ids[i] = -1;
			for (j = 0; j < i && ovf_block_ids[i] < 0; j++)
			{
				if (overflow_buffers.buffers[j].buffer == ovb->buffer)
					ovf_block_ids[i] = ovf_block_ids[j];
			}
			if (ovf_block_ids[i] < 0)
			{
				ovf_block_ids[i] = ovf_next_block_id++;
				XLogRegisterBuffer(ovf_block_ids[i], ovb->buffer,
								   REGBUF_STANDARD | REGBUF_FORCE_IMAGE);
			}

			ovf_xlrecs[i].offnum = ovb->offset;
			ovf_xlrecs[i].flags = ovb->flags;
			ovf_xlrecs[i].data_len = ovb->record_len;
			ovf_xlrecs[i].commit_ts = commit_ts;
			XLogRegisterBufData(ovf_block_ids[i], (char *) &ovf_xlrecs[i],
								sizeof(xl_flux_overflow_write));
			XLogRegisterBufData(ovf_block_ids[i], ovb->record_data,
								ovb->record_len);
		}

		if (logical_img.data != NULL)
		{
			XLogRegisterData(logical_img.data, logical_img.len);
			XLogRegisterData((char *) &logical_img.len, sizeof(uint32));
		}

		recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_INSERT);
		PageSetLSN(page, recptr);
	}

	FluxVMFinishClear(RelationNeedsWAL(relation) ?
					  PageGetLSN(page) : InvalidXLogRecPtr);
	END_CRIT_SECTION();
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);
	FluxXLogReleaseLogicalImage(&logical_img);

	/*
	 * SNAPSHOT_DIRTY callers find the inserter in t_xmin and the token in
	 * t_ctid, as in heap.  Commit clears the UNCOMMITTED hint and savepoint
	 * rollback discards the row through the dirty-page set, as for a plain
	 * INSERT.
	 */
	FluxEnsureSLogCallbacks();
	FluxLocalDirtyPageMark(relation, BufferGetBlockNumber(buf));

	/* Update FSM with remaining free space */
	FluxRecordFreeSpace(relation, BufferGetBlockNumber(buf),
						PageGetHeapFreeSpace(page));

	UnlockReleaseBuffer(buf);

	/* Release overflow buffers, deduplicating shared buffers */
	for (i = 0; i < overflow_buffers.count; i++)
	{
		Buffer		ovf_buf = overflow_buffers.buffers[i].buffer;
		bool		already_released = (ovf_buf == buf);
		int			j;

		for (j = 0; j < i && !already_released; j++)
		{
			if (overflow_buffers.buffers[j].buffer == ovf_buf)
				already_released = true;
		}

		if (!already_released)
			UnlockReleaseBuffer(ovf_buf);
		pfree(overflow_buffers.buffers[i].record_data);
	}

	FluxFreeTuple(tuple);

	/* The slot now stands for the version just stored (AFTER triggers). */
	FluxSlotSetVersion(slot, GetCurrentTransactionId(), cid);
}

/*
 * Complete speculative insertion for FLUX
 */
static void
flux_tuple_complete_speculative(Relation relation, TupleTableSlot *slot,
								uint32 specToken, bool succeeded)
{
	ItemPointer tid = &slot->tts_tid;
	Buffer		buf;
	Page		page;
	ItemId		itemid;
	FluxTupleHeader *tuple_hdr;
	FluxTupleData killed = {0};

	buf = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	itemid = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	if (!ItemIdIsNormal(itemid))
	{
		UnlockReleaseBuffer(buf);
		return;
	}

	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

	if (succeeded)
	{
		/*
		 * Speculative insertion succeeded.  Clear the speculative flag and
		 * restore t_ctid to point to the tuple itself (removing the
		 * speculative token), mirroring heap_finish_speculative().
		 *
		 * Dirty-read hole fix: this used to ALSO clear FLUX_TUPLE_UNCOMMITTED
		 * here, i.e. well before the inserting transaction actually commits.
		 * That made the row immediately visible to every concurrent snapshot
		 * the instant the speculative insert was confirmed (INSERT ... ON
		 * CONFLICT DO NOTHING/UPDATE), a straight dirty read -- HEAP never
		 * exposes an uncommitted speculative tuple this way.  The speculative
		 * INSERT tracked the tuple as a plain INSERT does (see
		 * flux_tuple_insert_speculative), so leaving FLUX_TUPLE_UNCOMMITTED
		 * set here is safe and correct: commit clears the hint
		 * (flux_finish_page), exactly like a plain INSERT's.
		 */
		START_CRIT_SECTION();

		tuple_hdr->t_flags &= ~FLUX_TUPLE_SPECULATIVE;
		ItemPointerSet(&tuple_hdr->t_ctid,
					   ItemPointerGetBlockNumber(tid),
					   ItemPointerGetOffsetNumber(tid));

		MarkBufferDirty(buf);

		/* WAL-log the confirmation */
		if (RelationNeedsWAL(relation))
		{
			XLogRecPtr	recptr;
			xl_flux_insert xlrec;

			xlrec.relid = RelationGetRelid(relation);
			xlrec.offnum = ItemPointerGetOffsetNumber(tid);
			xlrec.flags = FLUX_WAL_SPEC_CONFIRM;
			xlrec.tuple_len = 0;	/* no body: confirm only flips flags */
			xlrec.commit_ts = tuple_hdr->t_commit_ts;

			/*
			 * Confirmation does not add a tuple; it clears the speculative
			 * flag on a tuple already present on the page.  Force a full-page
			 * image so redo restores the (already-updated) page rather than
			 * re-adding the tuple via PageAddItem.  tuple_len == 0 keeps the
			 * record body-less, which flux_xlog_insert_redo treats as
			 * "nothing to add" on the (unreachable, given the forced FPI)
			 * non-FPI path.
			 */
			XLogBeginInsert();
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD | REGBUF_FORCE_IMAGE);
			XLogRegisterData((char *) &xlrec, sizeof(xl_flux_insert));

			recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_INSERT);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();
	}
	else
	{
		TransactionId abort_xid;

		/*
		 * Speculative abort: mark the tuple deleted by the same xid that
		 * inserted it (the current subxid stamped in t_xmin), so the inserter
		 * and deleter commit or roll back together and the tuple is dead to
		 * every snapshot from here on.  The transaction itself goes on to
		 * retry the arbiter check, so this must not rely on an abort; the
		 * DIRTY-snapshot fetch recognises t_xmin == t_xmax as dead, and
		 * VACUUM reclaims it once the xid is committed and past the horizon.
		 */
		abort_xid = GetCurrentTransactionId();
		Assert(TransactionIdEquals(abort_xid, FluxTupleGetXmin(tuple_hdr)));

		START_CRIT_SECTION();

		tuple_hdr->t_flags |= FLUX_TUPLE_DELETED;
		FluxTupleSetXmax(tuple_hdr, abort_xid);

		MarkBufferDirty(buf);

		/* WAL log the speculative abort as a delete */
		if (RelationNeedsWAL(relation))
		{
			XLogRecPtr	recptr;
			xl_flux_delete xlrec;

			xlrec.offnum = ItemPointerGetOffsetNumber(tid);
			xlrec.flags = FLUX_WAL_SPECULATIVE;
			xlrec.tuple_len = ItemIdGetLength(itemid);
			xlrec.commit_ts = (uint64) abort_xid;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xl_flux_delete));
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
			XLogRegisterBufData(0, (char *) tuple_hdr, ItemIdGetLength(itemid));

			recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_DELETE);
			PageSetLSN(page, recptr);
		}

		END_CRIT_SECTION();

		/*
		 * The killed tuple's TOAST values were inserted for it alone; delete
		 * them now, as heap_abort_speculative does, once the page is
		 * unlocked.  Deform a private copy of the tuple.
		 */
		if (tuple_hdr->t_infomask & FLUX_INFOMASK_HASEXTERNAL)
		{
			killed.t_len = ItemIdGetLength(itemid);
			killed.t_data = palloc(killed.t_len);
			memcpy(killed.t_data, tuple_hdr, killed.t_len);
			killed.t_self = *tid;
		}
	}

	UnlockReleaseBuffer(buf);

	if (killed.t_data != NULL)
	{
		TupleDesc	tupdesc = RelationGetDescr(relation);
		Datum	   *values = palloc_array(Datum, tupdesc->natts);
		bool	   *isnull = palloc_array(bool, tupdesc->natts);

		FluxDeformTuple(relation, &killed, tupdesc, values, isnull);
		flux_toast_delete(relation, values, isnull, true);
		pfree(values);
		pfree(isnull);
		pfree(killed.t_data);
	}
}

/*
 * The last superseded row version flux_tuple_lock reported (traversed, or
 * TM_Updated to a READ COMMITTED caller without FIND_LAST_VERSION) or
 * locked.  See the "Lock succeeded" comment in flux_tuple_lock.
 */
static struct
{
	TransactionId top_xid;
	Oid			relid;
	ItemPointerData tid;
	CommandId	cid;
	TransactionId changed_xid;
}			flux_lock_reported;

static bool
flux_lock_was_reported(Relation relation, ItemPointer tid, CommandId cid,
					   TransactionId top_xid, TransactionId changed_xid)
{
	return TransactionIdEquals(flux_lock_reported.top_xid, top_xid) &&
		flux_lock_reported.relid == RelationGetRelid(relation) &&
		ItemPointerEquals(&flux_lock_reported.tid, tid) &&
		flux_lock_reported.cid == cid &&
		TransactionIdEquals(flux_lock_reported.changed_xid, changed_xid);
}

static void
flux_lock_set_reported(Relation relation, ItemPointer tid, CommandId cid,
					   TransactionId top_xid, TransactionId changed_xid)
{
	flux_lock_reported.top_xid = top_xid;
	flux_lock_reported.relid = RelationGetRelid(relation);
	flux_lock_reported.tid = *tid;
	flux_lock_reported.cid = cid;
	flux_lock_reported.changed_xid = changed_xid;
}

/*
 * NOWAIT met a conflicting locker or writer.  Heap raises this error inside
 * heap_lock_tuple; TM_WouldBlock means SKIP LOCKED to every caller
 * (nodeLockRows skips the row), so returning it here made NOWAIT behave like
 * SKIP LOCKED.
 */
pg_noreturn static void
flux_lock_nowait_error(Relation relation, Buffer buf, bool have_tuple_lock,
					   ItemPointer tid, LockTupleMode mode)
{
	UnlockReleaseBuffer(buf);
	if (have_tuple_lock)
		FluxUnlockTuple(relation, tid, mode);
	ereport(ERROR,
			(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
			 errmsg("could not obtain lock on row in relation \"%s\"",
					RelationGetRelationName(relation))));
}

/*
 * Lock a tuple in FLUX table: the version at tid, which flux_tuple_lock may
 * have reached by following a moved row
 */
static TM_Result
flux_tuple_lock_version(Relation relation, ItemPointer tid, Snapshot snapshot,
						TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
						LockWaitPolicy wait_policy, uint8 flags,
						TM_FailureData *tmfd, bool follow_update, ItemPointer next_tid)
{
	Buffer		buf;
	Page		page;
	ItemId		itemid;
	FluxTupleHeader *tuple_hdr;
	TM_Result	result = TM_Ok;
	TransactionId current_xid;
	bool		have_tuple_lock = false;
	char	   *keyintact_image = NULL;
	TransactionId changed_xid = InvalidTransactionId;

	/*
	 * Copy the target TID before touching the slot.  Callers pass the slot's
	 * own tts_tid as this argument (execReplication.c, ri_triggers.c), and
	 * populating the slot below clears it, which would invalidate *tid
	 * mid-function.  heapam_tuple_lock() copies into tuple->t_self for the
	 * same reason.
	 */
	ItemPointerData tidcopy = *tid;

	tid = &tidcopy;

	/*
	 * Get transaction XID BEFORE entering critical section, as this may
	 * allocate memory.
	 */
	current_xid = GetTopTransactionId();

	/*
	 * The sLog lock marker names this subtransaction's own xid as the one a
	 * conflicting waiter sleeps on, so that ROLLBACK TO wakes it, as heap's
	 * xmax does.  Assign it now, outside the buffer lock.
	 */
	(void) GetCurrentTransactionId();

reacquire:

	/*
	 * Any retry re-reads the header from scratch, so a before-image captured
	 * by the key-intact fast path on a previous pass is stale: drop it.  This
	 * is the single point that guarantees no skip decision (and no image) is
	 * ever carried across a buffer-lock gap.
	 */
	if (keyintact_image != NULL)
	{
		pfree(keyintact_image);
		keyintact_image = NULL;
	}
	changed_xid = InvalidTransactionId;
	ItemPointerSetInvalid(next_tid);

	buf = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	/* Helping: see flux_help_revert. */
	if (flux_help_revert(relation, buf, ItemPointerGetOffsetNumber(tid)))
		goto reacquire;

	itemid = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	if (!ItemIdIsNormal(itemid))
	{
		UnlockReleaseBuffer(buf);
		return TM_Invisible;
	}

	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

	/*
	 * Already modified by our own transaction in this or a later command.
	 * heap_lock_tuple gets this verdict straight from
	 * HeapTupleSatisfiesUpdate(); FLUX must supply it explicitly.  MERGE's
	 * ExecMergeMatched() relies on it: when a BEFORE trigger modifies the row
	 * being merged, the lock must report TM_SelfModified so the executor
	 * raises "tuple to be updated or deleted was already modified ...".
	 * Without it, ExecMergeMatched loops back to lmerge_matched forever,
	 * which showed up as a backend growing to 60 GB until the OOM killer
	 * fired.
	 *
	 * Note the FLUX_TUPLE_DELETED handling below must come after this, so our
	 * own in-progress delete is not misreported as a concurrent TM_Deleted.
	 */
	{
		CommandId	self_cmax;

		if (FluxCheckSelfModified(relation, tid, tuple_hdr, cid, &self_cmax))
		{
			if (tmfd)
			{
				tmfd->ctid = *tid;
				tmfd->xmax = (tuple_hdr->t_flags & FLUX_TUPLE_DELETED) ?
					FluxTupleGetXmax(tuple_hdr) : tuple_hdr->t_xmin;
				tmfd->cmax = self_cmax;
				tmfd->traversed = false;
			}

			/*
			 * A locker sees the current version, not the superseded one
			 * targeted by UPDATE/DELETE.  ON CONFLICT needs the same verdict
			 * as heap for a version created by this command.
			 */
			if (!(tuple_hdr->t_flags & FLUX_TUPLE_DELETED) &&
				self_cmax == cid && tuple_hdr->t_cmin >= cid)
			{
				result = TM_Invisible;
				goto out_unlock;
			}
			UnlockReleaseBuffer(buf);
			return TM_SelfModified;
		}
	}

	/*
	 * Tuple is delete-marked.  FLUX_TUPLE_DELETED alone does NOT mean the row
	 * is gone -- it means *someone* stamped a delete, and that delete may
	 * still be in progress.  Reporting TM_Deleted straight off the flag made
	 * a locker treat an UNCOMMITTED delete as final: SELECT ... FOR SHARE /
	 * FOR KEY SHARE against a row with a pending DELETE returned the EMPTY
	 * set immediately, where heap blocks until the deleter commits or aborts
	 * (heap_delete stamps xmax with LockTupleExclusive, so heap_lock_tuple
	 * takes its ordinary xmax wait).  If the deleter then ROLLS BACK that is
	 * a WRONG RESULT -- the row is still live and lockable -- and for an FK
	 * parent probe (ri_LockPKTuple, LockTupleKeyShare) it is an integrity
	 * hole: the FK child is admitted against a parent row the locker was told
	 * does not exist.
	 *
	 * So resolve the deleter before answering, mirroring what
	 * flux_tuple_delete already does for a concurrent delete: wait out an
	 * in-progress non-self deleter, then jump back to 'reacquire' and decide
	 * on the re-read, post-wait header.  An aborted delete has had its
	 * before-image restored by UNDO (FLUX_TUPLE_DELETED cleared), so the
	 * retry finds a live row and locks it normally.  A committed delete, and
	 * our own delete, still report TM_Deleted.
	 *
	 * This check sits after the TM_SelfModified handling above so our own
	 * in-progress delete is never misreported as a concurrent one.
	 */
	if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
	{
		TransactionId del_xid;
		bool		del_is_insert = false;

		/* A non-key move is compatible, but its successors need locks too. */
		if (mode == LockTupleKeyShare &&
			(tuple_hdr->t_flags & FLUX_TUPLE_SUPERSEDED) &&
			(tuple_hdr->t_flags & FLUX_TUPLE_KEY_INTACT))
		{
			int			image_len;
			FluxSkippedUpdates skipped;

			bool		have_image;

			have_image = FluxReconstructVisibleVersionEx(relation, tid,
														 (const char *) tuple_hdr,
														 ItemIdGetLength(itemid), snapshot,
														 &keyintact_image, &image_len, &skipped);
			if ((follow_update || have_image) && skipped.complete &&
				!TransactionIdIsValid(skipped.committed_key_xid) &&
				!TransactionIdIsValid(skipped.inprogress_key_xid))
			{
				*next_tid = tuple_hdr->t_ctid;
				goto lock_tuple;
			}
			if (follow_update &&
				TransactionIdIsValid(skipped.committed_key_xid))
			{
				tmfd->ctid = *tid;
				tmfd->xmax = skipped.committed_key_xid;
				tmfd->cmax = InvalidCommandId;
				tmfd->traversed = false;
				result = TM_Updated;
				goto out_unlock;
			}

			if (keyintact_image != NULL)
			{
				pfree(keyintact_image);
				keyintact_image = NULL;
			}
		}

		del_xid = flux_header_dirty_xid(tuple_hdr, &del_is_insert);

		if (TransactionIdIsValid(del_xid) && !del_is_insert)
		{
			/*
			 * In-progress DELETE by another transaction.  Honour the caller's
			 * wait policy exactly as the writer-wait branch below does, so
			 * NOWAIT/SKIP LOCKED keep their semantics.
			 */
			if (wait_policy == LockWaitBlock)
			{
				TransactionId wait_xid = del_xid;

				UnlockReleaseBuffer(buf);
				FluxLockTupleToWait(relation, tid, mode, &have_tuple_lock);
				XactLockTableWait(wait_xid, relation, tid, XLTW_Lock);
				goto reacquire;
			}
			else if (wait_policy == LockWaitError)
				flux_lock_nowait_error(relation, buf, have_tuple_lock, tid,
									   mode);
			else				/* LockWaitSkip */
			{
				result = TM_WouldBlock;
				goto out_unlock;
			}
		}

		/*
		 * The deleter moved the row to another partition: report it as heap
		 * does, whose t_ctid then holds the moved-partitions marker.
		 * heapam_tuple_lock raises the error itself when asked to follow the
		 * row to its latest version.
		 */
		if (FluxTupleMovedPartition(tuple_hdr))
		{
			if (flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION)
			{
				UnlockReleaseBuffer(buf);
				if (have_tuple_lock)
					FluxUnlockTuple(relation, tid, mode);
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("tuple to be locked was already moved to another partition due to concurrent update")));
			}
			if (tmfd)
			{
				ItemPointerSetMovedPartitions(&tmfd->ctid);
				tmfd->xmax = FluxTupleGetXmax(tuple_hdr);
				tmfd->cmax = InvalidCommandId;
				tmfd->traversed = false;
			}
			result = TM_Updated;
			goto out_unlock;
		}

		/*
		 * The deleter moved the row to a new TID: report it as heap does,
		 * with the new TID, which flux_tuple_lock follows when asked for the
		 * latest version.
		 */
		if (FluxTupleSuperseded(tuple_hdr))
		{
			if (tmfd)
			{
				tmfd->ctid = tuple_hdr->t_ctid;
				tmfd->xmax = FluxTupleGetXmax(tuple_hdr);
				tmfd->cmax = InvalidCommandId;
				tmfd->traversed = false;
			}
			result = TM_Updated;
			goto out_unlock;
		}

		/* Deleter is committed, aborted, or ourselves: the mark stands. */
		if (tmfd)
		{
			tmfd->ctid = *tid;
			tmfd->xmax = InvalidTransactionId;
			tmfd->cmax = InvalidCommandId;
		}
		result = TM_Deleted;
		goto out_unlock;
	}

	/*
	 * A forward-linked version belongs to the row we already locked.  Its
	 * insertion need not be visible, but every later key change conflicts.
	 * Walk its in-place history even when the snapshot predates insertion.
	 */
	if (follow_update)
	{
		char	   *image = NULL;
		int			image_len;
		FluxSkippedUpdates skipped;
		TransactionId conflict;

		Assert(mode == LockTupleKeyShare);
		if (TransactionIdDidAbort(FluxTupleGetXmin(tuple_hdr)))
			goto out_unlock;
		(void) FluxReconstructVisibleVersionEx(relation, tid,
											   (const char *) tuple_hdr, ItemIdGetLength(itemid), snapshot,
											   &image, &image_len, &skipped);
		if (image != NULL)
			pfree(image);
		conflict = skipped.committed_key_xid;
		if (TransactionIdIsValid(conflict))
		{
			tmfd->ctid = *tid;
			tmfd->xmax = conflict;
			tmfd->cmax = InvalidCommandId;
			tmfd->traversed = false;
			result = TM_Updated;
			goto out_unlock;
		}
		if (skipped.complete &&
			!TransactionIdIsValid(skipped.inprogress_key_xid))
			goto lock_tuple;
	}

	/*
	 * Check visibility using timestamp-based MVCC and handle concurrent
	 * modifications.  Same pattern as UPDATE/DELETE: distinguish truly
	 * invisible tuples from concurrent modifications.
	 *
	 * A caller may pass no snapshot: GetTupleForTrigger passes
	 * estate->es_snapshot, which the logical replication apply worker never
	 * sets, so every replicated UPDATE or DELETE on a table with an ENABLE
	 * ALWAYS BEFORE trigger arrives here with NULL.  Heap never uses the
	 * snapshot to lock, so it locks the latest version.  Do the same: test
	 * the on-page version with FluxTupleSatisfiesLatest instead, which sends
	 * an in-progress writer to the wait below and an uncommitted or
	 * later-command version to TM_Invisible.  There is no version the caller
	 * saw for a newer one to supersede, so nothing is reported as changed
	 * (changed_xid stays invalid): no TM_Updated, no traversed, and so no
	 * serialization failure.
	 */
	if (snapshot == NULL ?
		!FluxTupleSatisfiesLatest(tuple_hdr, RelationGetRelid(relation), cid,
								  buf, tid) :
		!FluxTupleVisibleToSnapshotDual(tuple_hdr, snapshot,
										RelationGetRelid(relation),
										buf, tid))
	{
		TransactionId dirty_xid;
		bool		is_insert_entry;

		/*
		 * Writer-only probe: this branch waits on an in-progress
		 * INSERT/UPDATE/DELETE writer.  Row locks are handled by the
		 * dedicated lock-conflict check below (SLogTupleLockConflictXid),
		 * which serializes lockers via the heavyweight LOCKTAG_TUPLE lock.
		 * Waiting-as-reader on a pure locker's xid here lets two lockers each
		 * XactLockTableWait on the other and deadlock.
		 *
		 * The writer's xid is read from the on-page header (t_xmin inserter /
		 * in-place updater, t_xmax deleter), as the UPDATE and DELETE paths
		 * do.  The header stays authoritative through the writer's PRE_COMMIT
		 * window (UNCOMMITTED hint cleared before the CLOG write).
		 */
		dirty_xid = flux_header_dirty_xid(tuple_hdr, &is_insert_entry);

		if (TransactionIdIsValid(dirty_xid) && is_insert_entry)
		{
			/* Another txn's in-progress INSERT - truly invisible */
			if (tmfd)
			{
				tmfd->ctid = *tid;
				tmfd->xmax = dirty_xid;
				tmfd->cmax = InvalidCommandId;
			}
			result = TM_Invisible;
			goto out_unlock;
		}

		/*
		 * KEY-INTACT FAST PATH -- heap's require_sleep = false for
		 * LockTupleKeyShare when HEAP_KEYS_UPDATED is clear (heapam.c, "If
		 * we're requesting KeyShare, and there's no update present, we don't
		 * need to wait.  Even if there is an update, we can still continue if
		 * the key hasn't been modified.").
		 *
		 * Without this, an FK check's parent-row probe (ri_LockPKTuple ->
		 * table_tuple_lock with LockTupleKeyShare) waited on ANY in-progress
		 * in-place updater, because the branch below reads the writer's xid
		 * from the on-page header and waits unconditionally, never looking at
		 * the requested LockTupleMode.  Two concurrent FK inserts referencing
		 * the same parent row then each waited on the other's xid with no
		 * heavyweight lock cycle for the deadlock detector to break, so the
		 * backend hung forever (isolation test fk-deadlock).  Heap treats
		 * KeyShare vs a non-key UPDATE as COMPATIBLE and never waits.
		 *
		 * LOCK ORDERING (this is what makes the skip safe):
		 *
		 * The inputs to the decision -- the on-page image, whose
		 * FLUX_TUPLE_KEYS_UPDATED and xid describe the in-progress update,
		 * and the version chain behind it -- are read while this function
		 * holds the tuple's buffer content lock BUFFER_LOCK_EXCLUSIVE, taken
		 * at the 'reacquire' label above and never released anywhere between
		 * that LockBuffer() and this point.  Every path that stamps an
		 * in-place image holds a conflicting buffer content lock while doing
		 * so (the regular update path takes BUFFER_LOCK_EXCLUSIVE, the CAS
		 * fast path BUFFER_LOCK_SHARE_EXCLUSIVE), so no concurrent
		 * key-changing update can set the bit between our read of the flag
		 * and our read of the xid -- we observe one self-consistent header.
		 * Any path that does release and re-take the buffer lock jumps back
		 * to 'reacquire', which re-reads the header from scratch, so a
		 * decision is never carried across a lock gap.  This mirrors heap,
		 * which re-checks the tuple after re-grabbing the buffer lock and
		 * only then clears require_sleep.
		 *
		 * We additionally require that the pre-update image actually be
		 * reconstructible from the UNDO log BEFORE committing to the skip.
		 * The on-page bytes here are the updater's uncommitted new image
		 * (this whole branch is the "not visible to our snapshot" case), so
		 * returning them in the caller's slot would leak uncommitted data. If
		 * the visible before-image cannot be produced we do NOT skip and fall
		 * through to the ordinary wait, which is always correct.
		 *
		 * EVERY UPDATE SINCE THE SNAPSHOT counts, not only the in-progress
		 * one on the page.  Heap tests the caller's version and then, in
		 * heap_lock_updated_tuple_rec, each later version in turn, oldest
		 * first: a committed update that conflicts with the requested mode
		 * (for KeyShare, one that changed the key; for the other modes, any)
		 * fails the lock at once, without waiting for the in-progress update
		 * at the end of the chain; an in-progress key update makes KeyShare
		 * wait.  The updates in between survive only in the UNDO chain, so
		 * walk it back to the snapshot's version
		 * (FluxReconstructVisibleVersionEx) and classify each one by the
		 * FLUX_TUPLE_KEYS_UPDATED bit of the image it produced.  The fail-at-
		 * once case only matters to a caller that is told TM_Updated (a
		 * transaction snapshot without FIND_LAST_VERSION, see below); the
		 * others follow the chain to the latest version, which heap does by
		 * waiting for its in-progress writer, as we do below.  If no version
		 * is visible to the snapshot (the row was inserted after it), the
		 * caller did not find this row through its snapshot and heap would be
		 * locking the latest version directly: there is no older version to
		 * conflict with, so wait as before.
		 */
		if (TransactionIdIsValid(dirty_xid) && !is_insert_entry &&
			snapshot != NULL && IsMVCCSnapshot(snapshot) &&
			(mode == LockTupleKeyShare ||
			 (!(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) &&
			  IsolationUsesXactSnapshot())))
		{
			char	   *bi_data = NULL;
			int			bi_len = 0;
			FluxSkippedUpdates skipped;
			bool		have_image;
			TransactionId conflict_xid;

			have_image = FluxReconstructVisibleVersionEx(relation, tid,
														 (const char *) tuple_hdr,
														 ItemIdGetLength(itemid),
														 snapshot,
														 &bi_data, &bi_len,
														 &skipped);
			conflict_xid = (mode == LockTupleKeyShare) ?
				skipped.committed_key_xid : skipped.committed_xid;

			if (have_image && TransactionIdIsValid(conflict_xid) &&
				!(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) &&
				IsolationUsesXactSnapshot())
			{
				pfree(bi_data);
				if (tmfd)
				{
					tmfd->ctid = *tid;
					tmfd->xmax = conflict_xid;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				result = TM_Updated;
				goto out_unlock;
			}

			if (mode == LockTupleKeyShare && have_image && skipped.complete &&
				!TransactionIdIsValid(skipped.committed_key_xid) &&
				!TransactionIdIsValid(skipped.inprogress_key_xid))
			{
				/*
				 * Key is intact and we hold the committed pre-update image:
				 * grant the KeyShare lock without waiting, and serve that
				 * image (never the updater's uncommitted bytes) to the
				 * caller.  bi_data is palloc'd in the caller's context, so it
				 * outlives the buffer we are about to release.
				 */
				keyintact_image = bi_data;

				if (tmfd)
				{
					tmfd->ctid = *tid;
					tmfd->xmax = InvalidTransactionId;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				goto lock_tuple;
			}

			if (have_image)
				pfree(bi_data);
		}

		if (TransactionIdIsValid(dirty_xid) && !is_insert_entry)
		{
			/* Another txn's in-progress UPDATE/DELETE - wait and retry */
			if (wait_policy == LockWaitBlock)
			{
				TransactionId wait_xid = dirty_xid;

				UnlockReleaseBuffer(buf);
				FluxLockTupleToWait(relation, tid, mode, &have_tuple_lock);
				XactLockTableWait(wait_xid, relation, tid, XLTW_Lock);
				goto reacquire;
			}
			else if (wait_policy == LockWaitError)
				flux_lock_nowait_error(relation, buf, have_tuple_lock, tid,
									   mode);
			else				/* LockWaitSkip */
			{
				result = TM_WouldBlock;
				goto out_unlock;
			}
		}

		/*
		 * No in-progress writer.  The on-page inserter/updater (t_xmin) is
		 * therefore either committed, aborted, or our own transaction.
		 * Resolve it the way HeapTupleSatisfiesUpdate does -- against CLOG
		 * and the current transaction, NOT against the caller's snapshot.  A
		 * row whose inserter or in-place updater committed after this
		 * statement's snapshot was taken is invisible to the snapshot yet
		 * perfectly lockable; FLUX keeps the newest version at the same TID,
		 * so the on-page tuple is already "the latest version" heap would
		 * chase via t_ctid.  Reporting TM_Updated here instead sent ON
		 * CONFLICT DO UPDATE (ExecOnConflictLockRow, flags == 0) back to its
		 * arbiter scan, which found the same committed row, re-locked it
		 * under the same snapshot, got TM_Updated again, and so on forever.
		 *
		 * An in-progress or aborted inserter is TM_Invisible, as is our own
		 * inserter when the MVCC check rejected it (later command in this
		 * transaction, or a rolled-back savepoint).
		 */
		{
			TransactionId xmin = FluxTupleGetXmin(tuple_hdr);

			if (TransactionIdIsCurrentTransactionId(xmin) ||
				!TransactionIdDidCommit(xmin))
			{
				if (tmfd)
				{
					tmfd->ctid = *tid;
					tmfd->xmax = xmin;
					tmfd->cmax = InvalidCommandId;
				}
				result = TM_Invisible;
				goto out_unlock;
			}
		}

		/*
		 * The on-page version was written by a transaction that committed
		 * after our snapshot was taken: the version the caller saw has been
		 * superseded.  Heap reaches this state as TM_Updated on the old
		 * version (its xmax is the committed updater) and tells the caller
		 * so: heapam_tuple_lock either follows the chain and reports
		 * traversed (FIND_LAST_VERSION), or returns TM_Updated.  FLUX must
		 * report the same, or READ COMMITTED callers skip their EvalPlanQual
		 * recheck (SELECT ... WHERE v = 1 FOR UPDATE returned a row with v =
		 * 2, a BEFORE-trigger UPDATE overwrote the concurrent update, an FK
		 * check accepted a parent whose key had changed) and REPEATABLE READ
		 * loses its serialization failure.
		 *
		 * As in heap, FOR KEY SHARE is compatible with updates that left the
		 * key intact: heap locks the old version and reports nothing.  Serve
		 * the snapshot's version of the row in that case, as heap does.
		 *
		 * That takes every update committed since the snapshot leaving the
		 * key intact, not only the latest one, whose FLUX_TUPLE_KEYS_UPDATED
		 * is the bit on the page: heap_lock_updated_tuple_rec tests each
		 * version of the chain and fails on any committed key update, even
		 * one the next update changed back.  So walk the UNDO chain back to
		 * the snapshot's version and test the bit of every image on the way
		 * (FluxReconstructVisibleVersionEx).  If the walk cannot reach the
		 * snapshot's version the history is unknown: report the change, which
		 * is never wrong for a recheck and at worst a serialization failure
		 * heap would not have raised.
		 */
		if (snapshot == NULL)
		{
			/*
			 * No snapshot (see above): the writer committed while we looked,
			 * and what it wrote is the latest version, which we lock.
			 */
		}
		else if (!UndoRecPtrIsValid(FluxTupleGetVersionPtr(tuple_hdr)))
		{
			/*
			 * No version chain: t_xmin is the inserter, not an updater (an
			 * INSERT leaves t_verptr invalid; FLUX_TUPLE_UPDATED is not used
			 * because it is a hint that may already be cleared).  A row
			 * inserted after our snapshot is locked by heap with TM_Ok and no
			 * traversal, and so do we.
			 */
		}
		else if (mode == LockTupleKeyShare &&
				 !(tuple_hdr->t_flags & FLUX_TUPLE_KEYS_UPDATED))
		{
			char	   *bi_data = NULL;
			int			bi_len = 0;
			FluxSkippedUpdates skipped;
			bool		have_image;

			if (snapshot != NULL && IsMVCCSnapshot(snapshot))
			{
				have_image = FluxReconstructVisibleVersionEx(relation, tid,
															 (const char *) tuple_hdr,
															 ItemIdGetLength(itemid),
															 snapshot,
															 &bi_data, &bi_len,
															 &skipped);
				if (have_image && skipped.complete &&
					!TransactionIdIsValid(skipped.committed_key_xid))
					keyintact_image = bi_data;
				else
				{
					if (have_image)
						pfree(bi_data);

					/*
					 * A complete walk that found no version visible to the
					 * snapshot means the row was inserted after it: the
					 * caller found the latest version directly, so lock it
					 * and report nothing, as heap would.
					 */
					if (have_image || !skipped.complete)
						changed_xid = FluxTupleGetXmin(tuple_hdr);
				}
			}
		}
		else
			changed_xid = FluxTupleGetXmin(tuple_hdr);

		/*
		 * Without FIND_LAST_VERSION heap returns TM_Updated.  Under a
		 * transaction snapshot every caller turns that into "could not
		 * serialize access due to concurrent update".  The READ COMMITTED
		 * callers without the flag (ON CONFLICT, ExecOnConflictLockRow; the
		 * logical replication apply worker, execReplication.c) go back to
		 * their index search, which looks for the row by key: heap's finds
		 * the new version at its new TID and locks it, or, if the key
		 * changed, does not find the row at all (ON CONFLICT then inserts).
		 * FLUX returns TM_Updated as well, so the search decides.  When it
		 * finds this TID again the snapshot is the same, so report a given
		 * version once per command and then lock it, which is the row heap's
		 * retry would lock; reporting it every time would loop forever.  The
		 * "Lock succeeded" block records it again, so a second lock of the
		 * same version in the command (GetTupleForTrigger under ON CONFLICT)
		 * reports nothing either.
		 */
		if (TransactionIdIsValid(changed_xid) &&
			!(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) &&
			(IsolationUsesXactSnapshot() ||
			 !flux_lock_was_reported(relation, tid, cid, current_xid,
									 changed_xid)))
		{
			flux_lock_set_reported(relation, tid, cid, current_xid,
								   changed_xid);
			if (tmfd)
			{
				tmfd->ctid = *tid;
				tmfd->xmax = changed_xid;
				tmfd->cmax = InvalidCommandId;
				tmfd->traversed = false;
			}
			result = TM_Updated;
			goto out_unlock;
		}
		/* Fall through to lock the current (latest) version */
	}

lock_tuple:

	/*
	 * Check for lock conflicts using the sLog.  The sLog tracks all
	 * in-progress lock/delete/update operations, replacing the old
	 * t_xmax/MultiXact-based scheme.
	 *
	 * Pass the precise LockTupleMode so the real heavyweight matrix decides:
	 * a KeyShare FK probe must stay compatible with an in-progress
	 * NoKeyExclusive UPDATE, as it is in heap.
	 */
	{
		TransactionId xwait;

		xwait = SLogTupleLockConflictXid(RelationGetRelid(relation),
										 relation->rd_locator.relNumber, tid,
										 mode);
		if (TransactionIdIsValid(xwait))
		{
			/* There's a conflict - check wait policy */
			if (wait_policy == LockWaitError)
				flux_lock_nowait_error(relation, buf, have_tuple_lock, tid,
									   mode);
			else if (wait_policy == LockWaitSkip)
			{
				result = TM_WouldBlock;
				goto out_unlock;
			}
			else				/* LockWaitBlock */
			{
				/*
				 * Wait on the specific conflicting transaction identified by
				 * SLogTupleLockConflictXid -- never on an arbitrary
				 * in-progress peer, which could be a compatible locker queued
				 * behind us and would form a spurious mutual-wait cycle.
				 */

				/* Release buffer and wait */
				UnlockReleaseBuffer(buf);

				if (TransactionIdIsValid(xwait))
				{
					/* Acquire tuple-level lock to wait */
					FluxLockTupleToWait(relation, tid, mode,
										&have_tuple_lock);

					/* Wait for the conflicting transaction */
					XactLockTableWait(xwait, relation, tid, XLTW_Lock);
				}

				/* Re-acquire buffer and retry */
				goto reacquire;
			}
		}
	}

	/*
	 * Lock succeeded.
	 *
	 * tmfd->traversed tells a FIND_LAST_VERSION caller that the row it saw
	 * was superseded and that the slot now holds the latest version, which it
	 * must recheck (EvalPlanQual, FK key recheck).  Heap sets it after
	 * following the update chain to a new TID; FLUX keeps the latest version
	 * at the same TID, so it sets it when the version on the page committed
	 * after the caller's snapshot (changed_xid, above).
	 *
	 * Heap moves the caller to the new TID (heapam_tuple_lock overwrites
	 * *tid).  The executor's modify-table paths keep that TID and lock it
	 * again later in the same command: MERGE with a BEFORE ROW trigger gets
	 * TM_Updated from GetTupleForTrigger, re-locks and rechecks in
	 * ExecMergeMatched, and retries the action, whose trigger locks the row
	 * once more.  In heap that last lock names the latest version and reports
	 * nothing.  FLUX's TID and snapshot are unchanged, so it would report the
	 * change again and MERGE would loop forever; reporting true on every
	 * FIND_LAST_VERSION lock was once the cause of exactly that loop (a
	 * backend growing until the OOM killer fired).  So for those callers
	 * report a given superseded version once per command, like the
	 * once-per-command EvalPlanQual bounce in flux_tuple_update_inplace
	 * (FluxEpqReconcileMatches).  No later update can slip in between the two
	 * locks: the lock we hold conflicts with it.
	 *
	 * A lock without FIND_LAST_VERSION that takes the latest version (ON
	 * CONFLICT under READ COMMITTED, once its TM_Updated above has been
	 * reported) records it as well.  ON CONFLICT then locks the row again,
	 * from GetTupleForTrigger when the table has a BEFORE UPDATE trigger.
	 * Heap reports nothing to that second lock: its TID is the latest
	 * version, which we already hold locked.  ON CONFLICT sets up no
	 * EvalPlanQual, so reporting traversed there sent GetTupleForTrigger into
	 * EvalPlanQual with no plan, and the backend crashed in
	 * ExecGetUpdateNewTuple.
	 *
	 * Row-locking callers (nodeLockRows, ri_LockPKTuple) take every TID from
	 * a scan of the caller's snapshot and never re-lock a TID they were
	 * handed back, so heap reports the change to each of their locks: a FOR
	 * UPDATE join that repeats a row rechecks every repetition.  They are the
	 * callers that pass TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS, and the
	 * modify-table ones never do, so that flag selects which rule applies.
	 */
	if (tmfd)
	{
		bool		relocks_own_tid =
			(flags & TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS) == 0;

		tmfd->traversed = false;
		if (TransactionIdIsValid(changed_xid))
		{
			if ((flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) &&
				!(relocks_own_tid &&
				  flux_lock_was_reported(relation, tid, cid, current_xid,
										 changed_xid)))
				tmfd->traversed = true;
			flux_lock_set_reported(relation, tid, cid, current_xid,
								   changed_xid);
		}
		tmfd->ctid = *tid;
		tmfd->xmax = InvalidTransactionId;
		tmfd->cmax = InvalidCommandId;
	}

	/*
	 * Record the lock holder before changing the page: running out of shared
	 * memory ERRORs here with nothing changed.  It is registered under the
	 * buffer lock, as every conflict probe runs, so a concurrent writer sees
	 * either the lock or nothing (heap stamps xmax under the buffer lock).
	 * The precise mode lets a probe apply the heavyweight conflict matrix:
	 * FOR KEY SHARE stays compatible with a NoKeyExclusive UPDATE.
	 */
	if (result == TM_Ok)
	{
		FluxEnsureSLogCallbacks();
		SLogTupleLockAcquire(RelationGetRelid(relation),
							 relation->rd_locator.relNumber, tid, mode);
	}

	tuple_hdr->t_flags |= FLUX_TUPLE_LOCKED;

	START_CRIT_SECTION();

	MarkBufferDirty(buf);

	/* Log the lock operation */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr;
		xl_flux_lock xlrec;

		xlrec.offnum = ItemPointerGetOffsetNumber(tid);
		xlrec.flags = 0;
		xlrec.infomask = tuple_hdr->t_infomask;
		xlrec.lock_mode = (uint8) mode;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xl_flux_lock));
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);

		recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_LOCK);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/*
	 * Populate the slot with the locked tuple's data.  This must happen after
	 * END_CRIT_SECTION (since overflow fetch may do I/O and ereport). We must
	 * unlock the buffer (but keep the pin) before calling
	 * FluxTupleToSlotWithOverflow because it may fetch overflow data, and if
	 * that overflow is on the same page, it would try to lock an
	 * already-locked buffer causing an assertion failure.
	 *
	 * FK constraint triggers and other callers of table_tuple_lock() expect
	 * the slot to contain valid tuple data.
	 */
	if (slot && result == TM_Ok)
	{
		FluxTupleHeader *locked_copy = NULL;

		/*
		 * Copy the locked version while we still hold the content lock.  A
		 * pin does not keep the bytes in place: the UPDATE path that makes
		 * room for a growing row compacts the page under an exclusive lock
		 * alone, moving the other tuples on it, and an in-place UPDATE by a
		 * transaction that does not conflict with our lock mode rewrites the
		 * row itself.  Decoding from the page after unlocking could read a
		 * torn or moved tuple.
		 */
		if (keyintact_image == NULL)
		{
			Size		locked_len = ItemIdGetLength(itemid);

			locked_copy = (FluxTupleHeader *) palloc(locked_len);
			memcpy(locked_copy, tuple_hdr, locked_len);
		}

		/*
		 * Unlock buffer but keep pin for slot materialization.  The row lock
		 * was already recorded under the buffer lock above, so nothing to
		 * register here.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		/*
		 * Key-intact fast path: the on-page bytes belong to a still
		 * in-progress updater and are invisible to our snapshot, so convert
		 * the committed pre-update image reconstructed above instead.  The
		 * caller's slot is an arbitrary ops (ri_LockPKTuple passes a plain
		 * heap/virtual slot), so it must go through the same conversion +
		 * ExecMaterializeSlot path as the on-page case below -- which also
		 * deep-copies it, leaving keyintact_image ours to free.
		 */
		if (!FluxTupleToSlotWithOverflow(keyintact_image != NULL ?
										 (FluxTupleHeader *) keyintact_image :
										 locked_copy, slot, relation))
		{
			/*
			 * Conversion failed (e.g., tuple was concurrently deleted).
			 * Return TM_Deleted rather than leaving the slot empty.
			 */
			ReleaseBuffer(buf);
			if (keyintact_image != NULL)
				pfree(keyintact_image);
			if (locked_copy != NULL)
				pfree(locked_copy);
			if (have_tuple_lock)
				FluxUnlockTuple(relation, tid, mode);
			return TM_Deleted;
		}
		slot->tts_tid = *tid;
		slot->tts_tableOid = RelationGetRelid(relation);

		/*
		 * FluxTupleToSlotWithOverflow leaves the slot virtual with
		 * tts_values[] pointing into the page buffer (and records no pin in
		 * the slot).  Deep-copy those pass-by-reference datums into
		 * slot-owned memory BEFORE dropping the pin, so a returned varlena
		 * (e.g. numeric via UPDATE ... RETURNING / FK / EPQ locking) does not
		 * alias a buffer that is about to be unpinned and reused.  This
		 * matches heap's slot discipline (ExecStorePinnedBufferHeapTuple
		 * keeps the pin in the slot).
		 */
		ExecMaterializeSlot(slot);

		/* Release the buffer pin now that slot is materialized */
		ReleaseBuffer(buf);

		if (keyintact_image != NULL)
			pfree(keyintact_image);
		if (locked_copy != NULL)
			pfree(locked_copy);

		/* Release tuple-level lock if we acquired it */
		if (have_tuple_lock)
			FluxUnlockTuple(relation, tid, mode);

		return result;
	}

out_unlock:

	/*
	 * TM_Invisible still has to leave the tuple in the caller's slot.
	 *
	 * heap does: heap_lock_tuple returns TM_Invisible for the ON CONFLICT DO
	 * UPDATE/DO SELECT case (heapam.c: "We return this value here rather than
	 * throwing an error in order to give that case the opportunity to throw a
	 * more specific error"), and heapam_tuple_lock then falls through to
	 * ExecStorePinnedBufferHeapTuple, so the slot is populated on that path
	 * too.  The executor relies on it: ExecOnConflictUpdate's TM_Invisible
	 * arm reads xmin off this very slot via slot_getsysattr() to recognise a
	 * self-conflict and raise "ON CONFLICT DO UPDATE command cannot affect
	 * row a second time" (nodeModifyTable.c).
	 *
	 * FLUX returned TM_Invisible with the slot left EMPTY, so that
	 * slot_getsysattr() hit the no-tuple path and raised "cannot retrieve a
	 * system column in this context" instead -- the wrong error, and a
	 * user-visible difference on plain INSERT ... ON CONFLICT (regression
	 * tests insert_conflict, update).
	 *
	 * Fill the slot here, at the single shared exit, so every TM_Invisible
	 * return is covered rather than just the two current ones.  The store is
	 * done while the buffer is still pinned and then materialized, matching
	 * the discipline of the success path below.
	 */

	/*
	 * The ItemPointerIsValid() test is not redundant: the FK fast-path batch
	 * flush (ri_LockPKTuple -> ri_FastPathFlushArray) can reach a locker with
	 * slot->tts_tid already invalidated, and ItemPointerGetOffsetNumber()
	 * asserts on an invalid pointer under cassert.  See the KNOWN-OPEN note
	 * on fk-crosstype-recheck in flux_default_triage.md.
	 */
	if (result == TM_Invisible && slot != NULL && BufferIsValid(buf) &&
		tid != NULL && ItemPointerIsValid(tid))
	{
		Page		ipage = BufferGetPage(buf);
		OffsetNumber ioff = ItemPointerGetOffsetNumber(tid);

		if (ioff >= FirstOffsetNumber && ioff <= PageGetMaxOffsetNumber(ipage))
		{
			ItemId		iid = PageGetItemId(ipage, ioff);

			if (ItemIdIsNormal(iid))
			{
				FluxTupleHeader *ihdr =
					(FluxTupleHeader *) PageGetItem(ipage, iid);

				/*
				 * Use the plain store (not FluxTupleToSlotWithOverflow): the
				 * caller only needs the system columns here, and the DELETED
				 * reject inside the overflow-aware path would refuse a
				 * delete-marked tuple outright.
				 */
				FluxSlotStoreTuple(slot, ihdr, ItemIdGetLength(iid), buf);
				slot->tts_tableOid = RelationGetRelid(relation);
				slot->tts_tid = *tid;
				ExecMaterializeSlot(slot);
			}
		}
	}

	UnlockReleaseBuffer(buf);

	if (keyintact_image != NULL)
		pfree(keyintact_image);

	/* Release tuple-level lock if we acquired it */
	if (have_tuple_lock)
		FluxUnlockTuple(relation, tid, mode);

	return result;
}

/*
 * Lock a tuple in FLUX table.
 *
 * FLUX keeps a row's latest version at its TID, except when an UPDATE moved
 * it (FLUX_TUPLE_SUPERSEDED): then the old version reports TM_Updated with
 * the new TID.  With TUPLE_LOCK_FLAG_FIND_LAST_VERSION follow it, as
 * heapam_tuple_lock follows t_ctid: lock the new version, move the caller's
 * tid to it and report traversed.
 */
static TM_Result
flux_tuple_lock(Relation relation, ItemPointer tid, Snapshot snapshot,
				TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
				LockWaitPolicy wait_policy, uint8 flags,
				TM_FailureData *tmfd)
{
	ItemPointerData cur = *tid;
	bool		traversed = false;
	ItemPointerData next_tid;
	TM_Result	result;

	for (;;)
	{
		result = flux_tuple_lock_version(relation, &cur, snapshot, slot, cid,
										 mode, wait_policy, flags, tmfd, false, &next_tid);
		if (result == TM_Ok && ItemPointerIsValid(&next_tid))
		{
			TM_FailureData follow_tmfd;

			do
			{
				ItemPointerData next = next_tid;

				result = flux_tuple_lock_version(relation, &next, snapshot,
												 NULL, cid, mode, wait_policy, flags,
												 &follow_tmfd, true, &next_tid);
			} while (result == TM_Ok && ItemPointerIsValid(&next_tid));
			if (result != TM_Ok && tmfd != NULL)
				*tmfd = follow_tmfd;
		}
		if (result != TM_Updated || tmfd == NULL ||
			!(flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION) ||
			!ItemPointerIsValid(&tmfd->ctid) ||
			ItemPointerIndicatesMovedPartitions(&tmfd->ctid) ||
			ItemPointerEquals(&tmfd->ctid, &cur))
			break;
		cur = tmfd->ctid;
		traversed = true;
	}

	if (traversed)
	{
		*tid = cur;
		tmfd->traversed = true;
	}
	return result;
}

/*
 * Nontransactional truncate for FLUX relation
 *
 * This is called for TRUNCATE operations. We use RelationTruncate which
 * properly handles all forks (main, FSM, VM), WAL logging, shared buffer
 * invalidation, and cache coherency.
 */
static void
flux_relation_nontransactional_truncate(Relation rel)
{
	RelationTruncate(rel, 0);
	SLogTupleForgetStorage(RelationGetRelid(rel), rel->rd_locator.relNumber);
}

/*
 * Set new filelocator for FLUX relation
 */
static void
flux_relation_set_new_filelocator(Relation rel,
								  const RelFileLocator *newrlocator,
								  char persistence,
								  TransactionId *freezeXid,
								  MultiXactId *minmulti)
{
	SMgrRelation srel;

	/* Set freeze XID to current transaction minimum */
	*freezeXid = RecentXmin;

	/* Set minimum multixact ID */
	*minmulti = GetOldestMultiXactId();

	/* Create the storage file (empty, no blocks yet) */
	srel = RelationCreateStorage(*newrlocator, persistence, true);

	/* WAL-log the file creation */
	if (persistence == RELPERSISTENCE_PERMANENT)
		log_smgrcreate(newrlocator, MAIN_FORKNUM);

	/*
	 * Note: We do not initialize block 0 here. Block 0 will be created
	 * on-demand during the first scan or insert operation via
	 * FluxGetPageWithFreeSpace() or the scan's RBM_ZERO_AND_LOCK logic.
	 */

	/* Set up init fork for unlogged tables if needed */
	if (persistence == RELPERSISTENCE_UNLOGGED)
	{
		Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
			   rel->rd_rel->relkind == RELKIND_TOASTVALUE);
		smgrcreate(srel, INIT_FORKNUM, false);
		log_smgrcreate(newrlocator, INIT_FORKNUM);
	}

	smgrclose(srel);

	/*
	 * FLUX does not maintain a UNDO log.  Its DML UNDO goes to the
	 * per-backend UNDO engine (access/undo/perbackend), which owns its own
	 * log segments and discard worker, so there is nothing to create here.
	 */
}

/*
 * flux_index_tombstone_removable -- table_index_tombstone_removable() for FLUX.
 *
 * An in-place UPDATE of an indexed column delete-marks the old (key, TID)
 * entry so that snapshots still reading an older version of the row reach it
 * by the old key.  Older versions live only in UNDO, behind the version on
 * the page.  Once that version's writer (t_xmin) committed and is older than
 * every snapshot, no snapshot reads anything but the version on the page, so
 * an entry whose key image differs from that version's key is rejected by
 * every reader's recheck (flux_index_entry_key_mismatch) and needed by
 * nobody.  Rows whose writer is in progress or aborted (not yet reverted),
 * and deleted rows, keep their entries; a dead TID is VACUUM's ordinary
 * business.
 *
 * The version's xmin is also the recovery conflict horizon.  A snapshot that
 * reads an older version cannot see that xmin, so the xmin is in its xip or
 * not below its xmax, and either way the snapshot's xmin is not above it:
 * exactly the snapshots a conflict on it cancels.  This holds whatever order
 * the chain's writers got their XIDs in (an older XID can write the newest
 * version).  A subtransaction's xmin follows its parent's, which bounds the
 * snapshot's xmin the same way.
 *
 * Called with the index leaf page cleanup-locked, which also keeps a
 * concurrent key UPDATE from reviving the entry until it is gone (that UPDATE
 * then inserts a fresh one).  The table page lock nests inside it, the same
 * order as bottom-up index deletion; FLUX never holds a table page lock while
 * it locks an index page.  Nothing that could run user code or read the
 * catalogs or TOAST may run under the leaf lock, so only indexes on plain
 * columns whose index and table types agree (no expression or partial index,
 * no opclass storage type, no exclusion constraint) are handled, and rows
 * with an out-of-line key value keep their entries.
 */
static bool
flux_index_tombstone_removable(Relation rel, Relation irel, IndexTuple itup,
							   ItemPointer tid, TransactionId *conflict_xid)
{
	BlockNumber blkno = ItemPointerGetBlockNumber(tid);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
	TupleDesc	itupdesc = RelationGetDescr(irel);
	int			nkeys = IndexRelationGetNumberOfKeyAttributes(irel);
	Datum		ent_values[INDEX_MAX_KEYS];
	bool		ent_isnull[INDEX_MAX_KEYS];
	FluxTupleHeader *hdr = NULL;
	TransactionId xmin;
	TupleTableSlot *slot;
	Buffer		buf;
	Page		page;
	bool		removable = false;

	if (blkno >= RelationGetNumberOfBlocks(rel) ||
		irel->rd_index->indisexclusion ||
		!heap_attisnull(irel->rd_indextuple, Anum_pg_index_indexprs, NULL) ||
		!heap_attisnull(irel->rd_indextuple, Anum_pg_index_indpred, NULL))
		return false;
	for (int i = 0; i < nkeys; i++)
	{
		AttrNumber	attno = irel->rd_index->indkey.values[i];

		if (attno <= 0 ||
			TupleDescAttr(itupdesc, i)->atttypid !=
			TupleDescAttr(RelationGetDescr(rel), attno - 1)->atttypid)
			return false;
	}

	/*
	 * Share-exclusive, not share: the byte-CAS UPDATE rewrites the tuple in
	 * place under share-exclusive, and the copy must not be torn.
	 */
	buf = ReadBuffer(rel, blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE_EXCLUSIVE);
	page = BufferGetPage(buf);
	if (!PageIsNew(page) && offnum >= FirstOffsetNumber &&
		offnum <= PageGetMaxOffsetNumber(page))
	{
		ItemId		lp = PageGetItemId(page, offnum);

		if (ItemIdIsNormal(lp))
		{
			FluxTupleHeader *phdr = (FluxTupleHeader *) PageGetItem(page, lp);
			Size		len = ItemIdGetLength(lp);

			if (!(phdr->t_flags & (FLUX_TUPLE_DELETED |
								   FLUX_TUPLE_SPECULATIVE |
								   FLUX_TUPLE_HAS_OVERFLOW)) &&
				!FluxIsOverflowRecordInline(phdr, len))
			{
				hdr = palloc(len);
				memcpy(hdr, phdr, len);
			}
		}
	}
	UnlockReleaseBuffer(buf);
	if (hdr == NULL)
		return false;

	/*
	 * The writer's state comes from the horizon and CLOG, never from the
	 * UNCOMMITTED hint, which a committed write can still carry (see flux.h).
	 * Past the removable horizon the writer is not running (a prepared
	 * transaction counts as running); a subcommitted writer whose parent has
	 * not committed fails TransactionIdDidCommit.
	 */
	xmin = FluxTupleGetXmin(hdr);
	if (!TransactionIdIsNormal(xmin) ||
		!GlobalVisTestIsRemovableXid(GlobalVisTestFor(rel), xmin, true) ||
		!TransactionIdDidCommit(xmin))
	{
		pfree(hdr);
		return false;
	}

	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
									table_slot_callbacks(rel));
	FluxTupleToSlotWithOverflow(hdr, slot, rel);
	slot_getallattrs(slot);
	index_deform_tuple(itup, itupdesc, ent_values, ent_isnull);
	for (int i = 0; i < nkeys; i++)
	{
		Form_pg_attribute att = TupleDescAttr(itupdesc, i);
		AttrNumber	attno = irel->rd_index->indkey.values[i];
		Datum		val = slot->tts_values[attno - 1];
		bool		isnull = slot->tts_isnull[attno - 1];

		if (!isnull && att->attlen == -1 &&
			VARATT_IS_EXTERNAL(DatumGetPointer(val)))
		{
			removable = false;	/* would read TOAST: keep */
			break;
		}
		if (isnull != ent_isnull[i] ||
			(!isnull && !datum_image_eq(val, ent_values[i],
										att->attbyval, att->attlen)))
			removable = true;
	}
	ExecDropSingleTupleTableSlot(slot);
	pfree(hdr);

	*conflict_xid = xmin;
	return removable;
}

/*
 * Check whether table tuples referenced by index entries are dead.
 *
 * This is called by index AMs during index tuple deletion (both simple
 * deletion during VACUUM and bottom-up deletion during retail inserts).
 * The index AM passes a list of TIDs and we check each one's liveness.
 * We set knowndeletable=true for entries whose table tuples are dead,
 * allowing the index AM to remove its entries.
 *
 * IMPORTANT: This function must NEVER modify table data.  It only reads
 * tuple headers to check visibility status.
 *
 * Modeled on heap_index_delete_tuples() but simplified for FLUX's
 * timestamp-based MVCC.
 */
static TransactionId
flux_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	TransactionId snapshotConflictHorizon = InvalidTransactionId;
	BlockNumber blkno = InvalidBlockNumber;
	Buffer		buf = InvalidBuffer;
	Page		page = NULL;
	OffsetNumber maxoff = InvalidOffsetNumber;
	int			finalndeltids = 0;

	Assert(delstate->ndeltids > 0);

	/* Iterate over deltids, determine which are deletable */
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *ideltid = &delstate->deltids[i];
		TM_IndexStatus *istatus = delstate->status + ideltid->id;
		ItemPointer htid = &ideltid->tid;
		OffsetNumber offnum;

		/*
		 * Read buffer for this block if we haven't already.  Avoid refetching
		 * if it's the same block as the previous entry.
		 */
		if (blkno == InvalidBlockNumber ||
			ItemPointerGetBlockNumber(htid) != blkno)
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);

			blkno = ItemPointerGetBlockNumber(htid);
			buf = ReadBuffer(rel, blkno);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			maxoff = PageGetMaxOffsetNumber(page);
		}

		offnum = ItemPointerGetOffsetNumber(htid);

		/* Sanity check: offset must be valid */
		if (offnum < FirstOffsetNumber || offnum > maxoff)
		{
			/*
			 * Index entry points to invalid offset.  Mark as deletable to
			 * clean up the corruption.
			 */
			istatus->knowndeletable = true;
			finalndeltids = i + 1;
			continue;
		}

		/* Already known to be deletable by the index AM? */
		if (istatus->knowndeletable)
		{
			ItemId		lp = PageGetItemId(page, offnum);

			Assert(!delstate->bottomup && !istatus->promising);

			/*
			 * The index AM marked this entry deletable up front (a LP_DEAD
			 * index tuple from kill_prior_tuple).  Its FLUX tuple header must
			 * still advance the snapshotConflictHorizon, exactly as heap does
			 * for its own already-knowndeletable entries: nbtree logs the
			 * value in the simple-deletion record, so a hot standby cancels
			 * the snapshots that could still see this index entry before it
			 * replays the removal.  A non-normal (already LP_DEAD/unused) line
			 * pointer is skipped: the prune that set it dead already logged
			 * its own horizon.
			 */
			if (ItemIdIsNormal(lp))
			{
				FluxTupleHeader *tuple_hdr;

				tuple_hdr = (FluxTupleHeader *) PageGetItem(page, lp);
				FluxTupleAdvanceConflictHorizon(tuple_hdr,
												&snapshotConflictHorizon);
			}

			finalndeltids = i + 1;
			continue;
		}

		{
			ItemId		lp = PageGetItemId(page, offnum);

			if (!ItemIdIsNormal(lp))
			{
				/*
				 * LP_DEAD, LP_UNUSED, or LP_REDIRECT: the tuple is gone. The
				 * index entry can be removed.
				 */
				istatus->knowndeletable = true;
			}
			else
			{
				FluxTupleHeader *tuple_hdr;

				tuple_hdr = (FluxTupleHeader *) PageGetItem(page, lp);

				/*
				 * For FLUX, a tuple is vacuumable (and its index entry
				 * deletable) if it is deleted AND old enough that no snapshot
				 * can see it.
				 */
				if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
				{
					if (FluxTupleDeadToAll(tuple_hdr, FluxGetOldestXminHorizon(rel)))
					{
						istatus->knowndeletable = true;

						/*
						 * Ratchet the snapshotConflictHorizon forward past this
						 * committed DELETE's xmax, as in the knowndeletable
						 * branch above and as heap does with
						 * HeapTupleHeaderAdvanceConflictHorizon.
						 */
						FluxTupleAdvanceConflictHorizon(tuple_hdr,
														&snapshotConflictHorizon);
					}
					else
					{
						/* Recently dead -- cannot delete index entry yet */
						continue;
					}
				}
				else
				{
					/* Live tuple -- cannot delete index entry */
					continue;
				}
			}
		}

		/* Track progress for bottom-up deletion */
		if (delstate->bottomup && istatus->knowndeletable)
		{
			int			actualfreespace = 0;

			actualfreespace += istatus->freespace;
			if (actualfreespace >= delstate->bottomupfreespace)
			{
				/* Met the space target -- stop early */
				finalndeltids = i + 1;
				break;
			}
		}

		finalndeltids = i + 1;
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);

	/*
	 * Shrink deltids array to exclude non-deletable entries at the end.
	 */
	Assert(finalndeltids > 0 || delstate->bottomup);
	delstate->ndeltids = finalndeltids;

	return snapshotConflictHorizon;
}

/*
 * Copy data for FLUX relation (used by ALTER TABLE SET ACCESS METHOD, etc.)
 *
 * This performs a block-level copy of all storage forks from the old
 * relation files to new ones. Since we copy directly without examining
 * shared buffers, we must flush any dirty pages first. The old physical
 * files are scheduled for deletion.
 */
/*
 * flux_revert_pending
 *		Revert every aborted version of rel whose UNDO has not been applied
 *		yet, before rel's contents are copied to a new file.
 *
 * FLUX updates in place, so a transaction whose rollback was handed to an
 * undo apply worker, or a crash loser, leaves its images on the page until
 * the worker gets to them.  A rewrite copies the page into a new file, and
 * the UNDO records name the old one (FluxUndoPayloadHeader.relnumber), so
 * once the rewrite commits nothing would ever restore the copies: the copied
 * aborted UPDATE would be the row's value for good, and an aborted DELETE
 * would lose the row.  So the rewrite reverts them first, as a writer does
 * before it changes a row (flux_help_revert).
 *
 * The caller holds AccessExclusiveLock on rel.  An undo apply worker that
 * wants to apply the same records waits for that lock in flux_undo_apply,
 * and finds them no longer current afterwards (or the file gone), so only
 * this backend changes the pages here.  flux_help_revert releases the buffer
 * when it reverted something; the page is then locked again and the same
 * offset is retried, until nothing on it is left to revert.  The index
 * reversal of an in-place key UPDATE (flux_undo_index_first) runs on the old
 * indexes, which the rewrite rebuilds from the new file anyway.
 *
 * A hot standby does not rewrite tables, and flux_help_revert never changes
 * a page during recovery.
 */
static void
flux_revert_pending(Relation rel)
{
	BufferAccessStrategy bstrategy;
	BlockNumber nblocks;

	if (RecoveryInProgress())
		return;

	bstrategy = GetAccessStrategy(BAS_BULKREAD);
	nblocks = RelationGetNumberOfBlocks(rel);
	for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buf;
		Page		page;
		bool		pending = false;

		CHECK_FOR_INTERRUPTS();

		/* Look under a share lock first: most pages have nothing pending. */
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL,
								 bstrategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!PageIsNew(page))
		{
			for (OffsetNumber off = FirstOffsetNumber;
				 !pending && off <= PageGetMaxOffsetNumber(page);
				 off = OffsetNumberNext(off))
			{
				ItemId		lp = PageGetItemId(page, off);

				pending = ItemIdIsNormal(lp) &&
					!FluxIsOverflowRecordInline(PageGetItem(page, lp),
												ItemIdGetLength(lp)) &&
					FluxTupleHasAbortedVersion((FluxTupleHeader *)
											   PageGetItem(page, lp));
			}
		}
		if (!pending)
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		for (OffsetNumber off = FirstOffsetNumber;
			 off <= PageGetMaxOffsetNumber(page);
			 off = OffsetNumberNext(off))
		{
			ItemId		lp = PageGetItemId(page, off);

			if (!ItemIdIsNormal(lp) ||
				FluxIsOverflowRecordInline(PageGetItem(page, lp),
										   ItemIdGetLength(lp)))
				continue;

			while (flux_help_revert(rel, buf, off))
			{
				buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno,
										 RBM_NORMAL, bstrategy);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				page = BufferGetPage(buf);
			}
		}
		UnlockReleaseBuffer(buf);
	}
	FreeAccessStrategy(bstrategy);
}

static void
flux_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	SMgrRelation dstrel;

	/*
	 * The file is copied as it is, so first revert what an aborted
	 * transaction left on it: see flux_revert_pending.
	 */
	flux_revert_pending(rel);

	/*
	 * Since we copy the file directly without looking at the shared buffers,
	 * we'd better first flush out any pages of the source relation that are
	 * in shared buffers. We assume no new changes will be made while we are
	 * holding exclusive lock on the rel.
	 */
	FlushRelationBuffers(rel);

	/*
	 * Create and copy all forks of the relation, and schedule unlinking of
	 * old physical files.
	 *
	 * NOTE: any conflict in relfilenumber value will be caught in
	 * RelationCreateStorage().
	 */
	dstrel = RelationCreateStorage(*newrlocator, rel->rd_rel->relpersistence,
								   true);

	/* Copy main fork */
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* Copy any extra forks that exist (FSM, etc.) */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * WAL log creation if the relation is persistent, or this is the
			 * init fork of an unlogged relation.
			 */
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(newrlocator, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}

	/* Drop old relation storage, and close new one */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

/*
 * flux_cluster_write
 *		Copy one row of a CLUSTER / VACUUM FULL rewrite into the new relation,
 *		with the writers of the version it copies (FLUX_INSERT_REWRITE), as
 *		heap's rewriteheap.c keeps xmin and xmax: a snapshot taken before the
 *		rewrite must see the same rows after it.  CLUSTER and VACUUM FULL are
 *		MVCC-safe (see "Caveats" in mvcc.sgml); only TRUNCATE, the rewriting
 *		ALTER TABLE forms and REPACK CONCURRENTLY are not.
 */
static void
flux_cluster_write(Relation NewTable, TupleTableSlot *slot,
				   const FluxRewriteVersion *v)
{
	FluxSetRewriteVersion(v);
	table_tuple_insert(NewTable, slot, v->cmin, FLUX_INSERT_REWRITE, NULL);
}

/*
 * The writers of the version hdr, for a rewrite whose freeze cutoff is
 * xid_cutoff.  Its inserter committed or is this transaction: an aborted one
 * was dropped or reverted (flux_revert_pending), and nobody else can be
 * writing, as we hold AccessExclusiveLock.  A committed inserter before the
 * cutoff is frozen, as heap's rewrite_heap_tuple freezes with
 * heap_freeze_tuple: the cutoff becomes the new relfrozenxid.  A deleter
 * that survives the rewrite is recently dead, so it is at or after the
 * cutoff; it committed unless it is ours.
 */
static void
flux_rewrite_version_of(const FluxTupleHeader *hdr, TransactionId xid_cutoff,
						FluxRewriteVersion *v)
{
	TransactionId xmax = FluxTupleGetXmax(hdr);
	bool		own = TransactionIdIsCurrentTransactionId(FluxTupleGetXmin(hdr));

	memset(v, 0, sizeof(*v));
	v->xmin = FluxTupleGetXmin(hdr);
	v->cmin = own ? hdr->t_cmin : FirstCommandId;
	v->xmin_committed = !own;
	if (!own && TransactionIdIsNormal(v->xmin) &&
		TransactionIdPrecedes(v->xmin, xid_cutoff))
		v->xmin = FrozenTransactionId;
	if ((hdr->t_flags & FLUX_TUPLE_DELETED) && TransactionIdIsValid(xmax))
	{
		v->xmax = xmax;
		v->xmax_committed = !TransactionIdIsCurrentTransactionId(xmax);
		v->moved_partition =
			(hdr->t_flags & FLUX_TUPLE_MOVED_PARTITION) != 0;
	}
}

/*
 * Copy the version in slot, whose writers are v, into the new relation, or
 * into the CLUSTER sort.  The sort keeps a heap tuple, header included, so
 * the writers travel in its header as a heap tuple's would, and the version
 * needs no other identity: the versions of one row share its old TID.
 */
static void
flux_cluster_emit(Relation NewTable, TupleTableSlot *slot,
				  const FluxRewriteVersion *v, Tuplesortstate *tuplesort)
{
	bool		shouldFree;
	HeapTuple	tuple;
	HeapTupleHeader htup;

	if (tuplesort == NULL)
	{
		flux_cluster_write(NewTable, slot, v);
		return;
	}

	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);
	htup = tuple->t_data;
	htup->t_infomask &= ~HEAP_XACT_MASK;
	HeapTupleHeaderSetXmin(htup, v->xmin);
	HeapTupleHeaderSetCmin(htup, v->cmin);
	HeapTupleHeaderSetXmax(htup, v->xmax);
	if (v->xmin_committed)
		htup->t_infomask |= HEAP_XMIN_COMMITTED;
	if (!TransactionIdIsValid(v->xmax))
		htup->t_infomask |= HEAP_XMAX_INVALID;
	else if (v->xmax_committed)
		htup->t_infomask |= HEAP_XMAX_COMMITTED;
	htup->t_ctid = slot->tts_tid;
	if (v->moved_partition)
		HeapTupleHeaderSetMovedPartitions(htup);
	tuplesort_putheaptuple(tuplesort, tuple);
	if (shouldFree)
		heap_freetuple(tuple);
}

/* The writers flux_cluster_emit put in a sorted tuple's header. */
static void
flux_cluster_sorted_version(HeapTupleHeader htup, FluxRewriteVersion *v)
{
	memset(v, 0, sizeof(*v));
	v->xmin = HeapTupleHeaderGetRawXmin(htup);
	v->cmin = HeapTupleHeaderGetRawCommandId(htup);
	v->xmin_committed = (htup->t_infomask & HEAP_XMIN_COMMITTED) != 0;
	if (!(htup->t_infomask & HEAP_XMAX_INVALID))
	{
		v->xmax = HeapTupleHeaderGetRawXmax(htup);
		v->xmax_committed = (htup->t_infomask & HEAP_XMAX_COMMITTED) != 0;
		v->moved_partition = HeapTupleHeaderIndicatesMovedPartitions(htup);
	}
}

/*
 * The copy of REPACK (CONCURRENTLY): the rows 'snapshot' sees, in the order
 * of OldIndex or physical order, inserted into the new relation by this
 * transaction.  repack.c then replays the changes committed after the
 * snapshot, which it decodes from the WAL, onto the new relation.
 *
 * Unlike the copy for CLUSTER and VACUUM FULL, this one is not MVCC-safe,
 * exactly like heap's: a version only an older snapshot sees is not copied,
 * and the copied rows are this transaction's, so a snapshot taken before
 * REPACK committed finds the table empty afterwards (see "Caveats" in
 * mvcc.sgml).  Only that way do the replayed changes, which this
 * transaction makes too, apply to the copied rows.
 *
 * The scan is an ordinary MVCC scan, which reconstructs the version the
 * snapshot sees from UNDO.  So an aborted transaction's change, even with its
 * revert still pending, is not copied (its writer is not committed in the
 * snapshot), and the revert, when it comes, finds the record naming the old
 * file and does nothing to the new one.  The UNDO the snapshot needs stays:
 * UNDO is discarded only behind GetOldestNonRemovableTransactionId(), the
 * horizon that also keeps heap from pruning the tuples the snapshot sees.
 *
 * An index scan of OldIndex returns each row once: an in-place key UPDATE
 * leaves a delete-marked entry under the old key, and the key recheck in
 * flux_index_getnext_slot skips the entry whose key the version the snapshot
 * sees does not carry.
 */
static void
flux_copy_for_concurrent_repack(Relation OldTable, Relation NewTable,
								Relation OldIndex, bool use_sort,
								Snapshot snapshot, double *num_tuples)
{
	TableScanDesc tableScan = NULL;
	IndexScanDesc indexScan = NULL;
	Tuplesortstate *tuplesort = NULL;
	TupleTableSlot *slot;
	BulkInsertState bistate = GetBulkInsertState();
	CommandId	cid = GetCurrentCommandId(true);

	if (OldIndex != NULL && !use_sort)
	{
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_INDEX_SCAN_HEAP);
		indexScan = index_beginscan(OldTable, OldIndex, false, snapshot,
									NULL, 0, 0, SO_NONE);
		index_rescan(indexScan, NULL, 0, NULL, 0);
	}
	else
	{
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_SEQ_SCAN_HEAP);
		tableScan = table_beginscan(OldTable, snapshot, 0, NULL, SO_NONE);
	}

	if (use_sort)
		tuplesort = tuplesort_begin_cluster(RelationGetDescr(OldTable),
											OldIndex, maintenance_work_mem,
											NULL, TUPLESORT_NONE);

	slot = table_slot_create(OldTable, NULL);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		if (indexScan != NULL)
		{
			if (!table_index_getnext_slot(indexScan, ForwardScanDirection,
										  slot))
				break;
		}
		else if (!table_scan_getnextslot(tableScan, ForwardScanDirection,
										 slot))
			break;

		*num_tuples += 1;
		if (tuplesort != NULL)
		{
			bool		shouldFree;
			HeapTuple	tuple = ExecFetchSlotHeapTuple(slot, true, &shouldFree);

			tuplesort_putheaptuple(tuplesort, tuple);
			if (shouldFree)
				heap_freetuple(tuple);
		}
		else
		{
			const int	ct_index[] = {
				PROGRESS_REPACK_HEAP_TUPLES_SCANNED,
				PROGRESS_REPACK_HEAP_TUPLES_INSERTED
			};
			int64		ct_val[2] = {*num_tuples, *num_tuples};

			table_tuple_insert(NewTable, slot, cid, TABLE_INSERT_NO_LOGICAL,
							   bistate);
			pgstat_progress_update_multi_param(2, ct_index, ct_val);
			continue;
		}
		pgstat_progress_update_param(PROGRESS_REPACK_HEAP_TUPLES_SCANNED,
									 *num_tuples);
	}

	if (indexScan != NULL)
		index_endscan(indexScan);
	if (tableScan != NULL)
		table_endscan(tableScan);

	if (tuplesort != NULL)
	{
		double		n_tuples = 0;

		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_SORT_TUPLES);
		tuplesort_performsort(tuplesort);
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_WRITE_NEW_HEAP);
		for (;;)
		{
			HeapTuple	tuple;

			CHECK_FOR_INTERRUPTS();

			tuple = tuplesort_getheaptuple(tuplesort, true);
			if (tuple == NULL)
				break;
			ExecForceStoreHeapTuple(tuple, slot, false);
			table_tuple_insert(NewTable, slot, cid, TABLE_INSERT_NO_LOGICAL,
							   bistate);
			pgstat_progress_update_param(PROGRESS_REPACK_HEAP_TUPLES_INSERTED,
										 ++n_tuples);
		}
		tuplesort_end(tuplesort);
	}

	ExecDropSingleTupleTableSlot(slot);
	FreeBulkInsertState(bistate);
}

/*
 * Copy data for cluster operation
 *
 * This is called by CLUSTER and VACUUM FULL to copy tuples from the old
 * table to the new one, optionally reordering by an index. We scan the
 * old table using SnapshotAny and perform our own MVCC visibility checks
 * to decide which tuples to keep, which to discard as dead, and which
 * are recently dead.
 *
 * For FLUX, visibility is determined by the tuple's timestamp-based MVCC
 * flags (deleted flag, commit timestamp, etc.) rather than heap-style xmin/xmax.
 */
static void
flux_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
							   Relation OldIndex, bool use_sort,
							   TransactionId OldestXmin,
							   Snapshot snapshot,
							   TransactionId *xid_cutoff,
							   MultiXactId *multi_cutoff,
							   double *num_tuples,
							   double *tups_vacuumed,
							   double *tups_recently_dead)
{
	TableScanDesc tableScan;
	IndexScanDesc indexScan;
	TupleTableSlot *slot;
	TupleTableSlot *vslot;
	Tuplesortstate *tuplesort = NULL;
	double		live_tuples = 0;
	double		dead_tuples = 0;
	double		recent_dead = 0;

	/*
	 * Keep the caller's *xid_cutoff / *multi_cutoff: they become the new
	 * relfrozenxid / relminmxid of the table and of its TOAST table.  Rows
	 * keep the writers of the versions they copy (flux_cluster_write).  A
	 * version survives the copy only while some snapshot may see it, so its
	 * deleter is ours or committed at or after OldestXmin, which the cutoff
	 * does not follow; an inserter before the cutoff is frozen
	 * (flux_rewrite_version_of).  So no copied xid precedes the cutoff. FLUX
	 * tuples carry no multixacts.
	 */
	*num_tuples = 0;
	*tups_vacuumed = 0;
	*tups_recently_dead = 0;

	/* Only REPACK (CONCURRENTLY) passes a snapshot. */
	if (snapshot != NULL)
	{
		flux_copy_for_concurrent_repack(OldTable, NewTable, OldIndex,
										use_sort, snapshot, num_tuples);
		return;
	}

	/*
	 * Valid smgr_targblock implies something already wrote to the relation.
	 * This may be harmless, but this function hasn't planned for it.
	 */
	Assert(RelationGetTargetBlock(NewTable) == InvalidBlockNumber);

	/*
	 * Copy the committed state, not what an aborted transaction left on the
	 * page: see flux_revert_pending.  This must precede the scan, which reads
	 * the page versions with SnapshotAny.
	 */
	flux_revert_pending(OldTable);

	/*
	 * Set up the scan. If we have an index and are not doing a sort, use an
	 * index scan to get tuples in index order. Otherwise do a sequential scan
	 * (and optionally sort afterward).
	 */
	if (OldIndex != NULL && !use_sort)
	{
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_INDEX_SCAN_HEAP);

		tableScan = NULL;
		indexScan = index_beginscan(OldTable, OldIndex, false, SnapshotAny,
									NULL, 0, 0, SO_RETURN_DELETED);
		index_rescan(indexScan, NULL, 0, NULL, 0);
	}
	else
	{
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_SEQ_SCAN_HEAP);

		tableScan = table_beginscan(OldTable, SnapshotAny, 0, NULL,
									SO_RETURN_DELETED);
		indexScan = NULL;
	}

	/*
	 * Both scans hand over deleted versions too (SO_RETURN_DELETED): the loop
	 * below keeps those some snapshot may still see.
	 */
	slot = table_slot_create(OldTable, NULL);
	vslot = table_slot_create(OldTable, NULL);

	/*
	 * Scan and sort, as heap does: repack.c asks for a sort when the planner
	 * finds it cheaper than an index scan, and always for an expression
	 * index.  Ignoring use_sort copied the rows in physical order.
	 */
	if (use_sort)
		tuplesort = tuplesort_begin_cluster(RelationGetDescr(OldTable),
											OldIndex, maintenance_work_mem,
											NULL, TUPLESORT_NONE);

	/*
	 * Scan through the old table. For each tuple, check visibility using
	 * FLUX's timestamp-based MVCC and either copy it to the new table or skip
	 * it.
	 */
	for (;;)
	{
		bool		isdead = false;
		FluxRewriteVersion version;
		char	   *image = NULL;
		Size		image_len = 0;
		ItemPointerData otid;
		List	   *older;

		CHECK_FOR_INTERRUPTS();

		if (indexScan != NULL)
		{
			if (!table_index_getnext_slot(indexScan, ForwardScanDirection,
										  slot))
				break;
		}
		else
		{
			if (!table_scan_getnextslot(tableScan, ForwardScanDirection, slot))
				break;
		}

		/*
		 * For FLUX, check tuple visibility using our page-level access. Read
		 * the tuple header from the page to check MVCC flags.
		 */
		{
			Buffer		buf;
			Page		page;
			ItemId		itemid;
			FluxTupleHeader *tuple_hdr;
			BlockNumber blkno = ItemPointerGetBlockNumber(&slot->tts_tid);
			OffsetNumber offnum = ItemPointerGetOffsetNumber(&slot->tts_tid);

			buf = ReadBuffer(OldTable, blkno);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);

			itemid = PageGetItemId(page, offnum);
			if (!ItemIdIsNormal(itemid))
			{
				/* Item pointer is dead or unused -- skip */
				UnlockReleaseBuffer(buf);
				dead_tuples++;
				continue;
			}

			tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

			/*
			 * A version written by an INSERT (no UNDO history) whose inserter
			 * aborted is never visible to anyone, whatever OldestXmin says:
			 * drop it, as heap does with HEAPTUPLE_DEAD and as
			 * flux_index_build_range_scan does.  INSERT writes no UNDO, so
			 * flux_revert_pending leaves it alone; FluxTupleDeadToAll only
			 * reclaims it once the inserter is behind the horizon, which a
			 * pending revert may hold back.  Our own transaction's rows are
			 * kept.  Nobody else can still be inserting, since we hold
			 * AccessExclusiveLock.
			 */
			if (!UndoRecPtrIsValid(tuple_hdr->t_verptr) &&
				!(tuple_hdr->t_flags & FLUX_TUPLE_XMIN_COMMITTED))
			{
				TransactionId xmin = FluxTupleGetXmin(tuple_hdr);

				if (TransactionIdIsNormal(xmin) &&
					!TransactionIdIsCurrentTransactionId(xmin) &&
					!TransactionIdIsInProgress(xmin) &&
					!TransactionIdDidCommit(xmin))
					isdead = true;
			}

			/*
			 * An UPDATED tuple is the live current version: FLUX updates in
			 * place, and an UPDATE that moves the row deletes the old
			 * version.
			 */
			if (!isdead && (tuple_hdr->t_flags & FLUX_TUPLE_DELETED))
			{
				/*
				 * Tuple has been deleted. Check whether it's old enough to be
				 * truly dead vs recently dead (still needed for MVCC
				 * snapshots), by the XID horizon (FluxTupleDeadToAll).
				 */
				if (FluxTupleDeadToAll(tuple_hdr, OldestXmin))
				{
					/* Definitely dead -- can discard */
					isdead = true;
				}
				else
				{
					/*
					 * Recently dead -- still needed by some snapshots.  Copy
					 * it with its deleter, as heap copies a RECENTLY_DEAD
					 * tuple.
					 */
					recent_dead++;
					isdead = false;
				}
			}

			/*
			 * The writers of the version to copy, and the version itself if
			 * it has history (the chain is walked without the lock).
			 */
			if (!isdead)
			{
				flux_rewrite_version_of(tuple_hdr, *xid_cutoff, &version);
				if (UndoRecPtrIsValid(FluxTupleGetVersionPtr(tuple_hdr)))
				{
					image_len = ItemIdGetLength(itemid);
					image = palloc(image_len);
					memcpy(image, tuple_hdr, image_len);
				}
			}

			UnlockReleaseBuffer(buf);
		}

		if (isdead)
		{
			dead_tuples++;
			continue;
		}

		/* Live or recently-dead tuple -- copy to new table */
		otid = slot->tts_tid;	/* the insert sets the new TID */
		flux_cluster_emit(NewTable, slot, &version, tuplesort);
		live_tuples++;

		/*
		 * A row updated in place holds its older versions in UNDO, which the
		 * new relation does not inherit.  Copy each one some snapshot may
		 * still see as a row of its own, deleted by the update that replaced
		 * it, as heap copies the RECENTLY_DEAD versions of an update chain.
		 * Each is a version of the same row: they share its old TID, and
		 * VACUUM reclaims them like any committed delete.
		 *
		 * The version another transaction wrote and we updated is copied
		 * deleted by us.  One we wrote ourselves and updated again is
		 * dropped: no other transaction can see it, and this one's later
		 * commands see only the newest version.  A cursor of ours opened
		 * before our own update loses the row, as the command ids of our own
		 * updates (t_cmax) do not carry over to the new relation.
		 */
		older = image != NULL ?
			FluxRewriteChain(OldTable, &otid, image, image_len,
							 OldestXmin) : NIL;
		if (image != NULL)
			pfree(image);
		foreach_ptr(FluxChainVersion, cv, older)
		{
			FluxRewriteVersion ov;

			if (TransactionIdIsCurrentTransactionId(FluxTupleGetXmin((FluxTupleHeader *) cv->image)))
			{
				pfree(cv->image);
				continue;
			}
			flux_rewrite_version_of((FluxTupleHeader *) cv->image,
									*xid_cutoff, &ov);
			ov.xmax = cv->xmax;
			ov.xmax_committed = !TransactionIdIsCurrentTransactionId(cv->xmax);
			ov.moved_partition = false;
			FluxSlotStoreTuple(vslot, (FluxTupleHeader *) cv->image, cv->len,
							   InvalidBuffer);
			vslot->tts_tableOid = RelationGetRelid(OldTable);
			vslot->tts_tid = otid;
			flux_cluster_emit(NewTable, vslot, &ov, tuplesort);
			pfree(cv->image);
			recent_dead++;
			live_tuples++;
		}
		list_free_deep(older);

		pgstat_progress_update_param(PROGRESS_REPACK_HEAP_TUPLES_SCANNED,
									 live_tuples + dead_tuples + recent_dead);
	}

	/* Clean up scan resources */
	if (indexScan != NULL)
		index_endscan(indexScan);
	if (tableScan != NULL)
		table_endscan(tableScan);

	if (tuplesort != NULL)
	{
		double		n_tuples = 0;

		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_SORT_TUPLES);
		tuplesort_performsort(tuplesort);
		pgstat_progress_update_param(PROGRESS_REPACK_PHASE,
									 PROGRESS_REPACK_PHASE_WRITE_NEW_HEAP);

		for (;;)
		{
			HeapTuple	tuple;
			FluxRewriteVersion version;

			CHECK_FOR_INTERRUPTS();

			tuple = tuplesort_getheaptuple(tuplesort, true);
			if (tuple == NULL)
				break;
			flux_cluster_sorted_version(tuple->t_data, &version);
			ExecForceStoreHeapTuple(tuple, slot, false);
			flux_cluster_write(NewTable, slot, &version);
			pgstat_progress_update_param(PROGRESS_REPACK_HEAP_TUPLES_INSERTED,
										 ++n_tuples);
		}
		tuplesort_end(tuplesort);
	}

	ExecDropSingleTupleTableSlot(vslot);
	ExecDropSingleTupleTableSlot(slot);

	/* Return statistics to caller */
	*num_tuples = live_tuples;
	*tups_vacuumed = dead_tuples;
	*tups_recently_dead = recent_dead;
}

/*
 * Build range scan for index creation
 *
 * Scans the FLUX table and feeds tuples to the index AM's callback for
 * index building.  Handles partial indexes, expression indexes, uniqueness
 * checking, concurrent index builds, and proper visibility classification.
 *
 * Modeled on heapam_index_build_range_scan().
 */
static double
flux_index_build_range_scan(Relation tablerel, Relation indexrel,
							IndexInfo *indexInfo, bool allow_sync,
							bool anyvisible, bool progress,
							BlockNumber start_blockno, BlockNumber numblocks,
							IndexBuildCallback callback, void *callback_state,
							TableScanDesc scan)
{
	double		reltuples = 0;
	bool		checking_uniqueness;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	Snapshot	snapshot;
	bool		need_unregister_snapshot = false;

	/* See whether we're verifying uniqueness/exclusion properties */
	checking_uniqueness = (indexInfo->ii_Unique ||
						   indexInfo->ii_ExclusionOps != NULL);

	/* "Any visible" mode is not compatible with uniqueness checks */
	Assert(!(anyvisible && checking_uniqueness));

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.  Also a slot to hold the current tuple.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(tablerel, NULL);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up execution state for predicate, if any */
	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Prepare for scan.  Normal index build uses SnapshotAny (we do our own
	 * visibility checks).  Concurrent/bootstrap uses an MVCC snapshot.
	 */
	if (!scan)
	{
		if (IsBootstrapProcessingMode() || indexInfo->ii_Concurrent)
		{
			snapshot = RegisterSnapshot(GetTransactionSnapshot());
			need_unregister_snapshot = true;
		}
		else
			snapshot = SnapshotAny;

		scan = table_beginscan_strat(tablerel, snapshot, 0, NULL,
									 true, allow_sync);
	}
	else
	{
		snapshot = scan->rs_snapshot;
	}

	/* Recently dead rows, and those of aborted DELETEs; see below. */
	if (snapshot == SnapshotAny)
		((FluxScanDesc) scan)->rs_keep_deleted = true;

	/* Scan only the blocks asked for, as heap does (BRIN summarization). */
	if (!allow_sync)
		flux_scan_setlimits(scan, start_blockno, numblocks);
	else
	{
		/* syncscan can only be requested on whole relation */
		Assert(start_blockno == 0);
		Assert(numblocks == InvalidBlockNumber);
	}

	/* Scan all tuples in the base relation */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		tupleIsAlive;

		CHECK_FOR_INTERRUPTS();

		if (snapshot == SnapshotAny)
		{
			/*
			 * Classify the tuple using FLUX's timestamp-based MVCC by
			 * re-reading the tuple header from the page.
			 */
			Buffer		buf;
			Page		page;
			ItemId		itemid;
			FluxTupleHeader *tuple_hdr = NULL;
			BlockNumber blkno = ItemPointerGetBlockNumber(&slot->tts_tid);
			OffsetNumber offnum = ItemPointerGetOffsetNumber(&slot->tts_tid);
			bool		indexIt;
			FluxPendingVersion pending;

			buf = ReadBuffer(tablerel, blkno);
			/* Share-exclusive, for flux_pending_version and the copy below. */
			LockBuffer(buf, BUFFER_LOCK_SHARE_EXCLUSIVE);
			page = BufferGetPage(buf);

			/*
			 * A row whose version is an aborted transaction's, with the
			 * revert still pending, is indexed as the revert will leave it:
			 * an aborted UPDATE's row at its committed version, an aborted
			 * DELETE's row (the scan hands those over too, see
			 * rs_keep_deleted) as it was before, and an aborted INSERT's row
			 * not at all.  Indexing the aborted version would leave the index
			 * wrong for good, as the revert reverses only the index changes
			 * the aborted transaction made itself.  The slot then holds the
			 * version to index.
			 */
			pending = flux_pending_version(tablerel, buf, offnum, slot);
			itemid = PageGetItemId(page, offnum);
			if (pending == FLUX_PENDING_GONE ||
				(pending == FLUX_PENDING_NONE && !ItemIdIsNormal(itemid)))
			{
				UnlockReleaseBuffer(buf);
				continue;
			}

			if (pending == FLUX_PENDING_NONE)
			{
				/*
				 * Index the version on the page now, not the scan's copy of
				 * it: a revert may have run since.
				 */
				tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);
				FluxSlotStoreTuple(slot, tuple_hdr, ItemIdGetLength(itemid),
								   buf);
				slot->tts_tableOid = RelationGetRelid(tablerel);
				ItemPointerSet(&slot->tts_tid, blkno, offnum);
			}

			if (pending == FLUX_PENDING_RECONSTRUCTED)
			{
				/* Committed (or our own) version: live */
				indexIt = true;
				tupleIsAlive = true;
				reltuples += 1;
			}
			else if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
			{
				if (FluxTupleDeadToAll(tuple_hdr, FluxGetOldestXminHorizon(tablerel)))
				{
					/* Definitely dead -- skip */
					UnlockReleaseBuffer(buf);
					continue;
				}
				else
				{
					/* Recently dead -- index for MVCC, don't count */
					indexIt = true;
					tupleIsAlive = false;
				}
			}
			else if (tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE)
			{
				/* Speculative insertion not yet confirmed -- skip */
				UnlockReleaseBuffer(buf);
				continue;
			}
			else if (!UndoRecPtrIsValid(tuple_hdr->t_verptr) &&
					 !(tuple_hdr->t_flags & FLUX_TUPLE_XMIN_COMMITTED) &&
					 !TransactionIdIsCurrentTransactionId(FluxTupleGetXmin(tuple_hdr)))
			{
				/*
				 * Version written by an INSERT (no UNDO history), by another
				 * transaction: its inserter decides, as for heap.  An aborted
				 * INSERT's row is dead (only VACUUM removes it).  An
				 * in-progress one is indexed, but when checking uniqueness we
				 * first wait for it, exactly like heap.  The caller's
				 * ShareLock keeps any new inserter from starting.
				 */
				TransactionId xmin = FluxTupleGetXmin(tuple_hdr);

				UnlockReleaseBuffer(buf);

				/*
				 * In "any visible" mode (BRIN summarization) the row is
				 * simply indexed, with no warning, as heap does.
				 */
				if (!anyvisible && TransactionIdIsInProgress(xmin))
				{
					elog(WARNING, "concurrent insert in progress within table \"%s\"",
						 RelationGetRelationName(tablerel));
					if (checking_uniqueness)
					{
						XactLockTableWait(xmin, tablerel, &slot->tts_tid,
										  XLTW_InsertIndexUnique);
						CHECK_FOR_INTERRUPTS();
					}
				}
				/* In progress first: it may commit between the two tests. */
				if (TransactionIdIsInProgress(xmin))
				{
					indexIt = true;
					tupleIsAlive = true;
				}
				else if (TransactionIdDidCommit(xmin))
				{
					indexIt = true;
					tupleIsAlive = true;
					reltuples += 1;
				}
				else
					continue;	/* aborted: never visible to anyone */
				buf = InvalidBuffer;
			}
			else
			{
				/* Live tuple -- index and count it */
				indexIt = true;
				tupleIsAlive = true;
				reltuples += 1;
			}

			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);

			if (!indexIt)
				continue;
		}
		else
		{
			/* MVCC snapshot already filtered for visibility */
			tupleIsAlive = true;
			reltuples += 1;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		/* In a partial index, discard tuples that don't satisfy predicate */
		if (predicate != NULL)
		{
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * Extract all indexed attributes.  This also evaluates any index
		 * expressions.
		 */
		FormIndexDatum(indexInfo, slot, estate, values, isnull);

		/*
		 * Call the AM's callback with the tuple's own TID.  FLUX secondary
		 * indexes hold plain 6-byte heap-style TIDs (no RowID/gen suffix), so
		 * this matches the stock heap index-build contract exactly.
		 */
		callback(indexrel, &slot->tts_tid,
				 values, isnull, tupleIsAlive, callback_state);
	}

	table_endscan(scan);

	if (need_unregister_snapshot)
		UnregisterSnapshot(snapshot);

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	/* These may have been pointing to the now-gone estate */
	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;

	return reltuples;
}

/*
 * Validate scan for index
 */
static void
flux_index_validate_scan(Relation tablerel, Relation indexrel,
						 IndexInfo *indexInfo, Snapshot snapshot,
						 ValidateIndexState *state)
{
	TableScanDesc scan;
	TupleTableSlot *slot;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	ExprState  *predicate;
	EState	   *estate;
	ExprContext *econtext;
	ItemPointer indexcursor = NULL;
	ItemPointerData decoded;
	bool		tuplesort_empty = false;

	/*
	 * Need an EState for evaluation of index expressions and partial-index
	 * predicates.
	 */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(tablerel, NULL);
	econtext->ecxt_scantuple = slot;

	predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/*
	 * Scan the table and the sorted output from tuplesort in parallel. For
	 * each table tuple, check if there's a matching index entry. Tuples that
	 * satisfy the predicate but have no index entry need to be inserted into
	 * the index.
	 */
	scan = table_beginscan_strat(tablerel, snapshot, 0, NULL, true, false);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();

		state->htups += 1;

		/*
		 * Skip tuples that don't satisfy the partial index predicate.
		 */
		if (predicate != NULL)
		{
			MemoryContextReset(econtext->ecxt_per_tuple_memory);
			if (!ExecQual(predicate, econtext))
				continue;
		}

		/*
		 * Advance the tuplesort cursor past any entries that are for TIDs
		 * earlier than the current table tuple.
		 */
		while (!tuplesort_empty &&
			   (!indexcursor ||
				ItemPointerCompare(indexcursor, &slot->tts_tid) < 0))
		{
			Datum		ts_val;
			bool		ts_isnull;

			tuplesort_empty = !tuplesort_getdatum(state->tuplesort,
												  true, false,
												  &ts_val, &ts_isnull,
												  NULL);
			Assert(tuplesort_empty || !ts_isnull);
			if (!tuplesort_empty)
			{
				itemptr_decode(&decoded, DatumGetInt64(ts_val));
				indexcursor = &decoded;
			}
			else
			{
				indexcursor = NULL;
			}
		}

		/*
		 * If the sorted cursor TID matches the current table tuple, the index
		 * already has this entry.  Otherwise, we need to add it.
		 */
		if (indexcursor != NULL &&
			ItemPointerCompare(indexcursor, &slot->tts_tid) == 0)
		{
			/* Already in the index -- skip */
			continue;
		}

		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		FormIndexDatum(indexInfo, slot, estate, values, isnull);

		/*
		 * Insert the missing index entry using the tuple's own TID.
		 */
		index_insert(indexrel, values, isnull, &slot->tts_tid,
					 tablerel, indexInfo->ii_Unique ?
					 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
					 false, indexInfo);

		state->tups_inserted += 1;
	}

	/*
	 * Release any AM-cached insert state (e.g. BRIN's pinned revmap buffer in
	 * ii_AmCache) before we finish.  See the note in
	 * flux_inplace_index_maintenance.
	 */
	index_insert_cleanup(indexrel, indexInfo);

	table_endscan(scan);

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	indexInfo->ii_ExpressionsState = NIL;
	indexInfo->ii_PredicateState = NULL;
}

/*
 * Get relation size information
 *
 * Returns the on-disk size in bytes for the specified fork of the relation.
 * This is used by pg_relation_size(), VACUUM, CLUSTER, and many other
 * operations that need to know the physical storage footprint.
 */
/*
 * Use table_block_relation_size() from tableam.c directly.  FLUX uses
 * standard BLCKSZ-width forks just like heap, so the generic
 * implementation is correct and efficient (no smgrexists() overhead).
 */

/*
 * Check if relation needs a TOAST table
 */
static bool
flux_relation_needs_toast_table(Relation rel)
{
	/*
	 * FLUX uses standard heap TOAST for wide values (it has no on-page
	 * overflow mechanism).  This mirrors heapam_relation_needs_toast_table: a
	 * TOAST table is needed iff there is a toastable attribute and the
	 * maximum tuple length could exceed TOAST_TUPLE_THRESHOLD.
	 */
	int32		data_length = 0;
	bool		maxlength_unknown = false;
	bool		has_toastable_attrs = false;
	TupleDesc	tupdesc = rel->rd_att;
	int32		tuple_length;
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			continue;
		data_length = att_align_nominal(data_length, att->attalign);
		if (att->attlen > 0)
		{
			data_length += att->attlen;
		}
		else
		{
			int32		maxlen = type_maximum_size(att->atttypid,
												   att->atttypmod);

			if (maxlen < 0)
				maxlength_unknown = true;
			else
				data_length += maxlen;
			if (att->attstorage != TYPSTORAGE_PLAIN)
				has_toastable_attrs = true;
		}
	}
	if (!has_toastable_attrs)
		return false;			/* nothing to toast? */
	if (maxlength_unknown)
		return true;			/* any unlimited-length attrs? */
	tuple_length = MAXALIGN(FLUX_TUPLE_OVERHEAD +
							BITMAPLEN(tupdesc->natts)) +
		MAXALIGN(data_length);
	return (tuple_length > TOAST_TUPLE_THRESHOLD);
}

/*
 * TOAST tables for FLUX relations are ordinary heap relations (FLUX reuses
 * the standard heap TOAST machinery).
 */
static Oid
flux_relation_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

/*
 * Estimate relation size
 *
 * Heap's block-oriented estimator, with FLUX's page geometry: it reads no
 * buffer (only the relation's block count and pg_class), believes a
 * vacuumed empty table, assumes a never-vacuumed one has at least 10 pages,
 * and scales reltuples/relpages to the current size.
 *
 * Every FLUX tuple carries the fixed header plus a null bitmap for all of
 * its attributes, NULLs or not (FluxComputeDataSize), and the data starts
 * MAXALIGNed after it.  Pages carry FLUX's special space and, without a
 * fillfactor reloption, are filled to FLUX_DEFAULT_FILLFACTOR, as
 * FluxTargetFreeSpace() does on insert.
 */
static void
flux_relation_estimate_size(Relation rel, int32 *attr_widths,
							BlockNumber *pages, double *tuples,
							double *allvisfrac)
{
	Size		overhead;

	overhead = MAXALIGN(SizeOfFluxTupleHeader +
						BITMAPLEN(RelationGetNumberOfAttributes(rel))) +
		sizeof(ItemIdData);
	table_block_relation_estimate_size_extended(rel, attr_widths, pages,
												tuples, allvisfrac,
												overhead,
												BLCKSZ - FLUX_PAGE_OVERHEAD,
												FLUX_DEFAULT_FILLFACTOR);
}

/*
 * Sample scan: get next block for sampling (TABLESAMPLE support)
 *
 * Called by the TABLESAMPLE executor to prepare the next block for tuple
 * extraction.  The TSM (Table Sample Method) decides which block to visit
 * via its NextSampleBlock callback, or, if that callback is NULL, we scan
 * sequentially starting from rs_startblock and wrapping around.
 *
 * We read the selected block into a buffer (pinned, not locked -- locking
 * is deferred to flux_scan_sample_next_tuple) and return true.  Returns
 * false when there are no more blocks to sample.
 */
static bool
flux_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	FluxScanDesc rscan = (FluxScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno;

	/* Return false immediately if relation is empty */
	if (rscan->rs_nblocks == 0)
		return false;

	/* Release previous buffer, if any */
	if (BufferIsValid(rscan->rs_cbuf))
	{
		ReleaseBuffer(rscan->rs_cbuf);
		rscan->rs_cbuf = InvalidBuffer;
	}

	if (tsm->NextSampleBlock)
	{
		/* TSM tells us which block to visit next */
		blockno = tsm->NextSampleBlock(scanstate, rscan->rs_nblocks);
	}
	else
	{
		/* No NextSampleBlock callback -- scan sequentially */
		if (rscan->rs_cblock == InvalidBlockNumber)
		{
			Assert(!rscan->rs_inited);
			blockno = rscan->rs_startblock;
		}
		else
		{
			Assert(rscan->rs_inited);

			blockno = rscan->rs_cblock + 1;

			if (blockno >= rscan->rs_nblocks)
			{
				/* Wrap to beginning of relation */
				blockno = 0;
			}

			if (blockno == rscan->rs_startblock)
			{
				/* Completed full cycle -- done */
				blockno = InvalidBlockNumber;
			}
		}
	}

	rscan->rs_cblock = blockno;

	if (!BlockNumberIsValid(blockno))
	{
		rscan->rs_inited = false;
		return false;
	}

	Assert(rscan->rs_cblock < rscan->rs_nblocks);

	CHECK_FOR_INTERRUPTS();

	/* Read the selected block -- comes back pinned but not locked */
	rscan->rs_cbuf = ReadBufferExtended(scan->rs_rd, MAIN_FORKNUM,
										blockno, RBM_NORMAL, NULL);

	rscan->rs_inited = true;
	return true;
}

/*
 * Sample scan: get next tuple from current block (TABLESAMPLE support)
 *
 * Called repeatedly for the block prepared by flux_scan_sample_next_block().
 * The TSM's NextSampleTuple callback decides which tuple offsets to examine.
 * We lock the buffer, check the tuple at the selected offset for visibility,
 * and either return it in the slot (true) or indicate end-of-page (false).
 *
 * Unlike the ANALYZE path which iterates all items sequentially, here the
 * TSM picks specific offsets, and we loop until it returns InvalidOffsetNumber
 * to signal that it is done with this block.
 */
static bool
flux_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate,
							TupleTableSlot *slot)
{
	FluxScanDesc rscan = (FluxScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber blockno = rscan->rs_cblock;
	Page		page;
	OffsetNumber maxoffset;

	/*
	 * Lock the buffer for visibility checks.  We hold the lock for the
	 * duration of this call and release before returning, matching the heap
	 * AM's non-pagemode pattern.  Share-exclusive excludes in-place writers
	 * while we decide and copy (FluxSlotStoreTuple).
	 */
	LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_SHARE_EXCLUSIVE);

	page = BufferGetPage(rscan->rs_cbuf);
	maxoffset = PageGetMaxOffsetNumber(page);

	for (;;)
	{
		OffsetNumber tupoffset;
		ItemId		itemid;
		FluxTupleHeader *tuple_hdr;
		bool		visible;

		CHECK_FOR_INTERRUPTS();

		/* Ask the TSM which tuple to examine next on this page */
		tupoffset = tsm->NextSampleTuple(scanstate, blockno, maxoffset);

		if (OffsetNumberIsValid(tupoffset))
		{
			/* Skip invalid item pointers */
			itemid = PageGetItemId(page, tupoffset);
			if (!ItemIdIsNormal(itemid))
				continue;

			tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

			/* Skip overflow records -- not user-visible tuples */
			if (FluxIsOverflowRecordInline(tuple_hdr, ItemIdGetLength(itemid)))
				continue;

			/*
			 * Determine visibility.  FLUX uses heap-shaped xmin/xmax MVCC via
			 * FluxTupleVisibleToSnapshotDual, which handles DELETED/UPDATED
			 * tuples via sLog consultation.
			 */
			if (tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE)
				visible = false;
			else if (scan->rs_snapshot)
			{
				ItemPointerData self_tid;

				ItemPointerSet(&self_tid, blockno, tupoffset);
				visible = FluxTupleVisibleToSnapshotDual(tuple_hdr,
														 scan->rs_snapshot,
														 RelationGetRelid(scan->rs_rd),
														 rscan->rs_cbuf,
														 &self_tid);
			}
			else
				visible = !(tuple_hdr->t_flags & FLUX_TUPLE_DELETED);

			/* SSI, as heap's sample scan; beginscan locked the relation */
			if (!(tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE))
			{
				ItemPointerData stid;

				ItemPointerSet(&stid, blockno, tupoffset);
				flux_ssi_read(scan->rs_rd, &stid, tuple_hdr, rscan->rs_cbuf,
							  scan->rs_snapshot, visible, false);
			}

			if (!visible && !(tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE))
			{
				/* An older version, from UNDO, may be the snapshot's. */
				ItemPointerData stid;
				bool		unlocked;
				bool		stored;

				ItemPointerSet(&stid, blockno, tupoffset);
				stored = flux_store_visible_older(scan->rs_rd, rscan->rs_cbuf,
												  &stid, tuple_hdr,
												  ItemIdGetLength(itemid),
												  scan->rs_snapshot, slot,
												  &unlocked);
				if (stored)
					return true;
				if (unlocked)
				{
					LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_SHARE_EXCLUSIVE);
					page = BufferGetPage(rscan->rs_cbuf);
					maxoffset = PageGetMaxOffsetNumber(page);
				}
			}

			if (!visible)
				continue;

			/*
			 * Found a visible tuple: copy it into the slot
			 * (FluxSlotStoreTuple).
			 */
			FluxSlotStoreTuple(slot, tuple_hdr,
							   ItemIdGetLength(itemid), rscan->rs_cbuf);
			slot->tts_tableOid = RelationGetRelid(scan->rs_rd);
			ItemPointerSet(&slot->tts_tid, blockno, tupoffset);
			LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			return true;
		}
		else
		{
			/*
			 * NextSampleTuple returned InvalidOffsetNumber -- done with this
			 * block.  Unlock, clear the slot, and tell the caller to move on.
			 */
			LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
			ExecClearTuple(slot);
			return false;
		}
	}

	/* unreachable */
	Assert(false);
}

/*
 * ------------------------------------------------------------------------
 * Main table AM routine structure for FLUX
 * ------------------------------------------------------------------------
 */
static const TableAmRoutine flux_methods = {
	.type = T_TableAmRoutine,

	/*
	 * FLUX table DML records UNDO into the per-backend UNDO engine
	 * (access/undo/perbackend): flux_operations.c prepares one
	 * UnpackedUndoRecord per INSERT/UPDATE/DELETE via the flux_pbu.c shim
	 * (PrepareUndoInsert/InsertPreparedUndo), tagged uur_rmid =
	 * UNDO_RMID_FLUX, folding the undo page onto the FLUX forward WAL record
	 * as a full-page image for crash-safety.  On abort AbortTransaction() ->
	 * PbuAtAbort_ApplyUndo() -> execute_undo_actions() dispatches each record
	 * to flux_undo_apply() (the FLUX undo rmgr's rm_undo) which restores the
	 * before-image in place.
	 *
	 * FLUX declares am_undo_engine = UNDO_ENGINE_PERBACKEND and
	 * am_index_delete_marking = true.  An indexed-column UPDATE stays IN
	 * PLACE (stable TID): flux_tuple_update inserts the new (k_new,TID) index
	 * entry and delete-marks the old (k_old,TID) entry (index_delete_mark),
	 * reporting TU_None so the executor does not re-insert.  There is NO
	 * separate index UNDO: nbtree/hash suppress their index-UNDO writes for a
	 * delete-marking parent (RelationSupportsDeleteMarking) because FLUX's
	 * own table UNDO (flux_undo_apply, FLUX_UNDO_UPDATE) drives index cleanup
	 * on rollback -- it clears the old delete-mark and removes the new
	 * (k_new,TID) entry, restoring the index to its exact pre-update state.
	 * So no index UNDO is written for a FLUX table's indexes.
	 */
	.am_supports_undo = true,
	/* updates in place at a stable TID; never reports traversed */
	.am_inplace_update_keeps_tid = true,
	.am_undo_engine = UNDO_ENGINE_PERBACKEND,
	.am_index_delete_marking = true,
	/* every committed row change is decoded; see flux_decode */
	.am_logical_decoding = true,


	/* Use minimal tuple slot */
	.slot_callbacks = flux_slot_callbacks,

	.scan_begin = flux_scan_begin,
	.scan_end = flux_scan_end,
	.scan_rescan = flux_scan_rescan,
	.scan_getnextslot = flux_scan_getnextslot,

	.scan_set_tidrange = flux_scan_set_tidrange,
	.scan_getnextslot_tidrange = flux_scan_getnextslot_tidrange,

	.parallelscan_estimate = table_block_parallelscan_estimate,
	.parallelscan_initialize = table_block_parallelscan_initialize,
	.parallelscan_reinitialize = table_block_parallelscan_reinitialize,

	/* Use minimal index functions */
	.index_scan_begin = flux_index_scan_begin,
	.index_scan_reset = flux_index_scan_reset,
	.index_scan_end = flux_index_scan_end,

	/* Use minimal tuple functions */
	.tuple_insert = flux_tuple_insert,
	.tuple_insert_speculative = flux_tuple_insert_speculative,
	.tuple_complete_speculative = flux_tuple_complete_speculative,
	.multi_insert = flux_multi_insert,
	.tuple_delete = flux_tuple_delete,
	.tuple_update = flux_tuple_update,
	.tuple_lock = flux_tuple_lock,

	/* UNDO write-buffer activation / deactivation */

	.fetch_tid = flux_fetch_tid,
	.index_fetch_tid = flux_index_fetch_tid,
	.tuple_fetch_row_version = flux_tuple_fetch_row_version,
	.tuple_get_latest_tid = flux_tuple_get_latest_tid,
	.tuple_tid_valid = flux_tuple_tid_valid,
	.tuple_satisfies_snapshot = flux_tuple_satisfies_snapshot,
	.index_delete_tuples = flux_index_delete_tuples,
	.index_tombstone_removable = flux_index_tombstone_removable,

	/* Keep only essential relation functions */
	.relation_set_new_filelocator = flux_relation_set_new_filelocator,
	.relation_nontransactional_truncate = flux_relation_nontransactional_truncate,
	.relation_copy_data = flux_relation_copy_data,
	.relation_copy_for_cluster = flux_relation_copy_for_cluster,
	.relation_vacuum = flux_relation_vacuum,
	.scan_analyze_next_block = flux_scan_analyze_next_block,
	.scan_analyze_next_tuple = flux_scan_analyze_next_tuple,
	.index_build_range_scan = flux_index_build_range_scan,
	.index_validate_scan = flux_index_validate_scan,

	.relation_size = table_block_relation_size,
	.relation_needs_toast_table = flux_relation_needs_toast_table,
	.relation_toast_am = flux_relation_toast_am,
	.relation_fetch_toast_slice = heap_fetch_toast_slice,

	.relation_estimate_size = flux_relation_estimate_size,

	.scan_bitmap_next_tuple = flux_scan_bitmap_next_tuple,
	.scan_sample_next_block = flux_scan_sample_next_block,
	.scan_sample_next_tuple = flux_scan_sample_next_tuple,

	/*
	 * FLUX secondary indexes hold plain 6-byte heap-style TID entries and are
	 * maintained exactly like heap indexes.  A key-changing UPDATE stores the
	 * new version out of place (new TID) so the old (key, TID) index entry
	 * becomes dead and is reclaimed by VACUUM, never duplicated -- see
	 * flux_tuple_update().
	 */
};

/*
 * Return the FLUX table AM routine
 */
const TableAmRoutine *
GetFluxTableAmRoutine(void)
{
	return &flux_methods;
}

/*
 * Handler function for FLUX table access method
 */
Datum
flux_tableam_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(&flux_methods);
}

/*
 * flux_analyze_accumulate_sample
 *
 * During an ANALYZE scan, capture the decompressed bytes of the relation's
 * first varlena column from the just-materialized slot.  These samples feed
 * FluxMaybeRefreshDict() at scan end so it can train a candidate compression
 * dictionary from data ANALYZE already sampled, with no extra table reads.
 *
 * Sample buffers are allocated lazily in the scan's own memory context and
 * are bounded by fixed byte/count caps.  Once a cap is hit we stop collecting
 * but let the ANALYZE scan continue normally.
 */
static void
flux_analyze_accumulate_sample(FluxScanDesc rscan, TupleTableSlot *slot)
{
	/*
	 * FLUX does not compress attributes and has no compression dictionary, so
	 * ANALYZE does not accumulate a training corpus.
	 */
	(void) rscan;
	(void) slot;
}

/*
 * ANALYZE support: select next block to sample
 *
 * Called by ANALYZE to prepare the next sampled block for tuple extraction.
 * The ReadStream provides buffers for blocks selected by the BlockSampler
 * in analyze.c -- we do not choose blocks ourselves.
 *
 * We acquire a buffer pin and shared lock here and hold them until
 * flux_scan_analyze_next_tuple() has returned false for this block,
 * preventing concurrent activity (e.g. pruning) from removing tuples
 * out from under us.
 */
static bool
flux_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	FluxScanDesc rscan = (FluxScanDesc) scan;

	/*
	 * Get the next buffer from the read stream.  The stream was set up by
	 * analyze.c with a BlockSampler callback, so it yields only the randomly
	 * selected sample blocks.  The buffer comes back already pinned.
	 */
	rscan->rs_cbuf = read_stream_next_buffer(stream, NULL);

	if (!BufferIsValid(rscan->rs_cbuf))
		return false;

	/*
	 * Don't lock the buffer here; flux_scan_analyze_next_tuple() manages its
	 * own lock/unlock cycle so it can release the lock before returning a
	 * sampled tuple, allowing FluxFetchOverflowColumn() to safely lock the
	 * same buffer for overflow data on this page.
	 */

	rscan->rs_cblock = BufferGetBlockNumber(rscan->rs_cbuf);
	rscan->rs_cindex = FirstOffsetNumber;

	return true;
}

/*
 * ANALYZE support: get next tuple from current block
 *
 * Extracts tuples one at a time from the block prepared by
 * flux_scan_analyze_next_block().  For each item pointer on the page we
 * classify the tuple as live, dead, or not-a-tuple (overflow record,
 * unused pointer) and update the caller's counters.
 *
 * When a live tuple suitable for sampling is found, it is materialized
 * into the slot and we return true.  When all items on the page have been
 * examined, we release the buffer and return false.
 *
 * The buffer remains pinned and locked for the entire duration of tuple
 * iteration on this block, matching the heap AM contract.
 */
static bool
flux_scan_analyze_next_tuple(TableScanDesc scan,
							 double *liverows, double *deadrows,
							 TupleTableSlot *slot)
{
	FluxScanDesc rscan = (FluxScanDesc) scan;
	Page		targpage;
	OffsetNumber maxoffset;

	Assert(BufferIsValid(rscan->rs_cbuf));

	/*
	 * Re-acquire the buffer content lock.  We release it before returning a
	 * sampled tuple (see below) so that the caller can safely deform the
	 * tuple -- FluxFetchOverflowColumn() may need to lock the same buffer to
	 * read overflow data stored on the same page.  Share-exclusive: the copy
	 * below must exclude in-place writers (see FluxSlotStoreTuple).
	 */
	LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_SHARE_EXCLUSIVE);

	targpage = BufferGetPage(rscan->rs_cbuf);
	maxoffset = PageGetMaxOffsetNumber(targpage);

	/* Inner loop over items on the selected page */
	for (; rscan->rs_cindex <= maxoffset; rscan->rs_cindex++)
	{
		ItemId		itemid;
		FluxTupleHeader *tuple_hdr;
		bool		sample_it = false;
		bool		aborted_update;

		itemid = PageGetItemId(targpage, rscan->rs_cindex);

		/*
		 * Skip unused and dead line pointers.  Dead line pointers are counted
		 * as dead rows because vacuum needs to reclaim them.
		 */
		if (!ItemIdIsNormal(itemid))
		{
			if (ItemIdIsDead(itemid))
				*deadrows += 1;
			continue;
		}

		tuple_hdr = (FluxTupleHeader *) PageGetItem(targpage, itemid);

		/* Skip overflow records -- these are not user-visible tuples */
		if (FluxIsOverflowRecordInline(tuple_hdr, ItemIdGetLength(itemid)))
			continue;

		/*
		 * A row whose version is an aborted transaction's, with the revert
		 * still pending, is sampled as the revert will leave it (see
		 * flux_pending_version): an aborted UPDATE's or DELETE's row at its
		 * committed version, which flux_pending_version stores into slot. An
		 * aborted INSERT's row is dead, as for heap, and so is the version an
		 * aborted UPDATE left, while an aborted DELETE leaves none.
		 */
		aborted_update = !(tuple_hdr->t_flags & FLUX_TUPLE_DELETED);
		switch (flux_pending_version(scan->rs_rd, rscan->rs_cbuf,
									 rscan->rs_cindex, slot))
		{
			case FLUX_PENDING_RECONSTRUCTED:
				*liverows += 1;
				if (aborted_update)
					*deadrows += 1;
				rscan->rs_cindex++;
				LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
				if (scan->rs_flags & SO_TYPE_ANALYZE)
					flux_analyze_accumulate_sample(rscan, slot);
				return true;
			case FLUX_PENDING_GONE:
				*deadrows += 1;
				maxoffset = PageGetMaxOffsetNumber(targpage);
				continue;
			case FLUX_PENDING_NONE:
				break;
		}

		/* The lock may have been released meanwhile: read the item again. */
		maxoffset = PageGetMaxOffsetNumber(targpage);
		itemid = PageGetItemId(targpage, rscan->rs_cindex);
		if (!ItemIdIsNormal(itemid))
			continue;
		tuple_hdr = (FluxTupleHeader *) PageGetItem(targpage, itemid);

		/*
		 * Count the row as heap's ANALYZE does (FluxTupleStatsCount): a row
		 * another transaction is inserting is neither counted nor sampled,
		 * nor is a speculative insertion not yet confirmed; a row another
		 * transaction is deleting is live and sampled.
		 *
		 * An in-place UPDATE leaves no dead tuple: its before-image is in
		 * UNDO, which the undo discard worker reclaims without VACUUM.  So a
		 * row with UNDO history is one live row, not also a dead one.
		 */
		if ((tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE) &&
			!(tuple_hdr->t_flags & FLUX_TUPLE_DELETED))
			continue;
		switch (FluxTupleStatsCount(tuple_hdr))
		{
			case FLUX_COUNT_LIVE:
				sample_it = true;
				*liverows += 1;
				break;
			case FLUX_COUNT_DEAD:
				*deadrows += 1;
				break;
			case FLUX_COUNT_NONE:
				break;
		}

		if (sample_it)
		{
			/*
			 * Materialize the tuple into palloc'd memory rather than storing
			 * a buffer-pinned pointer.  This is necessary because slot
			 * deformation may call FluxFetchOverflowColumn(), which acquires
			 * buffer locks on overflow pages.  If the overflow data resides
			 * on the same page we are scanning, a buffer-pinned slot would
			 * cause a lock re-entry assertion failure in LockBuffer
			 * (bufmgr.c).
			 */
			FluxSlotStoreTuple(slot, tuple_hdr, ItemIdGetLength(itemid),
							   rscan->rs_cbuf);
			slot->tts_tableOid = RelationGetRelid(scan->rs_rd);
			ItemPointerSet(&slot->tts_tid, rscan->rs_cblock, rscan->rs_cindex);
			rscan->rs_cindex++;

			/*
			 * Release the content lock so the caller can safely deform the
			 * materialized tuple.  The buffer pin is kept so the page stays
			 * in the buffer pool.  We re-acquire the lock at the top of this
			 * function when called again.
			 */
			LockBuffer(rscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			/*
			 * Opportunistically feed the dictionary-refresh corpus from the
			 * same sampled tuple.  Done after the unlock so any
			 * overflow-column fetch can lock this page safely.
			 */
			if (scan->rs_flags & SO_TYPE_ANALYZE)
				flux_analyze_accumulate_sample(rscan, slot);

			return true;
		}
	}

	/*
	 * No more tuples on this page.  Release the buffer pin and lock that were
	 * acquired in flux_scan_analyze_next_block().
	 */
	UnlockReleaseBuffer(rscan->rs_cbuf);
	rscan->rs_cbuf = InvalidBuffer;

	/* Prevent stale slot contents from holding a pin */
	ExecClearTuple(slot);

	return false;
}
