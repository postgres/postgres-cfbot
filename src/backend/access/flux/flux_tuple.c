/*-------------------------------------------------------------------------
 *
 * flux_tuple.c
 *	  FLUX tuple handling routines
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_tuple.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/flux.h"
#include "access/flux_xlog.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/toast_helper.h"
#include "access/toast_internals.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "storage/bufpage.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * FluxComputeDataSize
 *
 * Calculate the total on-disk size needed to store a tuple with the given
 * attributes.  This includes the fixed-size FluxTupleHeader, the null
 * bitmap, alignment padding, and all attribute data.
 *
 * Parameters:
 *   tupdesc - tuple descriptor defining the attributes
 *   values  - array of Datum values for each attribute
 *   isnull  - array of boolean null indicators
 *
 * Returns the total size in bytes, including header and alignment.
 */
Size
FluxComputeDataSize(TupleDesc tupdesc, Datum *values, bool *isnull)
{
	Size		data_length = 0;
	Size		bitmap_len;
	int			i;

	Assert(tupdesc != NULL);
	Assert(values != NULL);
	Assert(isnull != NULL);

	/* Calculate null bitmap length */
	bitmap_len = BITMAPLEN(tupdesc->natts);

	/* Start with tuple header size */
	data_length = MAXALIGN(FLUX_TUPLE_OVERHEAD + bitmap_len);

	/* Add space for each attribute */
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		if (!isnull[i])
		{
			Size		attr_len;

			/* Align attribute */
			data_length = att_align_nominal(data_length, att->attalign);

			if (att->attlen > 0)
			{
				/* Fixed-length attribute */
				attr_len = att->attlen;
			}
			else if (att->attlen == -1)
			{
				/* Variable-length attribute */
				attr_len = VARSIZE_ANY(DatumGetPointer(values[i]));
			}
			else if (att->attlen == -2)
			{
				/* C string */
				attr_len = strlen(DatumGetCString(values[i])) + 1;
			}
			else
			{
				elog(ERROR, "unsupported attribute length: %d", att->attlen);
			}

			data_length += attr_len;
		}
	}

	return data_length;
}

/*
 * flux_toast_tuple
 *
 * TOAST the varlena columns of a to-be-stored FLUX tuple, exactly like heap.
 * Used only for logically-logged relations, whose wide values must reach
 * logical decoding as heap TOAST pointers; every other FLUX relation stores
 * wide values in FLUX-native overflow (flux_overflow.c).  Wide values are
 * pushed to the relation's standard heap TOAST table via the AM-agnostic
 * toast_helper routines (toast_tuple_*).  On return, values[]/isnull[] have had any
 * externalized/compressed columns replaced so that the tuple formed from
 * them fits within a FLUX page.
 *
 * oldvalues/oldisnull describe the previous tuple version on UPDATE (so
 * unchanged external datums are reused and superseded ones scheduled for
 * deletion); pass NULL for INSERT.
 *
 * The returned ToastTupleContext must be released with flux_toast_cleanup()
 * after the caller has finished forming and storing the tuple.  *changed is
 * set true iff any column was actually toasted.
 */
