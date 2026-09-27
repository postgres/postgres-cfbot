/*-------------------------------------------------------------------------
 *
 * fileops.c
 *	  Transactional file operations with WAL logging
 *
 * This module provides transactional filesystem operations that integrate
 * with PostgreSQL's WAL and transaction management.  It is NOT used for
 * relation files (smgr/catalog handle those); the only in-core callers are
 * dbcommands.c, tablespace.c, and copydir.c.
 *
 * Two op classes:
 *   - CONSTRUCTIVE ops (create, mkdir, chmod, chown, truncate, symlink, link,
 *     write, setxattr) run IMMEDIATELY, and each registers an abort-time undo
 *     action (a common-WAL UNDO record plus, where relevant, a pending op)
 *     that reverses it on rollback.
 *   - DESTRUCTIVE ops (rename, delete, rmdir, rmtree) are DEFERRED: a
 *     PendingFileOp is recorded now and the filesystem is touched at
 *     transaction end by FileOpsDoPendingOps().
 *
 * WAL and UNDO are written only when XLogIsNeeded() (wal_level >= replica);
 * at wal_level = minimal there is no crash or 2PC safety.
 *
 * An immediate op performs its system call FIRST, outside any critical
 * section, and WAL-logs itself only once the call has succeeded: the forward
 * WAL record and its UNDO record are inserted and flushed together in a short
 * critical section (fileops_log()).  So an ordinary filesystem error
 * (permission denied, no space, ...) is a plain ERROR, as it is for the same
 * call without FILEOPS, and a failed op leaves nothing in the WAL for redo or
 * a standby to replay.
 *
 * This is NOT atomic.  Between the system call and the WAL flush there is a
 * window in which the change is on disk with no record of it: a crash there
 * leaves the change in place, unknown to redo, to a standby and to rollback,
 * just as the bare system call would.  Nothing reverts it.  The
 * opposite order (log, then act inside a critical section) closes that window
 * only by turning every filesystem error into a PANIC.
 *
 * Reverting checks the object at the recorded path before touching it (see
 * fileops_undo.c), but a path, its type and, where recorded, its inode are
 * evidence that the object is the one this transaction made, not proof:
 * something else can have replaced it in between.
 *
 * The deferred-operations pattern works as follows:
 *   1. A destructive API call records a PendingFileOp entry in a linked list
 *   2. At commit/abort time, FileOpsDoPendingOps() executes or discards
 *      the pending operations based on transaction outcome (a commit-time
 *      destructive op emits its WAL record at execution time)
 *   3. At PREPARE, FileOpsAtPrepare() serializes the pending ops into the
 *      2PC state file so COMMIT/ROLLBACK PREPARED can perform them
 *
 * Subtransaction support:
 *   - At subtransaction commit, entries are reassigned to the parent level
 *   - At subtransaction abort, abort-time actions execute immediately
 *
 * Platform-specific handling:
 *   - O_DIRECT: Uses PG_O_DIRECT abstraction (Linux native O_DIRECT,
 *     macOS F_NOCACHE via fcntl, Windows FILE_FLAG_NO_BUFFERING)
 *   - fsync: Uses pg_fsync() which selects the appropriate mechanism
 *     (Linux fdatasync, macOS F_FULLFSYNC, Windows FlushFileBuffers,
 *     BSD fsync)
 *   - Directory sync: Uses fsync_fname()/fsync_parent_path() which
 *     handle directory fsync on Unix platforms (not needed on Windows)
 *   - Durable operations: Uses durable_rename()/durable_unlink() which
 *     ensure operations persist across crashes via proper fsync ordering
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fileops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_SYS_FCNTL_H
#include <fcntl.h>
#endif

/*
 * ENODATA is Linux-specific; FreeBSD and other BSDs don't define it.
 * When removing extended attributes, ENODATA means "attribute does not
 * exist" -- equivalent to ENOATTR on BSDs.
 */
#ifndef ENODATA
#ifdef ENOATTR
#define ENODATA ENOATTR
#else
#define ENODATA ENOENT
#endif
#endif

#include "access/fileops_xlog.h"
#include "access/rmgr.h"
#include "access/twophase_rmgr.h"
#include "access/undolog.h"
#include "access/undorecord.h"
#include "access/undormgr.h"
#include "access/xact.h"
#include "access/xactundo.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "miscadmin.h"
#include "port/pg_xattr.h"
#include "storage/fd.h"
#include "storage/fileops.h"
#include "storage/proc.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"


/* Head of the pending file operations linked list */
static PendingFileOp *pendingFileOps = NULL;
static Size inverseBytes = 0;

static void FileOpsExecOne(PendingFileOp *pending, bool isCommit,
						   XLogRecPtr *max_lsn);

/*
 * fileops_fsync_parent -- fsync the parent directory of a file path
 *
 * This ensures that directory entry changes (create, delete, rename)
 * are durable. On Windows, directory fsync is not needed because NTFS
 * journals directory entries; fsync_fname_ext() handles this by being
 * a no-op for directories on Windows.
 */
static void
fileops_fsync_parent(const char *fname, int elevel)
{
	char		parentpath[MAXPGPATH];
	char	   *sep;

	strlcpy(parentpath, fname, MAXPGPATH);

	sep = strrchr(parentpath, '/');
	if (sep != NULL)
	{
		/* Got a path component, fsync the directory portion */
		if (sep == parentpath)
			parentpath[1] = '\0';	/* root directory */
		else
			*sep = '\0';

		fsync_fname_ext(parentpath, true, true, elevel);
	}
}

/*
 * fileops_pathlen - Length of a path including its terminating NUL
 *
 * WAL and UNDO records store path lengths in a uint16 and the UNDO payload
 * buffers are sized for MAXPGPATH, so refuse a longer path rather than
 * truncate it (or overrun a buffer).  Called before the system call, so a
 * refused path changes nothing.
 */
static int
fileops_pathlen(const char *path)
{
	size_t		len = strlen(path) + 1;

	/* Reserve room before any mutation; abort itself must never exceed WAL. */
	if (inverseBytes + 4 * MAXPGPATH + 2 * FILEOPS_MAX_DATA_BYTES >
		FILEOPS_MAX_ABORT_BYTES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too much transactional file operation undo data")));

	if (len > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("file operation path length %zu exceeds maximum %d",
						len, MAXPGPATH)));
	return (int) len;
}

/*
 * fileops_log - WAL-log an immediate operation that has already succeeded
 *
 * The caller has made the filesystem change, prepared its UNDO record (if
 * any, undo_ctx != NULL) with PrepareXactUndoData(), and registered the
 * forward record's data after XLogBeginInsert().  Insert both and flush them
 * through the end of the UNDO batch, so that a crash leaves either both or
 * neither.  Only this is a critical section; see the file header comment.
 */
static void
fileops_log(uint8 info, XactUndoContext *undo_ctx)
{
	START_CRIT_SECTION();
	(void) XLogInsert(RM_FILEOPS_ID, info);
	if (undo_ctx != NULL)
	{
		InsertXactUndoData(undo_ctx);
		CleanupXactUndoInsertion(undo_ctx);
	}
	XLogFlush(XactLastRecEnd);
	END_CRIT_SECTION();
}

/*
 * AddPendingFileOp - Add a new pending file operation to the list
 *
 * All fields are deep-copied into TopMemoryContext to survive
 * until transaction end, following the PendingRelDelete pattern.
 */
