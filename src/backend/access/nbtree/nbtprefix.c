/*-------------------------------------------------------------------------
 *
 * nbtprefix.c
 *	  Leaf key prefix compression for Postgres btrees.
 *
 * A leaf of an index whose prefix_compression reloption is on may be flagged
 * BTP_PREFIX: its data items are stored "coded" against per-page group
 * anchors, dropping the leading bytes each shares with its anchor.  The
 * on-disk format is described at BTP_PREFIX in nbtree.h; the README covers how
 * the rest of nbtree uses it.
 *
 * Everything here works on pages and tuples alone, never the catalog, so WAL
 * redo can lay out a coded page exactly as the primary did.  A page gets its
 * directory when it is written as a whole (CREATE INDEX, either half of a
 * split); an insert codes the new tuple against an existing group, sharing
 * however much it happens to share, and deduplication re-codes against the
 * page's existing directory.
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

#include "access/itup.h"
#include "access/nbtree.h"
#include "access/tupmacs.h"
#include "storage/bufpage.h"
#include "utils/rel.h"
#include "varatt.h"

/* In-memory anchor: the group's first member, as a plain tuple. */
typedef struct BTPrefixAnchor
{
	IndexTuple	rep;			/* representative tuple owning the shared
								 * bytes */
} BTPrefixAnchor;

/* In-memory directory of a page being laid out by _bt_prefix_form_page. */
typedef struct BTPrefixDir
{
	int			ngroups;
	BTPrefixAnchor anchors[FLEXIBLE_ARRAY_MEMBER];
} BTPrefixDir;

/*
 * On-disk per-key-attribute layout (BTPrefixAttr in nbtree.h), enough to parse
 * a tuple's data area without the catalog.  The primary fills it from
 * RelationGetDescr(index); decode reads it from the directory so decoding
 * stays page-only.  The column walk (_bt_prefix_walk) uses every key column's
 * attlen, attalignby, and by-value flag to find where two tuples diverge, so
 * the whole key-column layout is stored.
 */
#define BTPREFIX_ATTR_BYVAL		0x01

/*
 * On-disk directory item header.  decodedsz and maxdecoded cache the decoded
 * (plain) size of the page's data items, each counted with its line pointer:
 * their sum and the largest.  They make _bt_prefix_split_needed O(1).  Every
 * change to the data items keeps them exact, through page-only routines that
 * WAL redo repeats (_bt_prefix_note_insert, _bt_prefix_recount).
 *
 * The variable tail is, in order: `natts' BTPrefixAttr records (the key-column
 * layout), the `ngroups' anchor offsets, and the packed anchors.  Only the
 * first two are fixed-position; the anchor-offset array starts after the
 * layout, so _bt_prefix_anchoroff computes its base from natts.  natts == 0
 * means no layout is recorded, so coding falls back to sharing the whole data
 * area from offset 0 (the pre-layout behavior); the primary always records the
 * real layout, but a redo path that cannot obtain it page-only writes 0.
 */
typedef struct BTPrefixDirHdr
{
	IndexTupleData header;		/* t_tid invalid; t_info = whole item size */
	uint16		ngroups;
	uint16		decodedsz;		/* sum of decoded item sizes + line pointers */
	uint16		maxdecoded;		/* largest of those */
	uint16		natts;			/* key-column count, layout records that
								 * follow */
	BTPrefixAttr attrs[FLEXIBLE_ARRAY_MEMBER];	/* [natts], then anchoroff[] */
} BTPrefixDirHdr;

/* One packed anchor in the directory item. */
typedef struct BTPrefixAnchorDisk
{
	uint16		repsz;			/* size of the representative tuple blob */
	/* IndexTupleData rep[repsz] follows, MAXALIGN'd */
} BTPrefixAnchorDisk;

/*
 * Code header of a coded member, right after its IndexTupleData header.  The
 * member layout is described at BTP_PREFIX in nbtree.h.
 *
 * lead_shared (= D) is the data-area byte offset of the divergent column: the
 * first key column whose value differs from the anchor's.  Every byte before
 * it, the fully-shared leading columns and their length headers, is identical
 * across the group and comes from the anchor.  shared_len (= sv) is the
 * divergent column's shared value bytes, after its length header.  Only that
 * one column's header is kept private to each member, so a run of equal
 * leading columns is shared whole even when the divergent column's length
 * header differs between members.
 */
typedef struct BTPrefixCode
{
	uint16		group;			/* anchor index in the directory */
	uint16		lead_shared;	/* data-area bytes of fully-shared leading
								 * cols */
	uint16		shared_len;		/* divergent column's shared value bytes */
	uint16		orig_info;		/* original tuple's t_info (size, flags) */
} BTPrefixCode;

/*
 * A coded member is never larger than its plain form by more than
 * BTREE_PREFIX_CODE_OVERHEAD, which _bt_findinsertloc reserves before coding.
 * The worst case is a posting tuple, whose body and posting list are each
 * MAXALIGN'd: MAXALIGN(code header + key) + MAXALIGN(posting list) exceeds
 * the plain MAXALIGN(key + posting list) by at most the code header plus one
 * alignment of the posting list.
 */
StaticAssertDecl(BTREE_PREFIX_CODE_OVERHEAD >=
				 MAXALIGN(sizeof(BTPrefixCode)) + MAXALIGN(sizeof(ItemPointerData)),
				 "BTREE_PREFIX_CODE_OVERHEAD too small for the coded member header");

/*
 * _bt_prefix_enabled
 *		Is prefix compression turned on for this index?
 */
bool
_bt_prefix_enabled(Relation index)
{
	return index->rd_options &&
		((BTOptions *) index->rd_options)->prefix_compression;
}

/*
 * Size of a tuple's key part: everything but a posting list.  Index tuples are
 * MAXALIGN'd, and so is a posting list's offset, so this is MAXALIGN'd too.
 */
static inline Size
_bt_prefix_keysz(IndexTuple itup)
{
	return BTreeTupleIsPosting(itup) ? BTreeTupleGetPostingOffset(itup) :
		IndexTupleSize(itup);
}

/*
 * Byte offset of the anchor-offset array within a directory item holding
 * `natts' layout records.  The layout precedes it; the anchors follow it.
 */
static inline Size
_bt_prefix_anchoroff_base(int natts)
{
	return offsetof(BTPrefixDirHdr, attrs) + sizeof(BTPrefixAttr) * natts;
}

/* The anchor-offset array of a directory header (natts layout records). */
static inline uint16 *
_bt_prefix_anchoroff(BTPrefixDirHdr *hdr)
{
	return (uint16 *) ((char *) hdr + _bt_prefix_anchoroff_base(hdr->natts));
}

/*
 * Size of a directory item for `ngroups' groups and `natts' key-column layout
 * records, whose MAXALIGN'd anchor blobs total `anchorsz' bytes.
 */
static inline Size
_bt_prefix_diritem_size(int natts, int ngroups, Size anchorsz)
{
	return MAXALIGN(_bt_prefix_anchoroff_base(natts) +
					sizeof(uint16) * ngroups) + anchorsz;
}

/*
 * Where two tuples first diverge inside one key column.  D (lead_shared) is
 * the data-area offset of that divergent column, so [0..D) is byte-identical
 * between the two (they share every leading column whole, header and value).
 * sv (shared_len) is the divergent column's common value-byte prefix, after
 * its length header.  Decode and coding carry both through BTPrefixCode.
 */
typedef struct BTPrefixSplit
{
	Size		lead_shared;	/* D: data-area bytes of fully-shared cols */
	Size		shared_len;		/* sv: divergent col's shared value bytes */
} BTPrefixSplit;

/*
 * Validated splice offsets of a coded member, filled by
 * _bt_prefix_check_member and consumed by _bt_prefix_decode so the two cannot
 * disagree: D (lead_shared), sv (shared_len), the member's and the anchor's
 * divergent-column prefix widths (mhdr, ahdr), and the member's stored residual
 * length.  All are bounded against the member and the anchor before use.
 */
typedef struct BTPrefixSplice
{
	Size		lead_shared;	/* D */
	Size		shared_len;		/* sv */
	Size		mhdr;			/* member's divergent-column prefix */
	Size		ahdr;			/* anchor's divergent-column prefix */
	Size		residual;		/* member's stored residual bytes */
} BTPrefixSplice;

/*
 * Width of the length header of a key column whose value begins at `data'.
 *
 * `attr' is the column's stored layout.  A varlena column's header is 1 or 4
 * bytes, read from `data'; any fixed-length or by-value column has none.  The
 * column walk uses this to step over each column's header.
 */
static inline Size
_bt_prefix_header_width(const BTPrefixAttr * attr, const char *data)
{
	if (attr->attlen != -1 || (attr->flags & BTPREFIX_ATTR_BYVAL) != 0)
		return 0;
	return VARATT_IS_1B(data) ? VARHDRSZ_SHORT : VARHDRSZ;
}

