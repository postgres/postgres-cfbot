/*-------------------------------------------------------------------------
 *
 * hot_indexed.h
 *	  Inline-trailing modified-attributes bitmap for HOT-indexed (HOT/SIU)
 *	  tuples.
 *
 * A heap tuple produced by a HOT-indexed UPDATE has HEAP_INDEXED_UPDATED set
 * in t_infomask2 and carries, appended after its normal attribute data, a
 * fixed-size bitmap recording which heap attributes changed at this chain hop
 * (relative to the prior chain member).  Bit (attnum - 1) corresponds to user
 * attribute attnum.
 *
 * The bitmap is ceil(natts / 8) bytes, where natts is the tuple's own
 * attribute count at the time it was written (HeapTupleHeaderGetNatts for a
 * live tuple; preserved separately for a stub, see below).  Its length is not
 * stored as such; it occupies the final HotIndexedBitmapBytes(natts) bytes of
 * the line pointer's item and is located via ItemIdGetLength(), past the
 * attribute data: routines that deform the tuple stop at natts and never see
 * it.
 *
 * Because ADD COLUMN raises the relation's natts without rewriting existing
 * tuples, a chain can hold tuples whose bitmaps were sized for different
 * (smaller) natts than the relation has now.  Consumers therefore size and
 * locate a tuple's bitmap from that tuple's OWN write-time natts
 * (HotIndexedTupleBitmapNatts), never from the relation's current natts.
 * Bit positions are attribute based and identical across sizes, so a smaller
 * bitmap simply ORs into the low bytes of a larger accumulator.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hot_indexed.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HOT_INDEXED_H
#define HOT_INDEXED_H

#include "access/htup_details.h"

/*
 * Number of bytes in the trailing modified-attrs bitmap for a relation with
 * natts user attributes (one bit per attribute, attnum - 1).
 */
static inline Size
HotIndexedBitmapBytes(int natts)
{
	Assert(natts >= 0);
	return (Size) ((natts + 7) / 8);
}

/*
 * Read-only pointer to the trailing modified-attrs bitmap of a HOT-indexed
 * tuple.  item_len is ItemIdGetLength() of the tuple's line pointer; natts is
 * the tuple's OWN write-time attribute count (HotIndexedTupleBitmapNatts()),
 * NOT the relation's current natts; see the invariant at the top of this
 * file: after ADD COLUMN a chain can hold tuples whose bitmaps were sized for
 * a smaller natts, and locating the bitmap from the relation's current natts
 * would read past the tuple's data.  The caller must have verified that the
 * tuple has HEAP_INDEXED_UPDATED set.
 */
static inline const uint8 *
HotIndexedGetModifiedBitmap(const HeapTupleHeaderData *htup,
							Size item_len, int natts)
{
	return (const uint8 *) ((const char *) htup +
							item_len - HotIndexedBitmapBytes(natts));
}

/* Writable variant, for OR-ing the union onto a surviving redirect target. */
static inline uint8 *
HotIndexedGetModifiedBitmapRW(HeapTupleHeaderData *htup,
							  Size item_len, int natts)
{
	return (uint8 *) ((char *) htup +
					  item_len - HotIndexedBitmapBytes(natts));
}

