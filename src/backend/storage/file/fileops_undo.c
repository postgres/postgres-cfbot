/*-------------------------------------------------------------------------
 *
 * fileops_undo.c
 *	  FILEOPS UNDO resource manager
 *
 * This module implements the UNDO apply callbacks for transactional file
 * operations.  The inverse payloads cover structural changes and bounded
 * before-images for overwritten or truncated file contents.
 *
 * On transaction abort, each FILEOPS UNDO record reverses the structural
 * change made by the corresponding FileOps* function, if the object still
 * shows that change (see fileops_undo_apply):
 *   - CREATE:     unlink the created file
 *   - WRITE:      restore overwritten bytes and original length
 *   - TRUNCATE:   restore removed bytes and original length
 *   - CHMOD:      chmod to original mode
 *   - CHOWN:      chown to original uid/gid
 *   - MKDIR:      rmdir the created directory, or remove its whole tree
 *   - SYMLINK:    unlink the symlink
 *   - LINK:       unlink the hard link
 *   - RENAME:     rename the new name back to the old one
 *   - SETXATTR:   restore/remove the original xattr
 *   - REMOVEXATTR: restore the original xattr value
 *
 * DELETE and RMDIR are handled by PendingFileOps (deferred execution) and do
 * not need UNDO records since they execute only at commit time.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fileops_undo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/undormgr.h"
#include "access/xact.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "port/pg_xattr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/fileops.h"
#include "storage/md.h"
#include "utils/hsearch.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"

/*
 * UNDO subtype constants and payload structures are defined in fileops.h,
 * shared between this file (apply side) and fileops.c (insert side).
 */

static bool applyingAbort = false;
bool		FileOpsSkipUndo = false;

/* Forward declarations */
static void fileops_note_inverse(uint16 info, const char *data, uint32 len,
								 int elevel);
static UndoApplyResult fileops_undo_apply(uint8 rmid, uint16 info,
										  TransactionId xid, Oid reloid,
										  const char *payload, Size payload_len,
										  UndoRecPtr urec_ptr);
static void fileops_undo_desc(StringInfo buf, uint8 rmid, uint16 info,
							  const char *payload, Size payload_len);

/*
 * Report an xattr restore that failed, and return UNDO_APPLY_ERROR so the
 * caller hands the chain to the logical revert worker.  Like the other
 * subtypes, a file that no longer exists is not a failure.
 */
static UndoApplyResult
fileops_undo_xattr_failed(const char *op, const char *path, const char *name)
{
	ereport(WARNING,
			(errcode_for_file_access(),
			 errmsg("FILEOPS UNDO %s: could not restore attribute \"%s\" of \"%s\": %m",
					op, name, path)));
	return UNDO_APPLY_ERROR;
}

/* The FILEOPS UNDO RM registration entry */
static const UndoRmgrData fileops_undo_rmgr = {
	.rm_name = "fileops",
	.rm_undo = fileops_undo_apply,
	.rm_desc = fileops_undo_desc,
};

/*
 * FileopsUndoRmgrInit - Register the FILEOPS UNDO resource manager
 */
void
FileopsUndoRmgrInit(void)
{
	RegisterUndoRmgr(UNDO_RMID_FILEOPS, &fileops_undo_rmgr);

	/*
	 * Wire FILEOPS's pending-structural-file-operation callbacks into the
	 * AM-neutral hooks in xact.c (see access/xact.h for why xact.c cannot
	 * call these by name directly).
	 */
	PendingPhysOpsDo_hook = FileOpsDoPendingOps;
	PendingPhysOpsPostPrepare_hook = PostPrepare_FileOps;
	PendingPhysOpsAtSubCommit_hook = AtSubCommit_FileOps;
	PendingPhysOpsAtSubAbort_hook = AtSubAbort_FileOps;
}

/*
 * fileops_undo_lstat - lstat() for an UNDO action
 *
 * Returns 1 if path exists, 0 if it does not (nothing left to undo), and -1
 * after a WARNING for any other failure.
 */