/*
 * Advance `off' (a data-area byte offset) past key column `i' of a tuple whose
 * data area starts at `data' and whose null bitmap is `bitmap' (NULL when the
 * tuple has no nulls).  Sets *valoff to the column's value offset (after any
 * length header) and *vallen to its value length; a null column occupies no
 * bytes and reports *vallen 0.  `attrs'/`natts' is the stored layout, `limit'
 * the data-area length (a bound so a malformed length cannot walk off the
 * tuple).  Returns false if column `i' does not fit within `limit'.
 */
static inline bool
_bt_prefix_column(const BTPrefixAttr * attrs, int natts, int i,
				  const char *data, const uint8 *bitmap, Size limit,
				  Size *off, Size *valoff, Size *vallen)
{
	const		BTPrefixAttr *attr = &attrs[i];
	Size		start;
	Size		hdr;
	Size		len;

	if (bitmap != NULL && att_isnull(i, bitmap))
	{
		*valoff = *off;
		*vallen = 0;
		return true;			/* a null column occupies no data bytes */
	}

	/* align the column start, honoring the packed-varlena rule */
	if (attr->attlen == -1 && *off < limit && VARATT_NOT_PAD_BYTE(data + *off))
		start = *off;
	else
		start = att_nominal_alignby(*off, attr->attalignby);
	if (start > limit)
		return false;

	if (attr->attlen > 0)
	{
		hdr = 0;
		len = attr->attlen;
	}
	else if (attr->attlen == -1)
	{
		if (start >= limit)
			return false;		/* no room for a length header */
		hdr = _bt_prefix_header_width(attr, data + start);
		if (start + hdr > limit)
			return false;
		len = VARSIZE_ANY(data + start);
	}
	else
	{
		Assert(attr->attlen == -2); /* cstring */
		hdr = 0;
		len = strnlen(data + start, limit - start) + 1;
	}

	if (start + len > limit)
		return false;
	*valoff = start + hdr;
	*vallen = len - hdr;
	*off = start + len;
	return true;
}

/*
 * Walk `rep' and `itup' column by column and set *split to where their key
 * values first diverge: the data-area offset D of the first column whose value
 * bytes differ, and sv the common value-byte prefix of that column.
 *
 * Every column before the divergent one has byte-identical value bytes in both
 * tuples; a column equal by value has equal length, hence an identical length
 * header and identical alignment padding, so [0..D) is byte-identical and can
 * be shared whole from the anchor.  The divergent column is the last shared
 * one, so re-inserting its (possibly different-width) header per member cannot
 * shift the alignment of any shared column.  Without a layout (`attrs' NULL,
 * CREATE INDEX before the descriptor exists), it shares the whole data area as
 * v15 did: D = 0 and sv = the common byte prefix from offset 0.  A single
 * leading varlena column makes the divergent column column 0, so D = 0 and the
 * on-disk form is byte-identical to v16.
 */
static void
_bt_prefix_walk(const BTPrefixAttr * attrs, int natts,
				IndexTuple rep, IndexTuple itup, BTPrefixSplit *split)
{
	Size		rephoff = IndexInfoFindDataOffset(rep->t_info);
	Size		hoff = IndexInfoFindDataOffset(itup->t_info);
	const char *rd = (const char *) rep + rephoff;
	const char *md = (const char *) itup + hoff;
	const uint8 *rbitmap = IndexTupleHasNulls(rep) ?
		(const uint8 *) rep + sizeof(IndexTupleData) : NULL;
	const uint8 *mbitmap = IndexTupleHasNulls(itup) ?
		(const uint8 *) itup + sizeof(IndexTupleData) : NULL;
	Size		replim = _bt_prefix_keysz(rep) - rephoff;
	Size		mlim = _bt_prefix_keysz(itup) - hoff;
	Size		roff = 0;
	Size		moff = 0;

	/* No layout: share the whole data area, with no carved header (v15). */
	if (attrs == NULL)
	{
		Size		limit = Min(replim, mlim);
		Size		n = 0;

		while (n < limit && rd[n] == md[n])
			n++;
		split->lead_shared = 0;
		split->shared_len = n;
		return;
	}

	for (int i = 0; i < natts; i++)
	{
		Size		rvaloff,
					rvallen,
					mvaloff,
					mvallen;
		Size		common;
		Size		cmplen;
		Size		divoff = Max(roff, moff);

		if (!_bt_prefix_column(attrs, natts, i, rd, rbitmap, replim,
							   &roff, &rvaloff, &rvallen) ||
			!_bt_prefix_column(attrs, natts, i, md, mbitmap, mlim,
							   &moff, &mvaloff, &mvallen))
		{
			split->lead_shared = divoff;
			split->shared_len = 0;
			return;
		}

		/*
		 * A column stays in the shared leading span only when its whole
		 * data-area footprint is byte-identical in both tuples: the same
		 * total length (roff == moff) and the same bytes.  That covers the
		 * length header and any alignment padding, and it separates a null
		 * column (no bytes) from an empty value (a bare varlena header),
		 * which share the same empty value but occupy different footprints.
		 * Compare from divoff, where both columns began in lockstep.
		 */
		if (roff == moff &&
			memcmp(rd + divoff, md + divoff, roff - divoff) == 0)
			continue;

		/*
		 * The divergent column: D is its data-area offset, sv its common
		 * value-byte prefix (0 when one side is null, since a null column has
		 * no value to share).
		 */
		cmplen = Min(rvallen, mvallen);
		common = 0;
		while (common < cmplen && rd[rvaloff + common] == md[mvaloff + common])
			common++;
		split->lead_shared = divoff;
		split->shared_len = common;
		return;
	}

	/* Every key column matched: share the whole data area, no divergent col. */
	Assert(roff == moff);
	split->lead_shared = roff;
	split->shared_len = 0;
}

/*
 * Divergent-column prefix (alignment padding plus length header) at data-area
 * offset D: the bytes decode must keep with each member before splicing in the
 * shared value.  Columns [0..D) are byte-identical across the group, so this
 * walks `data' (any group member's data area, bounded by `limit') with null
 * bitmap `bitmap' to the column that begins at D, then measures that column in
 * `divdata' (the bytes of the data area from D, bounded by `divlimit').  For
 * the anchor, `divdata' is `data + D'; for a coded member it is the member's
 * residual, whose [0..D) bytes equal the anchor's.  Returns 0 when D is at or
 * past the data area, the divergent column is null, or every column is shared;
 * sets *ok false (returns 0) on a malformed length that would overrun.
 *
 * A short (packed) varlena carries a 1-byte header with no padding; a long one
 * aligns and carries a 4-byte header.  Two members of one group can therefore
 * store prefixes of different width and still reconstruct.
 */
static Size
_bt_prefix_divprefix(const BTPrefixAttr * attrs, int natts,
					 const char *data, const uint8 *bitmap, Size limit,
					 const char *divdata, Size divlimit, Size lead_shared,
					 bool *ok)
{
	Size		off = 0;

	*ok = true;
	if (attrs == NULL || lead_shared >= limit)
		return 0;

	for (int i = 0; i < natts; i++)
	{
		const		BTPrefixAttr *attr = &attrs[i];
		Size		valoff,
					vallen;
		Size		pad;

		if (off > lead_shared)
			return 0;			/* D fell inside a column, not a boundary */
		if (bitmap != NULL && att_isnull(i, bitmap))
			continue;			/* null column: no bytes, no carved header */
		if (off < lead_shared)
		{
			if (!_bt_prefix_column(attrs, natts, i, data, bitmap, limit,
								   &off, &valoff, &vallen))
			{
				*ok = false;
				return 0;
			}
			continue;
		}

		/* column i is the first non-null column beginning at D */
		if (attr->attlen != -1 || (attr->flags & BTPREFIX_ATTR_BYVAL) != 0)
			return 0;			/* fixed or by-value: value starts at D */

		/* varlena: packed short stays unaligned, else align for a 4-byte hdr */
		if (divlimit > 0 && VARATT_NOT_PAD_BYTE(divdata))
			pad = 0;
		else
			pad = att_nominal_alignby(lead_shared, attr->attalignby) -
				lead_shared;
		if (pad + VARHDRSZ_SHORT > divlimit)
		{
			*ok = false;
			return 0;
		}
		if (VARATT_IS_1B(divdata + pad))
			return pad + VARHDRSZ_SHORT;
		if (pad + VARHDRSZ > divlimit)
		{
			*ok = false;
			return 0;
		}
		return pad + VARHDRSZ;
	}
	return 0;					/* every column shared: no divergent column */
}

/*
 * Divergent-column prefix of whole tuple `itup' at offset D (see
 * _bt_prefix_divprefix).  For the anchor, which is a complete tuple; the
 * caller has bounded D against the key size.
 */
static inline Size
_bt_prefix_divhdr(const BTPrefixAttr * attrs, int natts,
				  IndexTuple itup, Size lead_shared)
{
	Size		hoff = IndexInfoFindDataOffset(itup->t_info);
	Size		limit = _bt_prefix_keysz(itup) - hoff;
	const char *data = (const char *) itup + hoff;
	const uint8 *bitmap = IndexTupleHasNulls(itup) ?
		(const uint8 *) itup + sizeof(IndexTupleData) : NULL;
	bool		ok;

	return _bt_prefix_divprefix(attrs, natts, data, bitmap, limit,
								data + lead_shared, limit - lead_shared,
								lead_shared, &ok);
}

