/*-------------------------------------------------------------------------
 *
 * lsn_indexer.c
 *	  Index of pending WAL records by page, for on-demand replay after a crash
 *
 * During crash recovery the startup process scans the WAL and, instead of
 * replaying the records that modify relation pages, files the LSN of each
 * one under the page it touches, in a dshash table in dynamic shared memory.
 * The server then opens.  A page's pending records are replayed when the
 * page is first read from disk, by whichever process reads it, and the fast
 * recovery worker reads the rest in the background.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/lsn_indexer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/lsn_indexer.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "common/hashfn.h"
#include "common/relpath.h"
#include "lib/dshash.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "utils/dsa.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

/* GUC variable */
bool		fast_crash_recovery = false;

/* Control structure in traditional shared memory */
LSNIndexControl *LSNIndexCtl = NULL;

/* Per-process DSA and dshash handles (set by LSNIndexAttach) */
static dsa_area *lsn_dsa = NULL;
static dshash_table *lsn_hash = NULL;

/* State for on-demand replay; see lsn_indexer.h */
BufferTag	targetTag;
bool		inReplayPageWals = false;
Buffer		targetBuffer = InvalidBuffer;

/*
 * The page this process is replaying on demand, between
 * LSNIndexBeginPageReplay() and LSNIndexEndPageReplay().  Replays never
 * nest: redo of an indexed page touches no other page (see
 * LSNIndexBeginPageReplay()), so one slot is enough.
 */
static bool replay_begun = false;
static BufferTag replaying_tag;

/* InRecovery before on-demand redo set it; see LSNIndexReplayIntoBuffer() */
static bool saved_in_recovery = false;

/* Is lsn_index_exit_callback() registered in this process? */
static bool exit_callback_registered = false;

/* Have we run RmgrStartup() for on-demand replay in this process? */
static bool rmgrs_started = false;

/*
 * Scratch buffers handed out for the current record's other pages; dropped
 * once the record has been replayed.  A record references at most
 * XLR_MAX_BLOCK_ID + 1 blocks.
 */
static BufferTag scratch_tags[XLR_MAX_BLOCK_ID + 1];
static int	nscratch = 0;

/* What the scan did; reported by LSNIndexLogScanSummary() */
static int64 scan_records_indexed = 0;
static int64 scan_pages_indexed = 0;
static int64 scan_records_replayed = 0; /* with block references */
static int64 scan_pages_evicted = 0;
static int64 scan_blocks_extended = 0;

/*
 * dshash parameters for the LSN index.
 *
 * Key = BufferTag (first field of PageLSNEntry).
 * Built-in memcmp/memhash/memcpy work fine for BufferTag.
 */
static const dshash_parameters lsn_dsh_params = {
	sizeof(BufferTag),			/* key_size */
	sizeof(PageLSNEntry),		/* entry_size */
	dshash_memcmp,				/* compare_function */
	dshash_memhash,				/* hash_function */
	dshash_memcpy,				/* copy_function */
	LWTRANCHE_LSN_INDEX_HASH	/* tranche_id */
};

static void LSNIndexShmemRequest(void *arg);

const ShmemCallbacks LsnIndexerShmemCallbacks = {
	.request_fn = LSNIndexShmemRequest,
	.init_fn = LSNIndexShmemInit,
};

/* ----------------------------------------------------------------
 *  Shared-memory sizing and initialisation
 * ----------------------------------------------------------------
 */

static void
LSNIndexShmemRequest(void *arg)
{
	Size		size;

	size = MAXALIGN(sizeof(LSNIndexControl));

	if (!fast_crash_recovery)
		return;
	ShmemRequestStruct(.name = "LSNIndex Ctl",
					   .size = size,
					   .ptr = (void **) &LSNIndexCtl,
		);
}

/*
 * LSNIndexShmemInit - postmaster-safe phase 1 of init.
 *
 * Allocates the small control struct in main shmem and initializes the
 * DSA/dshash handles to "invalid".  This must be safe to run in the
 * postmaster, so we deliberately do NOT call dsa_create here - dsm.c
 * forbids that with an assertion (no PROC entry, no error path, etc.).
 *
 * Children inherit the LSNIndexCtl pointer through fork.  Whichever
 * process needs the index first calls LSNIndexInit() to materialize the
 * DSA+dshash and publish their handles via LSNIndexCtl.
 *
 * Called from CreateOrAttachShmemStructs.
 */
void
LSNIndexShmemInit(void *arg)
{
	if (!fast_crash_recovery)
		return;

	LSNIndexCtl->dsa_handle = DSA_HANDLE_INVALID;
	LSNIndexCtl->hash_handle = DSHASH_HANDLE_INVALID;
	LSNIndexCtl->is_active = false;
	pg_atomic_init_u32(&LSNIndexCtl->nreplaying, 0);
}