void
flux_toast_tuple(Relation rel, Datum *values, bool *isnull,
				 Datum *oldvalues, bool *oldisnull,
				 ToastTupleContext *ttc, ToastAttrInfo *toast_attr,
				 bool *changed, uint32 options)
{
	TupleDesc	tupleDesc = rel->rd_att;
	int			numAttrs = tupleDesc->natts;
	Size		hoff;
	Size		maxDataLen;

	options &= ~HEAP_INSERT_SPECULATIVE;

	ttc->ttc_rel = rel;
	ttc->ttc_values = values;
	ttc->ttc_isnull = isnull;
	ttc->ttc_oldvalues = oldvalues;
	ttc->ttc_oldisnull = oldisnull;
	ttc->ttc_attr = toast_attr;
	toast_tuple_init(ttc);

	/*
	 * Header overhead for a FLUX tuple: fixed header plus null bitmap when
	 * nulls are present.  Convert to a data-size limit.  FLUX targets the
	 * same TOAST_TUPLE_TARGET as heap so wide rows behave identically.
	 */
	hoff = FLUX_TUPLE_OVERHEAD;
	if ((ttc->ttc_flags & TOAST_HAS_NULLS) != 0)
		hoff += BITMAPLEN(numAttrs);
	hoff = MAXALIGN(hoff);
	maxDataLen = RelationGetToastTupleTarget(rel, TOAST_TUPLE_TARGET) - hoff;

	/* Round 1: compress EXTENDED, externalize very large EXTENDED/EXTERNAL */
	while (heap_compute_data_size(tupleDesc, values, isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(ttc, true, false);
		if (biggest_attno < 0)
			break;
		if (TupleDescAttr(tupleDesc, biggest_attno)->attstorage == TYPSTORAGE_EXTENDED)
			toast_tuple_try_compression(ttc, biggest_attno);
		else
			toast_attr[biggest_attno].tai_colflags |= TOASTCOL_INCOMPRESSIBLE;
		if (toast_attr[biggest_attno].tai_size > maxDataLen &&
			rel->rd_rel->reltoastrelid != InvalidOid)
			toast_tuple_externalize(ttc, biggest_attno, options);
	}

	/* Round 2: externalize remaining inline EXTENDED/EXTERNAL */
	while (heap_compute_data_size(tupleDesc, values, isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(ttc, false, false);
		if (biggest_attno < 0)
			break;
		toast_tuple_externalize(ttc, biggest_attno, options);
	}

	/* Round 3: compress MAIN */
	while (heap_compute_data_size(tupleDesc, values, isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(ttc, true, true);
		if (biggest_attno < 0)
			break;
		toast_tuple_try_compression(ttc, biggest_attno);
	}

	/* Round 4: externalize MAIN, at the larger MAIN target */
	maxDataLen = TOAST_TUPLE_TARGET_MAIN - hoff;
	while (heap_compute_data_size(tupleDesc, values, isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(ttc, false, true);
		if (biggest_attno < 0)
			break;
		toast_tuple_externalize(ttc, biggest_attno, options);
	}

	*changed = (ttc->ttc_flags & TOAST_NEEDS_CHANGE) != 0;
}

/*
 * flux_toast_cleanup
 *
 * Release toasting temporaries and delete any superseded external datums,
 * mirroring toast_tuple_cleanup() as used by heap after the new tuple is
 * durably stored.
 */
void
flux_toast_cleanup(ToastTupleContext *ttc)
{
	/*
	 * Not initialised when the caller skipped flux_toast_tuple(): nothing to
	 * do.
	 */
	if (ttc->ttc_rel == NULL)
		return;
	toast_tuple_cleanup(ttc);
}

/*
 * flux_toast_abandon
 *
 * Undo a flux_toast_tuple() whose tuple will not be stored after all: kill
 * the values it externalized (nothing references them) and release the
 * toasting temporaries, but keep the old version's external values.  The
 * values were inserted by the current command, so they are killed the way an
 * aborted speculative insertion kills its TOAST values, not MVCC-deleted.
 */
void
flux_toast_abandon(ToastTupleContext *ttc)
{
	int			numAttrs;

	/* Not initialised when the caller skipped flux_toast_tuple(). */
	if (ttc->ttc_rel == NULL)
		return;
	numAttrs = ttc->ttc_rel->rd_att->natts;

	for (int i = 0; i < numAttrs; i++)
	{
		ToastAttrInfo *attr = &ttc->ttc_attr[i];

		if ((attr->tai_colflags & TOASTCOL_NEEDS_FREE) != 0 &&
			VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(ttc->ttc_values[i])))
			toast_delete_datum(ttc->ttc_rel, ttc->ttc_values[i], true);
	}
	ttc->ttc_flags &= ~TOAST_NEEDS_DELETE_OLD;
	toast_tuple_cleanup(ttc);
}

/*
 * flux_toast_delete
 *
 * Delete the external TOAST datums referenced by a FLUX tuple that is being
 * removed (DELETE, or the old version of an out-of-place UPDATE).
 */
void
flux_toast_delete(Relation rel, Datum *values, bool *isnull,
				  bool is_speculative)
{
	toast_delete_external(rel, values, isnull, is_speculative);
}

/*
 * FluxFormTuple
 *
 * Create a new FLUX tuple from the given attribute values and null indicators.
 * Allocates memory for the FluxTupleData wrapper and the on-disk
 * FluxTupleHeader + attribute data.
 *
 * When compression is enabled (flux_enable_compression GUC), variable-length
 * attributes exceeding FLUX_MIN_COMPRESS_SIZE (32 bytes) are automatically
 * compressed using the algorithm selected by FluxChooseCompressionType().
 * Compressed attributes are stored with a FluxCompressionHeader prefix and
 * the tuple's FLUX_INFOMASK_COMPRESSED bit is set.
 *
 * When a relation is provided, large attributes exceeding FLUX_OVERFLOW_THRESHOLD
 * are automatically stored in overflow pages via FluxStoreOverflowColumn, which
 * self-logs each chunk under its own lock before this tuple is formed.
 *
 * Parameters:
 *   tupdesc - tuple descriptor defining the schema
 *   values  - array of Datum values for each attribute
 *   isnull  - array of boolean null indicators
 *   rel     - relation for overflow storage (NULL to disable overflow handling)
 *   overflow_buffers - overflow write context carrying avoid_block (NULL if
 *                      rel is NULL)
 *
 * Returns a palloc'd FluxTuple.  The caller is responsible for freeing it
 * with FluxFreeTuple() when done.
 */
static FluxTuple flux_form_tuple_internal(TupleDesc tupdesc, Datum *values,
										  bool *isnull, Relation rel,
										  FluxOverflowBuffers *overflow_buffers,
										  bool force_shrink,
										  const FluxOverflowPtr *old_ovptrs,
										  const bool *old_ovpresent);

/*
 * flux_overflow_prefix_len
 *		Choose the inline-prefix length for an overflow reference, making the
 *		tuple ref VARIABLE-WIDTH: a value only slightly over the overflow
 *		threshold pays NO prefix (ref is the minimum width), while larger values
 *		keep the full FLUX_OVERFLOW_DEFAULT_PREFIX so a prefix-only comparison
 *		can avoid a chunk fetch.  force-shrink always uses zero prefix.
 *
 * The locator struct (FluxOverflowPtr) is a fixed 20 bytes BY DESIGN: that is
 * the force-shrink MAX ref width (prefix == 0), and FLUX_OVERFLOW_PTR_SIZE must
 * stay within the bound the in-place UPDATE refit math assumes (flux.h:369),
 * so a force-shrunk column never grows past its current footprint.  The
 * VARIABLE part is the inline prefix, so the on-disk ref ranges from
 * VARHDRSZ+20 (tiny value / force-shrink) up to
 * VARHDRSZ+20+FLUX_OVERFLOW_DEFAULT_PREFIX, never exceeding the force-shrink
 * bound.
 */
static Size
flux_overflow_prefix_len(bool force_shrink, Size vlen)
{
	if (force_shrink)
		return 0;
	/* Within 2x the threshold the whole value is cheap to fetch: no prefix. */
	if (vlen <= (Size) FLUX_OVERFLOW_THRESHOLD * 2)
		return 0;
	return Min((Size) FLUX_OVERFLOW_DEFAULT_PREFIX, vlen);
}

FluxTuple
FluxFormTuple(TupleDesc tupdesc, Datum *values, bool *isnull,
			  Relation rel, FluxOverflowBuffers *overflow_buffers)
{
	return flux_form_tuple_internal(tupdesc, values, isnull, rel,
									overflow_buffers, false, NULL, NULL);
}

/*
 * FluxFormTupleUpdate
 *
 * Like FluxFormTuple, but for the in-place UPDATE path.  old_ovptrs and
 * old_ovpresent (indexed by attnum, natts entries) describe the OLD tuple's
 * on-page overflow pointers, collected while its buffer was still locked.  Any
 * over-threshold varlena whose content hash matches the old pointer's stored
 * hash (and byte-verifies equal) is COW-referenced against the existing
 * overflow value instead of being re-stored, avoiding needless disk growth and
 * WAL.  Pass NULL arrays to disable this (identical to FluxFormTuple).
 */
FluxTuple
FluxFormTupleUpdate(TupleDesc tupdesc, Datum *values, bool *isnull,
					Relation rel, FluxOverflowBuffers *overflow_buffers,
					const FluxOverflowPtr *old_ovptrs,
					const bool *old_ovpresent)
{
	return flux_form_tuple_internal(tupdesc, values, isnull, rel,
									overflow_buffers, false,
									old_ovptrs, old_ovpresent);
}

/*
 * FluxFormTupleForceShrink
 *
 * Like FluxFormTuple, but forces every inline varlena attribute larger than
 * an overflow pointer off-page with a zero inline prefix, regardless of the
 * normal FLUX_OVERFLOW_THRESHOLD.  This shrinks the main tuple to its minimum
 * footprint (header + fixed columns + one overflow pointer per large varlena).
 *
 * Used as a last resort by the in-place UPDATE path: when an updated tuple has
 * grown beyond the space available on its page and TID stability forbids
 * relocating it, pushing its variable-length data off-page lets the main tuple
 * fit back into (or near) its original slot.  A relation and overflow_buffers
 * are mandatory because every forced column is written to overflow pages.
 */
FluxTuple
FluxFormTupleForceShrink(TupleDesc tupdesc, Datum *values, bool *isnull,
						 Relation rel, FluxOverflowBuffers *overflow_buffers)
{
	Assert(rel != NULL);
	Assert(overflow_buffers != NULL);
	return flux_form_tuple_internal(tupdesc, values, isnull, rel,
									overflow_buffers, true, NULL, NULL);
}

static FluxTuple
flux_form_tuple_internal(TupleDesc tupdesc, Datum *values, bool *isnull,
						 Relation rel, FluxOverflowBuffers *overflow_buffers,
						 bool force_shrink,
						 const FluxOverflowPtr *old_ovptrs,
						 const bool *old_ovpresent)
{
	FluxTuple	tuple;
	FluxTupleHeader *header;
	Size		data_length;
	Size		tuple_length;
	Size		bitmap_len;
	char	   *data_ptr;
	uint8	   *nulls_bitmap;
	int			i;
	bool		has_nulls = false;
	bool		has_varwidth = false;
	bool		has_external = false;
	bool		has_compressed = false;
	bool		has_overflow = false;

	/*
	 * Working arrays for compressed/overflowed values. We attempt compression
	 * and overflow first, then compute the final tuple size using the
	 * (possibly compressed/overflowed) attribute values.
	 *
	 * Use stack arrays for small tuples (common OLTP case) to avoid palloc.
	 */
#define FLUX_FORM_STACK_ATTRS	16
	Datum	   *work_values;
	bool	   *is_compressed;	/* Track which attrs were compressed */
	bool	   *is_overflowed;	/* Track which attrs were overflowed */
	Datum		work_values_stack[FLUX_FORM_STACK_ATTRS];
	bool		is_compressed_stack[FLUX_FORM_STACK_ATTRS];
	bool		is_overflowed_stack[FLUX_FORM_STACK_ATTRS];

	Assert(tupdesc != NULL);
	Assert(values != NULL);
	Assert(isnull != NULL);

	if (tupdesc->natts <= FLUX_FORM_STACK_ATTRS)
	{
		work_values = work_values_stack;
		is_compressed = is_compressed_stack;
		is_overflowed = is_overflowed_stack;
		memset(is_compressed, 0, tupdesc->natts * sizeof(bool));
		memset(is_overflowed, 0, tupdesc->natts * sizeof(bool));
	}
	else
	{
		work_values = (Datum *) palloc(tupdesc->natts * sizeof(Datum));
		is_compressed = (bool *) palloc0(tupdesc->natts * sizeof(bool));
		is_overflowed = (bool *) palloc0(tupdesc->natts * sizeof(bool));
	}
	memcpy(work_values, values, tupdesc->natts * sizeof(Datum));

	/*
	 * Phase 1b: FLUX-native column overflow.
	 *
	 * When a relation and an overflow_buffers context are supplied, push each
	 * over-threshold varlena column off the main tuple into FLUX overflow
	 * records (DATA chunks + a ROOT directory) on FLUX-managed pages, and
	 * replace the inline value with a compact FluxOverflowPtr.  Each chunk is
	 * self-logged under its own lock and released before the next (so no
	 * chunk page is held when the caller later locks its main tuple page).
	 * Overflow records share the parent version's visibility (no MVCC fields
	 * of their own).
	 *
	 * Eligibility mirrors heap TOAST: only varlena (attlen == -1) columns
	 * with a storage mode other than PLAIN.  force_shrink lowers the bar to
	 * "wider than an overflow pointer" so an in-place UPDATE can refit its
	 * slot.
	 *
	 * Exception: a logically-logged relation keeps wide values on the
	 * standard heap TOAST path (the caller runs flux_toast_tuple first).
	 * Logical decoding must resolve a wide value WITHOUT a FLUX relation
	 * handle, and PostgreSQL's reorderbuffer requires a large logical value
	 * to travel as an EXTERNAL (heap-TOAST) pointer the output plugin
	 * detoasts -- it rejects a multi-megabyte inline logical tuple (Assert
	 * t_len <= MaxHeapTupleSize in ReorderBufferToastReplace).  A FLUX-native
	 * overflow pointer is neither, so it could not be decoded.  Declining
	 * native overflow here leaves the value as a heap-TOAST pointer, which
	 * both the read path and decoding resolve.
	 */
	if (rel != NULL && overflow_buffers != NULL &&
		!RelationIsLogicallyLogged(rel))
	{
		Size		threshold = force_shrink ? sizeof(FluxOverflowPtr)
			: FLUX_OVERFLOW_THRESHOLD;

		/*
		 * Two passes.  Pass 1 detoasts each eligible column and resolves the
		 * COW re-reference decision, which READS the old value's chunks via
		 * FluxFetchOverflowColumn.  Pass 2 WRITES the new chunks via the
		 * self-logged path (one chunk-page lock at a time, released before
		 * the next).  They must not interleave: a Pass-2 store briefly locks
		 * a page a later Pass-1 verify would share-lock to read an old chunk,
		 * and although each is released promptly, doing all reads first keeps
		 * the COW compare simple and avoids re-reading a page mid-write.
		 */
		Datum	   *ov_detoasted = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
		bool	   *ov_tostore = (bool *) palloc0(tupdesc->natts * sizeof(bool));

		for (i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			struct varlena *vl;
			struct varlena *detoasted;
			Size		vlen;
			const FluxOverflowPtr *reuse = NULL;

			if (isnull[i] || att->attisdropped || att->attlen != -1)
				continue;
			if (att->attstorage == TYPSTORAGE_PLAIN)
				continue;

			vl = (struct varlena *) DatumGetPointer(work_values[i]);

			/* An already-stored overflow pointer passes straight through. */
			if (FluxIsOverflowPtr(vl))
				continue;

			/* Detoast first: FLUX overflow stores plain uncompressed bytes. */
			detoasted = pg_detoast_datum(vl);
			vlen = VARSIZE_ANY_EXHDR(detoasted);
			if (vlen < threshold)
			{
				if (detoasted != vl)
					pfree(detoasted);
				continue;
			}

			/*
			 * COW re-reference (in-place UPDATE): if the old tuple had an
			 * overflow value for this attr whose content hash matches and
			 * byte-verifies equal, reuse the existing chunks instead of
			 * rewriting them -- avoids needless disk growth and WAL.
			 */
			if (old_ovptrs != NULL && old_ovpresent != NULL && old_ovpresent[i])
			{
				const FluxOverflowPtr *oldp = &old_ovptrs[i];
				uint32		h = hash_bytes((const unsigned char *) VARDATA_ANY(detoasted),
										   vlen);

				if (oldp->ov_total_length == vlen &&
					oldp->ov_content_hash == h)
					reuse = oldp;
			}

			if (reuse != NULL)
			{
				/*
				 * Byte-verify the candidate before trusting the hash: fetch
				 * the old value and compare.  On mismatch fall through to
				 * re-store.
				 */
				struct varlena *refvl;
				Datum		oldfull;
				bool		same = false;
				FluxOverflowPtr tmp = *reuse;
				char		refbuf[VARHDRSZ + sizeof(FluxOverflowPtr)];

				refvl = (struct varlena *) refbuf;
				SET_VARSIZE(refvl, VARHDRSZ + sizeof(FluxOverflowPtr));
				memcpy(VARDATA(refvl), &tmp, sizeof(FluxOverflowPtr));
				oldfull = FluxFetchOverflowColumn(rel, refvl);
				if (VARSIZE_ANY_EXHDR(DatumGetPointer(oldfull)) == vlen &&
					memcmp(VARDATA_ANY(DatumGetPointer(oldfull)),
						   VARDATA_ANY(detoasted), vlen) == 0)
					same = true;
				pfree(DatumGetPointer(oldfull));

				if (same)
				{
					/* Reuse old chunks: emit a ref varlena to the same root. */
					Size		pfx = flux_overflow_prefix_len(force_shrink, vlen);
					Size		sz = VARHDRSZ + sizeof(FluxOverflowPtr) + pfx;
					struct varlena *r = (struct varlena *) palloc0(sz);
					FluxOverflowPtr *op = (FluxOverflowPtr *) VARDATA(r);

					SET_VARSIZE(r, sz);
					*op = tmp;
					op->ov_inline_prefix = (uint16) pfx;
					if (pfx > 0)
						memcpy((char *) op + sizeof(FluxOverflowPtr),
							   VARDATA_ANY(detoasted), pfx);
					if (detoasted != vl)
						pfree(detoasted);
					work_values[i] = PointerGetDatum(r);
					is_overflowed[i] = true;
					has_overflow = true;
					continue;
				}
			}

			/* Not reused: remember the detoasted value for Pass 2. */
			ov_detoasted[i] = PointerGetDatum(detoasted);
			ov_tostore[i] = true;
		}

		/* Pass 2: write the new chunks (each self-logged under its own lock). */
		for (i = 0; i < tupdesc->natts; i++)
		{
			struct varlena *vl;
			struct varlena *detoasted;
			Size		vlen;
			Size		prefix;
			Datum		ref;

			if (!ov_tostore[i])
				continue;

			vl = (struct varlena *) DatumGetPointer(work_values[i]);
			detoasted = (struct varlena *) DatumGetPointer(ov_detoasted[i]);
			vlen = VARSIZE_ANY_EXHDR(detoasted);
			prefix = flux_overflow_prefix_len(force_shrink, vlen);
			ref = FluxStoreOverflowColumn(rel, PointerGetDatum(detoasted),
										  i, prefix, overflow_buffers);
			if (detoasted != vl)
				pfree(detoasted);
			work_values[i] = ref;
			is_overflowed[i] = true;
			has_overflow = true;
		}

		pfree(ov_detoasted);
		pfree(ov_tostore);
	}

	/*
	 * Any remaining wide varlena values (e.g. logically-logged relations, or
	 * values the overflow path declined) are TOASTed by the caller via the
	 * standard heap TOAST path before reaching here.  FLUX does not compress
	 * attributes.
	 */

	/*
	 * Phase 2: Calculate total space needed
	 */
	data_length = FluxComputeDataSize(tupdesc, work_values, isnull);
	tuple_length = data_length;

	/* Allocate tuple */
	tuple = (FluxTuple) palloc0(sizeof(FluxTupleData));
	tuple->t_len = tuple_length;
	tuple->t_data = (FluxTupleHeader *) palloc0(tuple_length);

	/* Set up header */
	header = tuple->t_data;
	header->t_natts = tupdesc->natts;
	header->t_flags = 0;
	header->t_commit_ts = 0;	/* Will be set during insert */
	ItemPointerSetInvalid(&header->t_ctid);
	header->t_infomask = 0;

	if (has_compressed)
	{
		header->t_flags |= FLUX_TUPLE_COMPRESSED;
		header->t_infomask |= FLUX_INFOMASK_COMPRESSED;
	}
	(void) has_compressed;

	/* Set up null bitmap */
	bitmap_len = BITMAPLEN(tupdesc->natts);
	nulls_bitmap = (uint8 *) header->t_attrs_bitmap;
	data_ptr = (char *) header + MAXALIGN(FLUX_TUPLE_OVERHEAD + bitmap_len);

	/*
	 * Initialize null bitmap - PostgreSQL expects all bits set to 1 initially
	 * (all NOT NULL)
	 */
	memset(nulls_bitmap, 0xFF, bitmap_len);

	/* Set infomask bits */
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		/*
		 * Dropped columns are never given storage in a newly formed tuple
		 * (FluxComputeDataSize and the store loop below both skip them), so
		 * they must be recorded as NULL in the bitmap.  Otherwise the deform
		 * side would think the bytes are present and read every following
		 * attribute from the wrong offset.
		 */
		if (isnull[i] || att->attisdropped)
		{
			has_nulls = true;
			/* Clear the bit for NULL attributes (bit=0 means NULL) */
			nulls_bitmap[i >> 3] &= ~(1 << (i & 0x07));
		}
		else
		{
			if (att->attlen == -1 || att->attlen == -2)
				has_varwidth = true;

			/* Check for external storage */
			if (att->attlen == -1 && VARATT_IS_EXTERNAL(DatumGetPointer(work_values[i])))
				has_external = true;
		}
	}

	if (has_nulls)
		header->t_infomask |= FLUX_INFOMASK_HASNULL;
	if (has_varwidth)
		header->t_infomask |= FLUX_INFOMASK_HASVARWIDTH;
	if (has_external)
		header->t_infomask |= FLUX_INFOMASK_HASEXTERNAL;
	if (has_overflow)
	{
		header->t_flags |= FLUX_TUPLE_HAS_OVERFLOW;
		header->t_infomask |= FLUX_INFOMASK_HASOVERFLOW;
	}

	/*
	 * Phase 3: Store attribute values (using compressed data where
	 * applicable)
	 */
	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped || isnull[i])
			continue;

		/* Align attribute */
		data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);

		if (att->attlen > 0)
		{
			/*
			 * Fixed-length attribute - never compressed. Must distinguish
			 * byval from by-reference fixed-length types (e.g., timetz is 12
			 * bytes but passed by reference).
			 */
			if (att->attbyval)
				store_att_byval(data_ptr, work_values[i], att->attlen);
			else
				memcpy(data_ptr, DatumGetPointer(work_values[i]), att->attlen);
			data_ptr += att->attlen;
		}
		else if (att->attlen == -1)
		{
			/* Variable-length attribute (possibly compressed) */
			Size		attr_len = VARSIZE_ANY(DatumGetPointer(work_values[i]));

			memcpy(data_ptr, DatumGetPointer(work_values[i]), attr_len);
			data_ptr += attr_len;
		}
		else if (att->attlen == -2)
		{
			/* C string */
			Size		attr_len = strlen(DatumGetCString(work_values[i])) + 1;

			memcpy(data_ptr, DatumGetCString(work_values[i]), attr_len);
			data_ptr += attr_len;
		}
	}

	/*
	 * Free compressed and overflow datums that were allocated by
	 * FluxCompressAttribute and FluxStoreOverflowColumn
	 */
	for (i = 0; i < tupdesc->natts; i++)
	{
		if (is_compressed[i] || is_overflowed[i])
			pfree(DatumGetPointer(work_values[i]));
	}
	if (tupdesc->natts > FLUX_FORM_STACK_ATTRS)
	{
		pfree(work_values);
		pfree(is_compressed);
		pfree(is_overflowed);
	}

	return tuple;
}

