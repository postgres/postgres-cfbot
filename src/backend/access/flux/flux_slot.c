/*-------------------------------------------------------------------------
 *
 * flux_slot.c
 *	  FLUX-specific TupleTableSlot implementation
 *
 * This implements custom TupleTableSlotOps for FLUX table access method.
 * FLUX tuples use timestamps for MVCC instead of transaction IDs, and
 * have a different on-disk format than heap tuples. This slot type handles
 * the FLUX tuple format natively, avoiding unnecessary conversions
 * through the heap tuple format.
 *
 * The slot can hold either:
 *   - A reference to a FLUX tuple in a buffer page (pinned buffer)
 *   - A materialized (palloc'd) copy of a FLUX tuple
 *   - Virtual data in tts_values/tts_isnull (after deforming or direct store)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_slot.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/flux.h"
#include "access/slog.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "storage/bufmgr.h"
#include "utils/expandeddatum.h"
#include "utils/memutils.h"

/*
 * FluxTupleTableSlot - slot type for FLUX tuples
 *
 * This extends the base TupleTableSlot with FLUX-specific fields to
 * hold a reference to a FLUX tuple either in a buffer or materialized
 * in memory.
 */
typedef struct FluxTupleTableSlot
{
	TupleTableSlot base;

	/* Pointer to the FLUX tuple header (in buffer or materialized) */
	FluxTupleHeader *tuple;

	/* Length of the tuple data pointed to by 'tuple' */
	uint32		tuple_len;

	/*
	 * Values-only ("virtual") payload block.  When copyslot deep-copies a
	 * source slot's datums instead of forming a physical tuple, the
	 * pass-by-reference values are packed into this single palloc'd block so
	 * that clear() can free them with one pfree (mirrors
	 * VirtualTupleTableSlot's ->data).  NULL when the slot holds a physical
	 * tuple or has no pass-by-ref values.
	 */
	void	   *values_block;

	/* Deform state: offset into tuple data for lazy attribute extraction */
	uint32		off;

	/*
	 * If buffer is not InvalidBuffer, the slot holds a pin on this buffer and
	 * 'tuple' points into the buffer page. When the slot is cleared or
	 * materialized, the pin is released.
	 */
	Buffer		buffer;

	/*
	 * Cached MVCC system-column values.
	 *
	 * The FLUX read paths deliberately leave a slot "virtual": they deform
	 * the on-page tuple into tts_values[] and then deep-copy those datums
	 * into slot-owned memory (ExecMaterializeSlot) so nothing aliases a
	 * buffer that is about to be unpinned.  That drops 'tuple', which is
	 * where xmin/xmax used to be read from -- so slot_getsysattr() on such a
	 * slot raised "cannot retrieve a system column in this context".
	 *
	 * That is not a corner case: ON CONFLICT DO UPDATE/DO SELECT reads xmin
	 * off the existing-row slot to detect a self-conflict
	 * (nodeModifyTable.c), so INSERT ... ON CONFLICT against a FLUX table
	 * reported the wrong error instead of "command cannot affect row a second
	 * time".
	 *
	 * So capture the two header-derived system columns whenever a physical
	 * tuple is stored, and keep answering from the cache after the tuple
	 * pointer is gone.  sys_valid says the cache holds real values; it is
	 * reset by clear() and by any store of a new tuple.
	 */
	bool		sys_valid;
	TransactionId sys_xmin;
	TransactionId sys_xmax;

	/*
	 * The command id stamped with sys_xmin (the header's t_cmin): with it,
	 * sys_xmin names one version of the row (FluxSlotGetVersion).
	 */
	CommandId	sys_cmin;
} FluxTupleTableSlot;

/* Forward declarations */
const TupleTableSlotOps TTSOpsFluxTuple;
static void tts_flux_deform(TupleTableSlot *slot, int natts);
static void tts_flux_materialize_values(TupleTableSlot *slot);
static void flux_slot_adopt_tuple(TupleTableSlot *slot, FluxTupleHeader *tuple,
								  uint32 tuple_len);


/*
 * Initialization - nothing special needed.
 */
static void
tts_flux_init(TupleTableSlot *slot)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	rslot->tuple = NULL;
	rslot->tuple_len = 0;
	rslot->off = 0;
	rslot->buffer = InvalidBuffer;
	rslot->values_block = NULL;
}

/*
 * Destruction - release any resources.
 */