/* ----------------------------------------------------------------
 *  Lazy create + attach
 * ----------------------------------------------------------------
 */

/*
 * Run LSNIndexErrorCleanup() if this process exits with a replay in
 * progress.  An ERROR in an auxiliary process is a FATAL, and a FATAL never
 * reaches the transaction abort path, where the cleanup normally runs.
 * before_shmem_exit() callbacks run last-registered first, and we register
 * after ShutdownPostgres() and ReleaseAuxProcessResources(), the two that
 * would otherwise reach AbortBufferIO() with the buffer still dirty.
 */
static void
lsn_index_exit_callback(int code, Datum arg)
{
	LSNIndexErrorCleanup();
}

static void
register_exit_callback(void)
{
	if (exit_callback_registered)
		return;
	before_shmem_exit(lsn_index_exit_callback, 0);
	exit_callback_registered = true;
}

/*
 * LSNIndexInit - phase 2: create the DSA+dshash if not yet done, else attach.
 *
 * Must run in a backend context (the startup process is fine).  Called by
 * the startup process when it begins crash recovery.  Pins the DSA so it
 * survives past startup-process exit; subsequent attachers (the worker and
 * regular backends) just call LSNIndexAttach().
 *
 * Modeled on init_dsm_registry() in src/backend/storage/ipc/dsm_registry.c.
 */
void
LSNIndexInit(void)
{
	MemoryContext oldcxt;

	/* Quick exit if already attached in this process. */
	if (lsn_hash != NULL)
		return;

	Assert(LSNIndexCtl != NULL);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	LWLockAcquire(LSNIndexLock, LW_EXCLUSIVE);

	if (LSNIndexCtl->dsa_handle == DSA_HANDLE_INVALID)
	{
		/* First in - create the DSA + dshash and publish handles. */
		lsn_dsa = dsa_create(LWTRANCHE_LSN_INDEX_DSA);
		dsa_pin(lsn_dsa);
		dsa_pin_mapping(lsn_dsa);

		lsn_hash = dshash_create(lsn_dsa, &lsn_dsh_params, NULL);

		LSNIndexCtl->dsa_handle = dsa_get_handle(lsn_dsa);
		LSNIndexCtl->hash_handle = dshash_get_hash_table_handle(lsn_hash);
		LSNIndexCtl->is_active = true;
	}
	else
	{
		/* Already created - just attach. */
		lsn_dsa = dsa_attach(LSNIndexCtl->dsa_handle);
		dsa_pin_mapping(lsn_dsa);
		lsn_hash = dshash_attach(lsn_dsa, &lsn_dsh_params,
								 LSNIndexCtl->hash_handle, NULL);
	}

	LWLockRelease(LSNIndexLock);
	MemoryContextSwitchTo(oldcxt);

	register_exit_callback();
}

/*
 * LSNIndexAttach - attach to an already-created index.
 *
 * Used by backends and the fast recovery worker.  Returns silently if the
 * index doesn't exist or has already been destroyed by the worker.
 */
void
LSNIndexAttach(void)
{
	MemoryContext oldcxt;

	if (lsn_hash != NULL)
		return;					/* already attached */

	if (LSNIndexCtl == NULL || !LSNIndexCtl->is_active ||
		LSNIndexCtl->dsa_handle == DSA_HANDLE_INVALID)
		return;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	LWLockAcquire(LSNIndexLock, LW_SHARED);

	/* Re-check under the lock - the worker may have torn it down. */
	if (LSNIndexCtl->is_active &&
		LSNIndexCtl->dsa_handle != DSA_HANDLE_INVALID)
	{
		lsn_dsa = dsa_attach(LSNIndexCtl->dsa_handle);
		dsa_pin_mapping(lsn_dsa);
		lsn_hash = dshash_attach(lsn_dsa, &lsn_dsh_params,
								 LSNIndexCtl->hash_handle, NULL);
	}

	LWLockRelease(LSNIndexLock);
	MemoryContextSwitchTo(oldcxt);

	if (lsn_hash != NULL)
		register_exit_callback();
}

void
LSNIndexDetach(void)
{
	if (lsn_hash != NULL)
	{
		dshash_detach(lsn_hash);
		lsn_hash = NULL;
	}
	if (lsn_dsa != NULL)
	{
		dsa_detach(lsn_dsa);
		lsn_dsa = NULL;
	}
}

bool
LSNIndexIsActive(void)
{
	return (LSNIndexCtl != NULL && LSNIndexCtl->is_active);
}