/*
 * FluxDeformTuple
 *
 * Extract attribute values and null indicators from a FLUX tuple into the
 * provided arrays.  This is the inverse of FluxFormTuple().
 *
 * When the tuple has the FLUX_INFOMASK_COMPRESSED flag set, variable-length
 * attributes may contain a FluxCompressionHeader prefix followed by
 * compressed data.  This function transparently decompresses such attributes
 * so that callers always see the original uncompressed Datum values.
 *
 * Parameters:
 *   tuple   - the FLUX tuple to deform
 *   tupdesc - tuple descriptor defining the schema
 *   values  - output array of Datum values (must be pre-allocated)
 *   isnull  - output array of boolean null indicators (must be pre-allocated)
 */
void
FluxDeformTuple(Relation rel, FluxTuple tuple, TupleDesc tupdesc, Datum *values, bool *isnull)
{
	FluxDeformTupleUpTo(rel, tuple, tupdesc, values, isnull, tupdesc->natts);
}

/*
 * FluxDeformTupleUpTo
 *
 * Like FluxDeformTuple(), but stops after extracting the first max_natts
 * attributes.  Because FLUX (like heap) stores variable-length attributes
 * consecutively, deforming is inherently sequential: extracting attribute N
 * still costs walking attributes 0..N.  But a caller that only needs the
 * low-numbered attributes (e.g. flux_indexed_attr_changed comparing indexed
 * columns, which are typically the leading columns) can cap the walk at the
 * highest attribute it will read instead of deforming every column of a wide
 * tuple.  Attributes at and beyond max_natts are returned as NULL/0 so a
 * caller that accidentally reads past its bound gets a defined (not garbage)
 * value; callers MUST NOT rely on those tail values.
 */
