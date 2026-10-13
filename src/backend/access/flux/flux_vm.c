/*-------------------------------------------------------------------------
 *
 * flux_vm.c
 *	  Visibility Map implementation for FLUX
 *
 * The Visibility Map (VM) tracks the visibility status of pages in a FLUX
 * relation. It stores two bits per heap page:
 *
 * - ALL_VISIBLE: All tuples on the page are visible to all transactions
 * - ALL_FROZEN: All tuples on the page are frozen (transaction IDs removed)
 *
 * The VM enables two critical optimizations:
 * 1. Index-only scans can skip heap fetches for all-visible pages
 * 2. VACUUM can skip pages that are already all-visible or all-frozen
 *
 * The VM is stored in a separate fork of the relation (VISIBILITYMAP_FORKNUM)
 * and is WAL-logged for crash recovery.
 *
 * This implementation is based on the heap visibility map
 * (src/backend/access/heap/visibilitymap.c) but adapted for FLUX's
 * timestamp-based MVCC model.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/flux/flux_vm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/flux.h"
#include "access/flux_xlog.h"
#include "access/visibilitymapdefs.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/inval.h"
#include "utils/rel.h"

/*
 * Size of the bitmap on each visibility map page, in bytes. There's no
 * extra headers, so the whole page minus the standard page header is
 * used for the bitmap.
 */
#define MAPSIZE (BLCKSZ - MAXALIGN(SizeOfPageHeaderData))

/* Number of heap blocks we can represent in one VM page */
#define HEAPBLOCKS_PER_PAGE (MAPSIZE * 4)

/* Mapping macros */
#define HEAPBLK_TO_MAPBLOCK(x) ((x) / HEAPBLOCKS_PER_PAGE)
#define HEAPBLK_TO_MAPBYTE(x) (((x) % HEAPBLOCKS_PER_PAGE) / 4)
#define HEAPBLK_TO_OFFSET(x) (((x) % HEAPBLOCKS_PER_PAGE) % 4)

/* Bit manipulation - use FLUX-specific values that match PostgreSQL's VM bits */

/* Forward declaration */
static Buffer flux_vm_extend(Relation rel, BlockNumber vm_nblocks);
static Buffer flux_vm_readbuf(Relation rel, BlockNumber blkno, bool extend);

/*
 * FluxVMInit - Initialize visibility map for a FLUX relation
 *
 * This is called when a FLUX table is created to ensure the VM fork exists.
 */
void
FluxVMInit(Relation rel)
{
	/*
	 * Create the visibility map fork if it doesn't exist. This happens
	 * automatically when we first try to extend it via flux_vm_extend().
	 */
}

/*
 * FluxVMSet - Set visibility map bits for a page
 *
 * Sets the specified bits for the given heap block. The heap buffer must
 * be exclusively locked. The VM buffer will be pinned and locked as needed.
 */
void
FluxVMSet(Relation rel, BlockNumber heapBlk, Buffer heapBuf, uint8 flags, TransactionId conflict_xid)
{
	BlockNumber mapBlock;
	uint32		mapByte;
	uint8		mapOffset;
	Page		page;
	uint8	   *map;
	Buffer		vmBuf;

	Assert(BufferIsValid(heapBuf));
	/* Buffer should be exclusively locked */

	/* Only set valid bits */
	flags &= FLUX_VM_VALID_BITS;
	if (flags == 0)
		return;

	/* Calculate the VM page and offset for this heap block */
	mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	mapOffset = HEAPBLK_TO_OFFSET(heapBlk);

	/*
	 * Read or extend the visibility map buffer.  flux_vm_readbuf() will
	 * create the VM fork if it doesn't exist yet.
	 */
	vmBuf = flux_vm_readbuf(rel, mapBlock, true);
	LockBuffer(vmBuf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(vmBuf);

	/* If the page is new, initialize it */
	if (PageIsNew(page))
		PageInit(page, BLCKSZ, 0);

	map = (uint8 *) PageGetContents(page);

	/* Set the bits for this heap block */
	map[mapByte] |= (flags << (mapOffset * 2));

	MarkBufferDirty(vmBuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_flux_vm_set xlrec;
		XLogRecPtr	recptr;

		xlrec.conflict_xid = conflict_xid;
		xlrec.heapBlk = heapBlk;
		xlrec.flags = flags;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));

		/*
		 * Register the heap buffer with REGBUF_NO_IMAGE.  We reference the
		 * heap page so that redo can update its LSN, but we do NOT need a
		 * full-page image of the heap page in this WAL record. The heap
		 * buffer may not be dirty (e.g., during VACUUM VM updates), so we
		 * must not let XLogInsert try to take an FPI of it -- that would trip
		 * the BufferIsDirty assertion.
		 */
		XLogRegisterBuffer(0, heapBuf, REGBUF_NO_IMAGE | REGBUF_NO_CHANGE);

		/*
		 * Not REGBUF_STANDARD: the map lives between pd_lower and pd_upper,
		 * which a standard page image leaves out, and redo of a restored page
		 * applies nothing, so every bit on the page would be lost.
		 */
		XLogRegisterBuffer(1, vmBuf, 0);

		recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_VM_SET);
		PageSetLSN(page, recptr);
	}

	UnlockReleaseBuffer(vmBuf);
}

