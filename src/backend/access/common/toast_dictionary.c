/*-------------------------------------------------------------------------
 *
 * toast_dictionary.c
 *	  Lifecycle of zstd compression dictionaries for TOAST.
 *
 * A dictionary-compressed datum cannot be read without its dictionary, so the
 * hard part of dictionary support is knowing when a dictionary may be
 * dropped.  Designs discussed on pgsql-hackers either kept every dictionary
 * forever or tracked, through pg_depend, every table a compressed value might
 * have been copied into; both were rejected.  This file keeps
 * dictionary-compressed values inside the column that owns their dictionary
 * instead, which makes the lifecycle local to that column:
 *
 * Ownership.  A pg_toast_dictionary row belongs to one (relid, attnum), like
 * a pg_statistic row.  DROP TABLE and DROP COLUMN delete it through
 * RemoveToastDictionaries(); nothing else ever references it.
 *
 * Containment.  heap_fill_tuple() sets HEAP_HASEXTERNAL for a tuple holding
 * a zstd_dict datum, as it does for TOAST pointers, so every path that
 * already makes a tuple self-contained before it outlives its source
 * (composite datums, tuplestores, holdable cursors, syscache entries,
 * PL/pgSQL records) also reaches the datum.  detoast_external_attr() then
 * decompresses it, exactly as it fetches a TOAST pointer.  When a datum is
 * stored into a heap table, toast_tuple_init() keeps it compressed only if
 * the target column owns its dictionary (toast_dictionary_keep()), and
 * decompresses it otherwise, so it is recompressed, if at all, with the
 * target column's own method.  The same applies to index and BRIN tuples,
 * which inline external values already.
 *
 * Retirement.  Training a dictionary marks the previous one of the column
 * non-current.  A non-current dictionary stays readable until the table is
 * rewritten (VACUUM FULL, CLUSTER, REPACK, rewriting ALTER TABLE): during the
 * rewrite toast_dictionary_keep() rejects it, so every value using it is
 * recompressed with the current one, and finish_heap_swap() then deletes it
 * with toast_dictionary_retire().
 *
 * The cost is only paid when a dictionary-compressed value crosses a column
 * boundary.  Copies of values compressed with any other method are unchanged.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_dictionary.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef USE_ZSTD
#include <zstd.h>
#include <zdict.h>
#endif

#ifdef USE_LZ4
#include <lz4.h>
#endif

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/toast_dictionary.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_toast_dictionary.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/attoptcache.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#define NO_ZSTD_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("compression method %s not supported", "zstd_dict"), \
			 errdetail("This functionality requires the server to be built with %s support.", "zstd")))

#define NO_LZ4_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("compression method %s not supported", "lz4_dict"), \
			 errdetail("This functionality requires the server to be built with %s support.", "lz4")))

#define DICT_MIN_SIZE		256
#define DICT_MAX_SIZE		(1024 * 1024)
#define DICT_DEFAULT_SIZE	(64 * 1024)	/* pg_train_toast_dictionary default */
#define DICT_MAX_SAMPLES	10000
#define DICT_SAMPLE_LIMIT	(128 * 1024)
/* zstd recommends samples totalling about 100 times the dictionary size */
#define DICT_SAMPLE_FACTOR	100

/*
 * GUCs for automatic dictionary evolution.  Codec-agnostic policy knobs; see
 * toast_dictionary_maybe_evolve().  Declared in toast_dictionary.h.
 */
double		dictionary_evolution_min_gain = 0.05;
int			dictionary_evolution_min_interval = 86400;
int			dictionary_evolution_max_retained = 8;

/*
 * Train a dictionary with zstd's ZDICT trainer.  Shared by both codecs:
 * lz4_dict dictionaries are also trained by ZDICT (lz4 ships no trainer of
 * its own), so lz4_dict_routine.train points here too.  Returns the trained
 * dictionary's length, written into dictbuf (which must hold max_dict_bytes),
 * or 0 on failure -- a decline to retrain, not an error.  When the server is
 * built without zstd, training is unavailable for both codecs and this
 * returns 0.
 */
static size_t
dict_train_zdict(const char *samples, const size_t *sizes, int nsamples,
				 size_t max_dict_bytes, void *dictbuf)
{
#ifndef USE_ZSTD
	return 0;
#else
	size_t		dictlen;

	if (nsamples == 0)
		return 0;
	dictlen = ZDICT_trainFromBuffer(dictbuf, max_dict_bytes, samples, sizes,
									nsamples);
	if (ZDICT_isError(dictlen))
		return 0;
	return dictlen;
#endif
}

/*
 * The relation owning the dictionaries of rel's columns.  During a table
 * rewrite, values are written into a transient heap that stands for the
 * table being rewritten.
 */
static Oid
dict_owner_relid(Relation rel)
{
	if (OidIsValid(rel->rd_rel->relrewrite))
		return rel->rd_rel->relrewrite;
	return RelationGetRelid(rel);
}

/* Does this compression method char use a TOAST dictionary? */
static inline bool
dict_method(char cmethod)
{
	return (cmethod == TOAST_ZSTD_DICT_COMPRESSION ||
			cmethod == TOAST_LZ4_DICT_COMPRESSION);
}

/*
 * Does relid.attnum (still) use zstd_dict?  A dropped column does not: a
 * rewrite has replaced its values with nulls.
 */
static bool
dict_column_uses_dict(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp;
	Form_pg_attribute att;
	bool		result;

	tp = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tp))
		return false;
	att = (Form_pg_attribute) GETSTRUCT(tp);
	result = !att->attisdropped && dict_method(att->attcompression);
	ReleaseSysCache(tp);
	return result;
}

/* Look up one dictionary row of rel.attnum; current_only selects tdcurrent */
static HeapTuple
dict_lookup(Oid relid, AttrNumber attnum, bool current_only, Relation drel,
			SysScanDesc *scanp)
{
	ScanKeyData key[2];
	HeapTuple	tup;

	ScanKeyInit(&key[0], Anum_pg_toast_dictionary_tdrelid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1], Anum_pg_toast_dictionary_tdattnum,
				BTEqualStrategyNumber, F_INT2EQ, Int16GetDatum(attnum));
	*scanp = systable_beginscan(drel, ToastDictionaryRelidAttnumIndexId, true,
								NULL, attnum != 0 ? 2 : 1, key);
	while (HeapTupleIsValid(tup = systable_getnext(*scanp)))
	{
		if (!current_only ||
			((Form_pg_toast_dictionary) GETSTRUCT(tup))->tdcurrent)
			return tup;
	}
	return NULL;
}

