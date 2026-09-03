/*-------------------------------------------------------------------------
 *
 * detoast.c
 *	  Retrieve compressed or external variable size attributes.
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/common/detoast.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/toast_internals.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/pg_lzcompress.h"
#include "storage/bufmgr.h"
#include "utils/expandeddatum.h"
#include "utils/rel.h"
#include "utils/array.h"

static varlena *toast_fetch_datum_slice(varlena *attr,
										int32 sliceoffset,
										int32 slicelength);
static inline varlena *
toast_fetch_datum(varlena *attr)
{
	return toast_fetch_datum_slice(attr, 0, -1);
}

static varlena *toast_decompress_datum(varlena *attr);
static varlena *toast_decompress_datum_slice(varlena *attr, int32 slicelength);
static void toast_fetch_datum_direct_slice_recursive(Relation toastrel, ItemPointer tid,
													 varlena *result, int32 *logical_offset,
													 int32 sliceoffset, int32 slicelength,
													 TupleTableSlot *slot);

/*
 * Unpacked metadata from either plain (varatt_external) or direct (varatt_direct)
 * on-disk TOAST pointers.
 */
typedef struct ToastExternalMetadata
{
	int32		extsize;
	uint32		compress_method;
	bool		is_compressed;
	Oid			toastrelid;
	bool		is_direct;
	Oid			valueid;
	union
	{
		varatt_direct	direct_tp;
		varatt_external tp;
	};
} ToastExternalMetadata;

static inline void
toast_get_external_metadata(varlena *attr, ToastExternalMetadata *meta)
{
	if (VARATT_IS_EXTERNAL_DIRECT(attr))
	{
		VARATT_EXTERNAL_GET_POINTER_DIRECT(meta->direct_tp, attr);
		meta->extsize = VARATT_DIRECT_GET_EXTSIZE(meta->direct_tp);
		meta->compress_method = VARATT_DIRECT_GET_COMPRESS_METHOD(meta->direct_tp);
		meta->is_compressed = VARATT_DIRECT_IS_COMPRESSED(meta->direct_tp);
		meta->toastrelid = meta->direct_tp.va_toastrelid;
		meta->is_direct = true;
		meta->valueid = InvalidOid;
	}
	else if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		VARATT_EXTERNAL_GET_POINTER(meta->tp, attr);
		meta->extsize = VARATT_EXTERNAL_GET_EXTSIZE(meta->tp);
		meta->compress_method = VARATT_EXTERNAL_GET_COMPRESS_METHOD(meta->tp);
		meta->is_compressed = VARATT_EXTERNAL_IS_COMPRESSED(meta->tp);
		meta->toastrelid = meta->tp.va_toastrelid;
		meta->is_direct = false;
		meta->valueid = meta->tp.va_valueid;
	}
	else
	{
		elog(ERROR, "toast_get_external_metadata called for unsupported datum");
	}
}

/* ----------
 * detoast_external_attr -
 *
 *	Public entry point to get back a toasted value from
 *	external source (possibly still in compressed format).
 *
 * This will return a datum that contains all the data internally, ie, not
 * relying on external storage or memory, but it can still be compressed or
 * have a short header.  Note some callers assume that if the input is an
 * EXTERNAL datum, the result will be a pfree'able chunk.
 * ----------
 */
