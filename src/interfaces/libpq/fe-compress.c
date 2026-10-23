/*-------------------------------------------------------------------------
 *
 * fe-compress.c
 *    Compression support for the frontend/backend protocol.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/interfaces/libpq/fe-compress.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <limits.h>

#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/protocol_compression.h"
#include "port/pg_bswap.h"

#define PQ_COMPRESSION_BUFFER_KEEP_SIZE (1024 * 1024)

void
pqCompressionReset(PGconn *conn)
{
#ifdef USE_ZSTD
	if (conn->compression_dctx != NULL)
	{
		ZSTD_freeDCtx((ZSTD_DCtx *) conn->compression_dctx);
		conn->compression_dctx = NULL;
	}
	if (conn->compression_cctx != NULL)
	{
		ZSTD_freeCCtx((ZSTD_CCtx *) conn->compression_cctx);
		conn->compression_cctx = NULL;
	}
#endif
	if (conn->compression_buffers_initialized)
	{
		termPQExpBuffer(&conn->compression_output_buffer);
		conn->compression_buffers_initialized = false;
	}
	free(conn->decompressBuffer.buffer);
	memset(&conn->decompressBuffer, 0, sizeof(msg_buffer));
	conn->compression_in_frame = false;
	conn->compression_frame_ended = false;
	conn->compression_ready = false;
	conn->compression_copy_started = false;
	conn->compression_rejected = false;
}

/*
 * Choose between network input and decoded messages. Only a complete inner
 * message is handed to the existing parser; an incomplete one stays here
 * while the next wrapper is read. Never copy decoded messages into inBuffer.
 */
msg_buffer *
pqGetMessageBuffer(PGconn *conn)
{
	msg_buffer *buf = &conn->decompressBuffer;
	int			available = buf->end - buf->start;

	if (available >= 5)
	{
		uint32		len;

		memcpy(&len, buf->buffer + buf->start + 1, 4);
		len = pg_ntoh32(len);
		if ((buf->buffer[buf->start] != PqMsg_DataRow &&
			 buf->buffer[buf->start] != PqMsg_CopyData) ||
			len < 4 || len >= INT_MAX)
			goto invalid;
		if ((size_t) available >= (size_t) len + 1)
			return buf;
	}
	if (available && conn->compression_frame_ended)
		goto invalid;

	buf = &conn->inBuffer;
	if (buf->end - buf->start >= 5)
	{
		char		id = buf->buffer[buf->start];
		uint32		len;

		memcpy(&len, buf->buffer + buf->start + 1, 4);
		len = pg_ntoh32(len);
		if (id == PqMsg_CompressedData)
		{
			if (!conn->compression_ready || len <= 4)
				goto invalid;
#ifdef USE_ZSTD
			if (len - 4 > PQ_COMPRESSION_MAX_WRAPPER_SIZE)
				goto invalid;
#endif
		}
		else if (available ||
				 (id == PqMsg_ReadyForQuery && conn->compression_in_frame))
			goto invalid;
	}
	return buf;

invalid:
	libpq_append_conn_error(conn, "invalid compressed protocol message");
	return NULL;
}

void
pqCompressionReady(PGconn *conn)
{
#ifdef USE_ZSTD
	char	   *data;

	if (conn->compression_copy_started)
	{
		ZSTD_freeCCtx((ZSTD_CCtx *) conn->compression_cctx);
		conn->compression_cctx = NULL;
		conn->compression_copy_started = false;
	}
	conn->compression_frame_ended = false;
	if (conn->compression && !conn->compression_rejected &&
		strcmp(conn->compression, "off") != 0)
		conn->compression_ready = true;

	/* No decoded row remains live at this boundary. */
	Assert(conn->decompressBuffer.start == conn->decompressBuffer.end);
	conn->decompressBuffer.start = conn->decompressBuffer.cursor =
		conn->decompressBuffer.end = 0;
	if (conn->decompressBuffer.bufSize > PQ_COMPRESSION_BUFFER_KEEP_SIZE)
	{
		data = realloc(conn->decompressBuffer.buffer, 8192);
		if (data)
		{
			conn->decompressBuffer.buffer = data;
			conn->decompressBuffer.bufSize = 8192;
		}
	}
	if (conn->compression_buffers_initialized &&
		conn->compression_output_buffer.maxlen > PQ_COMPRESSION_BUFFER_KEEP_SIZE)
	{
		data = realloc(conn->compression_output_buffer.data, INITIAL_EXPBUFFER_SIZE);
		if (data)
		{
			conn->compression_output_buffer.data = data;
			conn->compression_output_buffer.maxlen = INITIAL_EXPBUFFER_SIZE;
			resetPQExpBuffer(&conn->compression_output_buffer);
		}
	}
#endif
}

