/*-------------------------------------------------------------------------
 *
 * undo_bufmgr.c
 *	  UNDO log buffer manager integration with PostgreSQL's shared_buffers
 *
 * This module routes undo log I/O through PostgreSQL's standard
 * shared buffer pool.  The approach follows ZHeap's design where undo
 * data is "accessed through the buffer pool ... similar to regular
 * relation data" (ZHeap README, lines 30-40).
 *
 * Each undo log is mapped to a virtual RelFileLocator:
 *
 *   spcOid    = UNDO_DEFAULT_TABLESPACE_OID (pg_default, 1663)
 *   dbOid     = UNDO_DB_OID (pseudo-database 9)
 *   relNumber = undo log number
 *
 * This virtual locator is used with ReadBufferWithoutRelcache() to
 * read/write undo blocks through the shared buffer pool.  The fork
 * number MAIN_FORKNUM is used (following ZHeap's UndoLogForkNum
 * convention), and undo buffers are distinguished from regular data
 * by the UNDO_DB_OID in the BufferTag's dbOid field.
 *
 * Benefits:
 *   - Unified buffer management (no separate cache to tune)
 *   - Automatic clock-sweep eviction via shared_buffers
 *   - Built-in dirty buffer tracking and checkpoint support
 *   - WAL integration for crash safety
 *   - Standard buffer locking and pin semantics
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undo_bufmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"

#include "access/undo_bufmgr.h"


/* ----------------------------------------------------------------
 *		Buffer tag construction
 * ----------------------------------------------------------------
 */

/*
 * UndoMakeBufferTag
 *		Initialize a BufferTag for an undo log block.
 *
 * This constructs the BufferTag that the shared buffer manager uses
 * to identify this undo block in its hash table.  The tag encodes the
 * virtual RelFileLocator (mapping log_number to a pseudo-relation)
 * and UndoLogForkNum (MAIN_FORKNUM) as the fork number.
 */
void
UndoMakeBufferTag(BufferTag *tag, uint32 log_number,
				  BlockNumber block_number)
{
	RelFileLocator rlocator;

	UndoLogGetRelFileLocator(log_number, &rlocator);
	InitBufferTag(tag, &rlocator, UndoLogForkNum, block_number);
}


/* ----------------------------------------------------------------
 *		Buffer read/release API
 * ----------------------------------------------------------------
 */

/*
 * ReadUndoBuffer
 *		Read an undo log block into the shared buffer pool.
 *
 * Translates the undo log number and block number into a virtual
 * RelFileLocator and calls ReadBufferWithoutRelcache() to obtain
 * a shared buffer.
 *
 * The returned Buffer handle is pinned.  The caller must release it
 * via ReleaseUndoBuffer() (or UnlockReleaseUndoBuffer() if locked).
 *
 * For normal reads (RBM_NORMAL), the caller should lock the buffer
 * after this call:
 *
 *   buf = ReadUndoBuffer(logno, blkno, RBM_NORMAL);
 *   LockBuffer(buf, BUFFER_LOCK_SHARE);
 *   ... read data from BufferGetPage(buf) ...
 *   UnlockReleaseUndoBuffer(buf);
 *
 * For new page allocation (RBM_ZERO_AND_LOCK), the buffer is returned
 * zero-filled and exclusively locked:
 *
 *   buf = ReadUndoBuffer(logno, blkno, RBM_ZERO_AND_LOCK);
 *   ... initialize page contents ...
 *   MarkUndoBufferDirty(buf);
 *   UnlockReleaseUndoBuffer(buf);
 */
Buffer
ReadUndoBuffer(uint32 log_number, BlockNumber block_number,
			   ReadBufferMode mode)
{
	return ReadUndoBufferExtended(log_number, block_number, mode, NULL);
}

/*
 * ReadUndoBufferExtended
 *		Like ReadUndoBuffer but with explicit buffer access strategy.
 *
 * The strategy parameter can be used to control buffer pool usage when
 * performing bulk undo log operations (e.g., sequential scan during
 * discard, or recovery).  Pass NULL for the default strategy.
 *
 * Undo logs are always permanent (they must survive crashes for
 * recovery purposes), so we pass permanent=true to
 * ReadBufferWithoutRelcache().
 */