/* ----------------------------------------------------------------
 *  Building the index (startup process, during WAL scan)
 * ----------------------------------------------------------------
 */

void
LSNIndexAddEntry(XLogReaderState *state)
{
	RelFileLocator rlocator;
	ForkNumber	forknum;
	BlockNumber blkno;
	BlockNumber nblocks;
	SMgrRelation smgr;
	BufferTag	tag;
	PageLSNEntry *entry;
	bool		found;

	Assert(lsn_dsa != NULL && lsn_hash != NULL);

	for (int blk_id = 0; blk_id <= state->record->max_block_id; blk_id++)
	{
		LSNNode    *node;
		dsa_pointer node_dp;

		if (!XLogRecHasBlockRef(state, blk_id))
			continue;

		XLogRecGetBlockTag(state, blk_id, &rlocator, &forknum, &blkno);
		InitBufferTag(&tag, &rlocator, forknum, blkno);

		/*
		 * Make sure the page exists on disk before anyone can ask for it.  A
		 * record can be for a block past the end of its file: the page was
		 * created in shared buffers and never written before the crash, or
		 * the file was truncated earlier in this WAL and the block recreated
		 * after.  Stock recovery extends the file when it replays such a
		 * record (see XLogReadBufferExtended()); do the same now, while no
		 * other process can be extending the relation.  Otherwise a backend
		 * would later be handed this block as a brand new page, and its
		 * pending records would be lost with it.  The fork may not exist yet
		 * at all, like a visibility map before its first page.
		 */
		smgr = smgropen(rlocator, INVALID_PROC_NUMBER);
		smgrcreate(smgr, forknum, true);
		nblocks = smgrnblocks(smgr, forknum);
		if (blkno >= nblocks)
		{
			smgrzeroextend(smgr, forknum, nblocks, blkno - nblocks + 1, false);
			scan_blocks_extended += blkno - nblocks + 1;
		}

		entry = (PageLSNEntry *) dshash_find_or_insert(lsn_hash, &tag, &found);

		if (!found)
		{
			entry->lsn_head = InvalidDsaPointer;
			entry->lsn_tail = InvalidDsaPointer;
			entry->max_lsn = InvalidXLogRecPtr;
			scan_pages_indexed++;
		}

		/* Allocate a new LSNNode in DSA memory */
		node_dp = dsa_allocate(lsn_dsa, sizeof(LSNNode));
		node = (LSNNode *) dsa_get_address(lsn_dsa, node_dp);
		node->lsn = state->ReadRecPtr;
		node->next = InvalidDsaPointer;

		/* Append to tail of linked list */
		if (!DsaPointerIsValid(entry->lsn_tail))
		{
			entry->lsn_head = node_dp;
			entry->lsn_tail = node_dp;
		}
		else
		{
			LSNNode    *tail = (LSNNode *) dsa_get_address(lsn_dsa, entry->lsn_tail);

			tail->next = node_dp;
			entry->lsn_tail = node_dp;
		}

		/* Track max LSN for quick skip checks */
		if (!found || state->ReadRecPtr > entry->max_lsn)
			entry->max_lsn = state->ReadRecPtr;

		dshash_release_lock(lsn_hash, entry);
	}

	scan_records_indexed++;
}

/* ----------------------------------------------------------------
 *  Forgetting pages that WAL replay removes
 * ----------------------------------------------------------------
 *
 * While the startup process builds the index, replaying a record that drops
 * a relation, truncates one, or drops a database removes pages whose pending
 * records must never be replayed.  Reading a dropped relation's pages would
 * fail, because its files are gone; a truncated page could be created again
 * later, at LSN 0, and a stale pre-truncation record would then be applied
 * to it.
 *
 * Only entries indexed so far are removed.  WAL is scanned in LSN order, so
 * these are exactly the records written before the drop or truncation;
 * should the relfilenumber or the block be used again later in the WAL, the
 * new entries are added after this and kept.
 *
 * Each call scans the whole index, which is fine for a handful of drops.  A
 * workload dropping many relations would want a per-relation list of the
 * blocks in the index.
 *
 * A truncation also clears the tail of the last surviving FSM and VM page.
 * To do so it reads that page through the buffer manager, which replays its
 * pending records first, up to the truncation, and removes its entry.  The
 * page then sits in shared buffers until a later record for it is indexed;
 * see LSNIndexPrepareToDefer() for what happens then.
 */