static void
AddPendingFileOp(PendingFileOpType type, const char *path,
				 const char *newpath, off_t length, bool at_commit)
{
	PendingFileOp *pending;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	pending = (PendingFileOp *) palloc(sizeof(PendingFileOp));
	pending->type = type;
	pending->path = pstrdup(path);
	pending->newpath = newpath ? pstrdup(newpath) : NULL;
	pending->length = length;
	pending->data = NULL;
	pending->data_len = 0;
	pending->at_commit = at_commit;
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingFileOps;
	pendingFileOps = pending;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * AddPendingFileOpWithData - Add a pending file operation with extra data
 *
 * Like AddPendingFileOp but also stores arbitrary data (e.g., original
 * xattr value for restore on abort).
 */
static void
AddPendingFileOpWithData(PendingFileOpType type, const char *path,
						 const char *newpath, off_t length,
						 const void *data, size_t data_len,
						 bool at_commit)
{
	PendingFileOp *pending;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	pending = (PendingFileOp *) palloc(sizeof(PendingFileOp));
	pending->type = type;
	pending->path = pstrdup(path);
	pending->newpath = newpath ? pstrdup(newpath) : NULL;
	pending->length = length;
	if (data && data_len > 0)
	{
		pending->data = palloc(data_len);
		memcpy(pending->data, data, data_len);
		pending->data_len = data_len;
	}
	else
	{
		pending->data = NULL;
		pending->data_len = 0;
	}
	pending->at_commit = at_commit;
	pending->nestLevel = GetCurrentTransactionNestLevel();
	pending->next = pendingFileOps;
	pendingFileOps = pending;

	MemoryContextSwitchTo(oldcxt);
}

/* Keep the same inverse for online abort, abort redo, and crash losers. */
static void
fileops_prepare_undo(XactUndoContext *ctx, char persistence, uint8 rmid,
					uint16 info, Oid reloid, const char *payload, Size len)
{
	AddPendingFileOpWithData(PENDING_FILEOP_INVERSE, "", NULL,
							 info, payload, len, false);
	inverseBytes += len + 2 * sizeof(uint32);
	PrepareXactUndoData(ctx, persistence, rmid, info, reloid, payload, len);
}

/* Serialize in execution (newest-first) order, without alignment assumptions. */
void
FileOpsGetAbortData(char **data, uint32 *len)
{
	PendingFileOp *p;
	StringInfoData buf;
	int level = GetCurrentTransactionNestLevel();

	*data = NULL;
	*len = 0;
	if (inverseBytes == 0)
		return;
	initStringInfo(&buf);
	for (p = pendingFileOps; p; p = p->next)
	{
		uint32 info = p->length;
		uint32 size = p->data_len;

		if (p->type != PENDING_FILEOP_INVERSE || p->nestLevel < level)
			continue;
		appendBinaryStringInfo(&buf, &info, sizeof(info));
		appendBinaryStringInfo(&buf, &size, sizeof(size));
		appendBinaryStringInfo(&buf, p->data, size);
	}
	*data = buf.data;
	*len = buf.len;
}

/*
 * CreateCheckPoint waits for this flag after choosing its redo pointer.  Even
 * though inverses ride in abort WAL, a checkpoint must not omit that record
 * until the corresponding filesystem changes have been fsynced.  A database
 * directory tree can make this interval long; there is no retry loop here.
 */
void
FileOpsBeginAbort(void)
{
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;
}

void
FileOpsEndAbort(void)
{
	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;
}

/* Apply the self-contained abort payload, also used during WAL replay. */
void
FileOpsApplyAbort(const char *data, uint32 len)
{
	FileOpsResetAbortFailure();
	while (len > 0)
	{
		uint32 info;
		uint32 size;

		if (len < 2 * sizeof(uint32))
			elog(PANIC, "invalid FILEOPS abort payload");
		memcpy(&info, data, sizeof(info));
		memcpy(&size, data + sizeof(info), sizeof(size));
		data += 2 * sizeof(uint32);
		len -= 2 * sizeof(uint32);
		if (size > len)
			elog(PANIC, "invalid FILEOPS inverse length");
		FileOpsApplyInverse(info, data, size);
		data += size;
		len -= size;
	}
}

/*
 * FreePendingFileOp - Free a pending file operation entry
 */
static void
FreePendingFileOp(PendingFileOp *pending)
{
	if (pending->type == PENDING_FILEOP_INVERSE)
		inverseBytes -= pending->data_len + 2 * sizeof(uint32);
	if (pending->path)
		pfree(pending->path);
	if (pending->newpath)
		pfree(pending->newpath);
	if (pending->data)
		pfree(pending->data);
	pfree(pending);
}

/*
 * FileOpsCancelPendingDelete - Cancel a pending file deletion
 *
 * This removes matching DELETE entries from the pendingFileOps list.
 * It is called by RelationPreserveStorage() to ensure that when a
 * relation's storage is preserved (e.g., during index reuse in ALTER TABLE),
 * the corresponding FileOps DELETE entry is also cancelled, preventing
 * FileOpsDoPendingOps from deleting the file at commit time.
 */
void
FileOpsCancelPendingDelete(const char *path, bool at_commit)
{
	PendingFileOp *pending;
	PendingFileOp *prev;
	PendingFileOp *next;

	prev = NULL;
	for (pending = pendingFileOps; pending != NULL; pending = next)
	{
		next = pending->next;
		if (pending->type == PENDING_FILEOP_DELETE &&
			pending->at_commit == at_commit &&
			strcmp(pending->path, path) == 0)
		{
			/* unlink and free list entry */
			if (prev)
				prev->next = next;
			else
				pendingFileOps = next;
			FreePendingFileOp(pending);
			/* prev does not change */
		}
		else
		{
			prev = pending;
		}
	}
}

/*
 * FileOpsSync - Ensure a file's data is durably written to disk
 *
 * This is a convenience wrapper around fsync_fname() that uses the
 * platform-appropriate sync mechanism:
 *   - Linux: fdatasync() (only flushes data, not metadata unless needed)
 *   - macOS: fcntl(F_FULLFSYNC) (flushes disk write cache)
 *   - FreeBSD: fsync()
 *   - Windows: FlushFileBuffers()
 *
 * An ERROR is raised if the sync fails.
 */
/*
 * FileOpsCreate - Create a file within a transaction
 *
 * Creates the file immediately (so it can be used within the transaction)
 * and logs the creation to WAL. If register_delete is true, the file will
 * be deleted if the transaction aborts.
 *
 * Platform handling:
 *   - Linux/FreeBSD: O_DIRECT passed directly to open()
 *   - macOS: F_NOCACHE fcntl applied after open()
 *   - Windows: FILE_FLAG_NO_BUFFERING (handled by port layer)
 *
 * Returns the file descriptor; failing to create the file is an ERROR.
 */
int
FileOpsCreate(const char *path, int flags, mode_t mode, bool register_delete)
{
	int			fd;
	int			pathlen = fileops_pathlen(path);

	Assert(!IsInParallelMode());

	fd = OpenTransientFilePerm(path, flags | O_CREAT, mode);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", path)));

	if (register_delete && !XLogIsNeeded())
		AddPendingFileOp(PENDING_FILEOP_DELETE, path, NULL, 0, false);

	if (enableFsync)
	{
		if (pg_fsync(fd) != 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", path)));
		fileops_fsync_parent(path, data_sync_elevel(ERROR));
	}

	if (XLogIsNeeded())
	{
		xl_fileops_create xlrec;
		char		undo_payload[sizeof(FileopsUndoCreate) + MAXPGPATH];
		FileopsUndoCreate *hdr = (FileopsUndoCreate *) undo_payload;
		XactUndoContext undo_ctx;

		xlrec.flags = flags;
		xlrec.mode = mode;
		xlrec.register_delete = register_delete;

		hdr->path_len = (uint16) pathlen;
		memcpy(undo_payload + sizeof(FileopsUndoCreate), path, pathlen);
		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							UNDO_RMID_FILEOPS, FILEOPS_UNDO_CREATE,
							InvalidOid, undo_payload,
							sizeof(FileopsUndoCreate) + pathlen);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsCreate);
		XLogRegisterData(path, pathlen);
		fileops_log(XLOG_FILEOPS_CREATE, &undo_ctx);

		INJECTION_POINT("fileops-create-after-wal-flush", NULL);
	}

	return fd;
}

/*
 * FileOpsDelete - Schedule a file deletion within a transaction
 *
 * The file is not deleted immediately. Instead, the deletion is deferred
 * to transaction commit (if at_commit is true) or abort (if false).
 * On Unix: unlink() with parent dir fsync.
 * On Windows: pgunlink() with retry on EACCES.
 */
void
FileOpsDelete(const char *path, bool at_commit)
{
	Assert(!IsInParallelMode());

	/*
	 * no WAL is emitted at registration time.  The deferred delete is
	 * WAL-logged in FileOpsDoPendingOps() at commit-execution time, when it
	 * actually happens on the primary, so a physical standby reproduces it
	 * via redo.  Registration-time logging would be a no-op on redo (the
	 * standby never runs FileOpsDoPendingOps()) and, for at_commit=false
	 * delete-on-abort registrations, would wrongly apply an aborted
	 * transaction's effect on the standby.
	 */
	AddPendingFileOp(PENDING_FILEOP_DELETE, path, NULL, 0, at_commit);
}

/*
 * FileOpsRename - Rename a file within a transaction
 *
 * The rename is deferred to commit time. Uses durable_rename() internally
 * which handles all platform differences:
 *   - Unix: rename() with fsync of both old and new parent dirs
 *   - Windows: MoveFileEx(MOVEFILE_REPLACE_EXISTING) with retry
 */
int
FileOpsRename(const char *oldpath, const char *newpath)
{
	Assert(!IsInParallelMode());

	/*
	 * WAL is emitted at commit-execution time in FileOpsDoPendingOps(), not
	 * at registration, so the standby reproduces the rename via redo.
	 */
	AddPendingFileOp(PENDING_FILEOP_RENAME, oldpath, newpath, 0, true);

	return 0;
}

/* Capture a bounded range before changing it, including shrink tails. */
static char *
fileops_before_image(int fd, const char *path, off_t offset, Size len)
{
	char *data;

	if (len > FILEOPS_MAX_DATA_BYTES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("file operation before-image exceeds %d bytes",
						FILEOPS_MAX_DATA_BYTES)));
	data = palloc(len ? len : 1);
	if (len && pg_pread(fd, data, len, offset) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read before-image of file \"%s\": %m", path)));
	return data;
}

/*
 * FileOpsWrite - Write data to a file at a specific offset
 *
 * Immediate execution, WAL-logged for crash recovery replay.
 * Uses pwrite() on POSIX. On Windows: SetFilePointerEx + WriteFile.
 *
 * Bounded before-images restore overwritten bytes and the original length.
 *
 * Returns 0; a failed write is an ERROR.
 */