void
FluxDeformTupleUpTo(Relation rel, FluxTuple tuple, TupleDesc tupdesc,
					Datum *values, bool *isnull, int max_natts)
{
	FluxTupleHeader *header;
	uint8	   *nulls_bitmap;
	char	   *data_ptr;
	Size		bitmap_len;
	int			i;
	bool		tuple_has_compressed;
	bool		tuple_has_overflow;

	Assert(tuple != NULL);
	Assert(tupdesc != NULL);
	Assert(values != NULL);
	Assert(isnull != NULL);

	header = tuple->t_data;

	/*
	 * Use the tuple's actual natts for bitmap_len and data_ptr calculation.
	 * After ALTER TABLE ADD COLUMN, old tuples may have fewer attributes.
	 */
	{
		int			tuple_natts = header->t_natts;
		int			loop_natts = Min(tupdesc->natts, tuple_natts);

		/* Cap the deform at the caller's requested bound. */
		if (max_natts >= 0 && max_natts < loop_natts)
			loop_natts = max_natts;

		bitmap_len = BITMAPLEN(tuple_natts);
		nulls_bitmap = (uint8 *) header->t_attrs_bitmap;
		data_ptr = (char *) header + MAXALIGN(FLUX_TUPLE_OVERHEAD + bitmap_len);

		tuple_has_compressed = false;	/* FLUX does not compress attributes */
		tuple_has_overflow = (header->t_flags & FLUX_TUPLE_HAS_OVERFLOW) != 0;
		(void) tuple_has_compressed;

		/* Extract each attribute present in the tuple */
		for (i = 0; i < loop_natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (att->attisdropped)
			{
				values[i] = (Datum) 0;
				isnull[i] = true;

				/*
				 * The dropped column's bytes are still physically present in
				 * tuples written before the DROP, so step over them (see the
				 * matching comment in tts_flux_deform()).
				 */
				if (!((header->t_infomask & FLUX_INFOMASK_HASNULL) &&
					  att_isnull(i, nulls_bitmap)))
				{
					data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);
					data_ptr = (char *) att_addlength_pointer(data_ptr, att->attlen,
															  data_ptr);
				}
				continue;
			}

			/*
			 * Check null bitmap: bit=0 means NULL (bit cleared in
			 * FluxFormTuple)
			 */
			if (header->t_infomask & FLUX_INFOMASK_HASNULL &&
				att_isnull(i, nulls_bitmap))
			{
				values[i] = (Datum) 0;
				isnull[i] = true;
				continue;
			}

			isnull[i] = false;

			/* Align attribute */
			data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);

			if (att->attlen > 0)
			{
				/*
				 * Fixed-length attribute - never compressed. Use actual
				 * attbyval flag (e.g., timetz is 12 bytes but by-ref).
				 */
				values[i] = fetch_att(data_ptr, att->attbyval, att->attlen);
				data_ptr += att->attlen;
			}
			else if (att->attlen == -1)
			{
				/* Variable-length attribute */
				Size		attr_len = VARSIZE_ANY(data_ptr);

				/*
				 * A FLUX-native overflow value is held inline as a compact
				 * FluxOverflowPtr; resolve it to the full value by reading
				 * its chunks (like the normal read path) when a relation
				 * handle is available, so callers -- notably the
				 * logical-decode image builder and replica-identity -- get
				 * the real bytes, never the raw pointer.  rel == NULL
				 * (callers that only read narrow leading key columns) leaves
				 * the pointer verbatim.  The chunks must be on disk and
				 * unlocked here; the fresh-insert path, whose chunks are
				 * still held locked, builds its image from the full
				 * pre-overflow values instead (see
				 * FluxXLogPrepareLogicalImage*).
				 */
				if (tuple_has_overflow && rel != NULL &&
					FluxIsOverflowPtr(data_ptr))
					values[i] = FluxFetchOverflowColumn(rel, data_ptr);
				else
					values[i] = PointerGetDatum(data_ptr);
				data_ptr += attr_len;
			}
			else if (att->attlen == -2)
			{
				/* C string - never compressed */
				values[i] = CStringGetDatum(data_ptr);
				data_ptr += strlen(data_ptr) + 1;
			}
			else
			{
				elog(ERROR, "unsupported attribute length: %d", att->attlen);
			}
		}

		/*
		 * Fill missing attributes with defaults for columns added by ALTER
		 * TABLE ADD COLUMN after this tuple was stored.
		 */
		for (i = loop_natts; i < tupdesc->natts; i++)
		{
			if (i >= tuple_natts && (max_natts < 0 || i < max_natts) &&
				!TupleDescAttr(tupdesc, i)->attisdropped)
				values[i] = getmissingattr(tupdesc, i + 1, &isnull[i]);
			else
			{
				values[i] = (Datum) 0;
				isnull[i] = true;
			}
		}
	}							/* end of tuple_natts scope block */
}

