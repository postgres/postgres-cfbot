/*-------------------------------------------------------------------------
 *
 * gzip.h
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/include/fe_utils/gzip.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FE_UTILS_GZIP_H
#define FE_UTILS_GZIP_H

#ifdef HAVE_LIBZ
#include <zlib.h>

typedef enum
{
	GZIP_DECOMPRESS_MORE,
	GZIP_DECOMPRESS_DONE,
	GZIP_DECOMPRESS_ERROR
} GzipDecompressResult;

typedef enum
{
	GZIP_STREAM_NEW,
	GZIP_MEMBER_INCOMPLETE,
	GZIP_MEMBER_COMPLETE
} GzipDecompressState;

typedef struct GzipDecompressor
{
	z_stream	stream;
	GzipDecompressState state;
	const char *error;
} GzipDecompressor;

/*
 * Errors are returned to the caller and described in decoder->error.
 * After GZIP_DECOMPRESS_ERROR, callers must stop decoding and release the
 * decoder with gzip_decompressor_end().  Only a successfully initialized
 * decoder may be passed to gzip_decompressor_end().
 *
 * gzip_decompressor_end() releases resources without checking completeness.
 * To validate the stream, call gzip_decompressor_read() with eof set to true
 * until it returns GZIP_DECOMPRESS_DONE (or GZIP_DECOMPRESS_ERROR).
 */
extern bool gzip_decompressor_init(GzipDecompressor *decoder);
extern GzipDecompressResult gzip_decompressor_read(GzipDecompressor *decoder,
												   const void *input, size_t input_size,
												   void *output, size_t output_size,
												   bool eof, size_t *consumed, size_t *produced);
extern bool gzip_decompressor_end(GzipDecompressor *decoder);
#endif

#endif							/* FE_UTILS_GZIP_H */