/*
 * Total data-area bytes `itup' shares with `rep': the fully-shared leading
 * columns D plus the divergent column's shared value prefix sv.  This is what
 * coding removes from the member, so grouping and the size estimate rank by
 * it.
 */
static inline Size
_bt_prefix_shared(const BTPrefixSplit *split)
{
	return split->lead_shared + split->shared_len;
}

/*
 * Size of `itup' coded against representative `rep' (what
 * _bt_prefix_encode_against would produce), without forming it.  `shared'
 * counts the data-area bytes dropped (leading columns plus the divergent
 * value prefix); the member keeps its own divergent-column header, which is
 * part of the residual, so the residual is the data area minus `shared'.
 */
static Size
_bt_prefix_coded_size(IndexTuple itup, Size shared)
{
	Size		hoff = IndexInfoFindDataOffset(itup->t_info);
	Size		sz;

	sz = MAXALIGN(sizeof(IndexTupleData) + sizeof(BTPrefixCode) +
				  (hoff - sizeof(IndexTupleData)) +
				  (_bt_prefix_keysz(itup) - hoff - shared));
	if (BTreeTupleIsPosting(itup))
		sz = MAXALIGN(sz + BTreeTupleGetNPosting(itup) * sizeof(ItemPointerData));
	return sz;
}

/*
 * _bt_prefix_group_start
 *		Should `itup' start a new group?
 *
 * `rep' is the current group's representative and `prev' the preceding tuple
 * (both NULL for the page's first tuple).  Sets *shared to the data-area bytes
 * `itup' shares with the representative it ends up under (itself if it starts
 * a group): the fully-shared leading columns plus the divergent value prefix.
 * This is the only grouping rule, shared by the CREATE INDEX estimate
 * (_bt_prefix_est_add) and the layout (_bt_prefix_choose_groups), so the two
 * cannot disagree.
 *
 * A group starts when `itup' shares nothing with `rep', or shares at least one
 * MAXALIGN quantum more with `prev' than with `rep'.  The second case keeps a
 * long run of tuples that share much among themselves from being locked to a
 * short accidental prefix of the page's first tuple (e.g., just a leading
 * fixed-width column).  The rule looks only backwards and the new group is
 * led by `itup', so no earlier tuple is ever regrouped and the estimate stays
 * exact tuple by tuple.
 */
static bool
_bt_prefix_group_start(const BTPrefixAttr * attrs, int natts,
					   IndexTuple rep, IndexTuple prev, IndexTuple itup,
					   Size *shared)
{
	BTPrefixSplit split;
	Size		s_rep;

	if (rep == NULL)
	{
		_bt_prefix_walk(attrs, natts, itup, itup, &split);
		*shared = _bt_prefix_shared(&split);
		return true;
	}

	_bt_prefix_walk(attrs, natts, rep, itup, &split);
	s_rep = _bt_prefix_shared(&split);
	if (s_rep == 0)
	{
		_bt_prefix_walk(attrs, natts, itup, itup, &split);
		*shared = _bt_prefix_shared(&split);
		return true;
	}

	if (prev != NULL && prev != rep)
	{
		BTPrefixSplit psplit;
		Size		s_prev;

		_bt_prefix_walk(attrs, natts, prev, itup, &psplit);
		s_prev = _bt_prefix_shared(&psplit);
		if (s_prev >= s_rep + MAXIMUM_ALIGNOF)
		{
			_bt_prefix_walk(attrs, natts, itup, itup, &split);
			*shared = _bt_prefix_shared(&split);
			return true;
		}
	}

	*shared = s_rep;
	return false;
}

/*
 * Running size estimate of a BTP_PREFIX page laid out from a sequence of plain
 * tuples in key order, as _bt_prefix_form_page would lay it out (same
 * grouping, same coded sizes).  Used by CREATE INDEX to fill coded pages.
 * The tuples passed to _bt_prefix_est_add must stay valid while the estimate
 * is in use.
 */
void
_bt_prefix_est_add(BTPrefixPageEst *est, const BTPrefixAttr * attrs,
				   int natts, IndexTuple itup)
{
	Size		plain = MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
	Size		shared;

	est->natts = natts;
	if (_bt_prefix_group_start(attrs, natts, est->grouprep, est->prev, itup,
							   &shared))
	{
		est->grouprep = itup;
		est->ngroups++;
		est->anchorsz += MAXALIGN(MAXALIGN(sizeof(BTPrefixAnchorDisk)) +
								  _bt_prefix_keysz(itup));
	}
	est->lastcoded = _bt_prefix_coded_size(itup, shared) +
		sizeof(ItemIdData);
	est->maxcoded = Max(est->maxcoded, est->lastcoded - sizeof(ItemIdData));
	est->codedsz += est->lastcoded;
	est->plainsz += plain;
	est->maxplain = Max(est->maxplain, plain);
	est->prev = itup;
}

/* Bytes (items plus line pointers) the coded page would use */
Size
_bt_prefix_est_coded(const BTPrefixPageEst *est)
{
	return _bt_prefix_diritem_size(est->natts, est->ngroups, est->anchorsz) +
		sizeof(ItemIdData) + est->codedsz;
}

/* The directory and each member must fit the IndexTuple size field. */
bool
_bt_prefix_est_representable(const BTPrefixPageEst *est)
{
	return _bt_prefix_diritem_size(est->natts, est->ngroups, est->anchorsz) <=
		INDEX_SIZE_MASK &&
		est->maxcoded <= INDEX_SIZE_MASK;
}

/*
 * Is a coded page worth having, given its coded and plain sizes?  Demand a
 * real saving: decoding costs CPU on every access, so a page that would save
 * only a few bytes stays plain.
 */
bool
_bt_prefix_worthwhile(Size codedsz, Size plainsz)
{
	return codedsz * 16 <= plainsz * 15;
}

/*
 * _bt_prefix_build_layout
 *		Fill `attrs' (INDEX_MAX_KEYS entries) with the index's key-column
 *		layout and return the key count.
 *
 * The primary calls this from RelationGetDescr(index) at page-write time; the
 * layout it returns is stored in the directory so decode can parse tuples
 * page-only.
 */
int
_bt_prefix_build_layout(Relation index, BTPrefixAttr * attrs)
{
	TupleDesc	tupdesc = RelationGetDescr(index);
	int			natts = IndexRelationGetNumberOfKeyAttributes(index);

	Assert(natts >= 1 && natts <= INDEX_MAX_KEYS);
	for (int i = 0; i < natts; i++)
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupdesc, i);

		attrs[i].attlen = attr->attlen;
		attrs[i].attalignby = attr->attalignby;
		attrs[i].flags = attr->attbyval ? BTPREFIX_ATTR_BYVAL : 0;
	}
	return natts;
}

/*
 * _bt_prefix_choose_groups
 *		Group plain tuples items[0..n-1], in key order, by
 *		_bt_prefix_group_start.
 *
 * Returns a palloc'd BTPrefixDir, or NULL when no two tuples share a group.
 */
static BTPrefixDir *
_bt_prefix_choose_groups(const BTPrefixAttr * attrs, int natts,
						 IndexTuple *items, int n)
{
	BTPrefixDir *dir;
	int			ngroups = 0;

	StaticAssertStmt(MaxIndexTuplesPerPage <= BTREE_PREFIX_MAXGROUPS,
					 "group number must fit in uint16");

	if (n <= 1)
		return NULL;

	dir = palloc(offsetof(BTPrefixDir, anchors) + sizeof(BTPrefixAnchor) * n);
	for (int i = 0; i < n; i++)
	{
		IndexTuple	rep = ngroups == 0 ? NULL : dir->anchors[ngroups - 1].rep;
		IndexTuple	prev = i == 0 ? NULL : items[i - 1];
		Size		shared;

		if (_bt_prefix_group_start(attrs, natts, rep, prev, items[i], &shared))
			dir->anchors[ngroups++].rep = items[i];
	}

	if (ngroups >= n)
	{
		pfree(dir);
		return NULL;			/* every tuple its own group: nothing shared */
	}

	dir->ngroups = ngroups;
	return dir;
}

/*
 * Serialize `dir' as a palloc'd directory item, setting *itemsz to its size.
 * `attrs'/`natts' is the key-column layout stored in the header for page-only
 * decode.  The cached decoded sizes are left zero for _bt_prefix_recount.
 */
