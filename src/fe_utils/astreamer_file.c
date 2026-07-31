/*-------------------------------------------------------------------------
 *
 * astreamer_file.c
 *
 * Archive streamers that write to files. astreamer_plain_writer writes
 * the whole archive to a single file, and astreamer_extractor writes
 * each archive member to a separate file in a given directory.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/fe_utils/astreamer_file.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <fcntl.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/astreamer.h"

typedef struct astreamer_plain_writer
{
	astreamer	base;
	char	   *pathname;
	FILE	   *file;
	bool		should_close_file;
} astreamer_plain_writer;

typedef struct astreamer_extractor
{
	astreamer	base;
	char	   *basepath;
	const char *(*link_map) (const char *);
	void		(*report_output_file) (const char *);
	char		filename[MAXPGPATH];
	int			fd;
	bool		discard_backup;
} astreamer_extractor;

static void astreamer_plain_writer_content(astreamer *streamer,
										   astreamer_member *member,
										   const char *data, int len,
										   astreamer_archive_context context);
static void astreamer_plain_writer_finalize(astreamer *streamer);
static void astreamer_plain_writer_free(astreamer *streamer);

static const astreamer_ops astreamer_plain_writer_ops = {
	.content = astreamer_plain_writer_content,
	.finalize = astreamer_plain_writer_finalize,
	.free = astreamer_plain_writer_free
};

static void astreamer_extractor_content(astreamer *streamer,
										astreamer_member *member,
										const char *data, int len,
										astreamer_archive_context context);
static void astreamer_extractor_finalize(astreamer *streamer);
static void astreamer_extractor_free(astreamer *streamer);
static void extract_directory(const char *filename, mode_t mode);
static void extract_link(const char *filename, const char *linktarget);
static int	create_file_for_extract(const char *filename, mode_t mode,
									bool discard_backup);
static void write_file_range(int fd, const char *filename,
							 const char *data, int len);

static const astreamer_ops astreamer_extractor_ops = {
	.content = astreamer_extractor_content,
	.finalize = astreamer_extractor_finalize,
	.free = astreamer_extractor_free
};

/*
 * Create a astreamer that just writes data to a file.
 *
 * The caller must specify a pathname and may specify a file. The pathname is
 * used for error-reporting purposes either way. If file is NULL, the pathname
 * also identifies the file to which the data should be written: it is opened
 * for writing and closed when done. If file is not NULL, the data is written
 * there.
 */
astreamer *
astreamer_plain_writer_new(char *pathname, FILE *file)
{
	astreamer_plain_writer *streamer;

	streamer = palloc0_object(astreamer_plain_writer);
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_plain_writer_ops;

	streamer->pathname = pstrdup(pathname);
	streamer->file = file;

	if (file == NULL)
	{
		streamer->file = fopen(pathname, "wb");
		if (streamer->file == NULL)
			pg_fatal("could not create file \"%s\": %m", pathname);
		streamer->should_close_file = true;
	}

	return &streamer->base;
}

/*
 * Write archive content to file.
 */
static void
astreamer_plain_writer_content(astreamer *streamer,
							   astreamer_member *member, const char *data,
							   int len, astreamer_archive_context context)
{
	astreamer_plain_writer *mystreamer;

	mystreamer = (astreamer_plain_writer *) streamer;

	if (len == 0)
		return;

	write_file_range(fileno(mystreamer->file), mystreamer->pathname, data, len);
}

/*
 * End-of-archive processing when writing to a plain file consists of closing
 * the file if we opened it, but not if the caller provided it.
 */
static void
astreamer_plain_writer_finalize(astreamer *streamer)
{
	astreamer_plain_writer *mystreamer;

	mystreamer = (astreamer_plain_writer *) streamer;

	if (mystreamer->should_close_file && fclose(mystreamer->file) != 0)
		pg_fatal("could not close file \"%s\": %m",
				 mystreamer->pathname);

	mystreamer->file = NULL;
	mystreamer->should_close_file = false;
}

/*
 * Free memory associated with this astreamer.
 */
static void
astreamer_plain_writer_free(astreamer *streamer)
{
	astreamer_plain_writer *mystreamer;

	mystreamer = (astreamer_plain_writer *) streamer;

	Assert(!mystreamer->should_close_file);
	Assert(mystreamer->base.bbs_next == NULL);

	pfree(mystreamer->pathname);
	pfree(mystreamer);
}

