/*-------------------------------------------------------------------------
 *
 * compress_gzip.c
 *	 Routines for archivers to read or write a gzip compressed data stream.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	   src/bin/pg_dump/compress_gzip.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"
#include <fcntl.h>
#include <unistd.h>

#include "compress_gzip.h"
#include "fe_utils/gzip.h"
#include "pg_backup_utils.h"

#ifdef HAVE_LIBZ
#include <zlib.h>

#define GZIP_BUFFER_SIZE 8192

/*----------------------
 * Compressor API
 *----------------------
 */
typedef struct GzipCompressorState
{
	z_streamp	zp;

	void	   *outbuf;
	size_t		outsize;
} GzipCompressorState;

/* Private routines that support gzip compressed data I/O */
static void DeflateCompressorInit(CompressorState *cs);
static void DeflateCompressorEnd(ArchiveHandle *AH, CompressorState *cs);
static void DeflateCompressorCommon(ArchiveHandle *AH, CompressorState *cs,
									bool flush);
static void EndCompressorGzip(ArchiveHandle *AH, CompressorState *cs);
static void WriteDataToArchiveGzip(ArchiveHandle *AH, CompressorState *cs,
								   const void *data, size_t dLen);
static void ReadDataFromArchiveGzip(ArchiveHandle *AH, CompressorState *cs);

static void
DeflateCompressorInit(CompressorState *cs)
{
	GzipCompressorState *gzipcs;
	z_streamp	zp;

	gzipcs = pg_malloc0_object(GzipCompressorState);
	zp = gzipcs->zp = pg_malloc_object(z_stream);
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	/*
	 * outsize is the buffer size we tell zlib it can output to.  We actually
	 * allocate one extra byte because some routines want to append a trailing
	 * zero byte to the zlib output.
	 */
	gzipcs->outsize = DEFAULT_IO_BUFFER_SIZE;
	gzipcs->outbuf = pg_malloc(gzipcs->outsize + 1);

	/* -Z 0 uses the "None" compressor -- not zlib with no compression */
	Assert(cs->compression_spec.level != 0);

	if (deflateInit(zp, cs->compression_spec.level) != Z_OK)
		pg_fatal("could not initialize compression library: %s", zp->msg);

	/* Just be paranoid - maybe End is called after Start, with no Write */
	zp->next_out = gzipcs->outbuf;
	zp->avail_out = gzipcs->outsize;

	/* Keep track of gzipcs */
	cs->private_data = gzipcs;
}

static void
DeflateCompressorEnd(ArchiveHandle *AH, CompressorState *cs)
{
	GzipCompressorState *gzipcs = (GzipCompressorState *) cs->private_data;
	z_streamp	zp;

	zp = gzipcs->zp;
	zp->next_in = NULL;
	zp->avail_in = 0;

	/* Flush any remaining data from zlib buffer */
	DeflateCompressorCommon(AH, cs, true);

	if (deflateEnd(zp) != Z_OK)
		pg_fatal("could not close compression stream: %s", zp->msg);

	pg_free(gzipcs->outbuf);
	pg_free(gzipcs->zp);
	pg_free(gzipcs);
	cs->private_data = NULL;
}

static void
DeflateCompressorCommon(ArchiveHandle *AH, CompressorState *cs, bool flush)
{
	GzipCompressorState *gzipcs = (GzipCompressorState *) cs->private_data;
	z_streamp	zp = gzipcs->zp;
	void	   *out = gzipcs->outbuf;
	int			res = Z_OK;

	while (gzipcs->zp->avail_in != 0 || flush)
	{
		res = deflate(zp, flush ? Z_FINISH : Z_NO_FLUSH);
		if (res == Z_STREAM_ERROR)
			pg_fatal("could not compress data: %s", zp->msg);
		if ((flush && (zp->avail_out < gzipcs->outsize))
			|| (zp->avail_out == 0)
			|| (zp->avail_in != 0)
			)
		{
			/*
			 * Extra paranoia: avoid zero-length chunks, since a zero length
			 * chunk is the EOF marker in the custom format. This should never
			 * happen but ...
			 */
			if (zp->avail_out < gzipcs->outsize)
			{
				/*
				 * Any write function should do its own error checking but to
				 * make sure we do a check here as well ...
				 */
				size_t		len = gzipcs->outsize - zp->avail_out;

				cs->writeF(AH, out, len);
			}
			zp->next_out = out;
			zp->avail_out = gzipcs->outsize;
		}

		if (res == Z_STREAM_END)
			break;
	}
}