static void
tts_flux_release(TupleTableSlot *slot)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	/* If we own a materialized tuple, free it */
	if (TTS_SHOULDFREE(slot) && rslot->tuple)
	{
		pfree(rslot->tuple);
		rslot->tuple = NULL;
	}
	if (TTS_SHOULDFREE(slot) && rslot->values_block)
	{
		pfree(rslot->values_block);
		rslot->values_block = NULL;
	}

	/* Release buffer pin if held */
	if (BufferIsValid(rslot->buffer))
	{
		ReleaseBuffer(rslot->buffer);
		rslot->buffer = InvalidBuffer;
	}
}

/*
 * Clear the slot contents.
 *
 * Free materialized tuple if owned, release buffer pin, and reset
 * the slot to empty state.
 */
static void
tts_flux_clear(TupleTableSlot *slot)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	/*
	 * Free materialized tuple data if we own it. A tuple residing in a buffer
	 * cannot be freed directly; only materialized copies can.
	 */
	if (TTS_SHOULDFREE(slot))
	{
		Assert(!BufferIsValid(rslot->buffer));

		if (rslot->tuple)
			pfree(rslot->tuple);
		if (rslot->values_block)
			pfree(rslot->values_block);

		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
	}

	/* Release buffer pin if held */
	if (BufferIsValid(rslot->buffer))
	{
		ReleaseBuffer(rslot->buffer);
		rslot->buffer = InvalidBuffer;
	}

	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
	rslot->tuple = NULL;
	rslot->tuple_len = 0;
	rslot->off = 0;
	rslot->values_block = NULL;

	/*
	 * Drop the cached system columns with the tuple: an emptied slot must not
	 * answer xmin/xmax from a previous row (see sys_valid).
	 */
	rslot->sys_valid = false;
	rslot->sys_xmin = InvalidTransactionId;
	rslot->sys_xmax = InvalidTransactionId;
	rslot->sys_cmin = InvalidCommandId;
}

/*
 * Deform FLUX tuple to extract attributes into tts_values/tts_isnull.
 *
 * This is the FLUX-native equivalent of slot_deform_heap_tuple. It reads
 * the FLUX tuple format directly (bitmap + inline attribute data) rather
 * than going through the heap tuple deforming path.
 */
static void
tts_flux_deform(TupleTableSlot *slot, int natts)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	FluxTupleHeader *header = rslot->tuple;
	int			attnum;
	char	   *data_ptr;
	uint8	   *nulls_bitmap;
	Size		bitmap_len;
	bool		has_nulls;
	bool		tuple_has_compressed;

	Assert(header != NULL);
	Assert(natts <= tupdesc->natts);

	/* Start from where we left off last time */
	attnum = slot->tts_nvalid;
	if (attnum >= natts)
		return;

	/*
	 * Use the tuple's actual natts for bitmap_len and data_ptr calculation,
	 * not the tupdesc's natts.  After ALTER TABLE ADD COLUMN, old tuples may
	 * have fewer attributes than the current schema expects.
	 */
	{
		int			tuple_natts = header->t_natts;

		bitmap_len = BITMAPLEN(tuple_natts);
		nulls_bitmap = (uint8 *) header->t_attrs_bitmap;
		has_nulls = (header->t_infomask & FLUX_INFOMASK_HASNULL) != 0;
		tuple_has_compressed = false;	/* FLUX does not compress attributes */
		(void) tuple_has_compressed;

		/*
		 * If this is the first time deforming (attnum == 0), start from the
		 * beginning of the data area. Otherwise, resume from saved offset.
		 */
		if (attnum == 0)
			data_ptr = (char *) header + MAXALIGN(FLUX_TUPLE_OVERHEAD + bitmap_len);
		else
			data_ptr = (char *) header + rslot->off;

		/*
		 * Limit deformation to the attributes physically present in the
		 * tuple.  Attributes beyond tuple_natts were added by ALTER TABLE ADD
		 * COLUMN and will be filled with their defaults below.
		 */
		natts = Min(natts, tuple_natts);
	}

	for (; attnum < natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, attnum);

		if (att->attisdropped)
		{
			slot->tts_values[attnum] = (Datum) 0;
			slot->tts_isnull[attnum] = true;

			/*
			 * A column dropped after this tuple was written still occupies
			 * its bytes on the page, so we must step over them or every later
			 * attribute is read from the wrong offset.  ALTER TABLE DROP
			 * COLUMN deliberately preserves attlen/attalign for exactly this
			 * purpose (see RemoveAttributeById), and the null bitmap tells us
			 * whether the bytes are physically present.  Tuples formed after
			 * the drop store the column as NULL, so they correctly consume
			 * nothing here.
			 */
			if (!(has_nulls && att_isnull(attnum, nulls_bitmap)))
			{
				data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);
				data_ptr = (char *) att_addlength_pointer(data_ptr, att->attlen,
														  data_ptr);
			}
			continue;
		}

		/* Check null bitmap */
		if (has_nulls && att_isnull(attnum, nulls_bitmap))
		{
			slot->tts_values[attnum] = (Datum) 0;
			slot->tts_isnull[attnum] = true;
			continue;
		}

		slot->tts_isnull[attnum] = false;

		if (att->attlen > 0)
		{
			/* Fixed-length attribute - align first */
			data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);
			slot->tts_values[attnum] = fetchatt(att, data_ptr);
			data_ptr += att->attlen;
		}
		else if (att->attlen == -1)
		{
			Size		attr_len;

			/* Variable-length attribute - align first */
			data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);
			attr_len = VARSIZE_ANY(data_ptr);

			/*
			 * FLUX stores varlena values verbatim (wide values are TOASTed
			 * through the standard heap TOAST path; FLUX has no on-page
			 * overflow and does not compress attributes).
			 */
			slot->tts_values[attnum] = PointerGetDatum(data_ptr);
			data_ptr += attr_len;
		}
		else if (att->attlen == -2)
		{
			/* C string */
			data_ptr = (char *) att_align_nominal(data_ptr, att->attalign);
			slot->tts_values[attnum] = CStringGetDatum(data_ptr);
			data_ptr += strlen(data_ptr) + 1;
		}
		else
		{
			elog(ERROR, "unsupported attribute length: %d", att->attlen);
		}
	}

	/* Save deform state for incremental deforming */
	rslot->off = (uint32) (data_ptr - (char *) header);
	slot->tts_nvalid = natts;
}

