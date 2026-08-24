/*-------------------------------------------------------------------------
 *
 * fe-compress.c
 *	  Decompression support for frontend/backend protocol
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-compress.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <string.h>

#include "common/compression.h"
#include "libpq-fe.h"
#include "libpq-int.h"

static int	pqGetMsgLen(PGconn *conn, msg_buffer * msgBuf);
static bool pqHasFullMessage(PGconn *conn, msg_buffer * msgBuf);

/*
 * pqReadCompressedMessage: Read the content of a CompressedMessages message.
 *
 * returns EOF if not enough data, -2 if trouble, 0 if CompressedMessages
 * content has been fully processed, 1 if CompressedMessages has been
 * successfully decompressed
 */
int
pqReadCompressedMessage(PGconn *conn, int msgLength)
{
	char		compress_algorithm;
	int			endCompressMessage = conn->inBuffer.start + msgLength + 5;

	if (conn->compress_cursor > 0
		&& conn->compress_cursor == msgLength + 5)
	{
		/*
		 * The CompressedMessages was fully processed. Advance inBuffer so the
		 * message can be tagged as consumed.
		 */
		conn->inBuffer.cursor = endCompressMessage;
		conn->compress_cursor = 0;
		return 0;
	}

	/* Get compression algorithm */
	if (pqGetc(&compress_algorithm, conn, &conn->inBuffer) == EOF)
		return EOF;

	if (compress_algorithm != PG_COMPRESSION_ZSTD &&
		compress_algorithm != PG_COMPRESSION_LZ4)
	{
		libpq_append_conn_error(
								conn, "invalid compression algorithm in CompressedMessage: %d",
								compress_algorithm);
		return 0;
	}

	/* get the message types */
	if (pqGets(&conn->workBuffer, conn, &conn->inBuffer) == EOF)
		return EOF;

	if (conn->compress_algorithm != compress_algorithm)
	{
		/*
		 * Compression in the message is different from our current
		 * compression context, free the previous context to start from a
		 * clean slate
		 */
		if (conn->decompressor.free_context != NULL)
			conn->decompressor.free_context(conn);
		switch (compress_algorithm)
		{
			case PG_COMPRESSION_LZ4:
				if (pqInitDecompressorLz4(conn))
					return -2;
				break;
			case PG_COMPRESSION_ZSTD:
				if (pqInitDecompressorZstd(conn))
					return -2;
				break;
			case PG_COMPRESSION_GZIP:
			case PG_COMPRESSION_NONE:
				pg_unreachable();
		}
		conn->compress_algorithm = compress_algorithm;
	}

	conn->compress_cursor = conn->inBuffer.cursor - conn->inBuffer.start;
	conn->compress_end = endCompressMessage - conn->inBuffer.start;
	return 1;
}

/*
 * pqDecompressPayload: Decompress into the decompress buffer until:
 * - A full message is available for processing.
 * Or
 * - All input bytes are processed and the decompress buffer isn't full.
 *
 * Due to keeping the compression frame opened, it's possible to have
 * cursor==end while the compressor still has bytes to output, but couldn't due
 * to decompressBuffer being full. We rely on decompressBuffer being full as a
 * condition to run one more pq_decompress_payload to make sure there's nothing
 * stuck in the decompressor's buffers.
 *
 * returns EOF if not enough data, -2 if trouble, 0 if there's at least one
 * full message available
 */
int
pqDecompressPayload(PGconn *conn)
{
	bool		outBufferFull;

	if (pqHasFullMessage(conn, &conn->decompressBuffer))
	{
		/* There's already a full message in decompress buffer */
		return 0;
	}

	outBufferFull = conn->decompressBuffer.end == conn->decompressBuffer.bufSize;
	while (conn->compress_cursor < conn->compress_end || outBufferFull)
	{
		if (outBufferFull)
		{
			int			msgLen = pqGetMsgLen(conn, &conn->decompressBuffer);
			size_t		bytes_needed;

			if (msgLen == -1)
			{
				/*
				 * Not enough bytes for the msg length. Either the last
				 * message is in the last 5 bytes of decompressBuffer, or
				 * everything was processed. Just ask for 4 bytes, and
				 * pqCheckMsgBufferSpace will clean the already processed
				 * messages.
				 */
				msgLen = 4;
			}
			/* bytes_needed needs to include the starting 1 byte id. */
			bytes_needed = Max(msgLen + 1, conn->decompress_chunk_size);

			/*
			 * decompressBuffer is full, and we don't have a full message,
			 * call pqCheckMsgBufferSpace to either left justify the content
			 * or enlarge the buffer. Once space is freed, decompress needs to
			 * be called again to give the decompressor a chance to flush its
			 * internal buffers.
			 */
			if (pqCheckMsgBufferSpace(bytes_needed + conn->decompressBuffer.start, &conn->decompressBuffer, conn))
			{
				return -2;
			}
		}

		/*
		 * We should always have available bytes before calling
		 * decompress_payload
		 */
		Assert(conn->decompressBuffer.bufSize > conn->decompressBuffer.end);

		/* Do the decompression */
		if (conn->decompressor.decompress_payload(conn))

			/*
			 * there was a fatal error while decompressing the payload, bail
			 * out
			 */
			return -2;

		if (pqHasFullMessage(conn, &conn->decompressBuffer))

			/*
			 * We have at least one full message, exit to let the outer loop
			 * consume it
			 */
			return 0;

		outBufferFull = conn->decompressBuffer.end == conn->decompressBuffer.bufSize;
	}
	return EOF;
}

/*
 * Returns the length of the next message in the msgBuf
 *
 * Returns EOF if there's not enough bytes to read the length
 */
static int
pqGetMsgLen(PGconn *conn, msg_buffer * msgBuf)
{
	char		id;
	int			msgLength;

	msgBuf->cursor = msgBuf->start;
	if (pqGetc(&id, conn, msgBuf))
		return EOF;
	if (pqGetInt(&msgLength, 4, conn, msgBuf))
		return EOF;

	return msgLength;
}

/*
 * Returns true if there's a full message in the msgBuf
 */
static bool
pqHasFullMessage(PGconn *conn, msg_buffer * msgBuf)
{
	int			len = pqGetMsgLen(conn, msgBuf);
	int			available = msgBuf->end - msgBuf->start;

	if (len < 0)
		return false;

	/* Available bytes needs to include the 1 byte id */
	return available >= 1 + len;
}
