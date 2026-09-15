/*
 *	file.c
 *
 *	file system operations
 *
 *	Copyright (c) 2010-2026, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/file.c
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#include "access/visibilitymapdefs.h"
#include "common/file_perm.h"
#include "pg_upgrade.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"


/*
 * cloneFile()
 *
 * Clones/reflinks a relation file from src to dst.
 *
 * schemaName/relName are relation's SQL name (used for error messages only).
 */
void
cloneFile(const char *src, const char *dst,
		  const char *schemaName, const char *relName)
{
#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(src, dst, NULL, COPYFILE_CLONE_FORCE) < 0)
		pg_fatal("error while cloning relation \"%s.%s\" (\"%s\" to \"%s\"): %m",
				 schemaName, relName, src, dst);
#elif defined(__linux__) && defined(FICLONE)
	int			src_fd;
	int			dest_fd;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while cloning relation \"%s.%s\": could not open file \"%s\": %m",
				 schemaName, relName, src);

	if ((dest_fd = open(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("error while cloning relation \"%s.%s\": could not create file \"%s\": %m",
				 schemaName, relName, dst);

	if (ioctl(dest_fd, FICLONE, src_fd) < 0)
	{
		int			save_errno = errno;

		unlink(dst);

		pg_fatal("error while cloning relation \"%s.%s\" (\"%s\" to \"%s\"): %s",
				 schemaName, relName, src, dst, strerror(save_errno));
	}

	close(src_fd);
	close(dest_fd);
#endif
}


/*
 * copyFile()
 *
 * Copies a relation file from src to dst.
 * schemaName/relName are relation's SQL name (used for error messages only).
 */
void
copyFile(const char *src, const char *dst,
		 const char *schemaName, const char *relName)
{
#ifndef WIN32
	int			src_fd;
	int			dest_fd;
	char	   *buffer;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not open file \"%s\": %m",
				 schemaName, relName, src);

	if ((dest_fd = open(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not create file \"%s\": %m",
				 schemaName, relName, dst);

	/* copy in fairly large chunks for best efficiency */
#define COPY_BUF_SIZE (50 * BLCKSZ)

	buffer = (char *) pg_malloc(COPY_BUF_SIZE);

	/* perform data copying i.e read src source, write to destination */
	while (true)
	{
		ssize_t		nbytes = read(src_fd, buffer, COPY_BUF_SIZE);

		if (nbytes < 0)
			pg_fatal("error while copying relation \"%s.%s\": could not read file \"%s\": %m",
					 schemaName, relName, src);

		if (nbytes == 0)
			break;

		errno = 0;
		if (write(dest_fd, buffer, nbytes) != nbytes)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			pg_fatal("error while copying relation \"%s.%s\": could not write file \"%s\": %m",
					 schemaName, relName, dst);
		}
	}

	pg_free(buffer);
	close(src_fd);
	close(dest_fd);

#else							/* WIN32 */

	if (CopyFile(src, dst, true) == 0)
	{
		_dosmaperr(GetLastError());
		pg_fatal("error while copying relation \"%s.%s\" (\"%s\" to \"%s\"): %m",
				 schemaName, relName, src, dst);
	}

#endif							/* WIN32 */
}


/*
 * copyFileByRange()
 *
 * Copies a relation file from src to dst.
 * schemaName/relName are relation's SQL name (used for error messages only).
 */
void
copyFileByRange(const char *src, const char *dst,
				const char *schemaName, const char *relName)
{
#ifdef HAVE_COPY_FILE_RANGE
	int			src_fd;
	int			dest_fd;
	ssize_t		nbytes;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not open file \"%s\": %m",
				 schemaName, relName, src);

	if ((dest_fd = open(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not create file \"%s\": %m",
				 schemaName, relName, dst);

	do
	{
		nbytes = copy_file_range(src_fd, NULL, dest_fd, NULL, SSIZE_MAX, 0);
		if (nbytes < 0)
			pg_fatal("error while copying relation \"%s.%s\": could not copy file range from \"%s\" to \"%s\": %m",
					 schemaName, relName, src, dst);
	}
	while (nbytes > 0);

	close(src_fd);
	close(dest_fd);
#endif
}


/*
 * linkFile()
 *
 * Hard-links a relation file from src to dst.
 * schemaName/relName are relation's SQL name (used for error messages only).
 */
void
linkFile(const char *src, const char *dst,
		 const char *schemaName, const char *relName)
{
	if (link(src, dst) < 0)
		pg_fatal("error while creating link for relation \"%s.%s\" (\"%s\" to \"%s\"): %m",
				 schemaName, relName, src, dst);
}


void
check_file_clone(void)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", old_cluster.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.clonetest", new_cluster.pgdata);
	unlink(new_link_file);		/* might fail */

#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(existing_file, new_link_file, NULL, COPYFILE_CLONE_FORCE) < 0)
		pg_fatal("could not clone file between old and new data directories: %m");
#elif defined(__linux__) && defined(FICLONE)
	{
		int			src_fd;
		int			dest_fd;

		if ((src_fd = open(existing_file, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %m",
					 existing_file);

		if ((dest_fd = open(new_link_file, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
							pg_file_create_mode)) < 0)
			pg_fatal("could not create file \"%s\": %m",
					 new_link_file);

		if (ioctl(dest_fd, FICLONE, src_fd) < 0)
			pg_fatal("could not clone file between old and new data directories: %m");

		close(src_fd);
		close(dest_fd);
	}
#else
	pg_fatal("file cloning not supported on this platform");
#endif

	unlink(new_link_file);
}

void
check_copy_file_range(void)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", old_cluster.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.copy_file_range_test", new_cluster.pgdata);
	unlink(new_link_file);		/* might fail */

#if defined(HAVE_COPY_FILE_RANGE)
	{
		int			src_fd;
		int			dest_fd;

		if ((src_fd = open(existing_file, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %m",
					 existing_file);

		if ((dest_fd = open(new_link_file, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
							pg_file_create_mode)) < 0)
			pg_fatal("could not create file \"%s\": %m",
					 new_link_file);

		if (copy_file_range(src_fd, NULL, dest_fd, NULL, SSIZE_MAX, 0) < 0)
			pg_fatal("could not copy file range between old and new data directories: %m");

		close(src_fd);
		close(dest_fd);
	}
#else
	pg_fatal("copy_file_range not supported on this platform");
#endif

	unlink(new_link_file);
}

void
check_hard_link(transferMode transfer_mode)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", old_cluster.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.linktest", new_cluster.pgdata);
	unlink(new_link_file);		/* might fail */

	if (link(existing_file, new_link_file) < 0)
	{
		if (transfer_mode == TRANSFER_MODE_LINK)
			pg_fatal("could not create hard link between old and new data directories: %m\n"
					 "In link mode the old and new data directories must be on the same file system.");
		else if (transfer_mode == TRANSFER_MODE_SWAP)
			pg_fatal("could not create hard link between old and new data directories: %m\n"
					 "In swap mode the old and new data directories must be on the same file system.");
		else
			pg_fatal("unrecognized transfer mode");
	}

	unlink(new_link_file);
}

/*
 * rewriteVisibilityMap()
 *
 * Rewrite a relation's visibility map fork, all segments of it, from the old
 * 2-bits-per-heap-block layout to the 4-bits-per-heap-block layout that adds
 * VISIBILITYMAP_LOCATOR_SPLIT.  The all-visible and all-frozen bits are
 * preserved.  The split bit is left clear, which is correct because an
 * upgraded cluster has no selective-indexed update whose index entries could
 * disagree.
 *
 * Each old map byte describes four heap blocks and expands to two new bytes,
 * so old map page N becomes new map pages 2N and 2N+1.  Working a page at a
 * time keeps the arithmetic page-local.  Because the fork doubles in length,
 * new pages are placed in segment files by their absolute block number rather
 * than alongside the old segment they came from, and their checksums are
 * computed with that block number.  An all-zeroes old page stays all-zeroes,
 * so that it is still accepted as a new page.
 *
 * fromfile and tofile name the first segment of the old and new forks.
 */
void
rewriteVisibilityMap(const char *fromfile, const char *tofile,
					 const char *nspname, const char *relname)
{
	BlockNumber segsize = new_cluster.controldata.largesz;
	BlockNumber new_blkno = 0;
	int			dst_fd = -1;
	char		dst_path[MAXPGPATH];
	PGAlignedBlock src;
	PGAlignedBlock dst[2];

	/* The map data area of a page; see visibilitymap.c. */
#define MAPSIZE (BLCKSZ - MAXALIGN(SizeOfPageHeaderData))

	StaticAssertDecl(BITS_PER_HEAPBLOCK == 4,
					 "rewriteVisibilityMap assumes a 4-bit new layout");
	StaticAssertDecl(MAPSIZE % 2 == 0,
					 "rewriteVisibilityMap assumes an even map size");

	for (int segno = 0;; segno++)
	{
		char		src_path[MAXPGPATH];
		int			src_fd;
		ssize_t		nread;
		BlockNumber npages = 0;

		if (segno == 0)
			strlcpy(src_path, fromfile, sizeof(src_path));
		else
			snprintf(src_path, sizeof(src_path), "%s.%d", fromfile, segno);

		if ((src_fd = open(src_path, O_RDONLY | PG_BINARY, 0)) < 0)
		{
			if (segno > 0 && errno == ENOENT)
				break;
			pg_fatal("error while copying relation \"%s.%s\": could not open file \"%s\": %m",
					 nspname, relname, src_path);
		}

		while ((nread = read(src_fd, src.data, BLCKSZ)) == BLCKSZ)
		{
			char	   *old_map = src.data + MAXALIGN(SizeOfPageHeaderData);

			npages++;

			if (PageIsNew((Page) src.data))
			{
				memset(dst[0].data, 0, BLCKSZ);
				memset(dst[1].data, 0, BLCKSZ);
			}
			else
			{
				for (int i = 0; i < 2; i++)
				{
					memset(dst[i].data, 0, BLCKSZ);
					memcpy(dst[i].data, src.data, SizeOfPageHeaderData);
				}

				for (int i = 0; i < MAPSIZE; i++)
				{
					uint8		old = (uint8) old_map[i];
					char	   *new_map = dst[(2 * i) / MAPSIZE].data +
						MAXALIGN(SizeOfPageHeaderData) + (2 * i) % MAPSIZE;

					/* Four 2-bit entries become two bytes of 4-bit entries. */
					new_map[0] = (char) ((old & 0x03) | ((old & 0x0c) << 2));
					new_map[1] = (char) (((old & 0x30) >> 4) | ((old & 0xc0) >> 2));
				}
			}

			for (int i = 0; i < 2; i++)
			{
				/* Start the next destination segment when we reach it. */
				if (new_blkno % segsize == 0)
				{
					BlockNumber dst_segno = new_blkno / segsize;

					if (dst_fd >= 0)
						close(dst_fd);
					if (dst_segno == 0)
						strlcpy(dst_path, tofile, sizeof(dst_path));
					else
						snprintf(dst_path, sizeof(dst_path), "%s.%u", tofile, dst_segno);
					unlink(dst_path);
					if ((dst_fd = open(dst_path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
									   pg_file_create_mode)) < 0)
						pg_fatal("error while copying relation \"%s.%s\": could not create file \"%s\": %m",
								 nspname, relname, dst_path);
				}

				if (new_cluster.controldata.data_checksum_version != PG_DATA_CHECKSUM_OFF &&
					!PageIsNew((Page) dst[i].data))
					((PageHeader) dst[i].data)->pd_checksum =
						pg_checksum_page(dst[i].data, new_blkno);

				errno = 0;
				if (write(dst_fd, dst[i].data, BLCKSZ) != BLCKSZ)
				{
					/* if write didn't set errno, assume no disk space */
					if (errno == 0)
						errno = ENOSPC;
					pg_fatal("error while copying relation \"%s.%s\": could not write file \"%s\": %m",
							 nspname, relname, dst_path);
				}
				new_blkno++;
			}
		}

		if (nread < 0)
			pg_fatal("error while copying relation \"%s.%s\": could not read file \"%s\": %m",
					 nspname, relname, src_path);
		if (nread != 0)
			pg_fatal("error while copying relation \"%s.%s\": partial page found in file \"%s\"",
					 nspname, relname, src_path);
		close(src_fd);

		/* Like transfer_relfile(), an empty segment ends the fork. */
		if (npages == 0)
			break;
	}

	if (dst_fd >= 0)
		close(dst_fd);

#undef MAPSIZE
}
