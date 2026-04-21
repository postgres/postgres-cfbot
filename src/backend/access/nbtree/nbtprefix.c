/*-------------------------------------------------------------------------
 *
 * nbtprefix.c
 *	  Prefix-compressed leaf-page support for btrees.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtprefix.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/*
 * MaxHeapTuplesPerPage must fit in BT_OFFSET_MASK so that a compressed
 * follower's real heap-tuple offset can be packed into the low 12 bits of
 * t_tid's offset field, leaving BT_IS_COMPRESSED in the upper nibble.
 */
StaticAssertDecl(MaxHeapTuplesPerPage <= BT_OFFSET_MASK,
				 "MaxHeapTuplesPerPage must fit in BT_OFFSET_MASK");

/* Internal helper also exposed via _bt_prefix_build_follower_tupdesc below */
static TupleDesc build_follower_tupdesc(Relation rel, int natts_anchor);


/*
 * _bt_prefix_state_init() -- begin walking a prefix-compressed leaf page.
 *
 * Must be paired with _bt_prefix_state_cleanup() once walking is finished
 * (or before the frame owning 'st' is destroyed).  It is legal to call
 * _bt_prefix_state_init() again on the same state -- the previous state is
 * cleaned up implicitly.
 */
void
_bt_prefix_state_init(BTPrefixPageState *st, Page page)
{
	_bt_prefix_state_cleanup(st);
	st->page = page;
	st->anchor_off = InvalidOffsetNumber;
	st->anchor = NULL;
	st->natts_anchor = 0;
	st->anchor_deformed = false;
	st->next_anchor_off = InvalidOffsetNumber;
	st->follower_tupdesc = NULL;
}

/*
 * _bt_prefix_state_cleanup() -- release any per-page caches held by state.
 */
void
_bt_prefix_state_cleanup(BTPrefixPageState *st)
{
	if (st->follower_tupdesc != NULL)
	{
		FreeTupleDesc(st->follower_tupdesc);
		st->follower_tupdesc = NULL;
	}
	st->page = NULL;
	st->anchor_off = InvalidOffsetNumber;
	st->anchor = NULL;
	st->anchor_deformed = false;
	st->next_anchor_off = InvalidOffsetNumber;
}

/*
 * _bt_prefix_state_set_anchor() -- mark anchor_off as the current anchor.
 *
 * The tuple at anchor_off must be a plain (non-compressed, non-pivot,
 * non-posting) leaf tuple.  The anchor's Datums/isnull arrays are NOT
 * deformed here; deformation is deferred to first use in _bt_prefix_form_tuple
 * so that anchors without any followers pay no reconstruction cost.
 */
void
_bt_prefix_state_set_anchor(BTPrefixPageState *st, OffsetNumber anchor_off)
{
	IndexTuple	itup;

	Assert(st->page != NULL);
	Assert(anchor_off >= FirstOffsetNumber);
	Assert(anchor_off <= PageGetMaxOffsetNumber(st->page));

	itup = (IndexTuple) PageGetItem(st->page, PageGetItemId(st->page, anchor_off));
	Assert(!BTreeTupleIsCompressed(itup));
	Assert(!BTreeTupleIsPivot(itup) || anchor_off == P_HIKEY);
	Assert(!BTreeTupleIsPosting(itup));

	st->anchor_off = anchor_off;
	st->anchor = itup;
	st->anchor_deformed = false;
	st->next_anchor_off = InvalidOffsetNumber;
}

/*
 * _bt_prefix_find_anchor() -- walk left from offnum to find the governing
 * anchor tuple on the same page.
 *
 * Returns the offset of the anchor, or InvalidOffsetNumber if no anchor is
 * found before the start of the data area (which would indicate corruption).
 * The item at offnum itself is examined first, so callers can use this to
 * resolve any item to its anchor.
 */
OffsetNumber
_bt_prefix_find_anchor(Page page, OffsetNumber offnum)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber firstdata = P_FIRSTDATAKEY(opaque);
	OffsetNumber off;

	Assert(P_ISLEAF(opaque));
	Assert(P_HAS_PREFIX_COMPRESSION(opaque));
	Assert(offnum >= firstdata);

	for (off = offnum; off >= firstdata; off--)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		if (!BTreeTupleIsCompressed(itup))
			return off;
	}

	return InvalidOffsetNumber;
}

