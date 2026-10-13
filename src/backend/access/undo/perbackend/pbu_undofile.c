/*-------------------------------------------------------------------------
 *
 * pbu_undofile.c
 *	  management of file-backed segments for per-backend undo logs
 *
 * PostgreSQL undo file manager.  This module provides an SMGR-compatible
 * interface to the files that back per-backend undo logs on the file system,
 * so that undo log data can use the shared buffer pool.  Other aspects of
 * undo log management are provided by pbu_undolog.c, so the SMGR interfaces
 * not directly concerned with reading, writing and flushing data are
 * unimplemented.
 *
 * It is vendored/adapted from EnterpriseDB zheap's storage/smgr/undofile.c
 * and reconciled to the current smgr API:
 *   - the f_smgr vtable moved from read/write to readv/writev buffer vectors
 *     plus zeroextend/maxcombine/fd/startreadv entry points,
 *   - RelFileNode became RelFileLocator (smgr_rlocator), and
 *   - SMgrRelationData no longer carries a private_data pointer, so the
 *     recently-used segment File is cached in a small backend-local MRU
 *     table keyed by (logno, tablespace, segno) instead of on the reln.
 *
 * The undo smgr is registered as smgrsw[SMGR_UNDO] in smgr.c and selected by
 * smgropen() for rlocators whose dbOid is UndoLogDatabaseOid (the pseudo-oid
 * used for undo logs).  Its checkpoint sync path is registered as
 * SYNC_HANDLER_UNDO in sync.c (undofile_syncfiletag).
 *
 * Per-backend undo engine derives from EnterpriseDB zheap
 * (Amit Kapila, Dilip Kumar, Kuntal Ghosh, Mahendra Singh Thalor,
 *  Rafia Sabih, Thomas Munro et al.).
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/perbackend/pbu_undofile.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/perbackend/pbu_compat.h"
#include "access/perbackend/pbu_undofile.h"
#include "access/perbackend/pbu_undolog.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/aio.h"
#include "storage/aio_types.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "storage/sync.h"
#include "utils/memutils.h"

/*
 * While md.c expects random access and has a small number of huge segments,
 * undofile.c manages a potentially very large number of smaller segments and
 * has a less random access pattern.  Rather than keep a per-fork array of
 * vfds we keep the single most-recently-used segment File in a backend-local
 * cache keyed by (logno, tablespace, segno).  Undo access within a log is
 * largely sequential, so an MRU of one is sufficient here.  A workload that
 * thrashes across segments would benefit from promoting this to an LRU of N.
 */
typedef struct UndoFileMru
{
	bool		valid;
	Oid			relNode;		/* undo log number */
	Oid			spcNode;		/* tablespace */
	int			segno;
	File		file;
} UndoFileMru;

static UndoFileMru undofile_mru = {false, 0, 0, 0, 0};

static MemoryContext UndoFileCxt = NULL;

static File undofile_open_segment_file(Oid relNode, Oid spcNode, int segno,
									   bool missing_ok);
static File undofile_get_segment_file(SMgrRelation reln, int segno);

/* Populate a file tag describing an undofile.c segment file. */
static inline void
INIT_UNDOFILETAG(FileTag *tag, Oid xx_logno, Oid xx_tbspc, uint64 xx_segno)
{
	memset(tag, 0, sizeof(FileTag));
	tag->handler = SYNC_HANDLER_UNDO;
	tag->forknum = UndoLogForkNum;
	tag->rlocator.spcOid = xx_tbspc;
	tag->rlocator.dbOid = UndoLogDatabaseOid;
	tag->rlocator.relNumber = xx_logno;
	tag->segno = xx_segno;
}

void
undofile_init(void)
{
	if (UndoFileCxt == NULL)
		UndoFileCxt = AllocSetContextCreate(TopMemoryContext,
											"UndoFileSmgr",
											ALLOCSET_DEFAULT_SIZES);
}

void
undofile_open(SMgrRelation reln)
{
	/* No per-reln state to set up; segment Files are cached in undofile_mru. */
}

void
undofile_shutdown(void)
{
}

void
undofile_close(SMgrRelation reln, ForkNumber forknum)
{
}

void
undofile_create(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	/*
	 * File creation is managed by pbu_undolog.c, but xlogutils.c likes to
	 * call this just in case.  Ignore.
	 */
}

bool
undofile_exists(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * During recovery, xlogutils.c (via smgrexists) and the buffer manager
	 * probe whether an undo segment exists before reading/extending it.  A
	 * segment file is created lazily by undofile_get_segment_file() the first
	 * time a block is touched, so "exists" here means "the backing segment 0
	 * file is present, or can be created".  We report the log as existing so
	 * that redo of a registered undo buffer routes through the read path,
	 * which tolerates and (re)creates a missing segment during recovery.
	 *
	 * Outside recovery this smgr is only reached through the per-backend undo
	 * engine's own read/write paths, which never call smgrexists(); an
	 * unexpected caller there is a bug, so keep it loud.
	 */
	if (InRecovery)
		return true;

	elog(ERROR, "undofile_exists is not supported outside recovery");
	return false;				/* keep compiler quiet */
}