/*
 * FluxVMClear - Clear visibility map bits for a page
 *
 * Clears the specified bits for the given heap block. The heap buffer must
 * be exclusively locked.
 */
void
FluxVMClear(Relation rel, BlockNumber heapBlk, Buffer heapBuf, uint8 flags)
{
	BlockNumber mapBlock;
	uint32		mapByte;
	uint8		mapOffset;
	Page		page;
	uint8	   *map;
	Buffer		vmBuf;

	Assert(BufferIsValid(heapBuf));
	/* Buffer should be exclusively locked */

	/* Only clear valid bits */
	flags &= FLUX_VM_VALID_BITS;
	if (flags == 0)
		return;

	/* Calculate the VM page and offset for this heap block */
	mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	mapOffset = HEAPBLK_TO_OFFSET(heapBlk);

	/* Check if the VM fork/page exists; if not, nothing to clear */
	if (!smgrexists(RelationGetSmgr(rel), VISIBILITYMAP_FORKNUM))
		return;
	if (mapBlock >= RelationGetNumberOfBlocksInFork(rel, VISIBILITYMAP_FORKNUM))
		return;

	vmBuf = ReadBufferExtended(rel, VISIBILITYMAP_FORKNUM, mapBlock,
							   RBM_NORMAL, NULL);
	LockBuffer(vmBuf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(vmBuf);
	map = (uint8 *) PageGetContents(page);

	/*
	 * Check if the requested bits are already clear.  If so, skip the
	 * modification and WAL logging entirely.  This is the common case after
	 * the first modification to a page since the last VACUUM, and avoids
	 * significant WAL amplification on hot pages.
	 */
	if ((map[mapByte] & (flags << (mapOffset * 2))) == 0)
	{
		UnlockReleaseBuffer(vmBuf);
		return;
	}

	/* Clear the bits for this heap block */
	map[mapByte] &= ~(flags << (mapOffset * 2));

	MarkBufferDirty(vmBuf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_flux_vm_clear xlrec;
		XLogRecPtr	recptr;

		xlrec.heapBlk = heapBlk;
		xlrec.flags = flags;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));

		/*
		 * Register the heap buffer with REGBUF_NO_IMAGE for the same reason
		 * as in FluxVMSet: the heap buffer may not be dirty.
		 */
		XLogRegisterBuffer(0, heapBuf, REGBUF_NO_IMAGE | REGBUF_NO_CHANGE);
		/* Not REGBUF_STANDARD, see FluxVMSet. */
		XLogRegisterBuffer(1, vmBuf, 0);

		recptr = XLogInsert(RM_FLUX_ID, XLOG_FLUX_VM_CLEAR);
		PageSetLSN(page, recptr);
	}

	UnlockReleaseBuffer(vmBuf);
}

/*
 * FluxVMCheck - Check visibility map bits for a page
 *
 * Returns true if ALL the specified bits are set for the given heap block.
 * This function does not require any locks and can be called from
 * index-only scan paths.
 */
