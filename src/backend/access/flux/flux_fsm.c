/*-------------------------------------------------------------------------
 *
 * flux_fsm.c
 *	  FLUX free space management
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_fsm.c
 *
 * NOTES
 *	  This is a thin wrapper over PostgreSQL's standard free space map
 *	  (src/backend/storage/freespace).  It adds FLUX-specific relation
 *	  extension (page initialization and WAL-logging of the new page) on
 *	  top of the generic GetPageWithFreeSpace/RecordPageWithFreeSpace API.
 *
 *	  Page defragmentation is handled entirely by VACUUM via
 *	  FluxPageDefragment() in flux_tuple.c; the FSM only tracks free
 *	  space, it does not schedule or perform compaction.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/flux.h"
#include "access/flux_xlog.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "utils/rel.h"
#include "miscadmin.h"

/*
 * FluxGetPageWithFreeSpace
 *
 * Find or create a page with at least 'needed' bytes of free space.
 *
 * First queries PostgreSQL's standard FSM (GetPageWithFreeSpace).  If a
 * page is returned, that block is used directly.
 *
 * If no suitable existing page is found, extends the relation by allocating
 * a new page with ExtendBufferedRel(), initializes it with FluxInitPage(),
 * WAL-logs the initialization, and returns the new block.
 *
 * Parameters:
 *   rel    - open relation
 *   needed - minimum number of free bytes required
 *
 * Returns the block number of a page with sufficient free space.
 */
/*
 * FluxTargetFreeSpace
 *
 * Convert a raw tuple length into the amount of free space an insert should
 * ASK the FSM for, applying the fillfactor reservation the way heap does.
 *
 * Heap does this in RelationGetBufferForTuple (hio.c), and the clamp it
 * applies is not optional.  Adding the fillfactor reservation unconditionally
 * can push the request past what the FSM can represent (MaxFSMRequestSize ==
 * MaxHeapTupleSize); fsm_space_needed_to_cat() then rejects it outright with
 * "invalid FSM request size", which surfaces as a hard ERROR on a perfectly
 * legal INSERT.  FLUX hit exactly this: with fillfactor=10 the reservation is
 * 7428 bytes, so a 1000-byte row asked for 8428 and the INSERT failed where
 * heap accepted it (regression tests insert, brin, brin_bloom).
 *
 * The clamp must be expressed in FLUX's OWN page geometry, not heap's.  A FLUX
 * page reserves special space (FluxPageOpaqueData) that a heap page does not,
 * so the largest tuple that can ever live on a FLUX page is
 * FLUX_MAX_TUPLE_SIZE, which is strictly smaller than MaxHeapTupleSize.  Using
 * heap's bound here asks for more free space than any FLUX page can offer, the
 * FSM can never satisfy it, and the insert path's "retry until we find a
 * suitable page" loop spins forever -- an INSERT that HANGS instead of erroring.
 * Hence every quantity below is FLUX-native.
 *
 * Rule, mirroring heap: a nearly-empty page is good enough for a tuple that is
 * oversize relative to fillfactor, so once the reservation would push us past
 * nearlyEmptyFreeSpace we ask only for what the tuple itself needs (but at
 * least nearlyEmptyFreeSpace, so we don't settle for a page with a few spare
 * bytes).  That keeps the request satisfiable AND inside the FSM's range,
 * while preserving "don't extend the relation needlessly for a big row in a
 * low-fillfactor table".
 *
 * The result is only a hint: every caller re-checks the actual free space under
 * its own buffer lock and falls back to extending the relation.
 */
Size
FluxTargetFreeSpace(Relation rel, Size len)
{
	Size		saveFreeSpace;
	Size		nearlyEmptyFreeSpace;

	saveFreeSpace = RelationGetTargetPageFreeSpace(rel, FLUX_DEFAULT_FILLFACTOR);

	/*
	 * Pages with no live tuples can still carry line pointers, so allow a
	 * small slack before calling a page "nearly empty" (heap's reasoning, in
	 * FLUX's geometry).
	 */
	nearlyEmptyFreeSpace = FLUX_MAX_TUPLE_SIZE -
		(FLUX_MAX_TUPLE_SIZE / BLCKSZ + 32 * sizeof(ItemIdData));

	if (len + saveFreeSpace > nearlyEmptyFreeSpace)
	{
		/*
		 * Never ask for more than a FLUX page can physically hold, or the FSM
		 * can never answer and the caller's retry loop never terminates.
		 */
		Size		want = Max(len, nearlyEmptyFreeSpace);

		return Min(want, (Size) FLUX_MAX_TUPLE_SIZE);
	}

	return len + saveFreeSpace;
}

BlockNumber
FluxGetPageWithFreeSpace(Relation rel, Size needed)
{
	BlockNumber target_block;
	Buffer		buffer;
	Page		page;
	Size		free_space;

	/*
	 * Ask the FSM for a page with enough free space.  Note: we do NOT verify
	 * the page by locking it here, because callers may already hold buffer
	 * locks (e.g., the update path holds the old tuple's page lock).  Locking
	 * a page here would risk self-deadlock if the FSM returns a block that
	 * the caller already has locked.  Callers are responsible for rechecking
	 * free space after they acquire their own lock on the returned page.
	 */
	target_block = GetPageWithFreeSpace(rel, needed);

	if (target_block != InvalidBlockNumber)
		return target_block;

	/*
	 * No suitable page found -- extend the relation.
	 *
	 * Use the modern ExtendBufferedRel() API which properly handles
	 * concurrent extension by multiple backends.  The old
	 * ReadBufferExtended(P_NEW) path had a race condition that caused
	 * BM_IO_IN_PROGRESS assertion failures under concurrency.
	 */
	buffer = ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM, NULL,
							   EB_LOCK_FIRST);
	target_block = BufferGetBlockNumber(buffer);

	page = BufferGetPage(buffer);
	FluxInitPage(page, BufferGetPageSize(buffer));

	START_CRIT_SECTION();

	MarkBufferDirty(buffer);

	/* Log page initialization */
	if (RelationNeedsWAL(rel))
	{
		uint64		init_commit_ts = FluxGetCommitTimestamp();
		FluxPageOpaque phdr;
		XLogRecPtr	recptr;

		/*
		 * Set the page's opaque data to match what the REDO handler will
		 * produce.  This is essential for WAL consistency checking: the page
		 * image stored with the WAL record must match what REDO generates
		 * when replaying.
		 */
		phdr = FluxPageGetOpaque(page);
		FluxPageSetCommitTs(phdr, init_commit_ts);

		recptr = FluxXLogInitPage(rel, buffer, 0, init_commit_ts);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/* Capture free space before releasing the buffer */
	free_space = PageGetFreeSpace(page);

	UnlockReleaseBuffer(buffer);

	/* Record the new page in FSM */
	FluxRecordFreeSpace(rel, target_block, free_space);

	/*
	 * Propagate the new FSM leaf value up through the FSM tree so that
	 * subsequent GetPageWithFreeSpace() calls can find it.  Without this, the
	 * root of the FSM tree remains at zero and all searches fail.
	 */
	FreeSpaceMapVacuumRange(rel, target_block, target_block + 1);

	return target_block;
}

/*
 * FluxRecordFreeSpace
 *
 * Update the FSM with the actual free space for a page.
 *
 * Parameters:
 *   rel       - open relation
 *   page      - block number of the page
 *   freespace - actual free space in bytes on the page
 */
void
FluxRecordFreeSpace(Relation rel, BlockNumber page, Size freespace)
{
	RecordPageWithFreeSpace(rel, page, freespace);
}