varlena *
detoast_external_attr(varlena *attr)
{
	varlena    *result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr) || VARATT_IS_EXTERNAL_DIRECT(attr))
	{
		/*
		 * This is an external stored plain or direct value
		 */
		result = toast_fetch_datum(attr);
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		/*
		 * This is an indirect pointer --- dereference it
		 */
		varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);
		attr = (varlena *) redirect.pointer;

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		/* recurse if value is still external in some other way */
		if (VARATT_IS_EXTERNAL(attr))
			return detoast_external_attr(attr);

		/*
		 * Copy into the caller's memory context, in case caller tries to
		 * pfree the result.
		 */
		result = (varlena *) palloc(VARSIZE_ANY(attr));
		memcpy(result, attr, VARSIZE_ANY(attr));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/*
		 * This is an expanded-object pointer --- get flat format
		 */
		ExpandedObjectHeader *eoh;
		Size		resultsize;

		eoh = DatumGetEOHP(PointerGetDatum(attr));
		resultsize = EOH_get_flat_size(eoh);
		result = (varlena *) palloc(resultsize);
		EOH_flatten_into(eoh, result, resultsize);
	}
	else
	{
		/*
		 * This is a plain value inside of the main tuple - why am I called?
		 */
		result = attr;
	}

	return result;
}


/* ----------
 * detoast_attr -
 *
 *	Public entry point to get back a toasted value from compression
 *	or external storage.  The result is always non-extended varlena form.
 *
 * Note some callers assume that if the input is an EXTERNAL or COMPRESSED
 * datum, the result will be a pfree'able chunk.
 * ----------
 */
varlena *
detoast_attr(varlena *attr)
{
	if (VARATT_IS_EXTERNAL_ONDISK(attr) || VARATT_IS_EXTERNAL_DIRECT(attr))
	{
		/*
		 * This is an externally stored datum --- fetch it back from there
		 */
		attr = toast_fetch_datum(attr);
		/* If it's compressed, decompress it */
		if (VARATT_IS_COMPRESSED(attr))
		{
			varlena    *tmp = attr;

			attr = toast_decompress_datum(tmp);
			pfree(tmp);
		}
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		/*
		 * This is an indirect pointer --- dereference it
		 */
		varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);
		attr = (varlena *) redirect.pointer;

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		/* recurse in case value is still extended in some other way */
		attr = detoast_attr(attr);

		/* if it isn't, we'd better copy it */
		if (attr == (varlena *) redirect.pointer)
		{
			varlena    *result;

			result = (varlena *) palloc(VARSIZE_ANY(attr));
			memcpy(result, attr, VARSIZE_ANY(attr));
			attr = result;
		}
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/*
		 * This is an expanded-object pointer --- get flat format
		 */
		attr = detoast_external_attr(attr);
		/* flatteners are not allowed to produce compressed/short output */
		Assert(!VARATT_IS_EXTENDED(attr));
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/*
		 * This is a compressed value inside of the main tuple
		 */
		attr = toast_decompress_datum(attr);
	}
	else if (VARATT_IS_SHORT(attr))
	{
		/*
		 * This is a short-header varlena --- convert to 4-byte header format
		 */
		Size		data_size = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT;
		Size		new_size = data_size + VARHDRSZ;
		varlena    *new_attr;

		new_attr = (varlena *) palloc(new_size);
		SET_VARSIZE(new_attr, new_size);
		memcpy(VARDATA(new_attr), VARDATA_SHORT(attr), data_size);
		attr = new_attr;
	}

	return attr;
}


/* ----------
 * detoast_attr_slice -
 *
 *		Public entry point to get back part of a toasted value
 *		from compression or external storage.
 *
 * sliceoffset is where to start (zero or more)
 * If slicelength < 0, return everything beyond sliceoffset
 * ----------
 */
