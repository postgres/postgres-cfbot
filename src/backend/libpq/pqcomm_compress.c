/*-------------------------------------------------------------------------
 *
 * pqcomm_compress.c
 *    Compression layer for backend protocol I/O.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/libpq/pqcomm_compress.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/libpq.h"
#include "libpq/protocol.h"
#include "libpq/protocol_compression.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "utils/memutils.h"

int			protocol_compression = PROTOCOL_COMPRESSION_OFF;

#ifdef USE_ZSTD
/* Internal batching policy, independent of the negotiated size limits. */
#define PQ_COMPRESSION_INPUT_SIZE (32 * 1024)
#define PQ_COMPRESSION_BUFFER_KEEP_SIZE (1024 * 1024)

static const PQcommMethods *PrevPQcommMethods;
static bool PqCompressBusy;
static bool PqCompressionNegotiated;
static bool PqCompressionStarted;
static bool PqCompressionActive;
static bool PqCompressionFrameStarted;
static size_t PqCompressionSmallBytes;
static StringInfoData PqCompressionInput;
static StringInfoData PqCompressionOutput;
static StringInfoData PqDecompressionInput;
static StringInfoData PqDecompressionBuffer;
static bool PqDecompressionBufferInitialized;
static bool PqDecompressionFrameStarted;
static bool PqDecompressionFrameEnded;
static ZSTD_CCtx *PqCompressionContext;
static ZSTD_DCtx *PqDecompressionContext;

static void pq_compress_comm_reset(void);
static int	pq_compress_flush(void);
static int	pq_compress_flush_if_writable(void);
static bool pq_compress_is_send_pending(void);
static int	pq_compress_putmessage(char msgtype, const char *s, size_t len);
static void pq_compress_putmessage_noblock(char msgtype, const char *s, size_t len);
static int	pq_compress_putmessage_internal(char msgtype, const char *s,
											size_t len, bool block);
static int	pq_compress_flush_buffer(ZSTD_EndDirective directive, bool block);
static bool compression_buffer_init(StringInfo buf);
static bool compression_buffer_enlarge(StringInfo buf, size_t size);
static void compression_buffer_release(StringInfo buf);
static void pq_decompress_reset(void);

static const PQcommMethods PqCommCompressMethods = {
	.comm_reset = pq_compress_comm_reset,
	.flush = pq_compress_flush,
	.flush_if_writable = pq_compress_flush_if_writable,
	.is_send_pending = pq_compress_is_send_pending,
	.putmessage = pq_compress_putmessage,
	.putmessage_noblock = pq_compress_putmessage_noblock
};

void
pq_enable_protocol_compression(void)
{
	Assert(!PqCompressionNegotiated);
	PqCompressionNegotiated = true;
	PrevPQcommMethods = PqCommMethods;
	PqCommMethods = &PqCommCompressMethods;
}

static bool
compression_buffer_init(StringInfo buf)
{
	buf->data = MemoryContextAllocExtended(TopMemoryContext,
										   STRINGINFO_DEFAULT_SIZE,
										   MCXT_ALLOC_NO_OOM);
	if (buf->data == NULL)
		return false;
	buf->len = 0;
	buf->maxlen = STRINGINFO_DEFAULT_SIZE;
	buf->cursor = 0;
	buf->data[0] = '\0';
	return true;
}

static bool
compression_buffer_enlarge(StringInfo buf, size_t size)
{
	size_t		needed;
	size_t		newlen;
	char	   *data;

	if (size >= (size_t) MaxAllocSize - buf->len)
		return false;
	needed = (size_t) buf->len + size + 1;
	if (needed <= (size_t) buf->maxlen)
		return true;

	newlen = 2 * (size_t) buf->maxlen;
	while (needed > newlen)
		newlen = 2 * newlen;
	if (!AllocSizeIsValid(newlen))
		newlen = needed;

	data = repalloc_extended(buf->data, newlen, MCXT_ALLOC_NO_OOM);
	if (data == NULL)
		return false;
	buf->data = data;
	buf->maxlen = (int) newlen;
	return true;
}