/* Free a page's list of LSNs. */
static void
free_lsn_list(dsa_pointer node_dp)
{
	while (DsaPointerIsValid(node_dp))
	{
		dsa_pointer next_dp;

		next_dp = ((LSNNode *) dsa_get_address(lsn_dsa, node_dp))->next;
		dsa_free(lsn_dsa, node_dp);
		node_dp = next_dp;
	}
}

/*
 * LSNIndexForgetRelation - forget the pages of a relation fork from block
 * 'minblkno' on.
 *
 * Called from XLogDropRelation() with 0 and from XLogTruncateRelation() with
 * the new length, next to forgetting the fork's invalid pages (compare
 * forget_invalid_pages() in xlogutils.c).
 */
void
LSNIndexForgetRelation(RelFileLocator rlocator, ForkNumber forknum,
					   BlockNumber minblkno)
{
	dshash_seq_status seq;
	PageLSNEntry *entry;
	int			nforgotten = 0;

	/* Only while the index is being built, by the process building it. */
	if (lsn_hash == NULL || !InRecovery)
		return;

	dshash_seq_init(&seq, lsn_hash, true);
	while ((entry = (PageLSNEntry *) dshash_seq_next(&seq)) != NULL)
	{
		RelFileLocator tag_rlocator = BufTagGetRelFileLocator(&entry->tag);

		if (RelFileLocatorEquals(tag_rlocator, rlocator) &&
			BufTagGetForkNum(&entry->tag) == forknum &&
			entry->tag.blockNum >= minblkno)
		{
			free_lsn_list(entry->lsn_head);
			dshash_delete_current(&seq);
			nforgotten++;
		}
	}
	dshash_seq_term(&seq);

	if (nforgotten > 0)
		elog(DEBUG1, "fast recovery: forgot %d pages of relation %u/%u/%u fork %d from block %u",
			 nforgotten, rlocator.spcOid, rlocator.dbOid, rlocator.relNumber,
			 forknum, minblkno);
}

/*
 * LSNIndexForgetDatabase - forget every page of a database.
 *
 * Called from XLogDropDatabase() (compare forget_invalid_pages_db()).
 */
void
LSNIndexForgetDatabase(Oid dbid)
{
	dshash_seq_status seq;
	PageLSNEntry *entry;
	int			nforgotten = 0;

	if (lsn_hash == NULL || !InRecovery)
		return;

	dshash_seq_init(&seq, lsn_hash, true);
	while ((entry = (PageLSNEntry *) dshash_seq_next(&seq)) != NULL)
	{
		if (entry->tag.dbOid == dbid)
		{
			free_lsn_list(entry->lsn_head);
			dshash_delete_current(&seq);
			nforgotten++;
		}
	}
	dshash_seq_term(&seq);

	if (nforgotten > 0)
		elog(DEBUG1, "fast recovery: forgot %d pages of dropped database %u",
			 nforgotten, dbid);
}

/*
 * LSNIndexPrepareToDefer - may the startup process index this record instead
 * of replaying it?  Makes it so where needed.
 *
 * A record for a page that is in shared buffers cannot just be indexed.  A
 * page is replayed on demand when it is read from disk, and a resident page
 * is not read again while it stays in the pool, so the record would never be
 * applied and the page would be handed out stale.  During the scan a page
 * gets into the pool only when an eagerly replayed record reads it: a
 * truncation reads the last surviving FSM and VM page to clear their tails.
 * That read replayed the page's indexed records up to the truncation and
 * removed its entry.
 *
 * So write such a page out, drop it from the pool, and index the record like
 * any other: the next read of the page is a miss and replays it on demand.
 * The write is needed in any case, since the tail clear has no record of its
 * own.  An earlier version kept the page current instead, by replaying every
 * later record for it at once.  But replaying a record reads its other pages
 * into the pool, which then had to be kept current the same way, and so on:
 * one truncation of a table that fits in one VM page (about 255 MB of heap)
 * made the rest of that table's records eager.
 *
 * EvictUnpinnedBuffer() is documented as racy because the buffer may be
 * reassigned between the lookup and the eviction; that takes a concurrent
 * allocation, and during the scan no other process allocates buffers.  The
 * background writer may hold a transient pin while writing the page, in
 * which case the eviction fails and the record is replayed now, as before.
 *
 * Records for the init fork of an unlogged relation are always replayed now:
 * at the end of recovery the init fork is copied to the main fork without
 * going through shared buffers, so it must be on disk by then.
 */