static int
fileops_undo_lstat(const char *op, const char *path, struct stat *st)
{
	if (lstat(path, st) == 0)
		return 1;
	if (errno == ENOENT)
		return 0;
	ereport(WARNING,
			(errcode_for_file_access(),
			 errmsg("FILEOPS UNDO %s: could not stat \"%s\": %m", op, path)));
	return -1;
}

/*
 * fileops_undo_superseded - Report an object that no longer shows the effect
 * being undone: it was already reverted by an earlier walk of this chain, or
 * changed since.  Either way it is not ours to touch.
 */
static UndoApplyResult
fileops_undo_superseded(const char *op, const char *path)
{
	ereport(DEBUG2,
			(errmsg("FILEOPS UNDO %s: \"%s\" was changed since, leaving it",
					op, path)));
	return UNDO_APPLY_SUCCESS;
}

/*
 * fileops_xattr_is - Does attribute name of path hold exactly value?
 *
 * Returns 1 if so, 0 if not (or the attribute is absent), and -1 with errno
 * set if it cannot be read.
 */
static int
fileops_xattr_is(const char *path, const char *name, const char *value,
				 uint32 len)
{
	ssize_t		cur_len;
	char	   *cur;
	int			result;

	cur_len = pg_getxattr(path, name, NULL, 0);
	if (cur_len < 0)
		return (errno == PG_ENOATTR) ? 0 : -1;
	if ((size_t) cur_len != len)
		return 0;
	if (len == 0)
		return 1;

	cur = palloc(len);
	cur_len = pg_getxattr(path, name, cur, len);
	if (cur_len < 0)
		result = (errno == PG_ENOATTR || errno == ERANGE) ? 0 : -1;
	else
		result = ((size_t) cur_len == len && memcmp(cur, value, len) == 0);
	pfree(cur);
	return result;
}

/*
 * fileops_undo_apply - Apply a single FILEOPS UNDO record
 *
 * Reverses the structural filesystem operation, but only if the object still
 * shows its effect, as recorded in the payload.  The same chain can be walked
 * again later (see storage/fileops.h), when the object may already have been
 * reverted, or changed by someone else; then the record is a no-op.  A CREATE
 * or MKDIR cannot tell its own file or directory from one created again at
 * the same path, since the inode is not known when the UNDO is written, and
 * a REMOVEXATTR cannot tell an attribute it removed from one removed again;
 * nor can a RENAME tell the object it renamed from one put at the new name
 * since.
 */
static UndoApplyResult
fileops_undo_apply(uint8 rmid, uint16 info, TransactionId xid, Oid reloid,
				   const char *payload, Size payload_len, UndoRecPtr urec_ptr)
{
	UndoApplyResult result = UNDO_APPLY_SUCCESS;
	struct stat st;

	Assert(rmid == UNDO_RMID_FILEOPS);

	/* Abort WAL owns completed aborts; generic UNDO is for crash losers. */
	if (!applyingAbort && FileOpsSkipUndo)
		return UNDO_APPLY_SUCCESS;

	INJECTION_POINT("fileops-undo-apply-begin", NULL);

	switch (info)
	{
		case FILEOPS_UNDO_CREATE:
			{
				FileopsUndoCreate hdr;
				const char *path;

				if (payload_len < sizeof(FileopsUndoCreate))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoCreate));
				path = payload + sizeof(FileopsUndoCreate);

				switch (fileops_undo_lstat("CREATE", path, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						if (!S_ISREG(st.st_mode))
							result = fileops_undo_superseded("CREATE", path);
						else if (unlink(path) < 0 && errno != ENOENT)
						{
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("FILEOPS UNDO CREATE: could not unlink \"%s\": %m",
											path)));
							result = UNDO_APPLY_ERROR;
						}
						break;
				}
			}
			break;

		case FILEOPS_UNDO_WRITE:
			{
				FileopsUndoWrite hdr;
				const char *path;
				const char *before;
				const char *after;
				char	   *current;
				int			fd;
				int			rc;

				if (payload_len < sizeof(hdr))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(hdr));
				if (payload_len != sizeof(hdr) + hdr.path_len +
					hdr.before_len + hdr.after_len)
					return UNDO_APPLY_ERROR;
				path = payload + sizeof(hdr);
				before = path + hdr.path_len;
				after = before + hdr.before_len;

				/*
				 * 0 (gone) is nothing to undo, but -1 (a stat error) must fail
				 * like every other subtype so a crash loser's overwritten bytes
				 * are retried by the logical revert worker, not silently
				 * dropped.
				 */
				rc = fileops_undo_lstat("WRITE", path, &st);
