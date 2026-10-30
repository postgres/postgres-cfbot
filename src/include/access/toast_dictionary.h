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

#include "access/htup.h"
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
 * only these calls differ per codec.  compress() lays out the header and
 * writes the codec payload after VARHDRSZ_COMPRESSED_DICT; it returns NULL if
 * the value did not get smaller.  decompress() and decompress_slice() read a
 * datum this codec produced.
 *
 * train() and compressed_total() exist only for automatic dictionary
 * evolution (see toast_dictionary_maybe_evolve()).  train() builds a
 * dictionary of at most max_dict_bytes from nsamples concatenated samples
 * (sizes[] gives each one's length), writing it into dictbuf and returning
 * its length, or 0 on failure (a decline, not an error).  compressed_total()
 * compresses each of nvals concatenated values (sizes[] gives each one's
 * length) against raw dictionary bytes and returns the total compressed byte
 * count without storing anything, or 0 if any value fails, so the evolution
 * policy can weigh a candidate dictionary against the current one with one
 * call per dictionary.  Both are codec contract points only: the policy never
 * interprets the dictionary bytes.
 */
typedef struct DictCompressRoutine
{
	varlena    *(*compress) (const varlena *value, Oid dictid);
	varlena    *(*decompress) (const varlena *value);
	varlena    *(*decompress_slice) (const varlena *value, int32 slicelength);
	size_t		(*train) (const char *samples, const size_t *sizes,
						  int nsamples, size_t max_dict_bytes, void *dictbuf);
	size_t		(*compressed_total) (const char *dict, size_t dictlen,
									 const char *vals, const size_t *sizes,
									 int nvals);
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

/*
 * Automatic dictionary evolution, called from ANALYZE for opted-in columns
 * that use a dictionary method.  Reuses ANALYZE's already-fetched sample
 * rows; trains a candidate, runs a held-out size test, and installs it only
 * on a margin win, all in the maintenance process.  A no-op for every other
 * column.
 */
extern void toast_dictionary_maybe_evolve(Relation onerel, AttrNumber attnum,
										  HeapTuple *rows, int numrows,
										  double totalrows);

/* GUCs controlling automatic dictionary evolution; see toast_dictionary.c */
extern PGDLLIMPORT double dictionary_evolution_min_gain;
extern PGDLLIMPORT int	dictionary_evolution_min_interval;
extern PGDLLIMPORT int	dictionary_evolution_max_retained;

#endif							/* TOAST_DICTIONARY_H */