varlena *
detoast_attr_slice(varlena *attr,
				   int32 sliceoffset, int32 slicelength)
{
	varlena    *preslice;
	varlena    *result;
	char	   *attrdata;
	int32		slicelimit;
	int32		attrsize;

	if (sliceoffset < 0)
		elog(ERROR, "invalid sliceoffset: %d", sliceoffset);

	/*
	 * Compute slicelimit = offset + length, or -1 if we must fetch all of the
	 * value.  In case of integer overflow, we must fetch all.
	 */
	if (slicelength < 0)
		slicelimit = -1;
	else if (pg_add_s32_overflow(sliceoffset, slicelength, &slicelimit))
		slicelength = slicelimit = -1;

	if (VARATT_IS_EXTERNAL_ONDISK(attr) || VARATT_IS_EXTERNAL_DIRECT(attr))
	{
		ToastExternalMetadata meta;

		toast_get_external_metadata(attr, &meta);

		/* fast path for non-compressed external datums */
		if (!meta.is_compressed)
			return toast_fetch_datum_slice(attr, sliceoffset, slicelength);

		/*
		 * For compressed values, we need to fetch enough slices to decompress
		 * at least the requested part (when a prefix is requested).
		 * Otherwise, just fetch all slices.
		 */
		if (slicelimit >= 0)
		{
			int32		max_size = meta.extsize;

			/*
			 * Determine maximum amount of compressed data needed for a prefix
			 * of a given length (after decompression).
			 *
			 * At least for now, if it's LZ4 data, we'll have to fetch the
			 * whole thing, because there doesn't seem to be an API call to
			 * determine how much compressed data we need to be sure of being
			 * able to decompress the required slice.
			 */
			if (meta.compress_method == TOAST_PGLZ_COMPRESSION_ID)
				max_size = pglz_maximum_compressed_size(slicelimit, max_size);

			/*
			 * Fetch enough compressed slices (compressed marker will get set
			 * automatically).
			 */
			preslice = toast_fetch_datum_slice(attr, 0, max_size);
		}
		else
			preslice = toast_fetch_datum(attr);
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(redirect.pointer));

		return detoast_attr_slice(redirect.pointer,
								  sliceoffset, slicelength);
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/* pass it off to detoast_external_attr to flatten */
		preslice = detoast_external_attr(attr);
	}
	else
		preslice = attr;

	Assert(!VARATT_IS_EXTERNAL(preslice));

	if (VARATT_IS_COMPRESSED(preslice))
	{
		varlena    *tmp = preslice;

		/* Decompress enough to encompass the slice and the offset */
		if (slicelimit >= 0)
			preslice = toast_decompress_datum_slice(tmp, slicelimit);
		else
			preslice = toast_decompress_datum(tmp);

		if (tmp != attr)
			pfree(tmp);
	}

	if (VARATT_IS_SHORT(preslice))
	{
		attrdata = VARDATA_SHORT(preslice);
		attrsize = VARSIZE_SHORT(preslice) - VARHDRSZ_SHORT;
	}
	else
	{
		attrdata = VARDATA(preslice);
		attrsize = VARSIZE(preslice) - VARHDRSZ;
	}

	/* slicing of datum for compressed cases and plain value */

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		slicelength = 0;
	}
	else if (slicelength < 0 || slicelimit > attrsize)
		slicelength = attrsize - sliceoffset;

	result = (varlena *) palloc(slicelength + VARHDRSZ);
	SET_VARSIZE(result, slicelength + VARHDRSZ);

	memcpy(VARDATA(result), attrdata + sliceoffset, slicelength);

	if (preslice != attr)
		pfree(preslice);

	return result;
}

/* ----------
 * toast_fetch_datum_slice -
 *
 *	Reconstruct a segment of a Datum from the chunks saved
 *	in the toast relation (supports both plain index-based and direct TOAST).
 *
 *	Note that this function supports non-compressed external datums
 *	and compressed external datums (in which case the requested slice
 *	has to be a prefix, i.e. sliceoffset has to be 0).
 * ----------
 */