/*
 * Fill up first natts entries of tts_values and tts_isnull.
 *
 * If the slot has a FLUX tuple, deform it natively. If values were already
 * stored directly (virtual-style), they are already present.
 */
static void
tts_flux_getsomeattrs(TupleTableSlot *slot, int natts)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	if (rslot->tuple != NULL)
	{
		/* Deform from the FLUX tuple */
		tts_flux_deform(slot, natts);

		/*
		 * If the tuple had fewer attributes than requested (e.g., after ALTER
		 * TABLE ADD COLUMN), fill in defaults for the missing ones.
		 */
		if (slot->tts_nvalid < natts)
		{
			slot_getmissingattrs(slot, slot->tts_nvalid, natts);
			slot->tts_nvalid = natts;
		}
	}
	else
	{
		/*
		 * No physical tuple - values were stored directly into tts_values
		 * (virtual-style). Fill missing attributes.
		 */
		slot_getmissingattrs(slot, slot->tts_nvalid, natts);
		slot->tts_nvalid = natts;
	}
}

/*
 * FluxSlotCacheSysAttrs
 *
 * Record a tuple's MVCC system columns on the slot so slot_getsysattr() still
 * works after the slot has been left "virtual".
 *
 * The FLUX read paths deform the on-page tuple into tts_values[] and then
 * deep-copy those datums into slot-owned memory, deliberately NOT keeping a
 * physical tuple pointer (nothing may alias a buffer that is about to be
 * unpinned).  xmin/xmax live only in the tuple header, so once that pointer is
 * gone they are unrecoverable -- and ON CONFLICT DO UPDATE/DO SELECT reads xmin
 * off exactly such a slot to detect a self-conflict.  Callers that populate a
 * slot from a header therefore stash the values here while the header is still
 * in hand.
 */
void
FluxSlotCacheSysAttrs(TupleTableSlot *slot, const FluxTupleHeader *tuple)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	if (slot->tts_ops != &TTSOpsFluxTuple || tuple == NULL)
		return;

	rslot->sys_valid = true;
	rslot->sys_xmin = tuple->t_xmin;
	rslot->sys_xmax = FluxTupleGetXmax(tuple);
	rslot->sys_cmin = tuple->t_cmin;
}

/*
 * FluxSlotSetVersion
 *		Record in a slot the version a FLUX write just stored from it.
 *
 * tuple_insert, tuple_update and their kin store the slot's values as a
 * new version stamped (xmin, cmin), and the executor then hands that slot
 * to the AFTER triggers as the row's NEW image.  The slot must name the
 * version it now stands for, not one it may have been loaded with (an
 * INSERT ... SELECT from a FLUX table, an UPDATE's re-fetched old row):
 * deform any stored tuple into owned values, drop it, and cache the new
 * identity.  A slot of another kind is left alone; the identity of what it
 * holds is not the AM's to answer.
 */