/*
 * FluxFreeTuple
 *
 * Free a FLUX tuple and its associated data.  Safe to call with NULL.
 *
 * Parameters:
 *   tuple - the FluxTuple to free (may be NULL)
 */
void
FluxFreeTuple(FluxTuple tuple)
{
	if (tuple)
	{
		if (tuple->t_data)
			pfree(tuple->t_data);
		pfree(tuple);
	}
}

/*
 * FluxInitPage
 *
 * Initialize a new FLUX page.  Calls PostgreSQL's PageInit() with space
 * reserved for FluxPageOpaqueData in the special area, then initializes
 * the opaque data fields to their default values.
 *
 * Parameters:
 *   page     - pointer to the page buffer
 *   pageSize - size of the page (typically BLCKSZ = 8192)
 */
void
FluxInitPage(Page page, Size pageSize)
{
	FluxPageOpaque phdr;

	PageInit(page, pageSize, sizeof(FluxPageOpaqueData));

	phdr = FluxPageGetOpaque(page);
	phdr->pd_commit_ts_and_flags = 0;
}

/*
 * FluxPageAddTuple
 *
 * Add a FLUX tuple to a page using PageAddItem().  Updates the page's
 * opaque data (commit timestamp, free space) after successful insertion.
 *
 * Parameters:
 *   page       - the page to add the tuple to (must be exclusively locked)
 *   tuple      - the FLUX tuple to add
 *   tuple_size - size of the tuple data in bytes
 *
 * Returns the OffsetNumber where the tuple was placed, or
 * InvalidOffsetNumber if the page does not have enough space.
 */