/*
 * Create a astreamer that extracts an archive.
 *
 * All pathnames in the archive are interpreted relative to basepath.
 *
 * Unlike e.g. astreamer_plain_writer_new() we can't do anything useful here
 * with untyped chunks; we need typed chunks which follow the rules described
 * in astreamer.h. Assuming we have that, we don't need to worry about the
 * original archive format; it's enough to just look at the member information
 * provided and write to the corresponding file.
 *
 * 'link_map' is a function that will be applied to the target of any
 * symbolic link, and which should return a replacement pathname to be used
 * in its place.  If NULL, the symbolic link target is used without
 * modification.
 *
 * 'report_output_file' is a function that will be called each time we open a
 * new output file. The pathname to that file is passed as an argument. If
 * NULL, the call is skipped.
 *
 * If 'discard_backup' is true, the extracted archive is thrown away rather
 * than written to the filesystem.
 */
astreamer *
astreamer_extractor_new(const char *basepath,
						const char *(*link_map) (const char *),
						void (*report_output_file) (const char *),
						bool discard_backup)
{
	astreamer_extractor *streamer;

	streamer = palloc0_object(astreamer_extractor);
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_extractor_ops;
	streamer->basepath = pstrdup(basepath);
	streamer->link_map = link_map;
	streamer->report_output_file = report_output_file;
	streamer->discard_backup = discard_backup;
	streamer->fd = -1;

	return &streamer->base;
}

/*
 * Extract archive contents to the filesystem.
 */
static void
astreamer_extractor_content(astreamer *streamer, astreamer_member *member,
							const char *data, int len,
							astreamer_archive_context context)
{
	astreamer_extractor *mystreamer = (astreamer_extractor *) streamer;
	int			fnamelen;

	Assert(member != NULL || context == ASTREAMER_ARCHIVE_TRAILER);
	Assert(context != ASTREAMER_UNKNOWN);

	switch (context)
	{
		case ASTREAMER_MEMBER_HEADER:
			Assert(mystreamer->fd == -1);

			if (!path_is_safe_for_extraction(member->pathname))
				pg_fatal("tar member has unsafe path name: \"%s\"",
						 member->pathname);

			/* Prepend basepath. */
			snprintf(mystreamer->filename, sizeof(mystreamer->filename),
					 "%s/%s", mystreamer->basepath, member->pathname);

			/* Remove any trailing slash. */
			fnamelen = strlen(mystreamer->filename);
			if (mystreamer->filename[fnamelen - 1] == '/')
				mystreamer->filename[fnamelen - 1] = '\0';

			/*
			 * Dispatch based on file type.
			 */
			if (member->is_regular)
				mystreamer->fd =
					create_file_for_extract(mystreamer->filename,
											member->mode,
											mystreamer->discard_backup);
			else if (member->is_directory)
			{
				if (!mystreamer->discard_backup)
					extract_directory(mystreamer->filename, member->mode);
			}
			else if (member->is_symlink)
			{
				const char *linktarget = member->linktarget;

				if (mystreamer->link_map)
					linktarget = mystreamer->link_map(linktarget);

				if (!is_absolute_path(linktarget) &&
					!path_is_safe_for_extraction(member->linktarget))
				{
					pg_fatal("link target has unsafe path name: \"%s\"",
							 member->linktarget);
				}

				if (!mystreamer->discard_backup)
					extract_link(mystreamer->filename, linktarget);
			}

			/* Report output file change. */
			if (mystreamer->report_output_file)
				mystreamer->report_output_file(mystreamer->filename);
			break;

		case ASTREAMER_MEMBER_CONTENTS:
			if (mystreamer->fd == -1)
				break;

			if (len > 0)
				write_file_range(mystreamer->fd, mystreamer->filename,
								 data, len);
			break;

		case ASTREAMER_MEMBER_TRAILER:
			if (mystreamer->fd == -1)
				break;
			if (close(mystreamer->fd) != 0)
				pg_fatal("could not close file \"%s\": %m",
						 mystreamer->filename);
			mystreamer->fd = -1;
			break;

		case ASTREAMER_ARCHIVE_TRAILER:
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while extracting archive");
	}
}