/*
 * Current dictionary of a column, or InvalidOid.
 *
 * Called once per compressed value on the write path, so the result is cached
 * in the relcache entry: a single-entry cache keyed by attnum, filled lazily
 * from the catalog here.  dict_install() changes the current dictionary only
 * at maintenance time and then issues CacheInvalidateRelcacheByRelid(), which
 * rebuilds this relcache entry and so empties the cache; the next call
 * repopulates it with the new OID.  A single entry suffices for a scan of one
 * column (the common case); a second dictionary column just re-probes.
 */
Oid
toast_dictionary_current(Relation rel, AttrNumber attnum)
{
	Relation	drel;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			result = InvalidOid;

	if (rel->rd_toastdictattnum == attnum)
		return rel->rd_toastdictoid;

	drel = table_open(ToastDictionaryRelationId, AccessShareLock);
	tup = dict_lookup(dict_owner_relid(rel), attnum, true, drel, &scan);
	if (tup)
		result = ((Form_pg_toast_dictionary) GETSTRUCT(tup))->oid;
	systable_endscan(scan);
	table_close(drel, AccessShareLock);

	rel->rd_toastdictoid = result;
	rel->rd_toastdictattnum = attnum;
	return result;
}

/* Dictionary OID of a compressed-in-line zstd_dict datum */
static Oid
dict_datum_get_id(const varlena *value)
{
	Oid			dictid;

	if (VARSIZE(value) < VARHDRSZ_COMPRESSED_DICT)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt")));
	memcpy(&dictid, (const char *) value + VARHDRSZ_COMPRESSED_LONG,
		   sizeof(dictid));
	return dictid;
}

/*
 * May this compressed-in-line zstd_dict datum be stored as-is into
 * rel.attnum?  Only if that column owns the dictionary, the dictionary is
 * still current, and the column still uses zstd_dict.  Anything else is
 * recompressed whenever it is written, which is what lets a table rewrite
 * retire dictionaries.
 */
bool
toast_dictionary_keep(Relation rel, AttrNumber attnum, const varlena *value)
{
	HeapTuple	tup;
	bool		result = false;
	Oid			relid = dict_owner_relid(rel);

	if (!dict_method(TupleDescAttr(RelationGetDescr(rel), attnum - 1)->attcompression))
		return false;

	tup = SearchSysCache1(TOASTDICTOID,
						  ObjectIdGetDatum(dict_datum_get_id(value)));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_toast_dictionary d = (Form_pg_toast_dictionary) GETSTRUCT(tup);

		result = (d->tdrelid == relid && d->tdattnum == attnum &&
				  d->tdcurrent);
		ReleaseSysCache(tup);
	}
	return result;
}

/*
 * The per-backend dictionary caches below hold DICT_CACHE_SIZE entries each,
 * keyed by dictionary OID, least recently used out.  More than one entry
 * matters whenever reads alternate between dictionaries: two dictionary
 * columns in one row, or one column retrained without a rewrite, whose old
 * and new values sit side by side.  With a single entry each such switch
 * re-fetches the dictionary and, for zstd, digests it again.
 */
#if defined(USE_ZSTD) || defined(USE_LZ4)
#define DICT_CACHE_SIZE 4

static uint64 dict_cache_clock = 0;

/*
 * Return the slot of dictid in ids[], or -1.  Marks a hit as just used.
 */
static int
dict_cache_lookup(Oid *ids, uint64 *used, Oid dictid)
{
	for (int i = 0; i < DICT_CACHE_SIZE; i++)
	{
		if (ids[i] == dictid)
		{
			used[i] = ++dict_cache_clock;
			return i;
		}
	}
	return -1;
}

/*
 * Pick the slot for a new entry: an empty one, else the least recently used.
 * Call it only after dict_fetch(), which may run invalidation callbacks that
 * empty the cache.
 */
static int
dict_cache_victim(Oid *ids, uint64 *used)
{
	int			victim = 0;

	for (int i = 0; i < DICT_CACHE_SIZE; i++)
	{
		if (!OidIsValid(ids[i]))
			return i;
		if (used[i] < used[victim])
			victim = i;
	}
	return victim;
}
#endif

#ifdef USE_ZSTD
/*
 * Per-backend cache of digested dictionaries.  Digesting builds the entropy
 * tables and costs far more than one small value's compression, so the
 * ZSTD_CDict/ZSTD_DDict objects are kept and dropped on invalidation of
 * pg_toast_dictionary.
 *
 * DICT_CACHE_SIZE entries per direction.
 */
static Oid	cdict_ids[DICT_CACHE_SIZE];
static uint64 cdict_used[DICT_CACHE_SIZE];
static ZSTD_CDict *cdicts[DICT_CACHE_SIZE];
static Oid	ddict_ids[DICT_CACHE_SIZE];
static uint64 ddict_used[DICT_CACHE_SIZE];
static ZSTD_DDict *ddicts[DICT_CACHE_SIZE];
static ZSTD_CCtx *dict_cctx = NULL;
static ZSTD_DCtx *dict_dctx = NULL;

static void
dict_cache_invalidate(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	for (int i = 0; i < DICT_CACHE_SIZE; i++)
	{
		if (cdicts[i])
			ZSTD_freeCDict(cdicts[i]);
		if (ddicts[i])
			ZSTD_freeDDict(ddicts[i]);
		cdicts[i] = NULL;
		ddicts[i] = NULL;
		cdict_ids[i] = ddict_ids[i] = InvalidOid;
	}
}
#endif							/* USE_ZSTD */

#if defined(USE_ZSTD) || defined(USE_LZ4)
/*
 * Fetch a dictionary's bytes from the catalog.  Used by both the zstd and
 * the lz4 codecs, so it must not depend on USE_ZSTD alone.
 */