static void
compression_buffer_release(StringInfo buf)
{
	char	   *data;

	if (buf->maxlen <= PQ_COMPRESSION_BUFFER_KEEP_SIZE)
		return;
	Assert(buf->len == 0);
	data = repalloc_extended(buf->data, STRINGINFO_DEFAULT_SIZE,
							 MCXT_ALLOC_NO_OOM);
	if (data == NULL)
		return;
	buf->data = data;
	buf->maxlen = STRINGINFO_DEFAULT_SIZE;
	buf->cursor = 0;
	buf->data[0] = '\0';
}

static void
pq_decompress_reset(void)
{
	if (PqDecompressionContext != NULL)
	{
		size_t		result;

		result = ZSTD_DCtx_reset(PqDecompressionContext,
								 ZSTD_reset_session_only);
		if (ZSTD_isError(result))
		{
			ZSTD_freeDCtx(PqDecompressionContext);
			PqDecompressionContext = NULL;
		}
	}
	if (PqDecompressionBufferInitialized)
	{
		resetStringInfo(&PqDecompressionInput);
		resetStringInfo(&PqDecompressionBuffer);
	}
	PqDecompressionFrameStarted = false;
	PqDecompressionFrameEnded = false;
}

void
pq_check_protocol_compression_message(int msgtype)
{
	if (msgtype == PqMsg_CompressedData)
	{
		if (!PqCompressionStarted)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received compressed data before protocol initialization completed")));
		if (PqDecompressionFrameEnded)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received data after the compressed protocol stream ended")));
		PqDecompressionFrameStarted = true;
	}
	else if (msgtype == PqMsg_CopyDone || msgtype == PqMsg_CopyFail)
	{
		if (PqDecompressionFrameStarted && !PqDecompressionFrameEnded)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("compressed protocol stream was not terminated before COPY ended")));
		pq_decompress_reset();
		compression_buffer_release(&PqDecompressionInput);
		compression_buffer_release(&PqDecompressionBuffer);
	}
}