static varlena *
toast_fetch_datum_slice(varlena *attr, int32 sliceoffset,
						int32 slicelength)
{
	Relation	toastrel;
	varlena    *result;
	int32		attrsize;
	ToastExternalMetadata meta;

	toast_get_external_metadata(attr, &meta);

	/*
	 * It's nonsense to fetch slices of a compressed datum unless when it's a
	 * prefix -- this isn't lo_* we can't return a compressed datum which is
	 * meaningful to toast later.
	 */
	Assert(!meta.is_compressed || 0 == sliceoffset);

	attrsize = meta.extsize;

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		slicelength = 0;
	}

	/*
	 * When fetching a prefix of a compressed external datum, account for the
	 * space required by va_tcinfo, which is stored at the beginning as an
	 * int32 value.
	 */
	if (meta.is_compressed && slicelength > 0)
		slicelength = slicelength + sizeof(int32);

	/*
	 * Adjust length request if needed.  (Note: our sole caller,
	 * detoast_attr_slice, protects us against sliceoffset + slicelength
	 * overflowing.)
	 */
	if (((sliceoffset + slicelength) > attrsize) || slicelength < 0)
		slicelength = attrsize - sliceoffset;

	result = (varlena *) palloc(slicelength + VARHDRSZ);

	if (meta.is_compressed)
		SET_VARSIZE_COMPRESSED(result, slicelength + VARHDRSZ);
	else
		SET_VARSIZE(result, slicelength + VARHDRSZ);

	if (slicelength == 0)
		return result;			/* Can save a lot of work at this point! */

	/* Open the toast relation */
	toastrel = table_open(meta.toastrelid, AccessShareLock);

	if (meta.is_direct)
	{
		/*
		 * Fast path for single-chunk direct TOAST: fetch directly via
		 * heap_fetch without allocating/dropping a TupleTableSlot.
		 */
		if (attrsize <= TOAST_MAX_CHUNK_SIZE)
		{
			HeapTupleData tup;
			Buffer		buffer = InvalidBuffer;
			bool		isnull;
			Pointer		chunk;
			int32		chunk_size;
			char	   *chunk_data;

			tup.t_self = meta.direct_tp.va_tid;
			if (!heap_fetch(toastrel, get_toast_snapshot(), &tup, &buffer, false))
				elog(ERROR, "failed to fetch toast tuple by TID");

			chunk = DatumGetPointer(fastgetattr(&tup, 3, toastrel->rd_att, &isnull));
			if (isnull)
				elog(ERROR, "unexpected NULL chunk_data in direct toast chunk");

			if (!VARATT_IS_EXTENDED(chunk))
			{
				chunk_size = VARSIZE(chunk) - VARHDRSZ;
				chunk_data = VARDATA(chunk);
			}
			else if (VARATT_IS_SHORT(chunk))
			{
				chunk_size = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
				chunk_data = VARDATA_SHORT(chunk);
			}
			else
				elog(ERROR, "unexpected type of toast chunk");

			if (sliceoffset >= chunk_size)
			{
				slicelength = 0;
				sliceoffset = 0;
			}
			else if (sliceoffset + slicelength > chunk_size || slicelength < 0)
				slicelength = chunk_size - sliceoffset;

			if (slicelength > 0)
				memcpy(VARDATA(result), chunk_data + sliceoffset, slicelength);

			ReleaseBuffer(buffer);
		}
		else
		{
			TupleTableSlot *slot = table_slot_create(toastrel, NULL);
			int32		logical_offset = 0;

			toast_fetch_datum_direct_slice_recursive(toastrel, &meta.direct_tp.va_tid,
													 result, &logical_offset,
													 sliceoffset, slicelength, slot);
			ExecDropSingleTupleTableSlot(slot);
		}
	}
	else
	{
		/* Fetch all chunks via Table AM index scan */
		table_relation_fetch_toast_slice(toastrel, meta.valueid,
										 attrsize, sliceoffset, slicelength,
										 result);
	}

	/* Close toast table */
	table_close(toastrel, AccessShareLock);

	return result;
}

/* ----------
 * toast_decompress_datum -
 *
 * Decompress a compressed version of a varlena datum
 */