static bytea *
dict_fetch(Oid dictid)
{
	HeapTuple	tup;
	bytea	   *result;

#ifdef USE_ZSTD
	static bool registered = false;

	if (!registered)
	{
		CacheRegisterSyscacheCallback(TOASTDICTOID, dict_cache_invalidate,
									  (Datum) 0);
		registered = true;
	}
#endif

	tup = SearchSysCache1(TOASTDICTOID, ObjectIdGetDatum(dictid));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("compression dictionary %u does not exist", dictid)));
	result = DatumGetByteaPCopy(SysCacheGetAttrNotNull(TOASTDICTOID, tup,
													   Anum_pg_toast_dictionary_tddict));
	ReleaseSysCache(tup);
	return result;
}
#endif

#ifdef USE_ZSTD
static ZSTD_CDict *
dict_get_cdict(Oid dictid)
{
	int			slot = dict_cache_lookup(cdict_ids, cdict_used, dictid);

	if (slot < 0)
	{
		bytea	   *d = dict_fetch(dictid);

		slot = dict_cache_victim(cdict_ids, cdict_used);
		if (cdicts[slot])
			ZSTD_freeCDict(cdicts[slot]);
		cdict_ids[slot] = InvalidOid;
		cdicts[slot] = ZSTD_createCDict(VARDATA_ANY(d), VARSIZE_ANY_EXHDR(d),
										ZSTD_CLEVEL_DEFAULT);
		pfree(d);
		if (cdicts[slot] == NULL)
			elog(ERROR, "could not load compression dictionary %u", dictid);
		cdict_ids[slot] = dictid;
		cdict_used[slot] = ++dict_cache_clock;
	}
	if (dict_cctx == NULL && (dict_cctx = ZSTD_createCCtx()) == NULL)
		elog(ERROR, "could not create zstd compression context");
	return cdicts[slot];
}

static ZSTD_DDict *
dict_get_ddict(Oid dictid)
{
	int			slot = dict_cache_lookup(ddict_ids, ddict_used, dictid);

	if (slot < 0)
	{
		bytea	   *d = dict_fetch(dictid);

		slot = dict_cache_victim(ddict_ids, ddict_used);
		if (ddicts[slot])
			ZSTD_freeDDict(ddicts[slot]);
		ddict_ids[slot] = InvalidOid;
		ddicts[slot] = ZSTD_createDDict(VARDATA_ANY(d), VARSIZE_ANY_EXHDR(d));
		pfree(d);
		if (ddicts[slot] == NULL)
			elog(ERROR, "could not load compression dictionary %u", dictid);
		ddict_ids[slot] = dictid;
		ddict_used[slot] = ++dict_cache_clock;
	}
	if (dict_dctx == NULL && (dict_dctx = ZSTD_createDCtx()) == NULL)
		elog(ERROR, "could not create zstd decompression context");
	return ddicts[slot];
}
#endif							/* USE_ZSTD */

/*
 * Compress with the given dictionary.  The datum is laid out with
 * VARHDRSZ_COMPRESSED_DICT bytes of header; toast_compress_datum() fills in
 * va_tcinfo and the method byte.
 */
static varlena *
zstd_dict_compress_datum(const varlena *value, Oid dictid)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
	return NULL;
#else
	int32		valsize = VARSIZE_ANY_EXHDR(value);
	size_t		max_size = ZSTD_compressBound(valsize);
	ZSTD_CDict *cd = dict_get_cdict(dictid);
	varlena    *tmp;
	size_t		len;

	tmp = (varlena *) palloc(max_size + VARHDRSZ_COMPRESSED_DICT);
	len = ZSTD_compress_usingCDict(dict_cctx,
								   (char *) tmp + VARHDRSZ_COMPRESSED_DICT,
								   max_size, VARDATA_ANY(value), valsize, cd);
	if (ZSTD_isError(len))
		elog(ERROR, "zstd compression failed: %s", ZSTD_getErrorName(len));
	if (len > (size_t) valsize)
	{
		pfree(tmp);
		return NULL;
	}
	memcpy((char *) tmp + VARHDRSZ_COMPRESSED_LONG, &dictid, sizeof(dictid));
	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED_DICT);
	return tmp;
#endif
}

static varlena *
zstd_dict_decompress_datum(const varlena *value)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
	return NULL;
#else
	Oid			dictid = dict_datum_get_id(value);
	uint32		extsize = VARDATA_COMPRESSED_GET_EXTSIZE(value);
	ZSTD_DDict *dd = dict_get_ddict(dictid);
	varlena    *result;
	size_t		rawsize;

	result = (varlena *) palloc(extsize + VARHDRSZ);
	rawsize = ZSTD_decompress_usingDDict(dict_dctx, VARDATA(result), extsize,
										 (const char *) value + VARHDRSZ_COMPRESSED_DICT,
										 VARSIZE(value) - VARHDRSZ_COMPRESSED_DICT,
										 dd);
	if (ZSTD_isError(rawsize) || rawsize != extsize)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed zstd data is corrupt")));
	SET_VARSIZE(result, rawsize + VARHDRSZ);
	return result;
#endif
}

/*
 * Prefix decompression.  For values below zstd's 128kB block size a prefix
 * costs a full decode anyway (see zstd_decompress_datum_slice).
 *
 * XXX: larger values are decoded in full too.  If prefixes of large
 * dictionary-compressed values matter, use the streaming loop of
 * zstd_decompress_datum_slice() with ZSTD_DCtx_refDDict().  That loop stops
 * in the middle of a frame, so dict_dctx must then be reset with
 * ZSTD_DCtx_reset(ZSTD_reset_session_only) before each use, as
 * zstd_get_dctx() does for plain zstd; without it the next value's decoding
 * continues the previous frame.  The one-shot call used now starts a new
 * frame each time and needs no reset.
 */
static varlena *
zstd_dict_decompress_datum_slice(const varlena *value, int32 slicelength)
{
	varlena    *result = zstd_dict_decompress_datum(value);

	if (VARSIZE(result) - VARHDRSZ > slicelength)
		SET_VARSIZE(result, slicelength + VARHDRSZ);
	return result;
}

/*
 * Total compressed size of the held-out values against raw dictionary bytes,
 * for the held-out test of dictionary evolution.  Digests the bytes into a
 * one-shot CDict once for all the values (not the per-backend digested cache,
 * which keys on OID and a candidate has no OID yet) and returns the total
 * compressed byte count without storing anything.  Returns 0 on any zstd
 * failure so the caller can treat it as "no estimate."
 */