/*
 * Should we tolerate an already-existing directory?
 *
 * When streaming WAL, pg_wal (or pg_xlog for pre-9.6 clusters) will have been
 * created by the wal receiver process. Also, when the WAL directory location
 * was specified, pg_wal (or pg_xlog) has already been created as a symbolic
 * link before starting the actual backup.  So just ignore creation failures
 * on related directories.
 *
 * If in-place tablespaces are used, pg_tblspc and subdirectories may already
 * exist when we get here. So tolerate that case, too.
 */
static bool
should_allow_existing_directory(const char *pathname)
{
	const char *filename = last_dir_separator(pathname) + 1;

	if (strcmp(filename, "pg_wal") == 0 ||
		strcmp(filename, "pg_xlog") == 0 ||
		strcmp(filename, "archive_status") == 0 ||
		strcmp(filename, "summaries") == 0 ||
		strcmp(filename, "pg_tblspc") == 0)
		return true;

	if (strspn(filename, "0123456789") == strlen(filename))
	{
		const char *pg_tblspc = strstr(pathname, "/pg_tblspc/");

		return pg_tblspc != NULL && pg_tblspc + 11 == filename;
	}

	return false;
}

/*
 * Create a directory.
 */
static void
extract_directory(const char *filename, mode_t mode)
{
	if (mkdir(filename, pg_dir_create_mode) != 0 &&
		(errno != EEXIST || !should_allow_existing_directory(filename)))
		pg_fatal("could not create directory \"%s\": %m",
				 filename);

#ifndef WIN32
	if (chmod(filename, mode))
		pg_fatal("could not set permissions on directory \"%s\": %m",
				 filename);
#endif
}

/*
 * Create a symbolic link.
 *
 * It's most likely a link in pg_tblspc directory, to the location of a
 * tablespace. Apply any tablespace mapping given on the command line
 * (--tablespace-mapping). (We blindly apply the mapping without checking that
 * the link really is inside pg_tblspc. We don't expect there to be other
 * symlinks in a data directory, but if there are, you can call it an
 * undocumented feature that you can map them too.)
 */
static void
extract_link(const char *filename, const char *linktarget)
{
	if (symlink(linktarget, filename) != 0)
		pg_fatal("could not create symbolic link from \"%s\" to \"%s\": %m",
				 filename, linktarget);
}

/*
 * Create a regular file.
 *
 * Return an open file descriptor so we can write the content to the file.
 *
 * We intentionally use a raw file descriptor to get unbuffered write()
 * and avoid potentiall libc interference.
 */
static int
create_file_for_extract(const char *filename, mode_t mode,
						bool discard_backup)
{
	int			fd;

	if (discard_backup)
	{
		fd = open(DEVNULL, O_WRONLY | PG_BINARY, 0);
		if (fd < 0)
			pg_fatal("could not open file \"%s\": %m", DEVNULL);
		return fd;
	}

	fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY,
			  pg_file_create_mode);
	if (fd < 0)
		pg_fatal("could not create file \"%s\": %m", filename);

#ifndef WIN32
	if (chmod(filename, mode))
		pg_fatal("could not set permissions on file \"%s\": %m",
				 filename);
#endif

	return fd;
}

/*
 * Wrapper for safely writing chunk of archive. Single write() is not
 * guaranteed to consume the whole, so we loop until all is on the disk.
 */
static void
write_file_range(int fd, const char *filename, const char *data, int len)
{
	while (len > 0)
	{
		ssize_t		written;

		errno = 0;
		written = write(fd, data, len);
		if (written <= 0)
		{
			if (errno == 0)
				errno = ENOSPC;
			pg_fatal("could not write to file \"%s\": %m", filename);
		}

		data += written;
		len -= written;
	}
}

/*
 * End-of-stream processing for extracting an archive.
 *
 * There's nothing to do here but sanity checking.
 */
static void
astreamer_extractor_finalize(astreamer *streamer)
{
	astreamer_extractor *mystreamer PG_USED_FOR_ASSERTS_ONLY
	= (astreamer_extractor *) streamer;

	Assert(mystreamer->fd == -1);
}

/*
 * Free memory.
 */
static void
astreamer_extractor_free(astreamer *streamer)
{
	astreamer_extractor *mystreamer = (astreamer_extractor *) streamer;

	pfree(mystreamer->basepath);
	pfree(mystreamer);
}
