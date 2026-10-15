/*-------------------------------------------------------------------------
 *
 * toast_dictionary.h
 *	  Lifecycle of compression dictionaries for TOAST.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/access/toast_dictionary.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOAST_DICTIONARY_H
#define TOAST_DICTIONARY_H

#include "utils/relcache.h"
#include "varatt.h"

/*
 * A dictionary-compressed datum stores the dictionary OID right after the
 * method byte of the long compressed header, regardless of which codec
 * produced the payload:
 *
 *	 va_header | va_tcinfo | va_cmid | dictid (4 bytes) | codec payload
 *
 * The method byte (va_cmid) says which codec's payload follows, so the same
 * layout serves zstd, lz4, and any future dictionary-aware codec.
 */
#define VARHDRSZ_COMPRESSED_DICT	(VARHDRSZ_COMPRESSED_LONG + sizeof(Oid))

/*
 * Per-codec dictionary routines.  The catalog, lifecycle, training, and the
 * datum layout above are codec-independent and live in toast_dictionary.c;
 * only these three calls differ per codec.  compress() lays out the header
 * and writes the codec payload after VARHDRSZ_COMPRESSED_DICT; it returns
 * NULL if the value did not get smaller.  decompress() and
 * decompress_slice() read a datum this codec produced.
 */
typedef struct DictCompressRoutine
{
	varlena    *(*compress) (const varlena *value, Oid dictid);
	varlena    *(*decompress) (const varlena *value);
	varlena    *(*decompress_slice) (const varlena *value, int32 slicelength);
} DictCompressRoutine;

/* Current dictionary of rel.attnum, or InvalidOid if none */
extern Oid	toast_dictionary_current(Relation rel, AttrNumber attnum);

/* May this dict-compressed datum be stored as-is in rel.attnum? */
extern bool toast_dictionary_keep(Relation rel, AttrNumber attnum,
								  const varlena *value);

/*
 * Generic dictionary compress/decompress.  The method byte in value (or, for
 * compress, the compression method cmid) selects the codec's routine.
 */
extern varlena *toast_dict_compress_datum(const varlena *value, char cmethod,
										  Oid dictid);
extern varlena *toast_dict_decompress_datum(const varlena *value);
extern varlena *toast_dict_decompress_datum_slice(const varlena *value,
												  int32 slicelength);

extern void RemoveToastDictionaries(Oid relid, AttrNumber attnum);
extern void toast_dictionary_retire(Oid relid);

#endif							/* TOAST_DICTIONARY_H */
