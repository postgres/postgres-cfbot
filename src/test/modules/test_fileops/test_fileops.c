/*-------------------------------------------------------------------------
 *
 * test_fileops.c
 *		Test module exposing FileOps C API to SQL for regression testing.
 *
 * This module provides SQL-callable wrappers around the FileOps functions
 * that have no DDL-level callers, enabling direct testing of:
 *   - FileOpsTruncate (with UNDO rollback of file size)
 *   - FileOpsChmod (with UNDO rollback of permissions)
 *   - FileOpsLink (with UNDO rollback of hard links)
 *   - FileOpsSetXattr / FileOpsRemoveXattr (with UNDO rollback)
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_xattr.h"
#include "access/xlogdefs.h"
#include "storage/fd.h"
#include "storage/fileops.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_fileops_create_tempfile);
PG_FUNCTION_INFO_V1(test_fileops_create);
PG_FUNCTION_INFO_V1(test_fileops_truncate);
PG_FUNCTION_INFO_V1(test_fileops_file_size);
PG_FUNCTION_INFO_V1(test_fileops_chmod);
PG_FUNCTION_INFO_V1(test_fileops_get_mode);
PG_FUNCTION_INFO_V1(test_fileops_link);
PG_FUNCTION_INFO_V1(test_fileops_file_exists);
PG_FUNCTION_INFO_V1(test_fileops_setxattr);
PG_FUNCTION_INFO_V1(test_fileops_getxattr);
PG_FUNCTION_INFO_V1(test_fileops_removexattr);
PG_FUNCTION_INFO_V1(test_fileops_data_dir);
PG_FUNCTION_INFO_V1(test_fileops_write);
PG_FUNCTION_INFO_V1(test_fileops_chown);
PG_FUNCTION_INFO_V1(test_fileops_symlink);

PG_FUNCTION_INFO_V1(test_fileops_delete);
PG_FUNCTION_INFO_V1(test_fileops_rename);
PG_FUNCTION_INFO_V1(test_fileops_mkdir);
PG_FUNCTION_INFO_V1(test_fileops_rmdir);

/*
 * test_fileops_create_tempfile - Create a test file in data directory.
 *
 * Creates a file with some content using FileOpsCreate + write, returning
 * the absolute path.  The file is created with 1024 bytes of content.
 */
Datum
test_fileops_create_tempfile(PG_FUNCTION_ARGS)
{
	text	   *filename = PG_GETARG_TEXT_PP(0);
	char	   *fname = text_to_cstring(filename);
	char		filepath[MAXPGPATH];
	int			fd;
	char		buf[1024];

	snprintf(filepath, MAXPGPATH, "%s/%s", DataDir, fname);

	/* Use raw create + write so we have a file to manipulate */
	fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC | PG_BINARY, 0644);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", filepath)));

	/* Fill with 1024 bytes */
	memset(buf, 'X', sizeof(buf));
	if (write(fd, buf, sizeof(buf)) != sizeof(buf))
	{
		int			save_errno = errno;

		close(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", filepath)));
	}
	close(fd);

	PG_RETURN_TEXT_P(cstring_to_text(filepath));
}

/*
 * test_fileops_create - Create a file using FileOpsCreate.
 *
 * Unlike test_fileops_create_tempfile (which uses raw open()/write() to set
 * up fixture files outside any transactional path), this calls FileOpsCreate
 * directly so tests can exercise its WAL-before-syscall crash-recovery
 * behavior.
 */
Datum
test_fileops_create(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	int			mode = PG_GETARG_INT32(1);
	char	   *filepath = text_to_cstring(filepath_text);
	int			fd;

	fd = FileOpsCreate(filepath, O_WRONLY | PG_BINARY, (mode_t) mode, true);
	if (fd >= 0)
		CloseTransientFile(fd);

	pfree(filepath);
	PG_RETURN_VOID();
}

/*
 * test_fileops_truncate - Truncate a file using FileOpsTruncate.
 *
 * This exercises the UNDO path: on rollback, the original file size
 * should be restored.
 */
Datum
test_fileops_truncate(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	int64		length = PG_GETARG_INT64(1);
	char	   *filepath = text_to_cstring(filepath_text);

	FileOpsTruncate(filepath, (off_t) length);

	pfree(filepath);
	PG_RETURN_VOID();
}