void
FluxSlotSetVersion(TupleTableSlot *slot, TransactionId xmin, CommandId cmin)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	if (slot->tts_ops != &TTSOpsFluxTuple || TTS_EMPTY(slot))
		return;

	if (rslot->tuple != NULL)
	{
		void	   *old_tuple = TTS_SHOULDFREE(slot) ? rslot->tuple : NULL;
		void	   *old_block = TTS_SHOULDFREE(slot) ? rslot->values_block : NULL;
		MemoryContext oldcxt = MemoryContextSwitchTo(slot->tts_mcxt);

		slot_getallattrs(slot);
		slot->tts_flags &= ~TTS_FLAG_SHOULDFREE;
		rslot->values_block = NULL;
		tts_flux_materialize_values(slot);	/* drops rslot->tuple */
		MemoryContextSwitchTo(oldcxt);
		if (old_tuple)
			pfree(old_tuple);
		if (old_block)
			pfree(old_block);
		if (BufferIsValid(rslot->buffer))
		{
			ReleaseBuffer(rslot->buffer);
			rslot->buffer = InvalidBuffer;
		}
	}

	rslot->sys_valid = true;
	rslot->sys_xmin = xmin;
	rslot->sys_xmax = InvalidTransactionId;
	rslot->sys_cmin = cmin;
}

/*
 * FluxSlotGetVersion
 *		The identity, (xmin, cmin), of the row version a slot holds, if it
 *		knows it.
 *
 * A FLUX slot knows it from the stored header or from the system-column
 * cache.  The cache survives a copy of the version through a heap-format
 * tuple and back (tts_flux_copy_heap_tuple, tts_flux_copyslot): that is how
 * the AFTER-trigger queue carries a version (am_inplace_update_keeps_tid).
 * A slot of another kind is not the AM's to answer for.
 */
bool
FluxSlotGetVersion(TupleTableSlot *slot, TransactionId *xmin, CommandId *cmin)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	if (slot->tts_ops != &TTSOpsFluxTuple)
		return false;
	if (rslot->tuple != NULL)
	{
		*xmin = rslot->tuple->t_xmin;
		*cmin = rslot->tuple->t_cmin;
		return true;
	}
	if (rslot->sys_valid && rslot->sys_cmin != InvalidCommandId)
	{
		*xmin = rslot->sys_xmin;
		*cmin = rslot->sys_cmin;
		return true;
	}
	return false;
}

/*
 * Return system attribute value for FLUX tuples.
 *
 * FLUX tuples have timestamps instead of XIDs, so most heap system columns
 * are not directly applicable. We handle the subset that makes sense.
 */
static Datum
tts_flux_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	Assert(!TTS_EMPTY(slot));

	/* Materialization can leave only the cached system attributes. */
	if (!rslot->tuple)
	{
		if (rslot->sys_valid)
		{
			*isnull = false;
			switch (attnum)
			{
				case MinTransactionIdAttributeNumber:
					return TransactionIdGetDatum(rslot->sys_xmin);
				case MaxTransactionIdAttributeNumber:
					return TransactionIdGetDatum(rslot->sys_xmax);
				case MinCommandIdAttributeNumber:
				case MaxCommandIdAttributeNumber:
					return CommandIdGetDatum(rslot->sys_cmin);
				default:
					break;
			}
		}

		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot retrieve a system column in this context")));
	}

	/*
	 * Return the tuple's real MVCC system columns.  FLUX uses heap-shaped
	 * xmin/xmax semantics: t_xmin is the inserter XID (always valid), and the
	 * xmax (deleter/updater XID) lives in the low 32 bits of t_commit_ts,
	 * accessed via FluxTupleGetXmax (0 == not deleted/updated).
	 */
	*isnull = false;

	switch (attnum)
	{
		case MinTransactionIdAttributeNumber:	/* xmin */
			return TransactionIdGetDatum(rslot->tuple->t_xmin);
		case MaxTransactionIdAttributeNumber:	/* xmax */
			return TransactionIdGetDatum(FluxTupleGetXmax(rslot->tuple));
		case MinCommandIdAttributeNumber:	/* cmin */
		case MaxCommandIdAttributeNumber:	/* cmax */
			{
				CommandId	cid = rslot->tuple->t_cmin;

				return CommandIdGetDatum(cid);
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("FLUX does not support system attribute %d",
							attnum)));
			return 0;			/* silence compiler */
	}
}