bool
FluxVMCheck(Relation rel, BlockNumber heapBlk, uint8 flags)
{
	BlockNumber mapBlock;
	uint32		mapByte;
	uint8		mapOffset;
	Page		page;
	uint8	   *map;
	Buffer		vmBuf;
	bool		result;

	/* Only check valid bits */
	flags &= FLUX_VM_VALID_BITS;
	if (flags == 0)
		return true;			/* No bits to check */

	/* Calculate the VM page and offset for this heap block */
	mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	mapOffset = HEAPBLK_TO_OFFSET(heapBlk);

	/* If the VM fork/page doesn't exist, the bits can't be set */
	if (!smgrexists(RelationGetSmgr(rel), VISIBILITYMAP_FORKNUM))
		return false;
	if (mapBlock >= RelationGetNumberOfBlocksInFork(rel, VISIBILITYMAP_FORKNUM))
		return false;

	vmBuf = ReadBufferExtended(rel, VISIBILITYMAP_FORKNUM, mapBlock,
							   RBM_NORMAL, NULL);
	LockBuffer(vmBuf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(vmBuf);
	map = (uint8 *) PageGetContents(page);

	/* Check if all requested bits are set */
	result = ((map[mapByte] >> (mapOffset * 2)) & flags) == flags;

	UnlockReleaseBuffer(vmBuf);

	return result;
}

/*
 * FluxVMCheckCached - Check visibility map bits with caller-managed buffer cache
 *
 * Like FluxVMCheck, but the caller provides pointers to a cached VM buffer
 * and its block number.  The VM buffer is kept pinned across calls; it is
 * only released and re-read when the heap block maps to a different VM page.
 * This eliminates per-page ReadBufferExtended + UnlockReleaseBuffer overhead
 * for sequential scans (one VM page covers HEAPBLOCKS_PER_PAGE heap pages,
 * typically ~32K pages with 8KB blocks).
 *
 * The caller must release the buffer when done (e.g., at scan end).
 */
bool
FluxVMCheckCached(Relation rel, BlockNumber heapBlk, uint8 flags,
				  Buffer *vmbuf, BlockNumber *vm_blockno)
{
	BlockNumber mapBlock;
	uint32		mapByte;
	uint8		mapOffset;
	Page		page;
	uint8	   *map;
	bool		result;

	/* Only check valid bits */
	flags &= FLUX_VM_VALID_BITS;
	if (flags == 0)
		return true;			/* No bits to check */

	/* Calculate the VM page and offset for this heap block */
	mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);
	mapByte = HEAPBLK_TO_MAPBYTE(heapBlk);
	mapOffset = HEAPBLK_TO_OFFSET(heapBlk);

	/* If the VM fork doesn't exist, the bits can't be set */
	if (!smgrexists(RelationGetSmgr(rel), VISIBILITYMAP_FORKNUM))
		return false;
	if (mapBlock >= RelationGetNumberOfBlocksInFork(rel, VISIBILITYMAP_FORKNUM))
		return false;

	/*
	 * Re-read the VM buffer only when the target VM page changes.  Each VM
	 * page covers HEAPBLOCKS_PER_PAGE heap pages, so for sequential scans
	 * this avoids ~32K redundant buffer reads per VM page.
	 */
	if (!BufferIsValid(*vmbuf) || *vm_blockno != mapBlock)
	{
		if (BufferIsValid(*vmbuf))
			ReleaseBuffer(*vmbuf);
		*vmbuf = ReadBufferExtended(rel, VISIBILITYMAP_FORKNUM, mapBlock,
									RBM_NORMAL, NULL);
		*vm_blockno = mapBlock;
	}

	LockBuffer(*vmbuf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(*vmbuf);
	map = (uint8 *) PageGetContents(page);

	/* Check if all requested bits are set */
	result = ((map[mapByte] >> (mapOffset * 2)) & flags) == flags;

	LockBuffer(*vmbuf, BUFFER_LOCK_UNLOCK);

	return result;
}

/*
 * FluxVMPinBuffer - Pin the visibility map buffer for a heap block
 *
 * This is used when we need to keep the VM buffer pinned across multiple
 * operations. The caller is responsible for unpinning the buffer.
 */
void
FluxVMPinBuffer(Relation rel, BlockNumber heapBlk, Buffer *vmbuf)
{
	BlockNumber mapBlock;

	/* Calculate the VM page for this heap block */
	mapBlock = HEAPBLK_TO_MAPBLOCK(heapBlk);

	/* Pin the buffer if not already pinned */
	if (!BufferIsValid(*vmbuf) || BufferGetBlockNumber(*vmbuf) != mapBlock)
	{
		if (BufferIsValid(*vmbuf))
			ReleaseBuffer(*vmbuf);
		*vmbuf = flux_vm_readbuf(rel, mapBlock, true);
	}
}

/*
 * FluxVMExtend - Extend the visibility map to cover more heap blocks
 *
 * This is called when the heap relation is extended.
 */
void
FluxVMExtend(Relation rel, BlockNumber nheapblocks)
{
	BlockNumber newnblocks;

	/* Calculate how many VM blocks we need */
	newnblocks = (nheapblocks + HEAPBLOCKS_PER_PAGE - 1) / HEAPBLOCKS_PER_PAGE;

	/* Extend the VM fork if necessary, creating it if needed */
	if (newnblocks > 0)
	{
		Buffer		buf;

		buf = flux_vm_extend(rel, newnblocks);
		ReleaseBuffer(buf);
	}
}

/*
 * FluxVMGetPageSize - Get the size of a VM page
 */
Size
FluxVMGetPageSize(void)
{
	return MAPSIZE;
}

/*
 * FluxVMMapHeapToVM - Map a heap block number to VM block number
 */
BlockNumber
FluxVMMapHeapToVM(BlockNumber heapBlk)
{
	return HEAPBLK_TO_MAPBLOCK(heapBlk);
}

/*
 * flux_vm_extend - Extend the VM fork to at least vm_nblocks.
 *
 * Creates the VM fork if it doesn't exist yet.  Returns a buffer for
 * the last block of the extended fork (pinned but not locked).
 */
static Buffer
flux_vm_extend(Relation rel, BlockNumber vm_nblocks)
{
	Buffer		buf;

	buf = ExtendBufferedRelTo(BMR_REL(rel), VISIBILITYMAP_FORKNUM, NULL,
							  EB_CREATE_FORK_IF_NEEDED |
							  EB_CLEAR_SIZE_CACHE,
							  vm_nblocks,
							  RBM_ZERO_ON_ERROR);

	/*
	 * Send a shared-inval message to force other backends to close any smgr
	 * references they may have for this rel, which we are about to change.
	 */
	CacheInvalidateSmgr(RelationGetSmgr(rel)->smgr_rlocator);

	return buf;
}

/*
 * flux_vm_readbuf - Read or extend the VM to get the page for blkno.
 *
 * If extend is true and the block doesn't exist, extends the fork
 * (creating it if needed).  Returns InvalidBuffer if extend is false
 * and the block doesn't exist.  Buffer is returned pinned but not locked.
 */
static Buffer
flux_vm_readbuf(Relation rel, BlockNumber blkno, bool extend)
{
	Buffer		buf;
	SMgrRelation reln = RelationGetSmgr(rel);

	/*
	 * Ensure we have the cached nblocks value for the VM fork.
	 */
	if (reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] == InvalidBlockNumber)
	{
		if (smgrexists(reln, VISIBILITYMAP_FORKNUM))
			smgrnblocks(reln, VISIBILITYMAP_FORKNUM);
		else
			reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = 0;
	}

	if (blkno >= reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM])
	{
		if (extend)
			buf = flux_vm_extend(rel, blkno + 1);
		else
			return InvalidBuffer;
	}
	else
		buf = ReadBufferExtended(rel, VISIBILITYMAP_FORKNUM, blkno,
								 RBM_ZERO_ON_ERROR, NULL);

	/*
	 * Initializing the page when needed is trickier than it looks, because of
	 * the possibility of multiple backends doing this concurrently, and our
	 * desire to not uselessly take the buffer lock in the normal path where
	 * the page is OK.  For a page that's just been extended, this is not
	 * needed since it was already initialized by ExtendBufferedRelTo.
	 */
	if (PageIsNew(BufferGetPage(buf)))
	{
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(BufferGetPage(buf)))
			PageInit(BufferGetPage(buf), BLCKSZ, 0);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}

	return buf;
}