bool
LSNIndexPrepareToDefer(XLogReaderState *record)
{
	for (int block_id = 0; block_id <= record->record->max_block_id; block_id++)
	{
		RelFileLocator rlocator;
		ForkNumber	forknum;
		BlockNumber blkno;
		BufferTag	tag;
		uint32		hash;
		LWLock	   *partitionLock;
		int			buf_id;
		bool		flushed;

		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		XLogRecGetBlockTag(record, block_id, &rlocator, &forknum, &blkno);
		if (forknum == INIT_FORKNUM)
		{
			scan_records_replayed++;
			return false;
		}

		InitBufferTag(&tag, &rlocator, forknum, blkno);
		hash = BufTableHashCode(&tag);
		partitionLock = BufMappingPartitionLock(hash);
		LWLockAcquire(partitionLock, LW_SHARED);
		buf_id = BufTableLookup(&tag, hash);
		LWLockRelease(partitionLock);

		if (buf_id < 0)
			continue;

		if (!EvictUnpinnedBuffer(BufferDescriptorGetBuffer(GetBufferDescriptor(buf_id)),
								 &flushed))
		{
			scan_records_replayed++;
			return false;
		}

		scan_pages_evicted++;
		elog(DEBUG1, "fast recovery: evicted block %u of relation %u/%u/%u fork %d to index its records",
			 blkno, rlocator.spcOid, rlocator.dbOid, rlocator.relNumber, forknum);
	}

	return true;
}

/*
 * LSNIndexLogScanSummary - report what the scan did, once it is over.
 */
void
LSNIndexLogScanSummary(void)
{
	if (lsn_dsa == NULL)
		return;

	ereport(LOG,
			errmsg("fast crash recovery: deferred %lld records for %lld pages (%zu MB of index), extended files by %lld blocks; replayed %lld records with block references immediately, evicting %lld pages",
				   (long long) scan_records_indexed,
				   (long long) scan_pages_indexed,
				   dsa_get_total_size(lsn_dsa) / (1024 * 1024),
				   (long long) scan_blocks_extended,
				   (long long) scan_records_replayed,
				   (long long) scan_pages_evicted));
}

/* ----------------------------------------------------------------
 *  On-demand WAL replay for a single page
 * ----------------------------------------------------------------
 */

/*
 * LSNIndexScratchBuffer - a throwaway buffer for one of the current record's
 * other pages.
 *
 * While a page is replayed on demand, the record's other pages are skipped:
 * their own replay applies the record to them.  Redo routines that
 * initialize such a page still need a buffer to write into, so give them a
 * local one, and drop it as soon as the record is done, before it could ever
 * be written out under the other page's identity.
 */
Buffer
LSNIndexScratchBuffer(RelFileLocator rlocator, ForkNumber forknum,
					  BlockNumber blkno)
{
	SMgrRelation smgr = smgropen(rlocator, INVALID_PROC_NUMBER);
	BufferDesc *bufHdr;
	bool		found;

	if (nscratch >= lengthof(scratch_tags))
		elog(ERROR, "too many scratch buffers for one WAL record");

	bufHdr = LocalBufferAlloc(smgr, forknum, blkno, &found);
	InitBufferTag(&scratch_tags[nscratch++], &rlocator, forknum, blkno);

	return BufferDescriptorGetBuffer(bufHdr);
}

/* Drop the scratch buffers handed out so far. */
static void
drop_scratch_buffers(void)
{
	for (int i = 0; i < nscratch; i++)
	{
		RelFileLocator rlocator = BufTagGetRelFileLocator(&scratch_tags[i]);
		ForkNumber	forknum = BufTagGetForkNum(&scratch_tags[i]);
		BlockNumber blkno = scratch_tags[i].blockNum;

		DropRelationLocalBuffers(rlocator, &forknum, 1, &blkno);
	}
	nscratch = 0;
}

/*
 * Error context for on-demand replay, like rm_redo_error_callback() in normal
 * recovery: name the record, and the page being recovered.
 */
static void
ondemand_redo_error_callback(void *arg)
{
	XLogReaderState *record = (XLogReaderState *) arg;
	StringInfoData buf;

	initStringInfo(&buf);
	xlog_outdesc(&buf, record);

	errcontext("on-demand WAL redo at %X/%08X for %s, recovering block %u of relation %s",
			   LSN_FORMAT_ARGS(record->ReadRecPtr), buf.data,
			   targetTag.blockNum,
			   relpathperm(BufTagGetRelFileLocator(&targetTag),
						   BufTagGetForkNum(&targetTag)).str);

	pfree(buf.data);
}

/*
 * Replay, in LSN order, the indexed records that touch the page 'tag'.
 */