/*
 * Check if the tuple was created by the current transaction.
 *
 * As for heap, that is a property of the stored version: its xmin, the XID
 * of the writer that produced it (FLUX keeps heap-shaped xmin, and an in-place
 * UPDATE stamps the updater's).  The slot has it in the tuple header, or in
 * the system-column cache once it has been left virtual (sys_valid).  It does
 * not depend on the slot's TID and table OID, which a copy of the version
 * (the AFTER-trigger OLD row of an in-place UPDATE) does not carry.
 */
static bool
tts_flux_is_current_xact_tuple(TupleTableSlot *slot)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;
	TransactionId xmin;

	Assert(!TTS_EMPTY(slot));

	if (rslot->tuple != NULL)
		xmin = rslot->tuple->t_xmin;
	else if (rslot->sys_valid)
		xmin = rslot->sys_xmin;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("don't have a storage tuple in this context")));

	return TransactionIdIsCurrentTransactionId(xmin);
}

/*
 * Pack a slot's pass-by-reference Datums into a single palloc'd block owned by
 * the slot, leaving the slot in a values-only ("virtual") state (tuple == NULL).
 *
 * This mirrors tts_virtual_materialize: a two-pass scan that first sums the
 * aligned size of every non-null pass-by-reference attribute, makes one
 * allocation in the slot's memory context, then copies each Datum in and
 * repoints tts_values at the copy.  byval and null Datums are left untouched.
 *
 * We deliberately do NOT form a physical FLUX tuple here.  Forming one would
 * compress every varlena, and the immediate next consumer (flux_multi_insert /
 * flux_tuple_insert via slot_getallattrs) would decompress it again before
 * re-forming the on-page tuple.  Deferring tuple formation to the insert path
 * makes bulk COPY compress each value exactly once instead of three times.
 *
 * The caller must have already populated tts_values/tts_isnull and set
 * tts_nvalid to the attribute count; those stay valid because getsomeattrs and
 * getsysattr handle tuple == NULL by working from tts_values.
 */
static void
tts_flux_materialize_values(TupleTableSlot *slot)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	Size		sz = 0;
	char	   *data;

	/* First pass: compute the aligned size of the owned block. */
	for (int natt = 0; natt < desc->natts; natt++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, natt);
		Datum		val;

		if (att->attbyval || slot->tts_isnull[natt])
			continue;

		val = slot->tts_values[natt];

		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			sz = att_nominal_alignby(sz, att->attalignby);
			sz += EOH_get_flat_size(DatumGetEOHP(val));
		}
		else
		{
			sz = att_nominal_alignby(sz, att->attalignby);
			sz = att_addlength_datum(sz, att->attlen, val);
		}
	}

	rslot->tuple = NULL;
	rslot->tuple_len = 0;
	rslot->off = 0;

	/* all data is byval / null: nothing to own */
	if (sz == 0)
		return;

	rslot->values_block = data = MemoryContextAlloc(slot->tts_mcxt, sz);
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	/* Second pass: copy each pass-by-reference Datum and repoint tts_values. */
	for (int natt = 0; natt < desc->natts; natt++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, natt);
		Datum		val;

		if (att->attbyval || slot->tts_isnull[natt])
			continue;

		val = slot->tts_values[natt];

		if (att->attlen == -1 &&
			VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			ExpandedObjectHeader *eoh = DatumGetEOHP(val);
			Size		data_length = EOH_get_flat_size(eoh);

			data = (char *) att_nominal_alignby(data, att->attalignby);
			EOH_flatten_into(eoh, data, data_length);

			slot->tts_values[natt] = PointerGetDatum(data);
			data += data_length;
		}
		else
		{
			Size		data_length = 0;

			data = (char *) att_nominal_alignby(data, att->attalignby);
			data_length = att_addlength_datum(data_length, att->attlen, val);

			memcpy(data, DatumGetPointer(val), data_length);

			slot->tts_values[natt] = PointerGetDatum(data);
			data += data_length;
		}
	}
}

/*
 * Materialize the slot contents.
 *
 * After materialization, the slot's data is independent of any external
 * storage (buffers, other memory contexts). If the slot references a
 * tuple in a buffer, the tuple data is copied and the buffer pin released.
 */