static IndexTuple
_bt_prefix_form_diritem(BTPrefixDir *dir, const BTPrefixAttr * attrs, int natts,
						Size *itemsz)
{
	int			ng = dir->ngroups;
	Size		total = _bt_prefix_diritem_size(natts, ng, 0);
	Size	   *anchoroff = palloc(sizeof(Size) * ng);
	char	   *buf;
	IndexTuple	item;
	BTPrefixDirHdr *hdr;
	uint16	   *offarr;

	for (int g = 0; g < ng; g++)
	{
		Size		repsz = _bt_prefix_keysz(dir->anchors[g].rep);

		anchoroff[g] = total;
		total += MAXALIGN(MAXALIGN(sizeof(BTPrefixAnchorDisk)) + repsz);
	}

	if (total > INDEX_SIZE_MASK)
		elog(ERROR, "nbtree prefix directory too large (%zu bytes)", total);

	buf = palloc0(total);
	item = (IndexTuple) buf;
	ItemPointerSetInvalid(&item->t_tid);
	item->t_info = (unsigned short) total;	/* size only; no null/varwidth */

	hdr = (BTPrefixDirHdr *) buf;
	hdr->ngroups = (uint16) ng;
	hdr->decodedsz = 0;
	hdr->maxdecoded = 0;
	hdr->natts = (uint16) natts;
	for (int i = 0; i < natts; i++)
		hdr->attrs[i] = attrs[i];
	offarr = _bt_prefix_anchoroff(hdr);
	for (int g = 0; g < ng; g++)
		offarr[g] = (uint16) anchoroff[g];

	for (int g = 0; g < ng; g++)
	{
		BTPrefixAnchorDisk *ad = (BTPrefixAnchorDisk *) (buf + anchoroff[g]);
		Size		repsz = _bt_prefix_keysz(dir->anchors[g].rep);
		IndexTuple	rep;

		ad->repsz = (uint16) repsz;
		/* store the key part only, as a plain non-posting tuple */
		rep = (IndexTuple) ((char *) ad + MAXALIGN(sizeof(BTPrefixAnchorDisk)));
		memcpy(rep, dir->anchors[g].rep, repsz);
		rep->t_info = (rep->t_info & ~(INDEX_SIZE_MASK | INDEX_ALT_TID_MASK)) |
			repsz;
	}

	pfree(anchoroff);
	*itemsz = total;
	return item;
}

/* The directory item of a BTP_PREFIX page, unchecked. */
static inline char *
_bt_prefix_diritem(Page page)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber off = BTPageGetPrefixOff(opaque);

	Assert(P_HASPREFIX(opaque));
	return (char *) PageGetItem(page, PageGetItemId(page, off));
}

/*
 * Key-column layout stored in a coded page's directory, for re-coding against
 * that page page-only (insert, recode).  The directory holds exactly the
 * layout the page was coded with, so a member re-coded with it decodes the
 * same way.
 */
static inline const BTPrefixAttr *
_bt_prefix_page_layout(char *diritem, int *natts)
{
	BTPrefixDirHdr *hdr = (BTPrefixDirHdr *) diritem;

	*natts = hdr->natts;
	return hdr->attrs;
}

/* Pointer to anchor g's on-disk blob within the directory item. */
static inline BTPrefixAnchorDisk *
_bt_prefix_anchor(char *diritem, int g)
{
	BTPrefixDirHdr *hdr = (BTPrefixDirHdr *) diritem;

	return (BTPrefixAnchorDisk *) (diritem + _bt_prefix_anchoroff(hdr)[g]);
}

/* The representative IndexTuple inside an on-disk anchor blob. */
static inline IndexTuple
_bt_prefix_anchor_rep(BTPrefixAnchorDisk *ad)
{
	return (IndexTuple) ((char *) ad + MAXALIGN(sizeof(BTPrefixAnchorDisk)));
}

/* Report a malformed physical representation without interpreting its data. */
pg_noreturn static void
_bt_prefix_corrupt(OffsetNumber offnum, const char *field)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg_internal("invalid prefix %s at offset %u", field, offnum)));
}

/* Establish page bounds before using either line pointers or the opaque area. */
static void
_bt_prefix_check_header(Page page)
{
	PageHeader	phdr = (PageHeader) page;
	BTPageOpaque opaque;

	if (phdr->pd_special != BLCKSZ - MAXALIGN(sizeof(BTPageOpaqueData)) ||
		phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		(phdr->pd_lower - SizeOfPageHeaderData) % sizeof(ItemIdData) != 0)
		_bt_prefix_corrupt(InvalidOffsetNumber, "page header");

	opaque = BTPageGetOpaque(page);
	if (!P_HASPREFIX(opaque) || !P_ISLEAF(opaque) || P_IGNORE(opaque) ||
		P_ISMETA(opaque) || opaque->btpo_level != 0 ||
		PageGetMaxOffsetNumber(page) < P_FIRSTDATAKEY(opaque))
		_bt_prefix_corrupt(InvalidOffsetNumber, "page layout");
}

/* The caller has checked the page header, but not the line pointer. */
static IndexTuple
_bt_prefix_checked_item(Page page, OffsetNumber offnum, Size minsize)
{
	ItemId		itemid;
	Size		offset;
	Size		len;
	IndexTuple	itup;

	if (offnum < FirstOffsetNumber || offnum > PageGetMaxOffsetNumber(page))
		_bt_prefix_corrupt(offnum, "item offset");
	itemid = PageGetItemId(page, offnum);
	offset = ItemIdGetOffset(itemid);
	len = ItemIdGetLength(itemid);
	if ((!ItemIdIsNormal(itemid) && !ItemIdIsDead(itemid)) ||
		offset != MAXALIGN(offset) ||
		offset < ((PageHeader) page)->pd_upper ||
		offset > ((PageHeader) page)->pd_special ||
		len > ((PageHeader) page)->pd_special - offset || len < minsize)
		_bt_prefix_corrupt(offnum, "line pointer");
	itup = (IndexTuple) PageGetItem(page, itemid);
	if (IndexTupleSize(itup) != len || len != MAXALIGN(len))
		_bt_prefix_corrupt(offnum, "tuple size");
	return itup;
}

/* Check the directory header and offset array, without visiting any anchor. */
static BTPrefixDirHdr *
_bt_prefix_checked_directory(Page page)
{
	OffsetNumber offnum = BTPageGetPrefixOff(BTPageGetOpaque(page));
	BTPrefixDirHdr *hdr = (BTPrefixDirHdr *)
		_bt_prefix_checked_item(page, offnum, offsetof(BTPrefixDirHdr, attrs));
	Size		len = IndexTupleSize(&hdr->header);

	if (hdr->header.t_info != len ||
		ItemPointerIsValid(&hdr->header.t_tid) ||
		ItemIdIsDead(PageGetItemId(page, offnum)) ||
		hdr->ngroups == 0 || hdr->ngroups > BTREE_PREFIX_MAXGROUPS ||
		hdr->natts > INDEX_MAX_KEYS ||
		_bt_prefix_diritem_size(hdr->natts, hdr->ngroups, 0) > len)
		_bt_prefix_corrupt(offnum, "directory header");
	return hdr;
}

/* Validate just the selected anchor; never follow an unchecked byte offset. */
static IndexTuple
_bt_prefix_checked_anchor(BTPrefixDirHdr *hdr, int group, OffsetNumber offnum)
{
	Size		len = IndexTupleSize(&hdr->header);
	Size		offset;
	BTPrefixAnchorDisk *ad;
	IndexTuple	rep;

	if (group >= hdr->ngroups)
		_bt_prefix_corrupt(offnum, "group number");
	offset = _bt_prefix_anchoroff(hdr)[group];
	if (offset != MAXALIGN(offset) ||
		offset < _bt_prefix_diritem_size(hdr->natts, hdr->ngroups, 0) ||
		offset > len ||
		MAXALIGN(sizeof(BTPrefixAnchorDisk)) + sizeof(IndexTupleData) > len - offset)
		_bt_prefix_corrupt(offnum, "anchor offset");
	ad = (BTPrefixAnchorDisk *) ((char *) hdr + offset);
	if (ad->repsz < sizeof(IndexTupleData) ||
		ad->repsz > len - offset - MAXALIGN(sizeof(BTPrefixAnchorDisk)))
		_bt_prefix_corrupt(offnum, "anchor size");
	rep = _bt_prefix_anchor_rep(ad);
	if (IndexTupleSize(rep) != ad->repsz ||
		ad->repsz != MAXALIGN(ad->repsz) ||
		(rep->t_info & INDEX_ALT_TID_MASK) != 0 ||
		ad->repsz < IndexInfoFindDataOffset(rep->t_info))
		_bt_prefix_corrupt(offnum, "anchor tuple header");
	return rep;
}