static void
EndCompressorGzip(ArchiveHandle *AH, CompressorState *cs)
{
	/* If deflation was initialized, finalize it */
	if (cs->private_data)
		DeflateCompressorEnd(AH, cs);
}

static void
WriteDataToArchiveGzip(ArchiveHandle *AH, CompressorState *cs,
					   const void *data, size_t dLen)
{
	GzipCompressorState *gzipcs = (GzipCompressorState *) cs->private_data;

	gzipcs->zp->next_in = data;
	gzipcs->zp->avail_in = dLen;
	DeflateCompressorCommon(AH, cs, false);
}

static void
ReadDataFromArchiveGzip(ArchiveHandle *AH, CompressorState *cs)
{
	z_streamp	zp;
	char	   *out;
	int			res = Z_OK;
	size_t		cnt;
	char	   *buf;
	size_t		buflen;

	zp = pg_malloc_object(z_stream);
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	buflen = DEFAULT_IO_BUFFER_SIZE;
	buf = pg_malloc(buflen);

	out = pg_malloc(DEFAULT_IO_BUFFER_SIZE + 1);

	if (inflateInit(zp) != Z_OK)
		pg_fatal("could not initialize compression library: %s",
				 zp->msg);

	/* no minimal chunk size for zlib */
	while ((cnt = cs->readF(AH, &buf, &buflen)))
	{
		zp->next_in = (void *) buf;
		zp->avail_in = cnt;

		while (zp->avail_in > 0)
		{
			zp->next_out = (void *) out;
			zp->avail_out = DEFAULT_IO_BUFFER_SIZE;

			res = inflate(zp, 0);
			if (res != Z_OK && res != Z_STREAM_END)
				pg_fatal("could not uncompress data: %s",
						 zp->msg ? zp->msg : "unknown error");

			out[DEFAULT_IO_BUFFER_SIZE - zp->avail_out] = '\0';
			ahwrite(out, 1, DEFAULT_IO_BUFFER_SIZE - zp->avail_out, AH);
		}
	}

	zp->next_in = NULL;
	zp->avail_in = 0;
	while (res != Z_STREAM_END)
	{
		zp->next_out = (void *) out;
		zp->avail_out = DEFAULT_IO_BUFFER_SIZE;
		res = inflate(zp, 0);
		if (res != Z_OK && res != Z_STREAM_END)
			pg_fatal("could not uncompress data: %s",
					 zp->msg ? zp->msg : "unknown error");

		out[DEFAULT_IO_BUFFER_SIZE - zp->avail_out] = '\0';
		ahwrite(out, 1, DEFAULT_IO_BUFFER_SIZE - zp->avail_out, AH);
	}

	if (inflateEnd(zp) != Z_OK)
		pg_fatal("could not close compression library: %s", zp->msg);

	pg_free(buf);
	pg_free(out);
	pg_free(zp);
}

/* Public routines that support gzip compressed data I/O */
void
InitCompressorGzip(CompressorState *cs,
				   const pg_compress_specification compression_spec)
{
	cs->readData = ReadDataFromArchiveGzip;
	cs->writeData = WriteDataToArchiveGzip;
	cs->end = EndCompressorGzip;

	cs->compression_spec = compression_spec;

	/*
	 * If the caller has defined a write function, prepare the necessary
	 * state.  Note that if the data is empty, End may be called immediately
	 * after Init, without ever calling Write.
	 */
	if (cs->writeF)
		DeflateCompressorInit(cs);
}


/*----------------------
 * Compress File API
 *----------------------
 */
typedef struct GzipFileState
{
	bool		reading;
	union
	{
		gzFile		gzfp;
		struct
		{
			int			fd;
			GzipDecompressor decoder;
			char	   *input;
			size_t		input_len;
			size_t		input_pos;
			char	   *output;
			size_t		output_len;
			size_t		output_pos;
			bool		eof;
			bool		done;
		}			read;
	};
} GzipFileState;

