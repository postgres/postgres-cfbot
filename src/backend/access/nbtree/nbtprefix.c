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

/*
 * In-memory description of one group's anchor: a representative plain tuple
 * (the group's first member) whose leading data-area bytes the members share.
 */
typedef struct BTPrefixAnchor
{
	IndexTuple	rep;			/* representative tuple owning the shared
								 * bytes */
} BTPrefixAnchor;

/*
 * In-memory prefix directory for a page under construction (build, split,
 * dedup).  Written to the on-disk directory item by _bt_prefix_form_directory.
 */
typedef struct BTPrefixDir
{
	int			ngroups;
	BTPrefixAnchor anchors[FLEXIBLE_ARRAY_MEMBER];
} BTPrefixDir;

/*
 * On-disk directory item header.  decodedsz and maxdecoded cache the decoded
 * (plain) size of the page's data items, each counted with its line pointer:
 * their sum and the largest.  They make _bt_prefix_split_needed O(1).  Every
 * change to the data items keeps them exact, through page-only routines that
 * WAL redo repeats (_bt_prefix_note_insert, _bt_prefix_recount).
 */
typedef struct BTPrefixDirHdr
{
	IndexTupleData header;		/* t_tid invalid; t_info = whole item size */
	uint16		ngroups;
	uint16		decodedsz;		/* sum of decoded item sizes + line pointers */
	uint16		maxdecoded;		/* largest of those */
	uint16		anchoroff[FLEXIBLE_ARRAY_MEMBER];	/* from item start */
	/* then the packed anchors */
} BTPrefixDirHdr;

/* One packed anchor in the directory item. */
typedef struct BTPrefixAnchorDisk
{
	uint16		repsz;			/* size of the representative tuple blob */
	/* IndexTupleData rep[repsz] follows, MAXALIGN'd */
} BTPrefixAnchorDisk;

/*
 * A coded member's on-disk body (follows the IndexTupleData header, whose
 * t_tid is the member's own heap TID, or posting list metadata):
 *
 *     BTPrefixCode { group, shared_len, orig_info }
 *     null bitmap of the original tuple (present iff orig_info says so)
 *     residual: the original key part's data-area bytes from shared_len on
 *     posting list, if any, in the standard posting tuple layout
 *
 * Decode is pure byte-splicing and needs no catalog (so WAL redo can do it):
 *
 *     plain = [IndexTupleData with orig_info][null bitmap]
 *             [rep data bytes 0 .. shared_len)][residual][posting list]
 *
 * shared_len is the length of the common prefix of the two data areas, so the
 * bytes taken from rep are bitwise-identical to the original's, and decode
 * reproduces the original tuple byte for byte.
 */
typedef struct BTPrefixCode
{
	uint16		group;			/* anchor index in the directory */
	uint16		shared_len;		/* leading data-area bytes taken from rep */
	uint16		orig_info;		/* original tuple's t_info (size, flags) */
} BTPrefixCode;

/*
 * A coded member is never larger than its plain form by more than
 * BTREE_PREFIX_CODE_OVERHEAD, which insert reserves before coding (see
 * _bt_findinsertloc).  The worst case is a posting tuple, whose body and
 * posting list are each MAXALIGN'd once: MAXALIGN(code header + key) +
 * MAXALIGN(posting list) can exceed the plain MAXALIGN(key + posting list) by
 * at most the code header plus one alignment of the posting list.  Keep the
 * constant tied to the struct sizes so it cannot silently fall short if
 * BTPrefixCode ever grows.
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
 * Size of a directory item for `ngroups' groups whose MAXALIGN'd anchor blobs
 * total `anchorsz' bytes.
 */
static inline Size
_bt_prefix_diritem_size(int ngroups, Size anchorsz)
{
	return MAXALIGN(offsetof(BTPrefixDirHdr, anchoroff) +
					sizeof(uint16) * ngroups) + anchorsz;
}

/*
 * _bt_prefix_shared_len
 *		How many leading data-area bytes plain tuple `itup' can take from group
 *		representative `rep': the length of the common prefix of their key
 *		parts' data areas.  Sharing is by stored bytes, never by the opclass
 *		notion of equality, so it is sound for any type (an index-only scan
 *		gets back exactly the bytes that were stored) and needs no catalog.
 */