int
FileOpsWrite(const char *path, off_t offset, const void *data, uint32 len)
{
	int			fd;
	ssize_t		nbytes;
	int			pathlen = fileops_pathlen(path);
	struct stat st;
	char *before;
	uint32 before_len;
	char *payload;
	Size payload_len;
	FileopsUndoWrite *hdr;
	XactUndoContext undo_ctx;

	Assert(!IsInParallelMode());

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for writing: %m", path)));

	if (offset < 0 || len > FILEOPS_MAX_DATA_BYTES ||
		offset > PG_INT64_MAX - len)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("invalid or excessive transactional file write range")));
	if (fstat(fd, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	before_len = offset < st.st_size ? Min((off_t) len, st.st_size - offset) : 0;
	before = fileops_before_image(fd, path, offset, before_len);
	payload_len = sizeof(FileopsUndoWrite) + pathlen + before_len + len;
	payload = palloc0(payload_len);
	hdr = (FileopsUndoWrite *) payload;
	hdr->orig_length = st.st_size;
	hdr->new_length = Max(st.st_size, offset + len);
	hdr->offset = offset;
	hdr->before_len = before_len;
	hdr->after_len = len;
	hdr->path_len = pathlen;
	memcpy(payload + sizeof(*hdr), path, pathlen);
	memcpy(payload + sizeof(*hdr) + pathlen, before, before_len);
	memcpy(payload + sizeof(*hdr) + pathlen + before_len, data, len);

	nbytes = pg_pwrite(fd, data, len, offset);
	if (nbytes != (ssize_t) len)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write %u bytes to file \"%s\" at offset %lld: %m",
						len, path, (long long) offset)));
	}

	if (enableFsync && pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\" after write: %m", path)));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	if (XLogIsNeeded())
	{
		xl_fileops_write xlrec;

		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							 UNDO_RMID_FILEOPS, FILEOPS_UNDO_WRITE,
							 InvalidOid, payload, payload_len);
		xlrec.orig_length = st.st_size;
		xlrec.before_len = before_len;
		xlrec.offset = offset;
		xlrec.len = len;
		xlrec.path_len = (uint16) pathlen;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsWrite);
		XLogRegisterData(path, pathlen);
		XLogRegisterData(data, len);
		if (before_len)
			XLogRegisterData(before, before_len);
		fileops_log(XLOG_FILEOPS_WRITE, &undo_ctx);
	}

	pfree(before);
	pfree(payload);
	return 0;
}

/*
 * FileOpsTruncate - Truncate a file within a transaction
 *
 * Executed immediately and WAL-logged. Uses ftruncate() on POSIX,
 * SetEndOfFile() on Windows. File is fsynced after truncation.  The
 * original size is restored if the transaction aborts.
 */
void
FileOpsTruncate(const char *path, off_t length)
{
	int			fd;
	struct stat st;
	off_t		orig_length = 0;
	int			pathlen = fileops_pathlen(path);
	char *before;
	uint32 before_len;

	Assert(!IsInParallelMode());

	/* Capture original length for UNDO before truncation */
	if (stat(path, &st) == 0)
		orig_length = st.st_size;

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for truncation: %m", path)));

	if (length < 0)
		ereport(ERROR, (errmsg("negative file truncation length")));
	before_len = orig_length > length ?
		Min(orig_length - length, (off_t) FILEOPS_MAX_DATA_BYTES + 1) : 0;
	before = fileops_before_image(fd, path, length, before_len);

	if (ftruncate(fd, length) < 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\" to %lld bytes: %m",
						path, (long long) length)));
	}

	/*
	 * Register abort-time pending op to restore original size on rollback.
	 * This provides immediate rollback without waiting for the background
	 * UNDO worker.
	 */
	if (!XLogIsNeeded())
		AddPendingFileOp(PENDING_FILEOP_TRUNCATE, path, NULL, orig_length, false);

	if (enableFsync && pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\" after truncation: %m",
						path)));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	if (XLogIsNeeded())
	{
		xl_fileops_truncate xlrec;
		XactUndoContext undo_ctx;
		char *payload = palloc0(sizeof(FileopsUndoTruncate) + pathlen + before_len);
		FileopsUndoTruncate *hdr = (FileopsUndoTruncate *) payload;

		xlrec.before_len = before_len;
		xlrec.length = length;
		xlrec.orig_length = orig_length;

		hdr->before_len = before_len;
		hdr->orig_length = orig_length;
		hdr->new_length = length;
		hdr->path_len = (uint16) pathlen;
		memcpy(payload + sizeof(FileopsUndoTruncate), path, pathlen);
		memcpy(payload + sizeof(FileopsUndoTruncate) + pathlen, before, before_len);
		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							UNDO_RMID_FILEOPS, FILEOPS_UNDO_TRUNCATE,
							InvalidOid, payload,
							sizeof(FileopsUndoTruncate) + pathlen + before_len);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsTruncate);
		XLogRegisterData(path, pathlen);
		if (before_len)
			XLogRegisterData(before, before_len);
		fileops_log(XLOG_FILEOPS_TRUNCATE, &undo_ctx);
		pfree(payload);
	}
	pfree(before);
}

/*
 * FileOpsChmod - Change file permissions within a transaction
 *
 * Immediate execution, WAL-logged.
 * On POSIX: chmod(). On Windows: _chmod() with limited mode bits
 * (only _S_IREAD/_S_IWRITE; no group/other). Logs WARNING for
 * unsupported mode bits on Windows.
 *
 * Returns 0 on success, -1 on failure with errno left set, so the caller
 * can tell ENOENT from other errors and report it as it would a plain
 * chmod() failure (tablespace.c does).  Nothing is logged or registered
 * when it fails, because nothing changed.
 */
int
FileOpsChmod(const char *path, mode_t mode)
{
	struct stat st;
	mode_t		orig_mode;
	int			pathlen = fileops_pathlen(path);

	Assert(!IsInParallelMode());

	/* Capture original mode for UNDO before chmod */
	if (stat(path, &st) != 0)
		return -1;				/* leave errno set by stat() for the caller */
	orig_mode = st.st_mode & 07777;

	if (chmod(path, mode) < 0)
		return -1;				/* leave errno set by chmod() for the caller */

	/*
	 * Register abort-time pending op to restore original mode on rollback.
	 * Store mode in the length field (mode_t fits in off_t).
	 */
	if (!XLogIsNeeded())
		AddPendingFileOp(PENDING_FILEOP_CHMOD, path, NULL, (off_t) orig_mode, false);

	if (XLogIsNeeded())
	{
		xl_fileops_chmod xlrec;
		XactUndoContext undo_ctx;
		char		payload[sizeof(FileopsUndoChmod) + MAXPGPATH];
		FileopsUndoChmod *hdr = (FileopsUndoChmod *) payload;

		xlrec.mode = mode;
		xlrec.orig_mode = orig_mode;

		hdr->orig_mode = orig_mode;
		hdr->new_mode = mode & 07777;
		hdr->path_len = (uint16) pathlen;
		memcpy(payload + sizeof(FileopsUndoChmod), path, pathlen);
		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							UNDO_RMID_FILEOPS, FILEOPS_UNDO_CHMOD,
							InvalidOid, payload,
							sizeof(FileopsUndoChmod) + pathlen);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsChmod);
		XLogRegisterData(path, pathlen);
		fileops_log(XLOG_FILEOPS_CHMOD, &undo_ctx);

		INJECTION_POINT("fileops-chmod-after-wal-flush", NULL);
	}

	return 0;
}

/*
 * FileOpsChown - Change file ownership within a transaction
 *
 * Immediate execution, WAL-logged.
 * On POSIX: chown(). On Windows: no-op with WARNING (Windows uses
 * ACLs for ownership, not uid/gid).
 *
 * Returns 0; a failed chown() is an ERROR.
 */
int
FileOpsChown(const char *path, uid_t uid, gid_t gid)
{
	struct stat st;
	uid_t		orig_uid = (uid_t) -1;
	gid_t		orig_gid = (gid_t) -1;
	int			pathlen = fileops_pathlen(path);

	Assert(!IsInParallelMode());

	/* Capture original ownership for UNDO before chown */
	if (stat(path, &st) == 0)
	{
		orig_uid = st.st_uid;
		orig_gid = st.st_gid;
	}

#ifndef WIN32
	if (chown(path, uid, gid) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not chown file \"%s\" to %d:%d: %m",
						path, (int) uid, (int) gid)));
#else
	ereport(WARNING,
			(errmsg("chown is not supported on Windows, skipping for \"%s\"",
					path)));
	return 0;					/* nothing changed, so nothing to log */
#endif

	if (XLogIsNeeded())
	{
		xl_fileops_chown xlrec;
		XactUndoContext undo_ctx;
		char		payload[sizeof(FileopsUndoChown) + MAXPGPATH];
		FileopsUndoChown *hdr = (FileopsUndoChown *) payload;

		xlrec.uid = uid;
		xlrec.gid = gid;
		xlrec.orig_uid = orig_uid;
		xlrec.orig_gid = orig_gid;

		hdr->orig_uid = orig_uid;
		hdr->orig_gid = orig_gid;
		/* chown() leaves an id of -1 unchanged */
		hdr->new_uid = (uid == (uid_t) -1) ? orig_uid : uid;
		hdr->new_gid = (gid == (gid_t) -1) ? orig_gid : gid;
		hdr->path_len = (uint16) pathlen;
		memcpy(payload + sizeof(FileopsUndoChown), path, pathlen);
		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							UNDO_RMID_FILEOPS, FILEOPS_UNDO_CHOWN,
							InvalidOid, payload,
							sizeof(FileopsUndoChown) + pathlen);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsChown);
		XLogRegisterData(path, pathlen);
		fileops_log(XLOG_FILEOPS_CHOWN, &undo_ctx);
	}

	return 0;
}

void
FileOpsSync(const char *path)
{
	fsync_fname(path, false);
}

/*
 * fileops_mkdir - Create a directory within a transaction
 *
 * Shared by FileOpsMkdir() and FileOpsMkdirTree(); see there.
 */