#ifdef USE_INJECTION_POINTS
				/* Reach the stat-error (-1) arm; see 020_fileops_write_stat_error. */
				if (rc == 1 &&
					IS_INJECTION_POINT_ATTACHED("fileops-undo-write-lstat-error"))
					rc = -1;
#endif
				if (rc == -1)
					result = UNDO_APPLY_ERROR;
				if (rc != 1)
					break;
				if (!S_ISREG(st.st_mode) || st.st_size != hdr.new_length)
					break;
				fd = BasicOpenFile(path, O_RDWR | PG_BINARY);
				if (fd < 0)
				{
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO WRITE: could not open \"%s\": %m", path)));
					return UNDO_APPLY_ERROR;
				}
				current = palloc(hdr.after_len ? hdr.after_len : 1);
				if (pg_pread(fd, current, hdr.after_len, hdr.offset) == hdr.after_len &&
					memcmp(current, after, hdr.after_len) == 0)
				{
					if ((hdr.before_len &&
						 pg_pwrite(fd, before, hdr.before_len, hdr.offset) != hdr.before_len) ||
						ftruncate(fd, hdr.orig_length) != 0)
					{
						ereport(WARNING,
								(errcode_for_file_access(),
								 errmsg("FILEOPS UNDO WRITE: could not restore \"%s\": %m", path)));
						result = UNDO_APPLY_ERROR;
					}
				}
				pfree(current);
				close(fd);
			}
			break;

		case FILEOPS_UNDO_TRUNCATE:
			{
				FileopsUndoTruncate hdr;
				const char *path;
				int			fd;

				if (payload_len < sizeof(FileopsUndoTruncate))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoTruncate));
				path = payload + sizeof(FileopsUndoTruncate);

				switch (fileops_undo_lstat("TRUNCATE", path, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						if (!S_ISREG(st.st_mode) || st.st_size != hdr.new_length)
						{
							result = fileops_undo_superseded("TRUNCATE", path);
							break;
						}
						fd = BasicOpenFile(path, O_RDWR | PG_BINARY);
						if (fd < 0)
						{
							if (errno != ENOENT)
							{
								ereport(WARNING,
										(errcode_for_file_access(),
										 errmsg("FILEOPS UNDO TRUNCATE: could not open \"%s\": %m",
												path)));
								result = UNDO_APPLY_ERROR;
							}
						}
						else
						{
							if (hdr.before_len &&
								pg_pwrite(fd, path + hdr.path_len, hdr.before_len,
										  hdr.new_length) != hdr.before_len)
							{
								ereport(WARNING,
										(errcode_for_file_access(),
										 errmsg("could not restore bytes of file \"%s\": %m", path)));
								result = UNDO_APPLY_ERROR;
							}
							if (ftruncate(fd, hdr.orig_length) < 0)
							{
								ereport(WARNING,
										(errcode_for_file_access(),
										 errmsg("FILEOPS UNDO TRUNCATE: could not restore length of \"%s\": %m",
												path)));
								result = UNDO_APPLY_ERROR;
							}
							close(fd);
						}
						break;
				}
			}
			break;

		case FILEOPS_UNDO_CHMOD:
			{
				FileopsUndoChmod hdr;
				const char *path;

				if (payload_len < sizeof(FileopsUndoChmod))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoChmod));
				path = payload + sizeof(FileopsUndoChmod);

				switch (fileops_undo_lstat("CHMOD", path, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						if ((st.st_mode & 07777) != hdr.new_mode)
							result = fileops_undo_superseded("CHMOD", path);
						else if (chmod(path, hdr.orig_mode) < 0 &&
								 errno != ENOENT)
						{
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("FILEOPS UNDO CHMOD: could not restore mode of \"%s\": %m",
											path)));
							result = UNDO_APPLY_ERROR;
						}
						break;
				}
			}
			break;

		case FILEOPS_UNDO_CHOWN:
			{
#ifndef WIN32
				FileopsUndoChown hdr;
				const char *path;

				if (payload_len < sizeof(FileopsUndoChown))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoChown));
				path = payload + sizeof(FileopsUndoChown);

				switch (fileops_undo_lstat("CHOWN", path, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						if (st.st_uid != hdr.new_uid || st.st_gid != hdr.new_gid)
							result = fileops_undo_superseded("CHOWN", path);
						else if (chown(path, hdr.orig_uid, hdr.orig_gid) < 0 &&
								 errno != ENOENT)
						{
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("FILEOPS UNDO CHOWN: could not restore ownership of \"%s\": %m",
											path)));
							result = UNDO_APPLY_ERROR;
						}
						break;
				}
