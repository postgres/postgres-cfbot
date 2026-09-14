/*-------------------------------------------------------------------------
 *
 * fileops_undo.c
 *	  FILEOPS UNDO resource manager
 *
 * This module implements the UNDO apply callbacks for transactional file
 * operations.  UNDO covers structural filesystem operations (file existence,
 * permissions, directory structure).  File content recovery uses traditional
 * WAL (via FileOpsWrite), not FILEOPS UNDO.
 *
 * On transaction abort, each FILEOPS UNDO record reverses the structural
 * change made by the corresponding FileOps* function:
 *   - CREATE:     unlink the created file
 *   - RENAME:     rename back to the original path
 *   - TRUNCATE:   ftruncate to original length
 *   - CHMOD:      chmod to original mode
 *   - CHOWN:      chown to original uid/gid
 *   - MKDIR:      rmdir the created directory
 *   - SYMLINK:    unlink the symlink
 *   - LINK:       unlink the hard link
 *   - SETXATTR:   restore/remove the original xattr
 *   - REMOVEXATTR: restore the original xattr value
 *
 * DELETE and RMDIR are handled by PendingFileOps (deferred execution)
 * and do not need UNDO records since they execute only at commit time.
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
#include "port/pg_xattr.h"
#include "storage/fd.h"
#include "storage/fileops.h"
#include "utils/injection_point.h"

/*
 * UNDO subtype constants and payload structures are defined in fileops.h,
 * shared between this file (apply side) and fileops.c (insert side).
 */

/* Forward declarations */
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
 * fileops_undo_apply - Apply a single FILEOPS UNDO record
 *
 * Reverses the structural filesystem operation.
 */
static UndoApplyResult
fileops_undo_apply(uint8 rmid, uint16 info, TransactionId xid, Oid reloid,
				   const char *payload, Size payload_len, UndoRecPtr urec_ptr)
{
	UndoApplyResult result = UNDO_APPLY_SUCCESS;

	Assert(rmid == UNDO_RMID_FILEOPS);

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

				if (unlink(path) < 0 && errno != ENOENT)
				{
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO CREATE: could not unlink \"%s\": %m",
									path)));
					result = UNDO_APPLY_ERROR;
				}
				else
					ereport(DEBUG2,
							(errmsg("FILEOPS UNDO CREATE: unlinked \"%s\"",
									path)));
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
				oldpath = payload + sizeof(FileopsUndoRename);
				newpath = oldpath + hdr.oldpath_len;

				/* Reverse the rename: newpath -> oldpath */
				if (rename(newpath, oldpath) < 0)
				{
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO RENAME: could not rename \"%s\" back to \"%s\": %m",
									newpath, oldpath)));
					result = UNDO_APPLY_ERROR;
				}
				else
					ereport(DEBUG2,
							(errmsg("FILEOPS UNDO RENAME: renamed \"%s\" back to \"%s\"",
									newpath, oldpath)));
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

				if (chmod(path, hdr.orig_mode) < 0 && errno != ENOENT)
				{
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO CHMOD: could not restore mode of \"%s\": %m",
									path)));
					result = UNDO_APPLY_ERROR;
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

				if (chown(path, hdr.orig_uid, hdr.orig_gid) < 0 &&
					errno != ENOENT)
				{
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO CHOWN: could not restore ownership of \"%s\": %m",
									path)));
					result = UNDO_APPLY_ERROR;
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

				if (rmdir(path) < 0 && errno != ENOENT)
				{
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO MKDIR: could not rmdir \"%s\": %m",
									path)));
					result = UNDO_APPLY_ERROR;
				}
			}
			break;

		case FILEOPS_UNDO_SYMLINK:
			{
				FileopsUndoSymlink hdr;
				const char *linkpath;

				if (payload_len < sizeof(FileopsUndoSymlink))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoSymlink));
				linkpath = payload + sizeof(FileopsUndoSymlink);

				if (unlink(linkpath) < 0 && errno != ENOENT)
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

				if (unlink(newpath) < 0 && errno != ENOENT)
				{
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("FILEOPS UNDO LINK: could not unlink \"%s\": %m",
									newpath)));
					result = UNDO_APPLY_ERROR;
				}
			}
			break;

		case FILEOPS_UNDO_SETXATTR:
			{
				FileopsUndoSetxattr hdr;
				const char *path;
				const char *name;

				if (payload_len < sizeof(FileopsUndoSetxattr))
					return UNDO_APPLY_ERROR;
				memcpy(&hdr, payload, sizeof(FileopsUndoSetxattr));
				path = payload + sizeof(FileopsUndoSetxattr);
				name = path + hdr.path_len;

				if (hdr.had_value)
				{
					/* Restore original value */
					const void *orig_value = name + hdr.name_len;

					if (pg_setxattr(path, name, orig_value,
									hdr.orig_value_len) < 0 &&
						errno != ENOENT)
						result = fileops_undo_xattr_failed("SETXATTR", path,
														   name);
				}
				else
				{
					/* xattr didn't exist before, remove it */
					if (pg_removexattr(path, name) < 0 &&
						errno != ENOENT && errno != PG_ENOATTR)
						result = fileops_undo_xattr_failed("SETXATTR", path,
														   name);
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

				/* Restore the removed xattr */
				if (pg_setxattr(path, name, value, hdr.value_len) < 0 &&
					errno != ENOENT)
					result = fileops_undo_xattr_failed("REMOVEXATTR", path,
													   name);
			}
			break;

		default:
			ereport(WARNING,
					(errmsg("FILEOPS UNDO: unknown subtype %u", info)));
			return UNDO_APPLY_ERROR;
	}

	INJECTION_POINT("fileops-undo-apply-end", NULL);

	return result;
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
		case FILEOPS_UNDO_RENAME:
			opname = "RENAME";
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