/* Check every source and destination extent used by the decoder. */
static Size
_bt_prefix_check_member(BTPrefixDirHdr *hdr, IndexTuple coded,
						OffsetNumber offnum, IndexTuple *repp,
						BTPrefixSplice *splice)
{
	BTPrefixCode *code = (BTPrefixCode *) ((char *) coded + sizeof(IndexTupleData));
	IndexTuple	rep = _bt_prefix_checked_anchor(hdr, code->group, offnum);
	Size		keysz = code->orig_info & INDEX_SIZE_MASK;
	Size		hoff = IndexInfoFindDataOffset(code->orig_info);
	Size		rephoff = IndexInfoFindDataOffset(rep->t_info);
	Size		repdatalen = IndexTupleSize(rep) - rephoff;
	Size		lead_shared = code->lead_shared;
	Size		shared_len = code->shared_len;
	Size		bodysz;
	Size		plainsz = keysz;
	Size		codesz = IndexTupleSize(coded);
	Size		mhdr;
	Size		ahdr;
	bool		ok_m,
				ok_a;
	const char *codebody = (const char *) code + sizeof(BTPrefixCode);
	Size		bitmaplen = hoff - sizeof(IndexTupleData);
	const uint8 *mbitmap = (code->orig_info & INDEX_NULL_MASK) != 0 ?
		(const uint8 *) codebody : NULL;
	const char *repdata = (const char *) rep + rephoff;
	const char *residp;
	Size		residual;

	if ((code->orig_info & INDEX_ALT_TID_MASK) != 0 ||
		keysz < hoff || keysz != MAXALIGN(keysz) ||
		(coded->t_info & (INDEX_NULL_MASK | INDEX_VAR_MASK)) != 0 ||
		BTreeTupleIsPivot(coded))
		_bt_prefix_corrupt(offnum, "member header");

	/*
	 * The shared span is the fully-shared leading columns (lead_shared, from
	 * the anchor's [0..D)) plus the divergent column's shared value bytes
	 * (shared_len).  Both must lie within the member's data area and the
	 * anchor must carry the shared leading bytes.
	 */
	if (lead_shared > keysz - hoff ||
		shared_len > keysz - hoff - lead_shared ||
		lead_shared > repdatalen)
		_bt_prefix_corrupt(offnum, "shared length");
	bodysz = MAXALIGN(sizeof(BTPrefixCode) + keysz - lead_shared - shared_len);
	if (bodysz > codesz)
		_bt_prefix_corrupt(offnum, "residual extent");
	residual = keysz - hoff - lead_shared - shared_len;
	residp = codebody + bitmaplen;

	/*
	 * The divergent column's header stays private: the member's own copy
	 * leads its residual, the anchor's is skipped so the shared value bytes
	 * splice after it.  Both prefixes (padding plus length header) are
	 * measured from the layout: the member's from its residual, the anchor's
	 * from its stored tuple, each walking the anchor's byte-identical [0..D)
	 * to reach the divergent column.  Bound every splice extent against the
	 * member residual and the anchor's data area before decode copies.
	 */
	mhdr = _bt_prefix_divprefix(hdr->attrs, hdr->natts, repdata, mbitmap,
								repdatalen, residp, residual, lead_shared,
								&ok_m);
	ahdr = _bt_prefix_divprefix(hdr->attrs, hdr->natts, repdata,
								IndexTupleHasNulls(rep) ?
								(const uint8 *) rep + sizeof(IndexTupleData) :
								NULL, repdatalen, repdata + lead_shared,
								repdatalen - lead_shared, lead_shared, &ok_a);
	if (!ok_m || !ok_a || mhdr > residual ||
		lead_shared + ahdr + shared_len > repdatalen)
		_bt_prefix_corrupt(offnum, "shared length");
	if (BTreeTupleIsPosting(coded))
	{
		Size		listsz = BTreeTupleGetNPosting(coded) * sizeof(ItemPointerData);

		if (BTreeTupleGetNPosting(coded) < 2 ||
			(ItemPointerGetOffsetNumberNoCheck(&coded->t_tid) &
			 BT_STATUS_OFFSET_MASK) != BT_IS_POSTING ||
			BTreeTupleGetPostingOffset(coded) != bodysz ||
			listsz > codesz - bodysz || MAXALIGN(bodysz + listsz) != codesz)
			_bt_prefix_corrupt(offnum, "posting list extent");
		plainsz = MAXALIGN(keysz + listsz);
	}
	else if (bodysz != codesz)
		_bt_prefix_corrupt(offnum, "residual extent");
	if (plainsz > INDEX_SIZE_MASK || plainsz > BLCKSZ)
		_bt_prefix_corrupt(offnum, "decoded tuple size");
	if (repp != NULL)
		*repp = rep;
	if (splice != NULL)
	{
		splice->lead_shared = lead_shared;
		splice->shared_len = shared_len;
		splice->mhdr = mhdr;
		splice->ahdr = ahdr;
		splice->residual = residual;
	}
	return plainsz;
}

/*
 * _bt_prefix_check
 *		Validate a whole BTP_PREFIX page; ERROR if it is malformed.
 *
 * For amcheck and pageinspect, before they read the page's tuples.  Unlike
 * decode, this also visits unused anchors and checks the cached decoded sizes
 * against the split-fit invariant (see _bt_prefix_split_needed).
 */
void
_bt_prefix_check(Page page)
{
	BTPageOpaque opaque;
	BTPrefixDirHdr *hdr;
	OffsetNumber diroff;
	Size		end;
	Size		total = 0;
	Size		maxitem = 0;
	Size		hikeysz = 0;
	Size		usable = BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BTPageOpaqueData));

	_bt_prefix_check_header(page);
	opaque = BTPageGetOpaque(page);
	diroff = BTPageGetPrefixOff(opaque);
	hdr = _bt_prefix_checked_directory(page);
	end = _bt_prefix_diritem_size(hdr->natts, hdr->ngroups, 0);
	for (int g = 0; g < hdr->ngroups; g++)
	{
		IndexTuple	rep = _bt_prefix_checked_anchor(hdr, g, diroff);

		if (_bt_prefix_anchoroff(hdr)[g] != end)
			_bt_prefix_corrupt(diroff, "anchor layout");
		end += MAXALIGN(sizeof(BTPrefixAnchorDisk)) + IndexTupleSize(rep);
	}
	if (end != IndexTupleSize(&hdr->header))
		_bt_prefix_corrupt(diroff, "directory size");
	for (OffsetNumber off = P_FIRSTDATAKEY(opaque);
		 off <= PageGetMaxOffsetNumber(page); off++)
	{
		IndexTuple	coded = _bt_prefix_checked_item(page, off,
													sizeof(IndexTupleData) + sizeof(BTPrefixCode));
		Size		sz = _bt_prefix_check_member(hdr, coded, off, NULL, NULL) + sizeof(ItemIdData);

		total += sz;
		maxitem = Max(maxitem, sz);
	}
	if (total != hdr->decodedsz || maxitem != hdr->maxdecoded)
		_bt_prefix_corrupt(diroff, "directory decoded size");
	if (!P_RIGHTMOST(opaque))
	{
		IndexTuple	hikey = _bt_prefix_checked_item(page, P_HIKEY, sizeof(IndexTupleData));

		hikeysz = IndexTupleSize(hikey) + sizeof(ItemIdData);
	}
	if (total + hikeysz + 2 * maxitem + MAXALIGN(sizeof(ItemPointerData)) > 2 * usable)
		_bt_prefix_corrupt(diroff, "decoded split budget");
}

/*
 * _bt_prefix_page_descriptor
 *		Copy coded page `page's stored key-column layout into `attrs'
 *		(INDEX_MAX_KEYS entries) and return its key count, or 0 if `page' is
 *		not a coded leaf.
 *
 * For WAL redo, which re-codes the left half of a split from the original
 * page: a coded original carries the layout the primary used, so redo matches
 * it page-only.  Validates the directory header before reading.
 */
int
_bt_prefix_page_descriptor(Page page, BTPrefixAttr * attrs)
{
	BTPrefixDirHdr *hdr;

	if (!P_HASPREFIX(BTPageGetOpaque(page)))
		return 0;
	_bt_prefix_check_header(page);
	hdr = _bt_prefix_checked_directory(page);
	for (int i = 0; i < hdr->natts; i++)
		attrs[i] = hdr->attrs[i];
	return hdr->natts;
}

/*
 * Code plain tuple `itup' against group `group', whose representative is
 * `rep'.  `attrs'/`natts' is the key-column layout used to decide where the
 * two diverge (see _bt_prefix_walk); NULL falls back to v15 whole-data
 * sharing.  Returns a palloc'd coded tuple, or NULL if its size does not fit
 * INDEX_SIZE_MASK.
 *
 * The member drops the fully-shared leading columns (D bytes) and the
 * divergent column's shared value prefix (sv bytes), keeping its own
 * divergent-column header.  Its stored residual is the data area from D with
 * the shared value bytes cut out of the middle: [divergent header][value
 * bytes after the shared ones][later columns].
 */