static int
fileops_mkdir(const char *path, mode_t mode, uint16 flags, Oid dboid)
{
	int			pathlen = fileops_pathlen(path);

	Assert(!IsInParallelMode());

	if (MakePGDirectory(path) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", path)));

	/* Remove the directory (or tree) if the transaction aborts */
	if (!XLogIsNeeded())
		AddPendingFileOp((flags & FILEOPS_MKDIR_TREE) ?
					 PENDING_FILEOP_RMTREE : PENDING_FILEOP_RMDIR,
					 path, NULL, 0, false);

	if (enableFsync)
		fileops_fsync_parent(path, data_sync_elevel(ERROR));

	if (XLogIsNeeded())
	{
		xl_fileops_mkdir xlrec;
		XactUndoContext undo_ctx;
		char		payload[sizeof(FileopsUndoMkdir) + MAXPGPATH];
		FileopsUndoMkdir *hdr = (FileopsUndoMkdir *) payload;
		struct stat st;

		xlrec.mode = mode;
		xlrec.flags = flags;

		if (stat(path, &st) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m", path)));
		hdr->dev = st.st_dev;
		hdr->ino = st.st_ino;
		hdr->dboid = dboid;
		hdr->flags = flags;
		hdr->path_len = (uint16) pathlen;
		memcpy(payload + sizeof(FileopsUndoMkdir), path, pathlen);
		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							UNDO_RMID_FILEOPS, FILEOPS_UNDO_MKDIR,
							InvalidOid, payload,
							sizeof(FileopsUndoMkdir) + pathlen);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsMkdir);
		XLogRegisterData(path, pathlen);
		fileops_log(XLOG_FILEOPS_MKDIR, &undo_ctx);
	}

	return 0;
}

/*
 * FileOpsMkdir - Create a directory within a transaction
 *
 * Immediate execution; the directory is removed (rmdir) if the transaction
 * aborts, so it must then be empty.
 * Uses MakePGDirectory() pattern (mkdir with pg_dir_create_mode).
 * On Windows: _mkdir() (no mode parameter, permissions from parent).
 *
 * Returns 0; a failed mkdir() is an ERROR.
 */
int
FileOpsMkdir(const char *path, mode_t mode)
{
	return fileops_mkdir(path, mode, 0, InvalidOid);
}

/*
 * FileOpsMkdirTree - Create a directory the transaction will fill
 *
 * Like FileOpsMkdir(), but if the transaction aborts the whole tree is
 * removed, including files created in it by other means (smgr, copydir()).
 * For a database directory pass its OID as dboid: before the tree is removed
 * by the UNDO record, that database's shared buffers and pending fsync
 * requests are dropped, as they would be by DROP DATABASE.
 */
int
FileOpsMkdirTree(const char *path, mode_t mode, Oid dboid)
{
	return fileops_mkdir(path, mode, FILEOPS_MKDIR_TREE, dboid);
}

/*
 * FileOpsRmdir - Remove a directory within a transaction
 *
 * Deferred to commit time (like DELETE). Uses rmdir() on all platforms.
 * On Windows: _rmdir().
 */
void
FileOpsRmdir(const char *path, bool at_commit)
{
	Assert(!IsInParallelMode());

	/*
	 * WAL is emitted at commit-execution time in FileOpsDoPendingOps(), not
	 * at registration, so the standby reproduces the rmdir via redo.
	 */
	AddPendingFileOp(PENDING_FILEOP_RMDIR, path, NULL, 0, at_commit);
}

/*
 * FileOpsRmdirRecursive - Schedule recursive directory removal.
 *
 * Enumerates directory tree and schedules:
 * - FileOpsDelete() for each file (at_commit)
 * - FileOpsRmdir() for each subdirectory (at_commit, leaf-first order)
 *
 * Permissive: ENOENT is logged but not an error.
 */
void
FileOpsRmdirRecursive(const char *path, bool at_commit)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(path);
	if (dir == NULL)
	{
		if (errno == ENOENT)
		{
			elog(LOG, "FileOpsRmdirRecursive: directory \"%s\" does not exist",
				 path);
			return;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m", path)));
	}

	while ((de = ReadDirExtended(dir, path, LOG)) != NULL)
	{
		char		subpath[MAXPGPATH];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(subpath, MAXPGPATH, "%s/%s", path, de->d_name);
		if (lstat(subpath, &st) == 0)
		{
			if (S_ISDIR(st.st_mode))
				FileOpsRmdirRecursive(subpath, at_commit);
			else
				FileOpsDelete(subpath, at_commit);
		}
	}
	FreeDir(dir);
	FileOpsRmdir(path, at_commit);
}

/*
 * FileOpsSymlink - Create a symbolic link within a transaction
 *
 * Immediate execution. Registers delete-on-abort for cleanup.
 * On POSIX: symlink(). On Windows: pgsymlink() which creates NTFS
 * junction points via DeviceIoControl(). Note: junction points only
 * work for directories on Windows.
 *
 * Returns 0; a failed symlink() is an ERROR.
 */
int
FileOpsSymlink(const char *target, const char *linkpath)
{
	int			targetlen = fileops_pathlen(target);
	int			linkpathlen = fileops_pathlen(linkpath);

	Assert(!IsInParallelMode());

	if (symlink(target, linkpath) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create symbolic link \"%s\" -> \"%s\": %m",
						linkpath, target)));

	/* Register delete-on-abort to clean up the symlink on rollback */
	if (!XLogIsNeeded())
		AddPendingFileOp(PENDING_FILEOP_DELETE, linkpath, NULL, 0, false);

	if (enableFsync)
		fileops_fsync_parent(linkpath, data_sync_elevel(ERROR));

	if (XLogIsNeeded())
	{
		xl_fileops_symlink xlrec;
		XactUndoContext undo_ctx;
		char		payload[sizeof(FileopsUndoSymlink) + 2 * MAXPGPATH];
		FileopsUndoSymlink *hdr = (FileopsUndoSymlink *) payload;

		xlrec.target_len = (uint16) targetlen;

		/* the target lets the UNDO recognize the link it would remove */
		hdr->linkpath_len = (uint16) linkpathlen;
		hdr->target_len = (uint16) targetlen;
		memcpy(payload + sizeof(FileopsUndoSymlink), linkpath, linkpathlen);
		memcpy(payload + sizeof(FileopsUndoSymlink) + linkpathlen, target,
			   targetlen);
		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							UNDO_RMID_FILEOPS, FILEOPS_UNDO_SYMLINK,
							InvalidOid, payload,
							sizeof(FileopsUndoSymlink) + linkpathlen +
							targetlen);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsSymlink);
		XLogRegisterData(target, targetlen);
		XLogRegisterData(linkpath, linkpathlen);
		fileops_log(XLOG_FILEOPS_SYMLINK, &undo_ctx);
	}

	return 0;
}

/*
 * FileOpsLink - Create a hard link within a transaction
 *
 * Immediate execution. Registers delete-on-abort for cleanup.
 * On POSIX: link(). On Windows: CreateHardLinkA() (NTFS only).
 *
 * Returns 0; a failed link() is an ERROR.
 */
int
FileOpsLink(const char *oldpath, const char *newpath)
{
	int			oldpathlen = fileops_pathlen(oldpath);
	int			newpathlen = fileops_pathlen(newpath);
	struct stat st;

	Assert(!IsInParallelMode());

	/*
	 * The new link will be this inode; the UNDO removes newpath only while it
	 * still is.
	 */
	if (stat(oldpath, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", oldpath)));

	if (link(oldpath, newpath) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create hard link \"%s\" -> \"%s\": %m",
						newpath, oldpath)));

	/* Register delete-on-abort to clean up the link on rollback */
	if (!XLogIsNeeded())
		AddPendingFileOp(PENDING_FILEOP_DELETE, newpath, NULL, 0, false);

	if (enableFsync)
		fileops_fsync_parent(newpath, data_sync_elevel(ERROR));

	if (XLogIsNeeded())
	{
		xl_fileops_link xlrec;
		XactUndoContext undo_ctx;
		char		payload[sizeof(FileopsUndoLink) + MAXPGPATH];
		FileopsUndoLink *hdr = (FileopsUndoLink *) payload;

		xlrec.oldpath_len = (uint16) oldpathlen;

		hdr->dev = st.st_dev;
		hdr->ino = st.st_ino;
		hdr->newpath_len = (uint16) newpathlen;
		memcpy(payload + sizeof(FileopsUndoLink), newpath, newpathlen);
		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							UNDO_RMID_FILEOPS, FILEOPS_UNDO_LINK,
							InvalidOid, payload,
							sizeof(FileopsUndoLink) + newpathlen);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsLink);
		XLogRegisterData(oldpath, oldpathlen);
		XLogRegisterData(newpath, newpathlen);
		fileops_log(XLOG_FILEOPS_LINK, &undo_ctx);
	}

	return 0;
}

/*
 * FileOpsSetXattr - Set an extended attribute on a file
 *
 * Immediate execution, WAL-logged.
 * Uses pg_setxattr() portability layer which handles:
 *   - Linux: setxattr()
 *   - macOS: setxattr() with extra options parameter
 *   - FreeBSD: extattr_set_file()
 *   - Windows: NTFS Alternate Data Streams
 *   - Other: returns ENOTSUP
 *
 * Returns 0 on success.  Returns -1 with errno set if the filesystem does
 * not support extended attributes (ENOTSUP/EOPNOTSUPP) or refuses this one
 * (EPERM/EACCES); any other failure is an ERROR.  Nothing is logged or
 * registered when it fails, because nothing changed.
 */