OffsetNumber
FluxPageAddTuple(Page page, FluxTuple tuple, Size tuple_size)
{
	FluxPageOpaque phdr;
	OffsetNumber offnum;

	/*
	 * Like heap, never place a tuple past MaxHeapTuplesPerPage: tidbitmap.c
	 * and other AM-agnostic code size per-page offset arrays for that.
	 * PageGetHeapFreeSpace() is zero once the line-pointer array is full and
	 * none is reusable (LP_DEAD pointers left by pruning wait for VACUUM);
	 * fail quietly then, rather than through PageAddItemExtended's WARNING.
	 */
	if (PageGetHeapFreeSpace(page) == 0)
		return InvalidOffsetNumber;

	offnum = PageAddItemExtended(page, tuple->t_data, tuple_size,
								 InvalidOffsetNumber, PAI_IS_HEAP);

	if (offnum == InvalidOffsetNumber)
		return InvalidOffsetNumber;

	/* Update page header */
	phdr = FluxPageGetOpaque(page);

	/* Mark page for defragmentation if fragmented */
	if (PageGetFreeSpace(page) >= tuple_size * 2 &&
		PageGetMaxOffsetNumber(page) > FirstOffsetNumber + 5)
	{
		FluxPageSetFlag(phdr, FLUX_PAGE_DEFRAG_NEEDED);
	}

	return offnum;
}