#endif
			}
			break;

		case FILEOPS_UNDO_MKDIR:
			{
				FileopsUndoMkdir hdr;
				const char *path;

				if (payload_len < sizeof(FileopsUndoMkdir))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoMkdir));
				path = payload + sizeof(FileopsUndoMkdir);

				switch (fileops_undo_lstat("MKDIR", path, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						if (!S_ISDIR(st.st_mode))
							result = fileops_undo_superseded("MKDIR", path);
						else if ((hdr.flags & FILEOPS_MKDIR_TREE) &&
								 !InRecovery &&
								 (st.st_dev != hdr.dev || st.st_ino != hdr.ino))
							result = fileops_undo_superseded("MKDIR", path);
						else if (hdr.flags & FILEOPS_MKDIR_TREE)
						{
							if (OidIsValid(hdr.dboid))
							{
								DropDatabaseBuffers(hdr.dboid);
								ForgetDatabaseSyncRequests(hdr.dboid);
							}
							if (!rmtree(path, true))
							{
								ereport(WARNING,
										(errmsg("FILEOPS UNDO MKDIR: could not remove directory tree \"%s\"",
												path)));
								result = UNDO_APPLY_ERROR;
							}
						}
						else if (rmdir(path) < 0 && errno != ENOENT)
						{
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("FILEOPS UNDO MKDIR: could not rmdir \"%s\": %m",
											path)));
							result = UNDO_APPLY_ERROR;
						}
						break;
				}
			}
			break;

		case FILEOPS_UNDO_SYMLINK:
			{
				FileopsUndoSymlink hdr;
				const char *linkpath;
				const char *target;
				char		cur[MAXPGPATH];
				ssize_t		cur_len;

				if (payload_len < sizeof(FileopsUndoSymlink))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoSymlink));
				if (payload_len < sizeof(FileopsUndoSymlink) +
					hdr.linkpath_len + hdr.target_len)
					return UNDO_APPLY_ERROR;
				linkpath = payload + sizeof(FileopsUndoSymlink);
				target = linkpath + hdr.linkpath_len;

				cur_len = readlink(linkpath, cur, sizeof(cur));
				if (cur_len < 0)
				{
					if (errno == ENOENT)
						break;
					if (errno == EINVAL)	/* not a symlink any more */
					{
						result = fileops_undo_superseded("SYMLINK", linkpath);
						break;
					}
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO SYMLINK: could not read symbolic link \"%s\": %m",
									linkpath)));
					result = UNDO_APPLY_ERROR;
				}
				else if ((size_t) cur_len + 1 != hdr.target_len ||
						 memcmp(cur, target, cur_len) != 0)
					result = fileops_undo_superseded("SYMLINK", linkpath);
				else if (unlink(linkpath) < 0 && errno != ENOENT)
				{
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO SYMLINK: could not unlink \"%s\": %m",
									linkpath)));
					result = UNDO_APPLY_ERROR;
				}
			}
			break;

		case FILEOPS_UNDO_LINK:
			{
				FileopsUndoLink hdr;
				const char *newpath;

				if (payload_len < sizeof(FileopsUndoLink))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoLink));
				newpath = payload + sizeof(FileopsUndoLink);

				switch (fileops_undo_lstat("LINK", newpath, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						if (!InRecovery && (st.st_dev != hdr.dev || st.st_ino != hdr.ino))
							result = fileops_undo_superseded("LINK", newpath);
						else if (unlink(newpath) < 0 && errno != ENOENT)
						{
							ereport(WARNING,
									(errcode_for_file_access(),
									 errmsg("FILEOPS UNDO LINK: could not unlink \"%s\": %m",
											newpath)));
							result = UNDO_APPLY_ERROR;
						}
						break;
				}
			}
			break;

		case FILEOPS_UNDO_RENAME:
			{
				FileopsUndoRename hdr;
				const char *oldpath;
				const char *newpath;

				if (payload_len < sizeof(FileopsUndoRename))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoRename));
				if (payload_len < sizeof(FileopsUndoRename) +
					hdr.oldpath_len + hdr.newpath_len)
					return UNDO_APPLY_ERROR;
				oldpath = payload + sizeof(FileopsUndoRename);
				newpath = oldpath + hdr.oldpath_len;

				/*
				 * Rename back while newpath exists and oldpath is free (see
				 * FileopsUndoRename).  An old name reused since is not ours
				 * to replace.
				 */
				switch (fileops_undo_lstat("RENAME", newpath, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						switch (fileops_undo_lstat("RENAME", oldpath, &st))
						{
							case 0:
								if (rename(newpath, oldpath) < 0)
								{
									ereport(WARNING,
											(errcode_for_file_access(),
											 errmsg("FILEOPS UNDO RENAME: could not rename \"%s\" back to \"%s\": %m",
													newpath, oldpath)));
									result = UNDO_APPLY_ERROR;
								}
								break;
							case -1:
								result = UNDO_APPLY_ERROR;
								break;
							default:
								result = fileops_undo_superseded("RENAME",
																 oldpath);
								break;
						}
						break;
				}
			}
			break;

		case FILEOPS_UNDO_SETXATTR:
			{
				FileopsUndoSetxattr hdr;
				const char *path;
				const char *name;
				const char *orig_value;
				const char *new_value;

				if (payload_len < sizeof(FileopsUndoSetxattr))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoSetxattr));
				if (payload_len < sizeof(FileopsUndoSetxattr) + hdr.path_len +
					hdr.name_len + hdr.orig_value_len + hdr.new_value_len)
					return UNDO_APPLY_ERROR;
				path = payload + sizeof(FileopsUndoSetxattr);
				name = path + hdr.path_len;
				orig_value = name + hdr.name_len;
				new_value = orig_value + hdr.orig_value_len;

				switch (fileops_undo_lstat("SETXATTR", path, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						switch (fileops_xattr_is(path, name, new_value,
												 hdr.new_value_len))
						{
							case 0:
								result = fileops_undo_superseded("SETXATTR",
																 path);
								break;
							case -1:
								result = fileops_undo_xattr_failed("SETXATTR",
																   path, name);
								break;
							default:
								if (hdr.had_value)
								{
									if (pg_setxattr(path, name, orig_value,
													hdr.orig_value_len) < 0 &&
										errno != ENOENT)
										result = fileops_undo_xattr_failed("SETXATTR",
																		   path, name);
								}
								else if (pg_removexattr(path, name) < 0 &&
										 errno != ENOENT &&
										 errno != PG_ENOATTR)
									result = fileops_undo_xattr_failed("SETXATTR",
																	   path, name);
								break;
						}
						break;
				}
			}
			break;

		case FILEOPS_UNDO_REMOVEXATTR:
			{
				FileopsUndoRemovexattr hdr;
				const char *path;
				const char *name;
				const void *value;

				if (payload_len < sizeof(FileopsUndoRemovexattr))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoRemovexattr));
				path = payload + sizeof(FileopsUndoRemovexattr);
				name = path + hdr.path_len;
				value = name + hdr.name_len;

				switch (fileops_undo_lstat("REMOVEXATTR", path, &st))
				{
					case 0:
						break;
					case -1:
						result = UNDO_APPLY_ERROR;
						break;
					default:
						/* Restore the removed xattr, unless it is back */
						if (pg_getxattr(path, name, NULL, 0) >= 0)
							result = fileops_undo_superseded("REMOVEXATTR",
															 path);
						else if (errno != PG_ENOATTR)
							result = fileops_undo_xattr_failed("REMOVEXATTR",
															   path, name);
						else if (pg_setxattr(path, name, value,
											 hdr.value_len) < 0 &&
								 errno != ENOENT)
							result = fileops_undo_xattr_failed("REMOVEXATTR",
															   path, name);
						break;
				}
			}
			break;

		default:
			ereport(WARNING,
					(errmsg("FILEOPS UNDO: unknown subtype %u", info)));
			return UNDO_APPLY_ERROR;
	}

	INJECTION_POINT("fileops-undo-apply-end", NULL);

	/*
	 * Startup and the logical revert worker record the walk's completion
	 * after it returns, so the inverse must be durable by then; the walk
	 * makes it so (FileOpsSyncInverses).
	 */
	if (!applyingAbort && result == UNDO_APPLY_SUCCESS)
		fileops_note_inverse(info, payload, payload_len,
							 data_sync_elevel(ERROR));

	return result;
}