int
FileOpsSetXattr(const char *path, const char *name,
				const void *value, size_t len)
{
	bool		had_value = false;
	char	   *orig_value = NULL;
	ssize_t		orig_value_len = 0;
	int			pathlen = fileops_pathlen(path);
	ssize_t		vlen;

	Assert(!IsInParallelMode());

	/*
	 * Capture existing xattr value before overwriting, for the abort-time
	 * pending op and the UNDO record.  If the attribute doesn't exist,
	 * had_value stays false.
	 */
	vlen = pg_getxattr(path, name, NULL, 0);
	if (vlen > 0)
	{
		orig_value = (char *) palloc(vlen);
		orig_value_len = pg_getxattr(path, name, orig_value, vlen);
		if (orig_value_len >= 0)
			had_value = true;
		else
		{
			pfree(orig_value);
			orig_value = NULL;
			orig_value_len = 0;
		}
	}
	else if (vlen == 0)
	{
		/* Attribute exists but has zero-length value */
		had_value = true;
	}

	if (pg_setxattr(path, name, value, len) < 0)
	{
		int			save_errno = errno;

		if (orig_value)
			pfree(orig_value);
		errno = save_errno;
		if (errno == ENOTSUP || errno == EOPNOTSUPP ||
			errno == EPERM || errno == EACCES)
			return -1;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not set extended attribute \"%s\" on \"%s\": %m",
						name, path)));
	}

	/*
	 * Register abort-time pending op to restore/remove xattr on rollback. If
	 * had_value: restore original value.  If !had_value: remove the xattr.
	 */
	if (!XLogIsNeeded() && had_value)
		AddPendingFileOpWithData(PENDING_FILEOP_SETXATTR, path, name,
								 0, orig_value, (size_t) orig_value_len, false);
	else if (!XLogIsNeeded())
		AddPendingFileOp(PENDING_FILEOP_REMOVEXATTR, path, name, 0, false);

	if (XLogIsNeeded())
	{
		xl_fileops_setxattr xlrec;
		int			namelen = (int) strlen(name) + 1;
		XactUndoContext undo_ctx;
		Size		payload_len;
		char	   *payload;
		FileopsUndoSetxattr *hdr;

		xlrec.path_len = (uint16) pathlen;
		xlrec.name_len = (uint16) namelen;
		xlrec.value_len = (uint32) len;
		xlrec.had_value = had_value;
		xlrec.orig_value_len = (uint32) (had_value ? orig_value_len : 0);

		payload_len = sizeof(FileopsUndoSetxattr) + pathlen + namelen +
			(had_value ? orig_value_len : 0) + len;
		payload = (char *) palloc(payload_len);
		hdr = (FileopsUndoSetxattr *) payload;

		hdr->path_len = (uint16) pathlen;
		hdr->name_len = (uint16) namelen;
		hdr->orig_value_len = (uint32) (had_value ? orig_value_len : 0);
		hdr->new_value_len = (uint32) len;
		hdr->had_value = had_value;

		memcpy(payload + sizeof(FileopsUndoSetxattr), path, pathlen);
		memcpy(payload + sizeof(FileopsUndoSetxattr) + pathlen, name, namelen);
		if (had_value && orig_value_len > 0)
			memcpy(payload + sizeof(FileopsUndoSetxattr) + pathlen + namelen,
				   orig_value, orig_value_len);
		if (len > 0)
			memcpy(payload + sizeof(FileopsUndoSetxattr) + pathlen + namelen +
				   hdr->orig_value_len, value, len);

		fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
							UNDO_RMID_FILEOPS, FILEOPS_UNDO_SETXATTR,
							InvalidOid, payload, payload_len);

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsSetxattr);
		XLogRegisterData(path, pathlen);
		XLogRegisterData(name, namelen);
		XLogRegisterData(value, (uint32) len);
		if (had_value && orig_value_len > 0)
			XLogRegisterData(orig_value, (uint32) orig_value_len);
		fileops_log(XLOG_FILEOPS_SETXATTR, &undo_ctx);
		pfree(payload);
	}

	if (orig_value)
		pfree(orig_value);

	return 0;
}

/*
 * FileOpsRemoveXattr - Remove an extended attribute from a file
 *
 * Immediate execution, WAL-logged.
 * Uses pg_removexattr() portability layer.  Removing an attribute that does
 * not exist (PG_ENOATTR) succeeds.
 *
 * Returns 0 on success, -1 if extended attributes are not supported on
 * this filesystem/platform (errno left as ENOTSUP/EOPNOTSUPP); any other
 * failure is an ERROR.  Nothing is logged or registered when it fails.
 */
int
FileOpsRemoveXattr(const char *path, const char *name)
{
	char	   *saved_value = NULL;
	ssize_t		saved_value_len = 0;
	int			pathlen = fileops_pathlen(path);
	ssize_t		vlen;

	Assert(!IsInParallelMode());

	/*
	 * Capture existing xattr value before removal, to restore it on rollback.
	 * A NULL saved_value means there was none.
	 */
	vlen = pg_getxattr(path, name, NULL, 0);
	if (vlen > 0)
	{
		saved_value = (char *) palloc(vlen);
		saved_value_len = pg_getxattr(path, name, saved_value, vlen);
		if (saved_value_len < 0)
		{
			pfree(saved_value);
			saved_value = NULL;
			saved_value_len = 0;
		}
	}
	else if (vlen == 0)
	{
		/* Attribute exists but has zero-length value */
		saved_value_len = 0;
		saved_value = (char *) palloc(1);	/* non-NULL marker */
	}

	if (pg_removexattr(path, name) < 0 && errno != PG_ENOATTR)
	{
		int			save_errno = errno;

		if (saved_value)
			pfree(saved_value);
		errno = save_errno;
		if (errno == ENOTSUP || errno == EOPNOTSUPP)
			return -1;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove extended attribute \"%s\" from \"%s\": %m",
						name, path)));
	}

	/*
	 * Register abort-time pending op to restore the removed xattr on
	 * rollback. Use SETXATTR type: on abort, set the xattr back to its saved
	 * value.
	 */
	if (!XLogIsNeeded() && saved_value != NULL)
		AddPendingFileOpWithData(PENDING_FILEOP_SETXATTR, path, name,
								 0, saved_value, (size_t) saved_value_len, false);

	if (XLogIsNeeded())
	{
		xl_fileops_removexattr xlrec;
		int			namelen = (int) strlen(name) + 1;
		XactUndoContext undo_ctx;
		char	   *payload = NULL;
		bool		had_value = (saved_value != NULL);

		xlrec.path_len = (uint16) pathlen;
		xlrec.name_len = (uint16) namelen;
		xlrec.had_value = had_value;
		xlrec.orig_value_len = (uint32) (had_value ? saved_value_len : 0);

		if (had_value)
		{
			Size		payload_len;
			FileopsUndoRemovexattr *hdr;

			payload_len = sizeof(FileopsUndoRemovexattr) + pathlen + namelen +
				saved_value_len;
			payload = (char *) palloc(payload_len);
			hdr = (FileopsUndoRemovexattr *) payload;

			hdr->path_len = (uint16) pathlen;
			hdr->name_len = (uint16) namelen;
			hdr->value_len = (uint32) saved_value_len;

			memcpy(payload + sizeof(FileopsUndoRemovexattr), path, pathlen);
			memcpy(payload + sizeof(FileopsUndoRemovexattr) + pathlen,
				   name, namelen);
			if (saved_value_len > 0)
				memcpy(payload + sizeof(FileopsUndoRemovexattr) + pathlen + namelen,
					   saved_value, saved_value_len);

			fileops_prepare_undo(&undo_ctx, RELPERSISTENCE_PERMANENT,
								UNDO_RMID_FILEOPS, FILEOPS_UNDO_REMOVEXATTR,
								InvalidOid, payload, payload_len);
		}

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfFileOpsRemovexattr);
		XLogRegisterData(path, pathlen);
		XLogRegisterData(name, namelen);
		if (had_value && saved_value_len > 0)
			XLogRegisterData(saved_value, (uint32) saved_value_len);
		fileops_log(XLOG_FILEOPS_REMOVEXATTR, had_value ? &undo_ctx : NULL);
		if (payload)
			pfree(payload);
	}

	if (saved_value)
		pfree(saved_value);

	return 0;
}

/*
 * FileOpsDoPendingOps - Execute pending file operations at transaction end
 *
 * At commit, operations with at_commit=true are executed.
 * At abort, operations with at_commit=false are executed.
 *
 * This is called from xact.c at transaction commit/abort, analogous
 * to smgrDoPendingDeletes().
 */
void
FileOpsDoPendingOps(bool isCommit)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingFileOp *pending;
	PendingFileOp *prev;
	PendingFileOp *next;
	PendingFileOp *exec_head = NULL;
	PendingFileOp *exec_tail = NULL;
	XLogRecPtr	max_lsn = InvalidXLogRecPtr;

	/*
	 * Pass 1: detach the entries belonging to this (or an inner) nesting
	 * level from the pending list.  Those matching the transaction outcome
	 * are queued for execution; the rest are discarded.  Outer-level entries
	 * stay on the list for a later call.  The queue is built in pending-list
	 * order, which is newest-first because AddPendingFileOp prepends.
	 */
	prev = NULL;
	for (pending = pendingFileOps; pending != NULL; pending = next)
	{
		next = pending->next;

		if (pending->nestLevel < nestLevel)
		{
			/* outer-level entries should not be processed yet */
			prev = pending;
			continue;
		}

		/* unlink from list first, so we don't retry on failure */
		if (prev)
			prev->next = next;
		else
			pendingFileOps = next;

		if (pending->at_commit == isCommit)
		{
			/*
			 * queue for execution, preserving pending-list (newest-first)
			 * order
			 */
			pending->next = NULL;
			if (exec_tail != NULL)
				exec_tail->next = pending;
			else
				exec_head = pending;
			exec_tail = pending;
		}
		else
			FreePendingFileOp(pending);
	}

	/*
	 * Commit-time ops must run in registration order (oldest-first) so that
	 * nested directory removals proceed deepest-first.  The pending list is
	 * LIFO, so the queue collected above is newest-first; reverse it for
	 * commit.  Abort-time ops must keep LIFO order, which the collected queue
	 * already preserves, so they are left as-is.
	 */
	if (isCommit)
	{
		PendingFileOp *rev = NULL;

		while (exec_head != NULL)
		{
			next = exec_head->next;
			exec_head->next = rev;
			rev = exec_head;
			exec_head = next;
		}
		exec_head = rev;
	}

	/* Commit record written, nothing executed yet (for tests) */
	if (isCommit && exec_head != NULL)
		INJECTION_POINT("fileops-commit-before-pending-ops", NULL);

	/* Pass 2: execute the queued ops in order, freeing each when done. */
	for (pending = exec_head; pending != NULL; pending = next)
	{
		next = pending->next;

		FileOpsExecOne(pending, isCommit, &max_lsn);

		FreePendingFileOp(pending);
	}

	/*
	 * Each destructive commit op above XLogFlush()es its own record BEFORE
	 * performing the filesystem mutation (WAL-before-mutation), so no primary
	 * FS change can outrun its WAL record.  This trailing flush is therefore
	 * redundant, kept only as a no-op safety backstop (max_lsn is already
	 * durable).  Only the commit path ever sets max_lsn.
	 */
	if (!XLogRecPtrIsInvalid(max_lsn))
		XLogFlush(max_lsn);
}