static IndexTuple
_bt_prefix_encode_against(const BTPrefixAttr * attrs, int natts,
						  IndexTuple rep, IndexTuple itup, int group)
{
	Size		hoff = IndexInfoFindDataOffset(itup->t_info);
	Size		keysz = _bt_prefix_keysz(itup);
	bool		posting = BTreeTupleIsPosting(itup);
	BTPrefixSplit split;
	Size		lead_shared;	/* D: fully-shared leading columns */
	Size		shared_len;		/* sv: divergent column's shared value bytes */
	Size		mhdr;			/* divergent column's prefix in itup */
	Size		residual;
	Size		bodysz;
	Size		codesz;
	char	   *buf;
	char	   *residp;
	IndexTuple	coded;
	BTPrefixCode *code;

	_bt_prefix_walk(attrs, natts, rep, itup, &split);
	lead_shared = split.lead_shared;
	shared_len = split.shared_len;
	mhdr = _bt_prefix_divhdr(attrs, natts, itup, lead_shared);

	residual = keysz - hoff - lead_shared - shared_len;
	bodysz = MAXALIGN(sizeof(IndexTupleData) + sizeof(BTPrefixCode) +
					  (hoff - sizeof(IndexTupleData)) + residual);
	codesz = bodysz;
	if (posting)
		codesz = MAXALIGN(bodysz + BTreeTupleGetNPosting(itup) *
						  sizeof(ItemPointerData));
	if (codesz > INDEX_SIZE_MASK)
		return NULL;

	buf = palloc0(codesz);
	coded = (IndexTuple) buf;
	coded->t_tid = itup->t_tid;
	coded->t_info = (unsigned short) codesz;

	code = (BTPrefixCode *) (buf + sizeof(IndexTupleData));
	code->group = (uint16) group;
	code->lead_shared = (uint16) lead_shared;
	code->shared_len = (uint16) shared_len;
	/* flags and key-part size of the original; posting-ness is the coded's */
	code->orig_info = (itup->t_info & ~(INDEX_SIZE_MASK | INDEX_ALT_TID_MASK)) |
		keysz;

	/* copy the original's null bitmap (bytes between header and data area) */
	memcpy(buf + sizeof(IndexTupleData) + sizeof(BTPrefixCode),
		   (char *) itup + sizeof(IndexTupleData),
		   hoff - sizeof(IndexTupleData));

	/*
	 * Copy the residual.  The fully-shared leading D bytes come from the
	 * anchor, so they are not stored.  The member's own divergent header
	 * (mhdr bytes at D) stays, the next shared_len value bytes are dropped,
	 * and the rest of the data area follows.
	 */
	residp = buf + sizeof(IndexTupleData) + sizeof(BTPrefixCode) +
		(hoff - sizeof(IndexTupleData));
	memcpy(residp, (char *) itup + hoff + lead_shared, mhdr);
	memcpy(residp + mhdr,
		   (char *) itup + hoff + lead_shared + mhdr + shared_len,
		   residual - mhdr);

	/*
	 * A posting list stays uncoded, in the standard posting tuple layout, so
	 * code that only reads heap TIDs (VACUUM, LP_DEAD hinting, deletion,
	 * posting list splits) works on the coded tuple as is.
	 */
	if (posting)
	{
		int			nhtids = BTreeTupleGetNPosting(itup);

		BTreeTupleSetPosting(coded, nhtids, bodysz);
		memcpy(BTreeTupleGetPosting(coded), BTreeTupleGetPosting(itup),
			   nhtids * sizeof(ItemPointerData));
	}

	return coded;
}

/*
 * _bt_prefix_decode
 *		Decode the data item at `offnum' on BTP_PREFIX page `page' into `buf'.
 *
 * Returns a pointer into `buf'.  Every extent the copy reads or writes is
 * validated first, so a corrupt page raises ERROR rather than reading past
 * the page or overrunning `buf'.
 */
IndexTuple
_bt_prefix_decode(Page page, OffsetNumber offnum, BTItemBuf *buf)
{
	IndexTuple	coded;
	BTPrefixDirHdr *hdr;
	BTPrefixCode *code;
	IndexTuple	rep;
	BTPrefixSplice splice;
	Size		rephoff;
	Size		hoff;
	Size		bitmaplen;
	char	   *codebody;
	char	   *residp;
	char	   *out = buf->data;
	IndexTuple	plain = (IndexTuple) out;
	Size		keysz;

	_bt_prefix_check_header(page);
	hdr = _bt_prefix_checked_directory(page);
	coded = _bt_prefix_checked_item(page, offnum,
									sizeof(IndexTupleData) + sizeof(BTPrefixCode));
	_bt_prefix_check_member(hdr, coded, offnum, &rep, &splice);
	code = (BTPrefixCode *) ((char *) coded + sizeof(IndexTupleData));
	rephoff = IndexInfoFindDataOffset(rep->t_info);
	hoff = IndexInfoFindDataOffset(code->orig_info);
	bitmaplen = hoff - sizeof(IndexTupleData);
	codebody = (char *) code + sizeof(BTPrefixCode);
	residp = codebody + bitmaplen;

	/* key-part size from orig_info, not the coded size (padding, posting) */
	keysz = (code->orig_info & INDEX_SIZE_MASK);

	/*
	 * Reconstruct the data area from the offsets _bt_prefix_check_member
	 * validated.  [0..D) is the fully-shared leading columns, spliced whole
	 * from the anchor.  The divergent column's header stays private: the
	 * member's own prefix (mhdr) leads its residual, the anchor's (ahdr) is
	 * skipped so the shared value bytes come after it, then the member's
	 * remaining residual.  A single varlena column makes D 0, reproducing the
	 * v16 layout exactly.
	 */
	plain->t_tid = coded->t_tid;
	plain->t_info = code->orig_info;
	memcpy(out + sizeof(IndexTupleData), codebody, bitmaplen);
	/* fully-shared leading columns from the anchor */
	memcpy(out + hoff, (char *) rep + rephoff, splice.lead_shared);
	/* member's own divergent-column prefix (alignment and length header) */
	memcpy(out + hoff + splice.lead_shared, residp, splice.mhdr);
	/* shared value bytes from the anchor, after its divergent-column prefix */
	memcpy(out + hoff + splice.lead_shared + splice.mhdr,
		   (char *) rep + rephoff + splice.lead_shared + splice.ahdr,
		   splice.shared_len);
	/* the member's remaining residual value bytes */
	memcpy(out + hoff + splice.lead_shared + splice.mhdr + splice.shared_len,
		   residp + splice.mhdr, splice.residual - splice.mhdr);

	if (BTreeTupleIsPosting(coded))
	{
		int			nhtids = BTreeTupleGetNPosting(coded);
		Size		listsz = nhtids * sizeof(ItemPointerData);
		Size		plainsz = MAXALIGN(keysz + listsz);

		Assert(plainsz <= BLCKSZ);
		plain->t_info = (code->orig_info & ~INDEX_SIZE_MASK) | plainsz;
		BTreeTupleSetPosting(plain, nhtids, keysz);
		memcpy(out + keysz, BTreeTupleGetPosting(coded), listsz);
		memset(out + keysz + listsz, 0, plainsz - keysz - listsz);
	}

	return plain;
}

/*
 * _bt_prefix_form_page
 *		Lay out a BTP_PREFIX leaf from `hikey' and items[0..n-1].
 *
 * `newpage' must be _bt_pageinit'd with its opaque area set; `hikey' is NULL
 * on a rightmost page, and the items are plain and in key order.  Returns
 * false, leaving `newpage' unspecified, if coding does not pay off (unless
 * `force'), the directory or a coded member exceeds INDEX_SIZE_MASK, or the
 * coded items do not fit one page.  The caller sets LSN and prune XID.
 */
