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
#ifdef USE_LIBURING
#include <liburing.h>
#endif

#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/astreamer.h"

#ifdef USE_LIBURING
/*
 * Parameters for the io_uring + O_DIRECT write path. Aim is to keep deep
 * queue of the I/O devices populated by throwing large number of independent
 * synchronous/DIRECT_IO requests, but in asynchronous way. We avoid latency
 * of single synchronous write, and utilize the device up to the max what it
 * allows.
 */
#define DIO_ALIGN		4096			/* O_DIRECT aligment */
#define DIO_BUFSZ		(1024 * 1024)	/* size of each direct I/O write */
#define DIO_NBUF		32				/* queue depth, XXX:expose it via getopt? */
#define DIO_MIN_SIZE	DIO_BUFSZ		/* fsize threshold for activating O_DIRECT writes */
#define DIO_SUBMIT_BATCH	8			/* how many SQEs to batch */

/*
 * State for the dio/io_uring writer. Used by plain(file) and tar extractors.
 * We buffer data as we recieve it, into the "pool" of buffers and submit them
 * using DIO_BUFSZ sizes.
 */
typedef struct dio_writer
{
	struct io_uring ring;       /* see io_uring(7) */
	bool		ring_ready;		/* was the ring and buffer initialized */
	char	   *pool;			/* buffer memory, max size: DIO_NBUF * DIO_BUFSZ */
	bool		busy[DIO_NBUF]; /* is buffer in flight? */
	size_t		buflen[DIO_NBUF];	/* bytes submitted for in-flight buffer */
	int			fd;				/* DIO fd: if fd > 0 then it's active, -1 otherwise */
	const char *filename;		/* for error handling */
	pgoff_t		offset;			/* offset of the next write */
	pgoff_t		written;		/* bytes written */
	int			curidx;			/* buffer being filled, or -1 */
	int			curlen;			/* bytes filled in current buffer */
	int			inflight;		/* prepared writes not yet reaped */
	int			unsubmitted;	/* SQEs prepared but not yet submitted */
	bool		notified;		/* already logged a fall-back-to-buffered? */
} dio_writer;
#endif

typedef struct astreamer_plain_writer
{
	astreamer	base;
	char	   *pathname;
	FILE	   *file;			/* if NULL, then dio.fd is used */
	bool		should_close_file;
#ifdef USE_LIBURING
	dio_writer	dio;
#endif
} astreamer_plain_writer;

typedef struct astreamer_extractor
{
	astreamer	base;
	char	   *basepath;
	const char *(*link_map) (const char *);
	void		(*report_output_file) (const char *);
	char		filename[MAXPGPATH];
	int			fd;				/* if -1, then dio.fd is used */
	bool		discard_backup;
	bool		verbose;
#ifdef USE_LIBURING
	dio_writer	dio;
#endif
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
									bool discard_backup, pgoff_t size);
static void write_file_range(int fd, const char *filename,
							 const char *data, int len);
#ifdef USE_LIBURING
static bool dio_writer_start(dio_writer *dw, const char *filename,
							 pgoff_t prealloc_size, bool verbose);