/*
 * FileOpsExecOne - Execute one pending file operation
 *
 * Performs a single deferred op (commit- or abort-time) exactly as it would
 * run in an ordinary transaction end.  Shared by FileOpsDoPendingOps() and by
 * the 2PC post-commit/post-abort callbacks, so an op prepared with PREPARE
 * TRANSACTION runs identically to one that never left this backend.
 *
 * *max_lsn accumulates the highest WAL LSN written by a commit-time op so the
 * caller can flush it.  Each destructive commit op also flushes its own record
 * before mutating the filesystem (WAL-before-mutation), so *max_lsn is only a
 * backstop.
 */
static void
FileOpsExecOne(PendingFileOp *pending, bool isCommit, XLogRecPtr *max_lsn)
{
	{
		switch (pending->type)
		{
			case PENDING_FILEOP_INVERSE:
				break;
			case PENDING_FILEOP_CREATE:
				/* Creates are executed immediately, nothing to do */
				break;

			case PENDING_FILEOP_RENAME:

				/*
				 * log-at-commit-execution.  Emit the WAL record now, when the
				 * rename actually happens on the primary, so a physical
				 * standby reproduces it via redo (which is now a real,
				 * idempotent handler).  WAL-before-op preserves write-ahead
				 * ordering; the commit record that follows orders this after
				 * the transaction's other WAL.  Only commit-time execution is
				 * logged: an abort never reaches a PENDING_FILEOP_RENAME
				 * (rename is always at_commit).
				 */
				if (isCommit && XLogIsNeeded())
				{
					xl_fileops_rename xlrec;
					int			oldpathlen = (int) strlen(pending->path) + 1;
					int			newpathlen = (int) strlen(pending->newpath) + 1;

					xlrec.oldpath_len = (uint16) oldpathlen;
					XLogBeginInsert();
					XLogRegisterData(&xlrec, SizeOfFileOpsRename);
					XLogRegisterData(pending->path, oldpathlen);
					XLogRegisterData(pending->newpath, newpathlen);
					*max_lsn = XLogInsert(RM_FILEOPS_ID, XLOG_FILEOPS_RENAME);

					/*
					 * WAL-before-mutation durability: flush this record
					 * BEFORE touching the filesystem.  A crash after the
					 * rename but before a batched flush would mutate the
					 * primary's FS without the record ever reaching the
					 * standby, diverging the two.
					 */
					XLogFlush(*max_lsn);
				}
				(void) durable_rename(pending->path, pending->newpath,
									  WARNING);
				break;

			case PENDING_FILEOP_DELETE:

				/*
				 * log-at-commit-execution, but ONLY for the commit path.  A
				 * delete-on-abort registration (at_commit=false) executes
				 * here during abort; that must NOT be WAL-logged, because a
				 * standby must never apply an aborted transaction's
				 * filesystem effect.
				 */
				if (isCommit && XLogIsNeeded())
				{
					xl_fileops_delete xlrec;
					int			pathlen = (int) strlen(pending->path) + 1;

					xlrec.at_commit = true;
					XLogBeginInsert();
					XLogRegisterData(&xlrec, SizeOfFileOpsDelete);
					XLogRegisterData(pending->path, pathlen);
					*max_lsn = XLogInsert(RM_FILEOPS_ID, XLOG_FILEOPS_DELETE);

					/* WAL-before-mutation durability (see RENAME above). */
					XLogFlush(*max_lsn);
				}
				if (unlink(pending->path) < 0)
				{
					if (errno != ENOENT)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not remove file \"%s\": %m",
										pending->path)));
				}
				else
				{
					if (enableFsync)
						fileops_fsync_parent(pending->path, WARNING);
				}
				break;

			case PENDING_FILEOP_RMDIR:

				/*
				 * log-at-commit-execution, commit path only (see the DELETE
				 * case above for the abort-path rationale).
				 */
				if (isCommit && XLogIsNeeded())
				{
					xl_fileops_rmdir xlrec;
					int			pathlen = (int) strlen(pending->path) + 1;

					xlrec.at_commit = true;
					XLogBeginInsert();
					XLogRegisterData(&xlrec, SizeOfFileOpsRmdir);
					XLogRegisterData(pending->path, pathlen);
					*max_lsn = XLogInsert(RM_FILEOPS_ID, XLOG_FILEOPS_RMDIR);

					/* WAL-before-mutation durability (see RENAME above). */
					XLogFlush(*max_lsn);
				}
				if (rmdir(pending->path) < 0)
				{
					if (errno != ENOENT)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not remove directory \"%s\": %m",
										pending->path)));
				}
				else
				{
					if (enableFsync)
						fileops_fsync_parent(pending->path, WARNING);
				}
				break;

			case PENDING_FILEOP_RMTREE:

				/*
				 * Abort-time removal of a FileOpsMkdirTree() directory and
				 * whatever ended up in it.  The UNDO record, applied earlier
				 * in the abort, or the caller's own error cleanup has usually
				 * removed it already.
				 */
				{
					struct stat st;

					if (lstat(pending->path, &st) != 0 || !S_ISDIR(st.st_mode))
						break;
				}
				if (!rmtree(pending->path, true))
					ereport(WARNING,
							(errmsg("could not remove directory tree \"%s\"",
									pending->path)));
				else if (enableFsync)
					fileops_fsync_parent(pending->path, WARNING);
				break;

			case PENDING_FILEOP_TRUNCATE:
				{
					/*
					 * Restore original file size on abort. The original
					 * length is stored in pending->length.
					 */
					int			trunc_fd;

					trunc_fd = OpenTransientFile(pending->path,
												 O_RDWR | PG_BINARY);
					if (trunc_fd >= 0)
					{
						if (ftruncate(trunc_fd, pending->length) < 0)
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("could not restore file \"%s\" to original size %lld: %m",
											pending->path,
											(long long) pending->length)));
						else if (enableFsync && pg_fsync(trunc_fd) != 0)
							ereport(data_sync_elevel(ERROR),
									(errcode_for_file_access(),
									 errmsg("could not fsync file \"%s\" after restoring size: %m",
											pending->path)));
						CloseTransientFile(trunc_fd);
					}
					else
					{
						if (errno != ENOENT)
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("could not open file \"%s\" to restore size: %m",
											pending->path)));
					}
				}
				break;

			case PENDING_FILEOP_CHMOD:
				{
					/*
					 * Restore original file mode on abort. The original mode
					 * is stored in pending->length (cast from mode_t).
					 */
					mode_t		restore_mode = (mode_t) pending->length;

					if (chmod(pending->path, restore_mode) < 0)
					{
						if (errno != ENOENT)
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("could not restore file \"%s\" to original mode 0%o: %m",
											pending->path,
											(unsigned int) restore_mode)));
					}
				}
				break;

			case PENDING_FILEOP_SETXATTR:
				{
					/*
					 * Restore an xattr to its original value on abort. path =
					 * file, newpath = attr name, data = orig value.
					 */
					if (pending->data && pending->data_len > 0)
					{
						if (pg_setxattr(pending->path, pending->newpath,
										pending->data, pending->data_len) < 0)
						{
							if (errno != ENOENT)
								ereport(WARNING,
										(errcode_for_file_access(),
										 errmsg("could not restore extended attribute \"%s\" on \"%s\": %m",
												pending->newpath,
												pending->path)));
						}
					}
					else
					{
						/* Zero-length original value */
						if (pg_setxattr(pending->path, pending->newpath,
										"", 0) < 0)
						{
							if (errno != ENOENT)
								ereport(WARNING,
										(errcode_for_file_access(),
										 errmsg("could not restore extended attribute \"%s\" on \"%s\": %m",
												pending->newpath,
												pending->path)));
						}
					}
				}
				break;

			case PENDING_FILEOP_REMOVEXATTR:
				{
					/*
					 * Remove an xattr on abort (attribute was newly created
					 * in the aborted transaction).
					 */
					if (pg_removexattr(pending->path, pending->newpath) < 0)
					{
						/*
						 * "Attribute already absent" is success for a
						 * remove-on-abort (idempotent, like our unlink/rmdir
						 * redo).  Use PG_ENOATTR, the portable
						 * absent-attribute errno: on Linux that is ENODATA,
						 * on macOS/BSD ENOATTR -- the raw ENODATA/ENOENT
						 * check missed the macOS case and warned spuriously
						 * (sec3 portability bug).
						 */
						if (errno != ENOENT && errno != PG_ENOATTR)
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("could not remove extended attribute \"%s\" from \"%s\": %m",
											pending->newpath,
											pending->path)));
					}
				}
				break;

			default:
				break;
		}
	}
}