bool
_bt_prefix_form_page(Page newpage, IndexTuple hikey, IndexTuple *items, int n,
					 const BTPrefixAttr * attrs, int natts, bool force)
{
	BTPrefixPageEst est;
	BTPrefixDir *dir;
	IndexTuple	diritem;
	Size		diritemsz;
	OffsetNumber newoff;
	bool		ok = true;

	if (n <= 0)
		return false;

	/* Is it worth it?  The estimate is exact: same grouping, same sizes. */
	memset(&est, 0, sizeof(est));
	for (int i = 0; i < n; i++)
		_bt_prefix_est_add(&est, attrs, natts, items[i]);
	if (!_bt_prefix_est_representable(&est) ||
		(!force && !_bt_prefix_worthwhile(_bt_prefix_est_coded(&est),
										  est.plainsz)))
		return false;

	dir = _bt_prefix_choose_groups(attrs, natts, items, n);
	if (dir == NULL)
	{
		Assert(!force || n == 1);
		return false;			/* nothing shared */
	}

	if (hikey &&
		PageAddItem(newpage, hikey, MAXALIGN(IndexTupleSize(hikey)), P_HIKEY,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "could not add high key during prefix compression");

	diritem = _bt_prefix_form_diritem(dir, attrs, natts, &diritemsz);
	if (PageAddItem(newpage, diritem, diritemsz,
					BTPageGetPrefixOff(BTPageGetOpaque(newpage)),
					false, false) == InvalidOffsetNumber)
		ok = false;
	newoff = OffsetNumberNext(BTPageGetPrefixOff(BTPageGetOpaque(newpage)));

	for (int i = 0, g = 0; ok && i < n; i++)
	{
		IndexTuple	coded;

		while (g + 1 < dir->ngroups && items[i] == dir->anchors[g + 1].rep)
			g++;

		coded = _bt_prefix_encode_against(attrs, natts, dir->anchors[g].rep,
										  items[i], g);
		Assert(coded != NULL);
		if (PageAddItem(newpage, coded, IndexTupleSize(coded), newoff,
						false, false) == InvalidOffsetNumber)
			ok = false;			/* the coded items do not fit one page */
		newoff = OffsetNumberNext(newoff);
		pfree(coded);
	}

	pfree(diritem);
	pfree(dir);
	if (!ok)
		return false;

	BTPageGetOpaque(newpage)->btpo_flags |= BTP_PREFIX;
	_bt_prefix_recount(newpage);

#ifdef USE_ASSERT_CHECKING
	/* decode must reproduce every plain tuple byte for byte */
	for (int i = 0; i < n; i++)
	{
		BTItemBuf	chk;
		IndexTuple	d = _bt_getitem(newpage,
									P_FIRSTDATAKEY(BTPageGetOpaque(newpage)) + i,
									&chk);

		Assert(IndexTupleSize(d) == IndexTupleSize(items[i]) &&
			   memcmp(d, items[i], IndexTupleSize(d)) == 0);
	}
#endif

	return true;
}

/*
 * _bt_prefix_compress_page
 *		Rewrite plain leaf `page' in BTP_PREFIX form, in place, if that pays
 *		off; otherwise leave it unchanged.
 */
void
_bt_prefix_compress_page(Relation index, Page page)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber minoff,
				maxoff;
	int			n;
	IndexTuple *items;
	IndexTuple	hikey = NULL;
	Page		newpage;

	if (!_bt_prefix_enabled(index) || !P_ISLEAF(opaque) || P_HASPREFIX(opaque))
		return;

	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	if (maxoff < minoff)
		return;					/* empty page */
	n = maxoff - minoff + 1;

	if (!P_RIGHTMOST(opaque))
		hikey = (IndexTuple) PageGetItem(page, PageGetItemId(page, P_HIKEY));

	items = palloc(sizeof(IndexTuple) * n);
	for (int i = 0; i < n; i++)
		items[i] = (IndexTuple) PageGetItem(page,
											PageGetItemId(page, minoff + i));

	/* Heap-allocated: this runs inside _bt_split, already two pages deep. */
	newpage = (Page) palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);
	_bt_pageinit(newpage, BLCKSZ);
	memcpy(BTPageGetOpaque(newpage), opaque, sizeof(BTPageOpaqueData));
	{
		BTPrefixAttr attrs[INDEX_MAX_KEYS];
		int			natts = _bt_prefix_build_layout(index, attrs);

		if (_bt_prefix_form_page(newpage, hikey, items, n, attrs, natts, false))
		{
			PageSetLSN(newpage, PageGetLSN(page));
			((PageHeader) newpage)->pd_prune_xid =
				((PageHeader) page)->pd_prune_xid;
			memcpy(page, newpage, BLCKSZ);
		}
	}
	pfree(newpage);
	pfree(items);
}

/*
 * _bt_prefix_encode_insert
 *		Code plain tuple `itup' for insertion at `newitemoff' on BTP_PREFIX
 *		page `page'.
 *
 * The tuple joins the group of the item it displaces (or of the last item,
 * when appended), sharing however much it happens to share, possibly nothing.
 * The result is at most BTREE_PREFIX_CODE_OVERHEAD larger than `itup'.
 * Returns a palloc'd coded tuple, or NULL when it exceeds INDEX_SIZE_MASK and
 * the page must split.  NB: pallocs, so call it before the critical section.
 */
IndexTuple
_bt_prefix_encode_insert(Page page, OffsetNumber newitemoff, IndexTuple itup)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	char	   *diritem = _bt_prefix_diritem(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber neighbor;
	IndexTuple	ncoded;
	BTPrefixCode *ncode;
	BTPrefixAnchorDisk *ad;
	IndexTuple	rep;
	int			group;
	int			natts;
	const		BTPrefixAttr *attrs = _bt_prefix_page_layout(diritem, &natts);

	Assert(P_HASPREFIX(opaque));

	if (newitemoff <= maxoff && newitemoff >= P_FIRSTDATAKEY(opaque))
		neighbor = newitemoff;
	else if (maxoff >= P_FIRSTDATAKEY(opaque))
		neighbor = maxoff;
	else
	{
		/* no data items: use group 0 */
		ad = _bt_prefix_anchor(diritem, 0);
		return _bt_prefix_encode_against(attrs, natts,
										 _bt_prefix_anchor_rep(ad), itup, 0);
	}

	ncoded = (IndexTuple) PageGetItem(page, PageGetItemId(page, neighbor));
	ncode = (BTPrefixCode *) ((char *) ncoded + sizeof(IndexTupleData));
	group = ncode->group;
	ad = _bt_prefix_anchor(diritem, group);
	rep = _bt_prefix_anchor_rep(ad);

	return _bt_prefix_encode_against(attrs, natts, rep, itup, group);
}

/*
 * _bt_prefix_recode
 *		Code plain tuple `itup' against the group of the item at `offnum' on
 *		BTP_PREFIX page `page', returning a palloc'd coded tuple.
 *
 * For a tuple with the same key as that item (a split or merged posting
 * list), which therefore codes the same way.  NB: pallocs, so call it before
 * the critical section.
 */
IndexTuple
_bt_prefix_recode(Page page, OffsetNumber offnum, IndexTuple itup)
{
	char	   *diritem = _bt_prefix_diritem(page);
	IndexTuple	ncoded = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	BTPrefixCode *ncode = (BTPrefixCode *) ((char *) ncoded +
											sizeof(IndexTupleData));
	BTPrefixAnchorDisk *ad = _bt_prefix_anchor(diritem, ncode->group);
	IndexTuple	rep = _bt_prefix_anchor_rep(ad);
	int			natts;
	const		BTPrefixAttr *attrs = _bt_prefix_page_layout(diritem, &natts);
	IndexTuple	coded;

	coded = _bt_prefix_encode_against(attrs, natts, rep, itup, ncode->group);
	if (coded == NULL)
		elog(ERROR, "coded nbtree posting tuple too large");
	return coded;
}

/*
 * _bt_prefix_decode_copy
 *		Return a palloc'd decoded copy of the item at `offnum' on BTP_PREFIX
 *		page `page'.
 *
 * The BLCKSZ decode buffer lives in this frame, so hot-path callers need not
 * reserve one on their own stack for a case that arises only on coded pages.
 */
IndexTuple
_bt_prefix_decode_copy(Page page, OffsetNumber offnum)
{
	BTItemBuf	decbuf;

	return CopyIndexTuple(_bt_prefix_decode(page, offnum, &decbuf));
}

/*
 * _bt_prefix_decode_items
 *		Decode every data item of BTP_PREFIX page `page'.
 *
 * Sets *items_out to a palloc'd array of palloc'd plain tuples and returns
 * its length.  *hikey gets a palloc'd copy of the high key and *hikeysz its
 * line pointer length (NULL and 0 on a rightmost page).  The caller frees all
 * of it.
 */
int
_bt_prefix_decode_items(Page page, IndexTuple **items_out,
						IndexTuple *hikey, Size *hikeysz)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber minoff = P_FIRSTDATAKEY(opaque);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	int			n = (maxoff >= minoff) ? (maxoff - minoff + 1) : 0;
	IndexTuple *items = n ? palloc(sizeof(IndexTuple) * n) : NULL;
	BTItemBuf	decbuf;
	int			k = 0;

	if (!P_RIGHTMOST(opaque))
	{
		ItemId		hii = PageGetItemId(page, P_HIKEY);

		*hikey = CopyIndexTuple((IndexTuple) PageGetItem(page, hii));
		*hikeysz = ItemIdGetLength(hii);
	}
	else
	{
		*hikey = NULL;
		*hikeysz = 0;
	}

	for (OffsetNumber off = minoff; off <= maxoff; off = OffsetNumberNext(off))
	{
		items[k++] = CopyIndexTuple(_bt_prefix_decode(page, off, &decbuf));
	}
	*items_out = items;
	return n;
}

/*
 * _bt_prefix_try_plain_page
 *		Write a plain copy of BTP_PREFIX page `page' into BLCKSZ buffer `out'.
 *
 * Returns `out', or NULL if the decoded content does not fit one page, in
 * which case `out' is left partly written.
 */
Page
_bt_prefix_try_plain_page(Relation rel, Page page, Page out)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber minoff = P_FIRSTDATAKEY(opaque);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	OffsetNumber newoff;
	BTItemBuf	decbuf;

	_bt_pageinit(out, BLCKSZ);
	memcpy(BTPageGetOpaque(out), opaque, sizeof(BTPageOpaqueData));
	BTPageGetOpaque(out)->btpo_flags &= ~BTP_PREFIX;

	if (!P_RIGHTMOST(opaque))
	{
		ItemId		hii = PageGetItemId(page, P_HIKEY);

		if (PageAddItem(out, PageGetItem(page, hii), ItemIdGetLength(hii),
						P_HIKEY, false, false) == InvalidOffsetNumber)
			return NULL;
	}

	newoff = P_FIRSTDATAKEY(BTPageGetOpaque(out));
	for (OffsetNumber off = minoff; off <= maxoff; off = OffsetNumberNext(off))
	{
		IndexTuple	pt = _bt_prefix_decode(page, off, &decbuf);

		if (PageAddItem(out, pt, IndexTupleSize(pt), newoff, false, false) ==
			InvalidOffsetNumber)
			return NULL;		/* decoded content exceeds a page */
		newoff = OffsetNumberNext(newoff);
	}

	return out;
}

