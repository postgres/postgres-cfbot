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
#include "varatt.h"

/* GUC */
int			default_toast_compression = DEFAULT_TOAST_COMPRESSION;

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

	/* allocate memory for the uncompressed data */
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

	/* allocate memory for the uncompressed data */
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
	 */
	max_size = LZ4_compressBound(valsize);
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

	/* allocate memory for the uncompressed data */
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

	/* allocate memory for the uncompressed data */
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
 * Returns the compressed varlena, or NULL if compression fails.
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
	 * that will be needed for varlena overhead, and allocate that amount.
	 */
	max_size = ZSTD_compressBound(valsize);
	tmp = (varlena *) palloc(max_size + VARHDRSZ_COMPRESSED_LONG);

	len = ZSTD_compress((char *) tmp + VARHDRSZ_COMPRESSED_LONG,
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

	/* allocate memory for the uncompressed data */
	result = (varlena *) palloc(VARDATA_COMPRESSED_GET_EXTSIZE(value) + VARHDRSZ);

	/*
	 * Decompress the data.  zstd checks the frame against its own recorded
	 * content size; also check that it matches the size in our header, as
	 * pglz does.
	 */
	rawsize = ZSTD_decompress(VARDATA(result),
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

	/*
	 * Allocate memory for the uncompressed data first, so that a failure here
	 * cannot leak the decompression context, which is not palloc'd.
	 */
	result = (varlena *) palloc(slicelength + VARHDRSZ);

	dctx = ZSTD_createDCtx();
	if (dctx == NULL)
		elog(ERROR, "could not create zstd decompression context");

	inbuf.src = (const char *) value + VARHDRSZ_COMPRESSED_LONG;
	inbuf.size = VARSIZE(value) - VARHDRSZ_COMPRESSED_LONG;
	inbuf.pos = 0;
	outbuf.dst = VARDATA(result);
	outbuf.size = slicelength;
	outbuf.pos = 0;

	while (outbuf.pos < outbuf.size)
	{
		size_t		ret = ZSTD_decompressStream(dctx, &outbuf, &inbuf);

		if (ZSTD_isError(ret))
		{
			failed = true;
			break;
		}

		/*
		 * A zero return means the frame is fully decoded, and a positive one
		 * means more input is wanted.  Either way, if we could not fill the
		 * output buffer the data must be truncated or corrupt, since the
		 * caller has already established that the value decompresses to more
		 * than slicelength bytes.
		 */
		if (ret == 0 || inbuf.pos == inbuf.size)
		{
			failed = (outbuf.pos < outbuf.size);
			break;
		}
	}

	/* release the context before any possible error is thrown */
	ZSTD_freeDCtx(dctx);

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

		if (VARATT_EXTINFO_IS_COMPRESSED(toast_ext_data.extinfo, toast_ext_data.rawsize))
			cmid = toast_ext_data.compress_method;
	}
	else if (VARATT_IS_COMPRESSED(attr))
		cmid = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(attr);

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