/*
 * Clearing the visibility map for a change to a heap page.
 *
 * An INSERT, UPDATE or DELETE clears the page's PD_ALL_VISIBLE and its map
 * bits in its own critical section and WAL record, as heap_insert,
 * heap_update and heap_delete do, and replay of the record clears them too.
 * Clearing them afterwards, in a record of their own, let a standby that had
 * replayed the change but not the clear, or a crash between the two, keep an
 * all-visible page that holds a version not every snapshot sees.
 *
 * With the heap page exclusively locked and before the critical section,
 * FluxVMPinForChange pins the map page (reading it may need I/O).  In the
 * critical section, before the change's record is assembled,
 * FluxVMClearForChange clears the flag and the bits; the record's emitter
 * calls FluxVMRegisterClear, which flags the record
 * FLUX_WAL_ALL_VISIBLE_CLEARED and registers the map page at
 * FLUX_VM_BLOCK_ID; and FluxVMFinishClear sets the map page's LSN and
 * unlocks it.  The caller releases the pin.  The bits of a heap page are set
 * and cleared only by the holder of its exclusive lock, so they can be read
 * without the map page's lock.  The flag alone may be set (a hint set by
 * VACUUM), and the bits alone (PD_ALL_VISIBLE is not WAL-logged, so a crash
 * or a standby can lose it), so both are looked at.
 */