/*
 * MAXALIGN'd size of coded member `coded' once decoded, from its header alone.
 *
 * NB: the split-fit invariant (see _bt_prefix_split_needed) needs this to be
 * the same unit _bt_prefix_findsplitloc and the CREATE INDEX estimate use,
 * MAXALIGN(IndexTupleSize()) of the plain tuple.
 */
static inline Size
_bt_prefix_plain_size(IndexTuple coded)
{
	BTPrefixCode *code = (BTPrefixCode *) ((char *) coded +
										   sizeof(IndexTupleData));
	Size		keysz = code->orig_info & INDEX_SIZE_MASK;

	if (BTreeTupleIsPosting(coded))
		return MAXALIGN(keysz + BTreeTupleGetNPosting(coded) *
						sizeof(ItemPointerData));
	return MAXALIGN(keysz);
}

/*
 * _bt_prefix_split_needed
 *		Must inserting a plain tuple of (MAXALIGN'd) size `newitemsz' into
 *		BTP_PREFIX page `page' split it first?
 *
 * The split-fit guarantee.  A coded page may hold more decoded content than a
 * plain page, but its decoded items must always split into two plain pages.
 * Each coded page keeps
 *
 *		T + hikey + 2 * m + heap TID <= 2 * usable		(*)
 *
 * where T is the decoded size of its data items (including line pointers) and
 * m the largest.  Under (*) a split of the page plus a new item x always has a
 * legal point when x was admitted by this test, because admitting x requires
 * (*) to hold with x counted in T and m.  Proof: take the leftmost point whose
 * right half (items plus the old high key) fits.  The point before it did
 * not, so the left items before it total less than T - (usable - hikey);
 * adding one more item and the new high key (a truncated copy of firstright
 * plus a heap TID) gives a left half under T - usable + hikey + 2m + heap TID,
 * at most usable by (*).
 *
 * So insert splits a page before it would break (*).  _bt_split's split then
 * places the new item too: (*) with the new item counted is exactly what
 * failed, but the page without it satisfied (*), and a page that a plain
 * split would handle (decoded content at most one page) always has a legal
 * point by the usual one-third-of-a-page item size rule; the remaining case
 * (content between one and two pages, new item large) is handled by
 * _bt_prefix_findsplitloc trying the split without the new item first.
 */
bool
_bt_prefix_split_needed(Page page, Size newitemsz)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	BTPrefixDirHdr *hdr = (BTPrefixDirHdr *) _bt_prefix_diritem(page);
	Size		usable = BLCKSZ - SizeOfPageHeaderData -
		MAXALIGN(sizeof(BTPageOpaqueData));
	Size		newsz = MAXALIGN(newitemsz) + sizeof(ItemIdData);
	Size		total = hdr->decodedsz + newsz;
	Size		maxitem = Max((Size) hdr->maxdecoded, newsz);
	Size		hikeysz = 0;

	if (!P_RIGHTMOST(opaque))
		hikeysz = MAXALIGN(ItemIdGetLength(PageGetItemId(page, P_HIKEY))) +
			sizeof(ItemIdData);

	return total + hikeysz + 2 * maxitem + MAXALIGN(sizeof(ItemPointerData)) >
		2 * usable;
}

/*
 * _bt_prefix_recount
 *		Recompute the directory's cached decoded sizes after a change to the
 *		page's data items (deletion, posting list update, a rebuilt page), or
 *		drop the directory if no data items remain.  Page-only, so WAL redo
 *		calls it after replaying the same change and reproduces the page.
 */
void
_bt_prefix_recount(Page page)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber minoff,
				maxoff;
	BTPrefixDirHdr *hdr;
	Size		total = 0,
				maxitem = 0;

	if (!P_HASPREFIX(opaque))
		return;

	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	if (maxoff < minoff)
	{
		/*
		 * An emptied leaf holds only its high key (page deletion relies on
		 * that): drop the directory and become a plain page.
		 */
		PageIndexTupleDelete(page, BTPageGetPrefixOff(opaque));
		opaque->btpo_flags &= ~BTP_PREFIX;
		return;
	}

	for (OffsetNumber off = minoff; off <= maxoff; off = OffsetNumberNext(off))
	{
		IndexTuple	coded = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		Size		sz = _bt_prefix_plain_size(coded) + sizeof(ItemIdData);

		total += sz;
		maxitem = Max(maxitem, sz);
	}
	Assert(total <= PG_UINT16_MAX);
	hdr = (BTPrefixDirHdr *) _bt_prefix_diritem(page);
	hdr->decodedsz = (uint16) total;
	hdr->maxdecoded = (uint16) maxitem;
}

/*
 * _bt_prefix_note_insert
 *		Account for coded item `coded' just added to BTP_PREFIX page `page'.
 *		Page-only; insert redo calls it too.
 */
void
_bt_prefix_note_insert(Page page, IndexTuple coded)
{
	BTPrefixDirHdr *hdr = (BTPrefixDirHdr *) _bt_prefix_diritem(page);
	Size		sz = _bt_prefix_plain_size(coded) + sizeof(ItemIdData);

	Assert(hdr->decodedsz + sz <= PG_UINT16_MAX);
	hdr->decodedsz += (uint16) sz;
	hdr->maxdecoded = (uint16) Max((Size) hdr->maxdecoded, sz);
}

/*
 * _bt_prefix_findsplitloc
 *		Choose a split point for coded leaf `page' from its decoded items.
 *
 * ditems[] holds one plain tuple per data item.  Returns firstrightoff and
 * sets *newitemonleft, as _bt_findsplitloc does.  With newitemoff valid the
 * new item, of plain size newitemsz, is placed there, and the result is
 * InvalidOffsetNumber if no point keeps both halves within a page.  With
 * newitemoff invalid only the existing items are split, and the split-fit
 * invariant (see _bt_prefix_split_needed) guarantees a legal point.
 *
 * Each candidate is checked against the real sizes of both plain halves: the
 * left half's new high key is charged as firstright plus a heap TID, as
 * _bt_findsplitloc does, and the right half keeps the old high key.  The most
 * even legal point wins, except on a rightmost page, where points are ranked
 * by leaf fillfactor as _bt_findsplitloc ranks them, so ascending inserts
 * leave fillfactor-full left pages behind.
 */
OffsetNumber
_bt_prefix_findsplitloc(Relation rel, Page page, IndexTuple *ditems,
						OffsetNumber newitemoff, Size newitemsz,
						bool *newitemonleft)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber mn = P_FIRSTDATAKEY(opaque);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);
	bool		hasnew = OffsetNumberIsValid(newitemoff);
	int			n = maxoff - mn + 1;	/* existing items */
	int			nall = n + (hasnew ? 1 : 0);
	int			newpos = hasnew ? newitemoff - mn : -1;
	Size		usable = BLCKSZ - SizeOfPageHeaderData -
		MAXALIGN(sizeof(BTPageOpaqueData));
	Size		hikeysz = 0;
	Size	   *sz = palloc(sizeof(Size) * nall);
	Size		total = 0,
				acc = 0;
	int			best = -1;
	Size		bestdelta = SIZE_MAX;

	if (!P_RIGHTMOST(opaque))
		hikeysz = MAXALIGN(ItemIdGetLength(PageGetItemId(page, P_HIKEY))) +
			sizeof(ItemIdData);

	/* sizes of the combined sequence, new item spliced in at newpos */
	for (int k = 0, i = 0; k < nall; k++)
	{
		if (k == newpos)
			sz[k] = MAXALIGN(newitemsz) + sizeof(ItemIdData);
		else
			sz[k] = MAXALIGN(IndexTupleSize(ditems[i++])) + sizeof(ItemIdData);
		total += sz[k];
	}

	/* split before position k: left = [0,k), right = [k,nall) */
	for (int k = 1; k < nall; k++)
	{
		Size		left,
					right;

		acc += sz[k - 1];
		left = acc + sz[k] + MAXALIGN(sizeof(ItemPointerData));
		right = (total - acc) + hikeysz;
		if (left <= usable && right <= usable)
		{
			Size		delta = left > right ? left - right : right - left;

			if (P_RIGHTMOST(opaque))
			{
				double		fillfactormult = BTGetFillFactor(rel) / 100.0;
				int16		weighteddelta;

				weighteddelta = fillfactormult * (usable - left) -
					(1.0 - fillfactormult) * (usable - right);
				delta = abs(weighteddelta);
			}

			if (delta < bestdelta)
			{
				bestdelta = delta;
				best = k;
			}
		}
	}
	pfree(sz);

	/*
	 * With a new item there may be no legal point (the item is large and the
	 * page dense); the caller then splits the existing items alone.
	 */
	if (best < 0)
	{
		if (hasnew)
			return InvalidOffsetNumber;
		elog(ERROR, "could not find a legal split point for prefix-compressed page in index \"%s\"",
			 RelationGetRelationName(rel));
	}

	/* map combined position back to (firstrightoff, newitemonleft) */
	if (!hasnew || best <= newpos)
	{
		*newitemonleft = false;
		return mn + best;
	}
	*newitemonleft = true;
	return mn + best - 1;
}