int
pq_get_compressed_message(StringInfo s)
{
	size_t		result = 1;

	if (!PqCompressionNegotiated)
		ereport(FATAL,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("received compressed data without negotiated compression")));

	if (!PqDecompressionBufferInitialized)
	{
		if (!compression_buffer_init(&PqDecompressionInput) ||
			!compression_buffer_init(&PqDecompressionBuffer))
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed while decompressing protocol data.")));
		PqDecompressionBufferInitialized = true;
	}

	if (PqDecompressionContext == NULL)
	{
		size_t		rc;

		PqDecompressionContext = ZSTD_createDCtx();
		if (PqDecompressionContext == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed while creating Zstandard decompression context.")));
		rc = ZSTD_DCtx_setParameter(PqDecompressionContext,
									ZSTD_d_windowLogMax, PQ_COMPRESSION_WINDOW_LOG);
		if (ZSTD_isError(rc))
			ereport(FATAL,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not configure Zstandard decompression context: %s",
							ZSTD_getErrorName(rc))));
	}

	resetStringInfo(&PqDecompressionBuffer);
	for (;;)
	{
		ZSTD_inBuffer input;
		size_t		segment_size = 0;

		if (pq_getmessage(&PqDecompressionInput,
						  PQ_COMPRESSION_MAX_WRAPPER_SIZE + 4))
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid compressed protocol message")));
		if (PqDecompressionInput.len == 0)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("compressed protocol message is empty")));

		input.src = PqDecompressionInput.data;
		input.size = PqDecompressionInput.len;
		input.pos = 0;
		for (;;)
		{
			ZSTD_outBuffer out;
			size_t		old_input_pos = input.pos;
			size_t		output_size;

			output_size = PQ_COMPRESSION_MAX_SEGMENT_SIZE - segment_size;
			output_size = output_size == 0 ? 1 :
				Min(ZSTD_DStreamOutSize(), output_size);
			if (!compression_buffer_enlarge(&PqDecompressionBuffer, output_size))
				ereport(FATAL,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory"),
						 errdetail("Failed while decompressing protocol data.")));
			out.dst = PqDecompressionBuffer.data + PqDecompressionBuffer.len;
			out.size = output_size;
			out.pos = 0;
			result = ZSTD_decompressStream(PqDecompressionContext, &out, &input);
			if (ZSTD_isError(result))
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid compressed protocol message")));
			if (input.pos == old_input_pos && out.pos == 0)
			{
				if (input.pos == input.size)
					break;
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid compressed protocol message")));
			}
			PqDecompressionBuffer.len += out.pos;
			PqDecompressionBuffer.data[PqDecompressionBuffer.len] = '\0';
			segment_size += out.pos;
			if (segment_size > PQ_COMPRESSION_MAX_SEGMENT_SIZE)
				ereport(FATAL,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("compressed protocol message is too large")));
			if (result == 0)
			{
				if (input.pos != input.size)
					ereport(FATAL,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg("compressed protocol message contains multiple frames")));
				break;
			}
			if (input.pos == input.size && out.pos < out.size)
				break;
		}
		resetStringInfo(&PqDecompressionInput);
		PqDecompressionFrameEnded = (result == 0);

		if (PqDecompressionBuffer.len == 0)
		{
			if (result == 0)
				return 0;
			goto read_next_wrapper;
		}
		if (PqDecompressionBuffer.len >= 5)
		{
			uint32		message_length;

			if (PqDecompressionBuffer.data[0] != PqMsg_CopyData)
				goto invalid_contents;
			memcpy(&message_length, PqDecompressionBuffer.data + 1, 4);
			message_length = pg_ntoh32(message_length);
			if (message_length < 4 || message_length > PG_INT32_MAX)
				goto invalid_contents;
			if (message_length + 1 < PqDecompressionBuffer.len)
				goto invalid_contents;
			if (message_length + 1 == PqDecompressionBuffer.len)
			{
				resetStringInfo(s);
				if (!compression_buffer_enlarge(s,
												PqDecompressionBuffer.len - 5))
					ereport(FATAL,
							(errcode(ERRCODE_OUT_OF_MEMORY),
							 errmsg("out of memory"),
							 errdetail("Failed while decompressing protocol data.")));
				memcpy(s->data, PqDecompressionBuffer.data + 5,
					   PqDecompressionBuffer.len - 5);
				s->len = PqDecompressionBuffer.len - 5;
				s->data[s->len] = '\0';
				s->cursor = 0;
				resetStringInfo(&PqDecompressionBuffer);
				return PqMsg_CopyData;
			}
		}
		if (result == 0)
			goto invalid_contents;

read_next_wrapper:
		pq_startmsgread();
		if (pq_getbyte() != PqMsg_CompressedData)
			goto invalid_contents;
		if (PqDecompressionFrameEnded)
			goto invalid_contents;
	}

invalid_contents:
	ereport(FATAL,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("compressed protocol message contains invalid messages")));
	pg_unreachable();
}

static int
pq_compress_init(void)
{
	StringInfoData input;
	StringInfoData output;
	ZSTD_CCtx  *cctx;
	size_t		result;

	Assert(PqCompressionNegotiated);
	Assert(PqCompressionContext == NULL);
	if (!compression_buffer_init(&input))
		goto oom;
	if (!compression_buffer_init(&output))
	{
		pfree(input.data);
		goto oom;
	}
	cctx = ZSTD_createCCtx();
	if (cctx == NULL)
	{
		pfree(input.data);
		pfree(output.data);
		goto oom;
	}
	result = ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, PQ_COMPRESSION_WINDOW_LOG);
	if (ZSTD_isError(result))
	{
		ZSTD_freeCCtx(cctx);
		pfree(input.data);
		pfree(output.data);
		ereport(COMMERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not configure Zstandard compression window: %s",
						ZSTD_getErrorName(result))));
		goto fail;
	}
	PqCompressionInput = input;
	PqCompressionOutput = output;
	PqCompressionContext = cctx;
	return 0;

