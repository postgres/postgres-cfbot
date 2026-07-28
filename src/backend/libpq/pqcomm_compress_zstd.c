/*-------------------------------------------------------------------------
 *
 * pqcomm_compress_zstd.c
 *	  Compress backend messages with zstd
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/libpq/pqcomm_compress_zstd.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/libpq.h"
#include "utils/memutils.h"

#ifndef USE_ZSTD

const		PQcompressMethods *
pq_init_compressor_zstd(pqcomm_compress * cs, pg_compress_specification *specification, void **new_private_data)
{
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("zstd compression is not supported by this build")));
}

#else
#include <zstd.h>

typedef struct pqcomm_zstd
{
	ZSTD_CCtx  *cctx;
}			pqcomm_zstd;

static int	pq_compress_flush_zstd(pqcomm_compress * cs, bool block, bool end_frame);
static int	pq_compress_message_zstd(pqcomm_compress * cs, bool block, bool start_frame);
static void pq_compress_free_zstd(pqcomm_compress * cs);

static const PQcompressMethods PqCompressMethodsZstd = {
	.flush = pq_compress_flush_zstd,
	.compress_message = pq_compress_message_zstd,
	.free_compress_context = pq_compress_free_zstd,
};

/*
 * pq_init_compressor_zstd -- Initialize a zstd compressor or change compress
 * specification
 *
 * On success, the compressor state will be stored in new_private_data.
 *
 * returns PQcompressMethods of the zstd compressor
 */
const		PQcompressMethods *
pq_init_compressor_zstd(pqcomm_compress * cs, pg_compress_specification *specification, void **new_private_data)
{
	int			ret;
	pqcomm_zstd *zstate;

	Assert(!cs->opened_frame);
	if (cs->algorithm != PG_COMPRESSION_ZSTD)
	{
		/* We have an algorithm change, create a brand-new context */
		MemoryContext oldctx = MemoryContextSwitchTo(TopMemoryContext);
		size_t		outbuf_size = ZSTD_CStreamOutSize();

		/* Initialize compress buffer */
		if (cs->outBuf.data == NULL)
			initStringInfoExt(&cs->outBuf, outbuf_size);
		else
			enlargeStringInfo(&cs->outBuf, outbuf_size);

		zstate = palloc0_object(pqcomm_zstd);
		MemoryContextSwitchTo(oldctx);

		/* Create ctx */
		zstate->cctx = ZSTD_createCCtx();
		if (!zstate->cctx)
		{
			pfree(zstate);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not create zstd compression context")));
		}
	}
	else
	{
		/* This is just a parameter change, reuse existing context */
		zstate = (pqcomm_zstd *) cs->private_data;

		/*
		 * Some parameters like long_distance can only be changed during
		 * zstd's init state, so reset cctx's session to allow such changes.
		 */
		ZSTD_CCtx_reset(zstate->cctx, ZSTD_reset_session_and_parameters);
	}

	ret = ZSTD_CCtx_setParameter(zstate->cctx, ZSTD_c_compressionLevel,
								 specification->level);
	if (ZSTD_isError(ret))
	{
		if (cs->algorithm != PG_COMPRESSION_ZSTD)
		{
			/* Free the context and zstate if they were created */
			ZSTD_freeCCtx(zstate->cctx);
			pfree(zstate);
		}
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not set zstd compression level to %d: %s",
						specification->level, ZSTD_getErrorName(ret))));
	}


	if (specification->options & PG_COMPRESSION_OPTION_LONG_DISTANCE)
	{
		ret = ZSTD_CCtx_setParameter(zstate->cctx,
									 ZSTD_c_enableLongDistanceMatching,
									 specification->long_distance);
		if (ZSTD_isError(ret))
		{
			if (cs->algorithm != PG_COMPRESSION_ZSTD)
			{
				/* Free the context and zstate if they were created */
				ZSTD_freeCCtx(zstate->cctx);
				pfree(zstate);
			}
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not set zstd long distance to %i: %s",
							specification->long_distance, ZSTD_getErrorName(ret))));
		}
	}

	*new_private_data = zstate;
	return &PqCompressMethodsZstd;
}


/*
 * pq_compress_message_zstd -- Compress a message using zstd compressor
 *
 * If the compress buffer is full, it will be sent immediately, allowing to
 * reset the buffer and compress the rest of the message.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_message_zstd(pqcomm_compress * cs, bool block, bool start_frame)
{
	ZSTD_inBuffer inBuf;
	size_t		yet_to_flush = 0;
	ZSTD_outBuffer outBuf;
	pqcomm_zstd *zstate = (pqcomm_zstd *) cs->private_data;

	inBuf.src = cs->inBuf.data;
	inBuf.size = cs->inBuf.len;
	inBuf.pos = 0;

	outBuf.dst = cs->outBuf.data;
	outBuf.size = cs->outBuf.maxlen;
	outBuf.pos = cs->outBuf.len;

	do
	{
		yet_to_flush =
			ZSTD_compressStream2(zstate->cctx,
								 &outBuf,
								 &inBuf, ZSTD_e_continue);
		if (ZSTD_isError(yet_to_flush))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not compress data: %s",
							ZSTD_getErrorName(yet_to_flush))));
		/* Keep compress buffer in sync */
		cs->outBuf.len = outBuf.pos;

		/*
		 * If the output buffer is left with not enough space, send the
		 * compressed bytes to the underlying pqcomm, which will empty the
		 * buffer.
		 */
		if (yet_to_flush > 0)
		{
			if (pq_send_compressed_message(block, true) == EOF)
				return EOF;
			/* sync position after pq_send_compressed_message call */
			outBuf.pos = cs->outBuf.len;
		}
	} while (yet_to_flush > 0);

	return 0;
}

/*
 * pq_compress_flush_zstd -- flush any pending data in the compress buffer
 *
 * If the compress buffer is full, it will be sent immediately, allowing to
 * reset the buffer and compress the rest of the data.
 *
 * returns 0 if OK, EOF if trouble
 */
static int
pq_compress_flush_zstd(pqcomm_compress * cs, bool block, bool end_frame)
{
	size_t		yet_to_flush;
	pqcomm_zstd *zstate = (pqcomm_zstd *) cs->private_data;
	ZSTD_outBuffer outBuf;

	outBuf.dst = cs->outBuf.data;
	outBuf.size = cs->outBuf.maxlen;
	outBuf.pos = cs->outBuf.len;

	do
	{
		ZSTD_inBuffer in = {NULL, 0, 0};
		size_t		max_needed = ZSTD_compressBound(0);

		/*
		 * If the output buffer is left with not enough space, send the
		 * compressed bytes to the underlying pqcomm.
		 */
		if (outBuf.size - outBuf.pos < max_needed)
		{
			if (pq_send_compressed_message(block, false))
				return EOF;
			outBuf.pos = cs->outBuf.len;
		}

		yet_to_flush = ZSTD_compressStream2(zstate->cctx,
											&outBuf,
											&in, end_frame ? ZSTD_e_end : ZSTD_e_flush);
		/* Keep compress buffer in sync */
		cs->outBuf.len = outBuf.pos;

		if (ZSTD_isError(yet_to_flush))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not compress data: %s",
							ZSTD_getErrorName(yet_to_flush))));
	} while (yet_to_flush > 0);
	return 0;
}

/*
 * pq_compress_free_zstd -- free the compressor context
 */
static void
pq_compress_free_zstd(pqcomm_compress * cs)
{
	pqcomm_zstd *zstate = (pqcomm_zstd *) cs->private_data;

	ZSTD_freeCCtx(zstate->cctx);
	pfree(zstate);
}

#endif
