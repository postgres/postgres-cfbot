/*-------------------------------------------------------------------------
 *
 * fe-compress-lz4.c
 *	  Lz4 decompression support for frontend/backend protocol
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-compress-lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <string.h>

#include "libpq-fe.h"
#include "libpq-int.h"

#ifndef USE_LZ4

int
pqInitDecompressorLz4(PGconn *conn)
{
	libpq_append_conn_error(conn, "client does not support compression with lz4");
	return EOF;
}

#else

#include <lz4frame.h>

#define LZ4_CHUNK_SZ	64 * 1024	/* 64kB as maximum chunk size read */

static void free_context_lz4(PGconn *conn);
static int	decompress_payload_lz4(PGconn *conn);

static const pqDecompressor pqDecompressorLz4 = {
	.decompress_payload = decompress_payload_lz4,
	.free_context = free_context_lz4,
};

static void
free_context_lz4(PGconn *conn)
{
	LZ4F_decompressionContext_t dctx;

	Assert(conn->compress_state);
	dctx = (LZ4F_decompressionContext_t) conn->compress_state;
	LZ4F_freeDecompressionContext(dctx);
	conn->compress_state = NULL;
}

/*
 * Initialize lz4 decompression context
 *
 * returns 0 if OK, EOF if trouble
 */
int
pqInitDecompressorLz4(PGconn *conn)
{
	LZ4F_decompressionContext_t dctx;
	LZ4F_errorCode_t ctxError;

	ctxError = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
	if (LZ4F_isError(ctxError))
	{
		libpq_append_conn_error(conn, "out of memory");
		return EOF;
	}

	conn->compress_state = dctx;
	conn->decompressor = pqDecompressorLz4;
	conn->decompress_chunk_size = LZ4_CHUNK_SZ;
	return 0;
}

/*
 * Decompress the compressed payload using lz4. The result will be
 * stored in conn->decompressBuffer.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
decompress_payload_lz4(PGconn *conn)
{
	size_t		res;
	msg_buffer *msgBuf = &conn->inBuffer;
	msg_buffer *outBuf = &conn->decompressBuffer;
	LZ4F_decompressionContext_t dctx = (LZ4F_decompressionContext_t) conn->compress_state;

	do
	{
		void	   *srcBuffer = msgBuf->buffer + msgBuf->start + conn->compress_cursor;
		void	   *dstBuffer = outBuf->buffer + outBuf->end;
		size_t		consumed = conn->compress_end - conn->compress_cursor;
		size_t		decompressed = outBuf->bufSize - outBuf->end;

		Assert(msgBuf->start + conn->compress_end <= msgBuf->bufSize);
		res = LZ4F_decompress(dctx, dstBuffer, &decompressed,
							  srcBuffer, &consumed, NULL);

		if (LZ4F_isError(res))
		{
			libpq_append_conn_error(conn, "could not decompress data: %s",
									LZ4F_getErrorName(res));
			return EOF;
		}

		/* Update msgBuf with the consumed bytes */
		Assert(msgBuf->start + conn->compress_end + consumed <= msgBuf->bufSize);
		conn->compress_cursor += consumed;

		/* And update decompressBuffer with the decompressed bytes */
		Assert(outBuf->end + decompressed <= outBuf->bufSize);
		outBuf->end += decompressed;
		if (outBuf->end == outBuf->bufSize)
			/* Output buffer is full, leave the message unconsumed */
			return 0;
	} while (conn->compress_cursor < conn->compress_end);
	return 0;
}

#endif