#ifdef USE_ZSTD
static int
pqCompressionInitBuffers(PGconn *conn)
{
	if (conn->compression_buffers_initialized)
		return 0;

	initPQExpBuffer(&conn->compression_output_buffer);
	if (PQExpBufferBroken(&conn->compression_output_buffer))
	{
		termPQExpBuffer(&conn->compression_output_buffer);
		libpq_append_conn_error(conn, "out of memory");
		return 1;
	}

	conn->compression_buffers_initialized = true;
	return 0;
}

#endif

/*
 * Decode one complete outer message into the reusable msg_buffer. The caller
 * consumes/traces the wrapper, then parses its ordinary messages separately.
 */
int
pqReadCompressedMessage(PGconn *conn, int msgLength)
{
#ifdef USE_ZSTD
	ZSTD_DCtx  *dctx;
	ZSTD_inBuffer input;
	msg_buffer *buf = &conn->decompressBuffer;
	size_t		segment_size = 0;
	size_t		result = 1;
	int			position;

	if (!conn->compression_ready || conn->compression_frame_ended ||
		msgLength <= 0 || msgLength > PQ_COMPRESSION_MAX_WRAPPER_SIZE)
		goto invalid;

	if (conn->compression_dctx == NULL)
	{
		size_t		rc;

		dctx = ZSTD_createDCtx();
		if (dctx == NULL)
		{
			libpq_append_conn_error(conn, "out of memory");
			return -2;
		}
		rc = ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, PQ_COMPRESSION_WINDOW_LOG);
		if (ZSTD_isError(rc))
		{
			ZSTD_freeDCtx(dctx);
			goto invalid;
		}
		conn->compression_dctx = dctx;
	}
	dctx = (ZSTD_DCtx *) conn->compression_dctx;
	input.src = conn->inBuffer.buffer + conn->inBuffer.cursor;
	input.size = msgLength;
	input.pos = 0;

	for (;;)
	{
		ZSTD_outBuffer output;
		size_t		before = input.pos;
		size_t		room = PQ_COMPRESSION_MAX_SEGMENT_SIZE - segment_size;

		room = room == 0 ? 1 : Min(room, (size_t) 8192);
		if (pqCheckMsgBufferSpace((size_t) buf->end + room, buf, conn))
			return -2;
		output.dst = buf->buffer + buf->end;
		output.size = room;
		output.pos = 0;
		result = ZSTD_decompressStream(dctx, &output, &input);
		if (ZSTD_isError(result))
			goto invalid;
		buf->end += output.pos;
		segment_size += output.pos;
		if (segment_size > PQ_COMPRESSION_MAX_SEGMENT_SIZE)
			goto invalid;
		if (result == 0)
		{
			if (input.pos != input.size)
				goto invalid;
			break;
		}
		if (input.pos == input.size && output.pos < output.size)
			break;
		if (input.pos == before && output.pos == 0)
			goto invalid;
	}
	conn->compression_in_frame = (result != 0);
	conn->compression_frame_ended = (result == 0);

	/* Validate the decoded prefix, including an unfinished message header. */
	position = buf->start;
	while (buf->end - position >= 5)
	{
		uint32		len;

		if (buf->buffer[position] != PqMsg_DataRow &&
			buf->buffer[position] != PqMsg_CopyData)
			goto invalid;
		memcpy(&len, buf->buffer + position + 1, 4);
		len = pg_ntoh32(len);
		if (len < 4 || len >= INT_MAX)
			goto invalid;
		if (len > buf->end - position - 1)
			break;
		position += len + 1;
	}
	if (result == 0 && position != buf->end)
		goto invalid;
	conn->inBuffer.cursor += msgLength;
	return 0;

invalid:
	libpq_append_conn_error(conn, "invalid compressed protocol message");
#else
	libpq_append_conn_error(conn, "client does not support compression with zstd");
#endif
	return -2;
}

