/*-------------------------------------------------------------------------
 *
 * undo_bufmgr.h
 *	  UNDO log buffer management and file layout definitions
 *
 * UNDO-in-WAL architecture:
 *
 *   - UNDO records are embedded in the WAL stream as XLOG_UNDO_BATCH
 *     records.  There are no separate UNDO segment files.
 *   - Reads:  UndoReadBatchFromWAL() reads UNDO batches from WAL via
 *             XLogReader (for rollback chain traversal).
 *   - Sync:   WAL flush handles durability (standard XLogFlush path).
 *   - Retention: undo_discard_horizon prevents WAL recycling past
 *             oldest needed UNDO batch.
 *
 * This module retains virtual RelFileLocator mapping for:
 *   - Buffer invalidation during segment discard (InvalidateUndoBuffers)
 *   - Legacy backward compatibility
 *
 * Each undo log is mapped to a virtual relation:
 *   RelFileLocator = {
 *     spcOid    = UNDO_DEFAULT_TABLESPACE_OID (pg_default, 1663)
 *     dbOid     = UNDO_DB_OID (pseudo-database 9)
 *     relNumber = log_number (undo log number as RelFileNumber)
 *   }
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/undo_bufmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNDO_BUFMGR_H
#define UNDO_BUFMGR_H

#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/relfilelocator.h"

/*
 * Pseudo-database OID used for undo log relations in the buffer pool.
 * This matches ZHeap's UndoLogDatabaseOid convention.  This OID must not
 * collide with any real database OID; value 9 is reserved for this purpose.
 */
#define UNDO_DB_OID				9

/*
 * Default tablespace OID for undo log buffers.  This matches the
 * pg_default tablespace (OID 1663 from pg_tablespace.dat).
 * Eventually per-tablespace undo logs may be supported, but for now
 * all undo data uses the default tablespace.
 */
#define UNDO_DEFAULT_TABLESPACE_OID		1663

/*
 * Fork number used for undo log buffers in the shared buffer pool.
 *
 * Following ZHeap's convention (UndoLogForkNum = MAIN_FORKNUM), we use
 * MAIN_FORKNUM for undo log buffer operations.  Undo buffers are
 * distinguished from regular relation data by the UNDO_DB_OID in the
 * dbOid field of the BufferTag, not by a special fork number.
 *
 * Using MAIN_FORKNUM is necessary because the smgr layer sizes internal
 * arrays to MAX_FORKNUM+1 entries.  A fork number beyond that range
 * would cause out-of-bounds accesses in smgr_cached_nblocks[] and
 * similar arrays.
 */
#define UndoLogForkNum	MAIN_FORKNUM

/*
 * UNDO_FORKNUM is reserved for future use when the smgr layer is
 * extended to support undo-specific file management (Task #5).
 * It is defined in buf_internals.h as a constant but not currently
 * used in buffer operations.
 */


/* ----------------------------------------------------------------
 *		Undo log to RelFileLocator mapping
 * ----------------------------------------------------------------
 */

/*
 * UndoLogGetRelFileLocator
 *		Build a virtual RelFileLocator for an undo log number.
 *
 * This mapping allows the standard buffer manager to identify undo log
 * blocks using its existing BufferTag infrastructure.  The resulting
 * RelFileLocator does not correspond to any entry in pg_class; it is
 * purely a buffer-pool-internal identifier.
 *
 * Parameters:
 *   log_number - the undo log number (0..16M)
 *   rlocator   - output RelFileLocator to populate
 */
static inline void
UndoLogGetRelFileLocator(uint32 log_number, RelFileLocator *rlocator)
{
	rlocator->spcOid = UNDO_DEFAULT_TABLESPACE_OID;
	rlocator->dbOid = UNDO_DB_OID;
	rlocator->relNumber = (RelFileNumber) log_number;
}

/*
 * IsUndoRelFileLocator
 *		Check whether a RelFileLocator refers to an undo log.
 *
 * This is useful for code that needs to distinguish undo log locators
 * from regular relation locators (e.g., in smgr dispatch, checkpoint
 * logic, or buffer tag inspection).
 */
static inline bool
IsUndoRelFileLocator(const RelFileLocator *rlocator)
{
	return (rlocator->dbOid == UNDO_DB_OID);
}

/*
 * UNDO file layout: append-only
 *
 * UNDO log files use an append-only layout with NO PageHeaderData overhead.
 * The logical byte offset in UndoRecPtr maps directly to the physical file
 * offset.  This eliminates the overhead of page headers, pd_lower tracking,
 * LSN management, and full-page images for UNDO data.
 *
 * UNDO data is written via pwrite() and read via pread(), bypassing
 * shared_buffers entirely for the write path.  For reads, hot data is
 * served from the kernel page cache (no I/O), while cold data requires
 * sequential I/O on the pre-allocated file.
 *
 * The buffer pool integration (ReadUndoBuffer etc.) is retained only for
 * the buffer invalidation API used during segment discard.
 */

/*
 * UndoRecPtrGetFileOffset
 *		Compute the physical file offset for an undo log logical byte offset.
 *
 * With the append-only layout, the logical offset IS the file offset.
 */
#define UndoRecPtrGetFileOffset(offset) ((uint64) (offset))