static Size
_bt_prefix_shared_len(IndexTuple rep, IndexTuple itup)
{
	Size		rephoff = IndexInfoFindDataOffset(rep->t_info);
	Size		hoff = IndexInfoFindDataOffset(itup->t_info);
	const char *rp = (const char *) rep + rephoff;
	const char *mp = (const char *) itup + hoff;
	Size		limit = Min(_bt_prefix_keysz(rep) - rephoff,
							_bt_prefix_keysz(itup) - hoff);
	Size		n = 0;

	while (n < limit && rp[n] == mp[n])
		n++;
	return n;
}

/*
 * Size of `itup' coded against representative `rep' (what
 * _bt_prefix_encode_against would produce), without forming it.
 */
static Size
_bt_prefix_coded_size(IndexTuple rep, IndexTuple itup, Size shared_len)
{
	Size		hoff = IndexInfoFindDataOffset(itup->t_info);
	Size		sz;

	sz = MAXALIGN(sizeof(IndexTupleData) + sizeof(BTPrefixCode) +
				  (hoff - sizeof(IndexTupleData)) +
				  (_bt_prefix_keysz(itup) - hoff - shared_len));
	if (BTreeTupleIsPosting(itup))
		sz = MAXALIGN(sz + BTreeTupleGetNPosting(itup) * sizeof(ItemPointerData));
	return sz;
}

/*
 * _bt_prefix_group_start
 *		The single rule that decides where prefix groups begin, shared by the
 *		estimate (_bt_prefix_est_add) and the layout
 *		(_bt_prefix_choose_groups) so the two can never disagree.
 *
 *		Given the current group's representative `rep', the immediately
 *		preceding tuple `prev' (NULL for the very first tuple on the page),
 *		and the tuple `itup' being placed, return true when `itup' should
 *		start a new group led by `itup' itself.  On return *shared is how many
 *		leading data-area bytes `itup' shares with the representative it ends
 *		up under (itself when it starts a group).
 *
 *		A new group starts when `itup' shares nothing with `rep' (the original
 *		rule), or when it shares strictly more with `prev' than with `rep' by
 *		at least one MAXALIGN quantum.  The second case catches a page whose
 *		first tuple has a short accidental prefix (e.g. just the leading
 *		fixed-width columns) with a long run of tuples that share much more
 *		among themselves: without it the whole run is locked to the first
 *		tuple's short prefix.  The rule looks only backwards and the new group
 *		is always led by `itup', so the estimate stays exact tuple by tuple
 *		(no earlier tuple is ever re-grouped).
 */