oom:
	ereport(COMMERROR,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("out of memory"),
			 errdetail("Failed while initializing protocol compression.")));
fail:
	ClientConnectionLost = 1;
	InterruptPending = 1;
	return EOF;
}

static int
pq_compress_flush_buffer(ZSTD_EndDirective directive, bool block)
{
	size_t		output_size;
	ZSTD_inBuffer input;
	ZSTD_outBuffer out;
	size_t		result;

	/* Do not create an empty frame for a result that was kept uncompressed. */
	if (!PqCompressionFrameStarted)
		return 0;

	output_size = ZSTD_compressBound(PqCompressionInput.len);
	resetStringInfo(&PqCompressionOutput);
	if (!compression_buffer_enlarge(&PqCompressionOutput, output_size))
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while compressing protocol data.")));
		goto fail;
	}
	input.src = PqCompressionInput.data;
	input.size = PqCompressionInput.len;
	input.pos = 0;
	out.dst = PqCompressionOutput.data;
	out.size = output_size;
	out.pos = 0;

	while (input.pos < input.size)
	{
		result = ZSTD_compressStream2(PqCompressionContext, &out, &input,
									  ZSTD_e_continue);
		if (ZSTD_isError(result))
			goto zstd_error;
		if (out.pos == out.size && input.pos < input.size)
			goto output_too_small;
	}
	do
	{
		result = ZSTD_compressStream2(PqCompressionContext, &out, &input,
									  directive);
		if (ZSTD_isError(result))
			goto zstd_error;
		if (out.pos == out.size && result != 0)
			goto output_too_small;
	} while (result != 0);

	if (out.pos > 0)
	{
		if (block)
		{
			if (PrevPQcommMethods->putmessage(PqMsg_CompressedData,
											  PqCompressionOutput.data, out.pos))
				goto fail;
		}
		else
		{
			PG_TRY();
			{
				PrevPQcommMethods->putmessage_noblock(PqMsg_CompressedData,
													  PqCompressionOutput.data, out.pos);
			}
			PG_CATCH();
			{
				/* The compressor has advanced; do not retry this input. */
				resetStringInfo(&PqCompressionInput);
				ClientConnectionLost = 1;
				InterruptPending = 1;
				PG_RE_THROW();
			}
			PG_END_TRY();
		}
	}

	resetStringInfo(&PqCompressionInput);
	return 0;

zstd_error:
	ereport(COMMERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("Zstandard compression failed: %s",
					ZSTD_getErrorName(result))));
	goto fail;

output_too_small:
	ereport(COMMERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("Zstandard compression output buffer is too small")));

fail:

	/*
	 * The compressor has consumed part of the pending input, so the segment
	 * we were building cannot be produced again.  Drop it and give up on the
	 * connection, as the send path does for a socket error.
	 */
	resetStringInfo(&PqCompressionInput);
	ClientConnectionLost = 1;
	InterruptPending = 1;
	return EOF;
}

static int
pq_compress_append(const void *data, size_t len, bool block)
{
	const char *ptr = data;

	while (len > 0)
	{
		size_t		available = PQ_COMPRESSION_INPUT_SIZE -
			PqCompressionInput.len;
		size_t		part = Min(len, available);

		if (part == 0)
		{
			if (pq_compress_flush_buffer(ZSTD_e_flush, block))
				return EOF;
			continue;
		}
		if (!compression_buffer_enlarge(&PqCompressionInput, part))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed while compressing protocol data.")));
			ClientConnectionLost = 1;
			InterruptPending = 1;
			return EOF;
		}
		appendBinaryStringInfo(&PqCompressionInput, ptr, part);
		ptr += part;
		len -= part;
		if (PqCompressionInput.len == PQ_COMPRESSION_INPUT_SIZE)
		{
			if (pq_compress_flush_buffer(ZSTD_e_flush, block))
				return EOF;
			/* Do not let a good compression ratio delay network delivery. */
			if (block && PrevPQcommMethods->flush())
				return EOF;
		}
	}
	return 0;
}

