/*-------------------------------------------------------------------------
 *
 * toast_compression.h
 *	  Functions for toast compression.
 *
 * Copyright (c) 2021-2026, PostgreSQL Global Development Group
 *
 * src/include/access/toast_compression.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOAST_COMPRESSION_H
#define TOAST_COMPRESSION_H

#include "varatt.h"

/*
 * GUC support.
 *
 * default_toast_compression is an integer for purposes of the GUC machinery,
 * but the value is one of the char values defined below, as they appear in
 * pg_attribute.attcompression, e.g. TOAST_PGLZ_COMPRESSION.
 */
extern PGDLLIMPORT int default_toast_compression;

/*
 * Built-in compression method ID.  These built-in compression method IDs are
 * directly mapped to the built-in compression methods.
 *
 * A compressed varlena identifies its method in the two high-order bits of
 * its tcinfo/extinfo word.  Only pglz and lz4 are stored there directly; all
 * other methods use the long form of the header, flagged by
 * VARLENA_COMPRESS_METHOD_LONG in those bits, which stores the ID in a byte
 * of its own (see varatt.h) and so leaves room for IDs up to 255.
 * toast_compression_id_needs_cmid_byte() tells which form a given ID uses.
 *
 * TOAST_INVALID_COMPRESSION_ID is not a real compression method and is never
 * stored on disk; it only serves to report "this value is not compressed".
 * It is deliberately equal to VARLENA_COMPRESS_METHOD_LONG, so that code
 * that mistakenly interprets the raw two-bit field of a long-form value as a
 * method ID ends up with an invalid ID rather than a real method.
 *
 * Don't use these values for anything other than understanding the meaning
 * of the raw bits from a varlena; in particular, if the goal is to identify
 * a compression method, use the constants TOAST_PGLZ_COMPRESSION, etc.
 * below.
 */
typedef enum ToastCompressionId
{
	TOAST_PGLZ_COMPRESSION_ID = 0,
	TOAST_LZ4_COMPRESSION_ID = 1,
	TOAST_ZSTD_COMPRESSION_ID = 2,
	TOAST_INVALID_COMPRESSION_ID = 3,
} ToastCompressionId;

StaticAssertDecl(TOAST_INVALID_COMPRESSION_ID == VARLENA_COMPRESS_METHOD_LONG,
				 "TOAST_INVALID_COMPRESSION_ID must match VARLENA_COMPRESS_METHOD_LONG");

/*
 * Does this compression method ID need a byte of its own?
 *
 * Only the two original methods fit in the two method bits of the header;
 * every other method needs the long form of the compressed-in-line header
 * and of the TOAST pointer, which carry the ID in a separate byte.  Not
 * meaningful for TOAST_INVALID_COMPRESSION_ID.
 */
static inline bool
toast_compression_id_needs_cmid_byte(ToastCompressionId cmid)
{
	Assert(cmid != TOAST_INVALID_COMPRESSION_ID);
	return (cmid != TOAST_PGLZ_COMPRESSION_ID &&
			cmid != TOAST_LZ4_COMPRESSION_ID);
}

/*
 * Built-in compression methods.  pg_attribute will store these in the
 * attcompression column.  In attcompression, InvalidCompressionMethod
 * denotes the default behavior.
 */
#define TOAST_PGLZ_COMPRESSION			'p'
#define TOAST_LZ4_COMPRESSION			'l'
#define TOAST_ZSTD_COMPRESSION			'z'
#define InvalidCompressionMethod		'\0'

#define CompressionMethodIsValid(cm)  ((cm) != InvalidCompressionMethod)

/*
 * Choose an appropriate default toast compression method.  If lz4 is
 * compiled-in, use it, otherwise use pglz.
 */
#ifdef USE_LZ4
#define DEFAULT_TOAST_COMPRESSION	TOAST_LZ4_COMPRESSION
#else
#define DEFAULT_TOAST_COMPRESSION	TOAST_PGLZ_COMPRESSION
#endif

/* pglz compression/decompression routines */
extern varlena *pglz_compress_datum(const varlena *value);
extern varlena *pglz_decompress_datum(const varlena *value);
extern varlena *pglz_decompress_datum_slice(const varlena *value,
											int32 slicelength);

/* lz4 compression/decompression routines */
extern varlena *lz4_compress_datum(const varlena *value);
extern varlena *lz4_decompress_datum(const varlena *value);
extern varlena *lz4_decompress_datum_slice(const varlena *value,
										   int32 slicelength);

/* zstd compression/decompression routines */
extern varlena *zstd_compress_datum(const varlena *value);
extern varlena *zstd_decompress_datum(const varlena *value);
extern varlena *zstd_decompress_datum_slice(const varlena *value,
											int32 slicelength);

/* other stuff */
extern ToastCompressionId toast_get_compression_id(varlena *attr);
extern char CompressionNameToMethod(const char *compression);
extern const char *GetCompressionMethodName(char method);

#endif							/* TOAST_COMPRESSION_H */