/*
 * Apply an abort-WAL inverse with the same identity tests as loser recovery.
 * Ordinary syscall failures remain warnings.  A failed inverse can therefore
 * leave an online effect until recovery replays the abort record; callers must
 * not infer successful filesystem rollback merely from transaction abort.
 *
 * The caller makes the inverses durable with FileOpsSyncInverses(PANIC) once
 * it has applied them all.  Failing to is a PANIC regardless of
 * data_sync_retry.  Online, we run after the abort record is flushed and the
 * transaction is marked aborted in clog, so an ERROR would re-enter
 * AbortTransaction and log a second abort record.  The caller holds off
 * checkpoints until the inverses are durable, so crash recovery replays the
 * abort record and applies the inverses again.
 */
void
FileOpsApplyInverse(uint16 info, const char *data, uint32 len)
{
	applyingAbort = true;
	PG_TRY();
	{
		(void) fileops_undo_apply(UNDO_RMID_FILEOPS, info, InvalidTransactionId,
								  InvalidOid, data, len, InvalidUndoRecPtr);
		fileops_note_inverse(info, data, len, PANIC);
	}
	PG_FINALLY();
	{
		applyingAbort = false;
	}
	PG_END_TRY();
}

/*
 * Paths that applied inverses changed and that are not durable yet, each with
 * its parent directory.  N inverses on one file need one fsync of the file
 * and one of its directory, not N of each: every caller makes a whole
 * rollback durable before it records the rollback as complete, by calling
 * FileOpsSyncInverses() once.
 *
 * Online, the inverses are applied inside AbortTransaction, where an ERROR
 * would re-enter it.  So the table is created when the transaction writes its
 * first FILEOPS UNDO (FileOpsReserveInverseSyncs), and an entry that cannot be
 * allocated there is not an error: that path is synced at once instead.
 */
