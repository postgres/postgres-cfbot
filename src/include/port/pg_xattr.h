/*-------------------------------------------------------------------------
 *
 * pg_xattr.h
 *	  Cross-platform extended attribute abstraction
 *
 * Provides pg_setxattr() and pg_removexattr() that work across:
 *   - Linux: <sys/xattr.h> setxattr/removexattr
 *   - macOS: <sys/xattr.h> setxattr/removexattr (extra options param)
 *   - FreeBSD: <sys/extattr.h> extattr_set_file/extattr_delete_file
 *   - Windows: NTFS Alternate Data Streams
 *   - Fallback: returns ENOTSUP with WARNING
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/port/pg_xattr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_XATTR_H
#define PG_XATTR_H

/*
 * ENODATA is Linux-specific.  FreeBSD/macOS use ENOATTR for "attribute not
 * found".  Provide a portable PG_ENOATTR so callers don't need #ifdefs.
 */
#if defined(ENOATTR)
#define PG_ENOATTR ENOATTR
#elif defined(ENODATA)
#define PG_ENOATTR ENODATA
#else
#define PG_ENOATTR ENOENT		/* last-resort fallback */
#endif

extern int	pg_setxattr(const char *path, const char *name,
						const void *value, size_t size);
extern ssize_t pg_getxattr(const char *path, const char *name,
						   void *value, size_t size);
extern int	pg_removexattr(const char *path, const char *name);

#endif							/* PG_XATTR_H */