static void
ReplayPageRecords(BufferTag *tag, dsa_pointer head_dp)
{
	dsa_pointer node_dp;
	XLogReaderState *xlogreader;
	char	   *errormsg = NULL;
	ErrorContextCallback errcallback;

	xlogreader =
		XLogReaderAllocate(wal_segment_size, NULL,
						   XL_ROUTINE(.page_read = &read_local_xlog_page,
									  .segment_open = wal_segment_open,
									  .segment_close = wal_segment_close),
						   NULL);
	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	for (node_dp = head_dp;
		 DsaPointerIsValid(node_dp);
		)
	{
		LSNNode    *node = (LSNNode *) dsa_get_address(lsn_dsa, node_dp);
		XLogRecPtr	lsn = node->lsn;
		dsa_pointer next_dp = node->next;

		XLogBeginRead(xlogreader, lsn);
		if (!XLogReadRecord(xlogreader, &errormsg))
		{
			if (errormsg)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read WAL at %X/%X: %s",
								LSN_FORMAT_ARGS(lsn), errormsg)));
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read WAL at %X/%X",
								LSN_FORMAT_ARGS(lsn))));
		}

		/* Find the block reference matching our target tag and replay */
		for (int blk_id = 0; blk_id <= xlogreader->record->max_block_id; blk_id++)
		{
			RelFileLocator rlocator;
			ForkNumber	forknum;
			BlockNumber blkno;
			BufferTag	wal_tag;

			if (!XLogRecHasBlockRef(xlogreader, blk_id))
				continue;

			XLogRecGetBlockTag(xlogreader, blk_id, &rlocator, &forknum, &blkno);
			InitBufferTag(&wal_tag, &rlocator, forknum, blkno);

			if (BufferTagsEqual(tag, &wal_tag))
			{
				errcallback.callback = ondemand_redo_error_callback;
				errcallback.arg = (void *) xlogreader;
				errcallback.previous = error_context_stack;
				error_context_stack = &errcallback;

				GetRmgr(XLogRecGetRmid(xlogreader)).rm_redo(xlogreader);

				error_context_stack = errcallback.previous;

				/* the record is in; its other pages' scratch buffers can go */
				drop_scratch_buffers();
				break;
			}
		}

		node_dp = next_dp;
	}

	XLogReaderFree(xlogreader);
}

/*
 * Start rmgr-specific recovery state for on-demand replay, once per process.
 *
 * The startup process has its own, set up by PerformWalRecovery(), and must
 * not call RmgrStartup() again: gin, gist and spgist keep a static memory
 * context that their startup creates and their cleanup deletes, so a nested
 * call would pull it out from under the scan.  Anywhere else, start them
 * once, in TopMemoryContext so that the contexts outlive the query that
 * happened to read the first page, and keep them until the process exits.
 */
static void
StartRmgrsForOnDemandReplay(void)
{
	MemoryContext oldcxt;

	if (rmgrs_started || AmStartupProcess())
		return;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	RmgrStartup();
	MemoryContextSwitchTo(oldcxt);
	rmgrs_started = true;
}

/*
 * LSNIndexBeginPageReplay - does this page have pending records?  If so,
 * register us as replaying it.
 *
 * Called by the buffer manager for a page it's about to read from disk.  On
 * false, there's nothing to replay: read the page as usual.  On true, the
 * caller must read the page, replay it, mark it valid, and call
 * LSNIndexEndPageReplay(); if any of that fails, LSNIndexErrorCleanup()
 * undoes it instead.  Until then FastRecoveryInProgress() counts us, so no
 * checkpoint can start before the page is valid and dirty.
 *
 * is_active is checked and the counter bumped under LSNIndexLock, so
 * LSNIndexFinish(), which clears is_active under the same lock in exclusive
 * mode, sees every replay that got in.
 *
 * Replays never nest.  Redo of an indexed page reaches that page through
 * XLogReadBufferExtended(), which hands it the buffer being replayed into;
 * the record's other pages are skipped by XLogReadBufferForRedoExtended(),
 * and the free space map is left alone by XLogRecordPageWithFreeSpace().  So
 * nothing redo does can bring us back here, and a read that does is a bug: it
 * would wait forever for the I/O we hold if it's the same page, or replay
 * another page inside this one's redo if not.  (The startup process can get
 * here from its own, eager redo of a record that touches an indexed page;
 * that's one level, not nesting, since the replay it starts reads nothing.)
 */