/*
 * FluxPageUpdateTuple
 *
 * Attempt to update a tuple in place on a FLUX page.  If the new tuple
 * fits within the existing allocation (same size or smaller), the data is
 * overwritten directly (in-place update).  If the new tuple is larger but
 * the page has enough total free space, the old tuple is removed and the
 * new tuple is added at the same or a new offset.
 *
 * Parameters:
 *   page           - the page containing the tuple (must be exclusively locked)
 *   offnum         - offset number of the tuple to update
 *   new_tuple      - the new tuple data
 *   old_commit_ts  - commit timestamp of the old version (for WAL logging)
 *   new_commit_ts  - commit timestamp for the new version
 *
 * Returns true if the update was performed on this page, false if the new
 * tuple does not fit (caller must handle cross-page update).
 */
bool
FluxPageUpdateTuple(Page page, OffsetNumber offnum, FluxTuple new_tuple,
					uint64 old_commit_ts, uint64 new_commit_ts)
{
	ItemId		itemid;
	FluxTupleHeader *old_tuple;
	Size		old_size,
				new_size;
	FluxPageOpaque phdr;
	Size		available_space;
	OffsetNumber new_offnum;

	itemid = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(itemid))
		return false;

	old_tuple = (FluxTupleHeader *) PageGetItem(page, itemid);
	old_size = ItemIdGetLength(itemid);
	new_size = new_tuple->t_len;

	/* Check if new tuple fits in same space */
	if (new_size <= old_size)
	{
		/* In-place update */
		memcpy(old_tuple, new_tuple->t_data, new_size);
		if (new_size < old_size)
		{
			/* Update item length */
			ItemIdSetNormal(itemid, ItemIdGetOffset(itemid), new_size);
		}

		/* Update page header */
		phdr = FluxPageGetOpaque(page);
		FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), new_commit_ts));

		return true;
	}

	/* Need more space - check if available */
	available_space = PageGetFreeSpace(page) + old_size;
	if (new_size <= available_space)
	{
		/*
		 * Remove old tuple and re-add the new (larger) one.
		 *
		 * We use FluxPageIndexTupleDelete instead of PageIndexTupleDelete
		 * because the page may contain LP_UNUSED items from defragmentation.
		 * PageIndexTupleDelete asserts all items are LP_NORMAL;
		 * FluxPageIndexTupleDelete skips LP_UNUSED items safely.
		 */
		FluxPageIndexTupleDelete(page, offnum);

		new_offnum = PageAddItem(page, new_tuple->t_data,
								 new_size, offnum,
								 false, true);

		if (new_offnum != InvalidOffsetNumber)
		{
			/* Update page header */
			phdr = FluxPageGetOpaque(page);
			FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), new_commit_ts));
			return true;
		}
	}

	return false;				/* Update failed - need new page */
}

/*
 * Get number of live tuples on a FLUX page
 */
int
FluxPageGetLiveTuples(Page page, uint64 snapshot_ts)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	int			live_tuples = 0;
	OffsetNumber offnum;

	for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum++)
	{
		ItemId		itemid = PageGetItemId(page, offnum);

		if (ItemIdIsNormal(itemid))
		{
			FluxTupleHeader *tuple = (FluxTupleHeader *) PageGetItem(page, itemid);

			/*
			 * Heap-shaped: "live" here means not carrying a committed delete
			 * marker.  This helper is only an estimate (no current callers),
			 * so it counts non-DELETED tuples rather than doing a full
			 * snapshot visibility check.  snapshot_ts is unused.
			 */
			(void) snapshot_ts;
			if (!(tuple->t_flags & FLUX_TUPLE_DELETED))
				live_tuples++;
		}
	}

	return live_tuples;
}

/*
 * FluxPageDefragment
 *
 * Compact a FLUX page by calling PageRepairFragmentation() to consolidate
 * free space.  Updates the page opaque data with the new free space amount,
 * increments the defrag counter, and clears the FLUX_PAGE_DEFRAG_NEEDED flag.
 *
 * Parameters:
 *   page - the page to defragment (must be exclusively locked)
 */
void
FluxPageDefragment(Page page)
{
	FluxPageOpaque phdr = FluxPageGetOpaque(page);

	/* Use standard PageRepairFragmentation */
	PageRepairFragmentation(page);

	/* Update page header */
	FluxPageClearFlag(phdr, FLUX_PAGE_DEFRAG_NEEDED);
}

/*
 * FluxPageIndexTupleDelete
 *
 * Like PageIndexTupleDelete, but tolerates LP_UNUSED items on the page.
 *
 * Standard PageIndexTupleDelete asserts that ALL line pointers have storage
 * (ItemIdHasStorage).  FLUX pages may contain LP_UNUSED items left behind
 * by opportunistic defragmentation.  This function skips LP_UNUSED items
 * when adjusting offsets, preventing both assertion failures and data
 * corruption (LP_UNUSED items have lp_off=0 and must not be adjusted).
 */