/*
 * AtSubCommit_FileOps - Handle subtransaction commit
 *
 * Reassign all pending ops from the current nesting level to the parent.
 */
void
AtSubCommit_FileOps(void)
{
	int			nestLevel = GetCurrentTransactionNestLevel();
	PendingFileOp *pending;

	for (pending = pendingFileOps; pending != NULL; pending = pending->next)
	{
		if (pending->nestLevel >= nestLevel)
			pending->nestLevel = nestLevel - 1;
	}
}

/*
 * AtSubAbort_FileOps - Handle subtransaction abort
 *
 * Execute abort-time actions for the current nesting level immediately.
 */
void
AtSubAbort_FileOps(void)
{
	FileOpsDoPendingOps(false);
}

/*
 * PostPrepare_FileOps - Clean up after PREPARE TRANSACTION
 *
 * FileOpsAtPrepare() (called from EndPrepare, before the 2PC END sentinel)
 * has already serialized every pending file operation into the two-phase
 * state file, so this backend can now discard its in-memory list.  The ops
 * are performed later by the FILEOPS 2PC post-commit/post-abort callbacks
 * (fileops_twophase_postcommit/postabort), possibly from a different backend
 * after a crash, driven from the durable 2PC state.
 */
void
PostPrepare_FileOps(void)
{
	PendingFileOp *pending;
	PendingFileOp *next;

	for (pending = pendingFileOps; pending != NULL; pending = next)
	{
		next = pending->next;
		pendingFileOps = next;
		FreePendingFileOp(pending);
	}
}

/*
 * Serialized form of a PendingFileOp for the 2PC state file.
 *
 * A fixed header followed by three variable-length blobs (path, newpath,
 * data), in that order.  Self-describing so a crash-recovered COMMIT/ROLLBACK
 * PREPARED -- run by any backend -- can rebuild an equivalent PendingFileOp
 * and hand it to FileOpsExecOne().  Only the fields FileOpsExecOne() reads
 * are carried; nestLevel is irrelevant once prepared.
 */
typedef struct FileOpsTwoPhaseRecord
{
	int			type;			/* PendingFileOpType */
	bool		at_commit;		/* run at COMMIT PREPARED vs ROLLBACK PREPARED */
	off_t		length;			/* truncate length / write offset / mode */
	uint32		path_len;		/* bytes, including NUL, 0 if none */
	uint32		newpath_len;	/* bytes, including NUL, 0 if none */
	uint32		data_len;		/* bytes, 0 if none */
	/* followed by path, newpath, data */
} FileOpsTwoPhaseRecord;

/*
 * FileOpsAtPrepare - Serialize deferred commit-time file ops into 2PC state
 *
 * Called from EndPrepare() between StartPrepare() and the END sentinel.  Each
 * pending at_commit=true op (RENAME/DELETE/RMDIR) is written as one
 * TWOPHASE_RM_FILEOPS_ID record.  This is what makes those deferred
 * commit-time ops survive PREPARE for COMMIT PREPARED across a crash between
 * PREPARE and the final decision.
 *
 * The immediate-operation inverse list is serialized separately.  ROLLBACK
 * PREPARED includes it in its abort record and applies it before releasing
 * the prepared transaction's locks.
 */
void
FileOpsAtPrepare(void)
{
	PendingFileOp *pending;
	char *data;
	uint32 len;

	FileOpsGetAbortData(&data, &len);
	if (len)
		RegisterTwoPhaseRecord(TWOPHASE_RM_FILEOPS_ID,
							   FILEOPS_TWOPHASE_INVERSES, data, len);
	if (data)
		pfree(data);


	for (pending = pendingFileOps; pending != NULL; pending = pending->next)
	{
		FileOpsTwoPhaseRecord rec;
		char	   *buf;
		uint32		path_len;
		uint32		newpath_len;
		uint32		data_len;
		uint32		total;
		char	   *p;

		/* Immediate inverses were serialized as one blob above. */
		if (!pending->at_commit)
			continue;

		path_len = pending->path ? (uint32) strlen(pending->path) + 1 : 0;
		newpath_len = pending->newpath ? (uint32) strlen(pending->newpath) + 1 : 0;
		data_len = (pending->data && pending->data_len > 0) ?
			(uint32) pending->data_len : 0;

		rec.type = (int) pending->type;
		rec.at_commit = pending->at_commit;
		rec.length = pending->length;
		rec.path_len = path_len;
		rec.newpath_len = newpath_len;
		rec.data_len = data_len;

		total = sizeof(FileOpsTwoPhaseRecord) + path_len + newpath_len + data_len;
		buf = palloc(total);
		memcpy(buf, &rec, sizeof(FileOpsTwoPhaseRecord));
		p = buf + sizeof(FileOpsTwoPhaseRecord);
		if (path_len > 0)
		{
			memcpy(p, pending->path, path_len);
			p += path_len;
		}
		if (newpath_len > 0)
		{
			memcpy(p, pending->newpath, newpath_len);
			p += newpath_len;
		}
		if (data_len > 0)
			memcpy(p, pending->data, data_len);

		RegisterTwoPhaseRecord(TWOPHASE_RM_FILEOPS_ID, 0, buf, total);
		pfree(buf);
	}
}

/*
 * fileops_twophase_do - Shared body for the FILEOPS 2PC callbacks
 *
 * Rebuild a PendingFileOp from the serialized 2PC record and, if it matches
 * the transaction outcome, perform it exactly as FileOpsDoPendingOps() would.
 * A commit-time op emits its own WAL (idempotent redo on a standby); the
 * trailing flush makes any accumulated LSN durable.
 */
static void
fileops_twophase_do(bool isCommit, void *recdata, uint32 len)
{
	FileOpsTwoPhaseRecord rec;
	PendingFileOp pending;
	const char *p;
	XLogRecPtr	max_lsn = InvalidXLogRecPtr;

	if (len < sizeof(FileOpsTwoPhaseRecord))
	{
		elog(WARNING, "FILEOPS 2PC record too short (%u bytes)", len);
		return;
	}
	memcpy(&rec, recdata, sizeof(FileOpsTwoPhaseRecord));

	/* Only ops matching this outcome run; the rest are simply discarded. */
	if (rec.at_commit != isCommit)
		return;

	memset(&pending, 0, sizeof(pending));
	pending.type = (PendingFileOpType) rec.type;
	pending.at_commit = rec.at_commit;
	pending.length = rec.length;

	p = (const char *) recdata + sizeof(FileOpsTwoPhaseRecord);
	if (rec.path_len > 0)
	{
		pending.path = (char *) p;
		p += rec.path_len;
	}
	if (rec.newpath_len > 0)
	{
		pending.newpath = (char *) p;
		p += rec.newpath_len;
	}
	if (rec.data_len > 0)
	{
		pending.data = (void *) p;
		pending.data_len = rec.data_len;
	}

	FileOpsExecOne(&pending, isCommit, &max_lsn);

	if (!XLogRecPtrIsInvalid(max_lsn))
		XLogFlush(max_lsn);
}

/*
 * fileops_twophase_postcommit - COMMIT PREPARED callback (perform at_commit ops)
 */
void
fileops_twophase_postcommit(FullTransactionId fxid, uint16 info,
							void *recdata, uint32 len)
{
	if (info != FILEOPS_TWOPHASE_INVERSES)
		fileops_twophase_do(true, recdata, len);
}

/*
 * fileops_twophase_postabort - ROLLBACK PREPARED callback
 *
 * Immediate inverses are applied with the abort decision, not here.  Deferred
 * commit-time operations are discarded.
 */
void
fileops_twophase_postabort(FullTransactionId fxid, uint16 info,
						   void *recdata, uint32 len)
{
	/* Inverses were applied with the durable abort decision, before unlock. */
	if (info != FILEOPS_TWOPHASE_INVERSES)
		fileops_twophase_do(false, recdata, len);
}

/*
 * fileops_redo - WAL redo function for FILEOPS records
 *
 * Replay file operations during crash recovery or standby apply.
 * Each operation type has its own redo handler added in separate commits.
 */