static bool
_bt_prefix_group_start(IndexTuple rep, IndexTuple prev, IndexTuple itup,
					   Size *shared)
{
	Size		s_rep;

	if (rep == NULL)
	{
		*shared = _bt_prefix_shared_len(itup, itup);
		return true;
	}

	s_rep = _bt_prefix_shared_len(rep, itup);
	if (s_rep == 0)
	{
		*shared = _bt_prefix_shared_len(itup, itup);
		return true;
	}

	if (prev != NULL && prev != rep)
	{
		Size		s_prev = _bt_prefix_shared_len(prev, itup);

		if (s_prev >= s_rep + MAXIMUM_ALIGNOF)
		{
			*shared = _bt_prefix_shared_len(itup, itup);
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
_bt_prefix_est_add(BTPrefixPageEst *est, IndexTuple itup)
{
	Size		plain = MAXALIGN(IndexTupleSize(itup)) + sizeof(ItemIdData);
	Size		shared;

	if (_bt_prefix_group_start(est->grouprep, est->prev, itup, &shared))
	{
		est->grouprep = itup;
		est->ngroups++;
		est->anchorsz += MAXALIGN(MAXALIGN(sizeof(BTPrefixAnchorDisk)) +
								  _bt_prefix_keysz(itup));
	}
	est->lastcoded = _bt_prefix_coded_size(est->grouprep, itup, shared) +
		sizeof(ItemIdData);
	est->codedsz += est->lastcoded;
	est->plainsz += plain;
	est->maxplain = Max(est->maxplain, plain);
	est->nitems++;
	est->prev = itup;
}

/* Bytes (items plus line pointers) the coded page would use */
Size
_bt_prefix_est_coded(const BTPrefixPageEst *est)
{
	return _bt_prefix_diritem_size(est->ngroups, est->anchorsz) +
		sizeof(ItemIdData) + est->codedsz;
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
 * _bt_prefix_choose_groups
 *		Given `n' plain data tuples in key order (items[0..n-1]), decide the
 *		prefix groups using the same rule as the estimate
 *		(_bt_prefix_group_start): a group's representative is its first tuple,
 *		and a new group starts whenever _bt_prefix_group_start says so.
 *		Returns a palloc'd BTPrefixDir (caller frees), or NULL when nothing at
 *		all is shared.  Uses only the tuples' bytes, so it is deterministic
 *		and catalog-free.
 */
static BTPrefixDir *
_bt_prefix_choose_groups(IndexTuple *items, int n)
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

		if (_bt_prefix_group_start(rep, prev, items[i], &shared))
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
 * _bt_prefix_form_diritem
 *		Serialize an in-memory BTPrefixDir into the on-disk directory item:
 *			IndexTupleData header (t_tid zero, t_info = total size)
 *			uint16 ngroups
 *			uint16 anchoroff[ngroups]	 (byte offsets from item start)
 *			anchor blobs, each MAXALIGN'd:
 *				BTPrefixAnchorDisk { repsz }
 *				(padded to MAXALIGN so rep is aligned)
 *				IndexTuple rep (repsz bytes)
 *
 * Returns a palloc'd IndexTuple (the directory item).  *itemsz is its size.
 */
static IndexTuple
_bt_prefix_form_diritem(BTPrefixDir *dir, Size *itemsz)
{
	int			ng = dir->ngroups;
	Size		total = _bt_prefix_diritem_size(ng, 0);
	Size	   *anchoroff = palloc(sizeof(Size) * ng);
	char	   *buf;
	IndexTuple	item;
	BTPrefixDirHdr *hdr;

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
	hdr->decodedsz = 0;			/* set by the caller once items are placed */
	hdr->maxdecoded = 0;
	for (int g = 0; g < ng; g++)
		hdr->anchoroff[g] = (uint16) anchoroff[g];

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

/*
 * Read the directory item off a BTP_PREFIX page.
 */
static inline char *
_bt_prefix_diritem(Page page)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber off = BTPageGetPrefixOff(opaque);

	Assert(P_HASPREFIX(opaque));
	return (char *) PageGetItem(page, PageGetItemId(page, off));
}

/* Pointer to anchor g's on-disk blob within the directory item. */
static inline BTPrefixAnchorDisk *
_bt_prefix_anchor(char *diritem, int g)
{
	return (BTPrefixAnchorDisk *) (diritem +
								   ((BTPrefixDirHdr *) diritem)->anchoroff[g]);
}

/* The representative IndexTuple inside an on-disk anchor blob. */
static inline IndexTuple
_bt_prefix_anchor_rep(BTPrefixAnchorDisk *ad)
{
	return (IndexTuple) ((char *) ad + MAXALIGN(sizeof(BTPrefixAnchorDisk)));
}


/*
 * _bt_prefix_encode_against
 *		Produce the coded form of plain tuple `itup' against group `group',
 *		whose representative is `rep'.  Returns a palloc'd coded IndexTuple;
 *		t_tid (or a posting list) is carried over unchanged.
 */
static IndexTuple
_bt_prefix_encode_against(IndexTuple rep, IndexTuple itup, int group)
{
	Size		hoff = IndexInfoFindDataOffset(itup->t_info);
	Size		keysz = _bt_prefix_keysz(itup);
	bool		posting = BTreeTupleIsPosting(itup);
	Size		shared_len;		/* bytes of the data area taken from rep */
	Size		residual;
	Size		bodysz;
	Size		codesz;
	char	   *buf;
	IndexTuple	coded;
	BTPrefixCode *code;

	shared_len = _bt_prefix_shared_len(rep, itup);

	residual = keysz - hoff - shared_len;
	bodysz = MAXALIGN(sizeof(IndexTupleData) + sizeof(BTPrefixCode) +
					  (hoff - sizeof(IndexTupleData)) + residual);
	codesz = bodysz;
	if (posting)
		codesz = MAXALIGN(bodysz + BTreeTupleGetNPosting(itup) *
						  sizeof(ItemPointerData));
	if (codesz > INDEX_SIZE_MASK)
		elog(ERROR, "coded nbtree tuple too large");

	buf = palloc0(codesz);
	coded = (IndexTuple) buf;
	coded->t_tid = itup->t_tid;
	coded->t_info = (unsigned short) codesz;

	code = (BTPrefixCode *) (buf + sizeof(IndexTupleData));
	code->group = (uint16) group;
	code->shared_len = (uint16) shared_len;
	/* flags and key-part size of the original; posting-ness is the coded's */
	code->orig_info = (itup->t_info & ~(INDEX_SIZE_MASK | INDEX_ALT_TID_MASK)) |
		keysz;

	/* copy the original's null bitmap (bytes between header and data area) */
	memcpy(buf + sizeof(IndexTupleData) + sizeof(BTPrefixCode),
		   (char *) itup + sizeof(IndexTupleData),
		   hoff - sizeof(IndexTupleData));

	/* copy the residual: the original's data-area bytes from shared_len on */
	memcpy(buf + sizeof(IndexTupleData) + sizeof(BTPrefixCode) +
		   (hoff - sizeof(IndexTupleData)),
		   (char *) itup + hoff + shared_len, residual);

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
 *		Decode coded member `coded' on `page' into caller workspace `buf',
 *		returning the plain IndexTuple.  Pure byte-splicing; no catalog, so WAL
 *		redo can call it.
 */
IndexTuple
_bt_prefix_decode(Page page, IndexTuple coded, BTItemBuf *buf)
{
	char	   *diritem = _bt_prefix_diritem(page);
	BTPrefixCode *code = (BTPrefixCode *) ((char *) coded +
										   sizeof(IndexTupleData));
	BTPrefixAnchorDisk *ad = _bt_prefix_anchor(diritem, code->group);
	IndexTuple	rep = _bt_prefix_anchor_rep(ad);
	Size		rephoff = IndexInfoFindDataOffset(rep->t_info);
	Size		hoff = IndexInfoFindDataOffset(code->orig_info);
	Size		bitmaplen = hoff - sizeof(IndexTupleData);
	char	   *codebody = (char *) code + sizeof(BTPrefixCode);
	Size		residual;
	char	   *out = buf->data;
	IndexTuple	plain = (IndexTuple) out;
	Size		keysz;

	/*
	 * The original's key-part size is in orig_info; derive the residual
	 * length from it, not from the coded tuple's size (which has its own
	 * padding and any posting list).
	 */
	keysz = (code->orig_info & INDEX_SIZE_MASK);
	residual = keysz - hoff - code->shared_len;

	/* header + null bitmap from the original */
	plain->t_tid = coded->t_tid;
	plain->t_info = code->orig_info;
	memcpy(out + sizeof(IndexTupleData), codebody, bitmaplen);
	/* shared leading data bytes from the representative */
	memcpy(out + hoff, (char *) rep + rephoff, code->shared_len);
	/* the member's own residual */
	memcpy(out + hoff + code->shared_len, codebody + bitmaplen, residual);

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
 *		Lay out a BTP_PREFIX leaf in `newpage' (already _bt_pageinit'd, with
 *		its opaque area set) from high key `hikey' (NULL on a rightmost page)
 *		and plain data tuples items[0..n-1] in key order.  Returns false,
 *		leaving `newpage' unspecified, if prefix coding does not pay off (unless
 *		`force') or the coded items do not fit one page.  The page's LSN and
 *		prune XID are the caller's business.
 */
bool
_bt_prefix_form_page(Page newpage, IndexTuple hikey, IndexTuple *items, int n,
					 bool force)
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
		_bt_prefix_est_add(&est, items[i]);
	if (!force && !_bt_prefix_worthwhile(_bt_prefix_est_coded(&est),
										 est.plainsz))
		return false;

	dir = _bt_prefix_choose_groups(items, n);
	if (dir == NULL)
	{
		Assert(!force || n == 1);
		return false;			/* nothing shared */
	}

	if (hikey &&
		PageAddItem(newpage, hikey, MAXALIGN(IndexTupleSize(hikey)), P_HIKEY,
					false, false) == InvalidOffsetNumber)
		elog(ERROR, "could not add high key during prefix compression");

	diritem = _bt_prefix_form_diritem(dir, &diritemsz);
	if (PageAddItem(newpage, diritem, diritemsz,
					BTPageGetPrefixOff(BTPageGetOpaque(newpage)),
					false, false) == InvalidOffsetNumber)
		ok = false;
	newoff = OffsetNumberNext(BTPageGetPrefixOff(BTPageGetOpaque(newpage)));

	/* code each data item against its group */
	for (int i = 0, g = 0; ok && i < n; i++)
	{
		IndexTuple	coded;

		/* advance g to the group this item belongs to */
		while (g + 1 < dir->ngroups && items[i] == dir->anchors[g + 1].rep)
			g++;

		coded = _bt_prefix_encode_against(dir->anchors[g].rep, items[i], g);
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
 *		Rewrite a finished plain leaf `page' into BTP_PREFIX form, in place,
 *		if a prefix directory pays off.  Called for each half of a split.
 *		Leaves the page unchanged (and not BTP_PREFIX) when prefix compression
 *		would not help.
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
	PGAlignedBlock newpagebuf;
	Page		newpage = (Page) newpagebuf.data;

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

	_bt_pageinit(newpage, BLCKSZ);
	memcpy(BTPageGetOpaque(newpage), opaque, sizeof(BTPageOpaqueData));
	if (_bt_prefix_form_page(newpage, hikey, items, n, false))
	{
		PageSetLSN(newpage, PageGetLSN(page));
		((PageHeader) newpage)->pd_prune_xid = ((PageHeader) page)->pd_prune_xid;
		memcpy(page, newpage, BLCKSZ);
	}
	pfree(items);
}

/*
 * _bt_prefix_encode_insert
 *		Code plain tuple `itup' for insertion at `newitemoff' on BTP_PREFIX
 *		page `page'.  The new tuple joins the group of its neighbors: the
 *		governing group is read from the coded item currently at newitemoff
 *		(the item the insert shifts right) or, at the end of the data area,
 *		from the item just before.  `itup' is coded against that group's
 *		anchor, sharing however many leading bytes it happens to share
 *		(possibly none); it is never larger than the plain tuple plus the small
 *		code header.  Returns a palloc'd coded
 *		IndexTuple (caller frees).  Must run BEFORE the WAL critical section.
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

	Assert(P_HASPREFIX(opaque));

	if (newitemoff <= maxoff && newitemoff >= P_FIRSTDATAKEY(opaque))
		neighbor = newitemoff;
	else if (maxoff >= P_FIRSTDATAKEY(opaque))
		neighbor = maxoff;
	else
	{
		/* no data items yet: code uncoded against group 0 */
		ad = _bt_prefix_anchor(diritem, 0);
		return _bt_prefix_encode_against(_bt_prefix_anchor_rep(ad), itup, 0);
	}

	ncoded = (IndexTuple) PageGetItem(page, PageGetItemId(page, neighbor));
	ncode = (BTPrefixCode *) ((char *) ncoded + sizeof(IndexTupleData));
	group = ncode->group;
	ad = _bt_prefix_anchor(diritem, group);
	rep = _bt_prefix_anchor_rep(ad);

	return _bt_prefix_encode_against(rep, itup, group);
}



