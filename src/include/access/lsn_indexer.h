/*-------------------------------------------------------------------------
 *
 * lsn_indexer.h
 *	  Index of pending WAL records by page, for on-demand replay after a crash
 *
 * The index maps a BufferTag to the list of LSNs of the WAL records that
 * touch that page and have not been replayed yet.  It lives in a dshash
 * table in a DSA area, so it needs no size estimate up front.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/access/lsn_indexer.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef LSN_INDEXER_H
#define LSN_INDEXER_H

#include "access/xlogreader.h"
#include "lib/dshash.h"
#include "port/atomics.h"
#include "storage/block.h"
#include "storage/buf_internals.h"
#include "utils/dsa.h"

/*
 * LSNNode - one WAL record LSN for a page, stored in DSA memory.
 * Linked via dsa_pointer instead of raw pointers.
 */
typedef struct LSNNode
{
	XLogRecPtr	lsn;
	dsa_pointer next;			/* dsa_pointer to next LSNNode, or
								 * InvalidDsaPointer */
} LSNNode;

/*
 * PageLSNEntry - dshash entry: BufferTag -> list of LSNs.
 * The key (BufferTag) must be at the start for dshash.
 */
typedef struct PageLSNEntry
{
	BufferTag	tag;
	dsa_pointer lsn_head;		/* dsa_pointer to first LSNNode */
	dsa_pointer lsn_tail;		/* dsa_pointer to last LSNNode */
	XLogRecPtr	max_lsn;
} PageLSNEntry;

/*
 * LSNIndexControl - small fixed-size struct in traditional shared memory.
 * Holds handles so any process can attach to the DSA and dshash.
 *
 * is_active is set when the startup process creates the index and cleared
 * by LSNIndexFinish(), both under LSNIndexLock.  nreplaying counts pages
 * being replayed right now, between LSNIndexBeginPageReplay() and
 * LSNIndexEndPageReplay(); it is only incremented while holding LSNIndexLock
 * in shared mode and seeing is_active set.
 */
typedef struct LSNIndexControl
{
	dsa_handle dsa_handle;
	dshash_table_handle hash_handle;
	bool		is_active;
	pg_atomic_uint32 nreplaying;
} LSNIndexControl;

/* GUC */
extern bool fast_crash_recovery;

/* Shared control structure (in traditional shmem) */
extern LSNIndexControl *LSNIndexCtl;

/*
 * While a page is being replayed on demand: inReplayPageWals is true,
 * targetTag is the page, and targetBuffer the buffer it's being replayed
 * into.  XLogReadBufferExtended() uses them to hand redo that buffer,
 * XLogReadBufferForRedoExtended() to skip the record's other pages, and
 * XLogRecordPageWithFreeSpace() to leave the free space map alone.  Replays
 * never nest, so a single set of these is enough per process.
 */
extern bool inReplayPageWals;
extern BufferTag targetTag;
extern Buffer targetBuffer;

/* Phase 1: postmaster-safe - allocates control struct in main shmem */
extern Size LSNIndexShmemSize(void);
extern void LSNIndexShmemInit(void *arg);

/*
 * Phase 2: backend-context - creates the DSA+dshash on first call,
 * attaches on subsequent calls.  Called by the startup process.
 */
extern void LSNIndexInit(void);

/* Attach to an existing index - used by backends and the worker */
extern void LSNIndexAttach(void);
extern void LSNIndexDetach(void);

/* Check if the LSN index is active */
extern bool LSNIndexIsActive(void);

/* May pre-crash WAL still be replayed onto some page?  Blocks checkpoints. */
extern bool FastRecoveryInProgress(void);

/* Add an entry during WAL scan (startup process) */
extern void LSNIndexAddEntry(XLogReaderState *state);

/* May the startup process index this record?  Evicts resident pages for it. */
extern bool LSNIndexPrepareToDefer(XLogReaderState *record);

/* Report what the scan did, once it is over */
extern void LSNIndexLogScanSummary(void);

/* Forget pages removed by a drop or truncation replayed during WAL scan */
extern void LSNIndexForgetRelation(RelFileLocator rlocator, ForkNumber forknum,
								   BlockNumber minblkno);
extern void LSNIndexForgetDatabase(Oid dbid);

/*
 * On-demand replay of one page, driven by the buffer manager while it holds
 * the page's I/O-in-progress flag (see ReadAndReplayPendingBuffer()).
 */
extern bool LSNIndexBeginPageReplay(const BufferTag *tag);
extern void LSNIndexReplayIntoBuffer(Buffer buffer);
extern void LSNIndexEndPageReplay(void);
extern void LSNIndexErrorCleanup(void);

/* Drop a page's pending records: replayed, or about to be zeroed */
extern void LSNIndexForgetPage(const BufferTag *tag);

/* Scratch buffer for a page a record touches besides the one being replayed */
extern Buffer LSNIndexScratchBuffer(RelFileLocator rlocator, ForkNumber forknum,
									BlockNumber blkno);

/* Get the dshash table pointer (for sequential scans by the worker) */
extern dshash_table *LSNIndexGetHash(void);

/* End fast recovery once every page has been replayed (worker only) */
extern void LSNIndexFinish(void);

#endif							/* LSN_INDEXER_H */
