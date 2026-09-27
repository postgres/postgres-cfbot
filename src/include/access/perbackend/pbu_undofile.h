/*-------------------------------------------------------------------------
 *
 * pbu_undofile.h
 *	  PostgreSQL undo file manager (per-backend UNDO engine).
 *
 * Manages the files that back per-backend undo logs on the filesystem and
 * exposes the smgr vtable entry points that smgr.c dispatches to for the
 * base/undo segment files.
 *
 * Vendored/adapted from EnterpriseDB zheap
 * (Amit Kapila, Dilip Kumar, Kuntal Ghosh, Mahendra Singh Thalor,
 *  Rafia Sabih, Thomas Munro et al.).
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/perbackend/pbu_undofile.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PBU_UNDOFILE_H
#define PBU_UNDOFILE_H

#include "access/perbackend/pbu_undolog.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"
#include "storage/aio_types.h"
#include "storage/smgr.h"
#include "storage/sync.h"

/* smgr vtable entry points for undo log segment files. */
extern void undofile_init(void);
extern void undofile_open(SMgrRelation reln);
extern void undofile_shutdown(void);
extern void undofile_close(SMgrRelation reln, ForkNumber forknum);
extern void undofile_create(SMgrRelation reln, ForkNumber forknum,
							bool isRedo);
extern bool undofile_exists(SMgrRelation reln, ForkNumber forknum);
extern void undofile_unlink(RelFileLocatorBackend rlocator, ForkNumber forknum,
							bool isRedo);
extern void undofile_extend(SMgrRelation reln, ForkNumber forknum,
							BlockNumber blocknum, const void *buffer,
							bool skipFsync);
extern void undofile_zeroextend(SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum, int nblocks,
								bool skipFsync);
extern bool undofile_prefetch(SMgrRelation reln, ForkNumber forknum,
							  BlockNumber blocknum, int nblocks);
extern uint32 undofile_maxcombine(SMgrRelation reln, ForkNumber forknum,
								  BlockNumber blocknum);
extern void undofile_readv(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum, void **buffers,
						   BlockNumber nblocks);
extern void undofile_startreadv(PgAioHandle *ioh,
								SMgrRelation reln, ForkNumber forknum,
								BlockNumber blocknum, void **buffers,
								BlockNumber nblocks);
extern void undofile_writev(SMgrRelation reln, ForkNumber forknum,
							BlockNumber blocknum, const void **buffers,
							BlockNumber nblocks, bool skipFsync);
extern void undofile_writeback(SMgrRelation reln, ForkNumber forknum,
							   BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber undofile_nblocks(SMgrRelation reln, ForkNumber forknum);
extern void undofile_truncate(SMgrRelation reln, ForkNumber forknum,
							  BlockNumber old_blocks, BlockNumber nblocks);
extern void undofile_immedsync(SMgrRelation reln, ForkNumber forknum);
extern void undofile_registersync(SMgrRelation reln, ForkNumber forknum);
extern int	undofile_fd(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, uint32 *off);

/* checkpointer sync handler (SYNC_HANDLER_UNDO). */
extern int	undofile_syncfiletag(const FileTag *tag, char *path);

/*
 * Forget any pending fsync request for a discarded/recycled undo segment.
 */
extern void undofile_forget_sync(UndoLogNumber logno, BlockNumber segno,
								 Oid tablespace);

#endif							/* PBU_UNDOFILE_H */