static size_t
zstd_dict_compressed_total(const char *dict, size_t dictlen,
						   const char *vals, const size_t *sizes, int nvals)
{
#ifndef USE_ZSTD
	return 0;
#else
	ZSTD_CDict *cd;
	size_t		total = 0;
	size_t		maxlen = 0;
	char	   *out;

	for (int i = 0; i < nvals; i++)
		maxlen = Max(maxlen, sizes[i]);
	if (dict_cctx == NULL && (dict_cctx = ZSTD_createCCtx()) == NULL)
		elog(ERROR, "could not create zstd compression context");
	cd = ZSTD_createCDict(dict, dictlen, ZSTD_CLEVEL_DEFAULT);
	if (cd == NULL)
		return 0;
	out = palloc(ZSTD_compressBound(maxlen));
	for (int i = 0; i < nvals; i++)
	{
		size_t		clen = ZSTD_compress_usingCDict(dict_cctx, out,
													ZSTD_compressBound(maxlen),
													vals, sizes[i], cd);

		if (ZSTD_isError(clen))
		{
			total = 0;
			break;
		}
		total += clen;
		vals += sizes[i];
	}
	ZSTD_freeCDict(cd);
	pfree(out);
	return total;
#endif
}

#ifdef USE_LZ4
/*
 * Per-backend cache of the raw dictionary bytes most recently used, keyed by
 * OID, DICT_CACHE_SIZE entries.  Unlike zstd, lz4 has no digested dictionary object: LZ4_loadDict()
 * and LZ4_decompress_safe_usingDict() take the dictionary bytes directly, so
 * caching the bytea is all that is needed.  Dropped on invalidation of
 * pg_toast_dictionary.
 */
static Oid	lz4dict_ids[DICT_CACHE_SIZE];
static uint64 lz4dict_used[DICT_CACHE_SIZE];
static bytea *lz4dicts[DICT_CACHE_SIZE];

static void
lz4_dict_cache_invalidate(Datum arg, SysCacheIdentifier cacheid, uint32 hashvalue)
{
	for (int i = 0; i < DICT_CACHE_SIZE; i++)
	{
		if (lz4dicts[i])
			pfree(lz4dicts[i]);
		lz4dicts[i] = NULL;
		lz4dict_ids[i] = InvalidOid;
	}
}

static bytea *
lz4_dict_get(Oid dictid)
{
	static bool registered = false;
	int			slot;

	if (!registered)
	{
		CacheRegisterSyscacheCallback(TOASTDICTOID, lz4_dict_cache_invalidate,
									  (Datum) 0);
		registered = true;
	}
	slot = dict_cache_lookup(lz4dict_ids, lz4dict_used, dictid);
	if (slot < 0)
	{
		bytea	   *d = dict_fetch(dictid);
		bytea	   *cached;

		/*
		 * dict_fetch() allocates in the caller's (often short-lived) context,
		 * but this cache outlives one call, so keep a copy in
		 * CacheMemoryContext.  Unlike zstd, lz4 has no digested object to hold
		 * instead, so the bytes themselves are the cache.
		 */
		cached = (bytea *) MemoryContextAlloc(CacheMemoryContext, VARSIZE(d));
		memcpy(cached, d, VARSIZE(d));
		pfree(d);
		slot = dict_cache_victim(lz4dict_ids, lz4dict_used);
		if (lz4dicts[slot])
			pfree(lz4dicts[slot]);
		lz4dicts[slot] = cached;
		lz4dict_ids[slot] = dictid;
		lz4dict_used[slot] = ++dict_cache_clock;
	}
	return lz4dicts[slot];
}
#endif							/* USE_LZ4 */

/*
 * Compress with the given dictionary using lz4.  Layout matches
 * zstd_dict_compress_datum(): VARHDRSZ_COMPRESSED_DICT header then the lz4
 * block.  The uncompressed size is recovered from va_tcinfo at read time, so
 * lz4's block format (which does not store it) is enough.
 */
static varlena *
lz4_dict_compress_datum(const varlena *value, Oid dictid)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
	return NULL;
#else
	int32		valsize = VARSIZE_ANY_EXHDR(value);
	int32		max_size = LZ4_compressBound(valsize);
	bytea	   *dict = lz4_dict_get(dictid);
	LZ4_stream_t *stream;
	varlena    *tmp;
	int32		len;

	stream = LZ4_createStream();
	if (stream == NULL)
		elog(ERROR, "could not create lz4 stream");
	LZ4_loadDict(stream, VARDATA_ANY(dict), VARSIZE_ANY_EXHDR(dict));

	tmp = (varlena *) palloc(max_size + VARHDRSZ_COMPRESSED_DICT);
	len = LZ4_compress_fast_continue(stream,
									 VARDATA_ANY(value),
									 (char *) tmp + VARHDRSZ_COMPRESSED_DICT,
									 valsize, max_size, 1);
	LZ4_freeStream(stream);
	if (len <= 0)
		elog(ERROR, "lz4 compression failed");
	if (len > valsize)
	{
		pfree(tmp);
		return NULL;
	}
	memcpy((char *) tmp + VARHDRSZ_COMPRESSED_LONG, &dictid, sizeof(dictid));
	SET_VARSIZE_COMPRESSED(tmp, len + VARHDRSZ_COMPRESSED_DICT);
	return tmp;
#endif
}

static varlena *
lz4_dict_decompress_datum(const varlena *value)
{
#ifndef USE_LZ4
	NO_LZ4_SUPPORT();
	return NULL;
#else
	Oid			dictid = dict_datum_get_id(value);
	uint32		extsize = VARDATA_COMPRESSED_GET_EXTSIZE(value);
	bytea	   *dict = lz4_dict_get(dictid);
	varlena    *result;
	int32		rawsize;

	result = (varlena *) palloc(extsize + VARHDRSZ);
	rawsize = LZ4_decompress_safe_usingDict((const char *) value + VARHDRSZ_COMPRESSED_DICT,
											VARDATA(result),
											VARSIZE(value) - VARHDRSZ_COMPRESSED_DICT,
											extsize,
											VARDATA_ANY(dict),
											VARSIZE_ANY_EXHDR(dict));
	if (rawsize < 0 || (uint32) rawsize != extsize)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("compressed lz4 data is corrupt")));
	SET_VARSIZE(result, rawsize + VARHDRSZ);
	return result;