static void
tts_flux_materialize(TupleTableSlot *slot)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;
	MemoryContext oldContext;

	Assert(!TTS_EMPTY(slot));

	/* Already materialized */
	if (TTS_SHOULDFREE(slot))
		return;

	oldContext = MemoryContextSwitchTo(slot->tts_mcxt);

	if (rslot->tuple != NULL)
	{
		/*
		 * We have a physical FLUX tuple (in a buffer or external memory).
		 * Copy it into the slot's own memory context.
		 */
		FluxTupleHeader *newtuple;

		newtuple = (FluxTupleHeader *) palloc(rslot->tuple_len);
		memcpy(newtuple, rslot->tuple, rslot->tuple_len);
		rslot->tuple = newtuple;

		/*
		 * Reset deform state since tts_values entries may point into the old
		 * (buffer) tuple data that we're about to release.
		 */
		rslot->off = 0;
		slot->tts_nvalid = 0;
	}
	else
	{
		/*
		 * Virtual tuple (values stored directly).  Materialize by copying all
		 * pass-by-reference Datums into a single block in the slot's memory
		 * context and leaving the slot in a values-only state (tuple ==
		 * NULL). We deliberately do NOT form a physical FLUX tuple: that
		 * would compress every varlena, and the insert path
		 * (flux_multi_insert via slot_getallattrs) would decompress and
		 * recompress it.  Deferring tuple formation to insert compresses each
		 * value exactly once.
		 */
		tts_flux_materialize_values(slot);

		/*
		 * tts_flux_materialize_values sets SHOULDFREE itself when it owns a
		 * block; return early so we don't set SHOULDFREE unconditionally for
		 * a slot that owns nothing (all-byval/null).  We still must release
		 * any buffer pin first.
		 */
		if (BufferIsValid(rslot->buffer))
		{
			ReleaseBuffer(rslot->buffer);
			rslot->buffer = InvalidBuffer;
		}
		MemoryContextSwitchTo(oldContext);
		return;
	}

	/*
	 * Release buffer pin if held. Do this after copying but before setting
	 * TTS_FLAG_SHOULDFREE to avoid a transient state where the slot owns a
	 * buffer and has SHOULDFREE set.
	 */
	if (BufferIsValid(rslot->buffer))
	{
		ReleaseBuffer(rslot->buffer);
		rslot->buffer = InvalidBuffer;
	}

	slot->tts_flags |= TTS_FLAG_SHOULDFREE;

	MemoryContextSwitchTo(oldContext);
}

/*
 * Copy the contents of srcslot into dstslot.
 *
 * The destination must not depend on the source slot's memory after this
 * returns.  We satisfy that by deep-copying the source's attribute values
 * into the destination's own memory context and leaving the destination in
 * a values-only ("virtual") state -- rdst->tuple stays NULL.  We deliberately
 * do NOT form a physical FLUX tuple here: forming one would compress every
 * varlena, and the immediate next consumer (flux_multi_insert /
 * flux_tuple_insert via slot_getallattrs) would have to decompress it again
 * before re-forming the on-page tuple.  Deferring tuple formation to the
 * insert path makes bulk COPY compress each value exactly once instead of
 * three times (compress here, decompress there, recompress there).
 *
 * That is for a source of another kind (COPY's scratch slot); a FLUX source
 * holding a stored version is copied as that version, below.
 *
 * The values-only state is fully supported by the other slot ops:
 * getsomeattrs and materialize both handle rslot->tuple == NULL by working
 * from tts_values, and copy_heap_tuple/copy_minimal_tuple go through
 * slot_getallattrs.
 */