bool
LSNIndexBeginPageReplay(const BufferTag *tag)
{
	PageLSNEntry *entry;
	bool		pending = false;

	if (LSNIndexCtl == NULL || !LSNIndexCtl->is_active)
		return false;

	/* Attach before taking LSNIndexLock: LSNIndexAttach() takes it too. */
	if (lsn_hash == NULL)
		LSNIndexAttach();
	if (lsn_hash == NULL)
		return false;

	if (replay_begun)
		elog(ERROR, "block %u of relation %s read during on-demand replay of block %u of relation %s",
			 tag->blockNum,
			 relpathperm(BufTagGetRelFileLocator(tag),
						 BufTagGetForkNum(tag)).str,
			 replaying_tag.blockNum,
			 relpathperm(BufTagGetRelFileLocator(&replaying_tag),
						 BufTagGetForkNum(&replaying_tag)).str);

	LWLockAcquire(LSNIndexLock, LW_SHARED);
	if (LSNIndexCtl->is_active)
	{
		entry = (PageLSNEntry *) dshash_find(lsn_hash, tag, false);
		if (entry != NULL)
		{
			dshash_release_lock(lsn_hash, entry);
			pg_atomic_fetch_add_u32(&LSNIndexCtl->nreplaying, 1);
			pending = true;
		}
	}
	LWLockRelease(LSNIndexLock);

	if (pending)
	{
		replay_begun = true;
		replaying_tag = *tag;
	}

	return pending;
}

/*
 * LSNIndexEndPageReplay - undo LSNIndexBeginPageReplay().
 */
void
LSNIndexEndPageReplay(void)
{
	Assert(replay_begun);
	replay_begun = false;
	pg_atomic_fetch_sub_u32(&LSNIndexCtl->nreplaying, 1);
}

/*
 * LSNIndexErrorCleanup - undo an on-demand replay that an error cut short.
 *
 * Called from AbortTransaction() and AbortSubTransaction(), next to
 * pgaio_error_cleanup() and before the resource owner that started the
 * buffer's I/O ends it, and at process exit.  Redo dirties the buffer before
 * it is valid, which nothing else ever does, and AbortBufferIO() rightly
 * insists that an invalid buffer is clean; so clean it here, and let the
 * resource owner end the I/O as for any failed read.  The page was never
 * valid and its entry is still in the index, so the next reader starts over.
 * Then forget the replay, so that this process can read pages again and
 * stops holding checkpoints off.
 *
 * Scratch buffers handed to redo may still be pinned at this point; the next
 * replay drops them, once the pins are gone.
 */
void
LSNIndexErrorCleanup(void)
{
	if (!replay_begun)
		return;

	if (inReplayPageWals)
	{
		AbortPendingBufferReplay(targetBuffer);
		inReplayPageWals = false;
		targetBuffer = InvalidBuffer;
		InRecovery = saved_in_recovery;
	}

	LSNIndexEndPageReplay();
}

/*
 * LSNIndexReplayIntoBuffer - apply a page's pending records to its buffer.
 *
 * The caller holds 'buffer' pinned and its I/O-in-progress flag, and has
 * just read the page from disk into it.  Nobody else can see the page until
 * the caller marks it valid, so redo needs no lock to keep readers out, and
 * nobody can be holding a pointer into the page.  Redo reaches this page
 * through XLogReadBufferExtended(), which hands it this buffer instead of
 * reading the page again; the record's other pages are skipped, because
 * their own replay applies the record to them.
 */
void
LSNIndexReplayIntoBuffer(Buffer buffer)
{
	BufferTag	tag = GetBufferDescriptor(buffer - 1)->tag;
	PageLSNEntry *entry;
	dsa_pointer head_dp;

	Assert(replay_begun && BufferTagsEqual(&replaying_tag, &tag));
	Assert(!inReplayPageWals);

	/*
	 * Scratch buffers left over by a replay that failed: their pins are gone
	 * with the aborted transaction, so they can go now.
	 */
	if (nscratch > 0)
		drop_scratch_buffers();

	/* Only the process holding the page's I/O retires its entry: us. */
	entry = (PageLSNEntry *) dshash_find(lsn_hash, &tag, false);
	if (entry == NULL)
		elog(ERROR, "no pending WAL records for block %u of relation %s",
			 tag.blockNum,
			 relpathperm(BufTagGetRelFileLocator(&tag),
						 BufTagGetForkNum(&tag)).str);
	head_dp = entry->lsn_head;
	dshash_release_lock(lsn_hash, entry);

	StartRmgrsForOnDemandReplay();

	/*
	 * Redo routines were written to run in the startup process and use
	 * InRecovery to mean "applying WAL, not generating it":
	 * visibilitymap_set() asserts it, since outside recovery setting a bit
	 * must happen in the critical section that logs it; mdreadv() zero-fills
	 * a short read under it, as recovery of a relation the OS crash left
	 * short requires.  On-demand replay is that same work done later, in
	 * another process, so say so for the duration of the redo calls.  Nothing
	 * redo does under closure consults the flag for its other meaning, "I am
	 * the startup process": it reads no page but the one it's handed.
	 *
	 * Should redo fail, LSNIndexErrorCleanup() puts all of this back.
	 */
	saved_in_recovery = InRecovery;
	InRecovery = true;
	inReplayPageWals = true;
	targetTag = tag;
	targetBuffer = buffer;

	ReplayPageRecords(&tag, head_dp);

	inReplayPageWals = false;
	targetBuffer = InvalidBuffer;
	InRecovery = saved_in_recovery;
}