/*
 * build_follower_tupdesc() -- construct a trailing-attrs TupleDesc.
 *
 * The returned TupleDesc covers attrs (natts_anchor+1 .. natts) of the
 * relation's tupdesc, renumbered to start at 1.  This matches the on-disk
 * layout of a compressed follower.  Caller owns the returned TupleDesc and
 * must free it with FreeTupleDesc().
 */
static TupleDesc
build_follower_tupdesc(Relation rel, int natts_anchor)
{
	TupleDesc	src = RelationGetDescr(rel);
	int			natts = src->natts;
	int			follower_natts = natts - natts_anchor;
	TupleDesc	desc;

	Assert(natts_anchor > 0);
	Assert(follower_natts > 0);

	desc = CreateTemplateTupleDesc(follower_natts);

	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(src, natts_anchor),
		   follower_natts * sizeof(FormData_pg_attribute));

	/*
	 * Renumber attnums so they run 1..follower_natts.  Attribute metadata
	 * otherwise matches the source (including typlen, typalign, attoptions).
	 */
	for (int i = 0; i < follower_natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		att->attnum = i + 1;
		/* Constraint metadata is not copied and not needed for deform/form */
		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';

		populate_compact_attribute(desc, i);
	}

	desc->tdtypeid = src->tdtypeid;
	desc->tdtypmod = src->tdtypmod;

	TupleDescFinalize(desc);

	return desc;
}

/*
 * _bt_prefix_form_tuple() -- materialize a compressed follower.
 *
 * Combines the leading attrs from the state's current anchor with the
 * trailing attrs from 'compressed' to produce a plain-looking, fully formed
 * IndexTuple.  The follower's real heap TID (extracted from the low 12 bits
 * of its t_tid offset field) is written into the result's t_tid.
 *
 * If 'dstbuf' is non-NULL it is used as the output buffer (caller must
 * ensure it is at least BTMaxItemSize bytes).  Otherwise the returned tuple
 * is palloc()'d in CurrentMemoryContext and caller is responsible for
 * pfree()ing it.
 *
 * Requires that _bt_prefix_state_set_anchor() has been called for the
 * governing anchor of 'compressed'.
 */
IndexTuple
_bt_prefix_form_tuple(Relation rel, BTPrefixPageState *st,
					  IndexTuple compressed, char *dstbuf)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			natts = tupdesc->natts;
	int			natts_anchor = st->natts_anchor;
	int			follower_natts = natts - natts_anchor;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	Datum		trail_values[INDEX_MAX_KEYS];
	bool		trail_isnull[INDEX_MAX_KEYS];
	IndexTuple	result;
	ItemPointerData heap_tid;

	Assert(BTreeTupleIsCompressed(compressed));
	Assert(st->anchor != NULL);
	Assert(natts_anchor > 0 && natts_anchor < natts);

	/* Deform the anchor once per anchor-group */
	if (!st->anchor_deformed)
	{
		index_deform_tuple(st->anchor, tupdesc,
						   st->anchor_values, st->anchor_isnull);
		st->anchor_deformed = true;
	}

	/*
	 * Build (or reuse) the trailing-attrs TupleDesc; the on-disk layout of a
	 * follower matches this shape exactly.
	 */
	if (st->follower_tupdesc == NULL)
		st->follower_tupdesc = build_follower_tupdesc(rel, natts_anchor);

	index_deform_tuple(compressed, st->follower_tupdesc,
					   trail_values, trail_isnull);

	/* Merge leading (anchor) + trailing (follower) into one Datum array */
	memcpy(values, st->anchor_values, natts_anchor * sizeof(Datum));
	memcpy(isnull, st->anchor_isnull, natts_anchor * sizeof(bool));
	memcpy(values + natts_anchor, trail_values, follower_natts * sizeof(Datum));
	memcpy(isnull + natts_anchor, trail_isnull, follower_natts * sizeof(bool));

	result = index_form_tuple(tupdesc, values, isnull);

	/* Patch heap TID: compressed tuple's real heap TID lives in t_tid */
	BTreeCompressedTupleGetHeapTID(compressed, &heap_tid);
	result->t_tid = heap_tid;

	if (dstbuf != NULL)
	{
		Size		sz = IndexTupleSize(result);

		memcpy(dstbuf, result, sz);
		pfree(result);
		return (IndexTuple) dstbuf;
	}

	return result;
}