static void dio_writer_write(dio_writer *dw, const char *data, int len);
static void dio_writer_finish(dio_writer *dw);
static void dio_writer_destroy(dio_writer *dw);
#endif

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
astreamer_plain_writer_new(char *pathname, FILE *file, bool verbose)
{
	astreamer_plain_writer *streamer;

	streamer = palloc0_object(astreamer_plain_writer);
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_plain_writer_ops;

	streamer->pathname = pstrdup(pathname);
	streamer->file = file;
#ifdef USE_LIBURING
	streamer->dio.fd = -1;
#endif

	if (file == NULL)
	{
#ifdef USE_LIBURING

		/*
		 * Fallback to classic buffered writes in case of:
		 * - env variable PG_BASEBACKUP_NODIO is set (debugging)
		 * - pipe/stdout is used
		 * - /dev/null is being used (-t client-blackhole)
		 * - filesystems without O_DIRECT support
		 */
		if (getenv("PG_BASEBACKUP_NODIO") == NULL &&
			dio_writer_start(&streamer->dio, streamer->pathname, 0, verbose) == true) {
			/* do not use buffered writes, as the dio.fd is going to be used */
			streamer->file = NULL;
		} else
#endif
		{
			streamer->file = fopen(pathname, "wb");
			if (streamer->file == NULL)
				pg_fatal("could not create file \"%s\": %m", pathname);
			streamer->should_close_file = true;
		}
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

#ifdef USE_LIBURING
	if (mystreamer->dio.fd != -1)
	{
		dio_writer_write(&mystreamer->dio, data, len);
		return;
	}
#endif
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

#ifdef USE_LIBURING
	if (mystreamer->dio.fd != -1)
	{
		dio_writer_finish(&mystreamer->dio);
		return;
	}
#endif
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
#ifdef USE_LIBURING
	dio_writer_destroy(&mystreamer->dio);
#endif
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
						bool discard_backup, bool verbose)
{
	astreamer_extractor *streamer;

	streamer = palloc0_object(astreamer_extractor);
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_extractor_ops;
	streamer->basepath = pstrdup(basepath);
	streamer->link_map = link_map;
	streamer->report_output_file = report_output_file;
	streamer->discard_backup = discard_backup;
	streamer->verbose = verbose;
	streamer->fd = -1;
#ifdef USE_LIBURING
	streamer->dio.fd = -1;
#endif

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
#ifdef USE_LIBURING
			Assert(mystreamer->dio.fd == -1);
#endif

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
			{
#ifdef USE_LIBURING
				/*
				 * Fallback to classic buffered writes in case of:
				 * - small files
				 * - env variable PG_BASEBACKUP_NODIO is set (debugging)
				 * - pipe/stdout is used
				 * - /dev/null is being used (-t client-blackhole)
				 * - filesystems without O_DIRECT support
				 */
				if (member->size >= DIO_MIN_SIZE &&
					!mystreamer->discard_backup &&
					getenv("PG_BASEBACKUP_NODIO") == NULL &&
					dio_writer_start(&mystreamer->dio, mystreamer->filename,
									 member->size, mystreamer->verbose)) {
					/* do not use buffered writes, as the dio.fd is going to be used */
					mystreamer->fd = -1;
				} else
#endif
					mystreamer->fd =
						create_file_for_extract(mystreamer->filename,
												member->mode,
												mystreamer->discard_backup,
												member->size);
			}
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
#ifdef USE_LIBURING
			if (mystreamer->dio.fd != -1)
			{
				if (len > 0)
					dio_writer_write(&mystreamer->dio, data, len);
				break;
			}
#endif
			if (mystreamer->fd == -1)
				break;

			if (len > 0)
				write_file_range(mystreamer->fd, mystreamer->filename,
								 data, len);
			break;

		case ASTREAMER_MEMBER_TRAILER:
#ifdef USE_LIBURING
			if (mystreamer->dio.fd != -1)
			{
				dio_writer_finish(&mystreamer->dio);
				break;
			}
#endif
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
						bool discard_backup, pgoff_t size)
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

	/*
	 * Preallocate the file to its final size.  We know the size up front
	 * from the tar member header, so this lets the filesystem allocate all
	 * the blocks in one go rather than growing the file on every write.
	 */
#ifdef HAVE_POSIX_FALLOCATE
	if (size > 0)
	{
		int			rc = posix_fallocate(fd, 0, size);

		/*
		 * This is just an optimization, so we ignore failures such as
		 * EINVAL/EOPNOTSUPP, however we need to properly fail on ENOSPC.
		 */
		if (rc == ENOSPC)
		{
			errno = rc;
			pg_fatal("could not preallocate file \"%s\": %m", filename);
		}
	}
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
#ifdef USE_LIBURING
	Assert(mystreamer->dio.fd == -1);
#endif
}

/*
 * Free memory.
 */
static void
astreamer_extractor_free(astreamer *streamer)
{
	astreamer_extractor *mystreamer = (astreamer_extractor *) streamer;

	pfree(mystreamer->basepath);
#ifdef USE_LIBURING
	dio_writer_destroy(&mystreamer->dio);
#endif
	pfree(mystreamer);
}

#ifdef USE_LIBURING
/*
 * IO_uring/liburing uses concept of two rings (submissions and completion).
 * See io_uring_queue_init(3) and io_uring(7) for more information and
 * especially https://github.com/axboe/liburing/blob/master/examples/io_uring-cp.c
 * for nice example on how to use it.
 */

