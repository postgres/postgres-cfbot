/*-------------------------------------------------------------------------
 *
 * pqcomm_compress_lz4.c
 *	  Compress backend messages with lz4
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/libpq/pqcomm_compress_lz4.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/libpq.h"
#include "utils/memutils.h"

#ifndef USE_LZ4

const		PQcompressMethods *
pq_init_compressor_lz4(pqcomm_compress * cs, pg_compress_specification *specification, void **new_private_data)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("lz4 compression is not supported by this build")));
}

#else
#include <lz4frame.h>

#define CHUNK_SIZE (64 * 1024)

typedef struct pqcomm_lz4
{
	LZ4F_cctx  *ctx;
	LZ4F_preferences_t prefs;
}			pqcomm_lz4;

static int	pq_compress_flush_lz4(pqcomm_compress * cs, bool block, bool end_frame);
static int	pq_compress_message_lz4(pqcomm_compress * cs, bool block, bool start_frame);
static void pq_compress_free_lz4(pqcomm_compress * cs);

static const PQcompressMethods PqCompressMethodsLz4 = {
	.flush = pq_compress_flush_lz4,
	.compress_message = pq_compress_message_lz4,
	.free_compress_context = pq_compress_free_lz4,
};

/*
 * pq_init_compressor_lz4 -- Initialize a lz4 compressor or change compress specification
 *
 * On success, the compressor state will be stored in new_private_data.
 *
 * returns PQcompressMethods of the lz4 compressor
 */
const		PQcompressMethods *
pq_init_compressor_lz4(pqcomm_compress * cs, pg_compress_specification *specification, void **new_private_data)
{
	pqcomm_lz4 *lz4_state;

	Assert(!cs->opened_frame);
	if (cs->algorithm != PG_COMPRESSION_LZ4)
	{
		/* We have an algorithm change, create a brand-new context */
		LZ4F_errorCode_t ctxError;
		size_t		outbuf_size;

		MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);

		/* Initialize state */
		lz4_state = palloc0_object(pqcomm_lz4);
		lz4_state->prefs.compressionLevel = specification->level;

		/*
		 * LZ4F_compressBound provides the outbuf size needed in the worst
		 * case scenario, which is going to be 65544 for a 64KB srcSize. As we
		 * want to send chunks of 64KB of compressed data, we need to double
		 * that size so we can call LZ4F_compressUpdate until we have 64KB
		 * available to send.
		 */
		outbuf_size = 2 * LZ4F_compressBound(CHUNK_SIZE, &lz4_state->prefs);

		/*
		 * Initialize or enlarge outBuf to be able to store a full chunk
		 */
		if (cs->outBuf.data == NULL)
			initStringInfoExt(&cs->outBuf, outbuf_size);
		else
			enlargeStringInfo(&cs->outBuf, outbuf_size);

		MemoryContextSwitchTo(oldctx);

		/* and create the lz4 ctx */
		ctxError = LZ4F_createCompressionContext(&lz4_state->ctx, LZ4F_VERSION);
		if (LZ4F_isError(ctxError))
		{
			pfree(lz4_state);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not create lz4 compression context: %s",
							LZ4F_getErrorName(ctxError))));
		}
	}
	else
	{
		/* This is just a parameter change, reuse existing context */
		lz4_state = (pqcomm_lz4 *) cs->private_data;
		lz4_state->prefs.compressionLevel = specification->level;
	}

	*new_private_data = lz4_state;
	return &PqCompressMethodsLz4;
}

/*
 * pq_compress_start_lz4_frame -- Start a new lz4 frame
 */