/*
 * _bt_prefix_leading_equalimage() -- is the index eligible for prefix
 * compression?
 *
 * True when the index has >= 2 key attrs and each of the leading
 * (nkeyatts - 1) key columns is "equalimage" -- i.e. byte-equality of a
 * stored attr is a sound proxy for opclass equality.  That is what lets us
 * inherit the anchor's leading attrs verbatim for every compressed follower
 * in the group without collating or round-tripping the values.
 *
 * INCLUDE (non-key) attributes do not participate in the prefix and are
 * never checked here.  The trailing key attribute also isn't checked: it is
 * always stored in full on every follower.
 */
bool
_bt_prefix_leading_equalimage(Relation rel)
{
	int			nkeyatts = IndexRelationGetNumberOfKeyAttributes(rel);

	if (nkeyatts < 2)
		return false;

	for (int i = 0; i < nkeyatts - 1; i++)
	{
		Oid			opfamily = rel->rd_opfamily[i];
		Oid			opcintype = rel->rd_opcintype[i];
		Oid			collation = rel->rd_indcollation[i];
		Oid			equalimageproc;

		equalimageproc = get_opfamily_proc(opfamily, opcintype, opcintype,
										   BTEQUALIMAGE_PROC);
		if (!OidIsValid(equalimageproc) ||
			!DatumGetBool(OidFunctionCall1Coll(equalimageproc, collation,
											   ObjectIdGetDatum(opcintype))))
			return false;
	}

	return true;
}

/*
 * _bt_prefix_build_follower_tupdesc() -- public wrapper for the internal
 * follower-tupdesc builder.  See build_follower_tupdesc() for details.
 *
 * Returned TupleDesc covers attrs (natts_anchor+1 .. natts), renumbered to
 * start at 1.  Caller owns the TupleDesc and must free it with
 * FreeTupleDesc().
 */
TupleDesc
_bt_prefix_build_follower_tupdesc(Relation rel, int natts_anchor)
{
	return build_follower_tupdesc(rel, natts_anchor);
}

/*
 * _bt_prefix_compress_new_tuple() -- build a compressed follower IndexTuple
 * from a sorted/insert plain tuple that is known to share the anchor's
 * leading natts_anchor key attrs.
 *
 * full_tupdesc is RelationGetDescr(rel), follower_tupdesc covers just the
 * trailing attrs (built once by the caller via
 * _bt_prefix_build_follower_tupdesc).  The returned IndexTuple is palloc'd
 * (or written to dstbuf when non-NULL).  Its t_tid carries BT_IS_COMPRESSED
 * in the upper nibble and the real heap-tuple offset in the low 12 bits.
 */
IndexTuple
_bt_prefix_compress_new_tuple(TupleDesc full_tupdesc,
							  TupleDesc follower_tupdesc,
							  int natts_anchor,
							  IndexTuple new_plain,
							  char *dstbuf)
{
	int			natts = full_tupdesc->natts;
	int			follower_natts = natts - natts_anchor;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	Datum		trail_values[INDEX_MAX_KEYS];
	bool		trail_isnull[INDEX_MAX_KEYS];
	IndexTuple	result;

	Assert(natts_anchor > 0 && natts_anchor < natts);
	Assert(!BTreeTupleIsPivot(new_plain));
	Assert(!BTreeTupleIsPosting(new_plain));
	Assert(!BTreeTupleIsCompressed(new_plain));
	Assert(follower_tupdesc->natts == follower_natts);

	/*
	 * Deform new_plain once with the full tupdesc, then repack only the
	 * trailing attrs through the follower tupdesc.  This is a tiny bit of
	 * extra work vs. walking just the trailing attrs, but it keeps us honest
	 * about NULL bitmap semantics and handles toasting/compression through
	 * index_form_tuple uniformly.
	 */
	index_deform_tuple(new_plain, full_tupdesc, values, isnull);

	memcpy(trail_values, values + natts_anchor, follower_natts * sizeof(Datum));
	memcpy(trail_isnull, isnull + natts_anchor, follower_natts * sizeof(bool));

	result = index_form_tuple(follower_tupdesc, trail_values, trail_isnull);

	/*
	 * Patch t_tid to carry the real heap TID plus BT_IS_COMPRESSED.  Caller
	 * passes new_plain's heap TID through new_plain->t_tid (non-ALT form),
	 * which we copy verbatim before setting the status bit.
	 */
	BTreeCompressedTupleSetHeapTID(result, &new_plain->t_tid);

	if (dstbuf != NULL)
	{
		Size		sz = IndexTupleSize(result);

		memcpy(dstbuf, result, sz);
		pfree(result);
		return (IndexTuple) dstbuf;
	}

	return result;
}