#ifdef USE_ZSTD
static int
pqPutCompressedCopySegment(PGconn *conn, const char *buffer, int nbytes,
						   ZSTD_EndDirective directive)
{
	ZSTD_CCtx  *cctx = (ZSTD_CCtx *) conn->compression_cctx;
	char		header[5];
	bool		context_advanced = false;
	size_t		source_size = nbytes > 0 ? (size_t) nbytes + 5 : 0;
	size_t		source_pos = 0;
	size_t		result = 0;
	uint32		n32;

	if (cctx == NULL)
	{
		if (pqCompressionInitBuffers(conn))
			return EOF;
		cctx = ZSTD_createCCtx();
		if (cctx == NULL)
			goto oom;
		result = ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, PQ_COMPRESSION_WINDOW_LOG);
		if (ZSTD_isError(result))
		{
			ZSTD_freeCCtx(cctx);
			cctx = NULL;
			goto zstd_error;
		}
		conn->compression_cctx = cctx;
	}

	if (source_size > 0)
	{
		header[0] = PqMsg_CopyData;
		n32 = pg_hton32((uint32) nbytes + 4);
		memcpy(header + 1, &n32, 4);
	}
	do
	{
		ZSTD_EndDirective segment_directive;
		ZSTD_outBuffer output;
		size_t		segment_size;
		size_t		segment_end;
		size_t		compressed_size;

		segment_size = Min(source_size - source_pos,
						   (size_t) PQ_COMPRESSION_MAX_SEGMENT_SIZE);
		segment_end = source_pos + segment_size;
		segment_directive = (segment_end == source_size) ?
			directive : ZSTD_e_flush;
		compressed_size = ZSTD_compressBound(segment_size);
		/* Prefer sending pending data to enlarging the output buffer. */
		if ((size_t) conn->outCount + 5 + compressed_size > conn->outBufSize &&
			pqFlush(conn) < 0)
			goto fail;
		if (compressed_size > PQ_COMPRESSION_MAX_WRAPPER_SIZE ||
			compressed_size > INT_MAX - conn->outCount - 5 ||
			pqCheckOutBufferSpace(conn->outCount + 5 + compressed_size, conn))
			goto fail;

		resetPQExpBuffer(&conn->compression_output_buffer);
		if (!enlargePQExpBuffer(&conn->compression_output_buffer,
								compressed_size))
			goto oom;
		output.dst = conn->compression_output_buffer.data;
		output.size = compressed_size;
		output.pos = 0;

		while (source_pos < segment_end)
		{
			ZSTD_inBuffer input;
			size_t		part_end;

			if (source_pos < sizeof(header))
			{
				part_end = Min(segment_end, sizeof(header));
				input.src = header + source_pos;
				input.size = part_end - source_pos;
			}
			else
			{
				part_end = segment_end;
				input.src = buffer + source_pos - sizeof(header);
				input.size = part_end - source_pos;
			}
			input.pos = 0;
			while (input.pos < input.size)
			{
				context_advanced = true;
				result = ZSTD_compressStream2(cctx, &output, &input,
											  ZSTD_e_continue);
				if (ZSTD_isError(result))
					goto zstd_error;
				if (output.pos == output.size && input.pos < input.size)
					goto output_too_small;
			}
			source_pos = part_end;
		}

		{
			ZSTD_inBuffer input = {NULL, 0, 0};

			do
			{
				context_advanced = true;
				result = ZSTD_compressStream2(cctx, &output, &input,
											  segment_directive);
				if (ZSTD_isError(result))
					goto zstd_error;
				if (output.pos == output.size && result != 0)
					goto output_too_small;
			} while (result != 0);
		}

		if (pqPutMsgStart(PqMsg_CompressedData, conn) < 0 ||
			pqPutnchar(conn->compression_output_buffer.data, output.pos, conn) < 0 ||
			pqPutMsgEnd(conn) < 0)
			goto fail;
	} while (source_pos < source_size);

	return 0;

oom:
	libpq_append_conn_error(conn, "out of memory");
	goto fail;

zstd_error:
	libpq_append_conn_error(conn, "Zstandard compression failed: %s",
							ZSTD_getErrorName(result));
	goto fail;

output_too_small:
	libpq_append_conn_error(conn, "Zstandard compression output buffer is too small");
fail:
	if (context_advanced)
		conn->status = CONNECTION_BAD;
	return EOF;
}

int
pqPutCompressedCopyData(PGconn *conn, const char *buffer, int nbytes)
{
	if (pqPutCompressedCopySegment(conn, buffer, nbytes, ZSTD_e_flush) < 0)
		return EOF;
	conn->compression_copy_started = true;
	return 0;
}

int
pqEndCompressedCopyData(PGconn *conn)
{
	if (!conn->compression_copy_started)
		return 0;
	if (pqPutCompressedCopySegment(conn, NULL, 0, ZSTD_e_end) < 0)
		return EOF;
	conn->compression_copy_started = false;
	return 0;
}

#endif