/*
 * _bt_prefix_recode
 *		Code plain tuple `itup' against the group of the coded item at `offnum'
 *		on BTP_PREFIX page `page'.  Used to replace a coded posting list tuple
 *		with its post-split version (_bt_swap_posting), which keeps the same
 *		key and therefore codes identically.  palloc'd result.
 */
IndexTuple
_bt_prefix_recode(Page page, OffsetNumber offnum, IndexTuple itup)
{
	IndexTuple	ncoded = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));
	BTPrefixCode *ncode = (BTPrefixCode *) ((char *) ncoded +
											sizeof(IndexTupleData));
	BTPrefixAnchorDisk *ad = _bt_prefix_anchor(_bt_prefix_diritem(page),
											   ncode->group);
	IndexTuple	rep = _bt_prefix_anchor_rep(ad);

	return _bt_prefix_encode_against(rep, itup, ncode->group);
}

/*
 * _bt_prefix_decode_items
 *		Decode every data item of BTP_PREFIX page `page' into a palloc'd array
 *		of plain IndexTuples (each palloc'd via CopyIndexTuple).  *hikey gets a
 *		palloc'd copy of the high key (NULL if rightmost).  Returns the item
 *		count.  Used by _bt_split to split a coded page whose decoded content
 *		may exceed one page, by reasoning about the plain tuples directly.
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
		IndexTuple	coded = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		items[k++] = CopyIndexTuple(_bt_prefix_decode(page, coded, &decbuf));
	}
	*items_out = items;
	return n;
}

/*
 * _bt_prefix_try_plain_page
 *		If BTP_PREFIX page `page' decodes to content that fits one BLCKSZ page,
 *		lay a plain copy of it (high key + decoded items in key order) into the
 *		caller-provided BLCKSZ buffer `out' and return it.  If it would overflow
 *		a page, return NULL (caller uses the decoded-item array path for the
 *		split instead).  `out' is owned by the caller, so this is reentrant.
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
		IndexTuple	coded = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		IndexTuple	pt = _bt_prefix_decode(page, coded, &decbuf);

		if (PageAddItem(out, pt, IndexTupleSize(pt), newoff, false, false) ==
			InvalidOffsetNumber)
			return NULL;		/* decoded content exceeds a page */
		newoff = OffsetNumberNext(newoff);
	}

	/*
	 * _bt_findsplitloc models the split as if the new item were already on
	 * the page, and sizes its candidate array by the current item count.  If
	 * the decoded page is so full that there's no room to imagine the new
	 * item, use the decoded-item array split path instead (signalled by
	 * returning NULL).
	 */
	if (PageGetFreeSpace(out) < (Size) BTMaxItemSize)
		return NULL;
	return out;
}