Buffer
ReadUndoBufferExtended(uint32 log_number, BlockNumber block_number,
					   ReadBufferMode mode, BufferAccessStrategy strategy)
{
	RelFileLocator rlocator;

	UndoLogGetRelFileLocator(log_number, &rlocator);

	return ReadBufferWithoutRelcache(rlocator,
									 UndoLogForkNum,
									 block_number,
									 mode,
									 strategy,
									 true); /* permanent */
}

/*
 * ReleaseUndoBuffer
 *		Release a pinned undo buffer.
 *
 * The buffer must not be locked when this is called.
 * This is a thin wrapper for API consistency; callers that hold
 * a lock should use UnlockReleaseUndoBuffer() instead.
 */
void
ReleaseUndoBuffer(Buffer buffer)
{
	ReleaseBuffer(buffer);
}

/*
 * UnlockReleaseUndoBuffer
 *		Unlock and release an undo buffer in one call.
 *
 * Convenience function that combines UnlockReleaseBuffer() semantics
 * for undo buffers.
 */
void
UnlockReleaseUndoBuffer(Buffer buffer)
{
	UnlockReleaseBuffer(buffer);
}

/*
 * MarkUndoBufferDirty
 *		Mark an undo buffer as needing write-back.
 *
 * The buffer must be exclusively locked when this is called.
 * The dirty buffer will be written back during the next checkpoint
 * or when evicted from the buffer pool.
 */
void
MarkUndoBufferDirty(Buffer buffer)
{
	MarkBufferDirty(buffer);
}


/* ----------------------------------------------------------------
 *		Buffer invalidation
 * ----------------------------------------------------------------
 */

/*
 * InvalidateUndoBuffers
 *		Drop all shared buffers belonging to a given undo log.
 *
 * This is called when an undo log is fully discarded and no longer
 * needed.  All pages for the specified undo log number are removed
 * from the shared buffer pool without being written back to disk,
 * since the underlying undo log files are being removed.
 *
 * Uses DropRelationBuffers() which is the standard public API for
 * dropping buffers belonging to a relation.  We open an SMgrRelation
 * for the virtual undo log locator and drop all buffers for the
 * UndoLogForkNum fork starting from block 0.
 *
 * The caller must ensure that no other backend is concurrently
 * accessing buffers for this undo log.
 */
void
InvalidateUndoBuffers(uint32 log_number)
{
	RelFileLocator rlocator;
	SMgrRelation srel;
	ForkNumber	forknum = UndoLogForkNum;
	BlockNumber firstDelBlock = 0;

	UndoLogGetRelFileLocator(log_number, &rlocator);
	srel = smgropen(rlocator, INVALID_PROC_NUMBER);

	DropRelationBuffers(srel, &forknum, 1, &firstDelBlock);

	smgrclose(srel);
}

/*
 * InvalidateUndoBufferRange
 *		Drop shared buffers for a range of blocks in an undo log.
 *
 * This is called during undo log truncation when only a portion of
 * the undo log is being discarded.  Blocks starting from first_block
 * onward are invalidated.
 *
 * Note: DropRelationBuffers drops all blocks >= firstDelBlock for the
 * given fork, so we pass first_block as the starting block.  The
 * last_block parameter documents the intended range boundary but the
 * buffer manager will drop any matching buffer with blockNum >=
 * first_block.
 *
 * The caller must ensure that no other backend is concurrently
 * accessing the buffers being invalidated.
 */
void
InvalidateUndoBufferRange(uint32 log_number, BlockNumber first_block,
						  BlockNumber last_block)
{
	RelFileLocator rlocator;
	SMgrRelation srel;
	ForkNumber	forknum = UndoLogForkNum;

	Assert(first_block <= last_block);

	UndoLogGetRelFileLocator(log_number, &rlocator);
	srel = smgropen(rlocator, INVALID_PROC_NUMBER);

	DropRelationBuffers(srel, &forknum, 1, &first_block);

	smgrclose(srel);
}
