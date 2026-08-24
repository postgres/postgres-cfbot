/*-------------------------------------------------------------------------
 *
 * fe-compress-zstd.c
 *	  Zstd decompression support for frontend/backend protocol
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-compress-zstd.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "libpq-fe.h"
#include "libpq-int.h"

#ifndef USE_ZSTD

int
pqInitDecompressorZstd(PGconn *conn)
{
	libpq_append_conn_error(conn, "client does not support compression with zstd");
	return EOF;
}

#else

#include <zstd.h>
#include <string.h>

static void free_context_zstd(PGconn *conn);
static int	decompress_payload_zstd(PGconn *conn);

static const pqDecompressor pqDecompressorZstd = {
	.decompress_payload = decompress_payload_zstd,
	.free_context = free_context_zstd,
};

static void
free_context_zstd(PGconn *conn)
{
	ZSTD_DStream *dctx;

	Assert(conn->compress_state);
	dctx = (ZSTD_DStream *) conn->compress_state;
	ZSTD_freeDStream(dctx);
	conn->compress_state = NULL;
}

/*
 * Initialize zstd decompression context
 *
 * returns 0 if OK, EOF if trouble
 */
int
pqInitDecompressorZstd(PGconn *conn)
{
	ZSTD_DStream *dctx;

	dctx = ZSTD_createDStream();

	if (dctx == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return EOF;
	}
	conn->compress_state = dctx;
	conn->decompressor = pqDecompressorZstd;
	conn->decompress_chunk_size = ZSTD_DStreamOutSize();
	return 0;
}

/*
 * Decompress the compressed payload using zstd. The result will be
 * stored in conn->decompressBuffer.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
decompress_payload_zstd(PGconn *conn)
{
	size_t		res;
	ZSTD_inBuffer inBuf = {conn->inBuffer.buffer + conn->inBuffer.start, conn->compress_end, conn->compress_cursor};
	ZSTD_outBuffer outBuf;
	ZSTD_DStream *dctx = (ZSTD_DStream *) conn->compress_state;

	outBuf.dst = conn->decompressBuffer.buffer;
	outBuf.size = conn->decompressBuffer.bufSize;
	outBuf.pos = conn->decompressBuffer.end;

	/*
	 * It's possible to have inBuf.pos == inBuf.size and outBuf.pos ==
	 * outBuf.size after a ZSTD_decompressStream due to how frames are closed.
	 * zstd normally keeps the last byte "hostage" when the last block of a
	 * frame is decompressed, forcing (inBuf.pos < inBuf.size) to be true.
	 *
	 * However, as we leave the frame opened, this never happens, and we can
	 * reach a point where the block is completely consumed while the outBuf
	 * is full.
	 *
	 * Thus the use of the do while as we want to call ZSTD_decompressStream
	 * even if inBuf was completely processed, to force zstd to flush any
	 * leftover buffers.
	 */
	do
	{
		res = ZSTD_decompressStream(dctx, &outBuf, &inBuf);

		if (ZSTD_isError(res))
		{
			libpq_append_conn_error(conn, "could not decompress data: %s",
									ZSTD_getErrorName(res));
			return EOF;
		}

		if (outBuf.pos == outBuf.size)

			/*
			 * output buffer is full, break to let the outer loop either
			 * consume the messages or enlarge the buffer
			 */
			break;
	} while (inBuf.pos < inBuf.size);

	conn->decompressBuffer.end = outBuf.pos;
	conn->compress_cursor = inBuf.pos;
	return 0;
}

#endif
