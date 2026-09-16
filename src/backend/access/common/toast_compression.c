/*-------------------------------------------------------------------------
 *
 * toast_compression.c
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021-2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_compression.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_LZ4
#include <lz4.h>
#endif

#ifdef USE_ZSTD
#include <zstd.h>
#endif

#include "access/detoast.h"
#include "access/toast_compression.h"
#include "common/pg_lzcompress.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "varatt.h"

/* GUC */
int			default_toast_compression = DEFAULT_TOAST_COMPRESSION;

#ifdef USE_ZSTD
/*
 * zstd decompression context, created on first use and kept for the life of
 * the backend: creating one per value costs more than decompressing a small
 * value.
 *
 * It is reset at the start of each use.  This is required, not an
 * optimization: a slice stops in the middle of a frame, and an error can
 * leave a frame half decoded, so without the reset the next value's decoding
 * would continue the previous frame and fail or return wrong data.  Anything
 * else that reuses this context must go through zstd_get_dctx().
 *
 * The scope is the backend because detoasting has no owning object to tie
 * the context to, unlike WAL decoding, which keeps its context in the
 * XLogReaderState.  WAL compression likewise keeps one context for the
 * backend.
 *
 * zstd allocates the context with malloc(), so its memory is not visible in
 * pg_backend_memory_contexts.  After full decompressions it is about 100kB.
 * Slices also leave the streaming decoder's buffer in it, the frame's window
 * plus a block.  The window size comes from the frame header, so it is
 * capped at ZSTD_TOAST_WINDOWLOG_MAX, which the default compression level
 * never exceeds; a corrupt header cannot make the context grow beyond that.
 * zstd shrinks the buffer by itself once later uses no longer need it.
 */
#define ZSTD_TOAST_WINDOWLOG_MAX	23

static ZSTD_DCtx *zstd_dctx = NULL;

static ZSTD_DCtx *
zstd_get_dctx(void)
{
	if (zstd_dctx == NULL)
	{
		size_t		ret;

		zstd_dctx = ZSTD_createDCtx();
		if (zstd_dctx == NULL)
			elog(ERROR, "could not create zstd decompression context");
		/* a session reset keeps parameters, so this is set once */
		ret = ZSTD_DCtx_setParameter(zstd_dctx, ZSTD_d_windowLogMax,
									 ZSTD_TOAST_WINDOWLOG_MAX);
		if (ZSTD_isError(ret))
		{
			ZSTD_freeDCtx(zstd_dctx);
			zstd_dctx = NULL;
			elog(ERROR, "could not set zstd decompression window: %s",
				 ZSTD_getErrorName(ret));
		}
	}
	else
		ZSTD_DCtx_reset(zstd_dctx, ZSTD_reset_session_only);
	return zstd_dctx;
}

/*
 * A backend-lifetime ZSTD_CCtx for plain (non-dictionary) compression, kept
 * for the same reason as the decompression context above: a one-shot
 * ZSTD_compress() allocates and frees a compression context for every value,
 * sized to the input, from about 90kB for a 4kB value up to about 1.3MB for
 * values of 1MB or more; for a bulk load that dominates.  The context keeps
 * the size of the largest value compressed so far, outside any memory
 * context like the decompression context.
 */
static ZSTD_CCtx *zstd_cctx = NULL;

static ZSTD_CCtx *
zstd_get_cctx(void)
{
	if (zstd_cctx == NULL)
	{
		zstd_cctx = ZSTD_createCCtx();
		if (zstd_cctx == NULL)
			elog(ERROR, "could not create zstd compression context");
	}
	return zstd_cctx;
}
#endif

#define NO_COMPRESSION_SUPPORT(method) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("compression method %s not supported", method), \
			 errdetail("This functionality requires the server to be built with %s support.", method)))

/*
 * Compress a varlena using PGLZ.
 *
 * Returns the compressed varlena, or NULL if compression fails.
 */