void
fileops_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *data = XLogRecGetData(record);

	switch (info)
	{
		case XLOG_FILEOPS_CREATE:
			{
				xl_fileops_create *xlrec = (xl_fileops_create *) data;
				const char *path = data + SizeOfFileOpsCreate;
				int			fd;

				fd = BasicOpenFilePerm(path,
									   (xlrec->flags & ~PG_O_DIRECT) | O_CREAT,
									   xlrec->mode);
				if (fd < 0)
				{
					if (errno == ENOENT)
					{
						char		parentpath[MAXPGPATH];
						char	   *sep;

						strlcpy(parentpath, path, MAXPGPATH);
						sep = strrchr(parentpath, '/');
						if (sep != NULL)
						{
							*sep = '\0';
							if (MakePGDirectory(parentpath) < 0 &&
								errno != EEXIST)
								ereport(WARNING,
										(errcode_for_file_access(),
										 errmsg("could not create directory \"%s\" during WAL replay: %m",
												parentpath)));
						}

						fd = BasicOpenFilePerm(path,
											   (xlrec->flags & ~PG_O_DIRECT) | O_CREAT,
											   xlrec->mode);
					}

					if (fd < 0 && errno != EEXIST)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not create file \"%s\" during WAL replay: %m",
										path)));
				}

				if (fd >= 0)
				{
					if (enableFsync && pg_fsync(fd) != 0)
						ereport(data_sync_elevel(ERROR),
								(errcode_for_file_access(),
								 errmsg("could not fsync file \"%s\" during WAL replay: %m",
										path)));
					close(fd);
					if (enableFsync)
						fileops_fsync_parent(path, data_sync_elevel(ERROR));
				}
			}
			break;

		case XLOG_FILEOPS_WRITE:
			{
				xl_fileops_write *xlrec = (xl_fileops_write *) data;
				const char *path = data + SizeOfFileOpsWrite;
				const char *wdata = path + xlrec->path_len;
				int			fd;

				fd = BasicOpenFile(path, O_RDWR | PG_BINARY);
				if (fd < 0)
				{
					if (errno != ENOENT)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not open file \"%s\" for write during WAL replay: %m",
										path)));
				}
				else
				{
					if (pg_pwrite(fd, wdata, xlrec->len, xlrec->offset) !=
						(ssize_t) xlrec->len)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not write to file \"%s\" during WAL replay: %m",
										path)));
					else if (enableFsync && pg_fsync(fd) != 0)
						ereport(data_sync_elevel(ERROR),
								(errcode_for_file_access(),
								 errmsg("could not fsync file \"%s\" during WAL replay: %m",
										path)));
					close(fd);
				}
			}
			break;

		case XLOG_FILEOPS_RENAME:

			/*
			 * idempotent redo of a deferred rename executed at commit on the
			 * primary.  The record carries oldpath then newpath.  A standby
			 * (or a re-replay after a primary crash mid-commit) may find the
			 * rename already applied: source gone.  In that case treat it as
			 * success rather than failing on a missing source -- this is what
			 * makes redo converge whether the crash happened before or after
			 * the physical rename.
			 */
			{
				xl_fileops_rename *xlrec = (xl_fileops_rename *) data;
				const char *oldpath = data + SizeOfFileOpsRename;
				const char *newpath = oldpath + xlrec->oldpath_len;

				if (rename(oldpath, newpath) < 0)
				{
					/*
					 * ENOENT on the source means the rename already took
					 * effect (or the source never existed on this standby);
					 * either way the desired end state -- newpath present,
					 * oldpath absent -- is what we want, so it is not an
					 * error.
					 */
					if (errno != ENOENT)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not rename file \"%s\" to \"%s\" during WAL replay: %m",
										oldpath, newpath)));
				}
				else if (enableFsync)
				{
					fileops_fsync_parent(newpath, data_sync_elevel(ERROR));
					fileops_fsync_parent(oldpath, data_sync_elevel(ERROR));
				}
			}
			break;

		case XLOG_FILEOPS_DELETE:

			/*
			 * idempotent redo of a deferred delete executed at commit on the
			 * primary.  An already-absent file (ENOENT) means the delete
			 * already took effect -- treat as success.  Only commit-path
			 * deletes are ever logged (see FileOpsDoPendingOps), so redo of
			 * an aborted transaction's delete-on-abort can never occur here.
			 */
			{
				xl_fileops_delete *xlrec pg_attribute_unused() =
					(xl_fileops_delete *) data;
				const char *path = data + SizeOfFileOpsDelete;

				if (unlink(path) < 0)
				{
					if (errno != ENOENT)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not remove file \"%s\" during WAL replay: %m",
										path)));
				}
				else if (enableFsync)
					fileops_fsync_parent(path, data_sync_elevel(ERROR));
			}
			break;

		case XLOG_FILEOPS_SYMLINK:

			/*
			 * sec2: intentional no-op during redo.
			 *
			 * FileOpsSymlink() has exactly one caller,
			 * create_tablespace_directories() in tablespace.c, and it is used
			 * ONLY for tablespace symlinks under $PGDATA/pg_tblspc.  Standby
			 * (and primary crash-recovery) replay of those symlinks is OWNED
			 * by RM_TBLSPC: tblspc_redo() -> create_tablespace_directories()
			 * runs with InRecovery set, first removing any stale symlink
			 * (remove_tablespace_symlink) and then re-creating it with the
			 * recovery-time layout logic that honors the standby's own
			 * tablespace layout / allow_in_place_tablespaces / tablespace_map
			 * remap.
			 *
			 * Replaying the FILEOPS record here would be redundant (RM_TBLSPC
			 * already creates the link) and, worse, actively wrong on a
			 * remapped standby: it would unlink the correctly-remapped link
			 * and re-point it at the primary's LITERAL absolute target, which
			 * may not exist or may belong to a different location on the
			 * standby.  So FILEOPS defers tablespace-symlink replay to
			 * RM_TBLSPC and does nothing here.
			 *
			 * If FileOpsSymlink ever gains a non-tablespace caller, this must
			 * be revisited to replay those (and only those) symlinks; that
			 * would require distinguishing the two cases in the record.
			 */
			break;

		case XLOG_FILEOPS_LINK:
			{
				xl_fileops_link *xlrec = (xl_fileops_link *) data;
				const char *oldpath = data + SizeOfFileOpsLink;
				const char *newpath = oldpath + xlrec->oldpath_len;

				/* Remove existing link first for idempotent redo */
				unlink(newpath);
				if (link(oldpath, newpath) < 0 && errno != EEXIST)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not create hard link \"%s\" during WAL replay: %m",
									newpath)));
				else if (enableFsync)
					fileops_fsync_parent(newpath, data_sync_elevel(ERROR));
			}
			break;

		case XLOG_FILEOPS_MKDIR:
			{
				xl_fileops_mkdir *xlrec pg_attribute_unused() =
					(xl_fileops_mkdir *) data;
				const char *path = data + SizeOfFileOpsMkdir;

				if (MakePGDirectory(path) < 0 && errno != EEXIST)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not create directory \"%s\" during WAL replay: %m",
									path)));
				else if (enableFsync)
					fileops_fsync_parent(path, data_sync_elevel(ERROR));
			}
			break;

		case XLOG_FILEOPS_RMDIR:

			/*
			 * idempotent redo of a deferred rmdir executed at commit on the
			 * primary.  An already-absent directory (ENOENT) means the rmdir
			 * already took effect -- treat as success.
			 */
			{
				xl_fileops_rmdir *xlrec pg_attribute_unused() =
					(xl_fileops_rmdir *) data;
				const char *path = data + SizeOfFileOpsRmdir;

				if (rmdir(path) < 0)
				{
					if (errno != ENOENT)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not remove directory \"%s\" during WAL replay: %m",
										path)));
				}
				else if (enableFsync)
					fileops_fsync_parent(path, data_sync_elevel(ERROR));
			}
			break;

		case XLOG_FILEOPS_CHMOD:
			{
				xl_fileops_chmod *xlrec = (xl_fileops_chmod *) data;
				const char *path = data + SizeOfFileOpsChmod;

				if (chmod(path, xlrec->mode) < 0 && errno != ENOENT)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not chmod file \"%s\" during WAL replay: %m",
									path)));
			}
			break;

		case XLOG_FILEOPS_CHOWN:
			{
#ifndef WIN32
				xl_fileops_chown *xlrec = (xl_fileops_chown *) data;
				const char *path = data + SizeOfFileOpsChown;

				if (chown(path, xlrec->uid, xlrec->gid) < 0 &&
					errno != ENOENT)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not chown file \"%s\" during WAL replay: %m",
									path)));
#endif
			}
			break;

		case XLOG_FILEOPS_TRUNCATE:
			{
				xl_fileops_truncate *xlrec = (xl_fileops_truncate *) data;
				const char *path = data + SizeOfFileOpsTruncate;
				int			fd;

				XLogFlush(record->EndRecPtr);

				fd = BasicOpenFile(path, O_RDWR | PG_BINARY);
				if (fd < 0)
				{
					if (errno != ENOENT)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not open file \"%s\" for truncation during WAL replay: %m",
										path)));
				}
				else
				{
					if (ftruncate(fd, xlrec->length) < 0)
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("could not truncate file \"%s\" to %lld bytes during WAL replay: %m",
										path, (long long) xlrec->length)));
					else if (enableFsync && pg_fsync(fd) != 0)
						ereport(data_sync_elevel(ERROR),
								(errcode_for_file_access(),
								 errmsg("could not fsync file \"%s\" during WAL replay: %m",
										path)));
					close(fd);
				}
			}
			break;

		case XLOG_FILEOPS_SETXATTR:
			{
				xl_fileops_setxattr *xlrec = (xl_fileops_setxattr *) data;
				const char *path = data + SizeOfFileOpsSetxattr;
				const char *name = path + xlrec->path_len;
				const void *value = name + xlrec->name_len;

				if (pg_setxattr(path, name, value, xlrec->value_len) < 0 &&
					errno != ENOTSUP && errno != ENOENT)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not set extended attribute \"%s\" on \"%s\" during WAL replay: %m",
									name, path)));
			}
			break;

		case XLOG_FILEOPS_REMOVEXATTR:
			{
				xl_fileops_removexattr *xlrec =
					(xl_fileops_removexattr *) data;
				const char *path = data + SizeOfFileOpsRemovexattr;
				const char *name = path + xlrec->path_len;

				if (pg_removexattr(path, name) < 0 &&
					errno != ENOTSUP && errno != ENOENT &&
					errno != PG_ENOATTR)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not remove extended attribute \"%s\" from \"%s\" during WAL replay: %m",
									name, path)));
			}
			break;

		default:
			elog(PANIC, "fileops_redo: unknown op code %u", info);
			break;
	}
}