static void
pq_compress_start_lz4_frame(pqcomm_compress * cs)
{
	size_t		ret;
	pqcomm_lz4 *lz4_state = (pqcomm_lz4 *) cs->private_data;

	Assert(cs->outBuf.len == 0);
	ret = LZ4F_compressBegin(lz4_state->ctx,
							 cs->outBuf.data + cs->outBuf.len,
							 cs->outBuf.maxlen - cs->outBuf.len,
							 &lz4_state->prefs);
	if (LZ4F_isError(ret))
	{
		if (cs->algorithm != PG_COMPRESSION_LZ4)
		{
			/* Free the context and state if they were created */
			LZ4F_freeCompressionContext(lz4_state->ctx);
			pfree(lz4_state);
		}
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not write lz4 header: %s", LZ4F_getErrorName(ret))));
	}
	cs->outBuf.len += ret;
}

/*
 * pq_compress_message_lz4 -- Compress a message using lz4 compressor
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_message_lz4(pqcomm_compress * cs, bool block, bool start_frame)
{
	pqcomm_lz4 *lz4_state = (pqcomm_lz4 *) cs->private_data;
	size_t		remaining = cs->inBuf.len;

	/* Start new lz4 frame if necessary */
	if (start_frame)
		pq_compress_start_lz4_frame(cs);

	Assert(cs->inBuf.cursor == 0);

	while (remaining > 0)
	{
		size_t		written;

		/*
		 * To keep memory usage constrained, we cap the amount of data to
		 * compress to CHUNK_SIZE.
		 */
		size_t		chunk = Min(remaining, CHUNK_SIZE);
		size_t		bound = LZ4F_compressBound(chunk, &lz4_state->prefs);

		if (cs->outBuf.maxlen - cs->outBuf.len < bound)
		{
			/* We need to free space in the outBuf, send what we have */
			if (pq_send_compressed_message(block, true))
				return EOF;
		}
		Assert(cs->outBuf.maxlen - cs->outBuf.len >= bound);

		written = LZ4F_compressUpdate(lz4_state->ctx,
									  cs->outBuf.data + cs->outBuf.len,
									  cs->outBuf.maxlen - cs->outBuf.len,
									  cs->inBuf.data + cs->inBuf.cursor,
									  chunk,
									  NULL);
		if (LZ4F_isError(written))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not compress data: %s", LZ4F_getErrorName(written))));

		/* Update output buffer len */
		cs->outBuf.len += written;

		/* Update input buffer with consumed chunk */
		cs->inBuf.cursor += chunk;
		remaining = cs->inBuf.len - cs->inBuf.cursor;
	}

	return 0;
}

/*
 * pq_compress_flush_lz4 -- flush any pending data in the compress buffer
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_flush_lz4(pqcomm_compress * cs, bool block, bool end_frame)
{
	pqcomm_lz4 *lz4_state = (pqcomm_lz4 *) cs->private_data;
	size_t		ret;
	size_t		bound;

	/* Make sure the output buffer has enough room for the flush */
	bound = LZ4F_compressBound(0, &lz4_state->prefs);
	if (cs->outBuf.maxlen - cs->outBuf.len < bound)
	{
		if (pq_send_compressed_message(block, false))
			return EOF;
	}
	Assert(cs->outBuf.maxlen - cs->outBuf.len >= bound);

	if (end_frame)
	{
		ret = LZ4F_compressEnd(lz4_state->ctx,
							   cs->outBuf.data + cs->outBuf.len,
							   cs->outBuf.maxlen - cs->outBuf.len,
							   NULL);
	}
	else
		ret = LZ4F_flush(lz4_state->ctx,
						 cs->outBuf.data + cs->outBuf.len,
						 cs->outBuf.maxlen - cs->outBuf.len,
						 NULL);
	if (LZ4F_isError(ret))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not flush lz4 compressed data: %s", LZ4F_getErrorName(ret))));

	/* Update output buffer len */
	cs->outBuf.len += ret;

	return 0;
}

/*
 * pq_compress_free_lz4 -- free the compressor context
 */
static void
pq_compress_free_lz4(pqcomm_compress * cs)
{
	pqcomm_lz4 *lz4_state = (pqcomm_lz4 *) cs->private_data;

	LZ4F_freeCompressionContext(lz4_state->ctx);
	pfree(lz4_state);
}

#endif