void
undofile_unlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	/*
	 * Undo segment files are removed by pbu_undolog.c's discard path
	 * (replayed via undolog_xlog_discard), not through the smgr unlink API.
	 * xlogutils.c never unlinks undo logs, so a call here is unexpected.
	 */
	elog(ERROR, "undofile_unlink is not supported");
}

void
undofile_extend(SMgrRelation reln, ForkNumber forknum,
				BlockNumber blocknum, const void *buffer,
				bool skipFsync)
{
	/*
	 * Undo segments are pre-allocated full-size by pbu_undolog.c, so the
	 * normal engine never extends via the smgr.  During recovery, however,
	 * ExtendBufferedRelTo() (reached from XLogReadBufferExtended when a
	 * registered undo page lies beyond the current segment) may extend one
	 * block at a time.  Since undofile_nblocks() reports the log as maximal,
	 * the buffer-manager "page exists" test is always true and this path is
	 * not normally reached; treat a single-block extend during recovery as an
	 * initialized write of that block so it is idempotent with the read path.
	 */
	if (InRecovery)
	{
		undofile_writev(reln, forknum, blocknum, &buffer, 1, skipFsync);
		return;
	}

	elog(ERROR, "undofile_extend is not supported outside recovery");
}

void
undofile_zeroextend(SMgrRelation reln, ForkNumber forknum,
					BlockNumber blocknum, int nblocks, bool skipFsync)
{
	/*
	 * See undofile_extend: recovery may zero-extend a registered undo page
	 * that is past the (pre-allocated) segment boundary.  Write zeroed blocks
	 * so the page is initialized, matching md.c's zeroextend semantics.  The
	 * segment file itself is (re)created on demand by
	 * undofile_get_segment_file() during recovery.
	 */
	if (InRecovery)
	{
		char	   *zerobuf = palloc0(BLCKSZ);
		const void *buffers[1];
		int			i;

		buffers[0] = zerobuf;
		for (i = 0; i < nblocks; i++)
			undofile_writev(reln, forknum, blocknum + i, buffers, 1, skipFsync);
		pfree(zerobuf);
		return;
	}

	elog(ERROR, "undofile_zeroextend is not supported outside recovery");
}

bool
undofile_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				  int nblocks)
{
#ifdef USE_PREFETCH
	Assert(forknum == MAIN_FORKNUM);

	while (nblocks > 0)
	{
		File		file;
		off_t		seekpos;
		int			nthis;

		file = undofile_get_segment_file(reln, blocknum / UNDOSEG_SIZE);
		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) UNDOSEG_SIZE));
		Assert(seekpos < (off_t) BLCKSZ * UNDOSEG_SIZE);

		nthis = Min(nblocks, UNDOSEG_SIZE - (blocknum % UNDOSEG_SIZE));

		(void) FilePrefetch(file, seekpos, (off_t) BLCKSZ * nthis,
							WAIT_EVENT_DATA_FILE_PREFETCH);

		blocknum += nthis;
		nblocks -= nthis;
	}
#endif							/* USE_PREFETCH */
	return true;
}

/*
 * Undo I/O never combines across segments (a request is always a single
 * block, and segments are separate files), so report the maximum combine as
 * the remaining blocks in the current segment.
 */
uint32
undofile_maxcombine(SMgrRelation reln, ForkNumber forknum,
					BlockNumber blocknum)
{
	BlockNumber segoff = blocknum % ((BlockNumber) UNDOSEG_SIZE);

	return UNDOSEG_SIZE - segoff;
}

void
undofile_readv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   void **buffers, BlockNumber nblocks)
{
	Assert(forknum == MAIN_FORKNUM);

	while (nblocks > 0)
	{
		File		file;
		off_t		seekpos;
		struct iovec iov;
		ssize_t		nbytes;

		file = undofile_get_segment_file(reln, blocknum / UNDOSEG_SIZE);
		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) UNDOSEG_SIZE));
		Assert(seekpos < (off_t) BLCKSZ * UNDOSEG_SIZE);

		iov.iov_base = buffers[0];
		iov.iov_len = BLCKSZ;

		nbytes = FileReadV(file, &iov, 1, seekpos, WAIT_EVENT_DATA_FILE_READ);
		if (nbytes != BLCKSZ)
		{
			if (nbytes < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read block %u in file \"%s\": %m",
								blocknum, FilePathName(file))));
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read block %u in file \"%s\": read only %zd of %d bytes",
							blocknum, FilePathName(file),
							nbytes, BLCKSZ)));
		}

		blocknum++;
		buffers++;
		nblocks--;
	}
}