static Buffer flux_vm_clear_buf = InvalidBuffer;	/* locked map page */
static bool flux_vm_cleared = false;

/* The bits of heapBlk in the map page vmpage. */
static inline uint8
flux_vm_bits(Page vmpage, BlockNumber heapBlk)
{
	uint8	   *map = (uint8 *) PageGetContents(vmpage);

	return (map[HEAPBLK_TO_MAPBYTE(heapBlk)] >>
			(HEAPBLK_TO_OFFSET(heapBlk) * 2)) & FLUX_VM_VALID_BITS;
}

/*
 * Clear the bits of heapBlk in the map page vmpage; true if any was set.
 * Also used by redo.
 */
bool
FluxVMClearMapBits(Page vmpage, BlockNumber heapBlk)
{
	uint8	   *map = (uint8 *) PageGetContents(vmpage);

	if (flux_vm_bits(vmpage, heapBlk) == 0)
		return false;
	map[HEAPBLK_TO_MAPBYTE(heapBlk)] &=
		~(FLUX_VM_VALID_BITS << (HEAPBLK_TO_OFFSET(heapBlk) * 2));
	return true;
}

/*
 * Pin the map page of the locked heap page heapBuf, or return InvalidBuffer
 * if the map has none (then no bit can be set).
 *
 * The size of the map this backend cached may be out of date: VACUUM may
 * have created or extended the map since, and set bits there, and the
 * invalidation that says so is only processed at the next lock this backend
 * takes, which a running statement may never do.  So if the cached size does
 * not cover the page, read the size again, as heap's visibilitymap_pin does
 * by extending the map.  VACUUM sets a page's bits only under the page's
 * lock, which the caller holds, so the size read now covers any bit set.
 *
 * Skip that when the map did not exist and the page is not PD_ALL_VISIBLE: a
 * map that a VACUUM created since then set PD_ALL_VISIBLE with the bits, so
 * a table that was never vacuumed costs no lookup per change.  Only replay
 * sets bits without PD_ALL_VISIBLE, and a backend that cached the size before
 * a map existed cannot have been running then: the read paths
 * (FluxVMCheck) do not cache a missing map, and a standby does not write.
 */
Buffer
FluxVMPinForChange(Relation rel, Buffer heapBuf)
{
	SMgrRelation reln = RelationGetSmgr(rel);
	BlockNumber mapBlock = HEAPBLK_TO_MAPBLOCK(BufferGetBlockNumber(heapBuf));
	BlockNumber cached = reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM];

	/* Room for the block reference at FLUX_VM_BLOCK_ID. */
	if (RelationNeedsWAL(rel))
		XLogEnsureRecordSpace(FLUX_VM_BLOCK_ID, 0);
	if (cached != InvalidBlockNumber && mapBlock >= cached &&
		(cached > 0 || PageIsAllVisible(BufferGetPage(heapBuf))))
		reln->smgr_cached_nblocks[VISIBILITYMAP_FORKNUM] = InvalidBlockNumber;
	return flux_vm_readbuf(rel, mapBlock, false);
}

/*
 * Would a change to the locked heap page heapBuf have to clear its flag or
 * bits?  For a path that cannot log the clear, to stay off such a page.
 */
