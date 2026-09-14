/*-------------------------------------------------------------------------
 *
 * fileopsdesc.c
 *	  rmgr descriptor routines for storage/file/fileops.c
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/fileopsdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/fileops.h"

void
fileops_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *data = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_FILEOPS_CREATE:
			{
				xl_fileops_create *xlrec = (xl_fileops_create *) data;
				const char *path = data + SizeOfFileOpsCreate;

				appendStringInfo(buf, "create \"%s\" flags 0x%x mode 0%o",
								 path, xlrec->flags, xlrec->mode);
			}
			break;

		case XLOG_FILEOPS_WRITE:
			{
				xl_fileops_write *xlrec = (xl_fileops_write *) data;
				const char *path = data + SizeOfFileOpsWrite;

				appendStringInfo(buf, "write \"%s\" offset %lld len %u",
								 path, (long long) xlrec->offset, xlrec->len);
			}
			break;

		case XLOG_FILEOPS_RENAME:
			{
				xl_fileops_rename *xlrec = (xl_fileops_rename *) data;
				const char *oldpath = data + SizeOfFileOpsRename;
				const char *newpath = oldpath + xlrec->oldpath_len;

				appendStringInfo(buf, "rename \"%s\" to \"%s\"",
								 oldpath, newpath);
			}
			break;

		case XLOG_FILEOPS_DELETE:
			{
				xl_fileops_delete *xlrec = (xl_fileops_delete *) data;
				const char *path = data + SizeOfFileOpsDelete;

				appendStringInfo(buf, "delete \"%s\" at_%s",
								 path,
								 xlrec->at_commit ? "commit" : "abort");
			}
			break;

		case XLOG_FILEOPS_SYMLINK:
			{
				xl_fileops_symlink *xlrec = (xl_fileops_symlink *) data;
				const char *target = data + SizeOfFileOpsSymlink;
				const char *linkpath = target + xlrec->target_len;

				appendStringInfo(buf, "symlink \"%s\" -> \"%s\"",
								 linkpath, target);
			}
			break;

		case XLOG_FILEOPS_LINK:
			{
				xl_fileops_link *xlrec = (xl_fileops_link *) data;
				const char *oldpath = data + SizeOfFileOpsLink;
				const char *newpath = oldpath + xlrec->oldpath_len;

				appendStringInfo(buf, "link \"%s\" -> \"%s\"",
								 newpath, oldpath);
			}
			break;

		case XLOG_FILEOPS_MKDIR:
			{
				xl_fileops_mkdir *xlrec = (xl_fileops_mkdir *) data;
				const char *path = data + SizeOfFileOpsMkdir;

				appendStringInfo(buf, "mkdir \"%s\" mode 0%o",
								 path, (unsigned int) xlrec->mode);
			}
			break;

		case XLOG_FILEOPS_RMDIR:
			{
				xl_fileops_rmdir *xlrec = (xl_fileops_rmdir *) data;
				const char *path = data + SizeOfFileOpsRmdir;

				appendStringInfo(buf, "rmdir \"%s\" at_%s",
								 path,
								 xlrec->at_commit ? "commit" : "abort");
			}
			break;

		case XLOG_FILEOPS_CHMOD:
			{
				xl_fileops_chmod *xlrec = (xl_fileops_chmod *) data;
				const char *path = data + SizeOfFileOpsChmod;

				appendStringInfo(buf, "chmod \"%s\" mode 0%o",
								 path, (unsigned int) xlrec->mode);
			}
			break;

		case XLOG_FILEOPS_CHOWN:
			{
				xl_fileops_chown *xlrec = (xl_fileops_chown *) data;
				const char *path = data + SizeOfFileOpsChown;

				appendStringInfo(buf, "chown \"%s\" uid %d gid %d",
								 path, (int) xlrec->uid, (int) xlrec->gid);
			}
			break;

		case XLOG_FILEOPS_TRUNCATE:
			{
				xl_fileops_truncate *xlrec = (xl_fileops_truncate *) data;
				const char *path = data + SizeOfFileOpsTruncate;

				appendStringInfo(buf, "truncate \"%s\" to %lld bytes",
								 path, (long long) xlrec->length);
			}
			break;

		case XLOG_FILEOPS_SETXATTR:
			{
				xl_fileops_setxattr *xlrec = (xl_fileops_setxattr *) data;
				const char *path = data + SizeOfFileOpsSetxattr;
				const char *name = path + xlrec->path_len;

				appendStringInfo(buf, "setxattr \"%s\" name \"%s\" len %u",
								 path, name, xlrec->value_len);
			}
			break;

		case XLOG_FILEOPS_REMOVEXATTR:
			{
				xl_fileops_removexattr *xlrec =
					(xl_fileops_removexattr *) data;
				const char *path = data + SizeOfFileOpsRemovexattr;
				const char *name = path + xlrec->path_len;

				appendStringInfo(buf, "removexattr \"%s\" name \"%s\"",
								 path, name);
			}
			break;

		default:
			appendStringInfo(buf, "unknown fileops op code %u", info);
			break;
	}
}

const char *
fileops_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_FILEOPS_CREATE:
			id = "CREATE";
			break;
		case XLOG_FILEOPS_DELETE:
			id = "DELETE";
			break;
		case XLOG_FILEOPS_RENAME:
			id = "RENAME";
			break;
		case XLOG_FILEOPS_WRITE:
			id = "WRITE";
			break;
		case XLOG_FILEOPS_TRUNCATE:
			id = "TRUNCATE";
			break;
		case XLOG_FILEOPS_CHMOD:
			id = "CHMOD";
			break;
		case XLOG_FILEOPS_CHOWN:
			id = "CHOWN";
			break;
		case XLOG_FILEOPS_MKDIR:
			id = "MKDIR";
			break;
		case XLOG_FILEOPS_RMDIR:
			id = "RMDIR";
			break;
		case XLOG_FILEOPS_SYMLINK:
			id = "SYMLINK";
			break;
		case XLOG_FILEOPS_LINK:
			id = "LINK";
			break;
		case XLOG_FILEOPS_SETXATTR:
			id = "SETXATTR";
			break;
		case XLOG_FILEOPS_REMOVEXATTR:
			id = "REMOVEXATTR";
			break;
	}

	return id;
}