/*
 * Plain (decoded) size of coded member `coded', from its header alone.
 *
 * This is the MAXALIGN'd on-page footprint of the decoded tuple (what
 * PageAddItem would consume, before its line pointer), so that the directory's
 * cached decodedsz/maxdecoded match the sizes _bt_prefix_findsplitloc and the
 * CREATE INDEX estimate reason with (both MAXALIGN(IndexTupleSize(...))).  The
 * split-fit guarantee in _bt_prefix_split_needed depends on this unit being
 * identical to the real split's.
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
 * The directory's cached decoded sizes packed into one value, for amcheck.
 */
uint32
_bt_prefix_cached_size(Page page)
{
	BTPrefixDirHdr *hdr = (BTPrefixDirHdr *) _bt_prefix_diritem(page);

	return ((uint32) hdr->decodedsz << 16) | hdr->maxdecoded;
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
 *		Choose a split point for coded leaf `page', reasoning about its decoded
 *		items (ditems[], one per data item) as the two plain halves they will
 *		be written as.  With newitemoff valid, the new item (plain size
 *		newitemsz) is imagined at newitemoff and the result may be
 *		InvalidOffsetNumber if no point keeps both halves within a page; with
 *		InvalidOffsetNumber the existing items alone are split and a legal
 *		point always exists.  Returns firstrightoff and sets *newitemonleft,
 *		with the same meaning as _bt_findsplitloc.
 *
 * Every candidate is checked against the actual sizes of both halves: the
 * left half's new high key is charged as firstright plus a heap TID (as
 * _bt_findsplitloc does), the right half keeps the old high key.  Among legal
 * points the most even one wins.  _bt_prefix_split_needed guarantees a legal
 * point exists.
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

			if (delta < bestdelta)
			{
				bestdelta = delta;
				best = k;
			}
		}
	}
	pfree(sz);

	/*
	 * With a new item, a missing legal point is possible (the item is large
	 * and the page dense): return InvalidOffsetNumber so the caller splits
	 * without it.  Without a new item, the split-fit invariant guarantees a
	 * point (see _bt_prefix_split_needed).
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