/* Take (consume) one completion event from the ring */
static void
dio_wait_one(dio_writer *dw)
{
	int			idx;
	int			ret;
	struct io_uring_cqe *cqe;

	ret = io_uring_wait_cqe(&dw->ring, &cqe);
	if (ret < 0)
	{
		errno = -ret;
		pg_fatal("could not wait for io_uring_wait_cqe(): %m");
	}

	idx = (int) io_uring_cqe_get_data64(cqe);
	if (cqe->res < 0)
	{
		errno = -cqe->res;
		pg_fatal("could not write to file \"%s\": %m", dw->filename);
	}

	if ((size_t) cqe->res != dw->buflen[idx])
	{
		/* short write? */
		errno = ENOSPC;
		pg_fatal("could not write to file \"%s\": %m", dw->filename);
	}

	io_uring_cqe_seen(&dw->ring, cqe);
	dw->busy[idx] = false;
	dw->inflight--;
}

/*
 * Common routine for flushing (submitting) earlier prepared, but not yet
 * submitted SQEs.
 */
static void
dio_flush_sq(dio_writer *dw)
{
	int			ret;

	if (dw->unsubmitted == 0)
		return;

	ret = io_uring_submit(&dw->ring);
	if (ret < 0)
	{
		errno = -ret;
		pg_fatal("could not submit io_uring (io_uring_enter(2) failure?): %m");
	}

	/* Everything was submitted */
	dw->unsubmitted = 0;
}

/* Get free buffer index. If none are available, wait for some to finish */
static int
dio_get_free_buf(dio_writer *dw)
{
	for (;;)
	{
		for (int i = 0; i < DIO_NBUF; i++)
		{
			if (dw->busy[i] == false)
			{
				/*
				 * Mark it busy till we finish the write (technically we need CQE for
				 * this buffer to arrive, and then can mark it as non-busy again).
				 * */
				dw->busy[i] = true;
				return i;
			}
		}
		/* All buffers are busy, so we wait for writes to finish */
		dio_flush_sq(dw);
		dio_wait_one(dw);
	}
}

/*
 * Prepare the SQE from the buffer with full O_DIRECT write. We really
 * submit the SQEs to the kernel only (flush them) only once every
 * couple of times to avoid constant syscall tax (AKA io_uring batching).
 *
 * buffer's idx needs to have it's lenth aligned to DIO_ALIGN (O_DIRECT
 * requirement).
 */
static void
dio_submit(dio_writer *dw, int idx, size_t len)
{
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&dw->ring);
	/* this should never happen? liburing/examples uses abort/asserts for this */
	if (sqe == NULL)
		pg_fatal("io_uring submission queue full: %m");

	io_uring_prep_write(sqe, dw->fd,
						dw->pool + (size_t) idx * DIO_BUFSZ,
						len, dw->offset);
	io_uring_sqe_set_data64(sqe, idx);

	dw->buflen[idx] = len;
	dw->offset += len;
	dw->inflight++;
	dw->unsubmitted++;

	if (dw->unsubmitted >= DIO_SUBMIT_BATCH)
		dio_flush_sq(dw);
}

/*
 * Start writing to a file through the DIO+AIO path. Returns true if that is
 * supported (dw->fd is set).
 *
 * If prealloc_size is known it can be used to preallocate file which helps
 * greatly to avoid slow writes with O_DIRECT on some filesystems.
 */