#endif
}

/*
 * Prefix decompression.  lz4's block API cannot stop early, so a prefix of a
 * dictionary-compressed value decodes the whole value and then truncates,
 * exactly as the non-dictionary lz4 slice path does for values whose true
 * size exceeds the slice.
 */
static varlena *
lz4_dict_decompress_datum_slice(const varlena *value, int32 slicelength)
{
	varlena    *result = lz4_dict_decompress_datum(value);

	if (VARSIZE(result) - VARHDRSZ > slicelength)
		SET_VARSIZE(result, slicelength + VARHDRSZ);
	return result;
}

/*
 * Total compressed size of the held-out values against raw dictionary bytes,
 * for the held-out test of dictionary evolution.  Loads the bytes into a fresh
 * lz4 stream for each value, as compression does, and returns the total
 * compressed byte count without storing anything.  Returns 0 on any lz4
 * failure.
 */
static size_t
lz4_dict_compressed_total(const char *dict, size_t dictlen,
						  const char *vals, const size_t *sizes, int nvals)
{
#ifndef USE_LZ4
	return 0;
#else
	size_t		total = 0;

	for (int i = 0; i < nvals; i++)
	{
		LZ4_stream_t *stream;
		int32		bound = LZ4_compressBound(sizes[i]);
		char	   *out;
		int32		clen;

		if (bound <= 0)
			return 0;
		stream = LZ4_createStream();
		if (stream == NULL)
			return 0;
		LZ4_loadDict(stream, dict, dictlen);
		out = palloc(bound);
		clen = LZ4_compress_fast_continue(stream, vals, out, sizes[i], bound, 1);
		LZ4_freeStream(stream);
		pfree(out);
		if (clen <= 0)
			return 0;
		total += clen;
		vals += sizes[i];
	}
	return total;
#endif
}

/*
 * The lz4 dictionary codec.  See DictCompressRoutine in toast_dictionary.h.
 */
static const DictCompressRoutine lz4_dict_routine = {
	.compress = lz4_dict_compress_datum,
	.decompress = lz4_dict_decompress_datum,
	.decompress_slice = lz4_dict_decompress_datum_slice,
	.train = dict_train_zdict,
	.compressed_total = lz4_dict_compressed_total,
};

/*
 * The zstd dictionary codec.  See DictCompressRoutine in
 * toast_dictionary.h.
 */
static const DictCompressRoutine zstd_dict_routine = {
	.compress = zstd_dict_compress_datum,
	.decompress = zstd_dict_decompress_datum,
	.decompress_slice = zstd_dict_decompress_datum_slice,
	.train = dict_train_zdict,
	.compressed_total = zstd_dict_compressed_total,
};

/*
 * Pick the dictionary codec for a compression method char (for compress) or
 * for the method byte already in a datum (for decompress).  Adding a codec is
 * one case here plus its DictCompressRoutine.
 */
static const DictCompressRoutine *
dict_routine_for_method(char cmethod)
{
	switch (cmethod)
	{
		case TOAST_ZSTD_DICT_COMPRESSION:
			return &zstd_dict_routine;
		case TOAST_LZ4_DICT_COMPRESSION:
			return &lz4_dict_routine;
		default:
			elog(ERROR, "invalid dictionary compression method %d", cmethod);
			return NULL;		/* keep compiler quiet */
	}
}