/*
 * test_fileops_file_size - Get the current size of a file.
 */
Datum
test_fileops_file_size(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	char	   *filepath = text_to_cstring(filepath_text);
	struct stat st;

	if (stat(filepath, &st) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filepath)));

	pfree(filepath);
	PG_RETURN_INT64((int64) st.st_size);
}

/*
 * test_fileops_chmod - Change file permissions using FileOpsChmod.
 *
 * This exercises the UNDO path: on rollback, the original permissions
 * should be restored.
 */
Datum
test_fileops_chmod(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	int			mode = PG_GETARG_INT32(1);
	char	   *filepath = text_to_cstring(filepath_text);

	if (FileOpsChmod(filepath, (mode_t) mode) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not chmod file \"%s\": %m", filepath)));

	pfree(filepath);
	PG_RETURN_VOID();
}

/*
 * test_fileops_get_mode - Get file permission bits.
 */
Datum
test_fileops_get_mode(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	char	   *filepath = text_to_cstring(filepath_text);
	struct stat st;

	if (stat(filepath, &st) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filepath)));

	pfree(filepath);
	PG_RETURN_INT32((int32) (st.st_mode & 0777));
}

/*
 * test_fileops_link - Create a hard link using FileOpsLink.
 *
 * This exercises the UNDO path: on rollback, the link should be removed.
 */
Datum
test_fileops_link(PG_FUNCTION_ARGS)
{
	text	   *oldpath_text = PG_GETARG_TEXT_PP(0);
	text	   *newpath_text = PG_GETARG_TEXT_PP(1);
	char	   *oldpath = text_to_cstring(oldpath_text);
	char	   *newpath = text_to_cstring(newpath_text);

	if (FileOpsLink(oldpath, newpath) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not link \"%s\" to \"%s\": %m",
						oldpath, newpath)));

	pfree(oldpath);
	pfree(newpath);
	PG_RETURN_VOID();
}

/*
 * test_fileops_file_exists - Check whether a file exists.
 */
Datum
test_fileops_file_exists(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	char	   *filepath = text_to_cstring(filepath_text);
	struct stat st;
	bool		exists;

	exists = (stat(filepath, &st) == 0);

	pfree(filepath);
	PG_RETURN_BOOL(exists);
}

/*
 * test_fileops_setxattr - Set an extended attribute using FileOpsSetXattr.
 *
 * Returns true if the platform supports xattrs, false if ENOTSUP.
 * This exercises the UNDO path: on rollback, the original xattr value
 * (or absence) should be restored.
 */
Datum
test_fileops_setxattr(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	text	   *name_text = PG_GETARG_TEXT_PP(1);
	text	   *value_text = PG_GETARG_TEXT_PP(2);
	char	   *filepath = text_to_cstring(filepath_text);
	char	   *name = text_to_cstring(name_text);
	char	   *value = text_to_cstring(value_text);
	int			ret;

	ret = FileOpsSetXattr(filepath, name, value, strlen(value));
	if (ret != 0)
	{
		if (errno == ENOTSUP || errno == EOPNOTSUPP ||
			errno == EPERM || errno == EACCES)
		{
			pfree(filepath);
			pfree(name);
			pfree(value);
			PG_RETURN_BOOL(false);
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not set xattr \"%s\" on \"%s\": %m",
						name, filepath)));
	}

	pfree(filepath);
	pfree(name);
	pfree(value);
	PG_RETURN_BOOL(true);
}

/*
 * test_fileops_getxattr - Get an extended attribute value.
 *
 * Returns NULL if the attribute doesn't exist or xattrs are unsupported.
 */