static varlena *
toast_decompress_datum(varlena *attr)
{
	ToastCompressionId cmid;

	Assert(VARATT_IS_COMPRESSED(attr));

	/*
	 * Fetch the compression method id stored in the compression header and
	 * decompress the data using the appropriate decompression routine.
	 */
	cmid = TOAST_COMPRESS_METHOD(attr);
	switch (cmid)
	{
		case TOAST_PGLZ_COMPRESSION_ID:
			return pglz_decompress_datum(attr);
		case TOAST_LZ4_COMPRESSION_ID:
			return lz4_decompress_datum(attr);
		default:
			elog(ERROR, "invalid compression method id %d", cmid);
			return NULL;		/* keep compiler quiet */
	}
}


/* ----------
 * toast_decompress_datum_slice -
 *
 * Decompress the front of a compressed version of a varlena datum.
 * offset handling happens in detoast_attr_slice.
 * Here we just decompress a slice from the front.
 */
static varlena *
toast_decompress_datum_slice(varlena *attr, int32 slicelength)
{
	ToastCompressionId cmid;

	Assert(VARATT_IS_COMPRESSED(attr));

	/*
	 * Some callers may pass a slicelength that's more than the actual
	 * decompressed size.  If so, just decompress normally.  This avoids
	 * possibly allocating a larger-than-necessary result object, and may be
	 * faster and/or more robust as well.  Notably, some versions of liblz4
	 * have been seen to give wrong results if passed an output size that is
	 * more than the data's true decompressed size.
	 */
	if ((uint32) slicelength >= TOAST_COMPRESS_EXTSIZE(attr))
		return toast_decompress_datum(attr);

	/*
	 * Fetch the compression method id stored in the compression header and
	 * decompress the data slice using the appropriate decompression routine.
	 */
	cmid = TOAST_COMPRESS_METHOD(attr);
	switch (cmid)
	{
		case TOAST_PGLZ_COMPRESSION_ID:
			return pglz_decompress_datum_slice(attr, slicelength);
		case TOAST_LZ4_COMPRESSION_ID:
			return lz4_decompress_datum_slice(attr, slicelength);
		default:
			elog(ERROR, "invalid compression method id %d", cmid);
			return NULL;		/* keep compiler quiet */
	}
}

/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 *	(including the VARHDRSZ header)
 * ----------
 */
Size
toast_raw_datum_size(Datum value)
{
	varlena    *attr = (varlena *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/* va_rawsize is the size of the original datum -- including header */
		varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		result = toast_pointer.va_rawsize;
	}
	else if (VARATT_IS_EXTERNAL_DIRECT(attr))
	{
		struct varatt_direct toast_pointer;

		VARATT_EXTERNAL_GET_POINTER_DIRECT(toast_pointer, attr);
		result = toast_pointer.va_rawsize;
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		varatt_indirect toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(toast_pointer.pointer));

		return toast_raw_datum_size(PointerGetDatum(toast_pointer.pointer));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		result = EOH_get_flat_size(DatumGetEOHP(value));
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/* here, va_rawsize is just the payload size */
		result = VARDATA_COMPRESSED_GET_EXTSIZE(attr) + VARHDRSZ;
	}
	else if (VARATT_IS_SHORT(attr))
	{
		/*
		 * we have to normalize the header length to VARHDRSZ or else the
		 * callers of this function will be confused.
		 */
		result = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT + VARHDRSZ;
	}
	else
	{
		/* plain untoasted datum */
		result = VARSIZE(attr);
	}
	return result;
}

/* ----------
 * toast_datum_size
 *
 *	Return the physical storage size (possibly compressed) of a varlena datum
 * ----------
 */