/*
 * LSNIndexForgetPage - drop a page's pending records.
 *
 * Called once a replayed page has been marked valid: a valid page is
 * current, and must never be replayed again (a full-page image in its list
 * would roll it back).  Also called when a page is about to be zeroed for
 * reuse, as its old records no longer apply.  The caller holds the buffer
 * pinned, so the page can't be evicted and read back in before we're done.
 */
void
LSNIndexForgetPage(const BufferTag *tag)
{
	PageLSNEntry *entry;

	if (LSNIndexCtl == NULL || !LSNIndexCtl->is_active)
		return;
	if (lsn_hash == NULL)
		LSNIndexAttach();
	if (lsn_hash == NULL)
		return;

	/* is_active, seen under the lock, keeps LSNIndexFinish() from freeing it */
	LWLockAcquire(LSNIndexLock, LW_SHARED);
	if (LSNIndexCtl->is_active)
	{
		entry = (PageLSNEntry *) dshash_find(lsn_hash, tag, true);
		if (entry != NULL)
		{
			free_lsn_list(entry->lsn_head);
			dshash_delete_entry(lsn_hash, entry);
		}
	}
	LWLockRelease(LSNIndexLock);
}

/* ----------------------------------------------------------------
 *  Accessor for the dshash table (used by fast recovery worker)
 * ----------------------------------------------------------------
 */

dshash_table *
LSNIndexGetHash(void)
{
	Assert(lsn_hash != NULL);
	return lsn_hash;
}

/* ----------------------------------------------------------------
 *  End of fast recovery
 * ----------------------------------------------------------------
 */

/*
 * FastRecoveryInProgress - may pre-crash WAL still be replayed onto a page?
 *
 * True while the index is active, and afterwards until the last replay that
 * was already running has finished.  No checkpoint may start while this is
 * true.  A completed checkpoint promises that everything before its redo
 * point is on disk, and a crash after it starts recovery at that redo point.
 * A page that has not been replayed yet, or that a running replay dirties
 * after the checkpoint has collected its dirty buffers, would then never get
 * its pre-crash WAL replayed.
 */
bool
FastRecoveryInProgress(void)
{
	bool		result;

	if (LSNIndexCtl == NULL)
		return false;

	LWLockAcquire(LSNIndexLock, LW_SHARED);
	result = LSNIndexCtl->is_active ||
		pg_atomic_read_u32(&LSNIndexCtl->nreplaying) > 0;
	LWLockRelease(LSNIndexLock);

	return result;
}

/*
 * Free the DSA and dshash.  Only safe once nobody can reach them anymore.
 */
static void
LSNIndexDestroy(void)
{
	Assert(!LSNIndexCtl->is_active);
	Assert(pg_atomic_read_u32(&LSNIndexCtl->nreplaying) == 0);

	if (lsn_hash != NULL)
	{
		dshash_destroy(lsn_hash);
		lsn_hash = NULL;
	}
	if (lsn_dsa != NULL)
	{
		dsa_unpin(lsn_dsa);
		dsa_detach(lsn_dsa);
		lsn_dsa = NULL;
	}

	LWLockAcquire(LSNIndexLock, LW_EXCLUSIVE);
	LSNIndexCtl->dsa_handle = DSA_HANDLE_INVALID;
	LSNIndexCtl->hash_handle = DSHASH_HANDLE_INVALID;
	LWLockRelease(LSNIndexLock);
}

/*
 * LSNIndexFinish - end fast recovery.
 *
 * Called by the fast recovery worker once it has read every page in the
 * index.  Stops new on-demand replays, waits for the ones already running,
 * and only then frees the index.  FastRecoveryInProgress() turns false when
 * the wait is over, not before.
 */
void
LSNIndexFinish(void)
{
	LWLockAcquire(LSNIndexLock, LW_EXCLUSIVE);
	LSNIndexCtl->is_active = false;
	LWLockRelease(LSNIndexLock);

	while (pg_atomic_read_u32(&LSNIndexCtl->nreplaying) > 0)
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10L, WAIT_EVENT_FAST_RECOVERY_DRAIN);
		ResetLatch(MyLatch);
	}

	LSNIndexDestroy();
}
