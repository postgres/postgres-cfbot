/*-------------------------------------------------------------------------
 *
 * pg_xattr.c
 *	  Cross-platform extended attribute abstraction
 *
 * Platform detection uses compiler-defined macros rather than
 * configure-time checks, avoiding meson.build/configure.ac changes.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_xattr.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <errno.h>
#include "port/pg_xattr.h"

/*
 * Platform detection via compiler macros.
 * Linux and macOS both provide <sys/xattr.h> but with different APIs.
 * FreeBSD uses <sys/extattr.h>. Windows uses NTFS Alternate Data Streams.
 */
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#define PG_HAVE_XATTR 1
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/extattr.h>
#define PG_HAVE_EXTATTR 1
#elif defined(WIN32)
#define PG_HAVE_ADS 1
#endif

/*
 * pg_setxattr - Set an extended attribute on a file
 *
 * Returns 0 on success, -1 on failure (errno set).
 */
int
pg_setxattr(const char *path, const char *name,
			const void *value, size_t size)
{
#if defined(PG_HAVE_XATTR)
#if defined(__APPLE__)
	return setxattr(path, name, value, size, 0, 0);
#else
	return setxattr(path, name, value, size, 0);
#endif

#elif defined(PG_HAVE_EXTATTR)
	ssize_t		ret;

	ret = extattr_set_file(path, EXTATTR_NAMESPACE_USER,
						   name, value, size);
	return (ret >= 0) ? 0 : -1;

#elif defined(PG_HAVE_ADS)
	char		ads_path[MAXPGPATH];
	HANDLE		hFile;
	DWORD		written;

	snprintf(ads_path, sizeof(ads_path), "%s:%s", path, name);

	hFile = CreateFileA(ads_path, GENERIC_WRITE, 0, NULL,
						CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		_dosmaperr(GetLastError());
		return -1;
	}

	if (!WriteFile(hFile, value, (DWORD) size, &written, NULL) ||
		written != (DWORD) size)
	{
		_dosmaperr(GetLastError());
		CloseHandle(hFile);
		return -1;
	}

	CloseHandle(hFile);
	return 0;

#else
	/* Unsupported platform: succeed in WAL but no-op locally */
	(void) path;
	(void) name;
	(void) value;
	(void) size;
	errno = ENOTSUP;
	return -1;
#endif
}

/*
 * pg_getxattr - Get an extended attribute value from a file
 *
 * Returns the number of bytes placed in value on success,
 * or -1 on failure (errno set).  If value is NULL or size is 0,
 * returns the size of the attribute value without reading it.
 */
ssize_t
pg_getxattr(const char *path, const char *name,
			void *value, size_t size)
{
#if defined(PG_HAVE_XATTR)
#if defined(__APPLE__)
	return getxattr(path, name, value, size, 0, 0);
#else
	return getxattr(path, name, value, size);
#endif

#elif defined(PG_HAVE_EXTATTR)
	return extattr_get_file(path, EXTATTR_NAMESPACE_USER,
							name, value, size);

#elif defined(PG_HAVE_ADS)
	char		ads_path[MAXPGPATH];
	HANDLE		hFile;
	DWORD		bytesRead;
	LARGE_INTEGER fileSize;

	snprintf(ads_path, sizeof(ads_path), "%s:%s", path, name);

	hFile = CreateFileA(ads_path, GENERIC_READ, FILE_SHARE_READ, NULL,
						OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		_dosmaperr(GetLastError());
		return -1;
	}

	if (!GetFileSizeEx(hFile, &fileSize))
	{
		_dosmaperr(GetLastError());
		CloseHandle(hFile);
		return -1;
	}

	if (value == NULL || size == 0)
	{
		CloseHandle(hFile);
		return (ssize_t) fileSize.QuadPart;
	}

	if (!ReadFile(hFile, value, (DWORD) size, &bytesRead, NULL))
	{
		_dosmaperr(GetLastError());
		CloseHandle(hFile);
		return -1;
	}

	CloseHandle(hFile);
	return (ssize_t) bytesRead;

#else
	(void) path;
	(void) name;
	(void) value;
	(void) size;
	errno = ENOTSUP;
	return -1;
#endif
}

/*
 * pg_removexattr - Remove an extended attribute from a file
 *
 * Returns 0 on success, -1 on failure (errno set).
 */
int
pg_removexattr(const char *path, const char *name)
{
#if defined(PG_HAVE_XATTR)
#if defined(__APPLE__)
	return removexattr(path, name, 0);
#else
	return removexattr(path, name);
#endif

#elif defined(PG_HAVE_EXTATTR)
	return extattr_delete_file(path, EXTATTR_NAMESPACE_USER, name);

#elif defined(PG_HAVE_ADS)
	char		ads_path[MAXPGPATH];

	snprintf(ads_path, sizeof(ads_path), "%s:%s", path, name);
	if (DeleteFileA(ads_path))
		return 0;

	_dosmaperr(GetLastError());
	return -1;

#else
	(void) path;
	(void) name;
	errno = ENOTSUP;
	return -1;
#endif
}