typedef struct FileOpsSyncEntry
{
	char		path[MAXPGPATH];	/* hash key */
	bool		target;			/* an inverse's own path, not just a parent */
} FileOpsSyncEntry;

static HTAB *fileopsPendingSyncs = NULL;

/* Create the table now, where an out-of-memory ERROR is still harmless. */
void
FileOpsReserveInverseSyncs(void)
{
	HASHCTL		ctl;

	if (fileopsPendingSyncs != NULL)
		return;
	ctl.keysize = MAXPGPATH;
	ctl.entrysize = sizeof(FileOpsSyncEntry);
	ctl.hcxt = TopMemoryContext;
	fileopsPendingSyncs = hash_create("FILEOPS inverses to sync", 16, &ctl,
									  HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
}

/* fsync one path an inverse changed, as it is now; see FileOpsSyncInverses. */
static void
fileops_sync_path(const char *path, bool target, int elevel)
{
	struct stat st;

#ifdef USE_INJECTION_POINTS
	if (target && IS_INJECTION_POINT_ATTACHED("fileops-inverse-fsync-fail"))
	{
		errno = EIO;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	}
#endif
	if (lstat(path, &st) == 0 && !S_ISLNK(st.st_mode))
	{
		INJECTION_POINT("fileops-inverse-fsync", unconstify(char *, path));
		fsync_fname_ext(path, S_ISDIR(st.st_mode), true, elevel);
	}
}

static void
fileops_note_sync(const char *path, bool target, int elevel)
{
	FileOpsSyncEntry *entry = NULL;
	bool		found;

	if (!enableFsync)
		return;

	/*
	 * Inside AbortTransaction, an out-of-memory ERROR would re-enter it, so
	 * the table was created with the transaction's first FILEOPS UNDO.
	 * Elsewhere (abort redo, startup, the logical revert worker) the ERROR is
	 * harmless.
	 */
	if (fileopsPendingSyncs == NULL && !(applyingAbort && !InRecovery))
		FileOpsReserveInverseSyncs();
	if (fileopsPendingSyncs != NULL)
		entry = hash_search(fileopsPendingSyncs, path, HASH_ENTER_NULL,
							&found);
	if (entry == NULL)
	{
		/* No memory to defer it: make this path durable now. */
		fileops_sync_path(path, target, elevel);
		return;
	}
	if (!found)
		entry->target = false;
	entry->target |= target;
}

/* Remember the directory of a path an inverse changed, for syncing. */
static void
fileops_note_parent(const char *path, int elevel)
{
	char		parent[MAXPGPATH];
	char	   *sep;

	strlcpy(parent, path, sizeof(parent));
	sep = strrchr(parent, '/');
	if (sep == NULL)
		strlcpy(parent, ".", sizeof(parent));
	else if (sep == parent)
		parent[1] = '\0';
	else
		*sep = '\0';

	fileops_note_sync(parent, false, elevel);
}

/* Remember the paths an inverse changed, and their directories. */
static void
fileops_note_inverse(uint16 info, const char *data, uint32 len, int elevel)
{
	Size		hdrlen = 0;

	switch (info)
	{
		case FILEOPS_UNDO_CREATE:
			hdrlen = sizeof(FileopsUndoCreate);
			break;
		case FILEOPS_UNDO_WRITE:
			hdrlen = sizeof(FileopsUndoWrite);
			break;
		case FILEOPS_UNDO_TRUNCATE:
			hdrlen = sizeof(FileopsUndoTruncate);
			break;
		case FILEOPS_UNDO_CHMOD:
			hdrlen = sizeof(FileopsUndoChmod);
			break;
		case FILEOPS_UNDO_CHOWN:
			hdrlen = sizeof(FileopsUndoChown);
			break;
		case FILEOPS_UNDO_MKDIR:
			hdrlen = sizeof(FileopsUndoMkdir);
			break;
		case FILEOPS_UNDO_SYMLINK:
			hdrlen = sizeof(FileopsUndoSymlink);
			break;
		case FILEOPS_UNDO_LINK:
			hdrlen = sizeof(FileopsUndoLink);
			break;
		case FILEOPS_UNDO_RENAME:
			{
				FileopsUndoRename hdr;

				/*
				 * The old name is back, in its directory, and the new name is
				 * gone from its own.
				 */
				if (len < sizeof(hdr))
					break;
				memcpy(&hdr, data, sizeof(hdr));
				if (hdr.oldpath_len == 0 || hdr.newpath_len == 0 ||
					len < sizeof(hdr) + hdr.oldpath_len + hdr.newpath_len)
					break;
				fileops_note_sync(data + sizeof(hdr), true, elevel);
				fileops_note_parent(data + sizeof(hdr), elevel);
				fileops_note_parent(data + sizeof(hdr) + hdr.oldpath_len,
									elevel);
				return;
			}
		case FILEOPS_UNDO_SETXATTR:
			hdrlen = sizeof(FileopsUndoSetxattr);
			break;
		case FILEOPS_UNDO_REMOVEXATTR:
			hdrlen = sizeof(FileopsUndoRemovexattr);
			break;
	}
	if (hdrlen == 0 || len <= hdrlen)
		elog(PANIC, "invalid FILEOPS inverse payload");
	fileops_note_sync(data + hdrlen, true, elevel);
	fileops_note_parent(data + hdrlen, elevel);
}

/*
 * FileOpsSyncInverses - Make the inverses applied so far durable
 *
 * fsyncs each path noted since the last call once, as it is now: a file or
 * directory that still exists (a symlink is covered by its directory), and
 * the directory of each.  A path a later inverse removed is skipped; its
 * removal is in its directory, which an inverse that removed the directory
 * itself has noted in turn.  Failures are reported at elevel.  After an
 * ERROR the paths not yet synced stay noted, and the next call syncs them.
 */
void
FileOpsSyncInverses(int elevel)
{
	HASH_SEQ_STATUS seq;
	FileOpsSyncEntry *entry;
	FileOpsSyncEntry **entries;
	long		n;
	long		i = 0;

	if (fileopsPendingSyncs == NULL ||
		hash_get_num_entries(fileopsPendingSyncs) == 0)
		return;

	/*
	 * Sync from a copy of the entries: an ERROR must not leave a hash scan
	 * open, and every entry must stay noted until it is synced.  Without
	 * memory for the copy (we may be inside AbortTransaction), take the
	 * entries one at a time instead, each with a scan of its own.
	 */
	n = hash_get_num_entries(fileopsPendingSyncs);
	entries = palloc_extended(n * sizeof(FileOpsSyncEntry *),
							  MCXT_ALLOC_NO_OOM);
	if (entries != NULL)
	{
		hash_seq_init(&seq, fileopsPendingSyncs);
		while ((entry = hash_seq_search(&seq)) != NULL)
			entries[i++] = entry;
		for (i = 0; i < n; i++)
		{
			fileops_sync_path(entries[i]->path, entries[i]->target, elevel);
			(void) hash_search(fileopsPendingSyncs, entries[i]->path,
							   HASH_REMOVE, NULL);
		}
		pfree(entries);
	}
	else
	{
		for (;;)
		{
			hash_seq_init(&seq, fileopsPendingSyncs);
			entry = hash_seq_search(&seq);
			if (entry == NULL)
				break;
			hash_seq_term(&seq);
			fileops_sync_path(entry->path, entry->target, elevel);
			(void) hash_search(fileopsPendingSyncs, entry->path, HASH_REMOVE,
							   NULL);
		}
	}
}

/*
 * fileops_undo_desc - Describe a FILEOPS UNDO record for debugging
 */
static void
fileops_undo_desc(StringInfo buf, uint8 rmid, uint16 info,
				  const char *payload, Size payload_len)
{
	const char *opname;

	switch (info)
	{
		case FILEOPS_UNDO_CREATE:
			opname = "CREATE";
			break;
		case FILEOPS_UNDO_TRUNCATE:
			opname = "TRUNCATE";
			break;
		case FILEOPS_UNDO_CHMOD:
			opname = "CHMOD";
			break;
		case FILEOPS_UNDO_CHOWN:
			opname = "CHOWN";
			break;
		case FILEOPS_UNDO_MKDIR:
			opname = "MKDIR";
			break;
		case FILEOPS_UNDO_SYMLINK:
			opname = "SYMLINK";
			break;
		case FILEOPS_UNDO_LINK:
			opname = "LINK";
			break;
		case FILEOPS_UNDO_RENAME:
			opname = "RENAME";
			break;
		case FILEOPS_UNDO_SETXATTR:
			opname = "SETXATTR";
			break;
		case FILEOPS_UNDO_REMOVEXATTR:
			opname = "REMOVEXATTR";
			break;
		default:
			opname = "UNKNOWN";
			break;
	}

	appendStringInfo(buf, "fileops %s", opname);
}
