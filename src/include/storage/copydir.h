/*-------------------------------------------------------------------------
 *
 * copydir.h
 *	  Copy a directory.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/copydir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPYDIR_H
#define COPYDIR_H

typedef enum FileCopyMethod
{
	FILE_COPY_METHOD_COPY,
	FILE_COPY_METHOD_CLONE,
}			FileCopyMethod;

/* GUC parameters */
extern PGDLLIMPORT int file_copy_method;

/*
 * copydir(): copy a directory tree.
 *
 * If register_for_abort_cleanup is true, the destination tree is registered
 * with FILEOPS for delete-on-abort.  Forward-path callers in transactional
 * commands (CREATE DATABASE STRATEGY=FILE_COPY, ALTER DATABASE SET
 * TABLESPACE) should pass true so that a transaction abort removes the
 * partial copy.  Recovery callers (XLOG_DBASE_CREATE_FILE_COPY redo) must
 * pass false because there is no surrounding transaction context and the
 * redo handler is itself the recovery action.
 */
extern void copydir(const char *fromdir, const char *todir, bool recurse,
					bool register_for_abort_cleanup);
extern void copy_file(const char *fromfile, const char *tofile);

#endif							/* COPYDIR_H */