/*
 * Use strict gzip parsing: empty physical files, plain input, and trailing
 * non-member bytes are errors.  Valid empty and concatenated members work.
 */
static size_t
Gzip_read(void *ptr, size_t size, CompressFileHandle *CFH)
{
	GzipFileState *state = (GzipFileState *) CFH->private_data;
	size_t		copied = 0;

	while (copied < size)
	{
		size_t		bytes;

		if (state->read.output_pos == state->read.output_len)
		{
			/*
			 * No buffered output remains; refill it unless decoding is
			 * complete.
			 */
			size_t		consumed;
			GzipDecompressResult result;

			if (state->read.done)
				break;

			if (state->read.input_pos == state->read.input_len && !state->read.eof)
			{
				/* The input buffer is empty and EOF has not been reached */
				ssize_t		readlen;

				do
				{
					readlen = read(state->read.fd, state->read.input, GZIP_BUFFER_SIZE);
					/* retry if a signal interrupts the read */
				} while (readlen < 0 && errno == EINTR);

				if (readlen < 0)
					pg_fatal("could not read from input file: %m");
				state->read.input_pos = 0;
				state->read.input_len = readlen;
				state->read.eof = readlen == 0;
			}

			/* Produce more decompressed output */
			state->read.output_pos = 0;
			result = gzip_decompressor_read(&state->read.decoder,
											state->read.input + state->read.input_pos,
											state->read.input_len - state->read.input_pos,
											state->read.output, DEFAULT_IO_BUFFER_SIZE,
											state->read.eof, &consumed,
											&state->read.output_len);
			state->read.input_pos += consumed;
			if (result == GZIP_DECOMPRESS_ERROR)
				pg_fatal("could not decompress data: %s", state->read.decoder.error);
			state->read.done = result == GZIP_DECOMPRESS_DONE;

			/* Headers and empty members may produce no output. */
			if (state->read.output_len == 0)
				continue;
		}

		bytes = Min(size - copied, state->read.output_len - state->read.output_pos);
		memcpy((char *) ptr + copied, state->read.output + state->read.output_pos, bytes);
		state->read.output_pos += bytes;
		copied += bytes;
	}
	return copied;
}

static void
Gzip_write(const void *ptr, size_t size, CompressFileHandle *CFH)
{
	GzipFileState *state = (GzipFileState *) CFH->private_data;
	gzFile		gzfp = state->gzfp;
	int			errnum;
	const char *errmsg;

	if (gzwrite(gzfp, ptr, size) != size)
	{
		errmsg = gzerror(gzfp, &errnum);
		pg_fatal("could not write to file: %s",
				 errnum == Z_ERRNO ? strerror(errno) : errmsg);
	}
}

static int
Gzip_getc(CompressFileHandle *CFH)
{
	unsigned char ret;

	if (Gzip_read(&ret, 1, CFH) != 1)
		pg_fatal("could not read from input file: end of file");

	return ret;
}

static char *
Gzip_gets(char *ptr, int size, CompressFileHandle *CFH)
{
	int			i;

	Assert(size > 0);
	for (i = 0; i < size - 1; i++)
	{
		if (Gzip_read(&ptr[i], 1, CFH) != 1)
			break;
		if (ptr[i] == '\n')
		{
			i++;
			break;
		}
	}
	ptr[i] = '\0';
	return i > 0 ? ptr : NULL;
}

static bool
Gzip_close(CompressFileHandle *CFH)
{
	GzipFileState *state = (GzipFileState *) CFH->private_data;
	bool		ok = true;

	if (state->reading)
	{
		char		buf[GZIP_BUFFER_SIZE];

		/* Consume and validate any input the caller did not read. */
		while (Gzip_read(buf, sizeof(buf), CFH) > 0)
			;
		if (!gzip_decompressor_end(&state->read.decoder))
			pg_fatal("could not decompress data: %s", state->read.decoder.error);
		pg_free(state->read.input);
		pg_free(state->read.output);
		if (close(state->read.fd) != 0)
			ok = false;
	}
	else if (gzclose(state->gzfp) != Z_OK)
		ok = false;
	CFH->private_data = NULL;
	pg_free(state);
	return ok;
}