void
FluxPageIndexTupleDelete(Page page, OffsetNumber offnum)
{
	PageHeader	phdr = (PageHeader) page;
	char	   *addr;
	ItemId		tup;
	Size		size;
	unsigned	offset;
	int			nbytes;
	int			offidx;
	int			nline;

	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ ||
		phdr->pd_special != MAXALIGN(phdr->pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	nline = PageGetMaxOffsetNumber(page);
	if ((int) offnum <= 0 || (int) offnum > nline)
		elog(ERROR, "invalid index offnum: %u", offnum);

	offidx = offnum - 1;

	tup = PageGetItemId(page, offnum);
	Assert(ItemIdHasStorage(tup));
	size = ItemIdGetLength(tup);
	offset = ItemIdGetOffset(tup);

	if (offset < phdr->pd_upper || (offset + size) > phdr->pd_special ||
		offset != MAXALIGN(offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted line pointer: offset = %u, size = %zu",
						offset, size)));

	size = MAXALIGN(size);

	/* Remove the line pointer entry by shifting subsequent entries down */
	nbytes = phdr->pd_lower -
		((char *) &phdr->pd_linp[offidx + 1] - (char *) phdr);

	if (nbytes > 0)
		memmove(&(phdr->pd_linp[offidx]),
				&(phdr->pd_linp[offidx + 1]),
				nbytes);

	/* Shift tuple data forward to fill the gap */
	addr = (char *) page + phdr->pd_upper;

	if (offset > phdr->pd_upper)
		memmove(addr + size, addr, offset - phdr->pd_upper);

	phdr->pd_upper += size;
	phdr->pd_lower -= sizeof(ItemIdData);

	/* Adjust remaining line pointer offsets, skipping LP_UNUSED items */
	if (!PageIsEmpty(page))
	{
		int			i;

		nline--;
		for (i = 1; i <= nline; i++)
		{
			ItemId		ii = PageGetItemId(page, i);

			if (!ItemIdHasStorage(ii))
				continue;
			if (ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size;
		}
	}
}

/*
 * Convert a FLUX tuple to a TupleTableSlot
 *
 * This is the primary retrieval path used during sequential scans.
 * When the tuple has compressed attributes (FLUX_INFOMASK_COMPRESSED),
 * they are transparently decompressed so the slot always contains
 * uncompressed data visible to the executor.
 *
 * Overflow attributes (FLUX_INFOMASK_HASOVERFLOW) are returned as-is
 * by this function since it has no Relation handle.  Use
 * FluxTupleToSlotWithOverflow() for transparent overflow fetching.
 */
bool
FluxTupleToSlot(FluxTupleHeader *tuple_header, TupleTableSlot *slot)
{
	return FluxTupleToSlotWithOverflow(tuple_header, slot, NULL);
}

/*
 * Convert a FLUX tuple to a TupleTableSlot with overflow support.
 *
 * When rel is non-NULL and the tuple has overflow attributes, they are
 * transparently fetched from overflow records and the slot receives the
 * complete original values.  When rel is NULL, overflow pointers are
 * returned as-is (same as FluxTupleToSlot).
 */
bool
FluxTupleToSlotWithOverflow(FluxTupleHeader *tuple_header,
							TupleTableSlot *slot, Relation rel)
{
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	char	   *data_ptr;
	uint8	   *nulls_bitmap;
	int			i;
	Size		bitmap_len;
	bool		tuple_has_compressed;
	bool		tuple_has_overflow;

	if (!tuple_header)
		return false;

	/* Check if tuple is deleted */
	if (tuple_header->t_flags & FLUX_TUPLE_DELETED)
		return false;

	/* Clear the slot first */
	ExecClearTuple(slot);

	/*
	 * Use the tuple's actual natts for bitmap_len and data_ptr calculation.
	 * After ALTER TABLE ADD COLUMN, old tuples may have fewer attributes than
	 * the current schema expects.
	 */
	{
		int			tuple_natts = tuple_header->t_natts;
		int			loop_natts = Min(tupdesc->natts, tuple_natts);

		bitmap_len = BITMAPLEN(tuple_natts);

		/* Set up pointers to data */
		nulls_bitmap = (uint8 *) tuple_header->t_attrs_bitmap;
		data_ptr = (char *) tuple_header + MAXALIGN(FLUX_TUPLE_OVERHEAD + bitmap_len);

		tuple_has_compressed = false;	/* FLUX does not compress attributes */
		tuple_has_overflow = (tuple_header->t_flags & FLUX_TUPLE_HAS_OVERFLOW) != 0;
		(void) tuple_has_compressed;

		/* Decode each attribute present in the tuple */
		for (i = 0; i < loop_natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			bool		is_null;

			if (att->attisdropped)
			{
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;

				/*
				 * The dropped column's bytes are still physically present in
				 * tuples written before the DROP, so step over them (see the
				 * matching comment in tts_flux_deform()).
				 */
				if (!att_isnull(i, nulls_bitmap))
				{
					data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);
					data_ptr = (char *) att_addlength_pointer(data_ptr, att->attlen,
															  data_ptr);
				}
				continue;
			}

			/* Check if attribute is null */
			is_null = att_isnull(i, nulls_bitmap);

			if (is_null)
			{
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}
			else
			{
				/* Extract the actual data */
				if (att->attlen == -1)
				{
					Size		attr_len;

					/*
					 * Align to the start of this varlena, matching the form
					 * path (FluxFormTuple aligns data_ptr at the start of
					 * each attribute before writing).  Without this the
					 * previous attribute's raw += attr_len advance can leave
					 * data_ptr unaligned, so VARSIZE_ANY reads the length
					 * from padding bytes -> garbage length -> pglz-corrupt /
					 * overrun.
					 */
					data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);
					attr_len = VARSIZE_ANY(data_ptr);

					/*
					 * A FLUX overflow pointer is reassembled from its chunks
					 * when a relation handle is available; otherwise it is
					 * returned verbatim (same as FluxTupleToSlot).  Other
					 * varlena values are stored verbatim (wide values either
					 * overflow, above, or are heap-TOASTed; no attribute
					 * compression).
					 */
					if (tuple_has_overflow && rel != NULL &&
						FluxIsOverflowPtr(data_ptr))
					{
						Datum		full = FluxFetchOverflowColumn(rel, data_ptr);

						slot->tts_values[i] = full;
					}
					else
						slot->tts_values[i] = PointerGetDatum(data_ptr);

					/*
					 * No padding after the value: the next attribute aligns
					 * itself (as in FluxFormTuple and tts_flux_deform).
					 * Padding here by this attribute's own alignment skipped
					 * bytes of a following less-aligned value.
					 */
					data_ptr += attr_len;
				}
				else if (att->attlen > 0)
				{
					/* Fixed-length attribute - never compressed or overflow */
					data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);
					slot->tts_values[i] = fetchatt(att, data_ptr);
					data_ptr += att->attlen;
				}
				else
				{
					/* This shouldn't happen */
					elog(ERROR, "unsupported attribute length: %d", att->attlen);
				}

				slot->tts_isnull[i] = false;
			}
		}

		/*
		 * Fill missing attributes with defaults for columns added by ALTER
		 * TABLE ADD COLUMN after this tuple was stored.
		 */
		if (loop_natts < tupdesc->natts)
		{
			for (i = loop_natts; i < tupdesc->natts; i++)
			{
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}
			slot->tts_nvalid = loop_natts;
			slot_getmissingattrs(slot, loop_natts, tupdesc->natts);
		}
	}							/* end of tuple_natts scope block */

	/*
	 * This leaves the slot virtual (tts_values[] only, no physical tuple), so
	 * cache the header's system columns now -- otherwise slot_getsysattr()
	 * cannot answer xmin/xmax later.  See FluxSlotCacheSysAttrs.
	 */
	FluxSlotCacheSysAttrs(slot, tuple_header);

	/* Mark slot as valid */
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = tupdesc->natts;

	return true;
}
