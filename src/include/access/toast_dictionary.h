/*-------------------------------------------------------------------------
 *
 * toast_dictionary.h
 *	  Lifecycle of zstd compression dictionaries for TOAST.
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
 * A zstd_dict datum stores the dictionary OID right after the method byte
 * of the long compressed header:
 *
 *	 va_header | va_tcinfo | va_cmid | dictid (4 bytes) | zstd frame
 */
#define VARHDRSZ_COMPRESSED_DICT	(VARHDRSZ_COMPRESSED_LONG + sizeof(Oid))

/* Current dictionary of rel.attnum, or InvalidOid if none */
extern Oid	toast_dictionary_current(Relation rel, AttrNumber attnum);

/* May this zstd_dict datum be stored as-is in rel.attnum? */
extern bool toast_dictionary_keep(Relation rel, AttrNumber attnum,
								  const varlena *value);

extern varlena *zstd_dict_compress_datum(const varlena *value, Oid dictid);
extern varlena *zstd_dict_decompress_datum(const varlena *value);
extern varlena *zstd_dict_decompress_datum_slice(const varlena *value,
												 int32 slicelength);

extern void RemoveToastDictionaries(Oid relid, AttrNumber attnum);
extern void toast_dictionary_retire(Oid relid);

#endif							/* TOAST_DICTIONARY_H */