/*
 * _bt_prefix_vacuum_plan() -- plan survivor-follower promotions before VACUUM
 * applies 'deletable' to a page.
 *
 * Walks deletable[] and, for each plain (anchor) tuple that still has at
 * least one surviving compressed follower (one not also in deletable),
 * reconstructs that follower's plain form into a freshly-palloc'd
 * IndexTuple.  The returned state describes which offsets must be
 * PageIndexTupleOverwrite()-written before the main PageIndexMultiDelete
 * runs.
 *
 * This runs OUTSIDE the vacuum path's critical section: the palloc()s inside
 * _bt_prefix_reconstruct() are disallowed inside a critsec.  The subsequent
 * _bt_prefix_vacuum_apply() performs only MAXALIGN-safe memcpy-sized work
 * and is safe to call inside the critsec.
 *
 * Caller-provided 'promotions' must have room for at least ndeletable
 * entries.  Returns the number of planned promotions.  The palloc'd
 * IndexTuple pointers inside 'promotions' must be pfreed by the caller after
 * the critsec ends.
 *
 * 'deletable' must be sorted ascending (PostgreSQL's vacuum code already
 * guarantees this).
 */
int
_bt_prefix_vacuum_plan(Relation rel, Page page,
					   const OffsetNumber *deletable, int ndeletable,
					   BTPrefixPromotion *promotions)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	int			nprom = 0;

	if (!P_ISLEAF(opaque) || !P_HAS_PREFIX_COMPRESSION(opaque))
		return 0;

	for (int i = 0; i < ndeletable; i++)
	{
		OffsetNumber anchor_off = deletable[i];
		IndexTuple	anchor;
		OffsetNumber f;
		OffsetNumber survivor = InvalidOffsetNumber;
		BTPrefixPageState rst;
		IndexTuple	plain;
		int			precedingdeletes = 0;

		if (anchor_off < P_FIRSTDATAKEY(opaque) || anchor_off > maxoff)
			continue;

		anchor = (IndexTuple) PageGetItem(page, PageGetItemId(page, anchor_off));
		/*
		 * Only plain (non-compressed, non-pivot, non-posting) tuples on a
		 * prefix-compressed leaf page can be anchors.
		 */
		if (BTreeTupleIsCompressed(anchor) || BTreeTupleIsPivot(anchor) ||
			BTreeTupleIsPosting(anchor))
			continue;

		/*
		 * Scan forward from the anchor looking for the first compressed
		 * follower that is NOT itself scheduled for deletion.  Stop at the
		 * next plain tuple (group boundary).
		 */
		for (f = OffsetNumberNext(anchor_off); f <= maxoff;
			 f = OffsetNumberNext(f))
		{
			IndexTuple	item = (IndexTuple) PageGetItem(page,
														PageGetItemId(page, f));
			bool		in_deletable = false;

			if (!BTreeTupleIsCompressed(item))
				break;			/* group boundary */

			for (int j = i + 1; j < ndeletable && deletable[j] <= f; j++)
			{
				if (deletable[j] == f)
				{
					in_deletable = true;
					break;
				}
			}
			if (!in_deletable)
			{
				survivor = f;
				break;
			}
		}

		if (survivor == InvalidOffsetNumber)
			continue;

		/* Reconstruct into its own palloc'd IndexTuple */
		memset(&rst, 0, sizeof(rst));
		_bt_prefix_state_init(&rst, page);
		plain = _bt_prefix_reconstruct(rel, &rst, survivor, NULL);
		_bt_prefix_state_cleanup(&rst);

		/*
		 * Record how many deletable[] offsets are strictly less than the
		 * survivor's offset.  After PageIndexMultiDelete() runs, the
		 * survivor's offset shifts down by exactly that count -- we need
		 * the post-delete offset to do the promotion PageIndexTupleOverwrite.
		 *
		 * Apply-time ordering (delete-first) is what makes promotion
		 * always fit on the page: deleting the anchor releases at least
		 * plain_sz - follower_sz bytes (the plain form is the full schema
		 * the anchor had; the follower only stored the trailing attr), so
		 * the subsequent overwrite can never run out of space.
		 */
		for (int j = 0; j < ndeletable; j++)
		{
			if (deletable[j] < survivor)
				precedingdeletes++;
			else
				break;
		}

		promotions[nprom].off = survivor - precedingdeletes;
		promotions[nprom].plain = plain;
		promotions[nprom].plainsz = MAXALIGN(IndexTupleSize(plain));
		nprom++;
	}

	return nprom;
}