static bool
Gzip_eof(CompressFileHandle *CFH)
{
	GzipFileState *state = (GzipFileState *) CFH->private_data;

	return state->reading ?
		(state->read.done && state->read.output_pos == state->read.output_len) :
		gzeof(state->gzfp) == 1;
}

static const char *
Gzip_get_error(CompressFileHandle *CFH)
{
	GzipFileState *state = (GzipFileState *) CFH->private_data;
	const char *errmsg;
	int			errnum;

	if (state->reading)
		return state->read.decoder.error ? state->read.decoder.error : "";

	errmsg = gzerror(state->gzfp, &errnum);
	if (errnum == Z_ERRNO)
		errmsg = strerror(errno);

	return errmsg;
}

static bool
Gzip_open(const char *path, int fd, const char *mode, CompressFileHandle *CFH)
{
	GzipFileState *state;
	gzFile		gzfp = NULL;
	char		mode_compression[32];

	if (CFH->compression_spec.level != Z_DEFAULT_COMPRESSION)
	{
		/*
		 * user has specified a compression level, so tell zlib to use it
		 */
		snprintf(mode_compression, sizeof(mode_compression), "%s%d",
				 mode, CFH->compression_spec.level);
	}
	else
		strcpy(mode_compression, mode);

	state = pg_malloc0_object(GzipFileState);
	state->reading = mode[0] == 'r';
	if (state->reading)
	{
		state->read.fd = fd >= 0 ? dup(fd) : open(path, O_RDONLY | PG_BINARY, 0);
		if (state->read.fd < 0)
		{
			pg_free(state);
			return false;
		}
		if (!gzip_decompressor_init(&state->read.decoder))
			pg_fatal("could not initialize compression library: %s",
					 state->read.decoder.error);
		state->read.input = pg_malloc(GZIP_BUFFER_SIZE);
		state->read.output = pg_malloc(DEFAULT_IO_BUFFER_SIZE);
	}
	else if (fd >= 0)
	{
		int			dup_fd = dup(fd);

		if (dup_fd < 0)
		{
			pg_free(state);
			return false;
		}
		gzfp = gzdopen(dup_fd, mode_compression);
		if (gzfp == NULL)
		{
			close(dup_fd);
			pg_free(state);
			return false;
		}
	}
	else
	{
		gzfp = gzopen(path, mode_compression);
		if (gzfp == NULL)
		{
			pg_free(state);
			return false;
		}
	}

	if (!state->reading)
		state->gzfp = gzfp;

	CFH->private_data = state;

	return true;
}

static bool
Gzip_open_write(const char *path, const char *mode, CompressFileHandle *CFH)
{
	char	   *fname;
	bool		ret;
	int			save_errno;

	fname = psprintf("%s.gz", path);
	ret = CFH->open_func(fname, -1, mode, CFH);

	save_errno = errno;
	pfree(fname);
	errno = save_errno;

	return ret;
}

void
InitCompressFileHandleGzip(CompressFileHandle *CFH,
						   const pg_compress_specification compression_spec)
{
	CFH->open_func = Gzip_open;
	CFH->open_write_func = Gzip_open_write;
	CFH->read_func = Gzip_read;
	CFH->write_func = Gzip_write;
	CFH->gets_func = Gzip_gets;
	CFH->getc_func = Gzip_getc;
	CFH->close_func = Gzip_close;
	CFH->eof_func = Gzip_eof;
	CFH->get_error_func = Gzip_get_error;

	CFH->compression_spec = compression_spec;

	CFH->private_data = NULL;
}
#else							/* HAVE_LIBZ */
void
InitCompressorGzip(CompressorState *cs,
				   const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "gzip");
}

void
InitCompressFileHandleGzip(CompressFileHandle *CFH,
						   const pg_compress_specification compression_spec)
{
	pg_fatal("this build does not support compression with %s", "gzip");
}
#endif							/* HAVE_LIBZ */