static bool
dio_writer_start(dio_writer *dw, const char *filename, pgoff_t prealloc_size, bool verbose)
{
	int			fd;
	int			ret;
	struct stat st;

	fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | PG_BINARY,
			  pg_file_create_mode);
	if (fd < 0)
	{
		if (verbose && !dw->notified)
		{
			pg_log_info("could not open \"%s\" with O_DIRECT, using buffered I/O instead: %m",
						filename);
			dw->notified = true;
		}
		return false;
	}

	/*
	 * Only regular files can be written with O_DIRECT. However, the /dev/null
	 * device (used with -t client-blackhole mode) and other special files can
	 * be still opened with O_DIRECT, but must go through the buffered path(?).
	 * Ensure we opened regular file before setting up the io_uring, so we
	 * don't allocate the buffer pool just to fall back.
	 */
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
	{
		if (verbose && !dw->notified)
		{
			pg_log_info("\"%s\" is not a regular file, using buffered I/O instead",
						filename);
			dw->notified = true;
		}
		close(fd);
		return false;
	}

	/* Lazy setup of the io_uring */
	if (!dw->ring_ready)
	{
		ret = io_uring_queue_init(DIO_NBUF, &dw->ring, 0);
		if (ret != 0)
		{
			errno = -ret;
			/* XXX: shouldn't we warn about it non-verbose mode? */
			if (verbose && !dw->notified)
			{
				pg_log_info("could not set up io_uring, using buffered I/O instead: %m");
				dw->notified = true;
			}
			close(fd);
			return false;
		}

		/* O_DIRECT writes require memory aligment */
		if (posix_memalign((void **) &dw->pool, DIO_ALIGN,
						   (size_t) DIO_NBUF * DIO_BUFSZ) != 0)
			pg_fatal("unable to allocate aligned memory for direct I/O writes");

		dw->ring_ready = true;
		if (verbose)
			pg_log_info("using O_DIRECT with io_uring for large file writes");
	}

	/* If size is known (in plain mode), preallocate the space */
#ifdef HAVE_POSIX_FALLOCATE
	if (prealloc_size > 0)
	{
		int			rc = posix_fallocate(fd, 0, prealloc_size);

		if (rc == ENOSPC)
		{
			errno = rc;
			pg_fatal("could not preallocate file \"%s\": %m", filename);
		}
	}
#endif

	dw->fd = fd;
	dw->filename = filename;
	dw->offset = 0;
	dw->written = 0;
	dw->curidx = -1;
	dw->curlen = 0;
	return true;
}

/*
 * Handle buffering on DIO side (in dio->pool buffers) before submiting
 * full (big) writes as required by O_DIRECT.
 */
static void
dio_writer_write(dio_writer *dw, const char *data, int len)
{
	dw->written += len;

	while (len > 0)
	{
		char	   *buf;
		int			space;
		int			n;

		if (dw->curidx < 0)
		{
			dw->curidx = dio_get_free_buf(dw);
			dw->curlen = 0;
		}

		buf = dw->pool + (size_t) dw->curidx * DIO_BUFSZ;
		space = DIO_BUFSZ - dw->curlen;
		n = Min(space, len);
		/* append to the pool buffer */
		memcpy(buf + dw->curlen, data, n);
		dw->curlen += n;
		data += n;
		len -= n;

		if (dw->curlen == DIO_BUFSZ)
		{
			dio_submit(dw, dw->curidx, DIO_BUFSZ);
			dw->curidx = -1;
		}
	}
}

/* Send/wait for remaining in-flight writes, truncate and close the DIO */
static void
dio_writer_finish(dio_writer *dw)
{
	/* Flush partial buffer */
	if (dw->curidx >= 0 && dw->curlen > 0)
	{
		char	   *buf = dw->pool + (size_t) dw->curidx * DIO_BUFSZ;
		/* We need to pad stuff */
		size_t		padded = TYPEALIGN(DIO_ALIGN, dw->curlen);

		memset(buf + dw->curlen, 0, padded - dw->curlen);
		dio_submit(dw, dw->curidx, padded);
		dw->curidx = -1;
	}

	/* Submit any batched writes and wait for them all to finish */
	dio_flush_sq(dw);
	while (dw->inflight > 0)
		dio_wait_one(dw);

	/* Truncate the file to the right size (we could pre-allocate too much) */
	if (ftruncate(dw->fd, dw->written) != 0)
		pg_fatal("could not truncate file \"%s\": %m", dw->filename);

	if (close(dw->fd) != 0)
		pg_fatal("could not close file \"%s\": %m", dw->filename);
	dw->fd = -1;
}

/* Free the buffers and the io_uring */
static void
dio_writer_destroy(dio_writer *dw)
{
	if (dw->ring_ready)
	{
		io_uring_queue_exit(&dw->ring);
		free(dw->pool);
		dw->ring_ready = false;
		dw->fd = -1;
	}
}
#endif /* USE_LIBURING */
