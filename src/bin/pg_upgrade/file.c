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

#include "common/file_perm.h"
#include "pg_upgrade.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"
#include "access/visibilitymapdefs.h"


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
 * Transform a visibility map file from the old 2-bits-per-heap-block layout
 * (VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN) to the new
 * 4-bits-per-heap-block layout that additionally carries
 * VISIBILITYMAP_LOCATOR_SPLIT.  The two existing bits are preserved; the new
 * split bit is left clear, which is correct because an upgraded cluster has no
 * in-flight selective-indexed offset disagreement.
 *
 * The transform is done a heap block at a time: for every block represented in
 * the old file we read its two bits and store them at that same block's
 * position in the new (wider) file.  Because the new layout packs half as many
 * blocks per page, the map grows (one old page's blocks span two new pages)
 * but expressing the copy per block keeps the packing arithmetic in the shared
 * old/new macros rather than open-coded here.  This mirrors the 9.6 rewrite
 * that widened the map from one bit to two for the frozen bit.
 */
void
rewriteVisibilityMap(const char *fromfile, const char *tofile,
					 const char *nspname, const char *relname)
{
	int			src_fd;
	int			dst_fd;
	PGAlignedBlock src_buf;
	PGAlignedBlock dst_buf;
	ssize_t		bytesRead;
	BlockNumber heapblk = 0;	/* next heap block to transform */
	BlockNumber new_vmblk = 0;	/* current destination VM page number */
	char	   *new_map;
	bool		dst_dirty = false;

	/*
	 * The backend's MAPSIZE / HEAPBLK_TO_* macros are private to
	 * visibilitymap.c; define the equivalents for both layouts here.  MAPSIZE
	 * is the map data area (a page minus its standard header).
	 */
#define MAPSIZE			(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define NEW_HEAPBLOCKS_PER_BYTE	(BITS_PER_BYTE / BITS_PER_HEAPBLOCK)
#define NEW_HEAPBLOCKS_PER_PAGE	(MAPSIZE * NEW_HEAPBLOCKS_PER_BYTE)
#define HEAPBLK_TO_MAPBLOCK(x)	((x) / NEW_HEAPBLOCKS_PER_PAGE)
#define HEAPBLK_TO_MAPBYTE(x)	(((x) % NEW_HEAPBLOCKS_PER_PAGE) / NEW_HEAPBLOCKS_PER_BYTE)
#define HEAPBLK_TO_OFFSET(x)	(((x) % NEW_HEAPBLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK)

	/* Old layout: 2 bits/block, 4 blocks/byte. */
#define OLD_BITS_PER_HEAPBLOCK	2
#define OLD_HEAPBLOCKS_PER_BYTE	(BITS_PER_BYTE / OLD_BITS_PER_HEAPBLOCK)
#define OLD_HEAPBLOCKS_PER_PAGE	(MAPSIZE * OLD_HEAPBLOCKS_PER_BYTE)

	StaticAssertDecl(BITS_PER_HEAPBLOCK == 4,
					 "rewriteVisibilityMap assumes a 4-bit new layout");

	if ((src_fd = open(fromfile, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not open file \"%s\": %m",
				 nspname, relname, fromfile);
	if ((dst_fd = open(tofile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
					   pg_file_create_mode)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not create file \"%s\": %m",
				 nspname, relname, tofile);

	/* Prime the first destination page from the first source page header. */
	memset(dst_buf.data, 0, BLCKSZ);
	new_map = dst_buf.data + MAXALIGN(SizeOfPageHeaderData);

	while ((bytesRead = read(src_fd, src_buf.data, BLCKSZ)) == BLCKSZ)
	{
		char	   *old_map = src_buf.data + MAXALIGN(SizeOfPageHeaderData);
		BlockNumber pagefirst = heapblk;
		BlockNumber blk;

		/* Copy this source page's header onto the first dest page we open. */
		if (!dst_dirty)
			memcpy(dst_buf.data, src_buf.data, MAXALIGN(SizeOfPageHeaderData));

		for (blk = pagefirst; blk < pagefirst + OLD_HEAPBLOCKS_PER_PAGE; blk++)
		{
			uint32		obyte = (blk % OLD_HEAPBLOCKS_PER_PAGE) / OLD_HEAPBLOCKS_PER_BYTE;
			uint8		ooff = (blk % OLD_HEAPBLOCKS_PER_BYTE) * OLD_BITS_PER_HEAPBLOCK;
			uint8		bits = (uint8) ((old_map[obyte] >> ooff) & 0x03);

			/* Flush and start a new dest page when this block rolls onto it. */
			if (HEAPBLK_TO_MAPBLOCK(blk) != new_vmblk)
			{
				if (new_cluster.controldata.data_checksum_version != 0)
					((PageHeader) dst_buf.data)->pd_checksum =
						pg_checksum_page(dst_buf.data, new_vmblk);
				if (write(dst_fd, dst_buf.data, BLCKSZ) != BLCKSZ)
				{
					if (errno == 0)
						errno = ENOSPC;
					pg_fatal("error while copying relation \"%s.%s\": could not write file \"%s\": %m",
							 nspname, relname, tofile);
				}
				new_vmblk = HEAPBLK_TO_MAPBLOCK(blk);
				memset(dst_buf.data, 0, BLCKSZ);
				memcpy(dst_buf.data, src_buf.data, MAXALIGN(SizeOfPageHeaderData));
				new_map = dst_buf.data + MAXALIGN(SizeOfPageHeaderData);
			}

			if (bits != 0)
			{
				uint32		nbyte = HEAPBLK_TO_MAPBYTE(blk);
				uint8		noff = HEAPBLK_TO_OFFSET(blk);

				new_map[nbyte] |= (uint8) (bits << noff);
			}
			dst_dirty = true;
		}

		heapblk = pagefirst + OLD_HEAPBLOCKS_PER_PAGE;
	}

	/* Flush the final destination page. */
	if (dst_dirty)
	{
		if (new_cluster.controldata.data_checksum_version != 0)
			((PageHeader) dst_buf.data)->pd_checksum =
				pg_checksum_page(dst_buf.data, new_vmblk);
		if (write(dst_fd, dst_buf.data, BLCKSZ) != BLCKSZ)
		{
			if (errno == 0)
				errno = ENOSPC;
			pg_fatal("error while copying relation \"%s.%s\": could not write file \"%s\": %m",
					 nspname, relname, tofile);
		}
	}

	if (bytesRead != 0)
		pg_fatal("error while copying relation \"%s.%s\": partial page found in file \"%s\"",
				 nspname, relname, fromfile);

	close(dst_fd);
	close(src_fd);

#undef OLD_BITS_PER_HEAPBLOCK
#undef OLD_HEAPBLOCKS_PER_BYTE
#undef OLD_HEAPBLOCKS_PER_PAGE
#undef MAPSIZE
#undef NEW_HEAPBLOCKS_PER_BYTE
#undef NEW_HEAPBLOCKS_PER_PAGE
#undef HEAPBLK_TO_MAPBLOCK
#undef HEAPBLK_TO_MAPBYTE
#undef HEAPBLK_TO_OFFSET
}