void
undofile_startreadv(PgAioHandle *ioh,
					SMgrRelation reln, ForkNumber forknum,
					BlockNumber blocknum,
					void **buffers, BlockNumber nblocks)
{
	/*
	 * Asynchronous read of undo pages.  Reached during recovery when the
	 * buffer manager prefetches/reads registered undo buffers via the AIO
	 * path (e.g. XLogReadBufferExtended -> ReadBufferWithoutRelcache under an
	 * AIO io_method).  Mirrors mdstartreadv(): a single undo segment file,
	 * one FileStartReadV, reusing the generic md readv completion callback
	 * (it only interprets the byte count into blocks and does not touch
	 * md-private state, so it is correct for undo too).
	 */
	File		file;
	off_t		seekpos;
	struct iovec *iov;
	int			iovcnt;
	int			i;
	int			ret;
	BlockNumber nblocks_this_segment;

	Assert(forknum == MAIN_FORKNUM);

	nblocks_this_segment = Min(nblocks,
							   UNDOSEG_SIZE - (blocknum % UNDOSEG_SIZE));
	if (nblocks_this_segment != nblocks)
		elog(ERROR, "undo read crossing segment boundary");

	file = undofile_get_segment_file(reln, blocknum / UNDOSEG_SIZE);
	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) UNDOSEG_SIZE));
	Assert(seekpos < (off_t) BLCKSZ * UNDOSEG_SIZE);

	iovcnt = pgaio_io_get_iovec(ioh, &iov);
	Assert(nblocks <= iovcnt);

	for (i = 0; i < nblocks_this_segment; i++)
	{
		iov[i].iov_base = buffers[i];
		iov[i].iov_len = BLCKSZ;
	}
	iovcnt = nblocks_this_segment;

	pgaio_io_set_flag(ioh, PGAIO_HF_BUFFERED);

	pgaio_io_set_target_smgr(ioh, reln, forknum, blocknum, nblocks, false);
	pgaio_io_register_callbacks(ioh, PGAIO_HCB_MD_READV, 0);

	ret = FileStartReadV(ioh, file, iovcnt, seekpos, WAIT_EVENT_DATA_FILE_READ);
	if (ret != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not start reading blocks %u..%u in file \"%s\": %m",
						blocknum, blocknum + nblocks_this_segment - 1,
						FilePathName(file))));
}

void
undofile_writev(SMgrRelation reln, ForkNumber forknum,
				BlockNumber blocknum, const void **buffers,
				BlockNumber nblocks, bool skipFsync)
{
	Assert(forknum == MAIN_FORKNUM);

	while (nblocks > 0)
	{
		File		file;
		off_t		seekpos;
		struct iovec iov;
		ssize_t		nbytes;

		file = undofile_get_segment_file(reln, blocknum / UNDOSEG_SIZE);
		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) UNDOSEG_SIZE));
		Assert(seekpos < (off_t) BLCKSZ * UNDOSEG_SIZE);

		iov.iov_base = unconstify(void *, buffers[0]);
		iov.iov_len = BLCKSZ;

		nbytes = FileWriteV(file, &iov, 1, seekpos, WAIT_EVENT_DATA_FILE_WRITE);
		if (nbytes != BLCKSZ)
		{
			if (nbytes < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write block %u in file \"%s\": %m",
								blocknum, FilePathName(file))));

			/*
			 * short write: unexpected, because this should be overwriting an
			 * entirely pre-allocated segment file
			 */
			ereport(ERROR,
					(errcode(ERRCODE_DISK_FULL),
					 errmsg("could not write block %u in file \"%s\": wrote only %zd of %d bytes",
							blocknum, FilePathName(file),
							nbytes, BLCKSZ)));
		}

		/* Tell checkpointer this file is dirty. */
		if (!skipFsync && !SmgrIsTemp(reln))
		{
			FileTag		tag;

			INIT_UNDOFILETAG(&tag,
							 reln->smgr_rlocator.locator.relNumber,
							 reln->smgr_rlocator.locator.spcOid,
							 blocknum / UNDOSEG_SIZE);

			if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false /* retryOnError */ ))
			{
				if (FileSync(file, WAIT_EVENT_DATA_FILE_SYNC) < 0)
					ereport(data_sync_elevel(ERROR),
							(errcode_for_file_access(),
							 errmsg("could not fsync file \"%s\": %m",
									FilePathName(file))));
			}
		}

		blocknum++;
		buffers++;
		nblocks--;
	}
}

