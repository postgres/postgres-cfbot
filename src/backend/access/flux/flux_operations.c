/*-------------------------------------------------------------------------
 *
 * flux_operations.c
 *	  FLUX table manipulation operations
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_operations.c
 *
 * NOTES
 *	  This implements the remaining table manipulation operations for
 *	  FLUX storage manager including insert, update, delete, and
 *	  various DDL operations.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/visibilitymap.h"
#include "access/heaptoast.h"
#include "access/flux.h"
#include "access/slog.h"
#include "access/subtrans.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/flux_undo.h"
#include "access/flux_pbu.h"
#include "access/flux_xlog.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "access/tidstore.h"
#include "access/xact.h"
#include "access/xactundo.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "catalog/storage.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/latch.h"
#include "storage/read_stream.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"
#include "storage/bufpage.h"
#include "miscadmin.h"

/*
 * A copied before-image may outlive the CLOG segment containing its xmin.
 * Normalize it before full-image logging or delta encoding.  No snapshot can
 * need its predecessors once its committed writer is globally removable.
 * Own/uncommitted writers must retain both their identity and history.
 */
/* The version FLUX_INSERT_REWRITE copies; see FluxSetRewriteVersion. */
static FluxRewriteVersion flux_rewrite_version;
static bool flux_rewrite_version_set = false;

/*
 * FluxSetRewriteVersion
 *		Name the writers of the version the next FLUX_INSERT_REWRITE insert
 *		copies.  table_tuple_insert has no argument for them.
 */
void
FluxSetRewriteVersion(const FluxRewriteVersion *v)
{
	flux_rewrite_version = *v;
	flux_rewrite_version_set = true;
}

static TransactionId
flux_prepare_before_image(Relation rel, FluxTupleHeader *tuple)
{
	TransactionId xmin = FluxTupleGetXmin(tuple);

	if (!(tuple->t_flags & (FLUX_TUPLE_DELETED | FLUX_TUPLE_SPECULATIVE)) &&
		!TransactionIdIsValid(FluxTupleGetXmax(tuple)) &&
		TransactionIdIsNormal(xmin) &&
		TransactionIdPrecedes(xmin, GetOldestNonRemovableTransactionId(rel)) &&
		TransactionIdDidCommit(xmin))
	{
		FluxTupleSetXmin(tuple, FrozenTransactionId);
		tuple->t_verptr = InvalidUndoRecPtr;
		tuple->t_flags &= ~(FLUX_TUPLE_UNCOMMITTED | FLUX_TUPLE_HAS_VERSION_PTR);
		tuple->t_flags |= FLUX_TUPLE_XMIN_COMMITTED;
		return xmin;
	}
	return InvalidTransactionId;
}

/*
 * Maximum overflow pointers per tuple for VACUUM overflow cleanup.
 * This limits memory usage during VACUUM and is conservative since most
 * tuples won't have overflow data. 128 is sufficient for typical workloads.
 */
#define MAX_OVERFLOW_PTRS_PER_TUPLE 128

/* Function prototypes for locking */
extern bool FluxLockTuple(Relation rel, ItemPointer tid, LockTupleMode mode,
						  bool wait, bool *have_tuple_lock);
extern void FluxUnlockTuple(Relation rel, ItemPointer tid, LockTupleMode mode);
extern void FluxLockPage(Relation rel, BlockNumber blkno, LOCKMODE mode);
extern void FluxUnlockPage(Relation rel, BlockNumber blkno, LOCKMODE mode);

/* sLog transaction callback prototypes */
static void FluxSLogXactCallback(XactEvent event, void *arg);
static void FluxSLogSubXactCallback(SubXactEvent event,
									SubTransactionId mySubid,
									SubTransactionId parentSubid,
									void *arg);
static void flux_clear_uncommitted_by_page(TransactionId xid);

/*
 * Backend-local command-id map for the current transaction's own in-place
 * UPDATE/DELETE operations.
 *
 * FLUX removed the on-tuple t_cid field (saving 4 bytes/tuple); the command
 * id of an uncommitted UPDATE/DELETE was previously recovered from the
 * per-tuple sLog entry.  With per-page conflict tracking, FLUX no longer
 * writes a per-tuple sLog UPDATE/DELETE entry, so the command id -- needed
 * ONLY for the current transaction's own self-visibility ordering
 * (curcid/cmax) -- is kept here in a backend-local hash keyed by (relid,tid).
 * This is O(tuples this backend modified this transaction) local memory, the
 * same information heap keeps on-tuple; it is reset at transaction end.
 *
 * The cmin side is NOT stored here: the command id of the writer named by
 * t_xmin lives on the tuple itself, in FluxTupleHeader.t_cmin (heap's t_cid),
 * because the plain insert path writes no per-tuple sLog entry at all under
 * per-page conflict tracking.  See FluxTupleHeader.t_cmin and
 * FluxTupleSatisfiesMVCC.
 */
typedef struct FluxCidKey
{
	Oid			relid;
	ItemPointerData tid;
} FluxCidKey;

typedef struct FluxCidEntry
{
	FluxCidKey	key;			/* hash key -- must be first */
	CommandId	cid;
} FluxCidEntry;

static HTAB *flux_local_cid_map = NULL;

/* in-place indexed-column UPDATE via delete-marking */
static bool flux_indexed_attr_changed(Relation relation, ItemPointer otid,
									  TupleTableSlot *slot,
									  UndoRecPtr *decided_verptr,
									  TransactionId *decided_xmin);
static bool flux_index_keys_equalimage(Relation indexRel);
static bool flux_index_change_exact(Relation relation, ItemPointer otid,
									FluxTupleHeader *old_hdr, Size old_len,
									TupleTableSlot *newslot);
static void flux_inplace_index_maintenance(Relation relation, ItemPointer otid,
										   FluxTupleHeader *old_hdr, Size old_len,
										   TupleTableSlot *newslot, UndoRecPtr urp);
static TM_Result flux_tuple_update_inplace(Relation relation, ItemPointer otid,
										   TupleTableSlot *slot, CommandId cid,
										   uint32 options, Snapshot snapshot,
										   Snapshot crosscheck, bool wait,
										   TM_FailureData *tmfd,
										   LockTupleMode *lockmode,
										   TU_UpdateIndexes *update_indexes,
										   bool key_changed,
										   bool *did_inplace_key_update, UndoRecPtr *index_urp,
										   FluxTupleHeader **index_old, Size *index_old_len,
										   UndoRecPtr decided_verptr,
										   TransactionId decided_xmin,
										   bool *redecide, bool *kept_tuplock,
										   LockTupleMode *kept_mode);

/*
 * FluxLocalCidSet -- record the command id of our own UPDATE/DELETE at (relid,
 * tid).  Later calls for the same TID overwrite (last writer in the txn wins,
 * matching the sLog's previous behavior where the newest op's cid was read).
 */
static void
FluxLocalCidSet(Oid relid, ItemPointer tid, CommandId cid)
{
	FluxCidKey	key;
	FluxCidEntry *entry;
	bool		found;

	if (cid == InvalidCommandId)
		return;

	if (flux_local_cid_map == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(FluxCidKey);
		ctl.entrysize = sizeof(FluxCidEntry);
		ctl.hcxt = TopTransactionContext;
		flux_local_cid_map = hash_create("FLUX local cid map", 256, &ctl,
										 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);
	entry = (FluxCidEntry *) hash_search(flux_local_cid_map, &key,
										 HASH_ENTER, &found);
	entry->cid = cid;
}

/*
 * FluxLocalCidGet -- fetch the command id our transaction stamped on an
 * in-place UPDATE/DELETE of (relid, tid), or InvalidCommandId if none.
 */
CommandId
FluxLocalCidGet(Oid relid, ItemPointer tid)
{
	FluxCidKey	key;
	FluxCidEntry *entry;

	if (flux_local_cid_map == NULL)
		return InvalidCommandId;

	memset(&key, 0, sizeof(key));
	key.relid = relid;
	ItemPointerCopy(tid, &key.tid);
	entry = (FluxCidEntry *) hash_search(flux_local_cid_map, &key,
										 HASH_FIND, NULL);
	return entry ? entry->cid : InvalidCommandId;
}

/*
 * FluxLocalCidReset -- drop the backend-local cid map at transaction end.
 * The hash lives in TopTransactionContext, so its memory is reclaimed by the
 * context reset regardless; nulling the pointer makes the state consistent
 * for the next transaction in this backend.
 */
void
FluxLocalCidReset(void)
{
	flux_local_cid_map = NULL;
}

/*
 * FluxCheckSelfModified
 *
 * Decide whether a tuple we are about to UPDATE or DELETE was already
 * modified by OUR OWN transaction in this or a later command, which heap
 * reports as TM_SelfModified from HeapTupleSatisfiesUpdate().
 *
 * Heap's rule (heapam_visibility.c): if the tuple's xmax is the current
 * transaction and its cmax >= the caller's curcid, the row was updated after
 * the current command's scan started, so the caller must not silently
 * overwrite it.  The executor turns that verdict into either "ignore the
 * second update of the same row in one command" (cmax == es_output_cid) or
 * the "tuple to be updated was already modified by an operation triggered by
 * the current command" error raised for BEFORE-trigger self-modification.
 *
 * FLUX updates in place and keeps no on-tuple cmax, so the command id of our
 * own uncommitted UPDATE/DELETE lives in the backend-local cid map
 * (FluxLocalCidSet/Get).  That gives us exactly heap's inputs:
 *
 *   - the tuple must still be UNCOMMITTED (an in-progress modification),
 *   - its writer xid must be our own transaction,
 *   - and we must have recorded a cid for it, which must be >= cid.
 *
 * Without this check FLUX silently applied the second write: the BEFORE
 * trigger's own change was overwritten and lost (no error, wrong data), and a
 * MERGE whose BEFORE trigger modifies the target row never terminated.
 *
 * The caller must hold the tuple's buffer lock so the header read is stable.
 * On a true verdict *cmax_out receives the recorded cid for tmfd->cmax, which
 * is what the executor compares against es_output_cid.
 */
bool
FluxCheckSelfModified(Relation relation, ItemPointer tid,
					  FluxTupleHeader *tuple_hdr, CommandId cid,
					  CommandId *cmax_out)
{
	TransactionId writer;
	CommandId	local_cid;

	if (cmax_out)
		*cmax_out = InvalidCommandId;

	/* Only an in-progress modification can be our own uncommitted write. */
	if (!(tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED))
		return false;

	/*
	 * Identify the writer the same way flux_header_dirty_xid does, but keep
	 * OUR xid instead of filtering it out: a DELETE stamps t_xmax, while an
	 * in-place UPDATE stamps the updater as t_xmin.
	 */
	if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
		writer = FluxTupleGetXmax(tuple_hdr);
	else
		writer = FluxTupleGetXmin(tuple_hdr);

	if (!TransactionIdIsValid(writer) ||
		!TransactionIdIsCurrentTransactionId(writer))
		return false;

	/*
	 * A recorded cid below the current command id means the change came from
	 * an EARLIER command, which is visible to us and legitimately updatable
	 * (heap returns TM_Ok there too).
	 */
	local_cid = FluxLocalCidGet(RelationGetRelid(relation), tid);
	if (local_cid == InvalidCommandId || local_cid < cid)
		return false;

	if (cmax_out)
		*cmax_out = local_cid;
	return true;
}

/*
 * Backend-local set of pages this transaction modified in place
 * (UPDATE/DELETE), for per-page commit-time UNCOMMITTED-flag cleanup.
 *
 * Per-page conflict tracking no longer records a per-tuple sLog UPDATE/DELETE
 * entry, so FluxClearUncommittedFlags can no longer enumerate this
 * transaction's modified tuples from the shared sLog tracked-key list.  This
 * backend-local set instead records the DISTINCT (relid, blkno) pages we
 * dirtied -- O(pages touched), not O(tuples) -- which is the whole point of
 * the per-page scheme.  At PRE_COMMIT we visit each page once and clear the
 * hint flag on every tuple stamped with our xid.  Reset at transaction end.
 */
typedef struct FluxDirtyPageKey
{
	Oid			relid;
	RelFileNumber relnumber;	/* the relation's storage the page is in */
	BlockNumber blkno;
} FluxDirtyPageKey;

static HTAB *flux_local_dirty_pages = NULL;

/*
 * FluxLocalDirtyPageMark -- record that our transaction modified block blkno
 * of rel's current storage in place.  Idempotent; called at the in-place
 * UPDATE/DELETE sites.
 */
static void
FluxLocalDirtyPageMark(Relation rel, BlockNumber blkno)
{
	FluxDirtyPageKey key;

	if (flux_local_dirty_pages == NULL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(FluxDirtyPageKey);
		ctl.entrysize = sizeof(FluxDirtyPageKey);
		ctl.hcxt = TopTransactionContext;
		flux_local_dirty_pages = hash_create("FLUX local dirty pages", 128,
											 &ctl,
											 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	memset(&key, 0, sizeof(key));
	key.relid = RelationGetRelid(rel);
	key.relnumber = rel->rd_locator.relNumber;
	key.blkno = blkno;
	(void) hash_search(flux_local_dirty_pages, &key, HASH_ENTER, NULL);
}

/*
 * FluxLocalDirtyPagesReset -- drop the per-transaction dirty-page set at
 * transaction end (memory reclaimed by the TopTransactionContext reset).
 */
static void
FluxLocalDirtyPagesReset(void)
{
	flux_local_dirty_pages = NULL;
}


/*
 * In-place update statistics counters.
 *
 * These track the effectiveness of FLUX's in-place update optimization
 * across the lifetime of the backend.  They are exposed via
 * FluxGetUpdateStats() for monitoring.
 */
static int64 flux_stat_in_place_updates = 0;
static int64 flux_stat_out_of_place_updates = 0;
static int64 flux_stat_defrag_triggered_updates = 0;

/* Whether sLog transaction callbacks have been registered for this backend */
static bool flux_slog_callbacks_registered = false;

/*
 * GUC: skip commit-time page re-visits (lazy clear of UNCOMMITTED flags).
 *
 * Default off.  Clearing FLUX_TUPLE_UNCOMMITTED is a pure hint (commit
 * visibility comes from CLOG via heap-shaped xmin/xmax); the lazy path clears
 * the flag at the next visibility check, while the eager path visits each
 * modified page at PRE_COMMIT so later readers can skip the sLog fast-path
 * lookup.
 */
bool		flux_lazy_uncommitted_clear = false;

/*
 * FluxGetUpdateStats - Return in-place update statistics
 *
 * Fills in the provided counters with the current backend-local statistics.
 */
void
FluxGetUpdateStats(int64 *in_place, int64 *out_of_place, int64 *defrag_triggered)
{
	if (in_place)
		*in_place = flux_stat_in_place_updates;
	if (out_of_place)
		*out_of_place = flux_stat_out_of_place_updates;
	if (defrag_triggered)
		*defrag_triggered = flux_stat_defrag_triggered_updates;
}


/*
 * FluxPagePruneOpt -- opportunistic dead-tuple cleanup on a page.
 *
 * This is the FLUX equivalent of heap_page_prune_opt().  It is called
 * during normal DML operations (insert, update) and sequential scans
 * when a page looks like it might benefit from cleanup.  The goal is to
 * reclaim space from deleted tuples without waiting for VACUUM.
 *
 * The caller must hold a pin on the buffer but must NOT hold a lock on it.
 * We attempt a conditional (non-blocking) exclusive lock; if we cannot
 * get it, we return immediately -- this is best-effort cleanup.
 *
 * Returns the number of tuples pruned.
 */
int
FluxPagePruneOpt(Relation relation, Buffer buffer)
{
	Page		page;
	FluxPageOpaque opaque;
	OffsetNumber offnum;
	OffsetNumber maxoff;
	uint64		oldest_ts;
	TransactionId oldest_xmin_prune;
	TransactionId conflict_horizon = InvalidTransactionId;
	int			ndead = 0;
	Size		minfree;

	/* Cannot write WAL during recovery, so skip */
	if (RecoveryInProgress())
		return 0;

	page = BufferGetPage(buffer);

	/* Skip if page is not initialized */
	if (PageIsNew(page))
		return 0;

	/*
	 * Validate page header before accessing special space. During recovery or
	 * after crashes, pages may have invalid headers. Skip pruning if the page
	 * header looks corrupt.
	 */
	{
		PageHeader	phdr = (PageHeader) page;

		if (phdr->pd_special < SizeOfPageHeaderData ||
			phdr->pd_special > BLCKSZ)
			return 0;
	}

	/*
	 * Quick check without lock: does the page look like it needs pruning? The
	 * FLUX_PAGE_DEFRAG_NEEDED flag is set by delete and update operations
	 * when a tuple is marked as deleted.  If no deletions have occurred on
	 * this page, there is nothing to clean up.
	 */
	opaque = FluxPageGetOpaque(page);
	if (!(FluxPageGetFlags(opaque) & FLUX_PAGE_DEFRAG_NEEDED))
		return 0;

	/*
	 * Heuristic: only prune if the page's free space is below a threshold.
	 * This avoids spending cycles on pages that already have plenty of room.
	 * We use 10% of BLCKSZ as the minimum, matching heap's approach. Reading
	 * pd_lower/pd_upper without a lock is slightly racy but acceptable for a
	 * heuristic.
	 */
	minfree = BLCKSZ / 10;
	if (PageGetFreeSpace(page) >= minfree &&
		!(FluxPageGetFlags(opaque) & FLUX_PAGE_FULL))
		return 0;

	/*
	 * Try to get an exclusive lock without blocking.  If the page is busy,
	 * skip it -- we will get another chance later.
	 */
	if (!ConditionalLockBufferForCleanup(buffer))
		return 0;

	/*
	 * Re-check under lock: the page state may have changed while we were
	 * acquiring the lock (or another backend may have pruned it).
	 */
	page = BufferGetPage(buffer);
	opaque = FluxPageGetOpaque(page);
	if (!(FluxPageGetFlags(opaque) & FLUX_PAGE_DEFRAG_NEEDED))
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		return 0;
	}

	/*
	 * oldest_xmin_prune is the reclamation gate (XID horizon,
	 * FluxTupleDeadToAll below).  oldest_ts is NOT a decision input here: it
	 * is carried only into the DEFRAG WAL record's cosmetic page commit-ts
	 * word (FluxXLogDefrag -> FluxPageSetCommitTs).
	 */
	oldest_ts = FluxGetOldestActiveTimestamp();
	oldest_xmin_prune = FluxGetOldestXminHorizon(relation);
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		FluxTupleHeader *tuple_hdr;

		if (!ItemIdIsNormal(itemid))
			continue;

		/* Skip overflow records */
		if (FluxIsOverflowRecordInline(PageGetItem(page, itemid),
									   ItemIdGetLength(itemid)))
			continue;

		tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

		/*
		 * A deleted tuple can be pruned if: - UNCOMMITTED is NOT set (the
		 * inserting/deleting xact committed) - commit_ts is older than the
		 * oldest active snapshot
		 *
		 * If UNCOMMITTED is still set, the transaction is still in progress
		 * (or aborted but not yet cleaned up) -- skip it.
		 */
		if (FluxTupleDeadToAll(tuple_hdr, oldest_xmin_prune))
		{
			ndead++;
		}
	}

	if (ndead == 0)
	{
		/*
		 * No reclaimable dead tuples.  Clear the defrag flag so we don't
		 * recheck this page on every access until a new deletion occurs.
		 */
		FluxPageClearFlag(opaque, FLUX_PAGE_DEFRAG_NEEDED);
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		return 0;
	}

	/*
	 * We have reclaimable dead tuples.  Mark them LP_DEAD (reclaiming their
	 * storage) and defragment the page to compact free space.
	 *
	 * CRITICAL: opportunistic pruning must set LP_DEAD, never LP_UNUSED.  A
	 * deleted FLUX tuple may still be referenced by index entries that only
	 * VACUUM (after index bulk-delete) is allowed to remove.  LP_DEAD
	 * reclaims the tuple's storage while reserving its line pointer so the
	 * TID cannot be recycled by a later INSERT (PageAddItemExtended only
	 * reuses LP_UNUSED slots).  Recycling a TID whose index entries still
	 * exist would let an index scan return the unrelated tuple later placed
	 * in that slot.  Only VACUUM Phase III converts LP_DEAD -> LP_UNUSED,
	 * after Phase II has removed the corresponding index entries.  This
	 * mirrors heap pruning, which sets LP_DEAD and defers LP_UNUSED to
	 * post-index-cleanup VACUUM.
	 */
	START_CRIT_SECTION();

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		ItemId		itemid = PageGetItemId(page, offnum);
		FluxTupleHeader *tuple_hdr;

		if (!ItemIdIsNormal(itemid))
			continue;

		/* Skip overflow records */
		if (FluxIsOverflowRecordInline(PageGetItem(page, itemid),
									   ItemIdGetLength(itemid)))
			continue;

		tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

		if (FluxTupleDeadToAll(tuple_hdr, oldest_xmin_prune))
		{
			FluxTupleAdvanceConflictHorizon(tuple_hdr, &conflict_horizon);
			ItemIdSetDead(itemid);
		}
	}

	FluxPageDefragment(page);

	MarkBufferDirty(buffer);

	/* WAL-log the defragmentation using a proper defrag record */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr;

		/*
		 * Use FluxXLogDefrag (not FluxXLogInitPage).  INIT_PAGE with
		 * REGBUF_WILL_INIT would zero the page during redo, losing all live
		 * tuples.  The defrag record uses REGBUF_STANDARD which stores a Full
		 * Page Image, preserving the page contents.
		 */
		recptr = FluxXLogDefrag(relation, buffer, NULL, 0, oldest_ts,
								conflict_horizon);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/* Update FSM with the reclaimed free space */
	FluxRecordFreeSpace(relation, BufferGetBlockNumber(buffer),
						PageGetHeapFreeSpace(page));

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	return true;
}


/*
 * Insert a tuple into a FLUX table
 */
void
flux_tuple_insert(Relation relation, TupleTableSlot *slot, CommandId cid,
				  uint32 options, BulkInsertState bistate)
{
	FluxTuple	flux_tuple;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	BlockNumber target_block;
	Size		tuple_size;
	ItemPointer tid = &slot->tts_tid;
	uint64		current_ts;
	FluxOverflowBuffers overflow_buffers;
	FluxLogicalImage insert_logical_img;
	Buffer		vmbuffer;
	Size		targetFreeSpace;
	int			i;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	ToastAttrInfo toast_attr[MaxTupleAttributeNumber];
	ToastTupleContext ttc;
	bool		toasted = false;

	slot_getallattrs(slot);

	/*
	 * Get current timestamp for the page-level commit-ts bookkeeping word.
	 *
	 * IMPORTANT: Get transaction timestamp here, BEFORE entering critical
	 * section, because FluxGetTransactionTimestamp() may need to allocate
	 * memory to initialize transaction state.
	 *
	 * CRITICAL FIX: Use FluxGetTransactionTimestamp() for both current_ts and
	 * xact_ts to ensure consistency. The inserted tuple will be visible
	 * within the same transaction because the snapshot timestamp will match
	 * the tuple's commit timestamp.
	 */
	(void) FluxGetTransactionTimestamp();

	current_ts = (uint64) FluxGetDmlTimestamp();

	/*
	 * Create FLUX tuple from slot.  Use the overflow-aware variant which will
	 * store large attributes (> FLUX_OVERFLOW_THRESHOLD) in overflow records
	 * on normal data pages, replacing them with compact inline overflow
	 * pointers.
	 *
	 * Overflow buffers are kept pinned for atomic WAL logging inside the
	 * critical section below.
	 */
	overflow_buffers.count = 0;

	/*
	 * FLUX has no on-page overflow: TOAST any wide varlena columns into the
	 * relation's standard heap TOAST table before forming the tuple, exactly
	 * like heap.  On return toast_values[] holds the (possibly externalized)
	 * datums that FluxFormTuple stores verbatim.
	 */
	memcpy(toast_values, slot->tts_values,
		   RelationGetDescr(relation)->natts * sizeof(Datum));
	memcpy(toast_isnull, slot->tts_isnull,
		   RelationGetDescr(relation)->natts * sizeof(bool));
	flux_toast_tuple(relation, toast_values, toast_isnull, NULL, NULL,
					 &ttc, toast_attr, &toasted, 0);

	flux_tuple = FluxFormTuple(RelationGetDescr(relation),
							   toast_values,
							   toast_isnull,
							   relation,
							   &overflow_buffers);

	/* Set MVCC fields (heap-shaped xmin/xmax) */
	flux_tuple->t_data->t_commit_ts = 0;	/* t_xmax = InvalidTransactionId */

	/*
	 * Stamp the inserter XID as t_xmin.  Visibility resolves t_xmin against
	 * CLOG + the reader's snapshot (FluxTupleSatisfiesMVCC), so an
	 * uncommitted insert is automatically invisible to other snapshots.  The
	 * FLUX_TUPLE_UNCOMMITTED flag is kept for the sLog write-conflict and
	 * defrag paths, but it no longer drives read visibility.
	 */
	if (options & FLUX_INSERT_REWRITE)
	{
		const FluxRewriteVersion *v = &flux_rewrite_version;

		/*
		 * A CLUSTER / VACUUM FULL copy keeps the writers of the version it
		 * copies, so older snapshots keep seeing it.  Only a copy of our own
		 * transaction's row is still uncommitted.
		 */
		Assert(flux_rewrite_version_set);
		flux_rewrite_version_set = false;
		flux_tuple->t_data->t_xmin = v->xmin;
		if (v->xmin_committed)
			flux_tuple->t_data->t_flags |= FLUX_TUPLE_XMIN_COMMITTED;
		else if (TransactionIdIsCurrentTransactionId(v->xmin))
			flux_tuple->t_data->t_flags |= FLUX_TUPLE_UNCOMMITTED;
		if (TransactionIdIsValid(v->xmax))
		{
			FluxTupleSetXmax(flux_tuple->t_data, v->xmax);
			flux_tuple->t_data->t_flags |= FLUX_TUPLE_DELETED;
			if (v->xmax_committed)
				flux_tuple->t_data->t_flags |= FLUX_TUPLE_XMAX_COMMITTED;
			if (v->moved_partition)
				flux_tuple->t_data->t_flags |= FLUX_TUPLE_MOVED_PARTITION;
		}
		cid = v->cmin;
	}
	else
	{
		flux_tuple->t_data->t_flags |= FLUX_TUPLE_UNCOMMITTED;
		flux_tuple->t_data->t_xmin = GetCurrentTransactionId(); /* subxid: heap-shaped,
																 * so savepoint rollback
																 * marks it aborted in
																 * CLOG */
	}

	/*
	 * Stamp the inserting command id (heap's t_cid).  Self-visibility needs
	 * it so a statement does not see the rows it is itself producing --
	 * without it FLUX's out-of-place UPDATE (delete + insert at a new TID)
	 * let the same scan re-find and re-update its own output: the Halloween
	 * problem.  Read only while UNCOMMITTED and t_xmin is ours
	 * (FluxTupleSatisfiesMVCC).
	 */
	flux_tuple->t_data->t_cmin = cid;

	/*
	 * The version-chain head lives in the fixed header field t_verptr
	 * (counted by FLUX_TUPLE_OVERHEAD), so every tuple carries it from birth
	 * with no on-page growth.  Stamp InvalidUndoRecPtr here; the first UPDATE
	 * replaces it with a real chain head.  Readers treat an invalid head as
	 * "no history" (flux_pvs.c), so a never-updated row is unaffected.
	 * Because the slot width never changes on UPDATE, the first UPDATE is
	 * always a same-length overwrite (Strategy 1 / CAS fast path).
	 */
	flux_tuple->t_data->t_flags |= FLUX_TUPLE_HAS_VERSION_PTR;
	FluxTupleSetVersionPtr(flux_tuple->t_data,
						   InvalidUndoRecPtr);
	tuple_size = flux_tuple->t_len;

	/* Ensure relation storage exists */
	RelationGetSmgr(relation);

	/*
	 * Find a page with enough free space.
	 *
	 * Like heap (RelationGetBufferForTuple), we first try the page we last
	 * inserted into, cached per-relation in the relcache via
	 * RelationSetTargetBlock().  Only if we have no cached target do we ask
	 * the FSM.  This avoids an fsm_search() + RecordPageWithFreeSpace() on
	 * every append: on an append-heavy table (e.g. pgbench_history) every
	 * backend otherwise pounds the same tail FSM pages, saturating the FSM
	 * buffer-header spinlock (LockBufHdr) at high concurrency.  The cache is
	 * only a hint -- a stale or too-full target just falls through to the FSM
	 * below and to the have_page free-space recheck.
	 *
	 * The cached-target fast path is used only when the tuple has no overflow
	 * data: with overflow, FluxFormTupleWithOverflow has already chosen and
	 * locked pages and the target_block/overflow-buffer interplay below
	 * assumes the FSM was consulted, so that path stays on the FSM.
	 *
	 * Account for fill factor: reserve space for future in-place updates.
	 */

	/*
	 * Derive the free-space bar ONCE and use the identical value for the FSM
	 * request and for every page recheck below.
	 *
	 * This must be a single value or the insert path cannot terminate: asking
	 * the FSM for X but only accepting a page with >= Y > X means a page the
	 * FSM legitimately returns is rejected by the recheck, which records the
	 * page as full and asks again -- forever.  That is exactly what happened
	 * when the FSM ask was clamped (to stay inside MaxFSMRequestSize) while
	 * the recheck still demanded the raw tuple_size + saveFreeSpace: INSERT
	 * HUNG instead of erroring.  FluxTargetFreeSpace applies heap's
	 * nearlyEmptyFreeSpace rule in FLUX's page geometry and never exceeds
	 * FLUX_MAX_TUPLE_SIZE, so the bar is always satisfiable by a fresh page.
	 */
	targetFreeSpace = FluxTargetFreeSpace(relation, tuple_size);

	{
		target_block = InvalidBlockNumber;
		if (overflow_buffers.count == 0)
			target_block = RelationGetTargetBlock(relation);

		if (target_block == InvalidBlockNumber)
			target_block = FluxGetPageWithFreeSpace(relation, targetFreeSpace);
	}

	if (target_block == InvalidBlockNumber)
	{
		/* Clean up overflow buffers before throwing error */
		for (i = 0; i < overflow_buffers.count; i++)
		{
			UnlockReleaseBuffer(overflow_buffers.buffers[i].buffer);
			pfree(overflow_buffers.buffers[i].record_data);
		}
		elog(ERROR, "FLUX failed to allocate page for tuple insertion");
	}

	/*
	 * Pre-allocate WAL buffer space BEFORE acquiring the data buffer lock.
	 * XLogEnsureRecordSpace() may allocate memory, so it MUST be called
	 * outside the critical section.
	 *
	 * rdata slots needed: MAX_OVERFLOW_BUFFERS * 2 (header + data per
	 * overflow record) + 2 (xl_flux_insert header + tuple data)
	 */
	if (RelationNeedsWAL(relation))
		XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID, 3 + MAX_OVERFLOW_BUFFERS * 2);

	/*
	 * Check if target_block is already locked in overflow_buffers from
	 * FluxFormTupleWithOverflow. If FSM returns the same block for both
	 * overflow storage and main tuple storage, we must reuse that buffer to
	 * avoid double-locking.
	 */
	buffer = InvalidBuffer;
	for (i = 0; i < overflow_buffers.count; i++)
	{
		if (BufferGetBlockNumber(overflow_buffers.buffers[i].buffer) == target_block)
		{
			buffer = overflow_buffers.buffers[i].buffer;
			break;
		}
	}

	/*
	 * Read and lock the target page only if we don't already have it locked
	 * from overflow processing.
	 */
	if (!BufferIsValid(buffer))
	{
		buffer = ReadBuffer(relation, target_block);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	}
	page = BufferGetPage(buffer);

	/*
	 * Verify the page has room for the tuple PLUS the fillfactor reservation,
	 * so in-place UPDATE growth stays possible (heap applies the same
	 * targetFreeSpace = len + saveFreeSpace bar).  A cached target block or a
	 * stale FSM answer that lacks the slack falls through to FSM retry /
	 * relation extension below.
	 */
	if (PageGetHeapFreeSpace(page) < targetFreeSpace)
	{
		bool		buffer_is_from_overflow = false;

		/*
		 * Check if this buffer is from overflow_buffers. If so, we must NOT
		 * unlock it for pruning, as overflow_buffers expects all its buffers
		 * to remain locked until the critical section.
		 */
		for (i = 0; i < overflow_buffers.count; i++)
		{
			if (overflow_buffers.buffers[i].buffer == buffer)
			{
				buffer_is_from_overflow = true;
				break;
			}
		}

		/*
		 * Page doesn't have enough space.  Try opportunistic pruning to
		 * reclaim space from dead tuples before falling back to the FSM. We
		 * must release our lock first since FluxPagePruneOpt() takes its own
		 * conditional lock.
		 *
		 * IMPORTANT: Skip pruning if buffer is from overflow_buffers, as we
		 * must keep those buffers locked.
		 */
		if (!buffer_is_from_overflow)
		{
			bool		buffer_locked_after_prune = false;

			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			if (FluxPagePruneOpt(relation, buffer))
			{
				/* Pruning freed space -- re-lock and check again */
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				buffer_locked_after_prune = true;
				page = BufferGetPage(buffer);
				if (PageGetHeapFreeSpace(page) >= targetFreeSpace)
					goto have_page;
				/* Still not enough after pruning, fall through to FSM retry */
			}

			/*
			 * FSM information was stale or pruning didn't help. Update and
			 * retry.  Re-acquire the buffer lock only if we do not already
			 * hold it: FluxPagePruneOpt drops the lock it takes, so after a
			 * prune that did not free enough (buffer_locked_after_prune) the
			 * buffer IS still locked from the re-lock above -- locking again
			 * here would double-lock and trip the buffer content-lock
			 * assertion under concurrency.
			 */
			if (!buffer_locked_after_prune)
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			FluxRecordFreeSpace(relation, target_block, PageGetHeapFreeSpace(page));
			UnlockReleaseBuffer(buffer);
		}
		else
		{
			/*
			 * Buffer is from overflow_buffers and can't be pruned or
			 * released. This means FSM returned an overflow page for the main
			 * tuple, which doesn't have enough space. This should be rare but
			 * can happen if overflow pages filled up during tuple formation.
			 *
			 * Update FSM for this page, then get a DIFFERENT page. We must
			 * retry until we find a page that's NOT in overflow_buffers.
			 */
			FluxRecordFreeSpace(relation, target_block, PageGetHeapFreeSpace(page));
		}

		/*
		 * Retry with updated FSM, excluding blocks in overflow_buffers. Keep
		 * trying until we find a suitable page that we don't already have
		 * locked for overflow storage.
		 */
		for (;;)
		{
			target_block = FluxGetPageWithFreeSpace(relation, targetFreeSpace);
			if (target_block == InvalidBlockNumber)
			{
				/* Clean up overflow buffers before throwing error */
				for (i = 0; i < overflow_buffers.count; i++)
				{
					UnlockReleaseBuffer(overflow_buffers.buffers[i].buffer);
					pfree(overflow_buffers.buffers[i].record_data);
				}
				elog(ERROR, "FLUX failed to allocate page for tuple insertion after retry");
			}

			/*
			 * Check if target_block is already locked in overflow_buffers. If
			 * so, skip it and try again - we need a different page.
			 */
			buffer = InvalidBuffer;
			for (i = 0; i < overflow_buffers.count; i++)
			{
				if (BufferGetBlockNumber(overflow_buffers.buffers[i].buffer) == target_block)
				{
					/*
					 * This block is already used for overflow - mark FSM and
					 * retry
					 */
					FluxRecordFreeSpace(relation, target_block, 0);
					buffer = InvalidBuffer;
					break;
				}
			}

			/*
			 * If we found a block not in overflow_buffers, check if it has
			 * space
			 */
			if (i >= overflow_buffers.count)
			{
				buffer = ReadBuffer(relation, target_block);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				page = BufferGetPage(buffer);

				/*
				 * Verify the page actually has enough space. If not, update
				 * FSM and retry.
				 */
				if (PageGetHeapFreeSpace(page) >= targetFreeSpace)
				{
					/* Found a suitable page - exit retry loop */
					break;
				}
				else
				{
					/* FSM was wrong - update it and retry */
					FluxRecordFreeSpace(relation, target_block, PageGetHeapFreeSpace(page));
					UnlockReleaseBuffer(buffer);
					buffer = InvalidBuffer;
					/* Continue outer loop to try again */
				}
			}
		}
	}

have_page:

	/*
	 * SSI: check for rw-conflict in.  An INSERT may conflict with a
	 * concurrent serializable transaction that holds a relation-level or
	 * page-level predicate lock (e.g., from a range scan that would have
	 * included this new tuple).  Pass NULL tid since the tuple doesn't exist
	 * yet; only relation-level and page-level locks are checked.
	 */
	/* Page SIREAD locks consolidate tuple locks; they do not lock gaps. */
	CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

	/*
	 * Ensure the current transaction has an XID assigned BEFORE entering the
	 * critical section.  GetCurrentTransactionId() may call
	 * XactLockTableInsert() which acquires a lock and allocates memory --
	 * both forbidden in a critical section.  Most inserts already have an XID
	 * by now (assigned during the unique-index check), but inserting a NULL
	 * into a UNIQUE column skips that check, so the first assignment can
	 * otherwise land inside the crit section below.
	 *
	 * An assigned XID is also required for correctness: WAL records without
	 * an attached xid cannot be decoded into a logical replication stream
	 * (ReorderBuffer groups changes by xid and emits no commit record for
	 * InvalidTransactionId), and RecordTransactionCommit() would treat the
	 * transaction as read-only and skip the WAL flush.
	 */
	(void) GetCurrentTransactionId();

	/*
	 * Final free-space check before entering the critical section.
	 * PageGetFreeSpace may have been optimistic (alignment, line pointer
	 * overhead).  If the page can't actually fit the tuple, release it,
	 * update FSM, and extend the relation instead.  This prevents the PANIC
	 * that would otherwise fire inside the critical section.
	 */
	offnum = FluxPageAddTuple(page, flux_tuple, tuple_size);
	if (offnum == InvalidOffsetNumber)
	{
		/* Page too full despite FSM claim; record actual free space */
		FluxRecordFreeSpace(relation, BufferGetBlockNumber(buffer),
							PageGetHeapFreeSpace(page));
		UnlockReleaseBuffer(buffer);

		/* Extend the relation to get a guaranteed-empty page */
		buffer = ReadBuffer(relation, P_NEW);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);
		FluxInitPage(page, BufferGetPageSize(buffer));

		offnum = FluxPageAddTuple(page, flux_tuple, tuple_size);
		if (offnum == InvalidOffsetNumber)
		{
			/*
			 * The row does not fit even on a guaranteed-empty page.  This is
			 * a user-facing size limit, not an internal failure, so report it
			 * the way heap does (hio.c: "row is too big") with a proper
			 * SQLSTATE instead of an elog that reads as a FLUX bug.
			 *
			 * FLUXs ceiling is genuinely lower than heaps: a FLUX tuple
			 * header is larger than HeapTupleHeaderData, so
			 * FLUX_MAX_TUPLE_SIZE < MaxHeapTupleSize.  A row whose
			 * UN-TOASTABLE (plain-storage) columns exceed FLUX_MAX_TUPLE_SIZE
			 * is therefore accepted by heap and rejected here.  Columns with
			 * ordinary storage are pushed out to TOAST/overflow before we get
			 * here, so this only fires for plain storage or a very wide
			 * fixed-width row.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("row is too big: size %zu, maximum size %zu",
							(Size) tuple_size, (Size) FLUX_MAX_TUPLE_SIZE)));
		}
	}

	/*
	 * Prepare the heap-format logical-decoding image before entering the
	 * critical section (it calls palloc/heap_form_tuple, which are forbidden
	 * inside a crit section).  No-op unless the relation is logically logged
	 * and the caller wants the insert decoded.
	 */
	FluxXLogPrepareLogicalImage(relation,
								(options & TABLE_INSERT_NO_LOGICAL) ?
								NULL : flux_tuple,
								&insert_logical_img);

	/*
	 * No INSERT UNDO record is emitted.  An inserted tuple has no
	 * before-image; rolling one back only requires it to become invisible,
	 * which its inserter (t_xmin) never committing already guarantees.  The
	 * DIRTY-snapshot and version-reconstruction paths resolve t_xmin against
	 * CLOG, so an aborted or crashed inserter's tuple is invisible without
	 * any undo bookkeeping.
	 */

	/* Pin the visibility map page before the critical section (I/O). */
	vmbuffer = FluxVMPinForChange(relation, buffer);

	/* NO EREPORT(ERROR) from here till changes are logged */
	START_CRIT_SECTION();

	/* Set the tuple's TID */
	ItemPointerSet(tid, BufferGetBlockNumber(buffer), offnum);
	flux_tuple->t_self = *tid;
	slot->tts_tableOid = RelationGetRelid(relation);

	/*
	 * Set the on-disk tuple's t_ctid to point to itself, so that update-chain
	 * code can tell a tuple that is not part of an update chain (t_ctid ==
	 * self).
	 */
	{
		ItemId		inserted_itemid = PageGetItemId(page, offnum);
		FluxTupleHeader *inserted_hdr = (FluxTupleHeader *) PageGetItem(page, inserted_itemid);

		ItemPointerSet(&inserted_hdr->t_ctid, BufferGetBlockNumber(buffer), offnum);
	}


	/*
	 * Update page opaque fields BEFORE WAL logging.  When
	 * XLogRegisterBuffer() takes a Full Page Write (FPW), the page image must
	 * already contain the same opaque values that REDO will set during
	 * replay.  Otherwise WAL consistency checking will detect a mismatch
	 * between the FPW and the page produced by REDO, causing a FATAL
	 * "inconsistent page found" error on the standby.
	 *
	 * This matches the fix applied to FluxXLogInitPage in flux_fsm.c.
	 */
	{
		FluxPageOpaque phdr = FluxPageGetOpaque(page);

		FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), current_ts));
	}

	FluxVMClearForChange(buffer, vmbuffer);
	MarkBufferDirty(buffer);

	/* Log the insertion with all overflow buffers atomically */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr = FluxXLogInsert(relation, buffer, offnum,
											flux_tuple, current_ts,
											&overflow_buffers,
											&insert_logical_img,
											false);

		PageSetLSN(page, recptr);
	}

	FluxVMFinishClear(RelationNeedsWAL(relation) ?
					  PageGetLSN(page) : InvalidXLogRecPtr);
	END_CRIT_SECTION();
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	FluxXLogReleaseLogicalImage(&insert_logical_img);

	/*
	 * Release all overflow buffers and free their cached data.
	 *
	 * IMPORTANT: Due to spatial locality optimization, an overflow buffer
	 * might be the SAME as the main buffer or as another overflow buffer
	 * (when overflow data is placed on the same page). Skip releasing buffers
	 * that were already released.  The main buffer is NOT released yet; only
	 * overflow buffers that differ from it.
	 */
	for (i = 0; i < overflow_buffers.count; i++)
	{
		Buffer		ovf_buf = overflow_buffers.buffers[i].buffer;
		bool		already_released = (ovf_buf == buffer);
		int			j;

		/* Check if this buffer was already released by a prior overflow entry */
		for (j = 0; j < i && !already_released; j++)
		{
			if (overflow_buffers.buffers[j].buffer == ovf_buf)
				already_released = true;
		}

		if (!already_released)
			UnlockReleaseBuffer(ovf_buf);
		pfree(overflow_buffers.buffers[i].record_data);
	}


	/*
	 * Save free space while we still have the buffer locked, then release the
	 * data buffer as soon as possible to reduce contention on hot pages. The
	 * remaining operations (FSM update, sLog registration) don't need the
	 * data buffer lock.
	 */
	{
		Size		saved_free_space = PageGetHeapFreeSpace(page);
		BlockNumber saved_blkno = BufferGetBlockNumber(buffer);

		UnlockReleaseBuffer(buffer);

		/* Update FSM with remaining free space on the page */
		FluxRecordFreeSpace(relation, saved_blkno, saved_free_space);

		/*
		 * Cache this block as the next insert target (heap's
		 * RelationSetTargetBlock pattern) so the following append skips the
		 * FSM search entirely.  Only on the no-overflow path -- the overflow
		 * path deliberately stays on the FSM (see the target-block lookup
		 * above).  If the page is now too full for the next tuple, the
		 * have_page free-space recheck falls back to the FSM.
		 */
		if (overflow_buffers.count == 0)
			RelationSetTargetBlock(relation, saved_blkno);
	}

	/*
	 * Lightweight subtransaction tracking for savepoint rollback.
	 *
	 * Nothing shared: t_xmin and CLOG tell other backends about the insert.
	 * The tuple is recorded in a backend-local list, which commit uses to
	 * clear the UNCOMMITTED hint, and which marks it rolled back if its
	 * savepoint is (SLogTupleRemoveBySubXid).
	 */

	/*
	 * A rewrite's copy of another transaction's version is not our insert. A
	 * copy of our own is, as the row it copies was: commit clears its
	 * UNCOMMITTED hint (flux_stamp_tuple_committed) so VACUUM can reclaim it
	 * once deleted.
	 */
	if (!(options & FLUX_INSERT_REWRITE) ||
		TransactionIdIsCurrentTransactionId(flux_rewrite_version.xmin))
	{
		FluxEnsureSLogCallbacks();
		SLogTupleTrackLocalOnly(RelationGetRelid(relation),
								relation->rd_locator.relNumber, tid,
								GetTopTransactionId(),
								GetCurrentSubTransactionId());
	}

	FluxFreeTuple(flux_tuple);

	/* Release toasting temporaries now that the tuple is durably stored. */
	if (toasted)
		flux_toast_cleanup(&ttc);

	/* The slot now stands for the version just stored (AFTER triggers). */
	FluxSlotSetVersion(slot, (options & FLUX_INSERT_REWRITE) ?
					   flux_rewrite_version.xmin : GetCurrentTransactionId(),
					   cid);

	if (!(options & FLUX_DML_NO_STATS))
		pgstat_count_heap_insert(relation, 1);
}

/*
 * flux_help_revert
 *		Writer-side helping: if the tuple at offnum's current version belongs
 *		to an aborted transaction whose UNDO has not been applied yet, revert
 *		it now instead of failing or waiting for the undo apply worker.
 *
 * buffer is exclusively locked.  Returns false, with the lock still held, if
 * there was nothing to revert.  Returns true after reverting, with the buffer
 * released: the caller re-reads the page and starts over, since the tuple may
 * have changed length and the index work below ran without the lock.
 */
bool
flux_help_revert(Relation relation, Buffer buffer, OffsetNumber offnum)
{
	Page		page = BufferGetPage(buffer);
	ItemId		lp;

	/*
	 * A hot standby must not change a page itself: every change has to come
	 * from the WAL, or the page diverges from the primary's.  Its readers do
	 * not need the revert, as they walk past aborted versions.
	 */
	if (RecoveryInProgress())
		return false;

	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		return false;
	lp = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(lp) ||
		!FluxTupleHasAbortedVersion((FluxTupleHeader *) PageGetItem(page, lp)))
		return false;

	if (!FluxRevertAbortedVersions(relation, buffer, offnum))
		return false;

	UnlockReleaseBuffer(buffer);
	return true;
}

/*
 * Was this tuple deleted by a committed UPDATE of another transaction that
 * moved the row to another partition?  See FLUX_TUPLE_MOVED_PARTITION.
 */
bool
FluxTupleMovedPartition(FluxTupleHeader *tuple_hdr)
{
	TransactionId xmax = FluxTupleGetXmax(tuple_hdr);

	return (tuple_hdr->t_flags & (FLUX_TUPLE_DELETED | FLUX_TUPLE_MOVED_PARTITION)) ==
		(FLUX_TUPLE_DELETED | FLUX_TUPLE_MOVED_PARTITION) &&
		TransactionIdIsNormal(xmax) &&
		!TransactionIdIsCurrentTransactionId(xmax) &&
		TransactionIdDidCommit(xmax);
}

/*
 * Was this tuple deleted by a committed UPDATE of another transaction that
 * moved the row to a new TID, which t_ctid holds?  See FLUX_TUPLE_SUPERSEDED.
 */
bool
FluxTupleSuperseded(FluxTupleHeader *tuple_hdr)
{
	TransactionId xmax = FluxTupleGetXmax(tuple_hdr);

	return (tuple_hdr->t_flags & (FLUX_TUPLE_DELETED | FLUX_TUPLE_SUPERSEDED)) ==
		(FLUX_TUPLE_DELETED | FLUX_TUPLE_SUPERSEDED) &&
		TransactionIdIsNormal(xmax) &&
		!TransactionIdIsCurrentTransactionId(xmax) &&
		TransactionIdDidCommit(xmax);
}

/*
 * A write that found its row deleted reports TM_Deleted.  If the deleter
 * moved the row, report TM_Updated instead, as heap does: with the
 * moved-partitions marker in tmfd->ctid if it moved the row to another
 * partition (the executor then fails with "tuple to be locked was already
 * moved to another partition" or a serialization failure), or with the new
 * TID if an UPDATE moved it within the relation (the executor follows it
 * under READ COMMITTED, and fails with a serialization failure otherwise).
 * The row is re-read because the TM_Deleted paths have released the buffer.
 */
static TM_Result
flux_report_moved(Relation relation, ItemPointer tid,
				  TM_Result result, TM_FailureData *tmfd)
{
	OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);
	Buffer		buffer;
	Page		page;
	bool		moved = false;
	bool		superseded = false;
	ItemPointerData new_tid;

	if (result != TM_Deleted)
		return result;

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	if (offnum >= FirstOffsetNumber && offnum <= PageGetMaxOffsetNumber(page))
	{
		ItemId		itemid = PageGetItemId(page, offnum);

		if (ItemIdIsNormal(itemid))
		{
			FluxTupleHeader *tuple_hdr = (FluxTupleHeader *)
				PageGetItem(page, itemid);

			moved = FluxTupleMovedPartition(tuple_hdr);
			superseded = FluxTupleSuperseded(tuple_hdr);
			new_tid = tuple_hdr->t_ctid;
		}
	}
	UnlockReleaseBuffer(buffer);

	if (moved)
	{
		if (tmfd)
			ItemPointerSetMovedPartitions(&tmfd->ctid);
		return TM_Updated;
	}
	if (superseded)
	{
		if (tmfd)
			tmfd->ctid = new_tid;
		return TM_Updated;
	}
	return result;
}

/* A palloc'd copy of the tuple stored at tid, which must exist. */
static FluxTuple
flux_copy_version(Relation relation, ItemPointer tid)
{
	Buffer		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
	Page		page;
	ItemId		itemid;
	FluxTuple	tup;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	itemid = PageGetItemId(page, ItemPointerGetOffsetNumber(tid));
	if (!ItemIdIsNormal(itemid))
		elog(ERROR, "FLUX: version (%u,%u) of a moved row is gone",
			 ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid));
	tup = palloc0(sizeof(FluxTupleData));
	tup->t_len = ItemIdGetLength(itemid);
	tup->t_data = palloc(tup->t_len);
	memcpy(tup->t_data, PageGetItem(page, itemid), tup->t_len);
	tup->t_self = *tid;
	tup->t_tableOid = RelationGetRelid(relation);
	UnlockReleaseBuffer(buffer);
	return tup;
}

/*
 * Link the old version of a row that flux_update_out_of_place moved to its
 * new TID: set FLUX_TUPLE_SUPERSEDED and store the new TID in t_ctid, heap's
 * forward link.  A writer or locker that waited for the update then follows
 * the row to the new version instead of finding it deleted.
 *
 * The new TID is known only once the insert has placed the new version, so
 * the link is a third WAL record after the delete's and the insert's, which
 * stay as they are.  Until it is written the old version is deleted without a
 * link, but only by our own in-progress transaction: every writer that finds
 * it waits for us to finish, and readers decide by t_xmax alone.  A crash in
 * between loses the transaction, whose undo restores the old version.  So
 * nothing can observe the gap.  Rollback needs nothing more either: the
 * delete's undo before-image has neither the flag nor the link.
 *
 * A follower is safe without heap's xmin check of the next version: it holds
 * a snapshot that does not see the update (it found the old version), so the
 * horizon keeps both the old version and the new one, which only a later
 * transaction can delete.
 *
 * The link record is also the one logical decoding turns into the UPDATE:
 * the delete and the insert are logged without logical images, and when
 * walLogical this record carries the old replica identity and the new row,
 * as XLOG_FLUX_UPDATE_INPLACE does.  So the move decodes as one UPDATE, like
 * heap's cross-page UPDATE, not as a DELETE and an INSERT.  It is written
 * after the insert, so the new row's TOAST chunks precede it in WAL, where
 * the reorder buffer joins them to it.
 */
static void
flux_link_superseded(Relation relation, ItemPointer otid, ItemPointer new_tid,
					 bool walLogical, bool key_intact)
{
	OffsetNumber offnum = ItemPointerGetOffsetNumber(otid);
	Buffer		buffer;
	Page		page;
	ItemId		itemid;
	FluxTupleHeader *tuple_hdr;
	FluxLogicalImage old_img = {NULL, 0};
	FluxLogicalImage new_img = {NULL, 0};

	/*
	 * Form the images from copies of both versions, with no buffer locked:
	 * deforming may read overflow pages, and palloc is not allowed in the
	 * critical section.  Only our transaction can change either version.
	 */
	if (walLogical && RelationIsLogicallyLogged(relation))
	{
		FluxTuple	oldtup = flux_copy_version(relation, otid);
		FluxTuple	newtup = flux_copy_version(relation, new_tid);

		FluxXLogPrepareReplicaIdentity(relation, oldtup, newtup, &old_img);
		FluxXLogPrepareLogicalImage(relation, newtup, &new_img);
		FluxFreeTuple(oldtup);
		FluxFreeTuple(newtup);
	}

	buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(otid));
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	itemid = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(itemid))
		elog(ERROR, "FLUX: old version (%u,%u) of a moved row is gone",
			 ItemPointerGetBlockNumber(otid), offnum);
	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);
	Assert(tuple_hdr->t_flags & FLUX_TUPLE_DELETED);
	Assert(TransactionIdIsCurrentTransactionId(FluxTupleGetXmax(tuple_hdr)));

	START_CRIT_SECTION();

	tuple_hdr->t_flags |= FLUX_TUPLE_SUPERSEDED;
	if (key_intact)
		tuple_hdr->t_flags |= FLUX_TUPLE_KEY_INTACT;
	tuple_hdr->t_ctid = *new_tid;
	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(relation))
	{
		xl_flux_supersede xlrec;
		XLogRecPtr	recptr;

		xlrec.offnum = offnum;
		xlrec.new_tid = *new_tid;
		xlrec.flags = key_intact ? FLUX_WAL_KEY_INTACT : 0;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xl_flux_supersede));
		FluxXLogRegisterLogicalImage(&old_img, &xlrec.flags);
		FluxXLogRegisterLogicalImage(&new_img, &xlrec.flags);
		XLogRegisterBuffer(0, buffer, REGBUF_STANDARD);
		recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_SUPERSEDE);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
	FluxXLogReleaseLogicalImage(&old_img);
	FluxXLogReleaseLogicalImage(&new_img);
}

/* Does the new row change any column that requires an exclusive row lock? */
static bool
flux_update_changes_key(Relation relation, FluxTupleHeader *header, Size len,
						TupleTableSlot *slot)
{
	Bitmapset  *keys = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_KEY);
	FluxTupleData old;
	TupleDesc	desc = RelationGetDescr(relation);
	Datum		values[MaxTupleAttributeNumber];
	bool		nulls[MaxTupleAttributeNumber];
	bool		changed = false;
	int			member = -1;

	if (keys == NULL)
		return false;
	old.t_data = header;
	old.t_len = len;
	slot_getallattrs(slot);
	FluxDeformTuple(relation, &old, desc, values, nulls);
	while ((member = bms_next_member(keys, member)) >= 0)
	{
		AttrNumber	attnum = member + FirstLowInvalidHeapAttributeNumber;
		Form_pg_attribute att;

		if (attnum <= 0)
		{
			if (attnum != TableOidAttributeNumber)
				changed = true;
		}
		else
		{
			att = TupleDescAttr(desc, attnum - 1);
			if (nulls[attnum - 1] != slot->tts_isnull[attnum - 1] ||
				(!nulls[attnum - 1] &&
				 !datum_image_eq(values[attnum - 1], slot->tts_values[attnum - 1],
								 att->attbyval, att->attlen)))
				changed = true;
		}
		if (changed)
			break;
	}
	bms_free(keys);
	return changed;
}

static TM_Result flux_tuple_delete_internal(Relation relation, ItemPointer tid,
											CommandId cid, uint32 options,
											Snapshot snapshot, Snapshot crosscheck,
											bool wait, TM_FailureData *tmfd,
											bool *have_tuple_lock);

TM_Result
flux_tuple_delete(Relation relation, ItemPointer tid, CommandId cid,
				  uint32 options, Snapshot snapshot, Snapshot crosscheck,
				  bool wait, TM_FailureData *tmfd)
{
	bool		have_tuple_lock = false;
	TM_Result	result;

	result = flux_tuple_delete_internal(relation, tid, cid, options, snapshot,
										crosscheck, wait, tmfd,
										&have_tuple_lock);
	if (have_tuple_lock)
		FluxUnlockTuple(relation, tid, LockTupleExclusive);
	return flux_report_moved(relation, tid, result, tmfd);
}

/*
 * Delete a tuple from a FLUX table with proper tombstone marking
 *
 * Like heap_delete, a delete that has to wait for another transaction first
 * takes the heavyweight tuple lock (FluxLockTupleToWait), to keep its place
 * among the writers and lockers that wait for the row, and when the wait
 * ends it starts over: whoever changed the row meanwhile, and whatever the
 * waited-for transaction did to it, is classified as on the first pass.  The
 * tuple lock is kept across the restarts and released by flux_tuple_delete.
 */
static TM_Result
flux_tuple_delete_internal(Relation relation, ItemPointer tid, CommandId cid,
						   uint32 options, Snapshot snapshot, Snapshot crosscheck,
						   bool wait, TM_FailureData *tmfd,
						   bool *have_tuple_lock)
{
	BlockNumber blkno;
	OffsetNumber offnum;
	Buffer		buffer;
	Page		page;
	ItemId		itemid;
	FluxTupleHeader *tuple_hdr;
	uint64		current_ts;
	FluxTuple	old_tuple_for_delete_wal;
	TransactionId delete_freeze_conflict;
	FluxLogicalImage delete_logical_img;
	UndoRecPtr	del_undo_ptr = InvalidUndoRecPtr;
	TransactionId del_hint_xid;
	Buffer		vmbuffer;

	/* Extract block and offset from TID */
	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	/* Validate TID range */
	if (blkno >= RelationGetNumberOfBlocks(relation))
		return TM_Invisible;

restart:
	/* Read the page containing the tuple */
	buffer = ReadBuffer(relation, blkno);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);

	/* Helping: see flux_help_revert. */
	if (flux_help_revert(relation, buffer, offnum))
	{
		buffer = ReadBuffer(relation, blkno);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);
	}

	/* Validate offset number */
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		UnlockReleaseBuffer(buffer);
		return TM_Invisible;
	}

	/* Get the item */
	itemid = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(itemid))
	{
		UnlockReleaseBuffer(buffer);
		return TM_Invisible;
	}

	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

	/*
	 * Our own transaction already modified this row in this or a later
	 * command: report TM_SelfModified so the executor can either ignore the
	 * duplicate action or raise the BEFORE-trigger self-modification error.
	 * This must come before the DELETED handling below, which would otherwise
	 * report our own in-progress delete as a plain TM_Deleted.
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
			UnlockReleaseBuffer(buffer);
			return TM_SelfModified;
		}
	}

	/*
	 * Check if tuple is already deleted (tombstone exists).  A DELETED flag
	 * can mean either a committed delete OR an in-progress delete by a
	 * concurrent transaction (which now also leaves UNCOMMITTED set).
	 * Distinguish the two: if another in-progress transaction owns the
	 * delete, wait for it and retry, matching heap's behavior where the
	 * second DELETE blocks behind the first. Only report TM_Deleted once the
	 * delete is genuinely committed (or ours).
	 */
	if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
	{
		TransactionId del_xid = InvalidTransactionId;
		bool		del_is_insert = false;

		if (tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED)
		{
			/*
			 * The concurrent deleter's XID is already on the page (t_xmax);
			 * read it from the header instead of the per-tuple sLog.  The
			 * header stays authoritative through the deleter's PRE_COMMIT
			 * window (sLog marker cleared before CLOG commit), so it is the
			 * more accurate conflict source.
			 */
			del_xid = flux_header_dirty_xid(tuple_hdr, &del_is_insert);
		}

		if (!wait && TransactionIdIsValid(del_xid) &&
			!TransactionIdIsCurrentTransactionId(del_xid) &&
			!del_is_insert)
		{
			/* The deleter is still running: it may yet abort (heap_delete). */
			if (tmfd)
			{
				tmfd->ctid = *tid;
				tmfd->xmax = del_xid;
				tmfd->cmax = InvalidCommandId;
				tmfd->traversed = false;
			}
			UnlockReleaseBuffer(buffer);
			return TM_BeingModified;
		}
		else if (TransactionIdIsValid(del_xid) &&
				 !TransactionIdIsCurrentTransactionId(del_xid) &&
				 !del_is_insert)
		{
			/*
			 * Start over when the deleter is done: a committed delete is
			 * reported below, an aborted one has had its before-image
			 * restored, and the row may have another deleter by then.
			 */
			UnlockReleaseBuffer(buffer);
			FluxLockTupleToWait(relation, tid, LockTupleExclusive,
								have_tuple_lock);
			XactLockTableWait(del_xid, relation, tid, XLTW_Delete);
			goto restart;
		}
		else
		{
			if (tmfd)
			{
				tmfd->ctid = *tid;
				tmfd->xmax = GetCurrentTransactionId();
				tmfd->cmax = InvalidCommandId;
				tmfd->traversed = false;
			}
			UnlockReleaseBuffer(buffer);
			return TM_Deleted;
		}
	}

	/*
	 * Handle LOCKED flag: same logic as the UPDATE path; clear our own lock
	 * before proceeding with the delete.
	 */
	if (tuple_hdr->t_flags & FLUX_TUPLE_LOCKED)
	{
		LockTupleMode own_mode;

		if (SLogTupleLockHeldByMe(RelationGetRelid(relation),
								  relation->rd_locator.relNumber, tid,
								  &own_mode))
		{
			tuple_hdr->t_flags &= ~FLUX_TUPLE_LOCKED;
		}
	}

	/*
	 * Check tuple visibility against snapshot and handle concurrent
	 * modifications.  Same logic as the UPDATE path: distinguish truly
	 * invisible tuples from concurrent modifications.
	 */
	if (snapshot)
	{
		bool		visible;

		visible = FluxTupleVisibleToSnapshotDual(tuple_hdr, snapshot,
												 RelationGetRelid(relation),
												 buffer, tid);

		if (!visible)
		{
			TransactionId dirty_xid;
			bool		is_insert_entry;

			/*
			 * The concurrent writer's XID is on the page (t_xmin/t_xmax);
			 * read it from the header, which stays authoritative through the
			 * writer's PRE_COMMIT window (sLog marker cleared before CLOG).
			 */
			dirty_xid = flux_header_dirty_xid(tuple_hdr, &is_insert_entry);

			/* Check if tuple was deleted by another transaction */
			if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
			{
				if (tmfd)
				{
					tmfd->ctid = *tid;
					tmfd->xmax = TransactionIdIsValid(dirty_xid) ?
						dirty_xid : GetCurrentTransactionId();
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				return TM_Deleted;
			}

			/*
			 * Buffer lock was never released (wait-free sLog read), so the
			 * tuple cannot have changed.  Proceed with dirty_xid.
			 */
			{
				if (TransactionIdIsValid(dirty_xid) && is_insert_entry)
				{
					if (tmfd)
					{
						tmfd->ctid = *tid;
						tmfd->xmax = dirty_xid;
						tmfd->cmax = InvalidCommandId;
						tmfd->traversed = false;
					}
					UnlockReleaseBuffer(buffer);
					return TM_Invisible;
				}

				if (TransactionIdIsValid(dirty_xid) && !is_insert_entry)
				{
					if (wait)
					{
						UnlockReleaseBuffer(buffer);
						FluxLockTupleToWait(relation, tid, LockTupleExclusive,
											have_tuple_lock);
						XactLockTableWait(dirty_xid, relation, tid,
										  XLTW_Delete);
						goto restart;
					}
					else
					{
						if (tmfd)
						{
							tmfd->ctid = *tid;
							tmfd->xmax = dirty_xid;
							tmfd->cmax = InvalidCommandId;
							tmfd->traversed = false;
						}
						UnlockReleaseBuffer(buffer);
						return TM_BeingModified;
					}
				}
				else
				{
					/*
					 * No in-progress sLog entry for another txn. Same
					 * EPQ-loop fix as the UPDATE path: check if our
					 * transaction already has a sLog entry (from
					 * table_tuple_lock during EPQ).  If so, fall through;
					 * otherwise trigger EPQ.
					 */
					TransactionId myxid_chk =
						GetCurrentTransactionIdIfAny();

					if (TransactionIdIsValid(myxid_chk))
					{
						if (SLogTupleLockHeldByMe(RelationGetRelid(relation),
												  relation->rd_locator.relNumber, tid, NULL))
						{
							/* EPQ already done; proceed. */
						}
						else
						{
							if (tmfd)
							{
								tmfd->ctid = *tid;
								tmfd->xmax =
									InvalidTransactionId;
								tmfd->cmax = InvalidCommandId;
								tmfd->traversed = false;
							}
							UnlockReleaseBuffer(buffer);
							return TM_Updated;
						}
					}
					else
					{
						if (tmfd)
						{
							tmfd->ctid = *tid;
							tmfd->xmax = InvalidTransactionId;
							tmfd->cmax = InvalidCommandId;
							tmfd->traversed = false;
						}
						UnlockReleaseBuffer(buffer);
						return TM_Updated;
					}
				}
			}
			/* If now visible, fall through to perform the delete */
		}
	}

	/*
	 * Same as the UPDATE path: even when visibility returned "true", check
	 * for in-progress modifications by another transaction.  Block if found.
	 */
	if (tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED)
	{
		TransactionId dirty_xid;
		bool		is_insert_entry;

		/*
		 * Concurrent writer XID is on the page (authoritative through
		 * PRE_COMMIT).
		 */
		dirty_xid = flux_header_dirty_xid(tuple_hdr, &is_insert_entry);

		if (!TransactionIdIsValid(dirty_xid))
		{
			/*
			 * No in-flight transaction is modifying this tuple.  The
			 * UNCOMMITTED flag is stale (left over from a committed
			 * transaction whose cleanup callback already ran).  Clear it
			 * opportunistically to prevent future visibility re-checks.
			 */
			tuple_hdr->t_flags &= ~FLUX_TUPLE_UNCOMMITTED;
			MarkBufferDirty(buffer);
		}
		else if (TransactionIdIsValid(dirty_xid) &&
				 !TransactionIdIsCurrentTransactionId(dirty_xid) &&
				 !is_insert_entry)
		{
			if (wait)
			{
				TransactionId wait_xid = dirty_xid;

				UnlockReleaseBuffer(buffer);
				FluxLockTupleToWait(relation, tid, LockTupleExclusive,
									have_tuple_lock);
				XactLockTableWait(wait_xid, relation, tid, XLTW_Delete);

				buffer = ReadBuffer(relation, blkno);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				page = BufferGetPage(buffer);

				if (offnum < FirstOffsetNumber ||
					offnum > PageGetMaxOffsetNumber(page))
				{
					UnlockReleaseBuffer(buffer);
					return TM_Invisible;
				}
				itemid = PageGetItemId(page, offnum);
				if (!ItemIdIsNormal(itemid))
				{
					UnlockReleaseBuffer(buffer);
					return TM_Invisible;
				}
				tuple_hdr = (FluxTupleHeader *)
					PageGetItem(page, itemid);

				/*
				 * A delete mark is a delete only once its deleter commits,
				 * and the row may have another deleter by now: start over.
				 */
				if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
				{
					UnlockReleaseBuffer(buffer);
					goto restart;
				}

				if (tmfd)
				{
					tmfd->ctid = *tid;
					tmfd->xmax = wait_xid;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				return TM_Updated;
			}
			else
			{
				if (tmfd)
				{
					tmfd->ctid = *tid;
					tmfd->xmax = dirty_xid;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				return TM_BeingModified;
			}
		}
	}

	/*
	 * Authoritative write-write / lock-conflict gate (mirrors the UPDATE
	 * path's final gate).  The three checks above read the writer xids in the
	 * tuple header, which only detect in-progress WRITERS: a SELECT ... FOR
	 * UPDATE/FOR SHARE locker of a committed, non-deleted tuple leaves no xid
	 * there, so it would sail straight through and DELETE the row the locker
	 * is protecting.  Ask the tuple sLog (SLogTupleLockConflictXid), which
	 * reports a locker whose recorded LockTupleMode conflicts with ours under
	 * the real heavyweight matrix.
	 *
	 * LockTupleMode: LockTupleExclusive (AccessExclusiveLock), matching heap
	 * DELETE ("we need the strongest one").  A DELETE destroys the key, so it
	 * must conflict with FOR KEY SHARE FK lockers too; unlike UPDATE (which
	 * uses NoKeyExclusive precisely to stay compatible with a KeyShare FK
	 * lock), waiting on a KeyShare locker here is CORRECT, not spurious --
	 * the FK machinery guarantees that locker releases before the referenced
	 * row can go away.
	 *
	 * Self-wait / EPQ safety: SLogTupleLockConflictXid skips our own locks,
	 * so XactLockTableWait never receives GetTopTransactionId() (no lmgr
	 * self-wait assertion).  A lock this transaction took during EPQ
	 * re-evaluation is our own and is likewise skipped.  Before sleeping we
	 * acquire the heavyweight LOCKTAG_TUPLE lock so two writers racing the
	 * same tuple queue FIFO instead of mutually XactLockTableWait deadlocking
	 * (matches heap_delete's "establish our priority").
	 */
	{
		TransactionId conflict_xid;

		conflict_xid = SLogTupleLockConflictXid(RelationGetRelid(relation),
												relation->rd_locator.relNumber,
												tid, LockTupleExclusive);

		if (TransactionIdIsValid(conflict_xid) &&
			!TransactionIdIsCurrentTransactionId(conflict_xid))
		{
			if (!wait)
			{
				/* Not waiting: report the locker, as heap_delete does. */
				if (tmfd)
				{
					tmfd->ctid = *tid;
					tmfd->xmax = conflict_xid;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				return TM_BeingModified;
			}

			/*
			 * Start over when the locker is done: it may have updated or
			 * deleted the row before it finished, and another transaction may
			 * hold a lock on it by then.  Several transactions can hold FOR
			 * KEY SHARE on the row (one per foreign-key check), so this may
			 * wait for each in turn, as heap waits for the MultiXact of all
			 * lockers.  A locker that arrives after us queues behind us on
			 * the tuple lock, which flux_tuple_lock takes before it records
			 * its lock, unless it holds a lock on the row already.
			 */
			UnlockReleaseBuffer(buffer);
			FluxLockTupleToWait(relation, tid, LockTupleExclusive,
								have_tuple_lock);
			XactLockTableWait(conflict_xid, relation, tid, XLTW_Delete);
			goto restart;
		}
	}

	/*
	 * Transaction-snapshot RI action: the row must also be visible to the
	 * crosscheck snapshot, as heap_delete requires, or the RI query found a
	 * row (one inserted after our snapshot) that the transaction cannot see.
	 */
	if (crosscheck != InvalidSnapshot &&
		!FluxTupleVisibleToSnapshotDual(tuple_hdr, crosscheck,
										RelationGetRelid(relation), buffer, tid))
	{
		if (tmfd)
		{
			tmfd->ctid = *tid;
			tmfd->xmax = FluxTupleGetXmin(tuple_hdr);
			tmfd->cmax = InvalidCommandId;
			tmfd->traversed = false;
		}
		UnlockReleaseBuffer(buffer);
		return TM_Updated;
	}

	/*
	 * Get transaction timestamp BEFORE critical section. Use xact_ts as
	 * commit timestamp for within-transaction visibility.
	 */
	(void) FluxGetTransactionTimestamp();
	current_ts = (uint64) FluxGetDmlTimestamp();

	/*
	 * Allocate old_tuple structure and save a copy of the old tuple data
	 * BEFORE entering the critical section.  The tuple header will be
	 * modified below (DELETED flag, commit_ts, etc.), and the WAL record
	 * needs the unmodified before-image for UNDO support.
	 */
	{
		uint32		del_old_len = ItemIdGetLength(itemid);

		old_tuple_for_delete_wal = palloc(sizeof(FluxTupleData));
		old_tuple_for_delete_wal->t_len = del_old_len;
		old_tuple_for_delete_wal->t_data = (FluxTupleHeader *) palloc(del_old_len);
		memcpy(old_tuple_for_delete_wal->t_data, tuple_hdr, del_old_len);
		delete_freeze_conflict = flux_prepare_before_image(relation, old_tuple_for_delete_wal->t_data);
	}

	/*
	 * Prepare the heap-format logical-decoding image of the deleted tuple
	 * before the critical section (palloc/heap_form_tuple are forbidden
	 * inside).  No-op unless the relation is logically logged and the caller
	 * wants the delete decoded.
	 */
	if (options & TABLE_DELETE_NO_LOGICAL)
		FluxXLogPrepareLogicalImage(relation, NULL, &delete_logical_img);
	else
		FluxXLogPrepareReplicaIdentity(relation, old_tuple_for_delete_wal, NULL,
									   &delete_logical_img);

	/*
	 * SSI: check for rw-conflict in.  If a concurrent serializable
	 * transaction read this tuple (holds a SIREAD lock on it), our delete
	 * creates an rw-antidependency that may form a dangerous structure.
	 */
	CheckForSerializableConflictIn(relation, tid, BufferGetBlockNumber(buffer));

	/*
	 * Pre-allocate WAL buffer space BEFORE entering critical section. DELETE
	 * operations only need the main buffer (no overflow).
	 *
	 * CRITICAL: XLogEnsureRecordSpace() may allocate memory, so it MUST be
	 * called outside the critical section.
	 */
	if (RelationNeedsWAL(relation))
		XLogEnsureRecordSpace(0, 2);

	/*
	 * Per-backend UNDO: prepare a DELETE UNDO record carrying the full old
	 * tuple so a top-level ROLLBACK can restore the deleted tuple in place.
	 * Must happen before the critical section (PrepareUndoInsert may extend
	 * the undo log and ereport).  Its UndoRecPtr is stamped into t_verptr,
	 * like an UPDATE's: that is the idempotency key of the UNDO apply, and
	 * the before-image it restores carries the previous t_verptr.  Readers
	 * never step through a DELETE record: a deleted row's visibility is
	 * decided by t_xmax.
	 */
	del_undo_ptr = FluxPbuPrepareInsert(FLUX_UNDO_DELETE,
										relation, tid,
										(const char *) old_tuple_for_delete_wal->t_data,
										old_tuple_for_delete_wal->t_len,
										relation->rd_rel->relpersistence, false, delete_freeze_conflict,
										cid);

	/*
	 * Assign our top-level XID for the t_xmin stamp BEFORE the critical
	 * section: GetTopTransactionId() allocates a fresh XID on first use,
	 * which calls XactLockTableInsert -> LockAcquire -> palloc, forbidden
	 * inside a critical section.  Precompute here and write only the bare
	 * field below, matching the surrounding "compute before crit, write
	 * inside" pattern (del_undo_ptr, XLogEnsureRecordSpace, logical image).
	 */
	del_hint_xid = GetCurrentTransactionId();	/* subxid: heap-shaped t_xmax */

	vmbuffer = FluxVMPinForChange(relation, buffer);

	/* Start critical section for WAL logging */
	START_CRIT_SECTION();

	/*
	 * Mark tuple as deleted with tombstone - this is the key FLUX feature.
	 * Set UNCOMMITTED while the delete is in flight: a second DELETE or
	 * UPDATE on this TID must detect the in-progress delete (t_xmax, via
	 * flux_header_dirty_xid) and block on it, rather than racing ahead.  The
	 * flag is cleared at commit by flux_stamp_tuple_committed; VACUUM sees a
	 * clean committed delete via the xmax XID + CLOG.
	 *
	 * Heap-shaped: set t_xmax to the deleter XID (leaving t_xmin, the
	 * inserter, untouched).  Visibility resolves t_xmax against CLOG + the
	 * reader's snapshot: an old snapshot that predates the delete's commit
	 * still sees the row; a snapshot after it does not.
	 */
	tuple_hdr->t_flags |= FLUX_TUPLE_DELETED;
	tuple_hdr->t_flags |= FLUX_TUPLE_UNCOMMITTED;
	/* Fresh xmax being stamped: drop any stale XMAX_COMMITTED CLOG hint. */
	tuple_hdr->t_flags &= ~FLUX_TUPLE_XMAX_COMMITTED;
	FluxTupleSetXmax(tuple_hdr, del_hint_xid);

	/*
	 * A row moved to another partition: heap's moved-partitions t_ctid.
	 *
	 * SUPERSEDED is normally gone by now: it is set only with DELETED, and a
	 * writer reverts an aborted out-of-place UPDATE first (flux_help_revert).
	 * But a dirty-snapshot fetch can clear the DELETED hint of a deleter that
	 * aborted after FluxTupleHasAbortedVersion found it running, which leaves
	 * SUPERSEDED and its forward t_ctid without anything that makes a writer
	 * revert.  So clear it, and reset t_ctid; the redo does the same.
	 */
	tuple_hdr->t_flags &= ~(FLUX_TUPLE_MOVED_PARTITION | FLUX_TUPLE_SUPERSEDED |
							FLUX_TUPLE_KEY_INTACT);
	if (options & TABLE_DELETE_CHANGING_PARTITION)
		tuple_hdr->t_flags |= FLUX_TUPLE_MOVED_PARTITION;
	if (UndoRecPtrIsValid(del_undo_ptr))
		tuple_hdr->t_verptr = del_undo_ptr;
	/* Keep the original t_ctid for potential update chains */
	ItemPointerCopy(tid, &tuple_hdr->t_ctid);

	/* Update page header to match what redo does */
	{
		FluxPageOpaque phdr = FluxPageGetOpaque(page);

		FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), current_ts));
		FluxPageSetFlag(phdr, FLUX_PAGE_DEFRAG_NEEDED);
	}

	/*
	 * The page holds a version not every snapshot sees.  Clearing BOTH bits
	 * matters: VACUUM's first pass skips an all-frozen page, so a deleted
	 * tuple and its index entries left there would never be removed.
	 */
	FluxVMClearForChange(buffer, vmbuffer);
	MarkBufferDirty(buffer);

	/* WAL log the deletion using the pre-saved old tuple copy */

	/*
	 * Write the per-backend DELETE UNDO record into its pinned undo buffers
	 * BEFORE the forward-op WAL record so FluxXLogDelete can fold the undo
	 * pages onto its record as full-page images.
	 */
	if (UndoRecPtrIsValid(del_undo_ptr))
		FluxPbuInsertUndo();

	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	recptr;

		recptr = FluxXLogDelete(relation, buffer, offnum,
								old_tuple_for_delete_wal,
								(uint64) del_hint_xid,
								&delete_logical_img);
		PageSetLSN(page, recptr);

		if (UndoRecPtrIsValid(del_undo_ptr))
			FluxPbuFinishUndo(recptr);
	}
	else if (UndoRecPtrIsValid(del_undo_ptr))
		FluxPbuFinishUndo(InvalidXLogRecPtr);

	FluxVMFinishClear(RelationNeedsWAL(relation) ?
					  PageGetLSN(page) : InvalidXLogRecPtr);
	END_CRIT_SECTION();
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	FluxXLogReleaseLogicalImage(&delete_logical_img);

	/*
	 * The per-backend DELETE UNDO record was written and folded into the
	 * forward WAL record inside the critical section above.
	 */


	/*
	 * Clean up overflow chains if this tuple has overflow attributes. This
	 * must happen outside the critical section since it performs its own
	 * buffer I/O.  We check the flag and capture the free space before
	 * releasing the buffer so we can read the tuple header and page state.
	 */
	{
		bool		has_overflow = (tuple_hdr->t_flags & FLUX_TUPLE_HAS_OVERFLOW) != 0;
		Size		free_space = PageGetHeapFreeSpace(page);

		UnlockReleaseBuffer(buffer);

		/*
		 * Delete the row's external TOAST values, as heap_delete does: an
		 * MVCC delete in the TOAST table by this transaction, which commits
		 * or rolls back with the DELETE.  A snapshot that still reads the
		 * deleted version (from UNDO) still sees the values, and TOAST VACUUM
		 * removes them only once no snapshot can.  VACUUM of this table then
		 * has no TOAST work: when it removes the row, its values are already
		 * deleted (committed DELETE) or were never live (aborted INSERT).
		 * old_tuple_for_delete_wal is the version as it was before this
		 * DELETE; the buffer lock is released, as toast deletion reads and
		 * writes other relations.
		 */
		if ((old_tuple_for_delete_wal->t_data->t_infomask & FLUX_INFOMASK_HASEXTERNAL) &&
			(relation->rd_rel->relkind == RELKIND_RELATION ||
			 relation->rd_rel->relkind == RELKIND_MATVIEW))
		{
			TupleDesc	tupdesc = RelationGetDescr(relation);
			Datum	   *values = palloc_array(Datum, tupdesc->natts);
			bool	   *isnull = palloc_array(bool, tupdesc->natts);

			FluxDeformTuple(relation, old_tuple_for_delete_wal, tupdesc,
							values, isnull);
			flux_toast_delete(relation, values, isnull, false);
			pfree(values);
			pfree(isnull);
		}

		/*
		 * Ensure xact/subxact callbacks are registered before any sLog
		 * operation.  This is critical for savepoint rollback: without the
		 * SubXactCallback, ROLLBACK TO SAVEPOINT won't restore tuples.
		 */
		FluxEnsureSLogCallbacks();

		/*
		 * BUG 5 fix: track this DELETE with the bounded backend-local tracker
		 * (no shared per-tuple entry, no unbounded slog_tracked_keys list
		 * node).  Per-page conflict tracking means a concurrent observer
		 * reads the conflicting xid from the on-page t_xmax
		 * (flux_header_dirty_xid), not from a per-tuple sLog UPDATE/DELETE
		 * entry; commit-time flag cleanup comes from the per-page dirty set;
		 * and savepoint rollback restores from the durable UNDO log.  So the
		 * shared per-tuple entry is redundant and its O(N-rows) memory is the
		 * cause of the backend OOM under write-heavy load.
		 */

		/*
		 * BUG 5: UPDATE/DELETE keep NO per-tuple tracking at all. Commit-time
		 * UNCOMMITTED-flag cleanup comes from the per-page dirty set
		 * (flux_clear_uncommitted_by_page), savepoint rollback restores from
		 * the durable UNDO log, and conflict detection reads the on-page
		 * header -- so neither the bounded INSERT hash (O(N tuples/txn)) nor
		 * the subtransaction list is needed.  This keeps per-transaction
		 * backend memory O(pages), not O(rows), which is what heap does.
		 */

		/* Record our own delete's command id for self-visibility (cmax). */
		FluxLocalCidSet(RelationGetRelid(relation), tid, cid);

		FluxFreeTuple(old_tuple_for_delete_wal);

		FluxLocalDirtyPageMark(relation, blkno);

		/*
		 * NOTE: We do NOT immediately clean up overflow chains here.
		 * Immediate cleanup was: 1. Buggy (collected wrong overflow pointers
		 * after modification) 2. Expensive on hot paths (extra buffer I/O +
		 * locking) 3. Complex to WAL-log correctly
		 *
		 * Instead, overflow cleanup is deferred to VACUUM.  When VACUUM
		 * prunes deleted tuples, it will also reclaim orphaned overflow
		 * pages.
		 *
		 * Future enhancement: Log overflow block/offset in WAL DELETE record
		 * so UNDO log pruning can also clean up overflow chains.
		 */
		(void) has_overflow;	/* Suppress unused variable warning */

		/* Update free space map - deleted tuple creates more free space */
		FluxRecordFreeSpace(relation, blkno, free_space);
	}

	/* Return success - tuple was successfully marked as deleted */
	if (!(options & FLUX_DML_NO_STATS))
		pgstat_count_heap_delete(relation);
	return TM_Ok;
}

/*
 * Release a set of overflow buffers collected during a force-shrink update.
 *
 * FluxStoreOverflowColumn leaves each overflow page locked, dirty, and
 * unlogged, handing the buffers back to the caller for atomic WAL logging on
 * the success path.  On any error path before that logging happens, the caller
 * must release them here so no dirty unlogged page is pinned into transaction
 * abort.  Skips main_buffer (released separately by the caller) and any buffer
 * that appears more than once (two overflow columns can land on one page).
 */
static void
flux_release_update_overflow_buffers(FluxOverflowBuffers *bufs,
									 Buffer main_buffer)
{
	int			i;

	for (i = 0; i < bufs->count; i++)
	{
		Buffer		ovf_buf = bufs->buffers[i].buffer;
		bool		already_released = (ovf_buf == main_buffer);
		int			j;

		for (j = 0; j < i && !already_released; j++)
		{
			if (bufs->buffers[j].buffer == ovf_buf)
				already_released = true;
		}
		if (!already_released)
			UnlockReleaseBuffer(ovf_buf);
		pfree(bufs->buffers[i].record_data);
	}
	bufs->count = 0;
}

/*
 * flux_update_conflict_xid
 *
 * The in-progress transaction an UPDATE of this tuple must wait for, or
 * InvalidTransactionId if there is none.  The caller holds the buffer lock.
 *
 * The row's writer is read from the on-page header first
 * (flux_header_dirty_xid: t_xmin of an in-place updated image, t_xmax of a
 * delete).  UPDATE and DELETE keep no per-tuple sLog entry, so the sLog
 * cannot see an in-progress updater; the header can, and it stays
 * authoritative through the writer's PRE_COMMIT window, when the
 * UNCOMMITTED hint is already gone but CLOG does not know the outcome yet.
 * The xid there is the writing subtransaction's, which XactLockTableWait
 * resolves to its top-level transaction.
 *
 * Then the tuple sLog, for what only it records: row locks whose mode
 * conflicts with our update.  A key change also conflicts with FOR KEY
 * SHARE; a non-key change does not.
 *
 * "No in-progress writer" is not "safe to overwrite": a writer that has
 * committed since our snapshot was taken is the committed-update gates' job
 * (FluxTupleHasCommittedUpdateAfter), which every caller still runs.
 */
static TransactionId
flux_update_conflict_xid(Relation relation, ItemPointer otid,
						 FluxTupleHeader *tuple_hdr, bool key_changed,
						 bool *is_insert)
{
	TransactionId xid;

	xid = flux_header_dirty_xid(tuple_hdr, is_insert);
	if (TransactionIdIsValid(xid))
		return xid;

	return SLogTupleLockConflictXid(RelationGetRelid(relation),
									relation->rd_locator.relNumber, otid,
									key_changed ? LockTupleExclusive :
									LockTupleNoKeyExclusive);
}

/*
 * An UPDATE that waited for a writer found the row delete-marked.  It is
 * deleted only if the deleter committed: that deleter may have come after the
 * transaction we waited for and still be in progress, or have aborted.  Then
 * report TM_Updated, as after any wait for a writer (see below), and the
 * executor locks the row, which waits for the deleter, before it rechecks it.
 */
static TM_Result
flux_deleted_after_wait(FluxTupleHeader *tuple_hdr)
{
	TransactionId xmax = FluxTupleGetXmax(tuple_hdr);

	if (TransactionIdIsValid(xmax) && TransactionIdDidCommit(xmax))
		return TM_Deleted;
	return TM_Updated;
}

/*
 * Update a tuple in a FLUX table with versioning support
 *
 * FLUX_RELEASE_TUPLOCK releases the heavyweight tuple lock that competing
 * updaters acquire before XactLockTableWait (see the wait sites below).  The
 * lock is held continuously from acquisition through the recheck/update so it
 * serializes racing updaters into a FIFO queue (matching heap_update); it must
 * be released on every function-exit path so it never leaks past commit.  The
 * have_tuple_lock guard makes the macro a no-op on paths that never acquired.
 */
#define FLUX_RELEASE_TUPLOCK() \
	do { \
		if (have_tuple_lock) \
		{ \
			FluxUnlockTuple(relation, otid, update_lockmode); \
			have_tuple_lock = false; \
		} \
	} while (0)

/*
 * flux_header_dirty_xid
 *
 * Derive the in-progress, non-current writer XID for a tuple directly from its
 * ON-PAGE header, without consulting the per-tuple sLog.  This is the common
 * write-conflict question (who is concurrently modifying this tuple?) and the
 * answer is already on the page: the inserter is t_xmin and the
 * updater/deleter is t_xmax (low 32 bits of t_commit_ts).
 *
 * The caller must hold the tuple's buffer lock (shared or exclusive) so the
 * header read is stable.  Returns the conflicting XID and sets *is_insert to
 * whether that XID is an in-progress INSERT (t_xmin, tuple not yet
 * DELETED/UPDATED) rather than an in-progress UPDATE/DELETE (t_xmax).
 *
 * Returns InvalidTransactionId when there is no in-progress non-current writer
 * (the tuple is plain-committed, or the only writer is our own transaction).
 * A returned XID is advisory: the caller must re-validate after any wait
 * because the xid may commit/abort concurrently.  We filter
 * TransactionIdIsCurrentTransactionId and require TransactionIdIsInProgress.
 */
TransactionId
flux_header_dirty_xid(FluxTupleHeader *tuple_hdr, bool *is_insert)
{
	TransactionId xmax = FluxTupleGetXmax(tuple_hdr);
	TransactionId xmin = FluxTupleGetXmin(tuple_hdr);

	if (is_insert)
		*is_insert = false;

	/*
	 * DELETED: the concurrent writer is the deleter (t_xmax).
	 */
	if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
	{
		if (TransactionIdIsValid(xmax) &&
			!TransactionIdIsCurrentTransactionId(xmax) &&
			TransactionIdIsInProgress(xmax))
			return xmax;
		return InvalidTransactionId;
	}

	/*
	 * In-place UPDATED (but not deleted): FLUX rewrites the newest version in
	 * place, stamping the updater as the on-page t_xmin and leaving t_xmax
	 * Invalid (the row is still live).  So the concurrent writer to wait on
	 * is the UPDATER = t_xmin, and it is NOT an insert -- a concurrent DELETE
	 * or UPDATE must block behind it, exactly as heap blocks behind an
	 * in-progress updater.  (Reporting is_insert here would wrongly let a
	 * deleter/updater skip the wait, since those never wait on inserts.)
	 */
	if (tuple_hdr->t_flags & FLUX_TUPLE_UPDATED)
	{
		if (TransactionIdIsValid(xmin) &&
			!TransactionIdIsCurrentTransactionId(xmin) &&
			TransactionIdIsInProgress(xmin))
			return xmin;		/* in-progress in-place updater;
								 * is_insert=false */
		return InvalidTransactionId;
	}

	/*
	 * Otherwise the only possible in-progress writer is the inserter
	 * (t_xmin): a live, not-yet-superseded tuple that is still UNCOMMITTED is
	 * being inserted.
	 */
	if (TransactionIdIsValid(xmin) &&
		!TransactionIdIsCurrentTransactionId(xmin) &&
		TransactionIdIsInProgress(xmin))
	{
		if (is_insert)
			*is_insert = true;
		return xmin;
	}

	return InvalidTransactionId;
}

/*
 * flux_inplace_index_maint_possible
 *
 * Can FLUX's in-place (stable-TID) index maintenance correctly and completely
 * maintain EVERY index on this relation for a key-changing UPDATE?  If not,
 * the caller must fall back to the out-of-place (new-TID) path and let the
 * executor do all index work (TU_All).
 *
 * flux_inplace_index_maintenance is the SINGLE authority for index work on the
 * in-place path -- the executor is told TU_None and does none.  That is only
 * sound for indexes where FLUX can reproduce everything the executor would
 * have done.  Four cases where it cannot:
 *
 * 1. The index AM has no delete-marking support (amdeletemark == NULL).  Only
 *    nbtree implements it; gist, gin, spgist and brin do not.  For those,
 *    index_delete_mark() returns false and the OLD (k_old, TID) entry silently
 *    stays LIVE while the row at TID now holds k_new.  The scan-side safety
 *    net does not catch it either: the key recheck in flux_index_getnext_slot
 *    needs scan->xs_itup, and xs_want_itup_recheck is honoured only by nbtree.
 *    Measured symptom: after UPDATE gs SET r = <far away> WHERE id = 7, a gist
 *    Index Scan for the OLD range still returned id=7 with its NEW range,
 *    while a seq scan of the same predicate correctly did not -- an index scan
 *    returning a row that does not satisfy the qual.  (A Bitmap Heap Scan hid
 *    it, because the bitmap recheck re-evaluates the qual against the row.)
 *
 * 2. The index enforces an EXCLUSION constraint (indisexclusion).  The check
 *    lives in the executor (ExecInsertIndexTuples ->
 *    check_exclusion_or_unique_constraint), not in the index AM, so reporting
 *    TU_None skips it entirely.  Measured symptom: a WITHOUT OVERLAPS primary
 *    key accepted an UPDATE that produced two overlapping rows for the same
 *    scalar key -- silent constraint violation, no error.  Note this is NOT
 *    covered by case 1 even though such indexes are usually gist: EXCLUDE USING
 *    btree is legal, and btree does have delete-marking.
 *
 * 3. The constraint is DEFERRABLE (!indimmediate).  The executor inserts with
 *    UNIQUE_CHECK_PARTIAL and returns the index OID in its recheckIndexes list,
 *    which ExecUpdateEpilogue hands to ExecARUpdateTriggers to queue the
 *    end-of-statement/transaction recheck.  FLUX's in-place maintenance passes
 *    UNIQUE_CHECK_PARTIAL too but DISCARDS the result and has no way to queue
 *    the after-trigger event, so a deferred violation would never be reported.
 *
 * 4. The index is not valid yet (!indisvalid): CREATE INDEX CONCURRENTLY or
 *    REINDEX CONCURRENTLY is building it, or such a build failed and left it.
 *    The build indexes each TID once, at the version its snapshot sees, and
 *    its validation (validate_index) only adds the TIDs the index does not
 *    have yet; neither looks at a TID again.  So a key that changes in place
 *    while the index is not ready is never indexed (maintenance skips a
 *    not-ready index, and the validation finds the TID present), and one that
 *    changes in place after it is ready leaves the index without the version
 *    the validation's snapshot sees.  Measured symptom: after CREATE INDEX
 *    CONCURRENTLY returned, an index scan for the new key found no row, a
 *    seqscan found it, and amcheck reported "lacks matching index tuple"; a
 *    UNIQUE build accepted a duplicate.  A new TID is what heap gives such an
 *    UPDATE (it is never HOT while the index is listed): the build or the
 *    validation then finds the new version missing and indexes it, and the
 *    old entry points at the old version.  This holds whichever indexed
 *    column changed -- key, INCLUDE, expression or predicate column -- as
 *    flux_indexed_attr_changed already compares all of them.
 *
 * A SUMMARIZING index (BRIN) is exempt from rule 1: it has no per-row (key,TID)
 * entry to delete-mark, and a range scan rechecks the heap tuple, so a stale
 * summary can never make a scan return a wrong row.  Rules 2, 3 and 4 still
 * apply to it.  See the per-index comment in the loop.
 *
 * Cheap: the index list and every rd_index/rd_indam field read here are
 * relcache-resident, and this only runs on the already-rare key-changing
 * UPDATE path, which opens every index again a moment later anyway.
 */
static bool
flux_inplace_index_maint_possible(Relation relation)
{
	List	   *indexoidlist;
	ListCell   *l;
	bool		ok = true;

	indexoidlist = RelationGetIndexList(relation);

	foreach(l, indexoidlist)
	{
		Relation	indexRel = index_open(lfirst_oid(l), AccessShareLock);

		/*
		 * A SUMMARIZING index (BRIN) is exempt from the delete-marking
		 * requirement.  It stores a per-block-range summary, not one entry
		 * per row, so there is no (key, TID) entry to delete-mark and no way
		 * for a scan to return a row under a stale key: a range scan always
		 * rechecks the heap tuple itself.  Demanding amdeletemark here forced
		 * every BRIN table's key UPDATE out of place, which moved rows to
		 * fresh pages and left new unsummarized ranges behind --
		 * brin/brin_bloom/brin_multi then saw brin_summarize_new_values()
		 * return 2 where 0 was expected.  It is also exactly the distinction
		 * the executor draws with TU_Summarizing / EIIT_ONLY_SUMMARIZING and
		 * ii_Summarizing.
		 *
		 * Summarizing indexes are still maintained: flux_indexed_attr_changed
		 * unions INDEX_ATTR_BITMAP_SUMMARIZED into the indexed-attribute set,
		 * so a summarized-column change is detected, and the in-place
		 * maintenance loop below reinserts into every index whose key
		 * changed, BRIN included.
		 */
		if (!indexRel->rd_indam->amsummarizing &&
			(indexRel->rd_indam->amdeletemark == NULL ||
			 indexRel->rd_index->indisexclusion ||
			 !indexRel->rd_index->indimmediate))
			ok = false;

		/*
		 * An exclusion or deferred constraint is a correctness gate
		 * regardless of summarization (the check lives in the executor, not
		 * the AM), so it applies even to a summarizing index.  No in-tree
		 * summarizing AM can back either kind today; keep the rule explicit
		 * rather than implied.
		 */
		if (indexRel->rd_index->indisexclusion ||
			!indexRel->rd_index->indimmediate)
			ok = false;

		/*
		 * Rule 4.  Like heap's HOT test this is per relation, not per index:
		 * while a build runs, a change to any indexed column moves the row,
		 * even one the index being built does not use.  Comparing each
		 * invalid index's own columns would keep those in place.
		 */
		if (!indexRel->rd_index->indisvalid)
			ok = false;

		index_close(indexRel, AccessShareLock);

		if (!ok)
			break;
	}

	list_free(indexoidlist);
	return ok;
}

/*
 * flux_index_key_changed
 *
 * Compare the OLD and NEW index key datums for one index and report whether
 * the key actually changed.  Shared by flux_inplace_index_maintenance (do we
 * need to insert a new entry and delete-mark the old one?) and
 * flux_inplace_index_undo (do we need to reverse that?), which MUST agree
 * exactly or rollback leaves the index in a half-reversed state.
 *
 * Only the KEY attributes participate: ii_NumIndexKeyAttrs, not
 * ii_NumIndexAttrs.  INCLUDE'd columns are not part of the key, they are not
 * covered by rd_opcintype, and changing one does not move the entry.
 *
 * TYPE METADATA, the subtle part.  The datums here come from FormIndexDatum,
 * which returns values in the opclass INPUT type (rd_opcintype) -- not in the
 * index tuple descriptor's type.  For most opclasses those coincide, but any
 * opclass with a STORAGE type (pg_opclass.opckeytype <> 0) makes them differ,
 * and then reading typlen/typbyval off RelationGetDescr(index) describes the
 * wrong type entirely.  Concretely, gist_int4_ops has input int4 (4 bytes,
 * BY VALUE) and storage gbtreekey8 (8 bytes, BY REFERENCE): taking the index
 * tupdesc's attbyval=false/attlen=8 sent datum_image_eq down its memcmp()
 * branch, which dereferenced the int4 VALUE 7 as a pointer and SEGFAULTED the
 * backend on any in-place UPDATE of a gist-indexed int column.  In the tree
 * as shipped, 15 of 179 opclasses have a storage type (all 72 brin, all 4
 * gin, 6 of 9 gist, 1 btree = name_ops, 1 spgist).
 *
 * So resolve the type from the opclass input type, falling back to the index
 * tupdesc only for an INCLUDE-style column with no opclass entry.  Pseudo
 * types reachable here (anyrange, anymultirange, anyarray, anyenum, record)
 * all have correct typlen/typbyval in pg_type, so get_typlenbyval is safe.
 */
static bool
flux_index_key_changed(Relation indexRel, IndexInfo *indexInfo,
					   const Datum *old_values, const bool *old_isnull,
					   const Datum *new_values, const bool *new_isnull)
{
	int			i;

	for (i = 0; i < indexInfo->ii_NumIndexKeyAttrs; i++)
	{
		int16		typlen;
		bool		typbyval;
		Oid			keytype;

		if (old_isnull[i] != new_isnull[i])
			return true;
		if (new_isnull[i])
			continue;

		/* Type of the datum FormIndexDatum produced: the opclass input type. */
		keytype = (indexRel->rd_opcintype != NULL) ?
			indexRel->rd_opcintype[i] : InvalidOid;

		if (OidIsValid(keytype))
			get_typlenbyval(keytype, &typlen, &typbyval);
		else
		{
			Form_pg_attribute att = TupleDescAttr(RelationGetDescr(indexRel), i);

			typlen = att->attlen;
			typbyval = att->attbyval;
		}

		if (!datum_image_eq(old_values[i], new_values[i], typbyval, typlen))
			return true;
	}

	return false;
}

/*
 * Do equal keys always have equal images in this btree index?  The test
 * _bt_allequalimage makes for deduplication, but over the key columns only:
 * an INCLUDE column is not compared, so it cannot make two keys equal.
 */
static bool
flux_index_keys_equalimage(Relation indexRel)
{
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(indexRel); i++)
	{
		Oid			opcintype = indexRel->rd_opcintype[i];
		Oid			proc = get_opfamily_proc(indexRel->rd_opfamily[i],
											 opcintype, opcintype,
											 BTEQUALIMAGE_PROC);

		if (!OidIsValid(proc) ||
			!DatumGetBool(OidFunctionCall1Coll(proc,
											   indexRel->rd_indcollation[i],
											   ObjectIdGetDatum(opcintype))))
			return false;
	}
	return true;
}

/*
 * flux_index_change_exact
 *
 * Can delete-marking describe this UPDATE in every index?  An in-place key
 * UPDATE leaves the row at its TID under two entries, the delete-marked
 * (k_old, TID) for snapshots that see the old version and the live
 * (k_new, TID) for the others, and each entry stands for the version whose
 * key it carries.  That needs k_old and k_new to be different keys: they are
 * told apart, and found again (index_delete_mark, index_undo_mark), only by
 * the index's comparison.  It fails in two cases, each found as an index that
 * amcheck reports corrupt:
 *
 * - an INCLUDE column changed.  If the key is the same, no entry is
 *   inserted or marked, and the one entry keeps the old INCLUDE values:
 *   bt_index_check(heapallindexed) found the table tuple lacking its entry.
 *   If the key changed too, a later change back to the old key revives its
 *   tombstone re-formed with the new INCLUDE values, and the version the
 *   tombstone stood for loses its entry the same way.
 *
 * - the index has an opclass that can call two different images equal
 *   (numeric 3 and 3.00, float8 0 and -0, a nondeterministic collation).  A
 *   change to an equal key with another image left two entries that compare
 *   equal with the same TID, which breaks nbtree's (key, TID) order ("item
 *   order invariant violated").  A change to a different key is no safer:
 *   3 -> 4 -> 3.0 revives the tombstone of 3 as 3.0, which hid the row from
 *   an index scan of a snapshot that sees the version holding 3.  So any key
 *   change in such an index is refused.
 *
 * - the row enters or leaves a partial index.  A reader rechecks an entry by
 *   its key only, and the planner drops the quals the predicate implies, so
 *   a delete-marked entry of the same key returned the row to snapshots that
 *   see it outside the predicate, and a partial unique index still counted
 *   it.  Coming back revived the entry it left, re-formed from the new
 *   version, which may have another INCLUDE value or key image.  Heap's
 *   UPDATE is not HOT here either.
 *
 * With these refused, every version at a TID is a member of a partial index
 * or none is, and all of them carry the same INCLUDE values, so a tombstone
 * an UPDATE revives is the entry the new version would get.
 *
 * Whether equal keys always have equal images is the opclasses' answer
 * (BTEQUALIMAGE_PROC), which nbtree keeps in the metapage for deduplication
 * (btm_allequalimage, see _bt_allequalimage).  An index with INCLUDE columns
 * is never allequalimage there, but its INCLUDE columns do not take part in
 * the comparison, so for it the key columns' opclasses are asked
 * (flux_index_keys_equalimage), and its INCLUDE columns are compared image
 * by image when the key stays the same.
 *
 * old_hdr (old_len bytes) is the version the UPDATE replaces.  Returns false
 * when some btree index's entry for this row would change in a way
 * delete-marking cannot describe: the caller then moves the row to a new
 * TID, as heap's non-HOT UPDATE does, and the executor maintains every index.
 * Other index AMs are not delete-marked (flux_inplace_index_maint_possible
 * refuses them), and summarizing ones keep no per-row entry.
 */
static bool
flux_index_change_exact(Relation relation, ItemPointer otid,
						FluxTupleHeader *old_hdr, Size old_len,
						TupleTableSlot *newslot)
{
	List	   *indexoidlist;
	ListCell   *l;
	EState	   *estate = NULL;
	TupleTableSlot *oldslot = NULL;
	ExprContext *econtext = NULL;
	bool		exact = true;

	indexoidlist = RelationGetIndexList(relation);

	foreach(l, indexoidlist)
	{
		Relation	indexRel = index_open(lfirst_oid(l), AccessShareLock);
		IndexInfo  *indexInfo;
		bool		heapkeyspace;
		bool		allequalimage;
		bool		old_member = true;
		bool		new_member = true;
		Datum		old_values[INDEX_MAX_KEYS];
		bool		old_isnull[INDEX_MAX_KEYS];
		Datum		new_values[INDEX_MAX_KEYS];
		bool		new_isnull[INDEX_MAX_KEYS];
		bool		key_changed;
		TupleDesc	itupdesc = RelationGetDescr(indexRel);

		if (indexRel->rd_rel->relam != BTREE_AM_OID ||
			!indexRel->rd_index->indisready)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		_bt_metaversion(indexRel, &heapkeyspace, &allequalimage);
		if (allequalimage &&
			IndexRelationGetNumberOfAttributes(indexRel) ==
			IndexRelationGetNumberOfKeyAttributes(indexRel) &&
			heap_attisnull(indexRel->rd_indextuple, Anum_pg_index_indpred,
						   NULL))
		{
			/*
			 * Equal keys have equal images and there is nothing else: no
			 * INCLUDE columns, no predicate.
			 */
			index_close(indexRel, AccessShareLock);
			continue;
		}

		if (estate == NULL)
		{
			estate = CreateExecutorState();
			econtext = GetPerTupleExprContext(estate);
			oldslot = MakeSingleTupleTableSlot(RelationGetDescr(relation),
											   table_slot_callbacks(relation));
			FluxSlotStoreTuple(oldslot, old_hdr, old_len, InvalidBuffer);
			oldslot->tts_tid = *otid;
			slot_getallattrs(oldslot);
			slot_getallattrs(newslot);
		}

		indexInfo = BuildIndexInfo(indexRel);
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *pred = ExecPrepareQual(indexInfo->ii_Predicate, estate);

			econtext->ecxt_scantuple = newslot;
			new_member = ExecQual(pred, econtext);
			econtext->ecxt_scantuple = oldslot;
			old_member = ExecQual(pred, econtext);
		}

		if (old_member != new_member)
			exact = false;
		else if (old_member)
		{
			econtext->ecxt_scantuple = oldslot;
			FormIndexDatum(indexInfo, oldslot, estate, old_values, old_isnull);
			econtext->ecxt_scantuple = newslot;
			FormIndexDatum(indexInfo, newslot, estate, new_values, new_isnull);

			key_changed = flux_index_key_changed(indexRel, indexInfo,
												 old_values, old_isnull,
												 new_values, new_isnull);

			/*
			 * Even a change to a different key is refused if equal keys can
			 * have different images: the entry it revives or later
			 * delete-marks may hold another image of an equal key (3 -> 4 ->
			 * 3.0 revives the tombstone of 3).
			 */
			if (key_changed && !allequalimage &&
				!flux_index_keys_equalimage(indexRel))
				exact = false;

			/* Every INCLUDE column must be unchanged, key change or not. */
			for (int i = indexInfo->ii_NumIndexKeyAttrs;
				 exact && i < indexInfo->ii_NumIndexAttrs; i++)
			{
				Form_pg_attribute att = TupleDescAttr(itupdesc, i);

				if (old_isnull[i] != new_isnull[i] ||
					(!old_isnull[i] &&
					 !datum_image_eq(old_values[i], new_values[i],
									 att->attbyval, att->attlen)))
					exact = false;
			}
			ResetExprContext(econtext);
		}

		index_close(indexRel, AccessShareLock);
		if (!exact)
			break;
	}

	if (estate != NULL)
	{
		ExecDropSingleTupleTableSlot(oldslot);
		FreeExecutorState(estate);
	}
	list_free(indexoidlist);
	return exact;
}

/*
 * flux_indexed_attr_changed
 *
 * Self-compute whether any INDEXED attribute changed value between the old
 * on-page tuple at 'otid' and the new row in 'slot'.  Stock PostgreSQL no
 * longer passes the modified-attrs set into tuple_update, so FLUX derives it
 * here exactly like heap does internally (HeapDetermineColumnsInfo): deform
 * the old on-page tuple, deform the new slot, and value-compare each indexed
 * attribute with datum_image_eq.
 *
 * Returns true if any indexed attribute differs, forcing the OUT-OF-PLACE
 * (delete + insert at new TID) update path so secondary indexes are rebuilt.
 * Whole-row (attr 0) and non-tableOID system attributes in the indexed set
 * are treated as changed (force out-of-place), matching heap's conservative
 * direction.  If the old tuple can't be read (gone/abnormal), we conservatively
 * return true so index maintenance still happens.
 *
 * The image is read without waiting, so it may be a concurrent writer's
 * uncommitted one.  The version compared is returned in *decided_verptr and
 * *decided_xmin (see flux_same_version) for the caller to check once the row
 * is its to write; *decided_xmin stays invalid if no image was compared.
 */
static bool
flux_indexed_attr_changed(Relation relation, ItemPointer otid,
						  TupleTableSlot *slot, UndoRecPtr *decided_verptr,
						  TransactionId *decided_xmin)
{
	Bitmapset  *indexed;
	Bitmapset  *summarized;
	TupleDesc	tupdesc = RelationGetDescr(relation);
	BlockNumber blkno = ItemPointerGetBlockNumber(otid);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(otid);
	Buffer		buffer;
	Page		page;
	ItemId		itemid;
	FluxTupleData old_tup;
	Datum		old_values[MaxTupleAttributeNumber];
	bool		old_isnull[MaxTupleAttributeNumber];
	bool		changed = false;
	int			attidx = -1;

	/*
	 * The full set of attributes referenced by ANY index on the relation:
	 * HOT_BLOCKING (ordinary/key indexes) plus SUMMARIZED (BRIN and friends).
	 * Heap's HeapDetermineColumnsInfo uses the same bitmaps; taking their
	 * union means "any indexed column changed" and lets FLUX force the
	 * out-of-place TU_All path, which rebuilds every index (summarizing
	 * included) at the new TID -- so FLUX never needs TU_Summarizing.
	 */
	indexed = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_HOT_BLOCKING);
	summarized = RelationGetIndexAttrBitmap(relation, INDEX_ATTR_BITMAP_SUMMARIZED);
	indexed = bms_add_members(indexed, summarized);
	bms_free(summarized);
	if (indexed == NULL)
		return false;			/* no indexed columns -> always in place */

	/* New values live in the slot; make sure they are all deformed. */
	slot_getallattrs(slot);

	/* Read the old on-page tuple. */
	buffer = ReadBuffer(relation, blkno);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		UnlockReleaseBuffer(buffer);
		bms_free(indexed);
		return true;			/* can't read old tuple -> be conservative */
	}
	itemid = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(itemid))
	{
		UnlockReleaseBuffer(buffer);
		bms_free(indexed);
		return true;
	}

	old_tup.t_len = ItemIdGetLength(itemid);
	old_tup.t_data = (FluxTupleHeader *) PageGetItem(page, itemid);
	ItemPointerSet(&old_tup.t_self, blkno, offnum);
	*decided_verptr = FluxTupleGetVersionPtr(old_tup.t_data);
	*decided_xmin = FluxTupleGetXmin(old_tup.t_data);

	/*
	 * Deform only as far as the highest ordinary indexed column.  Indexed
	 * columns are usually the leading attributes, so on a wide table this
	 * avoids deforming (walking + fetching) every trailing non-indexed column
	 * just to compare a few keys -- one of the two per-update overheads
	 * called out in the write-gap diagnosis.  Whole-row (attrnum 0) and
	 * system (attrnum < 0) references are handled in the loop below without
	 * reading old_values[], so they do not raise the bound.
	 */
	{
		int			max_indexed_natts = 0;
		int			probe = -1;

		while ((probe = bms_next_member(indexed, probe)) >= 0)
		{
			AttrNumber	probenum = probe + FirstLowInvalidHeapAttributeNumber;

			if (probenum > max_indexed_natts)
				max_indexed_natts = probenum;
		}
		FluxDeformTupleUpTo(relation, &old_tup, tupdesc, old_values, old_isnull,
							max_indexed_natts);
	}

	while (!changed && (attidx = bms_next_member(indexed, attidx)) >= 0)
	{
		AttrNumber	attrnum = attidx + FirstLowInvalidHeapAttributeNumber;
		Form_pg_attribute att;
		Datum		old_val,
					new_val;
		bool		old_null,
					new_null;

		/* Whole-row reference: treat as changed (force all-index). */
		if (attrnum == 0)
		{
			changed = true;
			break;
		}

		/*
		 * System attributes other than tableOID cannot be compared
		 * meaningfully for an updated row: treat as changed.  tableOID never
		 * changes.
		 */
		if (attrnum < 0)
		{
			if (attrnum != TableOidAttributeNumber)
				changed = true;
			continue;
		}

		old_val = old_values[attrnum - 1];
		old_null = old_isnull[attrnum - 1];
		new_val = slot->tts_values[attrnum - 1];
		new_null = slot->tts_isnull[attrnum - 1];

		/* Null-ness differs -> changed. */
		if (old_null != new_null)
		{
			changed = true;
			break;
		}
		/* Both null -> unchanged. */
		if (new_null)
			continue;

		att = TupleDescAttr(tupdesc, attrnum - 1);
		if (!datum_image_eq(old_val, new_val, att->attbyval, att->attlen))
		{
			changed = true;
			break;
		}
	}

	UnlockReleaseBuffer(buffer);
	bms_free(indexed);
	return changed;
}

/*
 * Is the image at tuple_hdr the version an UPDATE decided its index work
 * from?  Every in-place UPDATE and DELETE stamps a new t_verptr and its xid,
 * and their rollback restores the version before, so an unchanged
 * (t_verptr, t_xmin) pair means an unchanged row.  Freezing changes the pair
 * but not the row, which only costs a second decision.
 */
static inline bool
flux_same_version(const FluxTupleHeader *tuple_hdr, UndoRecPtr verptr,
				  TransactionId xmin)
{
	return FluxTupleGetVersionPtr(tuple_hdr) == verptr &&
		TransactionIdEquals(FluxTupleGetXmin(tuple_hdr), xmin);
}

/*
 * Count an in-place UPDATE in the cumulative statistics by its physical
 * effect.  pgstat turns every counted update into one dead tuple when its
 * transaction ends (the old version after a commit, the new one after an
 * abort), which is what a heap UPDATE leaves.  An in-place UPDATE that
 * changes no indexed column leaves none either way: its before-image goes to
 * UNDO, which the undo discard worker reclaims without VACUUM, and an abort
 * restores the row in place.  So that dead tuple is taken back, and the
 * update is HOT in heap's sense: no index got a new entry.  An in-place
 * UPDATE that changes a key inserts index entries and delete-marks the old
 * ones, which only VACUUM removes: it is not HOT, and its dead tuple stays
 * counted so that autovacuum comes to remove them.
 *
 * ponytail: pgstat drops the dead-tuple delta of a transaction that
 * truncated the relation, correction included, so an in-place UPDATE after a
 * TRUNCATE in the same transaction counts one dead tuple, as heap's would.
 */
static void
flux_count_inplace_update(Relation relation, bool key_changed)
{
	pgstat_count_heap_update(relation, !key_changed, false);
	if (!key_changed)
		pgstat_update_heap_dead_tuples(relation, 1);
}

/*
 * flux_update_out_of_place
 *
 * Perform an out-of-place (new-TID) UPDATE: DELETE the old version at otid and
 * INSERT the new version from slot, which lands at a fresh TID.  Used in two
 * cases:
 *   1. an indexed attribute changed (secondary indexes carry plain heap-style
 *      TIDs, so the row must move and every index entry be rebuilt), and
 *   2. as a last-resort fallback when an in-place UPDATE of a NON-indexed
 *      column cannot fit in its slot on a full page (FLUX TIDs are stable, so
 *      the tuple cannot grow in place and cannot relocate within the page).
 *
 * In BOTH cases the executor must rebuild every index entry at the new TID, so
 * this always reports TU_All: even for a non-indexed-column update, the row's
 * unchanged indexed columns still need fresh (key, newTID) entries because the
 * old (key, oldTID) entries die with the old TID.  Reporting TU_None here would
 * leave indexes pointing at the dead old TID with nothing at the new one --
 * silent index corruption.  The old version stays visible to older snapshots
 * via the DELETE's UNDO before-image; ROLLBACK undoes both the delete and the
 * insert.  The old version links to the new one (flux_link_superseded), so
 * a concurrent writer or locker follows the row as it does in heap.
 */
static TM_Result
flux_update_out_of_place(Relation relation, ItemPointer otid,
						 TupleTableSlot *slot, CommandId cid, uint32 options,
						 Snapshot snapshot, Snapshot crosscheck, bool wait,
						 TM_FailureData *tmfd, LockTupleMode *lockmode,
						 TU_UpdateIndexes *update_indexes)
{
	TM_Result	del_res;
	bool		key_intact;
	FluxTuple	old;

	/*
	 * Report the lock mode before anything can fail, as heap_update does: on
	 * TM_Updated the executor locks the row with it to recheck it, and it
	 * must be the exclusive lock an UPDATE that moves the row takes.  Left at
	 * zero it would be LockTupleKeyShare, and FLUX's key-share lock path
	 * serves the snapshot's pre-update image, so EvalPlanQual recomputed the
	 * new row from the version the concurrent update had replaced: a lost
	 * update.
	 */
	if (lockmode != NULL)
		*lockmode = LockTupleExclusive;

	/*
	 * The halves are not decoded: the link record carries the logical UPDATE.
	 * The delete half takes delete options, not the update's: bit 0 is
	 * TABLE_UPDATE_NO_LOGICAL there but TABLE_DELETE_CHANGING_PARTITION here.
	 */
	del_res = flux_tuple_delete(relation, otid, cid,
								TABLE_DELETE_NO_LOGICAL | FLUX_DML_NO_STATS,
								snapshot, crosscheck, wait, tmfd);
	if (del_res != TM_Ok)
		return del_res;

	/* The deleted version is now ours and cannot change under the comparison. */
	old = flux_copy_version(relation, otid);
	key_intact = !flux_update_changes_key(relation, old->t_data, old->t_len, slot);
	FluxFreeTuple(old);

	flux_tuple_insert(relation, slot, cid,
					  TABLE_INSERT_NO_LOGICAL | FLUX_DML_NO_STATS, NULL);
	flux_link_superseded(relation, otid, &slot->tts_tid,
						 (options & TABLE_UPDATE_NO_LOGICAL) == 0,
						 key_intact);

	/*
	 * One UPDATE, counted as heap counts a non-HOT one: the deleted version
	 * is a dead tuple for VACUUM, and every index got a new entry.
	 */
	pgstat_count_heap_update(relation, false,
							 ItemPointerGetBlockNumber(otid) !=
							 ItemPointerGetBlockNumber(&slot->tts_tid));

	if (update_indexes != NULL)
		*update_indexes = TU_All;
	return TM_Ok;
}

/*
 * flux_inplace_index_maintenance
 *
 * After an in-place UPDATE that kept the row at its stable TID,
 * bring the secondary indexes up to date WITHOUT moving the row: for every
 * index whose key value for this row changed, INSERT the new (k_new, TID)
 * entry and DELETE-MARK the old (k_old, TID) entry (index_delete_mark, Phase
 * 5).  Indexes whose key did not change are left untouched -- their (k, TID)
 * entry is still valid because the TID is stable.
 *
 * This is the SINGLE authority for which indexes changed and what each needs,
 * so there is no cross-component agreement hazard: the executor is told
 * TU_None and does no index work for a FLUX indexed-column UPDATE.
 *
 * 'old_hdr'/'old_len' is the BEFORE-image on-page tuple (captured by the
 * caller before the in-place overwrite); 'newslot' holds the new row (its
 * tts_tid is the stable TID).  Partial and expression indexes are handled the
 * same way the executor's ExecInsertIndexTuples does: FormIndexDatum builds
 * expression keys, and a partial index's predicate is evaluated against both
 * the old and the new row so an entry is inserted only when the new row is a
 * member and delete-marked only when the old row was a member.
 *
 * On ROLLBACK the FLUX table UNDO (flux_undo_apply, FLUX_UNDO_UPDATE) reverses
 * exactly this: it clears the delete-mark on (k_old, TID) and delete-marks
 * (k_new, TID) (flux_inplace_index_undo), so each key leads to the row as it
 * did before the update.  So no
 * separate index UNDO is written for a FLUX-parented index (nbtree/hash
 * suppress it via RelationSupportsDeleteMarking).
 */
static void
flux_inplace_index_maintenance(Relation relation, ItemPointer otid,
							   FluxTupleHeader *old_hdr, Size old_len,
							   TupleTableSlot *newslot, UndoRecPtr urp)
{
	List	   *indexoidlist;
	ListCell   *l;
	EState	   *estate;
	TupleTableSlot *oldslot;
	ExprContext *econtext;
	Oid		   *revived;
	uint16		nrevived = 0;

	(void) old_len;				/* FluxTupleToSlotWithOverflow reads t_len
								 * itself */

	indexoidlist = RelationGetIndexList(relation);
	if (indexoidlist == NIL)
		return;

	revived = palloc(list_length(indexoidlist) * sizeof(Oid));
	estate = CreateExecutorState();
	oldslot = MakeSingleTupleTableSlot(RelationGetDescr(relation),
									   table_slot_callbacks(relation));

	/* Materialize the before-image into oldslot to form the old keys. */
	FluxTupleToSlotWithOverflow(old_hdr, oldslot, relation);
	oldslot->tts_tid = *otid;

	slot_getallattrs(newslot);
	slot_getallattrs(oldslot);

	econtext = GetPerTupleExprContext(estate);

	foreach(l, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(l);
		Relation	indexRel;
		IndexInfo  *indexInfo;
		Datum		new_values[INDEX_MAX_KEYS];
		bool		new_isnull[INDEX_MAX_KEYS];
		Datum		old_values[INDEX_MAX_KEYS];
		bool		old_isnull[INDEX_MAX_KEYS];
		bool		old_member = true;
		bool		new_member = true;
		bool		key_changed;

		indexRel = index_open(indexoid, RowExclusiveLock);

		/* Not-yet-ready indexes are maintained by the build, not here. */
		if (!indexRel->rd_index->indisready)
		{
			index_close(indexRel, RowExclusiveLock);
			continue;
		}

		indexInfo = BuildIndexInfo(indexRel);

		/* Partial index: evaluate the predicate against old and new rows. */
		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *pred = ExecPrepareQual(indexInfo->ii_Predicate, estate);

			econtext->ecxt_scantuple = newslot;
			new_member = ExecQual(pred, econtext);
			econtext->ecxt_scantuple = oldslot;
			old_member = ExecQual(pred, econtext);
		}

		if (new_member)
		{
			econtext->ecxt_scantuple = newslot;
			FormIndexDatum(indexInfo, newslot, estate, new_values, new_isnull);
		}
		if (old_member)
		{
			econtext->ecxt_scantuple = oldslot;
			FormIndexDatum(indexInfo, oldslot, estate, old_values, old_isnull);
		}

		/*
		 * Decide whether the index key actually changed for this row.  When
		 * both versions are index members we compare the formed key datums;
		 * differing predicate membership is itself a change (an entry appears
		 * or disappears).
		 */
		if (old_member != new_member)
			key_changed = true;
		else if (!old_member)	/* neither indexed -> nothing to do */
			key_changed = false;
		else
			key_changed = flux_index_key_changed(indexRel, indexInfo,
												 old_values, old_isnull,
												 new_values, new_isnull);

		if (key_changed)
		{
			/*
			 * Insert the new (k_new, TID) entry so new-snapshot readers reach
			 * the row under its new key.  A->B->A recurrence: if a
			 * delete-marked (k_new, TID) tombstone already exists (a previous
			 * UPDATE moved the key away and back), REVIVE it (clear the mark)
			 * instead of inserting a duplicate (key, TID) -- two entries with
			 * the same key AND heap TID would violate the heapkeyspace
			 * ordering invariant (amcheck "item order invariant violated").
			 * Only insert a fresh entry when no such tombstone exists.
			 * Uniqueness is enforced on the insert exactly as the executor
			 * would (immediate unique indexes throw on a real duplicate;
			 * delete-marked tombstones are skipped).
			 */
			if (new_member)
			{
				bool		revive = index_probe_delete_mark(indexRel, new_values,
															 new_isnull, &newslot->tts_tid, relation);

				if (revive)
				{
					revived[nrevived++] = indexoid;
					FluxPbuAmendRevivedList(urp, revived, nrevived);
					INJECTION_POINT("flux-index-after-amend", NULL);
					/* Never insert after logging a revive intent. */
					if (!index_undo_mark(indexRel, new_values, new_isnull,
										 &newslot->tts_tid, relation, true))
						elog(ERROR, "FLUX index tombstone disappeared before revival");
				}
				if (!revive)
				{
					IndexUniqueCheck checkUnique;

					if (!indexRel->rd_index->indisunique)
						checkUnique = UNIQUE_CHECK_NO;
					else if (indexRel->rd_index->indimmediate)
						checkUnique = UNIQUE_CHECK_YES;
					else
						checkUnique = UNIQUE_CHECK_PARTIAL;

					index_insert(indexRel, new_values, new_isnull,
								 &newslot->tts_tid, relation,
								 checkUnique, false, indexInfo);
				}
				else if (indexRel->rd_index->indisunique)
				{
					bool		anynull = false;

					/*
					 * A revived entry skipped the uniqueness check an insert
					 * runs.  The row left this key, and another row may have
					 * taken it since.  Check the entry now in place, as a
					 * deferred unique constraint's recheck does: wait for an
					 * in-progress holder, raise unique_violation for a live
					 * one. A key with a null is no duplicate unless NULLS NOT
					 * DISTINCT, and nbtree does not check it.
					 */
					for (int keyno = 0; keyno < indexInfo->ii_NumIndexKeyAttrs; keyno++)
						anynull |= new_isnull[keyno];
					if (!anynull || indexInfo->ii_NullsNotDistinct)
						index_insert(indexRel, new_values, new_isnull,
									 &newslot->tts_tid, relation,
									 UNIQUE_CHECK_EXISTING, false, indexInfo);
				}
			}

			/*
			 * Delete-mark the old (k_old, TID) entry so old-snapshot readers
			 * still reach the before-image under the old key, and a recheck
			 * keeps new-snapshot readers from seeing it under the old key.
			 * Missing entry is not an error (btdeletemark).
			 */
			if (old_member)
				(void) index_delete_mark(indexRel, old_values, old_isnull,
										 (ItemPointer) otid, relation);
		}

		/*
		 * Every index_insert() must be paired with index_insert_cleanup()
		 * before the index is closed, exactly as the executor does
		 * (execIndexing.c ExecCloseIndices) and as index.c/constraint.c do.
		 * Some AMs cache per-insert state in indexInfo->ii_AmCache that owns
		 * pinned buffers -- BRIN keeps its revmap buffer there -- and only
		 * the aminsertcleanup callback releases them.  Omitting it leaked one
		 * pin per inserted row per BRIN index page, which surfaced as a flood
		 * of "WARNING: resource was not closed: ... refcount=1" at
		 * transaction end (regression tests brin, brin_bloom, brin_multi,
		 * stats under default_table_access_method=flux).
		 */
		index_insert_cleanup(indexRel, indexInfo);

		index_close(indexRel, RowExclusiveLock);
	}

	pfree(revived);
	list_free(indexoidlist);
	ExecDropSingleTupleTableSlot(oldslot);
	FreeExecutorState(estate);
}

/*
 * flux_inplace_index_undo
 *
 * ROLLBACK reversal of flux_inplace_index_maintenance.  Before the
 * table UNDO restores the before-image tuple in place (see
 * flux_undo_index_first), bring the secondary indexes back to their
 * pre-update state for every index whose key changed: delete-mark a revived
 * new entry, physically remove an inserted one, and clear the mark on the old
 * entry so it is a live entry again.  Both steps are
 * idempotent, so a retry after a failure is harmless.
 *
 * 'new_hdr' is the current on-page (post-update, k_new) tuple; 'old_hdr' is
 * the before-image (k_old) about to be restored.  'indexes' is the record's
 * list of nindexes FluxUndoIndex entries: the indexes maintenance may have
 * changed, and the files it changed.
 * Partial/expression indexes are handled exactly as the forward path does:
 * an entry only needs delete-marking where the new row was a member, and only needs
 * unmarking where the old row was a member.
 *
 * This is why FLUX writes NO separate index UNDO: this function IS the index
 * UNDO, driven by the table's own undo apply (decision (a)).
 *
 * The relcache must show each listed index with the file the record names.
 * While the transaction xid is aborting, it may not: the transaction itself
 * dropped the index, or gave it a new file (REINDEX, SET TABLESPACE), and
 * the abort has not rolled that back yet.  Then return false, changing
 * nothing more, and the caller leaves the record for later: for after the
 * abort (PbuApplyPendingUndo), or, for ROLLBACK TO SAVEPOINT, for the retry
 * as the parent (PbuRetrySubxactUndo).  By then an index that is missing or
 * has another file was created after the record's file, by the rolled-back
 * transaction itself or a later one, and built from the table: there is
 * nothing to reverse in it.
 */
bool
flux_inplace_index_undo(Relation relation, ItemPointer tid,
						FluxTupleHeader *new_hdr, Size new_len,
						FluxTupleHeader *old_hdr, Size old_len,
						const char *indexes, uint16 nindexes,
						TransactionId xid)
{
	List	   *indexoidlist;
	EState	   *estate;
	TupleTableSlot *oldslot;
	TupleTableSlot *newslot;
	ExprContext *econtext;
	bool		pushed_snapshot = false;

	bool		aborting = TransactionIdEquals(xid, GetTopTransactionIdIfAny()) &&
		!PbuInSubxactRetry();
	bool		done = true;

	(void) new_len;
	(void) old_len;

	indexoidlist = RelationGetIndexList(relation);

	/*
	 * Index expressions and predicates may run SQL (a PL/pgSQL function),
	 * which needs an active snapshot.  An undo apply worker has none.
	 */
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed_snapshot = true;
	}

	estate = CreateExecutorState();
	oldslot = MakeSingleTupleTableSlot(RelationGetDescr(relation),
									   table_slot_callbacks(relation));
	newslot = MakeSingleTupleTableSlot(RelationGetDescr(relation),
									   table_slot_callbacks(relation));

	FluxTupleToSlotWithOverflow(old_hdr, oldslot, relation);
	oldslot->tts_tid = *tid;

	FluxTupleToSlotWithOverflow(new_hdr, newslot, relation);
	newslot->tts_tid = *tid;

	slot_getallattrs(oldslot);
	slot_getallattrs(newslot);

	econtext = GetPerTupleExprContext(estate);

	for (int i = 0; i < nindexes; i++)
	{
		Relation	indexRel;
		IndexInfo  *indexInfo;
		Datum		new_values[INDEX_MAX_KEYS];
		bool		new_isnull[INDEX_MAX_KEYS];
		Datum		old_values[INDEX_MAX_KEYS];
		bool		old_isnull[INDEX_MAX_KEYS];
		bool		old_member = true;
		bool		new_member = true;
		bool		key_changed;
		FluxUndoIndex entry;

		memcpy(&entry, indexes + i * sizeof(entry), sizeof(entry));
		if (!list_member_oid(indexoidlist, entry.indexoid))
		{
			if (aborting)
			{
				done = false;
				break;
			}
			continue;
		}

		indexRel = index_open(entry.indexoid, RowExclusiveLock);
		if (indexRel->rd_locator.relNumber != entry.relnumber ||
			indexRel->rd_locator.spcOid != entry.spcoid)
		{
			index_close(indexRel, RowExclusiveLock);
			if (aborting)
			{
				done = false;
				break;
			}
			continue;
		}
		if (!indexRel->rd_index->indisready)
		{
			index_close(indexRel, RowExclusiveLock);
			continue;
		}

		indexInfo = BuildIndexInfo(indexRel);

		if (indexInfo->ii_Predicate != NIL)
		{
			ExprState  *pred = ExecPrepareQual(indexInfo->ii_Predicate, estate);

			econtext->ecxt_scantuple = newslot;
			new_member = ExecQual(pred, econtext);
			econtext->ecxt_scantuple = oldslot;
			old_member = ExecQual(pred, econtext);
		}

		if (new_member)
		{
			econtext->ecxt_scantuple = newslot;
			FormIndexDatum(indexInfo, newslot, estate, new_values, new_isnull);
		}
		if (old_member)
		{
			econtext->ecxt_scantuple = oldslot;
			FormIndexDatum(indexInfo, oldslot, estate, old_values, old_isnull);
		}

		if (old_member != new_member)
			key_changed = true;
		else if (!old_member)
			key_changed = false;
		else
			key_changed = flux_index_key_changed(indexRel, indexInfo,
												 old_values, old_isnull,
												 new_values, new_isnull);

		if (key_changed)
		{
			if (new_member)
			{
				if (entry.flags & FLUX_UNDO_INDEX_REVIVED)
					(void) index_delete_mark(indexRel, new_values, new_isnull,
											 tid, relation);
				else
					(void) index_undo_mark(indexRel, new_values, new_isnull,
										   tid, relation, false);
			}
			/* Clear the delete-mark on the old (k_old, TID) entry. */
			if (old_member)
				(void) index_undo_mark(indexRel, old_values, old_isnull,
									   tid, relation, true);
		}

		index_close(indexRel, RowExclusiveLock);
	}

	list_free(indexoidlist);
	ExecDropSingleTupleTableSlot(oldslot);
	ExecDropSingleTupleTableSlot(newslot);
	FreeExecutorState(estate);
	if (pushed_snapshot)
		PopActiveSnapshot();
	return done;
}

/*
 * flux_tuple_update -- table AM entry point.
 *
 * Wrapper around the in-place update machinery.  An indexed-column
 * UPDATE stays IN PLACE (stable TID); this wrapper captures the old row's
 * before-image, runs the in-place update, and -- if any indexed column changed
 * and the update really stayed in place -- drives the secondary-index
 * maintenance at the stable TID (insert new (k_new,TID) + delete-mark old
 * (k_old,TID) per changed index).  It reports TU_None so the executor does no
 * further index work; on abort the FLUX table UNDO reverses both the tuple and
 * the index changes.
 *
 * The rare genuine no-room case still falls back to an out-of-place update
 * (inner returns with did_inplace_key_update = false and TU_All); there we do
 * NOT run in-place maintenance -- the executor rebuilds every index at the new
 * TID as before.
 */
TM_Result
flux_tuple_update(Relation relation, ItemPointer otid, TupleTableSlot *slot,
				  CommandId cid, uint32 options,
				  Snapshot snapshot, Snapshot crosscheck,
				  bool wait, TM_FailureData *tmfd,
				  LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	bool		key_changed;
	bool		did_inplace;
	UndoRecPtr	index_urp;
	FluxTupleHeader *old_copy;
	Size		old_copy_len;
	UndoRecPtr	decided_verptr;
	TransactionId decided_xmin;
	bool		redecide;
	bool		kept_tuplock = false;
	LockTupleMode kept_mode = LockTupleExclusive;
	TM_Result	res;

	/*
	 * Decide up front whether any indexed column changed, and if so grab a
	 * private copy of the old on-page tuple BEFORE the in-place overwrite so
	 * we can form the old index keys (k_old) for delete-marking after the
	 * update.  We only need this when the relation has indexes and an indexed
	 * attribute actually changed; the common non-key UPDATE skips all of it.
	 *
	 * The version compared may be a concurrent writer's uncommitted one,
	 * which its rollback replaces while we wait for it.  Then
	 * flux_tuple_update_inplace finds another version once the row is ours,
	 * and we decide again, as heap_update starts over at l2 after a wait.
	 */
decide:
	did_inplace = false;
	index_urp = InvalidUndoRecPtr;
	old_copy = NULL;
	old_copy_len = 0;
	decided_verptr = InvalidUndoRecPtr;
	decided_xmin = InvalidTransactionId;
	key_changed = (relation->rd_rel->relhasindex &&
				   flux_indexed_attr_changed(relation, otid, slot,
											 &decided_verptr, &decided_xmin));
	/* Tests change the row here, after the decision was made from it. */
	INJECTION_POINT("flux-update-after-decision", NULL);

	/*
	 * In-place index maintenance is only correct when FLUX can fully
	 * reproduce the executor's index work for every index on the relation
	 * (see flux_inplace_index_maint_possible: delete-marking capability,
	 * exclusion constraints, deferred constraints).  When it cannot, take the
	 * out-of-place (new-TID) path, which reports TU_All so the executor does
	 * ALL index work including the constraint checks -- correctness over the
	 * stable-TID optimisation.
	 */
	if (key_changed && !flux_inplace_index_maint_possible(relation))
	{
		res = flux_update_out_of_place(relation, otid, slot, cid, options,
									   snapshot, crosscheck, wait, tmfd,
									   lockmode, update_indexes);
		goto done;
	}

	if (key_changed)
	{
		BlockNumber blkno = ItemPointerGetBlockNumber(otid);
		OffsetNumber offnum = ItemPointerGetOffsetNumber(otid);
		Buffer		buffer;
		Page		page;
		ItemId		itemid;

		if (blkno < RelationGetNumberOfBlocks(relation))
		{
			buffer = ReadBuffer(relation, blkno);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buffer);
			if (offnum >= FirstOffsetNumber &&
				offnum <= PageGetMaxOffsetNumber(page))
			{
				itemid = PageGetItemId(page, offnum);
				if (ItemIdIsNormal(itemid))
				{
					old_copy_len = ItemIdGetLength(itemid);
					old_copy = (FluxTupleHeader *) palloc(old_copy_len);
					memcpy(old_copy, PageGetItem(page, itemid), old_copy_len);
				}
			}
			UnlockReleaseBuffer(buffer);
		}
		/* Old tuple unreadable: fall back to treating as no in-place maint. */
		if (old_copy == NULL)
			key_changed = false;

		/*
		 * flux_index_change_exact below reads this copy: it must be the
		 * version key_changed was computed from.
		 */
		else if (!flux_same_version(old_copy, decided_verptr, decided_xmin))
		{
			pfree(old_copy);
			goto decide;
		}
	}

	/*
	 * A change that delete-marking cannot describe moves the row instead, see
	 * flux_index_change_exact.
	 */
	if (key_changed &&
		!flux_index_change_exact(relation, otid, old_copy, old_copy_len,
								 slot))
	{
		pfree(old_copy);
		res = flux_update_out_of_place(relation, otid, slot, cid, options,
									   snapshot, crosscheck, wait, tmfd,
									   lockmode, update_indexes);
		goto done;
	}

	res = flux_tuple_update_inplace(relation, otid, slot, cid, options,
									snapshot, crosscheck, wait, tmfd,
									lockmode, update_indexes, key_changed,
									&did_inplace, &index_urp, &old_copy, &old_copy_len,
									decided_verptr, decided_xmin, &redecide,
									&kept_tuplock, &kept_mode);
	if (redecide)
	{
		if (old_copy != NULL)
			pfree(old_copy);
		INJECTION_POINT("flux-update-redecide", NULL);
		goto decide;
	}
	res = flux_report_moved(relation, otid, res, tmfd);

	/*
	 * The slot now stands for the version just stored, stamped with our xid
	 * and cid by every update path (flux_tuple_insert does the same for an
	 * out-of-place one): the AFTER triggers' NEW row.
	 */
	if (res == TM_Ok)
		FluxSlotSetVersion(slot, GetCurrentTransactionId(), cid);

	if (res == TM_Ok && key_changed && did_inplace)
	{
		/*
		 * The row stayed at its TID and an indexed column changed: maintain
		 * the secondary indexes in place.  slot->tts_tid was set to the
		 * stable TID by the in-place path.
		 */
		flux_inplace_index_maintenance(relation, otid, old_copy, old_copy_len,
									   slot, index_urp);
		if (update_indexes != NULL)
			*update_indexes = TU_None;
	}

	if (old_copy != NULL)
		pfree(old_copy);

done:
	if (kept_tuplock)
		FluxUnlockTuple(relation, otid, kept_mode);
	return res;
}

/*
 * flux_tuple_update_inplace
 *
 * The in-place UPDATE machinery: overwrite the tuple at 'otid' in place
 * (stable TID), writing the before-image to per-backend UNDO.  Never moves the
 * row for an indexed-column change; it only falls back to
 * out-of-place when the new tuple genuinely cannot fit on the page even after
 * defrag/shrink (the heap cross-page analogue).  On a successful in-place
 * update of an indexed-column change *did_inplace_key_update is set true so the
 * caller (flux_tuple_update) drives the secondary-index maintenance
 * (insert new + delete-mark old) at the stable TID.
 *
 * key_changed was computed from the version (decided_verptr, decided_xmin),
 * see flux_indexed_attr_changed; an invalid decided_xmin means no version was
 * compared.  If the row holds another version once it is ours to write,
 * nothing is changed: *redecide is set, and the caller decides again.  The
 * tuple lock this call took to wait is then handed to the caller, which sets
 * *kept_tuplock and *kept_mode and releases it once it is done, so that the
 * UPDATE keeps its place among the row's waiters while it decides again.
 */
static TM_Result
flux_tuple_update_inplace(Relation relation, ItemPointer otid, TupleTableSlot *slot,
						  CommandId cid, uint32 options,
						  Snapshot snapshot, Snapshot crosscheck,
						  bool wait, TM_FailureData *tmfd,
						  LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes,
						  bool key_changed,
						  bool *did_inplace_key_update, UndoRecPtr *index_urp,
						  FluxTupleHeader **index_old, Size *index_old_len,
						  UndoRecPtr decided_verptr, TransactionId decided_xmin,
						  bool *redecide, bool *kept_tuplock,
						  LockTupleMode *kept_mode)
{
	BlockNumber blkno;
	OffsetNumber offnum;
	Buffer		buffer;
	Page		page;
	ItemId		itemid;
	FluxTupleHeader *old_tuple_hdr;
	FluxTuple	new_tuple;
	Size		new_tuple_size;
	uint64		current_ts;
	bool		old_has_overflow = false;
	bool		have_tuple_lock = false;
	LockTupleMode update_lockmode = key_changed ?
		LockTupleExclusive : LockTupleNoKeyExclusive;
	bool		locked_by_me = false;
	FluxTuple	old_tuple_for_inplace_wal;
	TransactionId update_freeze_conflict;
	bool		walLogical = (options & TABLE_UPDATE_NO_LOGICAL) == 0;
	FluxLogicalImage update_old_img;
	FluxLogicalImage update_new_img;
	FluxOverflowBuffers update_overflow_buffers;
	int			upd_i;
	uint64		defrag_oldest_ts = 0;
	TransactionId defrag_oldest_xmin = InvalidTransactionId;
	bool		force_shrink_attempted = false;
	UndoRecPtr	upd_undo_ptr = InvalidUndoRecPtr;
	Datum		upd_toast_values[MaxTupleAttributeNumber];
	bool		upd_toast_isnull[MaxTupleAttributeNumber];
	ToastAttrInfo upd_toast_attr[MaxTupleAttributeNumber];
	ToastTupleContext upd_ttc;
	bool		upd_toasted = false;
	Datum		upd_old_values[MaxTupleAttributeNumber];
	bool		upd_old_isnull[MaxTupleAttributeNumber];
	FluxTupleHeader *upd_old_copy = NULL;
	Buffer		vmbuffer;

	*redecide = false;

	/* Extract block and offset from old TID */
	blkno = ItemPointerGetBlockNumber(otid);
	offnum = ItemPointerGetOffsetNumber(otid);


	/* Validate TID range */
	if (blkno >= RelationGetNumberOfBlocks(relation))
		return TM_Invisible;

	/*
	 * Concurrent updaters of the same row are serialized the way heap_update
	 * serializes them: by the identity of the row's latest writer, which FLUX
	 * keeps in the tuple itself.  An in-place UPDATE stamps the new image's
	 * t_xmin with the updater's xid and marks it FLUX_TUPLE_UPDATED |
	 * FLUX_TUPLE_UNCOMMITTED (flux_header_dirty_xid reads it back); a DELETE
	 * stamps t_xmax.  Both stay on the page until the writer's commit or
	 * rollback is complete.  So an updater that finds an in-progress foreign
	 * writer on the row waits for it (XactLockTableWait), holding the
	 * heavyweight tuple lock only for the duration of that wait so that
	 * several waiters queue up behind each other, and then reports TM_Updated
	 * so the executor re-evaluates via EvalPlanQual.  A writer that already
	 * committed but is invisible to our snapshot is caught by the
	 * committed-update gates (FluxTupleHasCommittedUpdateAfter) and also
	 * returns TM_Updated.
	 *
	 * No tuple lock is held to commit: one per updated row exhausted the
	 * shared lock table on any UPDATE of more than a few thousand rows.
	 */

	/*
	 * Update in place, even when an indexed column changes.
	 *
	 * With nbtree delete-marking the row keeps its TID: the in-place
	 * machinery below overwrites the tuple at 'otid', and the caller
	 * (flux_tuple_update) inserts the new (k_new, TID) entry and delete-marks
	 * the old (k_old, TID) entry for each index whose key changed.  Unchanged
	 * indexes are untouched (their (k, TID) entry is still valid).  On abort
	 * the FLUX table UNDO reverses both the tuple and the index changes -- no
	 * separate index UNDO.
	 *
	 * The only remaining out-of-place case is a genuine no-room fallback: an
	 * in-place-grown tuple that cannot fit on its page even after
	 * defrag/shrink (the heap cross-page analogue), handled at the ERROR
	 * sites far below. That path sets *did_inplace_key_update = false so the
	 * caller does not also run in-place index maintenance (the out-of-place
	 * path already sets TU_All to rebuild every index at the new TID).
	 */
	if (did_inplace_key_update != NULL)
		*did_inplace_key_update = true;

	/*
	 * Report the tuple-lock strength this UPDATE effectively holds, exactly
	 * as heap_update does (heapam.c: LockTupleNoKeyExclusive when no key
	 * column changed, LockTupleExclusive otherwise).  The executor keeps this
	 * in UpdateContext.lockmode -- an UNINITIALIZED stack field -- and passes
	 * it straight back to table_tuple_lock() when it has to re-lock the
	 * latest version for EvalPlanQual (nodeModifyTable.c, the TM_Updated ->
	 * TUPLE_LOCK_FLAG_FIND_LAST_VERSION path).
	 *
	 * Leaving it unset was a live lost-update bug: the garbage value happened
	 * to be 0 == LockTupleKeyShare, so an UPDATE that lost the race re-locked
	 * the row with a KEY-SHARE lock instead of an exclusive one.  That is
	 * compatible with the winner's in-progress non-key UPDATE, so the loser
	 * never waited, re-read the pre-update image, and overwrote the winner's
	 * committed value ('' -> 'A' -> 'B' instead of 'AB').
	 */
	if (lockmode != NULL)
		*lockmode = update_lockmode;

	/*
	 * In-place UPDATE default: the row keeps its TID.  The caller overrides
	 * *update_indexes to TU_None after driving any delete-marking
	 * maintenance, and for a non-key UPDATE this TU_None is already correct
	 * (no index work).
	 */
	if (update_indexes != NULL)
		*update_indexes = TU_None;

	/*
	 * --------------------------------------------------------------- FAST
	 * PATH: Same-size CAS update under SHARE_EXCLUSIVE buffer lock.
	 *
	 * For simple same-size updates (e.g. balance += delta in TPC-B), we can
	 * avoid the fully exclusive buffer lock by using a share-exclusive lock.
	 * This still allows concurrent plain-SHARE readers while serializing
	 * writers on the same page: BUFFER_LOCK_SHARE_EXCLUSIVE conflicts with
	 * itself and with BUFFER_LOCK_EXCLUSIVE (bufmgr.c BufferLockAttempt), so
	 * while we hold it no other backend can modify this page, and the buffer
	 * lock alone makes us the tuple's only writer.
	 *
	 * NOTE on hot-row contention (investigated, no change made): at most one
	 * CAS writer per buffer runs at a time; additional hot-row writers
	 * queue+sleep on the buffer content lock.  There is no spin storm to
	 * convert to a queued lock: the hot-row path ALREADY degrades to fair
	 * queued locks (BUFFER_LOCK_SHARE_EXCLUSIVE, then LOCKTAG_TUPLE +
	 * XactLockTableWait in the slow path).  The measured hot-row throughput
	 * decline (peak ~c8 then fall) is dominated by "Lock : tuple" heavyweight
	 * waits + per-update UNDO/sLog/WAL work under that lock -- the same
	 * serialization heap incurs on a hot row -- NOT by spinning. Switching
	 * the fast path to BUFFER_LOCK_EXCLUSIVE would not change this (both
	 * modes serialize one writer per buffer) and would regress the scattered
	 * case by blocking concurrent plain-SHARE readers.
	 *
	 * Hot-row throughput is limited by per-update UNDO/sLog/WAL work under
	 * the serialized lock, not by the writer-lock mode.
	 *
	 * Eligibility requirements: - New tuple must be the same on-disk size as
	 * the old tuple - Old tuple must not have overflow data - Old tuple must
	 * not be deleted, locked, or uncommitted - No speculative insertion -
	 * Must be a simple UPDATE (not HOT-chain following) - Snapshot visibility
	 * must be trivially true (committed tuple) - Relation must need WAL (for
	 * crash safety)
	 *
	 * If any condition fails, we fall through to the exclusive-lock path.
	 *
	 * A logically-logged relation is excluded: the CAS record only WAL-logs a
	 * byte-diff (offset+length+bytes), not a full tuple, so a logical decoder
	 * cannot reconstruct the new row from it.  Such relations take the
	 * regular exclusive-lock UPDATE path below, whose xl_flux_update record
	 * carries the full old and new heap-format images that DecodeFluxUpdate()
	 * reads. ---------------------------------------------------------------
	 */
	if (key_changed || RelationIsLogicallyLogged(relation) ||
		crosscheck != InvalidSnapshot)
		goto flux_update_slow_path;

	{
		FluxTuple	cas_new_tuple;
		Size		cas_new_size;

		/* Form the new tuple speculatively (no overflow handling) */
		slot_getallattrs(slot);
		cas_new_tuple = FluxFormTuple(RelationGetDescr(relation),
									  slot->tts_values,
									  slot->tts_isnull,
									  NULL, /* no overflow */
									  NULL);

		cas_new_size = cas_new_tuple->t_len;

		Assert(!key_changed);

		/* Attempt the CAS fast path */
		buffer = ReadBuffer(relation, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE_EXCLUSIVE);
		page = BufferGetPage(buffer);

		if (offnum >= FirstOffsetNumber &&
			offnum <= PageGetMaxOffsetNumber(page))
		{
			itemid = PageGetItemId(page, offnum);

			if (ItemIdIsNormal(itemid) &&
				cas_new_size <= ItemIdGetLength(itemid))
			{
				Size		cas_target_size;

				old_tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

				/*
				 * The version-chain head lives in the fixed header field
				 * t_verptr, so both the on-page tuple and the freshly formed
				 * new tuple carry it inside their header with no trailing
				 * growth.  Same-column-width updates are therefore naturally
				 * the same length, so this stays on the CAS fast path with no
				 * first-time growth dispatch.
				 */
				cas_target_size = cas_new_size;

				/*
				 * Cheap in-page eligibility gate: committed, not deleted, not
				 * locked, no overflow, same size for direct memcpy.  This is
				 * a pure buffer-domain check (no sLog probe).
				 *
				 * We deliberately do NOT probe the sLog for write-write
				 * conflicts here.  The flag gate is not a reliable conflict
				 * signal -- a committed in-place UPDATE rewinds t_commit_ts
				 * and clears FLUX_TUPLE_UNCOMMITTED, and a reader/third
				 * writer may clear a still-live flag as "stale" -- so any
				 * cheaper pre-check here would have to be repeated
				 * authoritatively below anyway.  The committed-update and
				 * in-progress-writer probes therefore run once, below; no WAL
				 * or page change happens until they pass, and a failed probe
				 * just falls to the exclusive path.
				 */
				if (!(old_tuple_hdr->t_flags & (FLUX_TUPLE_DELETED |
												FLUX_TUPLE_LOCKED |
												FLUX_TUPLE_UNCOMMITTED |
												FLUX_TUPLE_HAS_OVERFLOW |
												FLUX_TUPLE_SPECULATIVE)) &&
					!((old_tuple_hdr->t_infomask |
					   cas_new_tuple->t_data->t_infomask) &
					  FLUX_INFOMASK_HASEXTERNAL) && /* TOAST: slow path */
					cas_new_size <= TOAST_TUPLE_THRESHOLD &&	/* wide: slow path
																 * toasts it */
					cas_target_size == ItemIdGetLength(itemid) &&

				/*
				 * The CAS record cannot carry a visibility map clear: let the
				 * exclusive path update a page that is or may be marked
				 * all-visible.
				 *
				 * ponytail: so the first UPDATE of a page after VACUUM marked
				 * it takes the exclusive path; give the CAS record the clear
				 * (as the other change records have it) if that shows in a
				 * profile.
				 */
					!FluxVMNeedsClear(relation, buffer))
				{
					/*
					 * The buffer lock is what makes us the tuple's only
					 * writer from here to the page write: SHARE_EXCLUSIVE
					 * conflicts with itself and with EXCLUSIVE.
					 */
					Assert(BufferIsLockedByMeInMode(buffer, BUFFER_LOCK_SHARE_EXCLUSIVE) ||
						   BufferIsLockedByMeInMode(buffer, BUFFER_LOCK_EXCLUSIVE));

					{
						/*
						 * A competing writer may have completed its own
						 * in-place overwrite (it stamps
						 * FLUX_TUPLE_UNCOMMITTED into the new page image), or
						 * committed and left a conflict marker, before we got
						 * the buffer lock.  Acting on a stale decision would
						 * silently clobber that update (lost update).
						 * Re-validate the flags and run the write-write
						 * probes under the lock; if the tuple is
						 * disqualified, fall through to the exclusive path,
						 * which blocks on the in-progress writer or returns
						 * TM_Updated for EPQ.
						 */
						bool		cas_revalidated;

						cas_revalidated =
							!(old_tuple_hdr->t_flags & (FLUX_TUPLE_DELETED |
														FLUX_TUPLE_LOCKED |
														FLUX_TUPLE_UNCOMMITTED |
														FLUX_TUPLE_HAS_OVERFLOW |
														FLUX_TUPLE_SPECULATIVE)) &&
							cas_target_size == ItemIdGetLength(itemid) &&
							(!TransactionIdIsValid(decided_xmin) ||
							 flux_same_version(old_tuple_hdr, decided_verptr,
											   decided_xmin));
						if (cas_revalidated &&
							snapshot != NULL && IsMVCCSnapshot(snapshot))
						{
							UndoRecPtr	head_verptr;
							TransactionId head_xid;
							bool		head_inprogress = false;

							if (FluxTupleHasCommittedUpdateAfter(relation,
																 old_tuple_hdr,
																 ItemIdGetLength(itemid),
																 snapshot,
																 GetCurrentTransactionIdIfAny(),
																 &head_verptr,
																 &head_xid,
																 &head_inprogress) &&
								!FluxEpqReconcileMatches(snapshot,
														 RelationGetRelid(relation),
														 otid))
								cas_revalidated = false;

							/*
							 * Commit-window conflict: the head committer is
							 * between PRE_COMMIT (marker + UNCOMMITTED
							 * cleared) and CLOG commit.  Its sLog marker is
							 * gone, so the dirty-xid probe below cannot see
							 * it.  Abandon the CAS fast path and fall to the
							 * slow path, which waits on the in-flight writer;
							 * otherwise the just-committed update is lost.
							 */
							else if (head_inprogress)
								cas_revalidated = false;

							/*
							 * The row's writer per its header, which names it
							 * whether or not the UNCOMMITTED hint is still
							 * set (it is cleared at PRE_COMMIT).  Only a
							 * writer invisible to our snapshot can still be
							 * in progress, which keeps the proc-array probe
							 * off the common path.
							 */
							else if (!(old_tuple_hdr->t_flags & FLUX_TUPLE_XMIN_COMMITTED) &&
									 XidInMVCCSnapshot(FluxTupleGetXmin(old_tuple_hdr),
													   snapshot) &&
									 TransactionIdIsValid(flux_header_dirty_xid(old_tuple_hdr,
																				NULL)))
								cas_revalidated = false;
						}

						/*
						 * Re-check for a concurrent in-progress writer (see
						 * above)
						 */
						if (cas_revalidated)
						{
							bool		reval_is_insert = false;
							TransactionId reval_dirty_xid =
								flux_update_conflict_xid(relation, otid, old_tuple_hdr,
														 key_changed, &reval_is_insert);

							if (TransactionIdIsValid(reval_dirty_xid) &&
								!reval_is_insert)
								cas_revalidated = false;
						}

						if (cas_revalidated)
						{
							/*
							 * Fairness gate: funnel the CAS writer through
							 * LOCKTAG_TUPLE so lockers (flux_tuple_lock) and
							 * slow-path writers already queued on the same
							 * tag get FIFO service.  Without this, a stream
							 * of CAS writers can lap a locker sleeping in
							 * XactLockTableWait under FluxLockTuple, starving
							 * it past statement_timeout.  Must be dontWait:
							 * the buffer lock is held here, and blocking on a
							 * heavyweight lock under it could deadlock.  If
							 * not granted, drop the buffer lock and fall
							 * through to the exclusive path, which takes
							 * LOCKTAG_TUPLE with wait=true and queues FIFO
							 * behind the waiter that beat us here.
							 */
							bool		cas_have_tuple_lock = false;

							if (!FluxLockTuple(relation, otid,
											   update_lockmode,
											   false, &cas_have_tuple_lock))
							{
								/*
								 * CANDIDATE A (surgical fell-through): a
								 * locker or updater is queued on
								 * LOCKTAG_TUPLE for this tid.  Drop the page
								 * lock, then BLOCK on LOCKTAG_TUPLE so this
								 * writer queues FIFO behind the existing
								 * waiter.  Only after we acquire the tag (the
								 * locker ahead of us has been served) do we
								 * re-enter the update path via the slow
								 * route, which re-reads the page and
								 * re-classifies.  We must release the buffer
								 * content lock BEFORE the blocking acquire:
								 * holding it across a heavyweight sleep would
								 * deadlock.  Set the OUTER have_tuple_lock so
								 * the slow path's dirty_xid-gated LockTuple
								 * sites (guarded by !have_tuple_lock) do not
								 * double-acquire and FLUX_RELEASE_TUPLOCK
								 * frees it on every exit.
								 */
								LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
								ReleaseBuffer(buffer);
								FluxFreeTuple(cas_new_tuple);
								FluxLockTupleToWait(relation, otid,
													update_lockmode, &have_tuple_lock);
								goto flux_update_slow_path;
							}
							{
								/*
								 * We own this tuple exclusively under
								 * share-exclusive page lock.  No other writer
								 * can modify it until we release that.
								 *
								 * Compute timestamps outside critical section
								 * to avoid memory allocation issues.
								 */
								uint64		cas_current_ts;
								uint16		cas_data_offset;
								uint16		cas_data_len;
								char	   *cas_old_bytes;
								char	   *cas_new_bytes;
								Size		cas_tuple_len;

								(void) FluxGetTransactionTimestamp();
								cas_current_ts = (uint64) FluxGetDmlTimestamp();

								/* Ensure we have an XID for WAL flush */
								(void) GetCurrentTransactionId();

								/*
								 * Heap-shaped: the in-place image is a FRESH
								 * version. Stamp t_xmax = Invalid (this is
								 * the newest, live version) and t_xmin = the
								 * updater below.  The pre-update image is
								 * preserved in the UNDO log (the CAS path
								 * writes an undo record), so an older
								 * snapshot that cannot see the updater's xmin
								 * reads the before-image back via
								 * FluxReconstructVisible- Version.
								 *
								 * Torn-read note: the CAS path holds only
								 * BUFFER_LOCK_SHARE_EXCLUSIVE, so a
								 * concurrent SHARE reader can observe this
								 * image mid-overwrite.  It is marked
								 * UNCOMMITTED and its xmin is the still-
								 * in-progress updater, so
								 * FluxTupleSatisfiesMVCC hides it
								 * (XidInMVCCSnapshot(updater) == true) and
								 * the reader reconstructs the prior version
								 * -- the row never blinks out.
								 */
								cas_new_tuple->t_data->t_commit_ts = 0;
								cas_new_tuple->t_data->t_flags |= FLUX_TUPLE_UPDATED;

								/*
								 * Record whether this update changes an
								 * indexed column, so a concurrent KeyShare
								 * (FK) probe knows whether it may proceed
								 * without waiting.  See
								 * FLUX_TUPLE_KEYS_UPDATED in flux.h.
								 */
								if (key_changed)
									cas_new_tuple->t_data->t_flags |=
										FLUX_TUPLE_KEYS_UPDATED;

								/*
								 * Mark the in-place image UNCOMMITTED,
								 * mirroring the regular update path.  A
								 * second writer that hits this row sees
								 * UNCOMMITTED in the CAS eligibility gate,
								 * bails to the regular path, and blocks on
								 * the in-progress writer via
								 * XactLockTableWait -- without this flag the
								 * second CAS would silently overwrite an
								 * uncommitted update (lost update / no
								 * write-write blocking).  PRE_COMMIT clears
								 * the flag and stamps the real commit
								 * timestamp; readers self-heal a stale flag
								 * via the visibility path after a crash.
								 */
								cas_new_tuple->t_data->t_flags |= FLUX_TUPLE_UNCOMMITTED;

								/*
								 * Clear inherited CLOG hint bits: this image
								 * copied the old tuple's flags, but we are
								 * stamping a NEW t_xmin (the updater).  A
								 * stale XMIN_COMMITTED from the old inserter
								 * would wrongly assert the new xmin is
								 * committed.
								 */
								cas_new_tuple->t_data->t_flags &=
									~(FLUX_TUPLE_XMIN_COMMITTED | FLUX_TUPLE_XMAX_COMMITTED);
								cas_new_tuple->t_data->t_xmin = GetCurrentTransactionId();	/* subxid: heap-shaped,
																							 * so savepoint rollback
																							 * marks it aborted in
																							 * CLOG */
								ItemPointerSet(&cas_new_tuple->t_data->t_ctid, blkno, offnum);

								/*
								 * t_cmin must agree with the t_xmin stamped
								 * just above (ours), so use OUR cid, not the
								 * original inserter's.  See the longer note
								 * on the regular in-place path.
								 */
								cas_new_tuple->t_data->t_cmin = cid;

								/*
								 * Build the new-image buffer of
								 * cas_target_size (== cas_new_size; the
								 * version pointer is a header field, not a
								 * trailer).  We reserve the per-rel UNDO
								 * record first so its UndoRecPtr can be
								 * stamped into the new image BEFORE both the
								 * diff scan (so the diff includes the
								 * version-pointer change) and the page
								 * memcpy.
								 *
								 * PrepareUndoInsert may ereport -- neither is
								 * safe inside the critical section.  Lock
								 * order: data buffer then UNDO buffer.
								 */
								{
									uint32		cas_old_len = ItemIdGetLength(itemid);
									char	   *cas_old_copy = palloc(cas_old_len);
									char	   *cas_full_image;
									UndoRecPtr	cas_undo_ptr = InvalidUndoRecPtr;
									bool		cas_undo_prepared = false;
									TransactionId cas_freeze_conflict;

									memcpy(cas_old_copy, old_tuple_hdr, cas_old_len);
									cas_freeze_conflict = flux_prepare_before_image(relation, (FluxTupleHeader *) cas_old_copy);

									/*
									 * Assemble the on-page image: header+data
									 * from the freshly formed new tuple plus
									 * the trailing version-pointer.  Stamp a
									 * placeholder verptr for now; the real
									 * reserved cas_undo_ptr is stamped after
									 * the delta-vs-full decision below.  The
									 * diff region and the delta both force
									 * t_verptr into their changed span, so
									 * re-stamping it never moves those
									 * boundaries.  We prepare the UNDO record
									 * exactly once, choosing delta or full up
									 * front, so there is no cancel +
									 * re-prepare (that would double-advance
									 * the undo log's insert pointer).
									 */
									cas_full_image = palloc(cas_target_size);
									memcpy(cas_full_image, cas_new_tuple->t_data, cas_new_size);
									{
										FluxTupleHeader *cas_full_hdr =
											(FluxTupleHeader *) cas_full_image;

										cas_full_hdr->t_flags |= FLUX_TUPLE_HAS_VERSION_PTR;
										FluxTupleSetVersionPtr(cas_full_hdr,
															   InvalidUndoRecPtr);
									}

									/*
									 * Compute the diff region for WAL
									 * logging. We log only the bytes that
									 * actually changed within the unified
									 * image.  Read the old side from
									 * cas_old_copy (already memcpy'd from the
									 * page, identical bytes) rather than the
									 * live locked page: the scan is pure CPU
									 * work with no I/O, and reading a
									 * palloc'd copy instead of old_tuple_hdr
									 * means this loop no longer needs the
									 * data page to stay locked --
									 * cas_target_size == cas_old_len is
									 * guaranteed by the eligibility gate
									 * above (same-size CAS only), so
									 * cas_old_copy has exactly cas_tuple_len
									 * bytes available.
									 */
									cas_tuple_len = cas_target_size;
									cas_old_bytes = cas_old_copy;
									cas_new_bytes = cas_full_image;

									/* Find first differing byte */
									cas_data_offset = 0;
									while (cas_data_offset < cas_tuple_len &&
										   cas_old_bytes[cas_data_offset] == cas_new_bytes[cas_data_offset])
										cas_data_offset++;

									if (cas_data_offset < cas_tuple_len)
									{
										uint16		cas_end = (uint16) cas_tuple_len;

										/* Find last differing byte */
										while (cas_end > cas_data_offset &&
											   cas_old_bytes[cas_end - 1] == cas_new_bytes[cas_end - 1])
											cas_end--;

										cas_data_len = cas_end - cas_data_offset;
									}
									else
									{
										/*
										 * No actual data change -- release
										 * and return OK.  No UNDO record was
										 * prepared yet (the single prepare
										 * happens after this branch), so
										 * nothing to cancel.
										 */
										pfree(cas_full_image);
										pfree(cas_old_copy);
										LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
										ReleaseBuffer(buffer);
										FluxFreeTuple(cas_new_tuple);

										/* Set output TID */
										ItemPointerSet(&slot->tts_tid, blkno, offnum);
										slot->tts_tableOid = RelationGetRelid(relation);
										if (update_indexes)
											*update_indexes = TU_None;
										flux_count_inplace_update(relation, key_changed);
										FluxUnlockTuple(relation, otid,
														update_lockmode);
										return TM_Ok;
									}

									/*
									 * The real version pointer is stamped
									 * into t_verptr only below, after this
									 * scan.  The placeholder equals the old
									 * t_verptr on a row's first in-place
									 * update (both invalid), so the scan can
									 * leave t_verptr out of the logged
									 * region; redo would then drop the
									 * version-chain head.  Always log it.
									 */
									{
										uint16		vp_start = offsetof(FluxTupleHeader, t_verptr);
										uint16		vp_end = vp_start + sizeof(UndoRecPtr);
										uint16		cas_end = cas_data_offset + cas_data_len;

										cas_data_offset = Min(cas_data_offset, vp_start);
										cas_data_len = Max(cas_end, vp_end) - cas_data_offset;
									}

									/* SSI conflict check */
									CheckForSerializableConflictIn(relation, otid,
																   BufferGetBlockNumber(buffer));

									/*
									 * Choose delta vs full at emit time and
									 * prepare the UNDO record exactly once.
									 * The diff region (cas_data_offset/len)
									 * covers t_verptr (forced above), and
									 * FluxUndoDeltaCompute forces it into the
									 * delta too.  If a delta record is
									 * strictly smaller than the full
									 * before-image, prepare
									 * FLUX_UNDO_UPDATE_DELTA; else the full
									 * FLUX_UNDO_UPDATE with the OLD image.
									 * Then stamp the returned UndoRecPtr
									 * (this update version-chain head) into
									 * the image at offset 8 -- the only
									 * new-image mutation after the diff scan,
									 * inside the diff/delta region under any
									 * verptr.  cas_new_bytes aliases
									 * cas_full_image, so the page memcpy and
									 * the WAL redo byte-diff pick up the
									 * final verptr.  Preparing once (no
									 * cancel + re-prepare) advances the undo
									 * log insert pointer exactly once.
									 */
									{
										ItemPointerData cas_tid;
										char	   *cas_delta_buf =
											(char *) palloc(SizeOfFluxUndoDelta + cas_old_len);
										Size		cas_delta_total =
											FluxUndoDeltaCompute(cas_old_bytes, cas_old_len,
																 cas_new_bytes, (uint32) cas_tuple_len,
																 offsetof(FluxTupleHeader, t_verptr),
																 cas_delta_buf,
																 SizeOfFluxUndoDelta + cas_old_len);

										ItemPointerSet(&cas_tid, blkno, offnum);

										if (cas_delta_total > 0)
											cas_undo_ptr =
												FluxPbuPrepareInsert(FLUX_UNDO_UPDATE_DELTA,
																	 relation,
																	 &cas_tid,
																	 cas_delta_buf,
																	 (uint32) cas_delta_total,
																	 relation->rd_rel->relpersistence, false, cas_freeze_conflict,
																	 cid);
										else
											cas_undo_ptr =
												FluxPbuPrepareInsert(FLUX_UNDO_UPDATE,
																	 relation,
																	 &cas_tid,
																	 cas_old_copy, cas_old_len,
																	 relation->rd_rel->relpersistence, false, cas_freeze_conflict,
																	 cid);
										cas_undo_prepared = true;
										pfree(cas_delta_buf);

										FluxTupleSetVersionPtr((FluxTupleHeader *) cas_full_image, cas_undo_ptr);
									}

									/*
									 * FOLD variant: the per-backend UNDO
									 * record was already PREPARED up front
									 * (its old-image payload set, its
									 * UndoRecPtr stamped into the new image).
									 * Inside the critical section we write it
									 * into its pinned undo buffers
									 * (FluxPbuInsertUndo) and then fold those
									 * undo pages onto the ONE combined WAL
									 * record (block 0 = data page redo
									 * byte-diff, block 1+ = undo page
									 * full-page images), so the undo bytes
									 * ride to disk on the same record as the
									 * forward mutation (crash-safe) in one
									 * XLogInsert.
									 *
									 * The tid the record references was set
									 * at prepare; set the output slot TID
									 * here too.
									 */
									ItemPointerSet(&slot->tts_tid, blkno, offnum);
									slot->tts_tableOid = RelationGetRelid(relation);

									/* Critical section: modify page + WAL */
									START_CRIT_SECTION();

									memcpy(old_tuple_hdr, cas_full_image, cas_target_size);

									/*
									 * Update page-level commit timestamp
									 * atomically
									 */
									{
										FluxPageOpaque cas_opaque = FluxPageGetOpaque(page);
										uint64		cas_old_ts_flags;
										uint64		cas_new_ts_flags;
										uint64		cur_ts;

										do
										{
											cas_old_ts_flags = cas_opaque->pd_commit_ts_and_flags;
											cur_ts = cas_old_ts_flags & FLUX_PAGE_TS_MASK;

											if (cas_current_ts <= cur_ts)
												break;
											cas_new_ts_flags = (cas_old_ts_flags & FLUX_PAGE_FLAG_MASK) |
												(cas_current_ts & FLUX_PAGE_TS_MASK);
										} while (!pg_atomic_compare_exchange_u64(
																				 (pg_atomic_uint64 *) &cas_opaque->pd_commit_ts_and_flags,
																				 &cas_old_ts_flags, cas_new_ts_flags));
									}

									MarkBufferDirtyShared(buffer);

									/*
									 * Write the prepared per-backend UNDO
									 * record into its pinned undo buffers
									 * before the combined WAL record folds
									 * them in.
									 */
									if (cas_undo_prepared)
										FluxPbuInsertUndo();

									/*
									 * WAL log.  When a per-backend UNDO
									 * record was prepared, emit the combined
									 * fold record carrying both the main-fork
									 * redo diff (block 0) and the undo page
									 * full-page images (block 1+), then stamp
									 * the record LSN onto the undo pages.
									 * Otherwise (unlogged/temp: no WAL) emit
									 * the plain redo record.
									 */
									if (cas_undo_prepared && RelationNeedsWAL(relation))
									{
										XLogRecPtr	cas_recptr;

										cas_recptr =
											FluxXLogCasUpdateUndo(relation, buffer, offnum,
																  cas_data_offset, cas_data_len,
																  cas_new_bytes + cas_data_offset,
																  0);	/* new version: t_xmax =
																		 * Invalid */
										FluxPbuFinishUndo(cas_recptr);
									}
									else if (RelationNeedsWAL(relation))
									{
										FluxXLogCasUpdate(relation, buffer, offnum,
														  cas_data_offset, cas_data_len,
														  cas_new_bytes + cas_data_offset,
														  0);	/* new version: t_xmax =
																 * Invalid */
									}
									else if (cas_undo_prepared)
									{
										/* Unlogged/temp: no WAL LSN to stamp. */
										FluxPbuFinishUndo(InvalidXLogRecPtr);
									}

									END_CRIT_SECTION();

									/*
									 * The per-backend UNDO record was written
									 * and folded into the combined WAL record
									 * above; FluxPbuFinishUndo already
									 * released its undo buffers, and
									 * InsertPreparedUndo chained it into the
									 * xact for rollback discovery.  Nothing
									 * further to do here.
									 */

									/*
									 * sLog registration BEFORE buffer
									 * release. Eliminates the race window
									 * where another backend reads the
									 * modified tuple but finds no sLog entry
									 * (causing visibility failures at high
									 * concurrency).  Safe: seqlock reads are
									 * wait-free, no deadlock with buffer
									 * lock.
									 */
									FluxEnsureSLogCallbacks();

									/*
									 * BUG 5 fix: bounded local-only tracking
									 * (see flux_tuple_delete).
									 */

									/*
									 * BUG 5: no per-tuple tracking for UPDATE
									 * (see flux_tuple_delete).
									 */

									/*
									 * Record our own update's command id
									 * (cmax).
									 */
									FluxLocalCidSet(RelationGetRelid(relation),
													&slot->tts_tid, cid);

									LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

									/*
									 * No index maintenance for the CAS
									 * in-place path.  A CAS update is
									 * same-size and only reaches here when no
									 * indexed column changed (key changes are
									 * routed out of place at the top of
									 * flux_tuple_update, TU_All).  The TID is
									 * unchanged, so every (key, TID) entry
									 * still points at the live tuple;
									 * *update_indexes is set TU_None below.
									 */

									pfree(cas_old_copy);
									pfree(cas_full_image);


									ReleaseBuffer(buffer);

									/* Track in-place update */
									flux_stat_in_place_updates++;

									FluxLocalDirtyPageMark(relation, blkno);

									FluxFreeTuple(cas_new_tuple);

									/*
									 * The per-backend engine's discard worker
									 * reclaims stale undo; there is nothing
									 * to discard here.
									 */
									flux_count_inplace_update(relation, key_changed);
									FluxUnlockTuple(relation, otid,
													update_lockmode);
									return TM_Ok;
								}	/* end block (tuple-lock granted) */
							}	/* end else (revalidated) */
						}
					}
				}
				/* Not eligible for CAS fast path */
			}
			/* ItemId not normal or new tuple too large */
		}
		/* Offset out of range */

		/* Release shared lock, fall through to exclusive path */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		FluxFreeTuple(cas_new_tuple);
	}

	/*
	 * Lock the buffer exclusively.  The exclusive buffer lock prevents
	 * concurrent modification while we hold it, but it is dropped while we
	 * XactLockTableWait on a conflicting in-progress writer (the wait sites
	 * below).  To stop two backends that each see the other's in-progress
	 * write from mutually waiting and deadlocking, an updater first takes a
	 * heavyweight tuple lock (have_tuple_lock) that serializes them into a
	 * FIFO queue, held continuously through the recheck/update and released
	 * on every exit via FLUX_RELEASE_TUPLOCK().  This matches heap_update.
	 *
	 * CANDIDATE A: a CAS writer that found a locker queued on the tag jumps
	 * here (flux_update_slow_path) already holding have_tuple_lock, so do NOT
	 * reset it -- clobbering it would drop the lock we just queued for. The
	 * buffer was released before the goto, so the re-read below re-pins it;
	 * the label must precede ReadBuffer.
	 */
flux_update_slow_path:
	/* Read the page containing the old tuple */
	buffer = ReadBuffer(relation, blkno);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);

	/*
	 * Helping: revert an aborted transaction's not-yet-reverted versions of
	 * this row before working on it (see FluxRevertAbortedVersions).
	 */
	if (flux_help_revert(relation, buffer, offnum))
		goto flux_update_slow_path;

	/* Validate offset number */
	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
	{
		UnlockReleaseBuffer(buffer);
		FLUX_RELEASE_TUPLOCK();
		return TM_Invisible;
	}

	/* Get the old tuple */
	itemid = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(itemid))
	{
		UnlockReleaseBuffer(buffer);
		FLUX_RELEASE_TUPLOCK();
		return TM_Invisible;
	}

	old_tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

	/*
	 * Our own transaction already modified this row in this or a later
	 * command (typically a BEFORE trigger, or a volatile function, that
	 * updated the very row we are updating): report TM_SelfModified.  The
	 * executor either ignores the duplicate action (same command id) or
	 * raises "tuple to be updated was already modified by an operation
	 * triggered by the current command".  Silently applying our second write
	 * would discard the trigger's change.
	 */
	{
		CommandId	self_cmax;

		if (FluxCheckSelfModified(relation, otid, old_tuple_hdr, cid,
								  &self_cmax))
		{
			if (tmfd)
			{
				tmfd->ctid = *otid;
				tmfd->xmax = (old_tuple_hdr->t_flags & FLUX_TUPLE_DELETED) ?
					FluxTupleGetXmax(old_tuple_hdr) : old_tuple_hdr->t_xmin;
				tmfd->cmax = self_cmax;
				tmfd->traversed = false;
			}
			UnlockReleaseBuffer(buffer);
			FLUX_RELEASE_TUPLOCK();
			return TM_SelfModified;
		}
	}

	/* Check if old tuple has overflow chains to clean up later */
	old_has_overflow = (old_tuple_hdr->t_flags & FLUX_TUPLE_HAS_OVERFLOW) != 0;

	/*
	 * Check if tuple is already deleted.  As in the delete path, a DELETED
	 * flag may reflect an in-progress delete by a concurrent transaction
	 * (which also sets UNCOMMITTED).  If so, wait for that transaction and
	 * retry rather than reporting TM_Deleted immediately -- this matches
	 * heap, where an UPDATE blocks behind a concurrent in-progress DELETE.
	 */
	if (old_tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
	{
		TransactionId del_xid = InvalidTransactionId;
		bool		del_is_insert = false;

		if (old_tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED)
		{
			/*
			 * The concurrent deleter's XID is on the page (t_xmax).  Read it
			 * from the header: per-page conflict tracking no longer writes a
			 * shared per-tuple sLog DELETE entry, and the header stays
			 * authoritative through the deleter's PRE_COMMIT window.  An
			 * UPDATE must block behind a concurrent in-progress DELETE of the
			 * same row (matches heap), which requires seeing the deleter
			 * here.
			 */
			del_xid = flux_header_dirty_xid(old_tuple_hdr, &del_is_insert);
		}

		if (wait && TransactionIdIsValid(del_xid) &&
			!TransactionIdIsCurrentTransactionId(del_xid) &&
			!del_is_insert)
		{
			TransactionId wait_xid = del_xid;

			/*
			 * Acquire a heavyweight tuple lock before sleeping on the
			 * conflicting xid.  This serializes competing updaters of the
			 * same tuple into a FIFO queue; without it, two backends that
			 * each see the other's in-progress write would mutually
			 * XactLockTableWait and deadlock.  Matches heap_update.
			 */
			UnlockReleaseBuffer(buffer);
			FluxLockTupleToWait(relation, otid,
								update_lockmode, &have_tuple_lock);
			XactLockTableWait(wait_xid, relation, otid, XLTW_Update);

			/*
			 * Start over: a committed delete is reported then, an aborted one
			 * has had its before-image restored, and the row may have another
			 * deleter by now.
			 */
			goto flux_update_slow_path;
		}
		else
		{
			if (tmfd)
			{
				tmfd->ctid = *otid;
				tmfd->xmax = GetCurrentTransactionId();
				tmfd->cmax = InvalidCommandId;
				tmfd->traversed = false;
			}
			UnlockReleaseBuffer(buffer);
			FLUX_RELEASE_TUPLOCK();
			return TM_Deleted;
		}
	}

	/*
	 * Handle LOCKED flag: if this tuple is locked by the current transaction
	 * (FOR SHARE/FOR KEY SHARE/FOR UPDATE), the lock is compatible with
	 * UPDATE (self-lock).  Clear the LOCKED flag since we're about to modify
	 * the tuple.  The row lock itself stays in the tuple sLog until commit.
	 */
	if (old_tuple_hdr->t_flags & FLUX_TUPLE_LOCKED)
	{
		LockTupleMode own_mode;

		if (SLogTupleLockHeldByMe(RelationGetRelid(relation),
								  relation->rd_locator.relNumber, otid,
								  &own_mode))
		{
			/* Our own lock - clear flag and proceed with update */
			old_tuple_hdr->t_flags &= ~FLUX_TUPLE_LOCKED;

			/*
			 * Only an UPDATE-strength self-lock earns the conflict-gate
			 * exemption below.  The arbiter/EPQ re-entries this exists for
			 * take LockTupleExclusive / LockTupleNoKeyExclusive and have
			 * genuinely re-validated the row under that lock.  A KeyShare or
			 * Share marker has not: our own FK check on this row
			 * (ri_LockPKTuple) leaves a KeyShare marker behind, and treating
			 * that as "already validated" disabled the committed-update and
			 * committed-after-snapshot gates for the rest of the statement --
			 * so an UPDATE in a transaction that had FK-checked the same row
			 * silently overwrote a concurrent committed update ('' -> 'A' ->
			 * 'B' yielded 'B', not 'AB').  The weaker marker still clears
			 * FLUX_TUPLE_LOCKED above (it is ours, so it must not block us);
			 * it just does not suppress the lost-update gates.
			 */
			if (own_mode != LockTupleNoKeyExclusive &&
				own_mode != LockTupleExclusive)
				goto self_lock_done;

			/*
			 * LOST-UPDATE / ON CONFLICT fix (bug 4): we already hold our own
			 * LOCK marker on this exact tuple.  The ON CONFLICT DO UPDATE
			 * arbiter took that lock (ExecOnConflictLockRow ->
			 * table_tuple_lock), re-checked the row under it, and only then
			 * routed here to apply the update.  Heap's own-lock short-circuit
			 * (heap_update: if we hold the lock, the committed-after-snapshot
			 * xmax is not a conflict) then lets the update proceed.  FLUX
			 * must do the same: the committed-update /
			 * committed-after-snapshot gates below would otherwise return
			 * TM_Updated, and on the ON CONFLICT path the executor has no
			 * ri_oldTupleSlot to EPQ-refetch into ->
			 * flux_tuple_fetch_row_version dereferences a NULL slot and
			 * crashes (and even absent the crash it loops forever bouncing to
			 * EPQ).  Record locked_by_me so those gates are exempted; the
			 * invariant that makes this safe is bug 5's: the LOCK marker is
			 * registered under the buffer lock, so "we hold the lock" is
			 * authoritative here under this same buffer lock.
			 */
			locked_by_me = true;
		}

self_lock_done:

		/*
		 * If it's another transaction's lock, the conflict probe below
		 * (flux_update_conflict_xid) waits for it.
		 */
		;
	}

	/*
	 * Check tuple visibility against snapshot and handle concurrent
	 * modifications.  Unlike a simple scan visibility check, UPDATE must
	 * distinguish between: - Truly invisible (another txn's uncommitted
	 * insert) -> TM_Invisible - Concurrent update committed after our
	 * snapshot    -> TM_Updated - In-progress modification by another txn ->
	 * wait, retry
	 */
	if (snapshot)
	{
		bool		visible;

		visible = FluxTupleVisibleToSnapshotDual(old_tuple_hdr, snapshot,
												 RelationGetRelid(relation),
												 buffer, otid);

		if (!visible)
		{
			TransactionId dirty_xid;
			bool		is_insert_entry;

			/*
			 * The in-progress writer (header) or conflicting locker (sLog) to
			 * wait for.  A KeyShare locker is compatible only when no key
			 * changes.  For a non-key update, waiting on its xid while it is
			 * queued behind us would form the mutual wait cycle heap avoids
			 * via HEAP_XMAX_IS_LOCKED_ONLY.
			 */
			dirty_xid = flux_update_conflict_xid(relation, otid,
												 old_tuple_hdr, key_changed,
												 &is_insert_entry);

			/* Check if tuple was deleted by another transaction */
			if (old_tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
			{
				if (tmfd)
				{
					tmfd->ctid = *otid;
					tmfd->xmax = TransactionIdIsValid(dirty_xid) ?
						dirty_xid : GetCurrentTransactionId();
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				FLUX_RELEASE_TUPLOCK();
				return TM_Deleted;
			}

			/*
			 * Buffer lock was never released (wait-free sLog read), so the
			 * tuple cannot have changed.  Proceed with dirty_xid.
			 */
			{
				if (TransactionIdIsValid(dirty_xid) && is_insert_entry)
				{
					/*
					 * Another txn's in-progress INSERT.  The tuple truly
					 * doesn't exist in our snapshot.
					 */
					if (tmfd)
					{
						tmfd->ctid = *otid;
						tmfd->xmax = dirty_xid;
						tmfd->cmax = InvalidCommandId;
						tmfd->traversed = false;
					}
					UnlockReleaseBuffer(buffer);
					FLUX_RELEASE_TUPLOCK();
					return TM_Invisible;
				}

				if (TransactionIdIsValid(dirty_xid) && !is_insert_entry &&
					!TransactionIdIsCurrentTransactionId(dirty_xid))
				{
					/*
					 * Another txn's in-progress UPDATE/DELETE.  Wait for it
					 * to finish and then retry (the tuple may be gone or
					 * changed).
					 *
					 * The !TransactionIdIsCurrentTransactionId guard covers a
					 * row already modified in place by an earlier command of
					 * OUR OWN transaction (possibly at a shallower
					 * subtransaction level): its conflict xid resolves to us,
					 * and waiting on ourselves trips the self-wait assertion
					 * in XactLockTableWait and would self-deadlock.  Our own
					 * prior write is not a conflict; fall through to perform
					 * the update.
					 */
					if (wait)
					{
						TransactionId wait_xid = dirty_xid;

						/*
						 * Serialize competing updaters via a heavyweight
						 * tuple lock before sleeping on the conflicting xid,
						 * so two backends racing on the same tuple queue
						 * instead of deadlocking.  Matches heap_update.
						 */
						UnlockReleaseBuffer(buffer);
						FluxLockTupleToWait(relation, otid,
											update_lockmode, &have_tuple_lock);
						XactLockTableWait(wait_xid, relation,
										  otid, XLTW_Update);

						/* Re-read the page and re-check after waking */
						buffer = ReadBuffer(relation, blkno);
						LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
						page = BufferGetPage(buffer);

						if (offnum < FirstOffsetNumber ||
							offnum > PageGetMaxOffsetNumber(page))
						{
							UnlockReleaseBuffer(buffer);
							FLUX_RELEASE_TUPLOCK();
							return TM_Invisible;
						}
						itemid = PageGetItemId(page, offnum);
						if (!ItemIdIsNormal(itemid))
						{
							UnlockReleaseBuffer(buffer);
							FLUX_RELEASE_TUPLOCK();
							return TM_Invisible;
						}
						old_tuple_hdr = (FluxTupleHeader *)
							PageGetItem(page, itemid);

						/* If it got deleted while we waited, report that */
						if (old_tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
						{
							TM_Result	res = flux_deleted_after_wait(old_tuple_hdr);

							if (tmfd)
							{
								tmfd->ctid = *otid;
								tmfd->xmax = wait_xid;
								tmfd->cmax = InvalidCommandId;
								tmfd->traversed = false;
							}
							UnlockReleaseBuffer(buffer);
							FLUX_RELEASE_TUPLOCK();
							return res;
						}

						/*
						 * Re-check visibility.  The tuple was modified by the
						 * now-committed txn; its commit_ts is now later than
						 * our snapshot -> TM_Updated so the executor can EPQ.
						 */
						visible = FluxTupleVisibleToSnapshotDual(old_tuple_hdr, snapshot,
																 RelationGetRelid(relation),
																 buffer, otid);

						if (!visible)
						{
							/*
							 * Still not visible after the waited-on txn
							 * committed.  Before returning TM_Updated (which
							 * triggers another EPQ cycle), check whether we
							 * already hold a LOCK entry from a previous EPQ
							 * iteration.  If so, we've already re-evaluated
							 * the quals and should proceed with the update
							 * instead of looping forever.
							 *
							 * Without this check, the following livelock
							 * occurs with hot-row contention:
							 *
							 * 1. We return TM_Updated -> executor EPQ 2.
							 * table_tuple_lock takes a row lock 3. Retry ->
							 * another txn is in-progress -> wait 4. Waited
							 * txn commits -> still not visible 5. Return
							 * TM_Updated -> goto 2 (infinite)
							 *
							 * Each iteration leaks per-query memory in the
							 * executor, eventually causing OOM.
							 */
							TransactionId myxid_postw =
								GetCurrentTransactionIdIfAny();

							if (TransactionIdIsValid(myxid_postw))
							{
								if (SLogTupleLockHeldByMe(RelationGetRelid(relation),
														  relation->rd_locator.relNumber, otid, NULL))
								{
									/*
									 * Our LOCK entry from a prior EPQ cycle
									 * exists.  Fall through to perform the
									 * update.
									 */
								}
								else
								{
									if (tmfd)
									{
										tmfd->ctid = *otid;
										tmfd->xmax = wait_xid;
										tmfd->cmax = InvalidCommandId;
										tmfd->traversed = false;
									}
									UnlockReleaseBuffer(buffer);
									FLUX_RELEASE_TUPLOCK();
									return TM_Updated;
								}
							}
							else
							{
								if (tmfd)
								{
									tmfd->ctid = *otid;
									tmfd->xmax = wait_xid;
									tmfd->cmax = InvalidCommandId;
									tmfd->traversed = false;
								}
								UnlockReleaseBuffer(buffer);
								FLUX_RELEASE_TUPLOCK();
								return TM_Updated;
							}
						}
						/* Now visible; fall through to perform the update */
					}
					else
					{
						/* NOWAIT mode */
						if (tmfd)
						{
							tmfd->ctid = *otid;
							tmfd->xmax = dirty_xid;
							tmfd->cmax = InvalidCommandId;
							tmfd->traversed = false;
						}
						UnlockReleaseBuffer(buffer);
						FLUX_RELEASE_TUPLOCK();
						return TM_WouldBlock;
					}
				}
				else
				{
					/*
					 * No in-progress writer or locker in another transaction:
					 * the modification has already committed.
					 *
					 * Bounce to EvalPlanQual at most ONCE per (tid, cid). The
					 * previous termination check looked for our own per-tuple
					 * sLog entry, but per-page conflict tracking does not
					 * create one for UPDATE/DELETE, so it never terminated ->
					 * unbounded EPQ retry leaking into TopTransactionContext
					 * (the c>=16 PAYMENT OOM).  Use the reconcile marker
					 * instead: first encounter marks+bounces, a repeat for
					 * the same tuple this command proceeds.
					 *
					 * locked_by_me short-circuit (bug 4): if we already hold
					 * our own LOCK marker on this tuple (the ON CONFLICT
					 * arbiter path), a commit that landed after our snapshot
					 * is NOT a conflict -- we re-evaluated under the lock
					 * already, exactly as heap's own-lock short-circuit. Fall
					 * through and apply the update; returning TM_Updated here
					 * would EPQ-bounce into a NULL ri_oldTupleSlot on the ON
					 * CONFLICT path and crash.
					 */
					if (locked_by_me)
					{
						/* We hold the lock: proceed with the update. */
					}
					else if (FluxEpqReconcileMatches(snapshot,
													 RelationGetRelid(relation),
													 otid))
					{
						/* Already bounced to EPQ this command: proceed. */
					}
					else
					{
						FluxEpqReconcileMark(snapshot,
											 RelationGetRelid(relation),
											 otid, InvalidUndoRecPtr,
											 GetCurrentTransactionIdIfAny());
						if (tmfd)
						{
							tmfd->ctid = *otid;
							tmfd->xmax = InvalidTransactionId;
							tmfd->cmax = InvalidCommandId;
							tmfd->traversed = false;
						}
						UnlockReleaseBuffer(buffer);
						FLUX_RELEASE_TUPLOCK();
						return TM_Updated;
					}
				}
			}
			/* If now visible, fall through to perform the update */
		}

		/*
		 * Write-write conflict against an ALREADY-COMMITTED concurrent
		 * update.
		 *
		 * The visibility check above always returns "visible" for a committed
		 * in-place update, because at commit time t_commit_ts is rewound to
		 * the original insert timestamp (so mid-life readers still see the
		 * row). That rewind erases the "updated since you read it" signal
		 * heap keeps in xmax, so visibility alone can never detect a lost
		 * update.  Instead, the on-page tuple's verptr points at the UNDO
		 * record produced by the last committed update; if that committer is
		 * invisible to our snapshot, taking the in-place UPDATE now would
		 * silently lose it.  Return TM_Updated so the executor re-evaluates
		 * via EvalPlanQual, matching heap.
		 *
		 * Converge like heap: bounce to EPQ once per DISTINCT committed
		 * update, not once per probe.  FluxEpqReconcileMatches suppresses
		 * re-firing on the identical (verptr, xid) marker we already bounced
		 * on this statement; FluxEpqReconcileMark records the marker before
		 * we return.  A strictly-newer committer stamps a fresh verptr, so
		 * identity dedup only suppresses the same marker and never masks a
		 * genuine new conflict.
		 */
		if (!locked_by_me && IsMVCCSnapshot(snapshot))
		{
			UndoRecPtr	head_verptr;
			TransactionId head_xid;
			bool		head_inprogress = false;

			if (FluxTupleHasCommittedUpdateAfter(relation,
												 old_tuple_hdr,
												 ItemIdGetLength(itemid),
												 snapshot,
												 GetCurrentTransactionIdIfAny(),
												 &head_verptr,
												 &head_xid,
												 &head_inprogress) &&
				!FluxEpqReconcileMatches(snapshot,
										 RelationGetRelid(relation),
										 otid))
			{
				FluxEpqReconcileMark(snapshot,
									 RelationGetRelid(relation), otid,
									 head_verptr, head_xid);
				if (tmfd)
				{
					tmfd->ctid = *otid;
					tmfd->xmax = InvalidTransactionId;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				FLUX_RELEASE_TUPLOCK();
				return TM_Updated;
			}
			else if (head_inprogress &&
					 !TransactionIdIsCurrentTransactionId(head_xid))
			{
				/*
				 * Commit-window conflict (see
				 * FluxTupleHasCommittedUpdateAfter): the head committer
				 * cleared its sLog marker + UNCOMMITTED flag at PRE_COMMIT
				 * but has not yet reached CLOG.  Neither this gate nor the
				 * sLog dirty-xid probe sees it, so we would clobber a
				 * just-committed update.  Wait on the in-flight writer, then
				 * retry from the top of the slow path where CLOG now resolves
				 * it to a committed conflict -> TM_Updated -> EPQ.
				 * Heap-identical.
				 */
				if (wait)
				{
					TransactionId wait_xid = head_xid;

					/*
					 * Wait for the in-flight committer to reach CLOG or
					 * abort, then report TM_Updated so the executor
					 * re-fetches through EvalPlanQual.  EPQ's SnapshotAny
					 * fetch reconstructs the correct visible version (the
					 * committed value if head_xid committed, or the restored
					 * before-image if it aborted) and re-projects the update
					 * on top.  A blind update retry here would instead apply
					 * over the raw on-page bytes, which on abort are the
					 * not-yet-reverted value.
					 */
					UnlockReleaseBuffer(buffer);
					FluxLockTupleToWait(relation, otid,
										update_lockmode, &have_tuple_lock);
					XactLockTableWait(wait_xid, relation, otid, XLTW_Update);
					if (tmfd)
					{
						tmfd->ctid = *otid;
						tmfd->xmax = InvalidTransactionId;
						tmfd->cmax = InvalidCommandId;
						tmfd->traversed = false;
					}
					FLUX_RELEASE_TUPLOCK();
					return TM_Updated;
				}
			}
		}
	}

	/*
	 * Even when visibility returned "true", the tuple may have an in-progress
	 * modification by another transaction.  This happens when
	 * FluxTupleVisibleToSnapshotDual returns true for in-progress
	 * UPDATE/DELETE entries (to preserve tuple existence in scans).  We must
	 * still detect the write-write conflict and block.
	 *
	 * The in-progress writer is detected from the writer xids in the header
	 * and TransactionIdIsInProgress, and a conflicting row lock from the
	 * tuple sLog, NOT via the on-page FLUX_TUPLE_UNCOMMITTED flag, which is a
	 * hint cleared at PRE_COMMIT, before the writer's commit is in CLOG.
	 * Gating this wait on the flag (as we once did) let such a writer slip
	 * past, silently clobbering its in-progress update.  Probe
	 * unconditionally; only use the flag to opportunistically clear stale
	 * state when no writer is present.
	 */
	{
		TransactionId dirty_xid;
		bool		is_insert_entry;

		/*
		 * An in-progress writer (from the header) always conflicts, and a row
		 * lock conflicts iff its recorded LockTupleMode does.  A KeyShare FK
		 * locker conflicts only with a key change.  A pure writer-only probe
		 * would sail past a FOR UPDATE locker, which writes no xid in the
		 * header, and clobber the row it protects.  We then acquire the
		 * heavyweight LOCKTAG_TUPLE lock and XactLockTableWait on the
		 * conflicting xid, which queues us behind other waiters rather than
		 * deadlocking.
		 */
		dirty_xid = flux_update_conflict_xid(relation, otid, old_tuple_hdr,
											 key_changed, &is_insert_entry);

		if (!TransactionIdIsValid(dirty_xid))
		{
			/*
			 * No active writer.  If the page still carries a stale
			 * UNCOMMITTED flag, clear it opportunistically.
			 */
			if (old_tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED)
			{
				old_tuple_hdr->t_flags &= ~FLUX_TUPLE_UNCOMMITTED;
				MarkBufferDirty(buffer);
			}
		}
		else if (TransactionIdIsValid(dirty_xid) &&
				 !TransactionIdIsCurrentTransactionId(dirty_xid) &&
				 !is_insert_entry)
		{
			/*
			 * Another transaction has an in-progress UPDATE/DELETE. Block
			 * until it finishes, then re-check.
			 */
			if (wait)
			{
				TransactionId wait_xid = dirty_xid;

				/*
				 * Serialize competing updaters via a heavyweight tuple lock
				 * before sleeping on the conflicting xid, so two backends
				 * racing on the same tuple queue instead of deadlocking.
				 * Matches heap_update.
				 */
				UnlockReleaseBuffer(buffer);
				FluxLockTupleToWait(relation, otid,
									update_lockmode, &have_tuple_lock);
				XactLockTableWait(wait_xid, relation, otid, XLTW_Update);

				/* Re-read page after waking */
				buffer = ReadBuffer(relation, blkno);
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
				page = BufferGetPage(buffer);

				if (offnum < FirstOffsetNumber ||
					offnum > PageGetMaxOffsetNumber(page))
				{
					UnlockReleaseBuffer(buffer);
					FLUX_RELEASE_TUPLOCK();
					return TM_Invisible;
				}
				itemid = PageGetItemId(page, offnum);
				if (!ItemIdIsNormal(itemid))
				{
					UnlockReleaseBuffer(buffer);
					FLUX_RELEASE_TUPLOCK();
					return TM_Invisible;
				}
				old_tuple_hdr = (FluxTupleHeader *)
					PageGetItem(page, itemid);

				/* If deleted while we waited, report that */
				if (old_tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
				{
					TM_Result	res = flux_deleted_after_wait(old_tuple_hdr);

					if (tmfd)
					{
						tmfd->ctid = *otid;
						tmfd->xmax = wait_xid;
						tmfd->cmax = InvalidCommandId;
						tmfd->traversed = false;
					}
					UnlockReleaseBuffer(buffer);
					FLUX_RELEASE_TUPLOCK();
					return res;
				}

				/*
				 * The waited-on txn finished (committed or aborted).  Return
				 * TM_Updated to force the executor to re-evaluate via EPQ
				 * against the now-stable page.
				 *
				 * This is required even on ABORT, and is where FLUX differs
				 * from heap.  FLUX updates in place, so a READ COMMITTED scan
				 * that ran while the other writer was in progress dirty-read
				 * that writer's uncommitted value as the base for the new
				 * tuple's expression (e.g. counter = counter + 10 computed
				 * off the in-flight value).  Heap never sees this because its
				 * scan reads the prior committed version.  By the time we get
				 * here: - COMMIT: the page holds the other writer's committed
				 * value; EPQ recomputes on top of it. - ABORT: the
				 * per-backend UNDO reverse-apply has already restored the
				 * pre-update image inline (before the conflicting XID left
				 * the proc array), so the page holds the original committed
				 * value; EPQ recomputes on top of that. Either way EPQ
				 * re-reads the correct base and recomputes, matching heap's
				 * final result.
				 */
				if (tmfd)
				{
					tmfd->ctid = *otid;
					tmfd->xmax = wait_xid;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				FLUX_RELEASE_TUPLOCK();
				return TM_Updated;
			}
			else
			{
				/* NOWAIT mode */
				if (tmfd)
				{
					tmfd->ctid = *otid;
					tmfd->xmax = dirty_xid;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				FLUX_RELEASE_TUPLOCK();
				return TM_WouldBlock;
			}
		}
		/* If dirty_xid is our own or invalid, proceed with update */
	}

	/*
	 * Final committed-update gate (authoritative).
	 *
	 * STAGE 2 above runs the committed-update detector once, before the
	 * in-progress-writer wait.  That check races: while the head writer W is
	 * still in progress, the detector declines (W's undo record is not yet
	 * committed), so control falls through to the writer-wait stage.  The
	 * buffer exclusive lock does NOT block W's commit, and because FLUX
	 * updates in place, the on-page value is already W's.  If W commits in
	 * the window between STAGE 2's read and the writer-wait probe, the wait
	 * stage sees NO in-progress writer and would fall through to apply our
	 * update over W's now-committed value -- silently losing W's update.
	 *
	 * Re-run the detector here, after the wait stage has resolved, as the
	 * last action before the update proper.  Every path that reaches the
	 * update passes through this point with the buffer exclusively locked, so
	 * a committer that landed anywhere upstream is caught.  Reconcile dedup
	 * makes this converge exactly like STAGE 2: we bounce to EvalPlanQual
	 * once per distinct committed marker; a repeat of the identical (verptr,
	 * xid) marker we already reconciled falls through and applies.
	 *
	 * locked_by_me short-circuit (bug 4): skip this gate entirely when we
	 * already hold our own LOCK marker on the tuple.  The ON CONFLICT arbiter
	 * locked and re-evaluated the row; heap's own-lock short-circuit then
	 * applies the update without an EPQ bounce.  Returning TM_Updated here on
	 * that path would EPQ-refetch into a NULL ri_oldTupleSlot and crash.
	 */
	if (!locked_by_me && snapshot && IsMVCCSnapshot(snapshot))
	{
		UndoRecPtr	head_verptr;
		TransactionId head_xid;
		bool		head_inprogress = false;

		if (FluxTupleHasCommittedUpdateAfter(relation,
											 old_tuple_hdr,
											 ItemIdGetLength(itemid),
											 snapshot,
											 GetCurrentTransactionIdIfAny(),
											 &head_verptr,
											 &head_xid,
											 &head_inprogress))
		{
			if (!FluxEpqReconcileMatches(snapshot,
										 RelationGetRelid(relation),
										 otid))
			{
				FluxEpqReconcileMark(snapshot,
									 RelationGetRelid(relation), otid,
									 head_verptr, head_xid);
				if (tmfd)
				{
					tmfd->ctid = *otid;
					tmfd->xmax = InvalidTransactionId;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				UnlockReleaseBuffer(buffer);
				FLUX_RELEASE_TUPLOCK();
				return TM_Updated;
			}
		}
		else if (head_inprogress)
		{
			/*
			 * Commit-window conflict (see FluxTupleHasCommittedUpdateAfter):
			 * the head committer cleared its sLog marker + UNCOMMITTED flag
			 * at PRE_COMMIT but has not yet reached CLOG.  Neither this gate
			 * nor the sLog dirty-xid probe sees it, so we would clobber a
			 * just-committed update.  Wait on the in-flight writer, then
			 * retry from the top of the slow path where CLOG now resolves it
			 * to a committed conflict -> TM_Updated -> EPQ.  Heap-identical.
			 */
			if (wait)
			{
				TransactionId wait_xid = head_xid;

				/*
				 * Wait for the in-flight committer to reach CLOG or abort,
				 * then report TM_Updated so the executor re-fetches through
				 * EvalPlanQual.  EPQ's SnapshotAny fetch reconstructs the
				 * correct visible version (the committed value if head_xid
				 * committed, or the restored before-image if it aborted) and
				 * re-projects the update on top.  A blind update retry here
				 * would instead apply over the raw on-page bytes, which on
				 * abort are the not-yet-reverted value.
				 */
				UnlockReleaseBuffer(buffer);
				FluxLockTupleToWait(relation, otid,
									update_lockmode, &have_tuple_lock);
				XactLockTableWait(wait_xid, relation, otid, XLTW_Update);
				if (tmfd)
				{
					tmfd->ctid = *otid;
					tmfd->xmax = InvalidTransactionId;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				FLUX_RELEASE_TUPLOCK();
				return TM_Updated;
			}
		}
	}

	/* Transaction-snapshot RI action: see flux_tuple_delete_internal. */
	if (crosscheck != InvalidSnapshot &&
		!FluxTupleVisibleToSnapshotDual(old_tuple_hdr, crosscheck,
										RelationGetRelid(relation), buffer, otid))
	{
		if (tmfd)
		{
			tmfd->ctid = *otid;
			tmfd->xmax = FluxTupleGetXmin(old_tuple_hdr);
			tmfd->cmax = InvalidCommandId;
			tmfd->traversed = false;
		}
		UnlockReleaseBuffer(buffer);
		FLUX_RELEASE_TUPLOCK();
		return TM_Updated;
	}

	/*
	 * Every gate has passed, and the buffer stays locked until the row is
	 * written.  The caller decided whether an indexed column changes, and
	 * which index entries change, from the version it read before any wait. A
	 * writer we waited for may have rolled back since, restoring the version
	 * before its own, or have committed: the row is then not the one the
	 * decision describes, and keeping it would overwrite the row with no
	 * index work or the wrong one.  Ask the caller to decide again, and hand
	 * it the tuple lock: as heap_update keeps it across its goto l2, the
	 * UPDATE keeps its place ahead of the transactions queued behind it, or a
	 * DELETE queued behind it would delete the row first.
	 */
	if (TransactionIdIsValid(decided_xmin) &&
		!flux_same_version(old_tuple_hdr, decided_verptr, decided_xmin))
	{
		UnlockReleaseBuffer(buffer);
		if (have_tuple_lock && !*kept_tuplock)
		{
			*kept_tuplock = true;
			*kept_mode = update_lockmode;
			have_tuple_lock = false;
		}
		FLUX_RELEASE_TUPLOCK();
		*redecide = true;
		return TM_Ok;
	}

	/*
	 * Get transaction timestamp BEFORE critical section (initializes the
	 * per-transaction MVCC state FluxGetDmlTimestamp relies on).  Commit
	 * visibility comes from CLOG (heap-shaped xmin/xmax MVCC).
	 */
	(void) FluxGetTransactionTimestamp();
	current_ts = (uint64) FluxGetDmlTimestamp();

	/*
	 * Ensure the current transaction has an XID assigned BEFORE entering the
	 * critical section.  GetCurrentTransactionId() may call
	 * XactLockTableInsert() which acquires a lock and allocates memory --
	 * both forbidden in a critical section.
	 *
	 * Without an assigned XID, RecordTransactionCommit() considers the
	 * transaction read-only and skips the WAL flush, even though we write WAL
	 * records for the data change.  This would cause the update to be lost on
	 * crash recovery.
	 */
	(void) GetCurrentTransactionId();

	/*
	 * Form the new tuple from the slot.
	 *
	 * Fast path: If the old tuple is small (no overflow potential), keep the
	 * buffer locked and form the tuple without overflow handling.  This
	 * avoids the expensive unlock/relock cycle and the re-validation that
	 * follows.
	 *
	 * Slow path: For large tuples or those with existing overflow data,
	 * release the buffer lock first.  FluxFormTuple may call
	 * FluxStoreOverflowColumn which acquires buffer locks on overflow pages.
	 * If overflow data lands on the same page we're updating, that would
	 * cause a buffer lock re-entry assertion failure.
	 */
	update_overflow_buffers.count = 0;
	{
		bool		buffer_unlocked = false;

		/*
		 * This in-place UPDATE path runs only when no indexed column changed
		 * (a key-changing UPDATE is routed to the out-of-place path), so the
		 * varlena columns are typically unchanged.  FLUX has no on-page
		 * overflow: TOAST any oversized varlena into the relation's standard
		 * heap TOAST table before forming, exactly like the insert path, then
		 * form the tuple from the (possibly externalized) datums.
		 */
		slot_getallattrs(slot);
		{
			int			upd_natts = RelationGetDescr(relation)->natts;

			/*
			 * Like heap, hand the toaster the old version: an unchanged
			 * external value is reused, and a superseded one is deleted by
			 * flux_toast_cleanup() once the new version is in place.  That
			 * delete is an ordinary MVCC delete in the TOAST table, so an
			 * abort keeps the value for the restored before-image, and VACUUM
			 * reclaims it only once no snapshot that could read the
			 * before-image remains.  Deform a private copy: the page image is
			 * overwritten before the cleanup runs.
			 */
			if (old_tuple_hdr->t_infomask & FLUX_INFOMASK_HASEXTERNAL)
			{
				FluxTupleData old_tup;

				old_tup.t_len = ItemIdGetLength(itemid);
				upd_old_copy = palloc(old_tup.t_len);
				memcpy(upd_old_copy, old_tuple_hdr, old_tup.t_len);
				old_tup.t_data = upd_old_copy;
				ItemPointerSet(&old_tup.t_self, blkno, offnum);
				FluxDeformTuple(relation, &old_tup, RelationGetDescr(relation),
								upd_old_values, upd_old_isnull);
			}

			memcpy(upd_toast_values, slot->tts_values, upd_natts * sizeof(Datum));
			memcpy(upd_toast_isnull, slot->tts_isnull, upd_natts * sizeof(bool));

			/*
			 * Without logical decoding of the UPDATE, as for heap, none of
			 * its TOAST chunks either.
			 */
			flux_toast_tuple(relation, upd_toast_values, upd_toast_isnull,
							 upd_old_copy ? upd_old_values : NULL,
							 upd_old_copy ? upd_old_isnull : NULL,
							 &upd_ttc, upd_toast_attr,
							 &upd_toasted,
							 walLogical ? 0 : HEAP_INSERT_NO_LOGICAL);

			new_tuple = FluxFormTuple(RelationGetDescr(relation),
									  upd_toast_values,
									  upd_toast_isnull,
									  NULL, /* FLUX has no on-page overflow */
									  NULL);
		}

		/* Set MVCC fields for new tuple (heap-shaped: fresh version) */
		new_tuple->t_data->t_commit_ts = 0; /* t_xmax = InvalidTransactionId */

		/*
		 * The new on-page image is the NEWEST version; stamp its inserter
		 * (t_xmin) with our XID.  Older snapshots that cannot see this XID
		 * read the pre-update image back from the UNDO log via t_verptr.
		 */
		new_tuple->t_data->t_flags |= FLUX_TUPLE_UNCOMMITTED;
		new_tuple->t_data->t_xmin = GetCurrentTransactionId();	/* subxid: heap-shaped,
																 * so savepoint rollback
																 * marks it aborted in
																 * CLOG */
		new_tuple_size = new_tuple->t_len;

		/*
		 * The version-chain head lives in the fixed header field t_verptr, so
		 * new_tuple already carries it inside its header with no trailing
		 * growth.  The real urec_ptr is stamped in below after the
		 * per-backend UNDO insert returns; until then t_verptr holds
		 * InvalidUndoRecPtr (consistent with the unlogged/temp path that
		 * never reserves).
		 */
		new_tuple->t_data->t_flags |= FLUX_TUPLE_HAS_VERSION_PTR;
		FluxTupleSetVersionPtr(new_tuple->t_data,
							   InvalidUndoRecPtr);

		/*
		 * Pre-compute the oldest active timestamp before (re-)acquiring the
		 * buffer lock.  This avoids an O(MaxBackends) shared-memory scan
		 * while holding a page-level exclusive lock.  The value is used by
		 * the defrag estimation/execution path below.
		 */
		defrag_oldest_ts = FluxGetOldestActiveTimestamp();
		defrag_oldest_xmin = FluxGetOldestXminHorizon(relation);

		if (buffer_unlocked)
		{
			/*
			 * Re-acquire the buffer lock for the in-place update decision.
			 * Check if the main buffer is already locked as part of the
			 * overflow buffers to avoid double-lock assertion failure.
			 */
			bool		buffer_already_locked = false;

			for (upd_i = 0; upd_i < update_overflow_buffers.count; upd_i++)
			{
				if (update_overflow_buffers.buffers[upd_i].buffer == buffer)
				{
					buffer_already_locked = true;
					break;
				}
			}

			if (!buffer_already_locked)
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			page = BufferGetPage(buffer);

			/*
			 * Re-validate the tuple after re-locking.  Another backend may
			 * have reorganized the page while we didn't hold the lock.
			 */
			itemid = PageGetItemId(page, offnum);
			if (!ItemIdIsNormal(itemid))
			{
				UnlockReleaseBuffer(buffer);
				FluxFreeTuple(new_tuple);
				flux_toast_abandon(&upd_ttc);
				FLUX_RELEASE_TUPLOCK();
				return TM_Invisible;
			}
			old_tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);
		}
	}

	/*
	 * Determine if we can do an in-place update.
	 *
	 * In-place update is FLUX's primary advantage over heap: it avoids
	 * creating dead tuple versions and the associated index maintenance. We
	 * try several strategies in order of increasing cost:
	 *
	 * 1. Direct fit: new tuple fits within the old tuple's slot. 2. Page
	 * space fit: new tuple is larger but the extra bytes fit in the page's
	 * available free space. 3. Defrag fit: page defragmentation frees enough
	 * space for the new tuple to fit in-place.
	 *
	 */
retry_fit:
	if (new_tuple_size <= ItemIdGetLength(itemid))
	{
		/* Strategy 1: new tuple fits within old tuple's slot */
	}
	else if (new_tuple_size <= ItemIdGetLength(itemid) + PageGetFreeSpace(page))
	{
		/*
		 * Strategy 2: new tuple is larger but the difference fits in the
		 * page's free space.  We need to relocate the tuple data within the
		 * page, which PageRepairFragmentation can handle.
		 */
	}
	else
	{
		/*
		 * Strategy 3: try page defragmentation to reclaim dead tuple space.
		 * If the page has the defrag-needed flag and defragmentation would
		 * free enough space, do it now.
		 */
		FluxPageOpaque upd_opaque = FluxPageGetOpaque(page);
		Size		potential_free;

		(void) upd_opaque;		/* flag is only an optimization hint; see
								 * below */

		{
			/*
			 * Strategy 3: reclaim space occupied by dead-to-all superseded
			 * versions via defragmentation.
			 *
			 * We DO NOT gate this on FLUX_PAGE_DEFRAG_NEEDED.  That flag is
			 * an optimization hint set by the pruning paths, but in-place
			 * UPDATEs accumulate dead-to-all versions on a hot page (e.g.
			 * TPC-C district) without reliably setting it.  Trusting the flag
			 * as authoritative made a 2-byte tuple growth spuriously fail
			 * with "does not fit" on a page that was actually full of
			 * reclaimable dead versions (measured: ~34k such aborts/run,
			 * contigfree=0, maxoff 47-69 on a 500-row table).  Always scan;
			 * only error if a full defrag genuinely cannot free enough space.
			 */
			/*
			 * Estimate how much space defragmentation could free by scanning
			 * for dead tuples.  This is a quick scan without actually
			 * defragmenting yet.
			 */
			potential_free = PageGetFreeSpace(page);
			{
				OffsetNumber df_off;
				OffsetNumber df_maxoff = PageGetMaxOffsetNumber(page);

				for (df_off = FirstOffsetNumber; df_off <= df_maxoff; df_off++)
				{
					ItemId		df_itemid = PageGetItemId(page, df_off);
					FluxTupleHeader *df_hdr;

					if (!ItemIdIsNormal(df_itemid))
					{
						if (ItemIdIsDead(df_itemid))
							potential_free += ItemIdGetLength(df_itemid) + sizeof(ItemIdData);
						continue;
					}

					if (FluxIsOverflowRecordInline(PageGetItem(page, df_itemid),
												   ItemIdGetLength(df_itemid)))
						continue;

					df_hdr = (FluxTupleHeader *) PageGetItem(page, df_itemid);

					if (FluxTupleDeadToAll(df_hdr, defrag_oldest_xmin))
					{
						potential_free += ItemIdGetLength(df_itemid) + sizeof(ItemIdData);
					}
				}
			}

			if (new_tuple_size <= ItemIdGetLength(itemid) + potential_free)
			{
				/*
				 * Defragmentation should free enough space.  Do it now. We
				 * are already holding an exclusive lock on the buffer. First
				 * mark dead tuples LP_DEAD, then defragment.
				 *
				 * As in FluxPagePruneOpt, opportunistic pruning sets LP_DEAD
				 * (reclaiming storage) rather than LP_UNUSED.  The deleted
				 * tuples may still have index entries; reserving the line
				 * pointer until VACUUM removes those entries prevents the TID
				 * from being recycled and returning wrong index-scan results.
				 *
				 * defrag_oldest_ts was pre-computed before acquiring the
				 * buffer lock to avoid shared-memory scans while holding
				 * page-level exclusive locks.
				 */
				TransactionId df_horizon = InvalidTransactionId;

				/*
				 * This compacts under our exclusive lock alone, where VACUUM
				 * and FluxPagePruneOpt take a cleanup lock.  That is enough
				 * on the primary: every FLUX reader copies the tuple it
				 * returns while it holds the content lock, so a pin alone
				 * keeps no pointer into the page.  Replay of the record takes
				 * a cleanup lock, for standby readers.
				 */
				START_CRIT_SECTION();
				{
					OffsetNumber prune_off;
					OffsetNumber prune_maxoff = PageGetMaxOffsetNumber(page);

					for (prune_off = FirstOffsetNumber; prune_off <= prune_maxoff; prune_off++)
					{
						ItemId		prune_itemid = PageGetItemId(page, prune_off);
						FluxTupleHeader *prune_hdr;

						if (!ItemIdIsNormal(prune_itemid))
							continue;

						if (FluxIsOverflowRecordInline(PageGetItem(page, prune_itemid),
													   ItemIdGetLength(prune_itemid)))
							continue;

						prune_hdr = (FluxTupleHeader *) PageGetItem(page, prune_itemid);

						if (FluxTupleDeadToAll(prune_hdr, defrag_oldest_xmin))
						{
							FluxTupleAdvanceConflictHorizon(prune_hdr,
															&df_horizon);
							ItemIdSetDead(prune_itemid);
						}
					}
				}
				FluxPageDefragment(page);
				MarkBufferDirty(buffer);

				if (RelationNeedsWAL(relation))
				{
					XLogRecPtr	df_lsn;

					df_lsn = FluxXLogDefrag(relation, buffer, NULL, 0,
											defrag_oldest_ts, df_horizon);
					PageSetLSN(page, df_lsn);
				}
				END_CRIT_SECTION();

				FluxRecordFreeSpace(relation, blkno, PageGetHeapFreeSpace(page));

				/*
				 * Re-fetch the item after defragmentation since line pointers
				 * may have been reorganized.  The offset number should still
				 * be valid for surviving tuples.
				 */
				itemid = PageGetItemId(page, offnum);
				old_tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

				/* Check again if in-place update now fits */
				if (new_tuple_size <= ItemIdGetLength(itemid) + PageGetFreeSpace(page))
				{
					flux_stat_defrag_triggered_updates++;
				}
				else
				{
					if (!force_shrink_attempted &&
						update_overflow_buffers.count == 0)
						goto force_shrink_retry;
					flux_release_update_overflow_buffers(&update_overflow_buffers,
														 buffer);
					UnlockReleaseBuffer(buffer);
					FluxFreeTuple(new_tuple);
					flux_toast_abandon(&upd_ttc);

					/*
					 * BUG 2 fallback: the row cannot grow in place on this
					 * full page even after defragmentation, and FLUX TIDs are
					 * stable so it cannot relocate within the page.  Rather
					 * than error, re-drive as an out-of-place (new-TID)
					 * update, which moves the row to a page with room and
					 * rebuilds all index entries (TU_All).  We have released
					 * the buffer and freed the speculatively formed tuple,
					 * and no UNDO for this update has been reserved yet, so
					 * the state is clean to restart from.
					 */
					FLUX_RELEASE_TUPLOCK();
					if (did_inplace_key_update != NULL)
						*did_inplace_key_update = false;
					return flux_update_out_of_place(relation, otid, slot, cid,
													options, snapshot,
													crosscheck, wait, tmfd,
													lockmode, update_indexes);
				}
			}
			else
			{
				/* Defrag wouldn't free enough space */
				if (!force_shrink_attempted &&
					update_overflow_buffers.count == 0)
					goto force_shrink_retry;
				flux_release_update_overflow_buffers(&update_overflow_buffers,
													 buffer);
				UnlockReleaseBuffer(buffer);
				FluxFreeTuple(new_tuple);
				flux_toast_abandon(&upd_ttc);

				/* BUG 2 fallback (see above): out-of-place instead of error. */
				FLUX_RELEASE_TUPLOCK();
				if (did_inplace_key_update != NULL)
					*did_inplace_key_update = false;
				return flux_update_out_of_place(relation, otid, slot, cid,
												options, snapshot, crosscheck,
												wait, tmfd, lockmode,
												update_indexes);
			}
		}
	}

	if (false)
	{
		/*
		 * Recovery path for an in-place-grown tuple that no longer fits in
		 * its slot, even after page defragmentation.  FLUX TIDs are stable,
		 * so the tuple cannot move to another page; the only way to shrink it
		 * is to push variable-length columns off-page.  Re-form the new tuple
		 * forcing every eligible varlena column into overflow with a zero
		 * inline prefix, which collapses the main tuple to its minimum
		 * footprint, then retry the in-place fit (it should now satisfy
		 * Strategy 1).
		 *
		 * This branch is only entered from the ERROR sites above, and only
		 * when the first form attempt took the no-overflow fast path (the
		 * main buffer is still locked and no overflow buffers were
		 * collected).
		 */
force_shrink_retry:
		{
			bool		relock_already_locked = false;
			UndoRecPtr	shrink_verptr = FluxTupleGetVersionPtr(old_tuple_hdr);
			TransactionId shrink_xmin = FluxTupleGetXmin(old_tuple_hdr);

			FluxFreeTuple(new_tuple);

			/* Release the main buffer lock before re-forming with overflow. */
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

			new_tuple = FluxFormTupleForceShrink(RelationGetDescr(relation),
												 upd_toast_values,
												 upd_toast_isnull,
												 relation,
												 &update_overflow_buffers);

			new_tuple->t_data->t_commit_ts = 0; /* t_xmax =
												 * InvalidTransactionId */
			new_tuple->t_data->t_flags |= FLUX_TUPLE_UNCOMMITTED;
			new_tuple->t_data->t_xmin = GetCurrentTransactionId();	/* subxid: heap-shaped,
																	 * so savepoint rollback
																	 * marks it aborted in
																	 * CLOG */
			new_tuple_size = new_tuple->t_len;

			/*
			 * Stamp InvalidUndoRecPtr into t_verptr, mirroring the main
			 * exclusive path, regardless of whether we reached this branch
			 * via fit cascade or force-shrink retry.
			 */
			new_tuple->t_data->t_flags |= FLUX_TUPLE_HAS_VERSION_PTR;
			FluxTupleSetVersionPtr(new_tuple->t_data,
								   InvalidUndoRecPtr);

			/*
			 * Re-acquire the main buffer lock.  An overflow column may have
			 * landed on the page we are updating, in which case it is already
			 * locked as part of update_overflow_buffers.
			 */
			for (upd_i = 0; upd_i < update_overflow_buffers.count; upd_i++)
			{
				if (update_overflow_buffers.buffers[upd_i].buffer == buffer)
				{
					relock_already_locked = true;
					break;
				}
			}
			if (!relock_already_locked)
				LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			page = BufferGetPage(buffer);

			/* Re-validate the line pointer after re-locking. */
			itemid = PageGetItemId(page, offnum);
			if (!ItemIdIsNormal(itemid))
			{
				flux_release_update_overflow_buffers(&update_overflow_buffers,
													 buffer);
				UnlockReleaseBuffer(buffer);
				FluxFreeTuple(new_tuple);
				flux_toast_abandon(&upd_ttc);
				FLUX_RELEASE_TUPLOCK();
				return TM_Invisible;
			}
			old_tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

			/*
			 * Re-validate visibility after the unlock/relock window.  The fit
			 * cascade ran conflict detection before forming the tuple, but
			 * re-forming with overflow drops the buffer lock again.  A
			 * concurrent committed delete that landed in that window must be
			 * reported as TM_Deleted rather than silently overwritten.
			 */
			if ((old_tuple_hdr->t_flags & FLUX_TUPLE_DELETED) &&
				!(old_tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED))
			{
				flux_release_update_overflow_buffers(&update_overflow_buffers,
													 buffer);
				UnlockReleaseBuffer(buffer);
				FluxFreeTuple(new_tuple);
				flux_toast_abandon(&upd_ttc);
				FLUX_RELEASE_TUPLOCK();
				return TM_Deleted;
			}

			/*
			 * Likewise a concurrent writer that got in while the buffer was
			 * unlocked, or one that came and went: either the header names a
			 * foreign writer we did not see at the conflict gates, or the
			 * version-chain head moved.  Do not overwrite it; report
			 * TM_Updated so the executor waits and re-evaluates via EPQ, as
			 * the gates would have.
			 */
			if (FluxTupleGetVersionPtr(old_tuple_hdr) != shrink_verptr ||
				!TransactionIdEquals(FluxTupleGetXmin(old_tuple_hdr),
									 shrink_xmin))
			{
				flux_release_update_overflow_buffers(&update_overflow_buffers,
													 buffer);
				UnlockReleaseBuffer(buffer);
				FluxFreeTuple(new_tuple);
				if (tmfd)
				{
					tmfd->ctid = *otid;
					tmfd->xmax = InvalidTransactionId;
					tmfd->cmax = InvalidCommandId;
					tmfd->traversed = false;
				}
				FLUX_RELEASE_TUPLOCK();
				return TM_Updated;
			}

			force_shrink_attempted = true;
			goto retry_fit;
		}
	}

	/*
	 * Save a copy of the old tuple data BEFORE entering the critical section
	 * and BEFORE modifying the page.  palloc is not allowed inside critical
	 * sections, and in-place updates overwrite the on-page data, so we must
	 * preserve the original tuple for WAL logging (the before-image).
	 *
	 * For small tuples (common case in pgbench-style OLTP), use stack buffers
	 * to avoid palloc overhead in the hot path.
	 */
	{
		uint32		old_len = ItemIdGetLength(itemid);
		char	   *old_copy;

		old_tuple_for_inplace_wal = palloc0(sizeof(FluxTupleData));
		old_copy = palloc(old_len);

		memcpy(old_copy, old_tuple_hdr, old_len);
		update_freeze_conflict = flux_prepare_before_image(relation, (FluxTupleHeader *) old_copy);
		old_tuple_for_inplace_wal->t_len = old_len;
		old_tuple_for_inplace_wal->t_data = (FluxTupleHeader *) old_copy;
	}

	/*
	 * NOTE: The early UNDO record (pre-modification) was removed to avoid
	 * double UNDO records.  The deferred UNDO path below (upd_undo_ptr)
	 * handles UNDO recording for all in-place updates.  The deferred approach
	 * is safe because the WAL record includes both old and new tuple data,
	 * enabling crash recovery regardless of UNDO write order.
	 */


	/*
	 * Pre-allocate WAL buffer space BEFORE entering critical section. We may
	 * need to register the main buffer plus overflow buffers.
	 *
	 * rdata slots needed for UPDATE: MAX_OVERFLOW_BUFFERS * 2 (header + data
	 * per overflow record) + 3 (xl_flux_update header + old tuple data + new
	 * tuple data)
	 *
	 * CRITICAL: XLogEnsureRecordSpace() may allocate memory, so it MUST be
	 * called outside the critical section.
	 */
	if (RelationNeedsWAL(relation))
		XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID, 10 + MAX_OVERFLOW_BUFFERS * 2);

	/*
	 * Per-backend UNDO: prepare a full-tuple UPDATE record before the
	 * critical section.  The record stores the old tuple so a top-level
	 * ROLLBACK can restore it in place.  PrepareUndoInsert may ereport, so it
	 * must run outside the crit section.  Lock order: data buffer then UNDO
	 * buffer.
	 *
	 * If a force-shrink retry pushed columns off-page,
	 * update_overflow_buffers holds buffers that are pinned, content-locked,
	 * and marked dirty but not yet WAL-logged (overflow WAL is deferred to
	 * the atomic record below). PrepareUndoInsert and
	 * CheckForSerializableConflictIn can both ereport here, before the
	 * critical section makes the change durable.  Resource-owner cleanup
	 * would still release the pins and locks on abort, leaving only
	 * VACUUM-reclaimable dead overflow space, but release them explicitly so
	 * the abort path leaves no dirtied-yet-orphaned overflow pages behind.
	 */
	PG_TRY();
	{
		/*
		 * Build the heap-format logical-decoding images (old and new) BEFORE
		 * the critical section, since heap_form_tuple()/palloc() are
		 * forbidden inside it.  When the relation is not logically logged, or
		 * the caller asked for no decoding (TABLE_UPDATE_NO_LOGICAL, as
		 * REPACK (CONCURRENTLY) does for its transient table), these leave
		 * data == NULL, and the record carries no logical images, which
		 * decoding then skips.  These calls palloc and can ereport on OOM, so
		 * they stay inside the PG_TRY: a throw here must also release the
		 * force-shrink overflow buffers, exactly like the reservation above.
		 */
		FluxXLogPrepareReplicaIdentity(relation,
									   walLogical ? old_tuple_for_inplace_wal : NULL,
									   new_tuple, &update_old_img);
		FluxXLogPrepareLogicalImage(relation, walLogical ? new_tuple : NULL,
									&update_new_img);

		/*
		 * Per-backend UNDO: FLUX always produces a version-chain UNDO record
		 * for an in-place UPDATE.  The record is PREPARED after the new
		 * on-page image is finalized below (widen fixup, flags, t_gen,
		 * t_ctid), so the emit-time delta scan compares the old image against
		 * the exact bytes that will land on the page.  Only the version
		 * pointer at offset 8 is stamped afterwards (from the returned
		 * UndoRecPtr), and the delta always covers offset 8.
		 */

		/*
		 * Widen-then-narrow fixup.  The version pointer lives in the fixed
		 * header field t_verptr, so FluxTupleGetVersionPtr/SetVersionPtr read
		 * and write it independently of the slot length -- widening the image
		 * to the old (larger) slot length no longer moves the pointer.  The
		 * in-place overwrite path below keeps the old line-pointer length
		 * when the new image is smaller; deform's self-describing t_natts
		 * lets the trailing slack be ignored by ordinary readers.  We still
		 * widen + zero-pad here so the memcpy below fills the whole slot (no
		 * stale trailing bytes).
		 *
		 * Done here, before the critical section, because repalloc()/pfree()
		 * are forbidden inside it.
		 */
		if (new_tuple_size <= ItemIdGetLength(itemid))
		{
			Size		slot_len = ItemIdGetLength(itemid);

			if (slot_len > new_tuple_size &&
				(new_tuple->t_data->t_flags & FLUX_TUPLE_HAS_VERSION_PTR))
			{
				UndoRecPtr	verptr = FluxTupleGetVersionPtr(new_tuple->t_data);

				new_tuple->t_data = (FluxTupleHeader *)
					repalloc(new_tuple->t_data, slot_len);
				MemSet((char *) new_tuple->t_data + new_tuple_size, 0,
					   slot_len - new_tuple_size);
				new_tuple->t_len = (uint32) slot_len;
				new_tuple_size = slot_len;
				FluxTupleSetVersionPtr(new_tuple->t_data, verptr);
			}
		}

		/*
		 * transaction read this tuple (holds a SIREAD lock on it), our update
		 * creates an rw-antidependency that may form a dangerous structure.
		 */
		CheckForSerializableConflictIn(relation, otid, BufferGetBlockNumber(buffer));

		/*
		 * Always set UNCOMMITTED so that visibility checks consult the sLog.
		 * Even though the tuple position hasn't moved (in-place update), the
		 * DATA has changed and other transactions must see the old data until
		 * this update commits.  The flag will be lazily cleared on the first
		 * visibility check after the updating transaction commits (since the
		 * sLog entry will have been removed at commit time).
		 *
		 * Also set FLUX_TUPLE_UPDATED to mark that this tuple has been
		 * updated in-place.  After commit, this flag persists and indicates
		 * that the tuple's t_commit_ts reflects the original INSERT commit
		 * time (not the UPDATE commit time).  This preserves visibility for
		 * readers whose snapshots predate the update.
		 */
		new_tuple->t_data->t_flags |= FLUX_TUPLE_UPDATED;

		/*
		 * Record whether this update changes an indexed column.  This covers
		 * every non-CAS in-place image: same-size, growing, shrinking and the
		 * key-changing (delete-marking) UPDATE alike, because they all land
		 * here.  A concurrent LockTupleKeyShare probe reads this bit to
		 * decide whether it may skip waiting on us.  See
		 * FLUX_TUPLE_KEYS_UPDATED.
		 */
		if (key_changed)
			new_tuple->t_data->t_flags |= FLUX_TUPLE_KEYS_UPDATED;

		/*
		 * Stamp t_cmin to match the t_xmin this image carries.
		 *
		 * The in-place image's t_xmin was just re-stamped to OUR xid above
		 * (the on-page image is the newest version and we are its author), so
		 * t_cmin must be OUR command id: the pair (t_xmin, t_cmin) has to
		 * describe one writer.  Carrying the original INSERT's cid forward
		 * here was WRONG and broke six isolation tests -- it paired a NEW
		 * xmin with an OLD cid, so FluxTupleSatisfiesMVCC's "inserted by a
		 * later command of my own transaction" test compared our own curcid
		 * against a stranger's cid and hid rows we had legitimately written
		 * (freeze-the-dead reported "unexpected table_tuple_lock status: 1"
		 * == TM_Invisible; total-cash read a stale SUM).
		 *
		 * Using cid here is also exactly what self-visibility needs on this
		 * path: our own LATER command must not see a row this command wrote.
		 */
		new_tuple->t_data->t_cmin = cid;

		/*
		 * Finalize t_ctid to the (unchanged) in-place position so the emit
		 * delta scan sees the exact on-page bytes.  It is re-set identically
		 * inside the critical section.
		 */
		ItemPointerSet(&new_tuple->t_data->t_ctid, blkno, offnum);

		/*
		 * Choose delta vs full at emit time.  Compute the prefix/suffix diff
		 * of the OLD image relative to the FINAL new on-page image, forcing
		 * the t_verptr offset (8) into the recorded middle so the version
		 * chain link is always carried.  FluxUndoDeltaCompute returns 0 when
		 * the delta is not strictly smaller than the full image, in which
		 * case we emit the full before-image exactly as before.  DELETE keeps
		 * the full image; INSERT writes no undo.  PrepareUndoInsert returns
		 * the UndoRecPtr that becomes this new version's t_verptr (the head
		 * of the tuple's version chain); the before-image (full or delta)
		 * carries the prior chain link, so the chain continues verbatim
		 * through reverse- apply.
		 */
		{
			uint32		upd_old_len = old_tuple_for_inplace_wal->t_len;
			Size		upd_delta_total;
			char	   *upd_delta_buf =
				(char *) palloc(SizeOfFluxUndoDelta + upd_old_len);

			/*
			 * A waiter may have observed a now-aborted key before acquiring
			 * the row.  Index maintenance must use the same authoritative
			 * before-image as this undo record, not that earlier observation.
			 */
			if (key_changed)
			{
				if (*index_old != NULL)
					pfree(*index_old);
				*index_old = palloc(upd_old_len);
				*index_old_len = upd_old_len;
				memcpy(*index_old, old_tuple_for_inplace_wal->t_data, upd_old_len);
			}

			upd_delta_total =
				FluxUndoDeltaCompute((const char *) old_tuple_for_inplace_wal->t_data,
									 upd_old_len,
									 (const char *) new_tuple->t_data,
									 new_tuple_size,
									 offsetof(FluxTupleHeader, t_verptr),
									 upd_delta_buf,
									 SizeOfFluxUndoDelta + upd_old_len);

			if (upd_delta_total > 0)
				upd_undo_ptr = FluxPbuPrepareInsert(FLUX_UNDO_UPDATE_DELTA,
													relation,
													otid,
													upd_delta_buf,
													(uint32) upd_delta_total,
													relation->rd_rel->relpersistence, key_changed, update_freeze_conflict,
													cid);
			else
				upd_undo_ptr = FluxPbuPrepareInsert(FLUX_UNDO_UPDATE,
													relation,
													otid,
													(const char *) old_tuple_for_inplace_wal->t_data,
													upd_old_len,
													relation->rd_rel->relpersistence, key_changed, update_freeze_conflict,
													cid);

			pfree(upd_delta_buf);

			/*
			 * Stamp the freshly reserved urec_ptr (never preserve a prior
			 * pointer).  This is the only new-image mutation after the delta
			 * scan and it lands at offset 8, inside the delta's recorded
			 * middle.
			 */
			FluxTupleSetVersionPtr(new_tuple->t_data,
								   upd_undo_ptr);
			*index_urp = upd_undo_ptr;
		}


	}
	PG_CATCH();
	{
		flux_release_update_overflow_buffers(&update_overflow_buffers, buffer);
		PG_RE_THROW();
	}
	PG_END_TRY();

	vmbuffer = FluxVMPinForChange(relation, buffer);

	/* Start critical section for WAL logging */
	START_CRIT_SECTION();

	/*
	 * Set t_ctid on the in-memory new tuple BEFORE copying to the page. This
	 * ensures the WAL record's new_tuple image includes the correct t_ctid,
	 * so redo produces a page identical to the primary.  (We use blkno/offnum
	 * which is correct for both the "fits in existing slot" and
	 * "delete+re-add" strategies since we update offnum below if it changes.)
	 */
	ItemPointerSet(&new_tuple->t_data->t_ctid, blkno, offnum);

	if (new_tuple_size <= ItemIdGetLength(itemid))
	{
		/*
		 * New tuple fits within the old tuple's allocated space. Overwrite
		 * directly -- safe because we don't exceed the existing allocation.
		 * When the kept slot is larger than the new image, new_tuple was
		 * already widened to the full slot length before the critical section
		 * (see the widen block above), so new_tuple_size == slot_len here and
		 * the memcpy fills the whole slot, keeping any trailing slack zeroed.
		 * The version pointer lives in the header field t_verptr, so it is
		 * copied by the memcpy regardless of slot width.
		 */
		memcpy(old_tuple_hdr, new_tuple->t_data, new_tuple_size);
	}
	else
	{
		/*
		 * New tuple is larger than the old one but fits on the page (Strategy
		 * 2 or 3).  We cannot memcpy in place because that would overwrite
		 * adjacent data.  Use PageIndexTupleOverwrite, which grows the slot
		 * in place at the SAME offnum: it shifts only the tuple data that
		 * physically precedes this tuple and fixes the affected line-pointer
		 * offsets, keyed purely by (offnum, new size).  This is the identical
		 * primitive the redo handler replays (see
		 * flux_xlog_update_inplace_redo), so primary and recovery converge
		 * deterministically regardless of where the other tuples' data
		 * happens to sit on the page -- unlike the old
		 * FluxPageIndexTupleDelete + PageAddItem, whose page compaction and
		 * line-pointer renumbering were not reproducible under crash-recovery
		 * replay ordering.  offnum is unchanged.
		 */
		if (!PageIndexTupleOverwrite(page, offnum,
									 (const char *) new_tuple->t_data,
									 new_tuple_size))
			elog(PANIC, "failed to overwrite-grow FLUX tuple in place");

		/* Re-fetch itemid and header from the (same) location */
		itemid = PageGetItemId(page, offnum);
		old_tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

		ItemPointerSet(&new_tuple->t_data->t_ctid, blkno, offnum);
	}

	/* Set new TID to same location */
	ItemPointerSet(&slot->tts_tid, blkno, offnum);
	new_tuple->t_self = slot->tts_tid;

	/*
	 * t_ctid on the on-disk tuple is already correct from the memcpy or
	 * PageAddItem above, since we set it on new_tuple->t_data before copying.
	 */

	/* Track in-place update success */
	flux_stat_in_place_updates++;

	slot->tts_tableOid = RelationGetRelid(relation);

	/*
	 * Update page opaque header to track the latest commit timestamp and
	 * current free space.  This must happen before MarkBufferDirty and WAL
	 * logging so that full-page images capture the correct opaque state.  The
	 * redo function performs the same updates so WAL consistency checking
	 * passes.
	 */
	{
		FluxPageOpaque upd_phdr = FluxPageGetOpaque(page);

		FluxPageSetCommitTs(upd_phdr, Max(FluxPageGetCommitTs(upd_phdr), current_ts));
	}

	FluxVMClearForChange(buffer, vmbuffer);
	MarkBufferDirty(buffer);

	/*
	 * Write the per-backend UPDATE UNDO record into its pinned undo buffers
	 * BEFORE the forward-op WAL record so FluxXLogUpdate folds the undo pages
	 * onto its record as full-page images (crash-safe).
	 */
	if (UndoRecPtrIsValid(upd_undo_ptr))
		FluxPbuInsertUndo();

	/*
	 * WAL log the update with all overflow buffers (and folded undo)
	 * atomically
	 */
	if (RelationNeedsWAL(relation))
	{
		XLogRecPtr	upd_recptr;

		/*
		 * old_tuple_for_inplace_wal was populated with a copy of the old
		 * tuple data BEFORE we modified the page.  Use its saved
		 * old_commit_ts for the WAL record so the before-image is correct.
		 */
		upd_recptr = FluxXLogUpdate(relation, buffer, offnum,
									old_tuple_for_inplace_wal, new_tuple,
									old_tuple_for_inplace_wal->t_data->t_commit_ts,
									0,	/* new version: t_xmax = Invalid
										 * (heap-shaped) */
									&update_overflow_buffers,
									InvalidBuffer,
									&update_old_img, &update_new_img,
									!walLogical);

		if (UndoRecPtrIsValid(upd_undo_ptr))
			FluxPbuFinishUndo(upd_recptr);
	}
	else if (UndoRecPtrIsValid(upd_undo_ptr))
		FluxPbuFinishUndo(InvalidXLogRecPtr);

	FluxVMFinishClear(RelationNeedsWAL(relation) ?
					  PageGetLSN(page) : InvalidXLogRecPtr);
	END_CRIT_SECTION();
	if (BufferIsValid(vmbuffer))
		ReleaseBuffer(vmbuffer);

	FluxXLogReleaseLogicalImage(&update_old_img);
	FluxXLogReleaseLogicalImage(&update_new_img);

	/*
	 * Release all overflow buffers first; they were WAL-logged atomically
	 * above so they're safe to unlock now.
	 */
	for (upd_i = 0; upd_i < update_overflow_buffers.count; upd_i++)
	{
		Buffer		ovf_buf = update_overflow_buffers.buffers[upd_i].buffer;
		bool		already_released = (ovf_buf == buffer);
		int			dup_j;

		for (dup_j = 0; dup_j < upd_i && !already_released; dup_j++)
		{
			if (update_overflow_buffers.buffers[dup_j].buffer == ovf_buf)
				already_released = true;
		}

		if (!already_released)
			UnlockReleaseBuffer(ovf_buf);
		pfree(update_overflow_buffers.buffers[upd_i].record_data);
	}

	/* Capture free space while we still hold the main buffer lock. */
	{
		Size		update_free_space = PageGetHeapFreeSpace(page);

		/*
		 * sLog registration BEFORE buffer release; eliminates the race window
		 * where another backend reads the modified tuple but finds no sLog
		 * entry.  Safe: seqlock reads take no lock, no deadlock.
		 */
		FluxEnsureSLogCallbacks();
		/* BUG 5 fix: bounded local-only tracking (see flux_tuple_delete). */
		/* BUG 5: no per-tuple tracking for UPDATE (see flux_tuple_delete). */

		/* Record our own update's command id (cmax). */
		FluxLocalCidSet(RelationGetRelid(relation), &slot->tts_tid, cid);

		/* Release the main buffer lock; sLog registered, no race. */
		UnlockReleaseBuffer(buffer);

		FluxRecordFreeSpace(relation, blkno, update_free_space);
	}

	/*
	 * The per-backend UPDATE UNDO record was written and folded into the
	 * forward WAL record inside the critical section above (FluxPbuInsertUndo
	 * / FluxPbuFinishUndo).  Unlike the fork engine, the per-backend insert
	 * must happen inside the crit section so its pages ride the same WAL
	 * record; the record is chained into the xact automatically by
	 * InsertPreparedUndo (SetCurrentUndoLocation), so no explicit
	 * registration is needed.
	 */

	/*
	 * No index maintenance for this path.  Key-changing UPDATEs are routed
	 * out of place at the top of flux_tuple_update (delete + insert at a new
	 * TID, *update_indexes = TU_All); this in-place path runs only for
	 * non-indexed changes, so the TID is unchanged and every secondary (key,
	 * TID) entry still points at the live tuple.  *update_indexes was
	 * defaulted to TU_None at the top of the function, so no index re-insert
	 * happens.
	 */
	{
		FluxFreeTuple(old_tuple_for_inplace_wal);

		FluxLocalDirtyPageMark(relation, blkno);

		/*
		 * NOTE: We do NOT immediately clean up overflow chains here.
		 * Immediate cleanup was: 1. Buggy (collected wrong overflow pointers
		 * after in-place modification) 2. Expensive on hot paths (extra
		 * buffer I/O + locking during UPDATE) 3. Complex to WAL-log correctly
		 *
		 * Instead, overflow cleanup is deferred to VACUUM.  When VACUUM
		 * prunes deleted tuples, it will also reclaim orphaned overflow
		 * pages.
		 *
		 * Future enhancement: Log overflow block/offset in WAL UPDATE record
		 * so UNDO log pruning can also clean up overflow chains.
		 */
		(void) old_has_overflow;	/* Suppress unused variable warning */
	}

	FluxFreeTuple(new_tuple);

	/* Delete the superseded external values, free toasting temporaries. */
	flux_toast_cleanup(&upd_ttc);
	if (upd_old_copy)
		pfree(upd_old_copy);

	FLUX_RELEASE_TUPLOCK();

	flux_count_inplace_update(relation, key_changed);
	return TM_Ok;
}

#undef FLUX_RELEASE_TUPLOCK



/*
 * Multi-insert operation for bulk loading (batched page-at-a-time)
 *
 * Pre-forms all tuples, then inserts them page-at-a-time to minimize
 * per-tuple overhead: one FSM lookup, one buffer lock, one WAL record,
 * and one UNDO reservation per page batch instead of per tuple.
 *
 * Tuples that are too large for batch handling (need overflow) are
 * inserted individually via the single-insert path.
 */
void
flux_multi_insert(Relation relation, TupleTableSlot **slots, int ntuples,
				  CommandId cid, uint32 options, BulkInsertState bistate)
{
	FluxTuple  *formed_tuples;
	bool	   *needs_single_insert;
	uint64		current_ts;
	TransactionId my_xid;
	int			i;
	int			ndone;

	if (ntuples <= 0)
		return;

	/*
	 * Get timestamps and XID outside the loop; these are per-transaction
	 * cached values, but calling them once avoids function call overhead.
	 */
	(void) FluxGetTransactionTimestamp();
	current_ts = (uint64) FluxGetDmlTimestamp();
	my_xid = GetCurrentTransactionId(); /* subxid: heap-shaped xmin */
	/* Ensure relation storage exists */
	RelationGetSmgr(relation);

	/*
	 * Phase 1: Pre-form all tuples without overflow handling. Passing NULL
	 * for rel and overflow_buffers skips the overflow path, keeping the tuple
	 * inline.  Tuples that exceed the page size will be detected below and
	 * routed to single-insert.
	 */
	formed_tuples = (FluxTuple *) palloc(ntuples * sizeof(FluxTuple));
	needs_single_insert = (bool *) palloc0(ntuples * sizeof(bool));

	for (i = 0; i < ntuples; i++)
	{
		slot_getallattrs(slots[i]);
		formed_tuples[i] = FluxFormTuple(RelationGetDescr(relation),
										 slots[i]->tts_values,
										 slots[i]->tts_isnull,
										 NULL,	/* no overflow in batch */
										 NULL);

		/* Set MVCC fields (heap-shaped xmin/xmax) */
		formed_tuples[i]->t_data->t_commit_ts = 0;	/* t_xmax = Invalid */
		formed_tuples[i]->t_data->t_flags |= FLUX_TUPLE_UNCOMMITTED;
		formed_tuples[i]->t_data->t_xmin = my_xid;
		formed_tuples[i]->t_data->t_cmin = cid; /* see flux_tuple_insert */

		/*
		 * Reserve the version-pointer slot at INSERT time so the first UPDATE
		 * is a same-length overwrite (see flux_tuple_insert for the
		 * rationale).  Done before the size check below so oversize routing
		 * accounts for the base+8 on-page footprint.
		 */
		formed_tuples[i]->t_data->t_flags |= FLUX_TUPLE_HAS_VERSION_PTR;
		FluxTupleSetVersionPtr(formed_tuples[i]->t_data,
							   InvalidUndoRecPtr);

		/*
		 * Route a tuple that needs TOAST to the single-insert path, which
		 * toasts it through flux_toast_tuple: one holding an external value
		 * (possibly a pointer into another relation's TOAST table, e.g. from
		 * a column default) or one wider than heap's toasting threshold
		 * (heap_prepare_insert's test).  Tuples too large for the page go
		 * there too.
		 *
		 * ponytail: wide rows lose page batching; toast here and keep them in
		 * the batch if COPY of wide rows into FLUX matters.
		 */
		if ((formed_tuples[i]->t_data->t_infomask & FLUX_INFOMASK_HASEXTERNAL) ||
			formed_tuples[i]->t_len > TOAST_TUPLE_THRESHOLD ||
			formed_tuples[i]->t_len > FLUX_MAX_TUPLE_SIZE)
			needs_single_insert[i] = true;
	}

	/*
	 * Phase 2: Batch insert page-at-a-time.
	 *
	 * For each page: lock once, insert all fitting tuples, WAL-log once,
	 * unlock.  This is much faster than per-tuple buffer operations.
	 *
	 * We register each inserted tuple in the backend-local tracked-key list
	 * (via SLogTupleTrackLocalOnly) so that FluxClearUncommittedFlags() can
	 * find and stamp them at commit time.  Without this, COPY-inserted tuples
	 * retain FLUX_TUPLE_UNCOMMITTED permanently and are invisible to all
	 * snapshots.
	 */
	FluxEnsureSLogCallbacks();
	ndone = 0;
	while (ndone < ntuples)
	{
		Buffer		buffer;
		Page		page;
		BlockNumber target_block;
		int			batch_start;
		int			batch_count;
		int			nfit;
		Size		saveFreeSpace;
		Size		avail;
		OffsetNumber *offnums = NULL;
		FluxLogicalImage *logical_imgs = NULL;
		FluxLogicalImage combined_image = {NULL, 0};
		bool		need_logical = RelationIsLogicallyLogged(relation);
		Buffer		vmbuffer;

		/* Skip tuples that need single-insert (overflow) */
		if (needs_single_insert[ndone])
		{
			flux_tuple_insert(relation, slots[ndone], cid, options, bistate);
			FluxFreeTuple(formed_tuples[ndone]);
			ndone++;
			continue;
		}

		/* Find a page with space for at least one tuple (fill-factor aware) */
		saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
													   FLUX_DEFAULT_FILLFACTOR);
		/* Clamped request -- see FluxTargetFreeSpace in flux_fsm.c. */
		target_block = FluxGetPageWithFreeSpace(relation,
												FluxTargetFreeSpace(relation,
																	formed_tuples[ndone]->t_len));
		if (target_block == InvalidBlockNumber)
		{
			/* Fall back to single insert */
			flux_tuple_insert(relation, slots[ndone], cid, options, bistate);
			FluxFreeTuple(formed_tuples[ndone]);
			ndone++;
			continue;
		}

		buffer = ReadBuffer(relation, target_block);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);

		/*
		 * Determine how many tuples will fit on this page BEFORE entering the
		 * critical section.  Logical-decoding images and the offnums array
		 * must be allocated outside the crit section, so we need the count up
		 * front. This mirrors heap_multi_insert(): walk the pending tuples,
		 * subtracting each tuple's (line-pointer + aligned body) cost from
		 * the page's free space until the next one would not fit, honouring
		 * the fill-factor reserve and stopping at the first overflow tuple.
		 */
		avail = PageGetHeapFreeSpace(page);

		/*
		 * Honour the fill-factor reserve while PACKING, not just while
		 * choosing the target block.  FLUX has stable TIDs and updates rows
		 * in place, so a page packed 100%% full leaves no room for later
		 * in-place growth and a growing UPDATE (e.g. an accumulating numeric)
		 * aborts with "does not fit".  Reserve saveFreeSpace bytes here so
		 * bulk load (COPY / HammerDB) leaves the same headroom the
		 * single-insert path does.  The "always take at least one tuple" rule
		 * below still guarantees forward progress even when a single tuple
		 * exceeds the post-reserve budget.
		 */
		if (avail > saveFreeSpace)
			avail -= saveFreeSpace;
		nfit = 0;
		while (ndone + nfit < ntuples &&
			   !needs_single_insert[ndone + nfit])
		{
			Size		need = sizeof(ItemIdData) +
				MAXALIGN(formed_tuples[ndone + nfit]->t_len);

			/* Always take at least one tuple; stop when the next won't fit */
			if (nfit > 0 && need > avail)
				break;
			avail -= need;
			nfit++;
		}
		Assert(nfit > 0);

		offnums = (OffsetNumber *) palloc(nfit * sizeof(OffsetNumber));
		if (need_logical)
		{
			Size		total = 0;
			char	   *p;

			logical_imgs = (FluxLogicalImage *)
				palloc(nfit * sizeof(FluxLogicalImage));
			for (i = 0; i < nfit; i++)
				FluxXLogPrepareLogicalImage(relation,
											formed_tuples[ndone + i],
											&logical_imgs[i]);

			/*
			 * Serialize every image into one contiguous blob, framed exactly
			 * as FluxXLogRegisterLogicalImage emits a single image: each
			 * frame is "[heap bytes][uint32 len]" in tuple order.  The
			 * multi-insert emitter registers this blob as a single rdata
			 * chunk, keeping the registered-data slot count constant
			 * regardless of batch size (the old per-tuple registration
			 * overflowed XLR_NORMAL_RDATAS on dense pages).  We size for all
			 * nfit images but register only the prefix covering the tuples
			 * actually inserted (see below), matching the ntuples trailing
			 * frames the redo/decode readers expect.
			 */
			for (i = 0; i < nfit; i++)
				if (logical_imgs[i].data != NULL)
					total += logical_imgs[i].len + sizeof(uint32);
			if (total > 0)
			{
				combined_image.data = (char *) palloc(total);
				p = combined_image.data;
				for (i = 0; i < nfit; i++)
				{
					if (logical_imgs[i].data == NULL)
						continue;
					memcpy(p, logical_imgs[i].data, logical_imgs[i].len);
					p += logical_imgs[i].len;
					memcpy(p, &logical_imgs[i].len, sizeof(uint32));
					p += sizeof(uint32);
				}
			}
		}

		/*
		 * Ensure the current transaction has an XID assigned BEFORE entering
		 * the critical section (GetCurrentTransactionId may lock and
		 * allocate, both forbidden in a crit section).  An assigned XID is
		 * required for correctness: a multi-insert WAL record without an
		 * attached xid cannot be decoded into a logical replication stream
		 * (ReorderBuffer groups changes by xid and asserts the xid is valid),
		 * and a WAL-emitting transaction would otherwise be treated as
		 * read-only.  This mirrors flux_tuple_insert() and
		 * heap_multi_insert().
		 */
		if (RelationNeedsWAL(relation))
			(void) GetCurrentTransactionId();

		/* Pre-allocate WAL space outside critical section */
		if (RelationNeedsWAL(relation))
			XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID, 4);

		/* As for a single insert, only relation-level readers conflict. */
		CheckForSerializableConflictIn(relation, NULL, InvalidBlockNumber);

		vmbuffer = FluxVMPinForChange(relation, buffer);

		START_CRIT_SECTION();

		batch_start = ndone;
		batch_count = 0;

		/* Insert the tuples we sized for above */
		while (batch_count < nfit)
		{
			OffsetNumber offnum;
			ItemId		inserted_itemid;
			FluxTupleHeader *inserted_hdr;

			offnum = FluxPageAddTuple(page, formed_tuples[ndone],
									  formed_tuples[ndone]->t_len);
			if (offnum == InvalidOffsetNumber)
				break;			/* page is full, stop batching */

			/* Set TID in slot */
			ItemPointerSet(&slots[ndone]->tts_tid, target_block, offnum);
			formed_tuples[ndone]->t_self = slots[ndone]->tts_tid;
			slots[ndone]->tts_tableOid = RelationGetRelid(relation);
			/* Set t_ctid to self */
			inserted_itemid = PageGetItemId(page, offnum);
			inserted_hdr = (FluxTupleHeader *) PageGetItem(page, inserted_itemid);
			ItemPointerSet(&inserted_hdr->t_ctid, target_block, offnum);

			offnums[batch_count] = offnum;
			batch_count++;
			ndone++;
		}

		if (batch_count > 0)
		{
			/* Update page opaque fields */
			FluxPageOpaque phdr = FluxPageGetOpaque(page);

			FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), current_ts));

			FluxVMClearForChange(buffer, vmbuffer);
			MarkBufferDirty(buffer);

			/*
			 * WAL-log the whole batch in one record.  Every tuple body is
			 * logged individually (no forced full-page image), so recovery is
			 * crash-safe even with full_page_writes=off, and one logical-
			 * decoding image per tuple is appended for logically-logged
			 * relations so COPY'd rows replicate.
			 */
			if (RelationNeedsWAL(relation))
			{
				XLogRecPtr	recptr;
				FluxLogicalImage *emit_image = NULL;

				/*
				 * Register only the framed-image prefix covering the tuples
				 * we actually inserted.  batch_count can be < nfit if the
				 * page filled mid-loop, and the redo/decode readers walk
				 * exactly ntuples (== batch_count) trailing frames, so a
				 * longer blob would misalign their backward walk.
				 */
				if (combined_image.data != NULL)
				{
					Size		prefix = 0;

					for (i = 0; i < batch_count; i++)
						if (logical_imgs[i].data != NULL)
							prefix += logical_imgs[i].len + sizeof(uint32);
					combined_image.len = (uint32) prefix;
					if (prefix > 0)
						emit_image = &combined_image;
				}

				recptr = FluxXLogMultiInsert(relation, buffer,
											 offnums,
											 &formed_tuples[batch_start],
											 batch_count, current_ts,
											 emit_image);
				PageSetLSN(page, recptr);
			}
		}

		FluxVMFinishClear(batch_count > 0 && RelationNeedsWAL(relation) ?
						  PageGetLSN(page) : InvalidXLogRecPtr);
		END_CRIT_SECTION();
		if (BufferIsValid(vmbuffer))
			ReleaseBuffer(vmbuffer);

		if (logical_imgs != NULL)
		{
			for (i = 0; i < nfit; i++)
				FluxXLogReleaseLogicalImage(&logical_imgs[i]);
			pfree(logical_imgs);
		}
		if (combined_image.data != NULL)
			pfree(combined_image.data);
		pfree(offnums);

		/*
		 * Register each inserted tuple in the backend-local tracked-key list.
		 * This is lightweight (no shared hash entry, no LWLock); just a
		 * palloc'd linked-list node per tuple.  FluxClearUncommittedFlags()
		 * iterates this list at PRE_COMMIT to clear the UNCOMMITTED flag,
		 * making the tuples visible.
		 *
		 * We skip full sLog registration (shared hash) because COPY doesn't
		 * use SNAPSHOT_DIRTY or ON CONFLICT, and the local tracking is
		 * sufficient for commit-time processing.
		 */
		{
			TransactionId xid = GetTopTransactionId();
			TransactionId subxid = GetCurrentSubTransactionId();

			for (i = batch_start; i < batch_start + batch_count; i++)
			{
				SLogTupleTrackLocalOnly(RelationGetRelid(relation),
										relation->rd_locator.relNumber,
										&slots[i]->tts_tid,
										xid, subxid);
				FluxSlotSetVersion(slots[i], my_xid, cid);
			}
		}

		/* Update FSM with remaining free space */
		FluxRecordFreeSpace(relation, target_block, PageGetHeapFreeSpace(page));


		UnlockReleaseBuffer(buffer);

		/*
		 * Count the batched inserts here rather than once for all ntuples at
		 * the end: tuples routed to the single-insert fallback are already
		 * counted by their own flux_tuple_insert call, so counting the whole
		 * ntuples at the tail would double-count them.
		 */
		pgstat_count_heap_insert(relation, batch_count);

		/* Free formed tuples in this batch */
		for (i = batch_start; i < batch_start + batch_count; i++)
			FluxFreeTuple(formed_tuples[i]);
	}

	pfree(formed_tuples);
	pfree(needs_single_insert);
}

/*
 * Per-stream state for the Phase I VACUUM scan callback.
 */
typedef struct FluxVacScanState
{
	Relation	rel;
	BlockNumber current_block;	/* last block handed out (InvalidBlockNumber
								 * before the first call) */
	BlockNumber nblocks;
	bool		skip_all_frozen;	/* honor the VM ALL_FROZEN skip? */
} FluxVacScanState;

/*
 * Read stream callback for VACUUM Phase I.
 *
 * Hands the read stream the next block that actually needs scanning so the
 * upcoming pages are prefetched under AIO, matching heapam's
 * heap_vac_scan_next_block().  Pages marked ALL_FROZEN in the visibility map
 * are skipped here (unless DISABLE_PAGE_SKIPPING was requested), so the
 * stream never issues I/O for pages VACUUM would immediately discard.
 */
static BlockNumber
flux_vac_scan_next_block(ReadStream *stream,
						 void *callback_private_data,
						 void *per_buffer_data)
{
	FluxVacScanState *state = callback_private_data;
	BlockNumber next_block = state->current_block + 1;

	for (; next_block < state->nblocks; next_block++)
	{
		CHECK_FOR_INTERRUPTS();

		if (state->skip_all_frozen &&
			FluxVMCheck(state->rel, next_block, FLUX_VM_ALL_FROZEN))
			continue;

		state->current_block = next_block;
		return next_block;
	}

	state->current_block = state->nblocks;
	return InvalidBlockNumber;
}

/* Truncation lock-acquisition tuning, mirroring heap's lazy_truncate_heap. */
#define FLUX_TRUNCATE_LOCK_WAIT_INTERVAL	50	/* ms */
#define FLUX_TRUNCATE_LOCK_TIMEOUT			5000	/* ms */

/*
 * FluxPageIsEmpty
 *
 * A FLUX page is truncatable-empty when every line pointer is LP_UNUSED.
 * Pruning leaves removed ItemIds LP_UNUSED without resetting pd_lower, so
 * PageGetMaxOffsetNumber() alone would call such a page non-empty.  An
 * LP_DEAD slot keeps the page: an index may still name its TID.
 */
static bool
FluxPageIsEmpty(Page page)
{
	OffsetNumber maxoff;

	if (PageIsNew(page))
		return true;

	maxoff = PageGetMaxOffsetNumber(page);
	for (OffsetNumber offnum = FirstOffsetNumber; offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		/* an LP_DEAD slot keeps the page: index entries may still name it */
		if (ItemIdIsUsed(PageGetItemId(page, offnum)))
			return false;
	}

	return true;
}

/* Truncation thresholds and lock-waiter check interval, as in vacuumlazy.c */
#define FLUX_REL_TRUNCATE_MINIMUM	1000
#define FLUX_REL_TRUNCATE_FRACTION	16
#define FLUX_TRUNCATE_LOCK_CHECK_INTERVAL	20	/* ms */

/*
 * FluxShouldAttemptTruncation
 *
 * Truncate only if allowed (VACUUM (TRUNCATE), the vacuum_truncate reloption
 * and GUC, resolved into params->truncate) and if the empty tail is worth an
 * AccessExclusiveLock, which a hot standby must replay: at least
 * FLUX_REL_TRUNCATE_MINIMUM pages or 1/FLUX_REL_TRUNCATE_FRACTION of the
 * relation, as heap's should_attempt_truncation does.
 */
static bool
FluxShouldAttemptTruncation(const VacuumParams *params, BlockNumber nblocks,
							BlockNumber empty_end_pages)
{
	if (params->truncate == VACOPTVALUE_DISABLED)
		return false;

	return empty_end_pages > 0 &&
		(empty_end_pages >= FLUX_REL_TRUNCATE_MINIMUM ||
		 empty_end_pages >= nblocks / FLUX_REL_TRUNCATE_FRACTION);
}

/*
 * FluxCountNondeletablePages
 *
 * Scan backwards from nblocks, but not below desired_nblocks, and return one
 * past the last page that is not empty: the size to truncate to.  The caller
 * holds AccessExclusiveLock, so no backend adds a row behind the scan.  The
 * scan that found the empty tail ran under ShareUpdateExclusiveLock, which
 * INSERT does not conflict with, so this recount is necessary.
 *
 * As heap's count_nondeletable_pages does, check every 32 blocks, at most
 * once per FLUX_TRUNCATE_LOCK_CHECK_INTERVAL ms, whether another backend waits
 * for the lock.  If one does, set *lock_waiter_detected and return the size
 * verified so far, so that the caller truncates that much and lets it in.
 */
static BlockNumber
FluxCountNondeletablePages(Relation onerel, BlockNumber nblocks,
						   BlockNumber desired_nblocks,
						   bool *lock_waiter_detected)
{
	BlockNumber blkno = nblocks;
	instr_time	starttime;

	INSTR_TIME_SET_CURRENT(starttime);

	while (blkno > desired_nblocks)
	{
		Buffer		buf;
		bool		empty;

		if ((blkno % 32) == 0)
		{
			instr_time	currenttime;
			instr_time	elapsed;

			INSTR_TIME_SET_CURRENT(currenttime);
			elapsed = currenttime;
			INSTR_TIME_SUBTRACT(elapsed, starttime);
			if ((INSTR_TIME_GET_MICROSEC(elapsed) / 1000)
				>= FLUX_TRUNCATE_LOCK_CHECK_INTERVAL)
			{
				if (LockHasWaitersRelation(onerel, AccessExclusiveLock))
				{
					ereport(DEBUG2,
							(errmsg("table \"%s\": suspending truncate due to conflicting lock request",
									RelationGetRelationName(onerel))));
					*lock_waiter_detected = true;
					return blkno;
				}
				starttime = currenttime;
			}
		}

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(onerel, blkno - 1);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		empty = FluxPageIsEmpty(BufferGetPage(buf));
		UnlockReleaseBuffer(buf);

		if (!empty)
			break;

		blkno--;
	}

	return blkno;
}

/*
 * FluxTruncateRelation
 *
 * Truncate the empty tail VACUUM found, to no fewer than desired_nblocks
 * pages, with heap's lazy_truncate_heap protocol: take AccessExclusiveLock
 * without waiting for it (retrying every FLUX_TRUNCATE_LOCK_WAIT_INTERVAL ms
 * for up to FLUX_TRUNCATE_LOCK_TIMEOUT ms, then giving up), give up if the
 * relation grew, and recount the empty tail under the lock.  If a backend
 * starts waiting for the lock during the recount, truncate what was
 * verified, release the lock and start over.
 */
static BlockNumber
FluxTruncateRelation(Relation onerel, BlockNumber orig_nblocks,
					 BlockNumber desired_nblocks, bool verbose)
{
	BlockNumber start_nblocks = orig_nblocks;
	BlockNumber new_nblocks = orig_nblocks;
	bool		lock_waiter_detected;

	do
	{
		int			lock_retry = 0;

		lock_waiter_detected = false;
		while (!ConditionalLockRelation(onerel, AccessExclusiveLock))
		{
			CHECK_FOR_INTERRUPTS();

			if (++lock_retry > (FLUX_TRUNCATE_LOCK_TIMEOUT /
								FLUX_TRUNCATE_LOCK_WAIT_INTERVAL))
			{
				ereport(verbose ? INFO : DEBUG2,
						(errmsg("\"%s\": stopping truncate due to conflicting lock request",
								RelationGetRelationName(onerel))));
				return start_nblocks - orig_nblocks;
			}

			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 FLUX_TRUNCATE_LOCK_WAIT_INTERVAL,
							 WAIT_EVENT_VACUUM_TRUNCATE);
			ResetLatch(MyLatch);
		}

		/* New pages presumably hold rows: give up, as heap does */
		if (RelationGetNumberOfBlocks(onerel) != orig_nblocks)
		{
			UnlockRelation(onerel, AccessExclusiveLock);
			return start_nblocks - orig_nblocks;
		}

		new_nblocks = FluxCountNondeletablePages(onerel, orig_nblocks,
												 desired_nblocks,
												 &lock_waiter_detected);
		if (new_nblocks >= orig_nblocks)
		{
			UnlockRelation(onerel, AccessExclusiveLock);
			return start_nblocks - orig_nblocks;
		}

		RelationTruncate(onerel, new_nblocks);

		/*
		 * Other backends process the smgr invalidation smgrtruncate sent when
		 * they next lock the relation, so the lock can go now.
		 */
		UnlockRelation(onerel, AccessExclusiveLock);

		ereport(verbose ? INFO : DEBUG2,
				(errmsg("table \"%s\": truncated %u to %u pages",
						RelationGetRelationName(onerel),
						orig_nblocks, new_nblocks)));
		orig_nblocks = new_nblocks;
	} while (new_nblocks > desired_nblocks && lock_waiter_detected);

	return start_nblocks - new_nblocks;
}

/*
 * May VACUUM free this line pointer?  Only once Phase II has removed the
 * index entries naming it, that is, if Phase I recorded it.  A row that died
 * or was pruned after Phase I stays LP_DEAD for the next VACUUM.  Without
 * indexes (no dead_items) nothing names it.
 */
static bool
FluxVacuumMayFree(TidStore *dead_items, BlockNumber blkno, OffsetNumber offnum)
{
	ItemPointerData tid;

	if (dead_items == NULL)
		return true;
	ItemPointerSet(&tid, blkno, offnum);
	return TidStoreIsMember(dead_items, &tid);
}

/*
 * Reuse writer helping to clear aborted UPDATE/DELETE heads before freezing.
 * A pending worker's later apply is idempotent.  Finish outside the content
 * lock, retaining our pin, and recheck the page after any lock release.
 */
static void
flux_vacuum_revert_page(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	for (OffsetNumber off = FirstOffsetNumber; off <= PageGetMaxOffsetNumber(page); off++)
	{
		ItemId		item = PageGetItemId(page, off);

		if (!ItemIdIsNormal(item) ||
			FluxIsOverflowRecordInline(PageGetItem(page, item), ItemIdGetLength(item)))
			continue;
		if (FluxRevertAbortedVersions(rel, buf, off))
			off = InvalidOffsetNumber;
	}
}

/* Freeze a page under its exclusive content lock, before publishing horizons. */
static void
flux_vacuum_freeze_page(Relation rel, Buffer buf, const VacuumCutoffs *cutoffs,
						TransactionId *new_frozen)
{
	Page		page = BufferGetPage(buf);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	xl_flux_freeze_tuple plans[MaxOffsetNumber];
	int			nplans = 0;
	TransactionId conflict_xid = InvalidTransactionId;

	memset(plans, 0, sizeof(plans));
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		item = PageGetItemId(page, off);
		FluxTupleHeader *tuple;
		TransactionId xmin;
		TransactionId xmax;
		uint8		actions = 0;

		if (!ItemIdIsNormal(item))
			continue;
		tuple = (FluxTupleHeader *) PageGetItem(page, item);
		if (FluxIsOverflowRecordInline(tuple, ItemIdGetLength(item)))
			continue;
		xmin = FluxTupleGetXmin(tuple);
		xmax = FluxTupleGetXmax(tuple);
		/* A pending DELETE rollback owns its chain: leave it to helping. */
		if (TransactionIdIsNormal(xmax) &&
			!(tuple->t_flags & FLUX_TUPLE_DELETED) &&
			!TransactionIdIsInProgress(xmax) && !TransactionIdDidCommit(xmax))
			actions |= FLUX_FREEZE_XMAX;
		if (TransactionIdIsNormal(xmin) &&
			TransactionIdPrecedes(xmin, cutoffs->FreezeLimit) &&
			!(tuple->t_flags & FLUX_TUPLE_SPECULATIVE) &&
			TransactionIdDidCommit(xmin))
		{
			actions |= FLUX_FREEZE_XMIN;
			if (!TransactionIdIsValid(conflict_xid) ||
				TransactionIdFollows(xmin, conflict_xid))
				conflict_xid = xmin;
		}
		/* Only a universally visible live head can discard all history. */
		if ((actions & FLUX_FREEZE_XMIN || xmin == FrozenTransactionId) &&
			!(tuple->t_flags & FLUX_TUPLE_DELETED) &&
			(!TransactionIdIsValid(xmax) || actions & FLUX_FREEZE_XMAX))
			actions |= FLUX_FREEZE_DETACH;
		if (actions != 0 &&
			(actions != FLUX_FREEZE_DETACH || UndoRecPtrIsValid(tuple->t_verptr)))
		{
			plans[nplans].offset = off;
			plans[nplans++].actions = actions;
		}
	}
	if (nplans > 0)
	{
		START_CRIT_SECTION();
		FluxExecuteFreeze(buf, plans, nplans);
		MarkBufferDirty(buf);
		if (RelationNeedsWAL(rel))
			PageSetLSN(page, FluxXLogFreeze(rel, buf, plans, nplans, conflict_xid));
		END_CRIT_SECTION();
	}
	/* Include legacy and own-transaction before-images, not only the head. */
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		item = PageGetItemId(page, off);
		ItemPointerData tid;

		if (!ItemIdIsNormal(item) ||
			FluxIsOverflowRecordInline(PageGetItem(page, item), ItemIdGetLength(item)))
			continue;
		ItemPointerSet(&tid, BufferGetBlockNumber(buf), off);
		*new_frozen = FluxVersionChainOldestXid(rel, &tid,
												(const char *) PageGetItem(page, item), ItemIdGetLength(item),
												*new_frozen);
	}
}

/*
 * Run one index's ambulkdelete (dead_items != NULL) or amvacuumcleanup,
 * reporting the index to pg_stat_progress_vacuum as heap's VACUUM does.
 */
static IndexBulkDeleteResult *
flux_vacuum_one_index(Relation indrel, Relation heaprel,
					  IndexBulkDeleteResult *istat, double num_heap_tuples,
					  bool estimated_count, bool verbose,
					  BufferAccessStrategy bstrategy, TidStore *dead_items,
					  VacDeadItemsInfo *dead_items_info)
{
	IndexVacuumInfo ivinfo;
	const int	reset_index[] = {
		PROGRESS_VACUUM_CURRENT_INDEX_RELID,
		PROGRESS_SCAN_BLOCKS_TOTAL,
		PROGRESS_SCAN_BLOCKS_DONE
	};
	const int64 reset_val[] = {(int64) InvalidOid, 0, 0};

	ivinfo.index = indrel;
	ivinfo.heaprel = heaprel;
	ivinfo.analyze_only = false;
	ivinfo.is_autovacuum = AmAutoVacuumWorkerProcess();
	ivinfo.report_progress = true;
	ivinfo.estimated_count = estimated_count;
	ivinfo.message_level = verbose ? INFO : DEBUG2;
	ivinfo.num_heap_tuples = num_heap_tuples;
	ivinfo.strategy = bstrategy;

	pgstat_progress_update_param(PROGRESS_VACUUM_CURRENT_INDEX_RELID,
								 (int64) RelationGetRelid(indrel));
	if (dead_items != NULL)
		istat = vac_bulkdel_one_index(&ivinfo, istat, dead_items,
									  dead_items_info);
	else
		istat = vac_cleanup_one_index(&ivinfo, istat);
	pgstat_progress_update_multi_param(3, reset_index, reset_val);

	return istat;
}

/*
 * Run ambulkdelete (dead_items != NULL) or amvacuumcleanup on all indexes
 * with parallel workers, as heap's VACUUM does.  Heap keeps its parallel
 * context for the whole VACUUM; FLUX creates one per index pass, because
 * its table phases write in ways parallel mode forbids (Phase III deletes
 * dead rows' TOAST values, and reverting an aborted writer's versions can
 * write).  indstats carries the index statistics across passes.  The dead
 * TIDs are copied into the shared TidStore the workers read.
 *
 * ponytail: the copy holds the round's TIDs twice during the pass; collect
 * into the shared TidStore directly if that memory matters.
 *
 * Returns false, doing nothing, if no worker would be used.
 */
static bool
flux_vacuum_indexes_parallel(Relation onerel, Relation *indrels, int nindexes,
							 IndexBulkDeleteResult **indstats, int nworkers,
							 bool verbose, BufferAccessStrategy bstrategy,
							 TidStore *dead_items,
							 VacDeadItemsInfo *dead_items_info,
							 double num_heap_tuples, bool estimated_count,
							 int num_index_scans)
{
	ParallelVacuumState *pvs;

	pvs = parallel_vacuum_init(onerel, indrels, nindexes, nworkers,
							   maintenance_work_mem, verbose ? INFO : DEBUG2,
							   bstrategy);
	if (pvs == NULL)
		return false;
	parallel_vacuum_set_index_stats(pvs, indstats);

	if (dead_items != NULL)
	{
		VacDeadItemsInfo *shared_info;
		TidStore   *shared_items = parallel_vacuum_get_dead_items(pvs,
																  &shared_info);
		TidStoreIter *iter = TidStoreBeginIterate(dead_items);
		TidStoreIterResult *res;
		OffsetNumber offsets[MaxOffsetNumber];

		while ((res = TidStoreIterateNext(iter)) != NULL)
		{
			int			n = TidStoreGetBlockOffsets(res, offsets,
													lengthof(offsets));

			Assert(n <= lengthof(offsets));
			TidStoreSetBlockOffsets(shared_items, res->blkno, offsets, n);
		}
		TidStoreEndIterate(iter);
		shared_info->num_items = dead_items_info->num_items;

		parallel_vacuum_bulkdel_all_indexes(pvs, (long) num_heap_tuples,
											num_index_scans, NULL);
	}
	else
		parallel_vacuum_cleanup_all_indexes(pvs, (long) num_heap_tuples,
											num_index_scans, estimated_count,
											NULL);

	/* Copies the statistics back and leaves parallel mode. */
	parallel_vacuum_end(pvs, indstats);
	return true;
}

/*
 * Vacuum a FLUX relation
 *
 * This performs garbage collection on a FLUX table in multiple phases:
 *
 * Phase I:    Scan all pages, identify dead tuples, collect their TIDs
 * Phase II:   Remove dead index entries using the collected TIDs
 * Phase III:  Defragment data pages to reclaim space (must happen AFTER
 *             index cleanup to avoid dangling index pointers)
 * Phase IV:   Post-vacuum index cleanup (amvacuumcleanup)
 * Phase V:    Truncate trailing empty pages, update FSM
 */
void
flux_relation_vacuum(Relation onerel, const VacuumParams *params,
					 BufferAccessStrategy bstrategy)
{
	BlockNumber nblocks;
	BlockNumber blkno;
	Buffer		buf;
	Page		page;
	uint64		oldest_ts;
	TransactionId oldest_xmin_vac;
	VacuumCutoffs cutoffs;
	bool		aggressive;
	TransactionId new_frozen;
	TimestampTz starttime = GetCurrentTimestamp();
	BlockNumber all_visible_pages;
	BlockNumber all_frozen_pages;
	int64		remaining_dead = 0;
	int64		num_tuples = 0;
	int64		dead_tuples = 0;
	int64		recently_dead_tuples = 0;
	int64		live_tuples = 0;
	int64		pages_vacuumed = 0;
	BlockNumber empty_end_pages = 0;
	BlockNumber pages_truncated = 0;
	bool		verbose = (params->options & VACOPT_VERBOSE) != 0;

	/* Index cleanup state */
	Relation   *indrels = NULL;
	int			nindexes = 0;
	IndexBulkDeleteResult **indstats = NULL;
	TidStore   *dead_items = NULL;
	VacDeadItemsInfo *dead_items_info = NULL;
	bool		do_index_cleanup;
	int			num_index_scans = 0;
	int			nworkers = -1;	/* parallel index vacuum: < 0 is off */

	/* Rounds: Phases I-III over blocks round_start .. round_end - 1 */
	FluxVacScanState scan_state;
	BlockNumber round_start = 0;
	BlockNumber round_end;
	bool		round_full;
	int64		scan_live_tuples = 0;	/* Phase I's count, for Phase II */

	/*
	 * Report to pg_stat_progress_vacuum at the points heap's VACUUM does.
	 * Phase I is "scanning heap", Phase II "vacuuming indexes", Phase III
	 * "vacuuming heap" (it visits every page, so heap_blks_vacuumed advances
	 * page by page), Phase IV "cleaning up indexes", Phase V "truncating
	 * heap".
	 */
	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM,
								  RelationGetRelid(onerel));
	if (AmAutoVacuumWorkerProcess())
		pgstat_progress_update_param(PROGRESS_VACUUM_STARTED_BY,
									 params->is_wraparound
									 ? PROGRESS_VACUUM_STARTED_BY_AUTOVACUUM_WRAPAROUND
									 : PROGRESS_VACUUM_STARTED_BY_AUTOVACUUM);
	else
		pgstat_progress_update_param(PROGRESS_VACUUM_STARTED_BY,
									 PROGRESS_VACUUM_STARTED_BY_MANUAL);

	/*
	 * Initialize FLUX transaction state so that VACUUM's own start timestamp
	 * is registered in xact_start_ts_slots.  Without this,
	 * FluxGetOldestActiveTimestamp() would see no active transactions and
	 * fall back to the current wall clock (GetCurrentTimestamp()), which for
	 * the VM all-visible hint would over-eagerly mark tuples all-visible.
	 * (The reclamation gate itself is the XID horizon below, not this ts.)
	 */
	(void) FluxGetTransactionTimestamp();

	/*
	 * Get the oldest active transaction's start timestamp.  Deleted tuples
	 * whose commit timestamp is older than this are no longer visible to any
	 * running transaction and can safely be removed.
	 *
	 * Previously this called FluxGetCommitTimestamp() which returns (and
	 * advances) the current wall-clock time.  That was wrong: it made VACUUM
	 * consider almost all deleted tuples as reclaimable, even those still
	 * needed by long-running concurrent transactions.
	 */
	oldest_ts = FluxGetOldestActiveTimestamp();
	oldest_xmin_vac = FluxGetOldestXminHorizon(onerel);
	aggressive = vacuum_get_cutoffs(onerel, params, &cutoffs);
	if (params->options & VACOPT_DISABLE_PAGE_SKIPPING)
		aggressive = true;
	new_frozen = cutoffs.OldestXmin;
	nblocks = RelationGetNumberOfBlocks(onerel);
	pgstat_progress_update_param(PROGRESS_VACUUM_MODE,
								 aggressive
								 ? PROGRESS_VACUUM_MODE_AGGRESSIVE
								 : PROGRESS_VACUUM_MODE_NORMAL);

	if (verbose)
		ereport(INFO, (errmsg("vacuuming \"%s\": scanning %u pages",
							  RelationGetRelationName(onerel), nblocks)));

	/*
	 * Open all indexes on the relation. We need RowExclusiveLock to prevent
	 * concurrent index modifications during vacuum.
	 */
	vac_open_indexes(onerel, RowExclusiveLock, &nindexes, &indrels);
	do_index_cleanup = (nindexes > 0);

	/*
	 * Vacuum the indexes in parallel when heap would: VACUUM (PARALLEL n)
	 * with n > 0 asks for up to n workers, a default VACUUM lets
	 * parallel_vacuum_init choose, and PARALLEL 0 sets params->nworkers to
	 * -1.  Each index is vacuumed by one process, so it takes two indexes.
	 */
	if (params->nworkers >= 0 && nindexes > 1)
	{
		/* Parallel workers cannot access temporary tables. */
		if (RelationUsesLocalBuffers(onerel))
		{
			if (params->nworkers > 0)
				ereport(WARNING,
						(errmsg("disabling parallel option of vacuum on \"%s\" --- cannot vacuum temporary tables in parallel",
								RelationGetRelationName(onerel))));
		}
		else
			nworkers = params->nworkers;
	}

	/*
	 * Allocate TidStore for collecting dead tuple TIDs, and per-index stats
	 * array. We use maintenance_work_mem as the budget for the TidStore.
	 */
	if (do_index_cleanup)
	{
		dead_items_info = (VacDeadItemsInfo *) palloc0(sizeof(VacDeadItemsInfo));
		dead_items_info->max_bytes = (size_t) maintenance_work_mem * 1024;
		dead_items_info->num_items = 0;

		dead_items = TidStoreCreateLocal(dead_items_info->max_bytes, true);

		indstats = (IndexBulkDeleteResult **)
			palloc0(nindexes * sizeof(IndexBulkDeleteResult *));
	}

	{
		const int	prog_index[] = {
			PROGRESS_VACUUM_PHASE,
			PROGRESS_VACUUM_TOTAL_HEAP_BLKS,
			PROGRESS_VACUUM_MAX_DEAD_TUPLE_BYTES
		};
		int64		prog_val[3];

		prog_val[0] = PROGRESS_VACUUM_PHASE_SCAN_HEAP;
		prog_val[1] = nblocks;
		prog_val[2] = dead_items_info ? dead_items_info->max_bytes : 0;
		pgstat_progress_update_multi_param(3, prog_index, prog_val);
	}

	/*
	 * -----------------------------------------------------------------------
	 * Phase I: Scan all pages, identify dead tuples, collect TIDs
	 *
	 * We scan every page and classify each tuple as live, dead (removable),
	 * or recently dead (not yet removable).  Dead tuple TIDs are recorded in
	 * the TidStore for later index cleanup.  We do NOT defragment pages yet
	 * -- that must wait until after index entries pointing to dead tuples
	 * have been removed (Phase II), to avoid dangling index pointers.
	 * -----------------------------------------------------------------------
	 */
	scan_state.rel = onerel;
	scan_state.current_block = InvalidBlockNumber;
	scan_state.nblocks = nblocks;
	scan_state.skip_all_frozen =
		!(params->options & VACOPT_DISABLE_PAGE_SKIPPING);

	/*
	 * When dead_items outgrows maintenance_work_mem, Phase I stops, Phases II
	 * and III run for the blocks scanned so far, and the next round scans on
	 * from there with an empty dead_items, as heap's VACUUM does.  Phase III
	 * frees only TIDs of the current round (FluxVacuumMayFree), whose index
	 * entries that round's Phase II removed.
	 */
next_round:
	round_full = false;
	blkno = InvalidBlockNumber;
	{
		ReadStream *scan_stream;

		scan_stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
												 READ_STREAM_USE_BATCHING,
												 bstrategy,
												 onerel,
												 MAIN_FORKNUM,
												 flux_vac_scan_next_block,
												 &scan_state,
												 0);

		for (;;)
		{
			OffsetNumber offnum,
						maxoffnum;
			ItemId		itemid;
			OffsetNumber dead_offsets[MaxOffsetNumber];
			int			ndead_on_page = 0;

			/* Cost-based delay, before pinning the next page. */
			vacuum_delay_point(false);

			/* Out of room for dead TIDs: end the round here. */
			if (dead_items != NULL && dead_items_info->num_items > 0 &&
				TidStoreMemoryUsage(dead_items) > dead_items_info->max_bytes)
			{
				round_full = true;
				break;
			}
			buf = read_stream_next_buffer(scan_stream, NULL);
			if (!BufferIsValid(buf))
				break;

			blkno = BufferGetBlockNumber(buf);
			pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED,
										 blkno);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);

			/* Skip if page is new/uninitialized */
			if (PageIsNew(page))
			{
				UnlockReleaseBuffer(buf);
				continue;
			}

			flux_vacuum_revert_page(onerel, buf);
			maxoffnum = PageGetMaxOffsetNumber(page);

			/* Scan all tuples on the page */
			for (offnum = FirstOffsetNumber; offnum <= maxoffnum; offnum++)
			{
				FluxTupleHeader *tuple_hdr;

				itemid = PageGetItemId(page, offnum);

				/*
				 * LP_DEAD line pointers were produced by opportunistic
				 * pruning (FluxPagePruneOpt / UPDATE defrag-fit), which
				 * reclaims a committed-deleted tuple's storage but reserves
				 * its TID so it is not recycled while index entries still
				 * reference it.  VACUUM must record these TIDs for index
				 * cleanup (Phase II) before Phase III converts them to
				 * LP_UNUSED.  Their storage is already gone, so there is no
				 * tuple header to inspect.
				 */
				if (ItemIdIsDead(itemid))
				{
					if (do_index_cleanup)
					{
						dead_offsets[ndead_on_page++] = offnum;
						dead_tuples++;
					}
					continue;
				}

				/* Skip if not a normal tuple */
				if (!ItemIdIsNormal(itemid))
					continue;

				tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

				/* Skip overflow records - they are managed by tuple lifecycle */
				if (FluxIsOverflowRecordInline(tuple_hdr, ItemIdGetLength(itemid)))
					continue;

				num_tuples++;

				/*
				 * Check if tuple is deleted and old enough to be removed.
				 *
				 * With the sLog-based MVCC model, a deleted tuple can be
				 * vacuumed if: - FLUX_TUPLE_DELETED is set -
				 * FLUX_TUPLE_UNCOMMITTED is NOT set (committed delete) -
				 * commit_ts is older than the oldest active snapshot
				 *
				 * If UNCOMMITTED is still set, the deleting transaction is
				 * still in progress (or aborted but not yet cleaned up by the
				 * sLog callback).  Skip it.
				 */
				if (tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
				{
					if (tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED)
					{
						/* Transaction still in progress -- skip */
						scan_live_tuples++;
					}
					else if (FluxTupleDeadToAll(tuple_hdr, oldest_xmin_vac))
					{
						/* Tuple is dead and can be removed */
						dead_offsets[ndead_on_page++] = offnum;
						dead_tuples++;
					}
					else
					{
						/* Recently dead -- not yet reclaimable but still dead */
						dead_tuples++;
						recently_dead_tuples++;
					}
				}
				else if (FluxTupleDeadToAll(tuple_hdr, oldest_xmin_vac))
				{
					/* Aborted INSERT: never visible to anyone, remove it. */
					dead_offsets[ndead_on_page++] = offnum;
					dead_tuples++;
				}
				else
				{
					/* Tuple is live */
					scan_live_tuples++;
				}
			}

			/*
			 * Record dead tuple TIDs for this page in the TidStore. This is
			 * needed for index cleanup in Phase II.
			 */
			if (ndead_on_page > 0 && do_index_cleanup)
			{
				const int	prog_index[] = {
					PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS,
					PROGRESS_VACUUM_DEAD_TUPLE_BYTES
				};
				int64		prog_val[2];

				TidStoreSetBlockOffsets(dead_items, blkno,
										dead_offsets, ndead_on_page);
				dead_items_info->num_items += ndead_on_page;
				prog_val[0] = dead_items_info->num_items;
				prog_val[1] = TidStoreMemoryUsage(dead_items);
				pgstat_progress_update_multi_param(2, prog_index, prog_val);
			}

			UnlockReleaseBuffer(buf);
		}

		read_stream_end(scan_stream);
		if (round_full)
		{
			/* The next round resumes after the last block read. */
			Assert(BlockNumberIsValid(blkno));
			scan_state.current_block = blkno;
			round_end = blkno + 1;
		}
		else
		{
			pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_SCANNED,
										 nblocks);
			round_end = nblocks;
		}
	}

	/*
	 * -----------------------------------------------------------------------
	 * Phase II: Index vacuum -- remove dead index entries
	 *
	 * For each index on the relation, call the index AM's bulk delete routine
	 * to remove entries pointing to dead tuples.  This MUST happen before we
	 * defragment data pages (Phase III) to ensure no index entry points to a
	 * TID that has been recycled.
	 * -----------------------------------------------------------------------
	 */
	if (do_index_cleanup)
	{
		int			idx;
		bool		scanned = false;
		const int	start_index[] = {
			PROGRESS_VACUUM_PHASE,
			PROGRESS_VACUUM_INDEXES_TOTAL
		};
		const int	end_index[] = {
			PROGRESS_VACUUM_INDEXES_TOTAL,
			PROGRESS_VACUUM_INDEXES_PROCESSED,
			PROGRESS_VACUUM_NUM_INDEX_VACUUMS
		};
		int64		start_val[2];
		int64		end_val[3];

		if (verbose && dead_items_info->num_items > 0)
			ereport(INFO,
					(errmsg("vacuuming \"%s\": removing %lld dead index entries across %d indexes",
							RelationGetRelationName(onerel),
							(long long) dead_items_info->num_items,
							nindexes)));

		start_val[0] = PROGRESS_VACUUM_PHASE_VACUUM_INDEX;
		start_val[1] = nindexes;
		pgstat_progress_update_multi_param(2, start_index, start_val);

		/*
		 * The workers report indexes_processed.  A pass with no dead TIDs
		 * only scans delete-marking indexes, and stays serial.
		 */
		if (nworkers >= 0 && dead_items_info->num_items > 0 &&
			flux_vacuum_indexes_parallel(onerel, indrels, nindexes, indstats,
										 nworkers, verbose, bstrategy,
										 dead_items, dead_items_info,
										 (double) scan_live_tuples, true,
										 num_index_scans))
			scanned = true;
		else
		{
			for (idx = 0; idx < nindexes; idx++)
			{
				/*
				 * With no dead TIDs, only a delete-marking index has anything
				 * to remove: the delete-marked entries an in-place key UPDATE
				 * left for a live row (table_index_tombstone_removable): a
				 * scan of such an index on every VACUUM.  Counting key
				 * UPDATEs per relation would let us skip it when there were
				 * none.  The first round's scan has already removed those.
				 */
				if (dead_items_info->num_items > 0 ||
					(num_index_scans == 0 &&
					 indrels[idx]->rd_indam->amdeletemark != NULL))
				{
					indstats[idx] = flux_vacuum_one_index(indrels[idx], onerel,
														  indstats[idx],
														  (double) scan_live_tuples,
														  true, verbose, bstrategy,
														  dead_items,
														  dead_items_info);
					scanned = true;
				}
				pgstat_progress_update_param(PROGRESS_VACUUM_INDEXES_PROCESSED,
											 idx + 1);
			}
		}

		/* A pass that scanned an index counts as a round, as in heap. */
		if (scanned)
			num_index_scans++;
		end_val[0] = 0;
		end_val[1] = 0;
		end_val[2] = num_index_scans;
		pgstat_progress_update_multi_param(3, end_index, end_val);
	}

	/*
	 * -----------------------------------------------------------------------
	 * Phase III: Defragment data pages -- remove dead tuples from heap
	 *
	 * Now that index entries pointing to dead tuples have been removed, we
	 * can safely defragment data pages.  This reclaims the space occupied by
	 * dead tuples and makes it available for reuse.
	 * -----------------------------------------------------------------------
	 */
	/* Tests stop here, between Phase II and Phase III. */
	INJECTION_POINT("flux-vacuum-before-phase3", NULL);
	/* Phase III visits every page of the round, also those Phase I skipped. */
	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_VACUUM_HEAP);

	{
		ReadStream *stream;
		BlockRangeReadStreamPrivate stream_private;

		stream_private.current_blocknum = round_start;
		stream_private.last_exclusive = round_end;

		stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
											READ_STREAM_USE_BATCHING,
											bstrategy,
											onerel,
											MAIN_FORKNUM,
											block_range_read_stream_cb,
											&stream_private,
											0);

		for (;;)
		{
			OffsetNumber offnum,
						maxoffnum;
			ItemId		itemid;
			bool		page_has_dead_tuples = false;
			bool		page_modified = false;

			vacuum_delay_point(false);
			buf = read_stream_next_buffer(stream, NULL);
			if (!BufferIsValid(buf))
				break;

			blkno = BufferGetBlockNumber(buf);
			pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED,
										 blkno);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);

			if (PageIsNew(page))
			{
				empty_end_pages++;	/* empty too, see below */
				UnlockReleaseBuffer(buf);
				continue;
			}

			flux_vacuum_revert_page(onerel, buf);
			maxoffnum = PageGetMaxOffsetNumber(page);

			/*
			 * First pass: clean overflow chains for dead tuples.  We must do
			 * this BEFORE defragmenting, because FluxPageDefragment removes
			 * dead item pointers and after that we can no longer identify
			 * which tuples had overflow data.  We temporarily drop the
			 * exclusive lock since overflow chain deletion may need to read
			 * and lock other pages.
			 */
			for (offnum = FirstOffsetNumber; offnum <= maxoffnum; offnum++)
			{
				FluxTupleHeader *tuple_hdr;

				itemid = PageGetItemId(page, offnum);

				/*
				 * LP_DEAD line pointers were already pruned (storage
				 * reclaimed) by opportunistic pruning and their TIDs recorded
				 * in Phase I for index cleanup, which has now run.  They
				 * carry no storage and no overflow chain to clean; just flag
				 * the page so the defragment pass below converts them to
				 * LP_UNUSED.
				 */
				if (ItemIdIsDead(itemid))
				{
					if (FluxVacuumMayFree(dead_items, blkno, offnum))
						page_has_dead_tuples = true;
					continue;
				}

				if (!ItemIdIsNormal(itemid))
					continue;

				tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);
				if (FluxIsOverflowRecordInline(tuple_hdr, ItemIdGetLength(itemid)))
					continue;

				/*
				 * A dead row's TOAST values need nothing from VACUUM, as in
				 * heap: its DELETE deleted them, and an aborted INSERT's
				 * values died with it.  TOAST VACUUM removes them.
				 */
				if (FluxTupleDeadToAll(tuple_hdr, oldest_xmin_vac))
					page_has_dead_tuples = true;
			}

			/*
			 * If page has dead tuples, defragment it to consolidate space.
			 */
			if (page_has_dead_tuples)
			{
				TransactionId vac_horizon = InvalidTransactionId;

				/*
				 * Compaction moves tuples, so it needs a cleanup lock, as
				 * heap pruning does.  The exclusive lock we hold may already
				 * be one.  Otherwise an aggressive VACUUM waits for the other
				 * pins to go away and looks at the page again (the pass below
				 * decides again which tuples to remove); a non-aggressive one
				 * leaves the page for a later VACUUM, as heap's
				 * lazy_scan_noprune does, rather than wait behind a cursor
				 * that may hold its pin for a long time.  Freezing and the
				 * visibility map still see the page below.
				 */
				if (!IsBufferCleanupOK(buf))
				{
					if (!aggressive)
						goto page_not_compacted;
					LockBuffer(buf, BUFFER_LOCK_UNLOCK);
					LockBufferForCleanup(buf);
					page = BufferGetPage(buf);
					maxoffnum = PageGetMaxOffsetNumber(page);
				}

				START_CRIT_SECTION();

				/*
				 * Mark dead tuples as unused before defragmenting. The scan
				 * above already identified them; now set LP_UNUSED.  Index
				 * cleanup (Phase II) has removed every index entry pointing
				 * at these TIDs, so it is now safe to free the line pointers
				 * for recycling -- both the still-materialized DELETED tuples
				 * and the already-pruned LP_DEAD placeholders.
				 */
				for (offnum = FirstOffsetNumber; offnum <= maxoffnum; offnum++)
				{
					FluxTupleHeader *vac_hdr;

					itemid = PageGetItemId(page, offnum);

					if (ItemIdIsDead(itemid))
					{
						if (FluxVacuumMayFree(dead_items, blkno, offnum))
							ItemIdSetUnused(itemid);
						continue;
					}

					if (!ItemIdIsNormal(itemid))
						continue;

					if (FluxIsOverflowRecordInline(PageGetItem(page, itemid),
												   ItemIdGetLength(itemid)))
						continue;

					vac_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);
					if (FluxTupleDeadToAll(vac_hdr, oldest_xmin_vac))
					{
						FluxTupleAdvanceConflictHorizon(vac_hdr, &vac_horizon);
						/* unrecorded: its index entries survived Phase II */
						if (FluxVacuumMayFree(dead_items, blkno, offnum))
							ItemIdSetUnused(itemid);
						else
							ItemIdSetDead(itemid);
					}
				}

				FluxPageDefragment(page);
				page_modified = true;
				pages_vacuumed++;

				MarkBufferDirty(buf);

				if (RelationNeedsWAL(onerel))
				{
					XLogRecPtr	recptr;

					recptr = FluxXLogDefrag(onerel, buf, NULL, 0, oldest_ts,
											vac_horizon);
					PageSetLSN(page, recptr);
				}

				END_CRIT_SECTION();
			}

	page_not_compacted:
			flux_vacuum_freeze_page(onerel, buf, &cutoffs, &new_frozen);

			for (offnum = FirstOffsetNumber; offnum <= PageGetMaxOffsetNumber(page); offnum++)
			{
				FluxTupleHeader *hdr;

				itemid = PageGetItemId(page, offnum);
				if (ItemIdIsDead(itemid))
				{
					remaining_dead++;
					continue;
				}
				if (!ItemIdIsNormal(itemid))
					continue;
				hdr = (FluxTupleHeader *) PageGetItem(page, itemid);
				if (FluxIsOverflowRecordInline(hdr, ItemIdGetLength(itemid)))
					continue;
				switch (FluxTupleStatsCount(hdr))
				{
					case FLUX_COUNT_LIVE:
						live_tuples++;
						break;
					case FLUX_COUNT_DEAD:
						remaining_dead++;
						break;
					case FLUX_COUNT_NONE:
						break;
				}
			}

			/* Update FSM with accurate free space information */
			if (page_modified || PageGetFreeSpace(page) > 0)
			{
				FluxRecordFreeSpace(onerel, blkno, PageGetHeapFreeSpace(page));
			}

			/*
			 * Update the Visibility Map for this page.
			 *
			 * After defragmentation, check whether all remaining tuples on
			 * the page are visible to all transactions and/or frozen.  If so,
			 * set the appropriate VM bits.  This enables index-only scans to
			 * skip heap fetches and future VACUUMs to skip this page
			 * entirely.
			 */
			{
				bool		all_visible = true;
				bool		all_frozen = true;
				TransactionId vm_conflict = InvalidTransactionId;
				OffsetNumber vm_offnum;
				OffsetNumber vm_maxoff;

				vm_maxoff = PageGetMaxOffsetNumber(page);

				for (vm_offnum = FirstOffsetNumber; vm_offnum <= vm_maxoff; vm_offnum++)
				{
					ItemId		vm_itemid;
					FluxTupleHeader *vm_tuple_hdr;

					vm_itemid = PageGetItemId(page, vm_offnum);

					/*
					 * an LP_DEAD slot awaits a VACUUM, which must not skip
					 * the page
					 */
					if (ItemIdIsDead(vm_itemid))
					{
						all_visible = false;
						all_frozen = false;
						break;
					}
					if (!ItemIdIsNormal(vm_itemid))
						continue;

					/* Skip overflow records */
					if (FluxIsOverflowRecordInline(PageGetItem(page, vm_itemid),
												   ItemIdGetLength(vm_itemid)))
						continue;

					vm_tuple_hdr = (FluxTupleHeader *) PageGetItem(page, vm_itemid);

					/*
					 * Dead tuples (deleted) that survived defrag are recently
					 * dead
					 */
					if (vm_tuple_hdr->t_flags & FLUX_TUPLE_DELETED)
					{
						all_visible = false;
						all_frozen = false;
						break;
					}

					/* Speculative tuples are not visible to all */
					if (vm_tuple_hdr->t_flags & FLUX_TUPLE_SPECULATIVE)
					{
						all_visible = false;
						all_frozen = false;
						break;
					}

					if (vm_tuple_hdr->t_xmin != FrozenTransactionId)
					{
						all_frozen = false;
						if (!TransactionIdIsValid(vm_conflict) ||
							TransactionIdFollows(vm_tuple_hdr->t_xmin, vm_conflict))
							vm_conflict = vm_tuple_hdr->t_xmin;
						if (!TransactionIdIsNormal(vm_tuple_hdr->t_xmin) ||
							!TransactionIdPrecedes(vm_tuple_hdr->t_xmin, oldest_xmin_vac) ||
							!TransactionIdDidCommit(vm_tuple_hdr->t_xmin))
							all_visible = false;
					}
					if (TransactionIdIsNormal(FluxTupleGetXmax(vm_tuple_hdr)))
					{
						all_frozen = false;
						all_visible = false;
					}
				}

				/* Empty pages are trivially all-visible and all-frozen */
				if (vm_maxoff < FirstOffsetNumber)
				{
					all_visible = true;
					all_frozen = true;
				}

				FluxVMVacuumPage(onerel, buf, all_visible, all_frozen, vm_conflict);
			}

			/*
			 * Count the run of empty pages ending at this block.  Pages are
			 * read in ascending order, so the run that reaches the last block
			 * is the one Phase V truncates.
			 */
			if (PageGetMaxOffsetNumber(page) < FirstOffsetNumber)
				empty_end_pages++;
			else
				empty_end_pages = 0;

			UnlockReleaseBuffer(buf);
		}

		pgstat_progress_update_param(PROGRESS_VACUUM_HEAP_BLKS_VACUUMED,
									 round_end);
		read_stream_end(stream);
	}

	if (round_full)
	{
		/* Forget the TIDs this round vacuumed, and scan on. */
		TidStoreDestroy(dead_items);
		dead_items = TidStoreCreateLocal(dead_items_info->max_bytes, true);
		dead_items_info->num_items = 0;
		round_start = round_end;
		pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
									 PROGRESS_VACUUM_PHASE_SCAN_HEAP);
		goto next_round;
	}

	/*
	 * -----------------------------------------------------------------------
	 * Phase IV: Index cleanup (amvacuumcleanup)
	 *
	 * Call each index AM's vacuum cleanup routine.  This lets the index AM do
	 * any post-vacuum maintenance such as reclaiming empty pages, updating
	 * statistics, etc.  This is called even if no dead tuples were found,
	 * since some index AMs use this to update internal metadata.
	 * -----------------------------------------------------------------------
	 */
	if (do_index_cleanup)
	{
		int			idx;
		const int	start_index[] = {
			PROGRESS_VACUUM_PHASE,
			PROGRESS_VACUUM_INDEXES_TOTAL
		};
		const int	end_index[] = {
			PROGRESS_VACUUM_INDEXES_TOTAL,
			PROGRESS_VACUUM_INDEXES_PROCESSED
		};
		int64		start_val[2];
		int64		end_val[2] = {0, 0};

		start_val[0] = PROGRESS_VACUUM_PHASE_INDEX_CLEANUP;
		start_val[1] = nindexes;
		pgstat_progress_update_multi_param(2, start_index, start_val);

		if (nworkers < 0 ||
			!flux_vacuum_indexes_parallel(onerel, indrels, nindexes, indstats,
										  nworkers, verbose, bstrategy,
										  NULL, NULL, (double) live_tuples,
										  false, num_index_scans))
		{
			for (idx = 0; idx < nindexes; idx++)
			{
				indstats[idx] = flux_vacuum_one_index(indrels[idx], onerel,
													  indstats[idx],
													  (double) live_tuples,
													  false, verbose,
													  bstrategy, NULL, NULL);
				pgstat_progress_update_param(PROGRESS_VACUUM_INDEXES_PROCESSED,
											 idx + 1);
			}
		}

		/* Out of parallel mode now: catalog updates are allowed. */
		for (idx = 0; idx < nindexes; idx++)
		{
			if (indstats[idx] != NULL && !indstats[idx]->estimated_count)
				vac_update_relstats(indrels[idx], indstats[idx]->num_pages,
									indstats[idx]->num_index_tuples, 0, 0, false,
									InvalidTransactionId, InvalidMultiXactId,
									NULL, NULL, false);
		}
		pgstat_progress_update_multi_param(2, end_index, end_val);
	}

	/*
	 * -----------------------------------------------------------------------
	 * Phase IV-C: Orphan overflow cleanup
	 *
	 * Run the two-pass orphan detection algorithm to find and remove overflow
	 * records that are not referenced by any live tuple.  This catches
	 * overflow records that were orphaned by crashes, aborted transactions,
	 * or bugs in the eager cleanup path.
	 * -----------------------------------------------------------------------
	 */
	/* FLUX has no on-page overflow records to vacuum (wide values use TOAST). */

	/*
	 * -----------------------------------------------------------------------
	 * Phase IV-D: per-backend UNDO discard
	 *
	 * FLUX's DML UNDO lives in the per-backend engine, which reclaims its own
	 * log segments via its discard worker.  There is no per-relation UNDO log
	 * for VACUUM to discard here.
	 * -----------------------------------------------------------------------
	 */

	/*
	 * -----------------------------------------------------------------------
	 * Phase V: Truncation and final cleanup
	 * -----------------------------------------------------------------------
	 */

	/* Make the free space recorded by Phase III visible to FSM searches. */
	FreeSpaceMapVacuum(onerel);

	/* Truncate empty pages at the end of the relation */
	if (FluxShouldAttemptTruncation(params, nblocks, empty_end_pages))
	{
		BlockNumber new_nblocks = nblocks - empty_end_pages;

		pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
									 PROGRESS_VACUUM_PHASE_TRUNCATE);

		/* Tests insert into the empty tail here, before the lock is taken. */
		INJECTION_POINT("flux-vacuum-before-truncate", NULL);

		/*
		 * Truncate under AccessExclusiveLock with a tail re-verification, so
		 * we never remove a block a concurrent INSERT is targeting.  The
		 * helper may truncate to fewer pages than requested (never more) or
		 * decline entirely if it can't get the lock or the tail is no longer
		 * empty.
		 */
		pages_truncated = FluxTruncateRelation(onerel, nblocks, new_nblocks,
											   verbose);
	}

	pgstat_progress_update_param(PROGRESS_VACUUM_PHASE,
								 PROGRESS_VACUUM_PHASE_FINAL_CLEANUP);

	/* The complete Phase III scan bounds every on-page and historical XID. */
	nblocks -= pages_truncated;
	visibilitymap_count(onerel, &all_visible_pages, &all_frozen_pages);
	all_visible_pages = Min(all_visible_pages, nblocks);
	all_frozen_pages = Min(all_frozen_pages, all_visible_pages);
	vac_update_relstats(onerel, nblocks, live_tuples,
						all_visible_pages, all_frozen_pages, nindexes > 0,
						new_frozen, cutoffs.OldestMxact, NULL, NULL, false);
	pgstat_report_vacuum(onerel, live_tuples, remaining_dead, starttime);
	pgstat_progress_end_command();

	/* Clean up index resources */
	if (dead_items != NULL)
		TidStoreDestroy(dead_items);
	if (dead_items_info != NULL)
		pfree(dead_items_info);
	if (indstats != NULL)
		pfree(indstats);
	vac_close_indexes(nindexes, indrels, RowExclusiveLock);

	/* Report statistics */
	if (verbose || params->options & VACOPT_VERBOSE)
	{
		ereport(INFO,
				(errmsg("FLUX vacuum \"%s\": found %lld tuples (%lld live, %lld dead), "
						"%lld are dead but not yet removable, "
						"vacuumed %lld pages, truncated %u pages, "
						"cleaned %d indexes",
						RelationGetRelationName(onerel),
						(long long) num_tuples,
						(long long) live_tuples,
						(long long) dead_tuples,
						(long long) recently_dead_tuples,
						(long long) pages_vacuumed,
						pages_truncated,
						nindexes)));
	}
}

/* ================================================================
 * sLog transaction callbacks for FLUX
 *
 * These handle FLUX-specific page operations (clearing UNCOMMITTED flags,
 * marking aborted tuples as DELETED) at transaction boundaries.  They call
 * the generic SLogTuple* functions for shared-hash cleanup.
 * ================================================================
 */

/* One record per row lock held by a prepared transaction. */
typedef struct FluxTwoPhaseRecord
{
	RelFileLocator rlocator;
	Oid			relid;
	ItemPointerData tid;
	TransactionId xid;
	LockTupleMode mode;
} FluxTwoPhaseRecord;

#define FLUX_2PC_ROWLOCK		1

/* Allocated in TopTransactionContext at PRE_PREPARE. */
static List *flux_prepared_locks = NIL;

/*
 * FluxEnsureSLogCallbacks -- register xact/subxact callbacks once per backend.
 */
void
FluxEnsureSLogCallbacks(void)
{
	if (!flux_slog_callbacks_registered)
	{
		RegisterXactCallback(FluxSLogXactCallback, NULL);
		RegisterSubXactCallback(FluxSLogSubXactCallback, NULL);
		flux_slog_callbacks_registered = true;
	}
}

/*
 * Was this image written by one of top's own subtransactions that rolled
 * back?  t_xmin is the writing (sub)transaction's xid, so that is exactly:
 * a later xid than top, no longer current (a committed child still is), and
 * top is its topmost parent.  Every xid that follows top also follows
 * TransactionXmin, as pg_subtrans requires.
 */
static bool
flux_xmin_is_own_aborted_subxact(TransactionId xmin, TransactionId top)
{
	return TransactionIdIsNormal(xmin) &&
		TransactionIdFollows(xmin, top) &&
		!TransactionIdIsCurrentTransactionId(xmin) &&
		TransactionIdEquals(SubTransGetTopmostTransaction(xmin), top);
}

/*
 * FluxProcessAbortedEntries -- at PRE_COMMIT and PRE_PREPARE, mark the tuples
 * this transaction's rolled-back subtransactions inserted as DELETED on their
 * pages.
 *
 * SLogTupleRemoveBySubXid flips a tracked key's own op_type to
 * SLOG_OP_ABORTED when its subtransaction rolls back, so that is what says the
 * key's op was rolled back.
 *
 * A key made in storage the relation no longer uses (rewritten or truncated
 * since) has no page to fix: that storage goes away at commit.  A key in the
 * current storage can still name a block past its end (TRUNCATE of a table
 * created in this transaction keeps the storage), and that tuple is gone too.
 */
static void
flux_process_aborted_key(Relation rel, BlockNumber nblocks,
						 const SLogTrackedKeyInfo *tk)
{
	ItemPointer tid = (ItemPointer) &tk->key.tid;
	BlockNumber blkno = ItemPointerGetBlockNumber(tid);

	if (tk->relnumber == rel->rd_locator.relNumber && blkno < nblocks)
	{
		Buffer		buf;
		Page		page;
		ItemId		itemid;
		FluxTupleHeader *tuple_hdr;
		OffsetNumber offnum = ItemPointerGetOffsetNumber(tid);

		buf = ReadBuffer(rel, blkno);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		if (offnum <= PageGetMaxOffsetNumber(page))
		{
			itemid = PageGetItemId(page, offnum);
			if (ItemIdIsNormal(itemid))
			{
				tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

				/*
				 * Only an image one of our rolled-back subtransactions wrote
				 * is ours to discard.  The ABORTED op says only that we did
				 * something to this tid in a rolled-back subtransaction -- an
				 * INSERT, but also an UPDATE, a DELETE or a row lock -- and
				 * the UNCOMMITTED hint may belong to an image someone else
				 * wrote after that rollback (committed or still running), or
				 * to our own top-level UPDATE.  Marking such an image DELETED
				 * left a committed row that a plain SELECT still returned but
				 * UPDATE, DELETE and row locks no longer found, and did so
				 * with no WAL, on the primary only.
				 */
				if ((tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED) &&
					flux_xmin_is_own_aborted_subxact(FluxTupleGetXmin(tuple_hdr),
													 tk->xid))
				{
					tuple_hdr->t_flags |= FLUX_TUPLE_DELETED;
					tuple_hdr->t_flags &= ~FLUX_TUPLE_UNCOMMITTED;
					MarkBufferDirty(buf);
				}
			}
		}

		UnlockReleaseBuffer(buf);
	}
}

static void
FluxProcessAbortedEntries(TransactionId xid)
{
	SLogTrackedKeyInfo *keys;
	int			nkeys;
	Oid			cur_relid = InvalidOid;
	Relation	rel = NULL;
	BlockNumber nblocks = 0;

	nkeys = SLogTupleCollectTrackedKeys(xid, &keys);
	for (int i = 0; i < nkeys; i++)
	{
		SLogTrackedKeyInfo *tk = &keys[i];

		if (tk->op_type != SLOG_OP_ABORTED)
			continue;

		if (tk->key.relid != cur_relid)
		{
			if (rel != NULL)
				relation_close(rel, AccessShareLock);
			cur_relid = tk->key.relid;
			rel = try_relation_open(cur_relid, AccessShareLock);
			if (rel != NULL)
				nblocks = RelationGetNumberOfBlocks(rel);
		}

		if (rel != NULL)
			flux_process_aborted_key(rel, nblocks, tk);
	}

	if (rel != NULL)
		relation_close(rel, AccessShareLock);
	pfree(keys);
}

/* ----------------------------------------------------------------
 * Batched commit-time UNCOMMITTED-flag clearing for
 * FluxClearUncommittedFlags.
 *
 * Commit visibility comes from CLOG (heap-shaped xmin/xmax MVCC); commit
 * does NOT stamp any timestamp.  This path only clears the on-page
 * FLUX_TUPLE_UNCOMMITTED hint flag so later readers skip the sLog
 * fast-path lookup.  t_xmin/t_xmax written by the DML stay untouched.
 *
 * The batched approach collects tracked keys, sorts by (relid, blockno),
 * and processes them with minimal buffer I/O:
 *   - One try_relation_open() per distinct relid
 *   - One ReadBuffer() per distinct block
 *   - Local-only INSERTs skip the shared sLog lookup entirely
 * ----------------------------------------------------------------
 */
/*
 * flux_cmp_tracked_key_by_block -- qsort comparator for batch commit stamping.
 *
 * Sorts by (relid, blockno, offnum) to enable sequential I/O: one
 * try_relation_open per relation, one ReadBuffer per distinct block.
 */
static int
flux_cmp_tracked_key_by_block(const void *a, const void *b)
{
	const SLogTrackedKeyInfo *ka = (const SLogTrackedKeyInfo *) a;
	const SLogTrackedKeyInfo *kb = (const SLogTrackedKeyInfo *) b;

	if (ka->key.relid < kb->key.relid)
		return -1;
	if (ka->key.relid > kb->key.relid)
		return 1;

	{
		BlockNumber ba = ItemPointerGetBlockNumber((ItemPointer) &ka->key.tid);
		BlockNumber bb = ItemPointerGetBlockNumber((ItemPointer) &kb->key.tid);

		if (ba < bb)
			return -1;
		if (ba > bb)
			return 1;
	}

	{
		OffsetNumber oa = ItemPointerGetOffsetNumber((ItemPointer) &ka->key.tid);
		OffsetNumber ob = ItemPointerGetOffsetNumber((ItemPointer) &kb->key.tid);

		if (oa < ob)
			return -1;
		if (oa > ob)
			return 1;
	}

	return 0;
}

/*
 * flux_stamp_tuple_committed -- clear the UNCOMMITTED hint of one tuple we
 * inserted, at commit time.  Called from the batched commit path with the
 * buffer already locked exclusive.
 *
 * A key whose savepoint rolled back (op_type flipped to SLOG_OP_ABORTED by
 * SLogTupleRemoveBySubXid) is skipped: FluxProcessAbortedEntries has marked
 * its tuple DELETED.  Commit stamps nothing else: t_xmin and t_xmax were
 * written by the DML, CLOG is the commit oracle, and the flag is a hint.
 */
static void
flux_stamp_tuple_committed(Buffer buf, OffsetNumber offnum,
						   const SLogTrackedKeyInfo *tk,
						   TransactionId xid)
{
	Page		page = BufferGetPage(buf);
	ItemId		itemid;
	FluxTupleHeader *tuple_hdr;

	if (tk->op_type == SLOG_OP_ABORTED)
		return;
	if (offnum > PageGetMaxOffsetNumber(page))
		return;
	itemid = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(itemid))
		return;

	tuple_hdr = (FluxTupleHeader *) PageGetItem(page, itemid);
	if (tuple_hdr->t_flags & FLUX_TUPLE_UNCOMMITTED)
		tuple_hdr->t_flags &= ~FLUX_TUPLE_UNCOMMITTED;
}

/*
 * flux_batch_clear_uncommitted -- batch-process tracked keys at commit time.
 *
 * Processes the pre-sorted array of tracked keys with sequential I/O:
 *   - One try_relation_open() per distinct relid
 *   - One ReadBuffer() per distinct block within each relation
 *   - All tuples on the same page are stamped while holding one buffer lock
 *
 * This replaces the per-tuple callback pattern which did O(n) ReadBuffer
 * calls even when multiple tuples shared the same page.
 */
static void
flux_batch_clear_uncommitted(SLogTrackedKeyInfo *keys, int nkeys,
							 TransactionId xid)
{
	Oid			cur_relid = InvalidOid;
	BlockNumber cur_blkno = InvalidBlockNumber;
	BlockNumber nblocks = 0;
	Relation	rel = NULL;
	Buffer		buf = InvalidBuffer;
	int			i;

	/*
	 * Prefetch: issue advisory read-ahead for the first few distinct blocks.
	 * Since keys are sorted by (relid, blockno), we can scan ahead cheaply.
	 * This overlaps I/O with the kernel readahead path.
	 */
	if (nkeys > 0)
	{
		Oid			pf_relid = keys[0].key.relid;
		Relation	pf_rel;
		BlockNumber pf_prev = InvalidBlockNumber;
		int			pf_count = 0;

		pf_rel = try_relation_open(pf_relid, AccessShareLock);
		if (pf_rel != NULL)
		{
			for (int j = 0; j < nkeys && pf_count < 8; j++)
			{
				BlockNumber pf_blk;

				/* Stop prefetching if we cross into a different relation */
				if (keys[j].key.relid != pf_relid)
					break;
				if (keys[j].relnumber != pf_rel->rd_locator.relNumber)
					continue;

				pf_blk = ItemPointerGetBlockNumber((ItemPointer) &keys[j].key.tid);
				if (pf_blk != pf_prev)
				{
					PrefetchBuffer(pf_rel, MAIN_FORKNUM, pf_blk);
					pf_prev = pf_blk;
					pf_count++;
				}
			}
			relation_close(pf_rel, AccessShareLock);
		}
	}

	for (i = 0; i < nkeys; i++)
	{
		SLogTrackedKeyInfo *tk = &keys[i];
		Oid			relid = tk->key.relid;
		BlockNumber blkno = ItemPointerGetBlockNumber((ItemPointer) &tk->key.tid);
		OffsetNumber offnum = ItemPointerGetOffsetNumber((ItemPointer) &tk->key.tid);

		/* Switch relation if needed (sorted order minimizes switches) */
		if (relid != cur_relid)
		{
			/* Mark outgoing page dirty (once per page, not per tuple) */
			if (BufferIsValid(buf))
			{
				MarkBufferDirtyHint(buf, true);
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}
			if (rel != NULL)
			{
				relation_close(rel, AccessShareLock);
				rel = NULL;
			}

			rel = try_relation_open(relid, AccessShareLock);
			if (rel == NULL)
			{
				cur_relid = relid;
				cur_blkno = InvalidBlockNumber;
				continue;
			}
			cur_relid = relid;
			cur_blkno = InvalidBlockNumber;
			nblocks = RelationGetNumberOfBlocks(rel);
		}

		if (rel == NULL)
			continue;

		/*
		 * The row went to storage this relation no longer uses: a rewrite or
		 * TRUNCATE later in this transaction replaced it, and it goes away at
		 * commit.  The same TID in the current storage is another row, or a
		 * heap tuple, or past its end.  A row in the current storage can
		 * still be past its end, after a TRUNCATE of a table created in this
		 * transaction (which keeps the storage).
		 */
		if (tk->relnumber != rel->rd_locator.relNumber || blkno >= nblocks)
			continue;

		/*
		 * Switch block if needed; amortizes ReadBuffer across same-page
		 * tuples
		 */
		if (blkno != cur_blkno)
		{
			/* Mark outgoing page dirty (once per page, not per tuple) */
			if (BufferIsValid(buf))
			{
				MarkBufferDirtyHint(buf, true);
				UnlockReleaseBuffer(buf);
				buf = InvalidBuffer;
			}

			buf = ReadBuffer(rel, blkno);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			cur_blkno = blkno;
		}

		/* Stamp this tuple */
		flux_stamp_tuple_committed(buf, offnum, tk, xid);
	}

	/* Mark final page dirty and release */
	if (BufferIsValid(buf))
	{
		MarkBufferDirtyHint(buf, true);
		UnlockReleaseBuffer(buf);
	}
	if (rel != NULL)
		relation_close(rel, AccessShareLock);
}

/*
 * FluxClearUncommittedFlags -- proactively clear FLUX_TUPLE_UNCOMMITTED on
 * all tuples modified by the current transaction at PRE_COMMIT time.
 *
 * Uses a batched approach: collects all tracked keys into an array, sorts
 * by (relid, blockno) for sequential I/O, then processes them with at most
 * one ReadBuffer per distinct block and one try_relation_open per relation.
 *
 * A tracked INSERT whose savepoint did not roll back (op_type still
 * SLOG_OP_INSERT) is a live INSERT of this transaction.
 *
 * Clearing the flag is a pure hint: commit visibility comes from CLOG
 * (heap-shaped xmin/xmax MVCC), not from any stamped timestamp.
 */
static void
FluxClearUncommittedFlags(TransactionId xid)
{
	SLogTrackedKeyInfo *keys;
	int			nkeys;

	/*
	 * When lazy clear is enabled, skip the expensive batch page-visit loop.
	 * The UNCOMMITTED flags will be cleared lazily by readers via the
	 * visibility functions when they next access these tuples.
	 */
	if (flux_lazy_uncommitted_clear)
		return;

	/*
	 * Option B (per-page conflict tracking): clear the hint by visiting each
	 * page we dirtied in place once, rather than walking a per-tuple
	 * tracked-key list.  This is O(pages touched), not O(tuples), which is
	 * what keeps a large UPDATE/DELETE from exhausting backend memory.  The
	 * per-tuple INSERT tracked keys (local-only, bounded) are still cleared
	 * by the batch path below; only in-place UPDATE/DELETE flag cleanup moved
	 * to the page set.
	 */
	flux_clear_uncommitted_by_page(xid);

	/* Collect tracked keys into a sortable array */
	nkeys = SLogTupleCollectTrackedKeys(xid, &keys);
	if (nkeys == 0)
	{
		pfree(keys);
		return;
	}

	/* Sort by (relid, blockno, offnum) for sequential I/O */
	qsort(keys, nkeys, sizeof(SLogTrackedKeyInfo), flux_cmp_tracked_key_by_block);

	/* Batch-process: one ReadBuffer per distinct block */
	flux_batch_clear_uncommitted(keys, nkeys, xid);

	pfree(keys);
}

/*
 * flux_page_clear_uncommitted -- clear FLUX_TUPLE_UNCOMMITTED on every tuple
 * of our committing xid on one page.
 *
 * Per-page commit-time flag cleanup (option B): instead of visiting a
 * per-tuple tracked-key list, we visit each page this transaction dirtied
 * (from the backend-local dirty-page set) exactly once and scan its line
 * pointers, clearing the hint on tuples stamped with our xid.
 *
 * Correctness vs. the per-tuple path:
 *  - We clear only tuples whose inserter (t_xmin) or updater/deleter (t_xmax)
 *    is our committing xid, so we never touch another transaction's tuples
 *    that happen to share the page.
 *  - Savepoint-aborted tuples must NOT be un-hinted (that would resurrect
 *    them).  At PRE_COMMIT, FluxProcessAbortedEntries has already run and
 *    marked such tuples' shared ABORTED state; we additionally skip any
 *    tuple that still carries a shared ABORTED sLog entry.  Clearing the
 *    flag is only ever a hint, and CLOG remains the commit oracle, so a
 *    conservative skip here is always safe.
 */
static void
flux_page_clear_uncommitted(Relation rel, BlockNumber blkno, TransactionId xid)
{
	Buffer		buf;
	Page		page;
	OffsetNumber off,
				maxoff;
	bool		dirtied = false;

	buf = ReadBuffer(rel, blkno);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	maxoff = PageGetMaxOffsetNumber(page);

	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		itemid = PageGetItemId(page, off);
		FluxTupleHeader *hdr;

		if (!ItemIdIsNormal(itemid))
			continue;
		if (FluxIsOverflowRecordInline(PageGetItem(page, itemid),
									   ItemIdGetLength(itemid)))
			continue;

		hdr = (FluxTupleHeader *) PageGetItem(page, itemid);

		if (!(hdr->t_flags & FLUX_TUPLE_UNCOMMITTED))
			continue;

		/*
		 * Only our own top-level writes.  A rolled-back savepoint's carry its
		 * own, aborted, xid.
		 */
		if (!TransactionIdEquals(FluxTupleGetXmin(hdr), xid) &&
			!TransactionIdEquals(FluxTupleGetXmax(hdr), xid))
			continue;

		hdr->t_flags &= ~FLUX_TUPLE_UNCOMMITTED;
		dirtied = true;
	}

	if (dirtied)
		MarkBufferDirtyHint(buf, true);
	UnlockReleaseBuffer(buf);
}

/*
 * flux_clear_uncommitted_by_page -- option B driver.  Visit each page this
 * transaction modified in place (from the backend-local dirty-page set) once
 * and clear the UNCOMMITTED hint on our tuples.  O(pages), not O(tuples).
 */
static void
flux_clear_uncommitted_by_page(TransactionId xid)
{
	HASH_SEQ_STATUS seq;
	FluxDirtyPageKey *pk;
	Oid			cur_relid = InvalidOid;
	Relation	rel = NULL;
	BlockNumber nblocks = 0;

	if (flux_local_dirty_pages == NULL)
		return;

	hash_seq_init(&seq, flux_local_dirty_pages);
	while ((pk = (FluxDirtyPageKey *) hash_seq_search(&seq)) != NULL)
	{
		if (pk->relid != cur_relid)
		{
			if (rel != NULL)
				relation_close(rel, AccessShareLock);
			rel = try_relation_open(pk->relid, AccessShareLock);
			cur_relid = pk->relid;
			if (rel != NULL)
				nblocks = RelationGetNumberOfBlocks(rel);
		}

		/*
		 * Skip a page of storage the relation no longer uses (rewritten or
		 * truncated later in this transaction), or past the current storage's
		 * end, as flux_batch_clear_uncommitted does.
		 */
		if (rel != NULL && pk->relnumber == rel->rd_locator.relNumber &&
			pk->blkno < nblocks)
			flux_page_clear_uncommitted(rel, pk->blkno, xid);
	}

	if (rel != NULL)
		relation_close(rel, AccessShareLock);
}

/*
 * Resolve relation locators while relcache access is still legal.  AtPrepare
 * runs in TRANS_PREPARE, too late to open a relation.
 */
static void
flux_collect_prepared_lock(Oid relid, RelFileNumber relnumber,
						   ItemPointer tid, LockTupleMode mode, void *arg)
{
	FluxTwoPhaseRecord *rec;
	Relation	rel;
	MemoryContext oldcontext;

	rel = relation_open(relid, NoLock);
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);
	rec = palloc0(sizeof(*rec));
	rec->rlocator = rel->rd_locator;
	rec->rlocator.relNumber = relnumber;
	rec->relid = relid;
	rec->tid = *tid;
	rec->xid = *(TransactionId *) arg;
	rec->mode = mode;
	flux_prepared_locks = lappend(flux_prepared_locks, rec);
	MemoryContextSwitchTo(oldcontext);
	relation_close(rel, NoLock);
}

static void
FluxCollectPreparedLocks(TransactionId xid)
{
	SLogTupleLockForEachMine(flux_collect_prepared_lock, &xid);
}

void
AtPrepare_Flux(void)
{
	ListCell   *lc;

	foreach(lc, flux_prepared_locks)
		RegisterTwoPhaseRecord(TWOPHASE_RM_FLUX_ID, FLUX_2PC_ROWLOCK,
							   lfirst(lc), sizeof(FluxTwoPhaseRecord));
}

static void
FluxSLogXactCallback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			{
				TransactionId xid = GetCurrentTransactionIdIfAny();

				if (TransactionIdIsValid(xid))
				{
					/*
					 * 1. FluxProcessAbortedEntries -- mark DELETED the tuples
					 * our rolled-back subtransactions inserted.
					 *
					 * 2. FluxClearUncommittedFlags -- clear the on-page
					 * UNCOMMITTED hint flag.  Commit visibility itself comes
					 * from CLOG (heap-shaped xmin/xmax MVCC); clearing the
					 * flag is a pure hint.
					 *
					 * Both run at PRE_COMMIT, before the point of no return,
					 * where an ERROR still aborts the transaction.
					 */
					FluxProcessAbortedEntries(xid);
					FluxClearUncommittedFlags(xid);
				}
			}
			break;

		case XACT_EVENT_PRE_PREPARE:
			{
				TransactionId xid = GetCurrentTransactionIdIfAny();

				if (TransactionIdIsValid(xid))
				{
					/*
					 * At PREPARE, we must NOT clear UNCOMMITTED flags.  The
					 * transaction is not yet committed and another backend
					 * might ROLLBACK PREPARED.
					 *
					 * We still need to process any subtransaction-aborted
					 * entries (mark them DELETED on page) since those are
					 * definitively aborted regardless of PREPARE outcome.
					 *
					 * Two-phase record registration happens in
					 * AtPrepare_Flux(), called from PrepareTransaction()
					 * after StartPrepare().
					 */
					FluxProcessAbortedEntries(xid);
					FluxCollectPreparedLocks(xid);
				}
			}
			break;

		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			{
				/*
				 * The UNCOMMITTED-flag clear and before-image handling
				 * already happened at XACT_EVENT_PRE_COMMIT (see above). Here
				 * we only release backend-local tracking state, which is safe
				 * in the post-commit no-abort region.  Commit visibility
				 * comes from CLOG (heap-shaped xmin/xmax MVCC); nothing
				 * further is written at COMMIT.
				 */
				flux_prepared_locks = NIL;
				SLogTupleLockReleaseAll();
				SLogTupleResetTracking();
				FluxLocalCidReset();
				FluxLocalDirtyPagesReset();
			}
			break;

		case XACT_EVENT_PREPARE:
			{
				/*
				 * At PREPARE completion, do NOT remove sLog entries.  The
				 * sLog entries must persist so that visibility checks can see
				 * the transaction is still in-progress (prepared).  Only
				 * discard the backend-local tracking list since this backend
				 * is done.
				 *
				 * The two-phase records registered by AtPrepare_Flux will be
				 * used by COMMIT PREPARED / ROLLBACK PREPARED to perform the
				 * actual cleanup.  The row locks move to the prepared
				 * transaction's dummy proc.
				 */
				SLogTupleLockAtPrepare(GetTopFullTransactionIdIfAny(),
									   flux_prepared_locks != NIL);
				flux_prepared_locks = NIL;
				SLogTupleResetTracking();
				FluxLocalCidReset();
				FluxLocalDirtyPagesReset();
			}
			break;

		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			{
				/*
				 * CLOG says the inserts aborted; the UNDO records restore
				 * what we updated or deleted.
				 */
				flux_prepared_locks = NIL;
				SLogTupleLockReleaseAll();
				SLogTupleResetTracking();
				FluxLocalCidReset();
				FluxLocalDirtyPagesReset();
			}
			break;

		default:
			break;
	}
}

/*
 * FluxSLogSubXactCallback -- handle subtransaction events.
 */
static void
FluxSLogSubXactCallback(SubXactEvent event,
						SubTransactionId mySubid,
						SubTransactionId parentSubid,
						void *arg)
{
	TransactionId xid;

	switch (event)
	{
		case SUBXACT_EVENT_ABORT_SUB:
			xid = GetTopTransactionIdIfAny();
			if (TransactionIdIsValid(xid))
				SLogTupleRemoveBySubXid(xid, mySubid);
			break;

		case SUBXACT_EVENT_COMMIT_SUB:
			xid = GetTopTransactionIdIfAny();
			if (TransactionIdIsValid(xid))
				SLogTupleUpdateSubXid(xid, mySubid, parentSubid);
			break;

		default:
			break;
	}
}

/* ================================================================
 * Two-phase commit callbacks for FLUX
 *
 * These are invoked by FinishPreparedTransaction() in the backend that
 * runs COMMIT PREPARED or ROLLBACK PREPARED.  They perform the tuple-level
 * cleanup that would normally happen at XACT_EVENT_PRE_COMMIT / COMMIT
 * or ABORT in the originating backend.
 * ================================================================
 */

/* Release only this prepared transaction's row lock, not other holders. */
void
flux_twophase_postcommit(FullTransactionId fxid, uint16 info,
						 void *recdata, uint32 len)
{
	FluxTwoPhaseRecord *rec PG_USED_FOR_ASSERTS_ONLY = (FluxTwoPhaseRecord *) recdata;

	Assert(info == FLUX_2PC_ROWLOCK);
	Assert(len == sizeof(*rec));
	Assert(TransactionIdEquals(rec->xid, XidFromFullTransactionId(fxid)));
	SLogTupleLockReleasePrepared(fxid);
}

void
flux_twophase_postabort(FullTransactionId fxid, uint16 info,
						void *recdata, uint32 len)
{
	flux_twophase_postcommit(fxid, info, recdata, len);
}

/* Restore the lock owners lost with shared memory at restart or promotion. */
void
flux_twophase_recover(FullTransactionId fxid, uint16 info,
					  void *recdata, uint32 len)
{
	FluxTwoPhaseRecord *rec = (FluxTwoPhaseRecord *) recdata;
	SMgrRelation smgr;

	Assert(info == FLUX_2PC_ROWLOCK);
	Assert(len == sizeof(*rec));
	Assert(TransactionIdEquals(rec->xid, XidFromFullTransactionId(fxid)));
	smgr = smgropen(rec->rlocator, INVALID_PROC_NUMBER);
	if (!smgrexists(smgr, MAIN_FORKNUM))
		return;

	SLogTupleLockRestore(fxid, rec->relid, rec->rlocator.relNumber, &rec->tid,
						 rec->mode);
}