Size
toast_datum_size(Datum value)
{
	varlena    *attr = (varlena *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/*
		 * Attribute is stored externally - return the extsize whether
		 * compressed or not.  We do not count the size of the toast pointer
		 * ... should we?
		 */
		varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		result = VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer);
	}
	else if (VARATT_IS_EXTERNAL_DIRECT(attr))
	{
		struct varatt_direct toast_pointer;

		VARATT_EXTERNAL_GET_POINTER_DIRECT(toast_pointer, attr);
		result = VARATT_DIRECT_GET_EXTSIZE(toast_pointer);
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		varatt_indirect toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		return toast_datum_size(PointerGetDatum(toast_pointer.pointer));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		result = EOH_get_flat_size(DatumGetEOHP(value));
	}
	else if (VARATT_IS_SHORT(attr))
	{
		result = VARSIZE_SHORT(attr);
	}
	else
	{
		/*
		 * Attribute is stored inline either compressed or not, just calculate
		 * the size of the datum in either case.
		 */
		result = VARSIZE(attr);
	}
	return result;
}

/*
 * Helper to copy overlapping chunk slice into detoasted result varlena.
 */
static inline void
toast_slice_copy_chunk(struct varlena *result, const char *chunk_data,
					   int32 chunk_size, int32 *logical_offset,
					   int32 req_start, int32 req_end)
{
	int32		chunk_start = *logical_offset;
	int32		chunk_end = chunk_start + chunk_size;

	*logical_offset = chunk_end;

	if (chunk_end > req_start && chunk_start < req_end)
	{
		int32		copy_start = Max(chunk_start, req_start);
		int32		copy_end = Min(chunk_end, req_end);
		int32		copy_len = copy_end - copy_start;

		if (copy_len > 0)
		{
			int32		src_offset = copy_start - chunk_start;
			int32		dest_offset = copy_start - req_start;

			memcpy(VARDATA(result) + dest_offset, chunk_data + src_offset, copy_len);
		}
	}
}

/*
 * toast_fetch_datum_direct_slice_recursive -
 *
 * Traverse a Direct TOAST tree/DAG to retrieve full datums or partial slices.
 *
 * Direct TOAST organizes chunks either as a single chunk, a flat multi-chunk
 * list (chunk_tids populated, chunk_tid_offsets NULL), or a multi-level tree
 * (both chunk_tids and chunk_tid_offsets populated).
 *
 * - Leaf chunks (chunk_tids IS NULL) copy their slice payload directly into
 *   the result buffer at *logical_offset.
 * - Flat multi-chunk roots iterate through all child TIDs in chunk_tids.
 * - Interior tree chunks inspect chunk_tid_offsets to prune any subtrees that
 *   do not overlap the requested range [sliceoffset, sliceoffset + slicelength),
 *   achieving O(log N) slice fetching without consulting an index.
 */