static const DictCompressRoutine *
dict_routine_for_id(ToastCompressionId cmid)
{
	switch (cmid)
	{
		case TOAST_ZSTD_DICT_COMPRESSION_ID:
			return &zstd_dict_routine;
		case TOAST_LZ4_DICT_COMPRESSION_ID:
			return &lz4_dict_routine;
		default:
			elog(ERROR, "invalid dictionary compression method id %d", cmid);
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * Compress value with dictid using the codec named by cmethod, laying out the
 * dictionary header.  Returns NULL if the value did not get smaller.
 */
varlena *
toast_dict_compress_datum(const varlena *value, char cmethod, Oid dictid)
{
	return dict_routine_for_method(cmethod)->compress(value, dictid);
}

/* Decompress a dictionary-compressed datum, dispatching on its method byte. */
varlena *
toast_dict_decompress_datum(const varlena *value)
{
	ToastCompressionId cmid = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(value);

	return dict_routine_for_id(cmid)->decompress(value);
}

varlena *
toast_dict_decompress_datum_slice(const varlena *value, int32 slicelength)
{
	ToastCompressionId cmid = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(value);

	return dict_routine_for_id(cmid)->decompress_slice(value, slicelength);
}

/*
 * Delete the dictionaries of a relation (attnum == 0) or of one column, next
 * to RemoveStatistics().  The values that used them go away at the same time.
 */
void
RemoveToastDictionaries(Oid relid, AttrNumber attnum)
{
	Relation	drel;
	SysScanDesc scan;
	HeapTuple	tup;

	drel = table_open(ToastDictionaryRelationId, RowExclusiveLock);
	tup = dict_lookup(relid, attnum, false, drel, &scan);
	for (; HeapTupleIsValid(tup); tup = systable_getnext(scan))
		CatalogTupleDelete(drel, &tup->t_self);
	systable_endscan(scan);
	table_close(drel, RowExclusiveLock);
}

/*
 * After relid has been rewritten, no stored value references a dictionary
 * that toast_dictionary_keep() rejects: non-current ones, and all of those
 * of a column that no longer uses zstd_dict.  The rewrite recompressed those
 * values, so delete such dictionaries.  Caller holds AccessExclusiveLock on
 * relid.
 */
void
toast_dictionary_retire(Oid relid)
{
	Relation	drel;
	SysScanDesc scan;
	HeapTuple	tup;

	drel = table_open(ToastDictionaryRelationId, RowExclusiveLock);
	tup = dict_lookup(relid, 0, false, drel, &scan);
	for (; HeapTupleIsValid(tup); tup = systable_getnext(scan))
	{
		Form_pg_toast_dictionary d = (Form_pg_toast_dictionary) GETSTRUCT(tup);

		if (!d->tdcurrent || !dict_column_uses_dict(relid, d->tdattnum))
			CatalogTupleDelete(drel, &tup->t_self);
	}
	systable_endscan(scan);
	table_close(drel, RowExclusiveLock);
}

/*
 * Insert a dictionary for rel.attnum and make it current, retiring the
 * previous current one.  dictid may be given (binary upgrade) or chosen here.
 * trainrows is the reltuples estimate at training time, recorded for the
 * auto-evolution growth trigger; pass 0 when not training from a live count.
 */
static Oid
dict_install(Relation rel, AttrNumber attnum, Oid dictid, bool current,
			 int64 trainrows, const void *dict, size_t dictlen)
{
	Relation	drel;
	SysScanDesc scan;
	HeapTuple	tup;
	Datum		values[Natts_pg_toast_dictionary];
	bool		nulls[Natts_pg_toast_dictionary] = {0};
	bytea	   *dictval;

	drel = table_open(ToastDictionaryRelationId, RowExclusiveLock);

	if (current)
	{
		tup = dict_lookup(RelationGetRelid(rel), attnum, true, drel, &scan);
		if (tup)
		{
			HeapTuple	newtup = heap_copytuple(tup);

			((Form_pg_toast_dictionary) GETSTRUCT(newtup))->tdcurrent = false;
			CatalogTupleUpdate(drel, &newtup->t_self, newtup);
			heap_freetuple(newtup);
		}
		systable_endscan(scan);
	}

	if (!OidIsValid(dictid))
		dictid = GetNewOidWithIndex(drel, ToastDictionaryOidIndexId,
									Anum_pg_toast_dictionary_oid);
	dictval = (bytea *) palloc(dictlen + VARHDRSZ);
	SET_VARSIZE(dictval, dictlen + VARHDRSZ);
	memcpy(VARDATA(dictval), dict, dictlen);

	values[Anum_pg_toast_dictionary_oid - 1] = ObjectIdGetDatum(dictid);
	values[Anum_pg_toast_dictionary_tdrelid - 1] =
		ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_toast_dictionary_tdattnum - 1] = Int16GetDatum(attnum);
	values[Anum_pg_toast_dictionary_tdcurrent - 1] = BoolGetDatum(current);
	values[Anum_pg_toast_dictionary_tdtrainrows - 1] = Int64GetDatum(trainrows);
	values[Anum_pg_toast_dictionary_tdtraintime - 1] =
		TimestampTzGetDatum(GetCurrentTimestamp());
	values[Anum_pg_toast_dictionary_tddict - 1] = PointerGetDatum(dictval);
	tup = heap_form_tuple(RelationGetDescr(drel), values, nulls);
	CatalogTupleInsert(drel, tup);
	heap_freetuple(tup);
	table_close(drel, RowExclusiveLock);

	/*
	 * The current dictionary of this column changed.  Invalidate the owning
	 * relation's relcache entry so backends drop the cached current-dictionary
	 * OID (toast_dictionary_current()) and pick up the new one.
	 */
	CacheInvalidateRelcacheByRelid(RelationGetRelid(rel));

	/* let a later call in the same command see this one's changes */
	CommandCounterIncrement();

	return dictid;
}

#ifdef USE_ZSTD
/* Checks for pg_train_toast_dictionary(); returns attnum */
static AttrNumber
dict_check_column(Relation rel, const char *colname)
{
	AttrNumber	attnum;
	Form_pg_attribute att;

	if (!object_ownercheck(RelationRelationId, RelationGetRelid(rel),
						   GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER,
					   get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or materialized view",
						RelationGetRelationName(rel))));
	if (IsCatalogRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use compression dictionaries on a system catalog")));

	attnum = get_attnum(RelationGetRelid(rel), colname);
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colname, RelationGetRelationName(rel))));
	att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
	if (!dict_method(att->attcompression))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column \"%s\" does not use a dictionary compression method",
						colname)));
	return attnum;
}
#endif							/* USE_ZSTD */

/*
 * pg_train_toast_dictionary(rel regclass, col name, dict_size int4)
 *
 * Train a zstd dictionary from the column's current values and make it the
 * column's current dictionary.  The previous one stays readable until the
 * next rewrite of the table retires it.  Returns the new dictionary's OID, or
 * NULL if zstd could not train one from the values found.
 *
 * Takes ShareUpdateExclusiveLock, like ANALYZE: DML continues, concurrent
 * training and DDL wait.
 *
 * Automatic evolution (toast_dictionary_maybe_evolve) trains from ANALYZE's
 * block sample instead; this function keeps its own bounded seqscan for the
 * explicit, user-requested case.
 */
Datum
pg_train_toast_dictionary(PG_FUNCTION_ARGS)
{
#ifndef USE_ZSTD
	NO_ZSTD_SUPPORT();
	PG_RETURN_NULL();
#else
	Oid			relid = PG_GETARG_OID(0);
	char	   *colname = NameStr(*PG_GETARG_NAME(1));
	int32		dict_size = PG_GETARG_INT32(2);
	Relation	rel;
	AttrNumber	attnum;
	TableScanDesc scan;
	TupleTableSlot *slot;
	StringInfoData samples;
	size_t	   *sizes;
	int			nsamples = 0;
	void	   *dictbuf;
	size_t		dictlen;
	Oid			dictid;

	if (dict_size < DICT_MIN_SIZE || dict_size > DICT_MAX_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dictionary size must be between %d and %d bytes",
						DICT_MIN_SIZE, DICT_MAX_SIZE)));

	rel = table_open(relid, ShareUpdateExclusiveLock);
	attnum = dict_check_column(rel, colname);

	initStringInfo(&samples);
	sizes = palloc_array(size_t, DICT_MAX_SAMPLES);
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
	while (nsamples < DICT_MAX_SAMPLES &&
		   table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, attnum, &isnull);
		varlena    *v;
		size_t		len;

		CHECK_FOR_INTERRUPTS();
		if (isnull)
			continue;
		v = detoast_attr_slice((varlena *) DatumGetPointer(d), 0,
							   DICT_SAMPLE_LIMIT);
		len = VARSIZE_ANY_EXHDR(v);
		if (samples.len + len > (size_t) dict_size * DICT_SAMPLE_FACTOR)
		{
			pfree(v);
			break;
		}
		if (len >= 8)
		{
			appendBinaryStringInfo(&samples, VARDATA_ANY(v), len);
			sizes[nsamples++] = len;
		}
		pfree(v);
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);

	dictbuf = palloc(dict_size);
	dictlen = dict_routine_for_method(TupleDescAttr(RelationGetDescr(rel),
													attnum - 1)->attcompression)
		->train(samples.data, sizes, nsamples, dict_size, dictbuf);
	if (dictlen == 0)
	{
		ereport(NOTICE,
				(errmsg("could not train a compression dictionary for column \"%s\" from %d values",
						colname, nsamples)));
		table_close(rel, NoLock);
		PG_RETURN_NULL();
	}

	dictid = dict_install(rel, attnum, InvalidOid, true,
						  (int64) rel->rd_rel->reltuples, dictbuf, dictlen);
	table_close(rel, NoLock);

	PG_RETURN_OID(dictid);