/* True if user attribute attnum (1-based) is set in the bitmap. */
static inline bool
HotIndexedAttrIsModified(const uint8 *bitmap, AttrNumber attnum)
{
	int			bit = attnum - 1;

	Assert(attnum >= 1);
	return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

/* Set user attribute attnum (1-based) in the bitmap. */
static inline void
HotIndexedSetAttrModified(uint8 *bitmap, AttrNumber attnum)
{
	int			bit = attnum - 1;

	Assert(attnum >= 1);
	bitmap[bit / 8] |= (uint8) (1 << (bit % 8));
}

/*
 * OR the first nbytes of src into dst.  dst must be at least nbytes long; it
 * may be longer (sized for a larger natts); bit positions are attribute
 * based and identical across sizes, so OR-ing only src's bytes is correct.
 */
static inline void
HotIndexedBitmapUnion(uint8 *dst, const uint8 *src, int src_natts)
{
	Size		nbytes = HotIndexedBitmapBytes(src_natts);

	for (Size i = 0; i < nbytes; i++)
		dst[i] |= src[i];
}

/*
 * Is every bit set in sub also set in super?  sub is sized for sub_natts;
 * super must be at least that long.  Used by prune to decide a collapse
 * survivor is reclaimable: when the attributes a dead member changed are all
 * changed again by later hops, every index entry pointing at that member is
 * superseded (stale), so it carries no live entry.
 */
static inline bool
HotIndexedBitmapIsSubset(const uint8 *sub, const uint8 *super, int sub_natts)
{
	Size		nbytes = HotIndexedBitmapBytes(sub_natts);

	for (Size i = 0; i < nbytes; i++)
		if ((sub[i] & ~super[i]) != 0)
			return false;
	return true;
}

/*
 * Stub line pointers (collapse survivors).
 *
 * When prune collapses a dead HOT-indexed chain it cannot keep the dead key
 * tuples as live data-bearing tuples (their XIDs would hold back
 * relfrozenxid).  Each preserved dead key tuple is instead converted to an
 * xid-free "stub": an LP_NORMAL item whose HeapTupleHeader is frozen and
 * marked not-a-real-tuple (HEAP_INDEXED_UPDATED set, HeapTupleHeaderGetNatts
 * == 0), whose t_ctid.offnum forwards to the next key tuple on the page, and
 * whose payload is the modified-attrs bitmap for the segment it represents --
 * located, like a live tuple's inline bitmap, in the final
 * HotIndexedBitmapBytes(natts) bytes of the item, so the same accessors work
 * for both.  Because the natts field is overwritten with the 0 sentinel, the
 * stub's write-time natts (needed to size/locate that bitmap) is preserved in
 * the otherwise-unused block-number half of t_ctid; HotIndexedStubGetForward
 * reads only the offset half.
 *
 * A stub is signature-skipped (not visibility-skipped) by every consumer that
 * walks LP_NORMAL items.
 */
static inline bool
HotIndexedHeaderIsStub(const HeapTupleHeaderData *tup)
{
	return (tup->t_infomask2 & HEAP_INDEXED_UPDATED) != 0 &&
		HeapTupleHeaderGetNatts(tup) == 0;
}

/* Offset of the next key tuple this stub forwards to (same page). */
static inline OffsetNumber
HotIndexedStubGetForward(const HeapTupleHeaderData *tup)
{
	return ItemPointerGetOffsetNumberNoCheck(&tup->t_ctid);
}

/*
 * Bitmap sizing across the relation's lifetime.
 *
 * The trailing bitmap is ceil(natts / 8) bytes, where natts is the relation's
 * attribute count *at the time the tuple was written*.  ADD COLUMN raises the
 * relation's natts without rewriting existing tuples, so a chain can hold
 * tuples whose bitmaps were sized for different (smaller) natts than the
 * relation has now.  Every consumer must therefore size and locate a tuple's
 * bitmap from that tuple's own write-time natts, never from the relation's
 * current natts.
 *
 * For a live HOT-indexed tuple the write-time natts is HeapTupleHeaderGetNatts
 * (a freshly formed UPDATE tuple stores the full relation natts, which is what
 * heap_form_hot_indexed_tuple sized the bitmap with).  A stub overwrites natts
 * with 0 as its sentinel, so it preserves its write-time natts in the unused
 * block-number half of t_ctid instead (the offset half holds the forward
 * link).
 */
static inline void
HotIndexedStubSetBitmapNatts(HeapTupleHeaderData *tup, int natts)
{
	BlockIdSet(&tup->t_ctid.ip_blkid, (BlockNumber) natts);
}

static inline int
HotIndexedStubGetBitmapNatts(const HeapTupleHeaderData *tup)
{
	return (int) BlockIdGetBlockNumber(&tup->t_ctid.ip_blkid);
}

/* Write-time natts of any HOT-indexed item (live tuple or stub). */
static inline int
HotIndexedTupleBitmapNatts(const HeapTupleHeaderData *tup)
{
	if (HotIndexedHeaderIsStub(tup))
		return HotIndexedStubGetBitmapNatts(tup);
	return HeapTupleHeaderGetNatts(tup);
}

#endif							/* HOT_INDEXED_H */