static void
tts_flux_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	FluxTupleTableSlot *rsrc = (FluxTupleTableSlot *) srcslot;
	FluxTupleTableSlot *rdst = (FluxTupleTableSlot *) dstslot;
	TupleDesc	desc = dstslot->tts_tupleDescriptor;

	/*
	 * A FLUX source holding a stored version: copy the version itself, as
	 * heap's buffer slot copies a buffer slot's tuple.  The copy keeps what
	 * only the header knows, xmin and xmax, so slot_getsysattr() and
	 * slot_is_current_xact_tuple() answer for it as they do for the source.
	 * The AFTER-trigger OLD row of an in-place UPDATE is such a copy, and the
	 * RI triggers ask it whether this transaction created it.
	 */
	if (srcslot->tts_ops == &TTSOpsFluxTuple && rsrc->tuple != NULL)
	{
		FluxTupleHeader *copy = MemoryContextAlloc(dstslot->tts_mcxt,
												   rsrc->tuple_len);

		memcpy(copy, rsrc->tuple, rsrc->tuple_len);
		flux_slot_adopt_tuple(dstslot, copy, rsrc->tuple_len);
		dstslot->tts_tid = srcslot->tts_tid;
		return;
	}

	tts_flux_clear(dstslot);

	slot_getallattrs(srcslot);

	/* Copy the datum pointers first; they still reference source memory. */
	for (int natt = 0; natt < desc->natts; natt++)
	{
		dstslot->tts_values[natt] = srcslot->tts_values[natt];
		dstslot->tts_isnull[natt] = srcslot->tts_isnull[natt];
	}
	dstslot->tts_nvalid = desc->natts;
	dstslot->tts_flags &= ~TTS_FLAG_EMPTY;
	dstslot->tts_tid = srcslot->tts_tid;

	/*
	 * Deep-copy the pass-by-reference datums into a single owned block so the
	 * destination no longer depends on the source's memory (in COPY, the
	 * source is a per-row scratch slot that gets reset).  This leaves the
	 * destination in a values-only state (tuple == NULL), deferring
	 * compression to the insert path.
	 */
	tts_flux_materialize_values(dstslot);

	/*
	 * A FLUX source left virtual still knows its version's system columns
	 * (sys_valid); carry them, for the same reason as above.
	 */
	if (srcslot->tts_ops == &TTSOpsFluxTuple && rsrc->sys_valid)
	{
		rdst->sys_valid = true;
		rdst->sys_xmin = rsrc->sys_xmin;
		rdst->sys_xmax = rsrc->sys_xmax;
		rdst->sys_cmin = rsrc->sys_cmin;
	}

	/*
	 * A heap-format copy of a stored version, made by
	 * tts_flux_copy_heap_tuple, names that version in its header and says so
	 * by a valid t_ctid (heap_form_tuple leaves it invalid).  Take the
	 * identity back: the AFTER-trigger queue keeps an in-place AM's rows as
	 * such copies and fires the trigger with them in FLUX slots
	 * (am_inplace_update_keeps_tid), and the RI triggers ask
	 * flux_tuple_satisfies_snapshot about the version.
	 */
	else if (TTS_IS_HEAPTUPLE(srcslot))
	{
		HeapTuple	htup = ((HeapTupleTableSlot *) srcslot)->tuple;

		if (htup != NULL && ItemPointerIsValid(&htup->t_data->t_ctid))
		{
			rdst->sys_valid = true;
			rdst->sys_xmin = HeapTupleHeaderGetRawXmin(htup->t_data);
			rdst->sys_xmax = HeapTupleHeaderGetRawXmax(htup->t_data);
			rdst->sys_cmin = HeapTupleHeaderGetRawCommandId(htup->t_data);
		}
	}
}

/*
 * Return a HeapTuple "owned" by the slot.
 *
 * Since FLUX tuples are not heap tuples, we must form one from the
 * deformed values. The result is a palloc'd HeapTuple that the slot owns.
 *
 * This is needed by parts of the executor that require heap tuples
 * (e.g., for index tuple formation, triggers, etc.).
 */
static HeapTuple
tts_flux_copy_heap_tuple(TupleTableSlot *slot)
{
	HeapTuple	htup;

	Assert(!TTS_EMPTY(slot));

	/* Ensure all attributes are deformed */
	slot_getallattrs(slot);

	htup = heap_form_tuple(slot->tts_tupleDescriptor,
						   slot->tts_values,
						   slot->tts_isnull);

	/*
	 * Propagate TID and table OID from the slot to the HeapTuple. ANALYZE's
	 * compare_rows() sorts sample tuples by t_self (TID), which
	 * heap_form_tuple leaves zeroed.  Without this, the ItemPointerIsValid
	 * assertion in ItemPointerGetBlockNumber fires.
	 */
	htup->t_self = slot->tts_tid;
	htup->t_tableOid = slot->tts_tableOid;

	/*
	 * A copy of a stored version identifies the version, as a heap tuple's
	 * header does: xmin, cmin and xmax, with t_ctid set to the row's TID to
	 * say so (see FluxSlotGetVersion).  The AFTER-trigger queue keeps an
	 * in-place AM's rows this way.
	 */
	{
		TransactionId xmin;
		CommandId	cmin;

		if (ItemPointerIsValid(&slot->tts_tid) &&
			FluxSlotGetVersion(slot, &xmin, &cmin))
		{
			FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;
			TransactionId xmax = rslot->tuple ? FluxTupleGetXmax(rslot->tuple) :
				rslot->sys_xmax;

			HeapTupleHeaderSetXmin(htup->t_data, xmin);
			HeapTupleHeaderSetCmin(htup->t_data, cmin);
			HeapTupleHeaderSetXmax(htup->t_data, xmax);
			htup->t_data->t_infomask &= ~HEAP_XACT_MASK;
			if (!TransactionIdIsValid(xmax))
				htup->t_data->t_infomask |= HEAP_XMAX_INVALID;
			htup->t_data->t_ctid = slot->tts_tid;
		}
	}

	return htup;
}

/*
 * Return a MinimalTuple copy allocated in the caller's memory context.
 */