static void
toast_fetch_datum_direct_slice_recursive(Relation toastrel, ItemPointer tid,
										 struct varlena *result, int32 *logical_offset,
										 int32 sliceoffset, int32 slicelength,
										 TupleTableSlot *slot)
{
	Snapshot	snapshot = get_toast_snapshot();
	Datum		data_datum;
	Datum		tids_datum;
	Datum		offsets_datum;
	bool		is_null_data;
	bool		is_null_tids;
	bool		is_null_offsets = true;

	if (!table_tuple_fetch_row_version(toastrel, tid, snapshot, slot))
	{
		elog(ERROR, "failed to fetch toast tuple by TID");
	}

	data_datum = slot_getattr(slot, 3, &is_null_data);
	tids_datum = slot_getattr(slot, 4, &is_null_tids);
	if (is_null_tids)
	{
		/* Leaf chunk: copy data slice */
		if (!is_null_data)
		{
			struct varlena *data_val = PG_DETOAST_DATUM(data_datum);
			int32		chunk_size = VARSIZE_ANY_EXHDR(data_val);
			int32		req_start = sliceoffset;
			int32		req_end = sliceoffset + slicelength;

			toast_slice_copy_chunk(result, VARDATA_ANY(data_val), chunk_size,
								   logical_offset, req_start, req_end);
		}
		ExecClearTuple(slot);
	}
	else
	{
		ArrayType  *arr = DatumGetArrayTypePCopy(tids_datum);
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;
		int			i;

		offsets_datum = slot_getattr(slot, 5, &is_null_offsets);

		deconstruct_array_builtin(arr, TIDOID, &elems, &nulls, &nelems);

		if (!is_null_offsets)
		{
			/*
			 * Tree-structured interior node with chunk_tid_offsets.
			 * Use offsets to prune subtrees that don't overlap the requested slice.
			 */
			ArrayType  *arr_offsets = DatumGetArrayTypePCopy(offsets_datum);
			Datum	   *offset_elems;
			bool	   *offset_nulls;
			int			noffsets;
			int64		req_start = sliceoffset;
			int64		req_end = (slicelength < 0) ? PG_INT64_MAX : ((int64) sliceoffset + slicelength);

			deconstruct_array_builtin(arr_offsets, INT8OID, &offset_elems, &offset_nulls, &noffsets);
			Assert(noffsets == nelems + 1);

			ExecClearTuple(slot);

			for (i = 0; i < nelems; i++)
			{
				int64		child_start = DatumGetInt64(offset_elems[i]);
				int64		child_end = DatumGetInt64(offset_elems[i + 1]);

				if (child_end <= req_start || child_start >= req_end)
				{
					/* Subtree does not intersect slice range; skip it */
					*logical_offset = (int32) child_end;
					continue;
				}

				*logical_offset = (int32) child_start;
				toast_fetch_datum_direct_slice_recursive(toastrel,
														 (ItemPointer) DatumGetPointer(elems[i]),
														 result, logical_offset,
														 sliceoffset, slicelength,
														 slot);
				*logical_offset = (int32) child_end;
			}

			pfree(offset_elems);
			pfree(offset_nulls);
			pfree(arr_offsets);
		}
		else
		{
			/*
			 * Flat direct TOAST: chunks 0 to nelems-1 are leaf data chunks
			 * of size TOAST_MAX_CHUNK_SIZE, and the current chunk contains the
			 * final chunk_data (chunk nelems).
			 */
			int32		chunk_size = 0;
			char	   *chunk_data = NULL;
			struct varlena *data_val = NULL;
			int64		req_start = sliceoffset;
			int64		req_end = (slicelength < 0) ? PG_INT64_MAX : ((int64) sliceoffset + slicelength);

			if (!is_null_data)
			{
				data_val = PG_DETOAST_DATUM_COPY(data_datum);
				chunk_size = VARSIZE_ANY_EXHDR(data_val);
				chunk_data = VARDATA_ANY(data_val);
			}

			ExecClearTuple(slot);

			for (i = 0; i < nelems; i++)
			{
				int64		child_start = (int64) i * TOAST_MAX_CHUNK_SIZE;
				int64		child_end = child_start + TOAST_MAX_CHUNK_SIZE;

				if (child_end <= req_start || child_start >= req_end)
				{
					/* Chunk does not intersect requested slice */
					*logical_offset = (int32) child_end;
					continue;
				}

				*logical_offset = (int32) child_start;
				toast_fetch_datum_direct_slice_recursive(toastrel,
														 (ItemPointer) DatumGetPointer(elems[i]),
														 result, logical_offset,
														 sliceoffset, slicelength,
														 slot);
				*logical_offset = (int32) child_end;
			}

			if (chunk_size > 0)
			{
				int64		root_start = (int64) nelems * TOAST_MAX_CHUNK_SIZE;
				int64		root_end = root_start + chunk_size;

				if (root_end > req_start && root_start < req_end)
				{
					int32		copy_req_end = (slicelength < 0) ? PG_INT32_MAX : (sliceoffset + slicelength);

					*logical_offset = (int32) root_start;
					toast_slice_copy_chunk(result, chunk_data, chunk_size,
										   logical_offset, sliceoffset, copy_req_end);
				}
				else
					*logical_offset = (int32) root_end;
			}

			if (data_val)
				pfree(data_val);
		}

		pfree(elems);
		pfree(nulls);
		pfree(arr);
	}
}