bool
FluxVMNeedsClear(Relation rel, Buffer heapBuf)
{
	Buffer		vmbuf;
	bool		set;

	if (PageIsAllVisible(BufferGetPage(heapBuf)))
		return true;
	vmbuf = FluxVMPinForChange(rel, heapBuf);
	if (!BufferIsValid(vmbuf))
		return false;
	set = flux_vm_bits(BufferGetPage(vmbuf), BufferGetBlockNumber(heapBuf)) != 0;
	ReleaseBuffer(vmbuf);
	return set;
}

/*
 * In the change's critical section: clear PD_ALL_VISIBLE of heapBuf and its
 * bits in vmbuf (from FluxVMPinForChange, may be invalid).
 */
void
FluxVMClearForChange(Buffer heapBuf, Buffer vmbuf)
{
	BlockNumber heapBlk = BufferGetBlockNumber(heapBuf);
	Page		page = BufferGetPage(heapBuf);

	Assert(CritSectionCount > 0);
	Assert(!BufferIsValid(flux_vm_clear_buf));

	flux_vm_cleared = PageIsAllVisible(page);
	PageClearAllVisible(page);
	if (BufferIsValid(vmbuf) &&
		flux_vm_bits(BufferGetPage(vmbuf), heapBlk) != 0)
	{
		LockBuffer(vmbuf, BUFFER_LOCK_EXCLUSIVE);
		(void) FluxVMClearMapBits(BufferGetPage(vmbuf), heapBlk);
		MarkBufferDirty(vmbuf);
		flux_vm_clear_buf = vmbuf;
		flux_vm_cleared = true;
	}
}

/* In the emitter of the change's record, see FluxVMClearForChange. */
void
FluxVMRegisterClear(uint16 *flags)
{
	if (flux_vm_cleared)
		*flags |= FLUX_WAL_ALL_VISIBLE_CLEARED;

	/*
	 * Not REGBUF_STANDARD: the map lives between pd_lower and pd_upper, which
	 * a standard page image leaves out.
	 */
	if (BufferIsValid(flux_vm_clear_buf))
		XLogRegisterBuffer(FLUX_VM_BLOCK_ID, flux_vm_clear_buf, 0);
}

/*
 * After the change's record (lsn, or InvalidXLogRecPtr if it is not logged):
 * stamp and unlock the map page.  Does nothing if there is nothing to finish.
 */
void
FluxVMFinishClear(XLogRecPtr lsn)
{
	if (BufferIsValid(flux_vm_clear_buf))
	{
		if (lsn != InvalidXLogRecPtr)
			PageSetLSN(BufferGetPage(flux_vm_clear_buf), lsn);
		LockBuffer(flux_vm_clear_buf, BUFFER_LOCK_UNLOCK);
		flux_vm_clear_buf = InvalidBuffer;
	}
	flux_vm_cleared = false;
}

/*
 * FluxVMVacuumPage - Update VM during VACUUM
 *
 * This is called by VACUUM after processing a page to set the appropriate
 * visibility map bits based on the page's contents.
 */
void
FluxVMVacuumPage(Relation rel, Buffer buffer, bool all_visible, bool all_frozen, TransactionId conflict_xid)
{
	BlockNumber blkno = BufferGetBlockNumber(buffer);
	uint8		flags = 0;

	if (all_visible)
		flags |= FLUX_VM_ALL_VISIBLE;
	if (all_frozen)
		flags |= FLUX_VM_ALL_FROZEN;

	Assert(!all_frozen || all_visible);
	/* Repair stale bits as well as setting newly established facts. */
	FluxVMClear(rel, blkno, buffer, FLUX_VM_VALID_BITS & ~flags);
	if (flags != 0)
		FluxVMSet(rel, blkno, buffer, flags, conflict_xid);

	/*
	 * Synchronize the in-page PD_ALL_VISIBLE flag with the VM.  Use
	 * MarkBufferDirtyHint since losing this flag on crash is benign (just
	 * falls back to VM check on next scan; VACUUM will re-set it).
	 */
	if (all_visible)
	{
		if (!PageIsAllVisible(BufferGetPage(buffer)))
		{
			PageSetAllVisible(BufferGetPage(buffer));
			MarkBufferDirtyHint(buffer, true);
		}
	}
	else
	{
		if (PageIsAllVisible(BufferGetPage(buffer)))
		{
			PageClearAllVisible(BufferGetPage(buffer));
			MarkBufferDirtyHint(buffer, true);
		}
	}
}