#endif
}

/*
 * Collect the current dictionary row of rel.attnum, if any, returning its OID
 * and, when wantbytes, a palloc'd copy of its dictionary bytes (caller frees).
 * Also returns tdtrainrows and tdtraintime for the evolution gates.  Returns
 * InvalidOid when the column has no current dictionary.
 */
static Oid
dict_current_info(Relation drel, Oid relid, AttrNumber attnum,
				  int64 *trainrows, int64 *traintime,
				  bytea **dictbytes, bool wantbytes)
{
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			result = InvalidOid;

	*trainrows = 0;
	*traintime = 0;
	if (dictbytes)
		*dictbytes = NULL;
	tup = dict_lookup(relid, attnum, true, drel, &scan);
	if (tup)
	{
		Form_pg_toast_dictionary d = (Form_pg_toast_dictionary) GETSTRUCT(tup);

		result = d->oid;
		*trainrows = d->tdtrainrows;
		*traintime = d->tdtraintime;
		if (wantbytes)
			*dictbytes = DatumGetByteaPCopy(SysCacheGetAttrNotNull(TOASTDICTOID, tup,
																  Anum_pg_toast_dictionary_tddict));
	}
	systable_endscan(scan);
	return result;
}

/* Count dictionaries (current + non-current) for rel.attnum */
static int
dict_count(Relation drel, Oid relid, AttrNumber attnum)
{
	SysScanDesc scan;
	HeapTuple	tup;
	int			n = 0;

	tup = dict_lookup(relid, attnum, false, drel, &scan);
	for (; HeapTupleIsValid(tup); tup = systable_getnext(scan))
		n++;
	systable_endscan(scan);
	return n;
}

/*
 * toast_dictionary_maybe_evolve
 *
 * Automatic, best-effort dictionary retraining during ANALYZE.  Runs only in
 * the maintenance process (ANALYZE holds ShareUpdateExclusiveLock, like the
 * manual trainer), reuses ANALYZE's already-fetched sample rows, and installs
 * a new current dictionary only when a held-out size test says a candidate
 * beats the current one by the configured margin.  The write path is never
 * involved.  The work runs in an internal subtransaction: an error rolls it
 * back, releasing whatever the failed step held (a TOAST scan, buffer pins,
 * a half-done dict_install()), is reported as a WARNING, and ANALYZE goes on.
 * A query cancel or a pending interrupt is re-thrown instead, so ANALYZE stops
 * as it would anywhere else.
 */