void
undofile_writeback(SMgrRelation reln, ForkNumber forknum,
				   BlockNumber blocknum, BlockNumber nblocks)
{
	while (nblocks > 0)
	{
		File		file;
		int			nflush;

		file = undofile_get_segment_file(reln, blocknum / UNDOSEG_SIZE);

		/* compute number of desired writes within the current segment */
		nflush = Min(nblocks,
					 1 + UNDOSEG_SIZE - (blocknum % UNDOSEG_SIZE));

		FileWriteback(file,
					  (off_t) (blocknum % UNDOSEG_SIZE) * BLCKSZ,
					  (off_t) nflush * BLCKSZ, WAIT_EVENT_DATA_FILE_FLUSH);

		nblocks -= nflush;
		blocknum += nflush;
	}
}

BlockNumber
undofile_nblocks(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * xlogutils.c likes to call this to decide whether to read or extend; for
	 * now we lie and say the relation is as big as possible.
	 */
	return UndoLogMaxSize / BLCKSZ;
}

void
undofile_truncate(SMgrRelation reln, ForkNumber forknum,
				  BlockNumber old_blocks, BlockNumber nblocks)
{
	elog(ERROR, "undofile_truncate is not supported");
}

void
undofile_immedsync(SMgrRelation reln, ForkNumber forknum)
{
	elog(ERROR, "undofile_immedsync is not supported");
}

void
undofile_registersync(SMgrRelation reln, ForkNumber forknum)
{
	elog(ERROR, "undofile_registersync is not supported");
}

int
undofile_fd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			uint32 *off)
{
	File		file;

	file = undofile_get_segment_file(reln, blocknum / UNDOSEG_SIZE);
	*off = (uint32) ((off_t) BLCKSZ * (blocknum % ((BlockNumber) UNDOSEG_SIZE)));

	return FileGetRawDesc(file);
}

static File
undofile_open_segment_file(Oid relNode, Oid spcNode, int segno,
						   bool missing_ok)
{
	File		file;
	char		path[MAXPGPATH];

	UndoLogSegmentPath(relNode, segno, spcNode, path);
	file = PathNameOpenFile(path, O_RDWR | PG_BINARY);

	if (file <= 0 && (!missing_ok || errno != ENOENT))
		elog(ERROR, "cannot open undo segment file \"%s\": %m", path);

	return file;
}

/*
 * Get a File for a particular segment of a SMgrRelation representing an undo
 * log.  The result is cached in a backend-local MRU of one.
 */
static File
undofile_get_segment_file(SMgrRelation reln, int segno)
{
	Oid			relNode = reln->smgr_rlocator.locator.relNumber;
	Oid			spcNode = reln->smgr_rlocator.locator.spcOid;

	/* If we have the right file open already, reuse it. */
	if (undofile_mru.valid &&
		undofile_mru.file > 0 &&
		undofile_mru.relNode == relNode &&
		undofile_mru.spcNode == spcNode &&
		undofile_mru.segno == segno)
		return undofile_mru.file;

	/* Otherwise close whatever we had open. */
	if (undofile_mru.valid && undofile_mru.file > 0)
		FileClose(undofile_mru.file);
	undofile_mru.valid = false;
	undofile_mru.file = 0;

	undofile_mru.file =
		undofile_open_segment_file(relNode, spcNode, segno, InRecovery);
	if (InRecovery && undofile_mru.file <= 0)
	{
		/*
		 * If in recovery, we may be trying to access a file that will later
		 * be unlinked.  Tolerate missing files, creating a new zero-filled
		 * file as required.
		 */
		UndoLogNewSegment(relNode, spcNode, segno);
		undofile_mru.file =
			undofile_open_segment_file(relNode, spcNode, segno, false);
		Assert(undofile_mru.file > 0);
	}

	undofile_mru.relNode = relNode;
	undofile_mru.spcNode = spcNode;
	undofile_mru.segno = segno;
	undofile_mru.valid = true;

	return undofile_mru.file;
}

int
undofile_syncfiletag(const FileTag *tag, char *path)
{
	SMgrRelation reln = smgropen(tag->rlocator, INVALID_PROC_NUMBER);
	File		file;

	UndoLogSegmentPath(tag->rlocator.relNumber, tag->segno,
					   tag->rlocator.spcOid, path);

	file = undofile_get_segment_file(reln, tag->segno);
	if (file <= 0)
	{
		/* errno set by undofile_get_segment_file() */
		return -1;
	}

	return FileSync(file, WAIT_EVENT_DATA_FILE_SYNC);
}

void
undofile_forget_sync(UndoLogNumber logno, BlockNumber segno, Oid tablespace)
{
	FileTag		tag;

	INIT_UNDOFILETAG(&tag, logno, tablespace, segno);

	(void) RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST, true /* retryOnError */ );
}