/*
 * _bt_prefix_vacuum_apply() -- apply the promotions planned by
 * _bt_prefix_vacuum_plan().
 *
 * Must be called *after* PageIndexMultiDelete() has applied the deletable[]
 * list, so that the offsets recorded in promotions[] already reflect the
 * post-delete line-pointer shift.  Delete-first ordering is what makes this
 * routine's space use always-fit by construction, as follows.
 *
 * Each promotion grows one follower from size F to plain size P, where
 *   P = header + prefix_keys + suffix_keys + follower_include, and
 *   F = header + suffix_keys + follower_include.
 * So the growth is (P - F) = prefix_keys_size (within alignment slop).  The
 * anchor whose deletion motivated this promotion had size
 *   A = header + prefix_keys + suffix_keys + anchor_include >= prefix_keys,
 * so PageIndexMultiDelete() has already freed at least (P - F) bytes per
 * promotion.  Across multiple promotions on the same page the accounting
 * sums linearly (each promotion has its own deleted anchor contributing
 * enough bytes), so PageIndexTupleOverwrite() cannot run out of space.
 *
 * Hence failure here would indicate either (a) an invariant break in the
 * planner (e.g. a promotion scheduled without its anchor actually being
 * deleted), or (b) a broken page.  We PANIC because we are inside a critical
 * section and past the point of no return.
 *
 * Safe to call inside a critical section: the palloc()s that build
 * promotions[i].plain happen in the planner, outside the critsec.
 */
void
_bt_prefix_vacuum_apply(Page page, BTPrefixPromotion *promotions, int nprom)
{
	for (int i = 0; i < nprom; i++)
	{
		if (!PageIndexTupleOverwrite(page, promotions[i].off,
									 promotions[i].plain,
									 promotions[i].plainsz))
			elog(PANIC,
				 "failed to promote compressed follower to anchor at offset %u",
				 promotions[i].off);
	}
}

/*
 * _bt_prefix_reconstruct() -- return a plain-looking IndexTuple for offnum.
 *
 * If the item at offnum is a plain (non-compressed) tuple, the raw pointer
 * from PageGetItem() is returned unchanged.  If it is a compressed follower,
 * this function resolves its governing anchor (updating the state as needed)
 * and materializes a full tuple via _bt_prefix_form_tuple().
 *
 * 'dstbuf' behaves as described for _bt_prefix_form_tuple().
 */
IndexTuple
_bt_prefix_reconstruct(Relation rel, BTPrefixPageState *st,
					   OffsetNumber offnum, char *dstbuf)
{
	IndexTuple	raw;

	Assert(st->page != NULL);
	Assert(offnum >= FirstOffsetNumber);
	Assert(offnum <= PageGetMaxOffsetNumber(st->page));

	raw = (IndexTuple) PageGetItem(st->page, PageGetItemId(st->page, offnum));

	if (!BTreeTupleIsCompressed(raw))
	{
		/*
		 * Plain anchor (or high key, or internal pivot if caller misused the
		 * helper).  Opportunistically refresh the state so subsequent
		 * followers in the same group can skip the find-anchor walk.
		 */
		if (!BTreeTupleIsPivot(raw) && !BTreeTupleIsPosting(raw) &&
			offnum != P_HIKEY)
			_bt_prefix_state_set_anchor(st, offnum);
		return raw;
	}

	/* Compressed follower: ensure we have the governing anchor loaded */
	if (st->anchor_off == InvalidOffsetNumber ||
		offnum < st->anchor_off ||
		(st->next_anchor_off != InvalidOffsetNumber &&
		 offnum >= st->next_anchor_off))
	{
		OffsetNumber aoff = _bt_prefix_find_anchor(st->page, offnum);

		if (aoff == InvalidOffsetNumber)
			elog(ERROR, "no prefix anchor for compressed tuple at offset %u",
				 offnum);
		_bt_prefix_state_set_anchor(st, aoff);
	}

	/*
	 * Pick up natts_anchor from the relation's schema.  For v1, the value is
	 * a per-index constant (= nkeyatts - 1) and the state's natts_anchor is
	 * set here; a future version that records natts_anchor per-anchor can
	 * read it from the anchor itself.
	 */
	st->natts_anchor = IndexRelationGetNumberOfKeyAttributes(rel) - 1;

	return _bt_prefix_form_tuple(rel, st, raw, dstbuf);
}