void
toast_dictionary_maybe_evolve(Relation onerel, AttrNumber attnum,
							  HeapTuple *rows, int numrows, double totalrows)
{
	AttributeOpts *aopt;
	Form_pg_attribute att;
	const DictCompressRoutine *routine;
	MemoryContext evolvecxt,
				oldcxt;
	ResourceOwner oldowner;
	TupleDesc	tupdesc = RelationGetDescr(onerel);

	/* (a) opted in, and still a dictionary column? */
	if (attnum <= 0 || attnum > tupdesc->natts)
		return;
	att = TupleDescAttr(tupdesc, attnum - 1);
	if (att->attisdropped || !dict_method(att->attcompression))
		return;
	aopt = get_attribute_options(RelationGetRelid(onerel), attnum);
	if (aopt == NULL || !aopt->dictionary_evolution)
		return;

	routine = dict_routine_for_method(att->attcompression);

	/*
	 * Everything below runs in a short-lived context so that samples, the
	 * candidate dictionary, and fetched current-dictionary bytes are all
	 * freed on return, and in an internal subtransaction so that a codec or
	 * catalog error degrades to "no evolution this round" rather than
	 * aborting ANALYZE, without leaking what the failed step held.
	 */
	oldcxt = CurrentMemoryContext;
	oldowner = CurrentResourceOwner;
	evolvecxt = AllocSetContextCreate(oldcxt,
									  "Dictionary evolution",
									  ALLOCSET_DEFAULT_SIZES);

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(evolvecxt);

	PG_TRY();
	{
		Relation	drel;
		Oid			curid;
		int64		currows;
		int64		curtime;
		bytea	   *curbytes = NULL;
		int			ndicts;
		StringInfoData train;
		StringInfoData held;
		size_t	   *trainsizes;
		size_t	   *heldsizes;
		int			ntrain = 0;
		int			nheld = 0;
		int			i;
		size_t		samplebytes = 0;
		int32		dictsize;
		void	   *candbuf;
		size_t		candlen;
		bool		adopt;

		drel = table_open(ToastDictionaryRelationId, AccessShareLock);
		curid = dict_current_info(drel, RelationGetRelid(onerel), attnum,
								  &currows, &curtime, &curbytes, true);
		ndicts = dict_count(drel, RelationGetRelid(onerel), attnum);
		table_close(drel, AccessShareLock);

		/* (b) growth, floor, and cooldown gates */
		if (totalrows < (double) aopt->dictionary_evolution_min_rows)
			goto done;
		if (OidIsValid(curid))
		{
			if (totalrows < (double) currows * aopt->dictionary_evolution_growth)
				goto done;
			if (curtime != 0)
			{
				/* cooldown in ms, saturated to avoid int overflow */
				int			cooldown_ms = dictionary_evolution_min_interval > INT_MAX / 1000 ?
					INT_MAX : dictionary_evolution_min_interval * 1000;

				if (!TimestampDifferenceExceeds((TimestampTz) curtime,
												GetCurrentTimestamp(),
												cooldown_ms))
					goto done;
			}
		}

		/* (c) cap gate */
		if (ndicts >= dictionary_evolution_max_retained)
		{
			ereport(LOG,
					(errmsg("column \"%s\" of \"%s\" reached the retained-dictionary cap of %d; run VACUUM FULL / REPACK to retire old dictionaries",
							NameStr(att->attname),
							RelationGetRelationName(onerel),
							dictionary_evolution_max_retained)));
			goto done;
		}

		/*
		 * (d) build samples from the ANALYZE rows[].  Split deterministically
		 * into an 80% training set and a 20% held-out set (every 5th sample is
		 * held out), using the same predicates as the manual trainer: skip
		 * nulls and values under 8 bytes, detoast a 128kB prefix.
		 */
		initStringInfo(&train);
		initStringInfo(&held);
		trainsizes = palloc_array(size_t, numrows);
		heldsizes = palloc_array(size_t, numrows);
		for (i = 0; i < numrows; i++)
		{
			bool		isnull;
			Datum		d;
			varlena    *v;
			size_t		len;

			CHECK_FOR_INTERRUPTS();
			d = heap_getattr(rows[i], attnum, tupdesc, &isnull);
			if (isnull)
				continue;
			v = detoast_attr_slice((varlena *) DatumGetPointer(d), 0,
								   DICT_SAMPLE_LIMIT);
			len = VARSIZE_ANY_EXHDR(v);
			if (len >= 8)
			{
				if ((ntrain + nheld) % 5 == 4)
				{
					appendBinaryStringInfo(&held, VARDATA_ANY(v), len);
					heldsizes[nheld++] = len;
				}
				else
				{
					appendBinaryStringInfo(&train, VARDATA_ANY(v), len);
					trainsizes[ntrain++] = len;
					samplebytes += len;
				}
			}
			pfree(v);
		}

		if (ntrain == 0)
			goto done;

		/*
		 * (e) size the candidate.  zstd wants about DICT_SAMPLE_FACTOR times
		 * the dictionary in samples, so shrink to fit a small sample; but cap
		 * at the usual default size, because a much larger dictionary trained
		 * on a block-clustered ANALYZE sample tends to overfit and generalize
		 * worse on held-out data, not better.  Train it on the 80%.
		 */
		dictsize = (int32) (samplebytes / DICT_SAMPLE_FACTOR);
		if (dictsize > DICT_DEFAULT_SIZE)
			dictsize = DICT_DEFAULT_SIZE;
		if (dictsize < DICT_MIN_SIZE)
			dictsize = DICT_MIN_SIZE;
		candbuf = palloc(dictsize);
		candlen = routine->train(train.data, trainsizes, ntrain, dictsize,
								 candbuf);
		if (candlen == 0)
			goto done;

		/*
		 * (f) held-out test.  With no current dictionary, adopt the first one
		 * unconditionally (its baseline is plain zstd, already accepted by the
		 * manual path).  Otherwise compare total compressed held-out bytes and
		 * adopt only on a margin win.
		 */
		if (!OidIsValid(curid))
			adopt = true;
		else if (nheld == 0)
			adopt = false;		/* nothing to judge by; keep current */
		else
		{
			/* one call per dictionary: each is digested once, not per value */
			size_t		curtotal = routine->compressed_total(VARDATA_ANY(curbytes),
															 VARSIZE_ANY_EXHDR(curbytes),
															 held.data, heldsizes,
															 nheld);
			size_t		candtotal = routine->compressed_total(candbuf, candlen,
															  held.data, heldsizes,
															  nheld);

			adopt = curtotal != 0 && candtotal != 0 &&
				(double) candtotal <= (double) curtotal *
				(1.0 - dictionary_evolution_min_gain);

			elog(DEBUG1, "dictionary evolution for \"%s\": heldout=%d dictsize=%d cur=%zu cand=%zu gain=%.1f%% adopt=%d",
				 NameStr(att->attname), nheld, dictsize, curtotal, candtotal,
				 curtotal ? 100.0 * (1.0 - (double) candtotal / curtotal) : 0.0,
				 adopt);
		}

		/* (g) adopt on a win; otherwise change nothing */
		if (adopt)
			dict_install(onerel, attnum, InvalidOid, true,
						 (int64) totalrows, candbuf, candlen);

done:
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
		MemoryContextDelete(evolvecxt);

		/*
		 * A cancel (including statement_timeout) must stop ANALYZE as it
		 * would anywhere else.  ProcessInterrupts() clears QueryCancelPending
		 * before raising it, so test the error code; also re-throw if an
		 * interrupt is still pending.
		 */
		if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
			QueryCancelPending || ProcDiePending)
			ReThrowError(edata);

		ereport(WARNING,
				(errmsg("automatic dictionary evolution for column \"%s\" of \"%s\" was skipped: %s",
						NameStr(att->attname),
						RelationGetRelationName(onerel),
						edata->message)));
		FreeErrorData(edata);
		return;
	}
	PG_END_TRY();

	MemoryContextDelete(evolvecxt);
}

/*
 * binary_upgrade_add_toast_dictionary(rel regclass, attnum int2, dictid oid,
 *									   current bool, dict bytea)
 *
 * pg_upgrade copies the heap files as they are, and with them the dictionary
 * OIDs embedded in each datum, so dictionaries are restored with their OIDs.
 */
Datum
binary_upgrade_add_toast_dictionary(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	AttrNumber	attnum = PG_GETARG_INT16(1);
	Oid			dictid = PG_GETARG_OID(2);
	bool		current = PG_GETARG_BOOL(3);
	bytea	   *dict = PG_GETARG_BYTEA_PP(4);
	Relation	rel;

	if (!IsBinaryUpgrade)
		ereport(ERROR,
				(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
				 errmsg("function can only be called when server is in binary upgrade mode")));

	/*
	 * No dict_check_column() here: the column may have been switched to
	 * another compression method, or dropped, with old values still using
	 * these dictionaries until the table is rewritten.  pg_dump restores
	 * physical column numbers, so attnum is stable.
	 */
	rel = table_open(relid, AccessExclusiveLock);
	if (attnum <= 0 || attnum > RelationGetNumberOfAttributes(rel))
		elog(ERROR, "invalid attribute number %d for relation %u",
			 attnum, relid);
	dict_install(rel, attnum, dictid, current, 0, VARDATA_ANY(dict),
				 VARSIZE_ANY_EXHDR(dict));
	table_close(rel, NoLock);

	PG_RETURN_VOID();
}