Datum
test_fileops_getxattr(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	text	   *name_text = PG_GETARG_TEXT_PP(1);
	char	   *filepath = text_to_cstring(filepath_text);
	char	   *name = text_to_cstring(name_text);
	char		buf[1024];
	ssize_t		len;

	len = pg_getxattr(filepath, name, buf, sizeof(buf) - 1);
	if (len < 0)
	{
		pfree(filepath);
		pfree(name);
		PG_RETURN_NULL();
	}

	buf[len] = '\0';

	pfree(filepath);
	pfree(name);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * test_fileops_removexattr - Remove an extended attribute using
 * FileOpsRemoveXattr.
 *
 * Returns true if successful, false if xattrs are unsupported.
 * This exercises the UNDO path: on rollback, the removed xattr should
 * be restored.
 */
Datum
test_fileops_removexattr(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_GETARG_TEXT_PP(0);
	text	   *name_text = PG_GETARG_TEXT_PP(1);
	char	   *filepath = text_to_cstring(filepath_text);
	char	   *name = text_to_cstring(name_text);
	int			ret;

	ret = FileOpsRemoveXattr(filepath, name);
	if (ret != 0)
	{
		if (errno == ENOTSUP || errno == EOPNOTSUPP)
		{
			pfree(filepath);
			pfree(name);
			PG_RETURN_BOOL(false);
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove xattr \"%s\" from \"%s\": %m",
						name, filepath)));
	}

	pfree(filepath);
	pfree(name);
	PG_RETURN_BOOL(true);
}

/*
 * test_fileops_data_dir - Return the data directory path.
 */
Datum
test_fileops_data_dir(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(DataDir));
}

/*
 * test_fileops_delete - Deferred delete via FileOpsDelete(at_commit=true).
 *
 * Exercises the log-at-commit-execution path: the delete happens (and is
 * WAL-logged) in FileOpsDoPendingOps() at commit, so a physical standby
 * reproduces it by replay.
 */
Datum
test_fileops_delete(PG_FUNCTION_ARGS)
{
	char	   *filepath = text_to_cstring(PG_GETARG_TEXT_PP(0));

	FileOpsDelete(filepath, true);
	pfree(filepath);
	PG_RETURN_VOID();
}

/*
 * test_fileops_rename - Deferred rename via FileOpsRename (always at_commit).
 */
Datum
test_fileops_rename(PG_FUNCTION_ARGS)
{
	char	   *oldpath = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *newpath = text_to_cstring(PG_GETARG_TEXT_PP(1));

	if (FileOpsRename(oldpath, newpath) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename \"%s\" to \"%s\": %m",
						oldpath, newpath)));
	pfree(oldpath);
	pfree(newpath);
	PG_RETURN_VOID();
}

/*
 * test_fileops_mkdir - Create a directory via FileOpsMkdir.
 *
 * FileOpsMkdir creates the directory immediately and registers an
 * rmdir-on-abort; on commit the directory persists.  Used to set up a
 * directory the test can then remove with test_fileops_rmdir.
 */
Datum
test_fileops_mkdir(PG_FUNCTION_ARGS)
{
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			mode = PG_GETARG_INT32(1);

	if (FileOpsMkdir(path, (mode_t) mode) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", path)));
	pfree(path);
	PG_RETURN_VOID();
}

/*
 * test_fileops_rmdir - Deferred rmdir via FileOpsRmdir(at_commit=true).
 */
Datum
test_fileops_rmdir(PG_FUNCTION_ARGS)
{
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

	FileOpsRmdir(path, true);
	pfree(path);
	PG_RETURN_VOID();
}

/*
 * test_fileops_write - Write a string at an offset via FileOpsWrite.
 */
Datum
test_fileops_write(PG_FUNCTION_ARGS)
{
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		offset = PG_GETARG_INT64(1);
	char	   *data = text_to_cstring(PG_GETARG_TEXT_PP(2));

	(void) FileOpsWrite(path, (off_t) offset, data, (uint32) strlen(data));
	pfree(path);
	pfree(data);
	PG_RETURN_VOID();
}

/*
 * test_fileops_chown - Change ownership via FileOpsChown.
 */
Datum
test_fileops_chown(PG_FUNCTION_ARGS)
{
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));

	(void) FileOpsChown(path, (uid_t) PG_GETARG_INT32(1),
						(gid_t) PG_GETARG_INT32(2));
	pfree(path);
	PG_RETURN_VOID();
}

/*
 * test_fileops_symlink - Create a symbolic link via FileOpsSymlink.
 */
Datum
test_fileops_symlink(PG_FUNCTION_ARGS)
{
	char	   *target = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *linkpath = text_to_cstring(PG_GETARG_TEXT_PP(1));

	(void) FileOpsSymlink(target, linkpath);
	pfree(target);
	pfree(linkpath);
	PG_RETURN_VOID();
}