/*
 * Ordinary messages remain visible. Compression starts only after the first
 * ReadyForQuery, and every subsequent ReadyForQuery ends the current frame.
 */
static int
pq_compress_putmessage_internal(char msgtype, const char *s, size_t len,
								bool block)
{
	uint32		n32;

	if (PqCompressionStarted &&
		(msgtype == PqMsg_DataRow || msgtype == PqMsg_CopyData))
	{
		/* Keep isolated short rows out of the compressor. */
		if (!PqCompressionActive && len + 4 < 60)
		{
			PqCompressionSmallBytes += len + 5;
			if (PqCompressionSmallBytes >= 1024)
				PqCompressionActive = true;
		}
		else
		{
			if (PqCompressionContext == NULL && pq_compress_init())
				return EOF;
			PqCompressionActive = true;
			PqCompressionFrameStarted = true;
			n32 = pg_hton32((uint32) (len + 4));
			if (pq_compress_append(&msgtype, 1, block) ||
				pq_compress_append(&n32, 4, block) ||
				pq_compress_append(s, len, block))
				return EOF;
			return 0;
		}
	}

	if (PqCompressionStarted &&
		pq_compress_flush_buffer(msgtype == PqMsg_ReadyForQuery ?
								 ZSTD_e_end : ZSTD_e_flush, block))
		return EOF;

	if (block)
	{
		if (PrevPQcommMethods->putmessage(msgtype, s, len))
			return EOF;
	}
	else
		PrevPQcommMethods->putmessage_noblock(msgtype, s, len);

	if (msgtype == PqMsg_ReadyForQuery)
	{
		PqCompressionStarted = true;
		PqCompressionActive = false;
		PqCompressionFrameStarted = false;
		PqCompressionSmallBytes = 0;

		/*
		 * Do not reset the incoming stream here.  After a COPY error, the
		 * frontend may still be sending data that we must decode and discard
		 * until CopyDone or CopyFail.
		 */
	}
	return 0;
}

static void
pq_compress_comm_reset(void)
{
	PqCompressBusy = false;
	PrevPQcommMethods->comm_reset();
}

static int
pq_compress_flush(void)
{
	int			result;

	if (PqCompressBusy)
		return 0;
	PqCompressBusy = true;
	result = pq_compress_flush_buffer(ZSTD_e_flush, true);
	if (result == 0)
		result = PrevPQcommMethods->flush();
	PqCompressBusy = false;
	return result;
}

static int
pq_compress_flush_if_writable(void)
{
	int			result;

	if (PqCompressBusy)
		return 0;
	PqCompressBusy = true;
	result = pq_compress_flush_buffer(ZSTD_e_flush, false);
	if (result == 0)
		result = PrevPQcommMethods->flush_if_writable();
	PqCompressBusy = false;
	return result;
}

static bool
pq_compress_is_send_pending(void)
{
	return PqCompressionInput.len > 0 ||
		PrevPQcommMethods->is_send_pending();
}

static int
pq_compress_putmessage(char msgtype, const char *s, size_t len)
{
	int			result;

	if (PqCompressBusy)
		return 0;
	PqCompressBusy = true;
	result = pq_compress_putmessage_internal(msgtype, s, len, true);
	PqCompressBusy = false;
	return result;
}

static void
pq_compress_putmessage_noblock(char msgtype, const char *s, size_t len)
{
	if (PqCompressBusy)
		return;
	PqCompressBusy = true;
	(void) pq_compress_putmessage_internal(msgtype, s, len, false);
	PqCompressBusy = false;
}
#endif