/*
 * Legacy page-layout macros (retained for undo_bufmgr.c invalidation API).
 *
 * These are used only by buffer invalidation during discard, not by the
 * write/read paths.  The "block number" is conceptual, mapping the
 * contiguous byte stream to BLCKSZ-aligned regions.
 */
#define UNDO_USABLE_BYTES_PER_PAGE	BLCKSZ

#define UndoRecPtrGetBlockNum(offset) \
	((BlockNumber) ((offset) / BLCKSZ))

#define UndoRecPtrGetPageOffset(offset) \
	((uint32) ((offset) % BLCKSZ))

/*
 * UndoLogicalToFileSize
 *		Compute the physical file size needed for a given logical byte count.
 *
 * With append-only layout, physical size equals logical size (no headers).
 * We round up to BLCKSZ alignment for pre-allocation.
 */
#define UndoLogicalToFileSize(logical_size) \
	((uint64) (((logical_size) + BLCKSZ - 1) / BLCKSZ) * BLCKSZ)


/* ----------------------------------------------------------------
 *		Buffer read/release API
 * ----------------------------------------------------------------
 */

/*
 * ReadUndoBuffer
 *		Read an undo log block into the shared buffer pool.
 *
 * This is the primary entry point for reading undo data.  It translates
 * the undo log number and block number into a virtual RelFileLocator and
 * calls ReadBufferWithoutRelcache() to obtain a shared buffer.
 *
 * The returned Buffer must be released with ReleaseUndoBuffer() when the
 * caller is done.  The caller may also need to lock the buffer (via
 * LockBuffer) depending on the access pattern.
 *
 * Parameters:
 *   log_number   - undo log number
 *   block_number - block within the undo log
 *   mode         - RBM_NORMAL, RBM_ZERO_AND_LOCK, etc.
 *
 * Returns: a valid Buffer handle.
 */
extern Buffer ReadUndoBuffer(uint32 log_number, BlockNumber block_number,
							 ReadBufferMode mode);

/*
 * ReadUndoBufferExtended
 *		Like ReadUndoBuffer but with explicit strategy control.
 *
 * Allows the caller to specify a buffer access strategy (e.g., for
 * sequential undo log scans during discard or recovery).
 */
extern Buffer ReadUndoBufferExtended(uint32 log_number,
									 BlockNumber block_number,
									 ReadBufferMode mode,
									 BufferAccessStrategy strategy);

/*
 * ReleaseUndoBuffer
 *		Release a previously read undo buffer.
 *
 * This is a thin wrapper around ReleaseBuffer() for API symmetry.
 * If the buffer was locked, it must be unlocked first (or use
 * UnlockReleaseUndoBuffer).
 */
extern void ReleaseUndoBuffer(Buffer buffer);

/*
 * UnlockReleaseUndoBuffer
 *		Unlock and release an undo buffer in one call.
 */
extern void UnlockReleaseUndoBuffer(Buffer buffer);

/*
 * MarkUndoBufferDirty
 *		Mark an undo buffer as dirty.
 *
 * This is a thin wrapper around MarkBufferDirty() for API consistency.
 */
extern void MarkUndoBufferDirty(Buffer buffer);


/* ----------------------------------------------------------------
 *		Buffer tag construction (requires buf_internals.h)
 * ----------------------------------------------------------------
 */

/*
 * UndoMakeBufferTag
 *		Initialize a BufferTag for an undo log block.
 *
 * This constructs the BufferTag that the shared buffer manager will use
 * to identify this undo block in its hash table.  It uses the virtual
 * RelFileLocator mapping and UndoLogForkNum.
 *
 * Callers must include storage/buf_internals.h before this header to
 * make these declarations visible.
 */
#ifdef BUFMGR_INTERNALS_H
extern void UndoMakeBufferTag(BufferTag *tag, uint32 log_number,
							  BlockNumber block_number);

/*
 * IsUndoBufferTag
 *		Check whether a BufferTag refers to an undo log buffer.
 *
 * Undo buffers are identified by the UNDO_DB_OID in the dbOid field
 * of the buffer tag.
 */
static inline bool
IsUndoBufferTag(const BufferTag *tag)
{
	return (tag->dbOid == UNDO_DB_OID);
}
#endif							/* BUFMGR_INTERNALS_H */


/* ----------------------------------------------------------------
 *		Invalidation
 * ----------------------------------------------------------------
 */

/*
 * InvalidateUndoBuffers
 *		Drop all shared buffers for a given undo log.
 *
 * Called when an undo log is discarded to remove stale entries from
 * the shared buffer pool.  This is analogous to DropRelationBuffers()
 * for regular relations.
 */
extern void InvalidateUndoBuffers(uint32 log_number);

/*
 * InvalidateUndoBufferRange
 *		Drop shared buffers for a range of blocks in an undo log.
 *
 * Called during undo log truncation/discard to invalidate only the
 * blocks that are being reclaimed.  Blocks starting from first_block
 * onward are invalidated.
 */
extern void InvalidateUndoBufferRange(uint32 log_number,
									  BlockNumber first_block,
									  BlockNumber last_block);

#endif							/* UNDO_BUFMGR_H */