varlena *
pglz_compress_datum(const varlena *value)
{
	int32		valsize,
				len;
	varlena    *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * No point in wasting a palloc cycle if value size is outside the allowed
	 * range for compression.
	 */
	if (valsize < PGLZ_strategy_default->min_input_size ||
		valsize > PGLZ_strategy_default->max_input_size)
		return NULL;

	/*
	 * Figure out the maximum possible size of the pglz output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 *
	 * VARSIZE_ANY_EXHDR's refusal to return values higher than 1GB are what
	 * protect this addition from wraparound; the PGLZ strategy doesn't
	 * provide a useful backstop.
	 */
	tmp = (varlena *) palloc(PGLZ_MAX_OUTPUT(valsize) +
							 VARHDRSZ_COMPRESSED);

	len = pglz_compress(VARDATA_ANY(value),
						valsize,
						(char *) tmp + VARHDRSZ_COMPRESSED,
						NULL);
	if (len < 0)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
}

/*
 * Decompress a varlena that was compressed using PGLZ.
 */
varlena *
pglz_decompress_datum(const varlena *value)
{
	varlena    *result;
	int32		rawsize;

	Assert(VARDATA_COMPRESSED_GET_COMPRESS_METHOD(value) == TOAST_PGLZ_COMPRESSION_ID);

	/*
	 * Allocate memory for the uncompressed data.  VARLENA_EXTSIZE_MASK
	 * protects this against overflow.
	 */
	result = (varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/* decompress the data */
	rawsize = pglz_decompress((const char *) value + VARHDRSZ_COMPRESSED,
							  VARSIZE(value) - VARHDRSZ_COMPRESSED,
							  VARDATA(result),
							  VARDATA_COMPRESSED_GET_EXTSIZE(value), true);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * Decompress part of a varlena that was compressed using PGLZ.
 */
varlena *
pglz_decompress_datum_slice(const varlena *value,
							int32 slicelength)
{
	varlena    *result;
	int32		rawsize;

	Assert(VARDATA_COMPRESSED_GET_COMPRESS_METHOD(value) == TOAST_PGLZ_COMPRESSION_ID);

	/*
	 * Allocate memory for the uncompressed data.  Caller must have checked
	 * the requested size, but verify.
	 */
	if (!AllocSizeIsValid(slicelength))
		elog(ERROR, "value is too big");
	result = (varlena *) palloc(slicelength + VARHDRSZ);

	/* decompress the data */
	rawsize = pglz_decompress((const char *) value + VARHDRSZ_COMPRESSED,
							  VARSIZE(value) - VARHDRSZ_COMPRESSED,
							  VARDATA(result),
							  slicelength, false);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed pglz data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * Compress a varlena using LZ4.
 *
 * Returns the compressed varlena, or NULL if compression fails.
 */
varlena *
lz4_compress_datum(const varlena *value)
{
#ifndef USE_LZ4
	NO_COMPRESSION_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		valsize;
	int32		len;
	int32		max_size;
	varlena    *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * Figure out the maximum possible size of the LZ4 output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount.
	 *
	 * The current (2026) definition of LZ4_compressBound cannot yield a value
	 * that can wrap-around the VARHDRSZ_COMPRESSED addition in 32-bit
	 * machines, but we have no way to prevent future LZ4 development (or a
	 * malicious LZ4 library) from changing that, so verify.
	 */
	max_size = LZ4_compressBound(valsize);
	if (!AllocSizeIsValid(max_size))
		elog(ERROR, "toast object size is too big");
	tmp = (varlena *) palloc(max_size + VARHDRSZ_COMPRESSED);

	len = LZ4_compress_default(VARDATA_ANY(value),
							   (char *) tmp + VARHDRSZ_COMPRESSED,
							   valsize, max_size);
	if (len <= 0)
		elog(ERROR, "lz4 compression failed");

	/* data is incompressible so just free the memory and return NULL */
	if (len > valsize)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED);

	return tmp;
#endif
}

/*
 * Decompress a varlena that was compressed using LZ4.
 */
varlena *
lz4_decompress_datum(const varlena *value)
{
#ifndef USE_LZ4
	NO_COMPRESSION_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		rawsize;
	varlena    *result;

	Assert(VARDATA_COMPRESSED_GET_COMPRESS_METHOD(value) == TOAST_LZ4_COMPRESSION_ID);

	/*
	 * Allocate memory for the uncompressed data.  VARLENA_EXTSIZE_MASK
	 * protects this against overflow.
	 */
	Assert(AllocSizeIsValid(VARDATA_COMPRESSED_GET_EXTSIZE(value)));
	result = (varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/* decompress the data */
	rawsize = LZ4_decompress_safe((const char *) value + VARHDRSZ_COMPRESSED,
								  VARDATA(result),
								  VARSIZE(value) - VARHDRSZ_COMPRESSED,
								  VARDATA_COMPRESSED_GET_EXTSIZE(value));
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));


	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * Decompress part of a varlena that was compressed using LZ4.
 */
varlena *
lz4_decompress_datum_slice(const varlena *value, int32 slicelength)
{
#ifndef USE_LZ4
	NO_COMPRESSION_SUPPORT("lz4");
	return NULL;				/* keep compiler quiet */
#else
	int32		rawsize;
	varlena    *result;

	Assert(VARDATA_COMPRESSED_GET_COMPRESS_METHOD(value) == TOAST_LZ4_COMPRESSION_ID);

	/* slice decompression not supported prior to 1.8.3 */
	if (LZ4_versionNumber() < 10803)
		return lz4_decompress_datum(value);

	/*
	 * Allocate memory for the uncompressed data.  Caller must have checked
	 * the requested size, but verify.
	 */
	if (!AllocSizeIsValid(slicelength))
		elog(ERROR, "toast object size is too big");
	result = (varlena *) palloc(slicelength + VARHDRSZ);

	/* decompress the data */
	rawsize = LZ4_decompress_safe_partial((const char *) value + VARHDRSZ_COMPRESSED,
										  VARDATA(result),
										  VARSIZE(value) - VARHDRSZ_COMPRESSED,
										  slicelength,
										  slicelength);
	if (rawsize < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * Compress a varlena using Zstandard.
 *
 * Returns the compressed varlena, or NULL if the data is incompressible.
 *
 * Unlike pglz and lz4, zstd's method ID does not fit in the two method bits
 * of the compressed header, so the datum is laid out with the long form of
 * the header (VARHDRSZ_COMPRESSED_LONG), whose extra byte will receive the ID.
 */
varlena *
zstd_compress_datum(const varlena *value)
{
#ifndef USE_ZSTD
	NO_COMPRESSION_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else
	int32		valsize;
	size_t		len;
	size_t		max_size;
	varlena    *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	/*
	 * Figure out the maximum possible size of the zstd output, add the bytes
	 * that will be needed for varlena overhead, and allocate that amount. As
	 * for lz4, verify the bound rather than trust the library.
	 */
	max_size = ZSTD_compressBound(valsize);
	if (ZSTD_isError(max_size) || !AllocSizeIsValid(max_size))
		elog(ERROR, "toast object size is too big");
	tmp = (varlena *) palloc(max_size + VARHDRSZ_COMPRESSED_LONG);

	len = ZSTD_compressCCtx(zstd_get_cctx(),
							(char *) tmp + VARHDRSZ_COMPRESSED_LONG,
							max_size,
							VARDATA_ANY(value),
							valsize,
							ZSTD_CLEVEL_DEFAULT);
	if (ZSTD_isError(len))
		elog(ERROR, "zstd compression failed: %s", ZSTD_getErrorName(len));

	/* data is incompressible so just free the memory and return NULL */
	if (len > (size_t) valsize)
	{
		pfree(tmp);
		return NULL;
	}

	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED_LONG);

	return tmp;
#endif
}

/*
 * Decompress a varlena that was compressed using Zstandard.
 */
varlena *
zstd_decompress_datum(const varlena *value)
{
#ifndef USE_ZSTD
	NO_COMPRESSION_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else
	size_t		rawsize;
	varlena    *result;

	Assert(VARDATA_COMPRESSED_GET_COMPRESS_METHOD(value) == TOAST_ZSTD_COMPRESSION_ID);

	/*
	 * The compressed size is passed to zstd as a size_t, so check it cannot
	 * underflow; pglz and lz4 take a signed size and reject it themselves.
	 */
	if (VARSIZE(value) < VARHDRSZ_COMPRESSED_LONG)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt")));

	/*
	 * Allocate memory for the uncompressed data.  VARLENA_EXTSIZE_MASK
	 * protects this against overflow.
	 */
	Assert(AllocSizeIsValid(VARDATA_COMPRESSED_GET_EXTSIZE(value)));
	result = (varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/*
	 * Decompress the data.  zstd checks the frame against its own recorded
	 * content size; also check that it matches the size in our header, as
	 * pglz does.
	 */
	rawsize = ZSTD_decompressDCtx(zstd_get_dctx(),
								  VARDATA(result),
								  VARDATA_COMPRESSED_GET_EXTSIZE(value),
								  (const char *) value + VARHDRSZ_COMPRESSED_LONG,
								  VARSIZE(value) - VARHDRSZ_COMPRESSED_LONG);
	if (ZSTD_isError(rawsize) ||
		rawsize != VARDATA_COMPRESSED_GET_EXTSIZE(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt")));

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
#endif
}

/*
 * Decompress part of a varlena that was compressed using Zstandard.
 */
varlena *
zstd_decompress_datum_slice(const varlena *value, int32 slicelength)
{
#ifndef USE_ZSTD
	NO_COMPRESSION_SUPPORT("zstd");
	return NULL;				/* keep compiler quiet */
#else
	ZSTD_DCtx  *dctx;
	ZSTD_inBuffer inbuf;
	ZSTD_outBuffer outbuf;
	const char *src;
	size_t		srclen;
	varlena    *result;
	bool		failed = false;

	Assert(VARDATA_COMPRESSED_GET_COMPRESS_METHOD(value) == TOAST_ZSTD_COMPRESSION_ID);

	/*
	 * The one-shot API can only decompress a whole frame, so use the
	 * streaming API instead: it stops as soon as the output buffer is full.
	 * Note that this means we never reach the end of the frame, so its
	 * checksum (if any) is not verified; that matches the behavior of the
	 * other methods here.
	 *
	 * Also note that zstd cannot stop inside a block, and a block holds up to
	 * 128 kB of uncompressed data, so for values smaller than that this
	 * decodes the whole value anyway and only saves the copy of the rest. The
	 * saving over a full decompression starts with larger values.
	 */

	/* As in zstd_decompress_datum(), the compressed size must not underflow */
	if (VARSIZE(value) < VARHDRSZ_COMPRESSED_LONG)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt")));

	src = (const char *) value + VARHDRSZ_COMPRESSED_LONG;
	srclen = VARSIZE(value) - VARHDRSZ_COMPRESSED_LONG;

	/*
	 * Unlike the one-shot API, the streaming decoder does not check the
	 * frame's recorded content size against anything, so check it against the
	 * size in our header here.  Our frames always record it.
	 */
	if (ZSTD_getFrameContentSize(src, srclen) !=
		(unsigned long long) VARDATA_COMPRESSED_GET_EXTSIZE(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt")));

	/*
	 * Allocate memory for the uncompressed slice.  Caller must have checked
	 * the requested size, but verify.
	 */
	if (!AllocSizeIsValid(slicelength))
		elog(ERROR, "toast object size is too big");
	result = (varlena *) palloc(slicelength + VARHDRSZ);

	dctx = zstd_get_dctx();

	inbuf.src = src;
	inbuf.size = 0;
	inbuf.pos = 0;
	outbuf.dst = VARDATA(result);
	outbuf.size = slicelength;
	outbuf.pos = 0;

	/*
	 * Feed the input a chunk at a time, so that a long decode can be
	 * interrupted.
	 */
	while (outbuf.pos < outbuf.size)
	{
		size_t		ret;

		CHECK_FOR_INTERRUPTS();
		if (inbuf.pos == inbuf.size)
		{
			if (inbuf.size == srclen)
			{
				/* all input consumed without filling the output */
				failed = true;
				break;
			}
			inbuf.size = Min(srclen, inbuf.size + ZSTD_DStreamInSize());
		}
		ret = ZSTD_decompressStream(dctx, &outbuf, &inbuf);

		if (ZSTD_isError(ret))
		{
			failed = true;
			break;
		}

		/*
		 * A zero return means the frame is fully decoded.  If we could not
		 * fill the output buffer by then the data must be truncated or
		 * corrupt, since the caller has already established that the value
		 * decompresses to more than slicelength bytes.
		 */
		if (ret == 0)
		{
			failed = (outbuf.pos < outbuf.size);
			break;
		}
	}

	if (failed)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt")));

	SET_VARSIZE(result, outbuf.pos + VARHDRSZ);

	return result;
#endif
}

/*
 * Extract compression ID from a varlena.
 *
 * Returns TOAST_INVALID_COMPRESSION_ID if the varlena is not compressed.
 */
ToastCompressionId
toast_get_compression_id(varlena *attr)
{
	ToastCompressionId cmid = TOAST_INVALID_COMPRESSION_ID;

	/*
	 * If it is stored externally then fetch the compression method id from
	 * the external toast pointer.  If compressed inline, fetch it from the
	 * toast compression header.
	 */
	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		toast_external_data toast_ext_data;

		toast_external_info_get(attr, &toast_ext_data);

		cmid = toast_ext_data.compress_method;
		if (toast_ext_data.is_compressed &&
			cmid == TOAST_INVALID_COMPRESSION_ID)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("compressed data is corrupt")));
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		cmid = toast_inline_compression_id(attr);
		if (cmid == TOAST_INVALID_COMPRESSION_ID)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("compressed data is corrupt")));
	}

	return cmid;
}

/*
 * CompressionNameToMethod - Get compression method from compression name
 *
 * Search in the available built-in methods.  If the compression not found
 * in the built-in methods then return InvalidCompressionMethod.
 */
char
CompressionNameToMethod(const char *compression)
{
	if (strcmp(compression, "pglz") == 0)
		return TOAST_PGLZ_COMPRESSION;
	else if (strcmp(compression, "lz4") == 0)
	{
#ifndef USE_LZ4
		NO_COMPRESSION_SUPPORT("lz4");
#endif
		return TOAST_LZ4_COMPRESSION;
	}
	else if (strcmp(compression, "zstd") == 0)
	{
#ifndef USE_ZSTD
		NO_COMPRESSION_SUPPORT("zstd");
#endif
		return TOAST_ZSTD_COMPRESSION;
	}

	return InvalidCompressionMethod;
}

/*
 * GetCompressionMethodName - Get compression method name
 */
const char *
GetCompressionMethodName(char method)
{
	switch (method)
	{
		case TOAST_PGLZ_COMPRESSION:
			return "pglz";
		case TOAST_LZ4_COMPRESSION:
			return "lz4";
		case TOAST_ZSTD_COMPRESSION:
			return "zstd";
		default:
			elog(ERROR, "invalid compression method %c", method);
			return NULL;		/* keep compiler quiet */
	}
}