static MinimalTuple
tts_flux_copy_minimal_tuple(TupleTableSlot *slot, Size extra)
{
	Assert(!TTS_EMPTY(slot));

	/* Ensure all attributes are deformed */
	slot_getallattrs(slot);

	return heap_form_minimal_tuple(slot->tts_tupleDescriptor,
								   slot->tts_values,
								   slot->tts_isnull,
								   extra);
}

/*
 * The FLUX TupleTableSlotOps structure.
 *
 * FLUX slots do not "own" heap tuples or minimal tuples natively, so
 * get_heap_tuple and get_minimal_tuple are NULL. The copy_ variants are
 * provided to satisfy the executor's needs.
 */
const TupleTableSlotOps TTSOpsFluxTuple = {
	.base_slot_size = sizeof(FluxTupleTableSlot),
	.init = tts_flux_init,
	.release = tts_flux_release,
	.clear = tts_flux_clear,
	.getsomeattrs = tts_flux_getsomeattrs,
	.getsysattr = tts_flux_getsysattr,
	.is_current_xact_tuple = tts_flux_is_current_xact_tuple,
	.materialize = tts_flux_materialize,
	.copyslot = tts_flux_copyslot,

	/* FLUX slots do not natively own heap or minimal tuples */
	.get_heap_tuple = NULL,
	.get_minimal_tuple = NULL,
	.copy_heap_tuple = tts_flux_copy_heap_tuple,
	.copy_minimal_tuple = tts_flux_copy_minimal_tuple,
};


/*
 * Store a copy of a FLUX tuple into the slot.
 *
 * This is the only way to put a physical FLUX tuple into a FLUX slot.  The
 * slot allocates the copy in its own context, slot->tts_mcxt, and frees it
 * on clear or release; the caller keeps ownership of tuple.  A slot never
 * takes over a caller's allocation: the caller's CurrentMemoryContext may be
 * a per-tuple context (ATRewriteTable, validateCheckConstraint) that is
 * reset under the slot, after which clearing the slot would free the chunk a
 * second time.
 *
 * If buffer is valid, tuple points into that buffer's page.  Unlike heap,
 * FLUX rewrites a tuple's bytes in place on UPDATE, so a slot must never
 * point into the page: a datum deformed from it would change under the
 * reader, to a version its snapshot must not see.  The caller holds the
 * buffer's content lock in a mode that excludes those writers
 * (share-exclusive or exclusive; the byte-CAS UPDATE runs under
 * share-exclusive), and decided visibility on these same bytes under that
 * same lock.  The slot holds no pin.  If buffer is InvalidBuffer, tuple is
 * in local memory, e.g. a version reconstructed from UNDO, which the caller
 * frees.
 */
void
FluxSlotStoreTuple(TupleTableSlot *slot, FluxTupleHeader *tuple,
				   uint32 tuple_len, Buffer buffer)
{
	FluxTupleHeader *copy;

	Assert(slot->tts_ops == &TTSOpsFluxTuple);
	Assert(!BufferIsValid(buffer) ||
		   BufferIsLockedByMeInMode(buffer, BUFFER_LOCK_SHARE_EXCLUSIVE) ||
		   BufferIsLockedByMeInMode(buffer, BUFFER_LOCK_EXCLUSIVE));

	copy = MemoryContextAlloc(slot->tts_mcxt, tuple_len);
	memcpy(copy, tuple, tuple_len);
	flux_slot_adopt_tuple(slot, copy, tuple_len);
}

/*
 * Make tuple, a chunk of slot->tts_mcxt, the slot's tuple.  The slot frees
 * it when cleared or released.
 */
static void
flux_slot_adopt_tuple(TupleTableSlot *slot, FluxTupleHeader *tuple,
					  uint32 tuple_len)
{
	FluxTupleTableSlot *rslot = (FluxTupleTableSlot *) slot;

	Assert(slot->tts_ops == &TTSOpsFluxTuple);
	Assert(GetMemoryChunkContext(tuple) == slot->tts_mcxt);

	tts_flux_clear(slot);

	rslot->tuple = tuple;
	rslot->tuple_len = tuple_len;
	rslot->off = 0;
	rslot->buffer = InvalidBuffer;

	/* Cache the system columns while we still have the header. */
	rslot->sys_valid = true;
	rslot->sys_xmin = tuple->t_xmin;
	rslot->sys_xmax = FluxTupleGetXmax(tuple);
	rslot->sys_cmin = tuple->t_cmin;

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_flags |= TTS_FLAG_SHOULDFREE;
	slot->tts_nvalid = 0;
}
