/*-------------------------------------------------------------------------
 *
 * nbtprefix.c
 *	  Leaf key prefix compression for Postgres btrees.
 *
 * A leaf page of an index whose prefix_compression reloption is on, and whose
 * first key column is of a varlena type, may be flagged BTP_PREFIX.  Such a
 * page front-codes the first key column of each non-pivot tuple against a
 * per-page PREFIX item, so that a long key that repeats on a page costs
 * little more than its unshared tail.  The on-disk format is documented at
 * BTP_PREFIX in nbtree.h; this file is the encoder, decoder, and the policy
 * that chooses a page's prefix.
 *
 * Coding and decoding depend only on the page's prefix and the tuple, not on
 * the catalog, so WAL redo repeats them, and decoding reproduces the plain
 * tuple byte for byte.  A tuple inserted later that shares less of the prefix
 * just stores a smaller `shared' count, so no insert, VACUUM, deduplication
 * or change of the page's key range ever has to rewrite the page to keep the
 * prefix valid.  The prefix is chosen only when a page is written as a whole
 * (CREATE INDEX, or a half of a split).
 *
 * This is modeled on the leaf prefix compression of the BARK access method
 * (src/backend/access/bark/barkutils.c).
 *
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
#include "storage/bufpage.h"
#include "utils/rel.h"

/*
 * Where each part of a tuple goes in its coded form.  Coding and decoding
 * both lay the tuple out from this, so the two cannot disagree.
 */
typedef struct BTPrefixPlan
{
	Size		hoff;			/* data offset: header and null bitmap */
	Size		c1;				/* plain first column's size, with header */
	Size		keyend;			/* end of the plain key (posting offset) */
	uint8		code;			/* first payload byte of the coded column */
	const char *suffix;			/* bytes stored after it */
	Size		suffixlen;
	Size		vsize;			/* coded column's size, with header */
	Size		codedkeyend;	/* end of the coded key */
	Size		total;			/* size of the coded tuple */
} BTPrefixPlan;

/*
 * Is `itup' stored coded on a BTP_PREFIX page?  Only a non-pivot tuple (plain
 * or posting) whose first column is not NULL; a pivot (high key, downlink)
 * is stored as it is.
 */
static bool
_bt_prefix_codable(IndexTuple itup)
{
	if (BTreeTupleIsPivot(itup))
		return false;
	return !IndexTupleHasNulls(itup) ||
		!att_isnull(0, (uint8 *) itup + sizeof(IndexTupleData));
}

/*
 * Does the varlena `val' have the header decoding would give it back: the
 * 1-byte header if its data fit one, else an uncompressed 4-byte header?
 */
static bool
_bt_prefix_canonical(const char *val)
{
	if (VARATT_IS_SHORT(val))
		return !VARATT_IS_EXTERNAL(val);
	return !VARATT_IS_EXTENDED(val) &&
		VARSIZE(val) - VARHDRSZ + VARHDRSZ_SHORT > VARATT_SHORT_MAX;
}

/*
 * End of a leaf tuple's key: the posting-list offset for a posting tuple, the
 * whole tuple size otherwise.  The bytes after the key (a posting list) are
 * moved as they are.
 */
static Size
_bt_prefix_keyend(IndexTuple itup)
{
	if (BTreeTupleIsPosting(itup))
		return BTreeTupleGetPostingOffset(itup);
	return IndexTupleSize(itup);
}

/*
 * The size of a coded first column whose payload is `payload' bytes: a
 * varlena with a 1-byte header when it fits one.
 */
static inline Size
_bt_prefix_vsize(Size payload)
{
	if (payload + VARHDRSZ_SHORT <= VARATT_SHORT_MAX)
		return payload + VARHDRSZ_SHORT;
	return payload + VARHDRSZ;
}

/*
 * Offset at which the bytes after the first column go in a coded tuple whose
 * coded column ends at `vend', when in the plain tuple they start at
 * `restorig': the first offset at or after `vend' that is the same distance
 * past a MAXALIGN boundary.
 */
static inline Size
_bt_prefix_restoff(Size vend, Size restorig)
{
	return vend + (MAXIMUM_ALIGNOF + restorig % MAXIMUM_ALIGNOF -
				   vend % MAXIMUM_ALIGNOF) % MAXIMUM_ALIGNOF;
}

/* Plan the coding of the codable tuple `itup' against `prefix'. */
static void
_bt_prefix_plan(const char *prefix, Size prefixlen, IndexTuple itup,
				BTPrefixPlan *plan)
{
	const char *val;
	Size		restorig;
	bool		storerest;

	plan->hoff = IndexInfoFindDataOffset(itup->t_info);
	val = (const char *) itup + plan->hoff;
	plan->c1 = VARSIZE_ANY(val);
	plan->keyend = _bt_prefix_keyend(itup);

	if (_bt_prefix_canonical(val))
	{
		const char *data = VARDATA_ANY(val);
		Size		datalen = VARSIZE_ANY_EXHDR(val);
		Size		limit = Min(prefixlen, datalen);
		Size		shared = 0;

		while (shared < limit && prefix[shared] == data[shared])
			shared++;
		plan->code = (uint8) shared;
		plan->suffix = data + shared;
		plan->suffixlen = datalen - shared;
	}
	else
	{
		plan->code = BTREE_PREFIX_RAW;
		plan->suffix = val;
		plan->suffixlen = plan->c1;
	}
	plan->vsize = _bt_prefix_vsize(1 + plan->suffixlen);

	/*
	 * The bytes after the first column can be left out when they are just the
	 * zero padding that ends the key.
	 */
	restorig = plan->hoff + plan->c1;
	storerest = plan->keyend != MAXALIGN(restorig);
	for (Size i = restorig; !storerest && i < plan->keyend; i++)
		storerest = ((const char *) itup)[i] != 0;

	if (storerest)
	{
		plan->code |= BTREE_PREFIX_REST;
		plan->codedkeyend = _bt_prefix_restoff(plan->hoff + plan->vsize,
											   restorig) +
			(plan->keyend - restorig);
	}
	else
		plan->codedkeyend = MAXALIGN(plan->hoff + plan->vsize);
	plan->total = plan->codedkeyend + (IndexTupleSize(itup) - plan->keyend);
}

/*
 * Copy n bytes, as memcpy.  Decoding moves a few short runs of bytes per
 * tuple; eight bytes at a time is several times faster than a libc memcpy
 * for runs this short (the string-move startup cost dominates otherwise).
 */
static inline void
_bt_prefix_copy(char *dst, const char *src, Size n)
{
	while (n >= sizeof(uint64))
	{
		memcpy(dst, src, sizeof(uint64));
		dst += sizeof(uint64);
		src += sizeof(uint64);
		n -= sizeof(uint64);
	}
	while (n-- > 0)
		*dst++ = *src++;
}

/* The prefix of a BTP_PREFIX page, and its length in *len. */
const char *
_bt_page_get_prefix(Page page, Size *len)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	IndexTuple	item;

	Assert(P_HASPREFIX(opaque));
	item = (IndexTuple) PageGetItem(page,
									PageGetItemId(page,
												  BTPageGetPrefixOff(opaque)));
	*len = IndexTupleSize(item) - sizeof(IndexTupleData);
	return (const char *) item + sizeof(IndexTupleData);
}

/*
 * Give `page', a leaf holding nothing yet but its high key (if it has one),
 * the prefix `prefix' of `len' bytes: add the PREFIX item and set BTP_PREFIX.
 * Its padding comes from the page's free space, which _bt_pageinit zeroed, so
 * a page rebuilt in redo is the same.
 */
void
_bt_page_set_prefix(Page page, const char *prefix, Size len)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	union
	{
		IndexTupleData hdr;
		char		data[sizeof(IndexTupleData) + BTREE_PREFIX_MAX];
	}			item;

	Assert(P_ISLEAF(opaque) && !P_HASPREFIX(opaque));
	Assert(len >= 1 && len <= BTREE_PREFIX_MAX);
	Assert(PageGetMaxOffsetNumber(page) == BTPageGetPrefixOff(opaque) - 1);

	memset(&item.hdr, 0, sizeof(IndexTupleData));
	item.hdr.t_info = (unsigned short) (sizeof(IndexTupleData) + len);
	memcpy(item.data + sizeof(IndexTupleData), prefix, len);
	if (PageAddItem(page, item.data, sizeof(IndexTupleData) + len,
					BTPageGetPrefixOff(opaque), false, false) ==
		InvalidOffsetNumber)
		elog(ERROR, "failed to add prefix item to btree page");
	opaque->btpo_flags |= BTP_PREFIX;
}

/*
 * The coded form of the leaf tuple `itup' for `page', palloc'd; `itup' itself
 * when the page has no prefix or the tuple is stored plain.
 */
IndexTuple
_bt_prefix_encode(Page page, IndexTuple itup)
{
	const char *prefix;
	Size		prefixlen;
	BTPrefixPlan plan;
	char	   *coded;
	char	   *v;
	char	   *payload;

	if (!P_HASPREFIX(BTPageGetOpaque(page)) ||
		!_bt_prefix_codable(itup))
		return itup;

	prefix = _bt_page_get_prefix(page, &prefixlen);
	_bt_prefix_plan(prefix, prefixlen, itup, &plan);
	Assert(plan.total <= INDEX_SIZE_MASK);

	coded = palloc0(plan.total);
	memcpy(coded, itup, plan.hoff);
	v = coded + plan.hoff;
	if (plan.vsize <= VARATT_SHORT_MAX)
	{
		SET_VARSIZE_SHORT(v, plan.vsize);
		payload = v + VARHDRSZ_SHORT;
	}
	else
	{
		SET_VARSIZE(v, plan.vsize);
		payload = v + VARHDRSZ;
	}
	payload[0] = (char) plan.code;
	memcpy(payload + 1, plan.suffix, plan.suffixlen);
	if (plan.code & BTREE_PREFIX_REST)
	{
		Size		restorig = plan.hoff + plan.c1;

		memcpy(coded + _bt_prefix_restoff(plan.hoff + plan.vsize, restorig),
			   (char *) itup + restorig, plan.keyend - restorig);
	}
	memcpy(coded + plan.codedkeyend, (char *) itup + plan.keyend,
		   IndexTupleSize(itup) - plan.keyend);

	((IndexTuple) coded)->t_info =
		(itup->t_info & ~INDEX_SIZE_MASK) | (unsigned short) plan.total;
	if (BTreeTupleIsPosting(itup))
		BTreeTupleSetPosting((IndexTuple) coded, BTreeTupleGetNPosting(itup),
							 plan.codedkeyend);

#ifdef USE_ASSERT_CHECKING
	{
		BTItemBuf	check;
		IndexTuple	decoded = _bt_prefix_decode(page, (IndexTuple) coded,
												&check);

		Assert(IndexTupleSize(decoded) == IndexTupleSize(itup) &&
			   memcmp(decoded, itup, IndexTupleSize(itup)) == 0);
	}
#endif

	return (IndexTuple) coded;
}

/* The MAXALIGNed size `itup' takes on `page', coded if the page codes it. */
Size
_bt_prefix_coded_size(Page page, IndexTuple itup)
{
	const char *prefix;
	Size		prefixlen;
	BTPrefixPlan plan;

	if (!P_HASPREFIX(BTPageGetOpaque(page)) ||
		!_bt_prefix_codable(itup))
		return MAXALIGN(IndexTupleSize(itup));

	prefix = _bt_page_get_prefix(page, &prefixlen);
	_bt_prefix_plan(prefix, prefixlen, itup, &plan);
	return MAXALIGN(plan.total);
}

/*
 * Decode the item `itup' of the BTP_PREFIX page `page' into `buf', and return
 * it; an item stored plain is returned as it is.  The inverse of
 * _bt_prefix_encode, byte for byte.  An item that does not decode within its
 * own bounds, or claims more of the prefix than the page has, is reported as
 * corruption.
 */
IndexTuple
_bt_prefix_decode(Page page, IndexTuple itup, BTItemBuf *buf)
{
	const char *prefix;
	Size		prefixlen;
	Size		size = IndexTupleSize(itup);
	Size		hoff;
	Size		codedkeyend;
	const char *v;
	Size		vsize = 0;
	const char *payload;
	Size		paylen = 0;
	uint8		code;
	Size		shared;
	char	   *out = buf->data;
	char	   *val = NULL;
	Size		c1;
	Size		restorig;
	Size		keyend;

	if (!_bt_prefix_codable(itup))
		return itup;

	prefix = _bt_page_get_prefix(page, &prefixlen);
	hoff = IndexInfoFindDataOffset(itup->t_info);
	codedkeyend = _bt_prefix_keyend(itup);
	v = (const char *) itup + hoff;
	if (hoff >= codedkeyend || codedkeyend > size ||
		(vsize = VARSIZE_ANY(v)) > codedkeyend - hoff ||
		(paylen = VARSIZE_ANY_EXHDR(v)) < 1)
		goto corrupt;
	payload = VARDATA_ANY(v);
	code = (uint8) payload[0];
	shared = code & BTREE_PREFIX_SHARED_MASK;

	_bt_prefix_copy(out, (const char *) itup, hoff);
	val = out + hoff;
	if (shared == BTREE_PREFIX_RAW)
	{
		c1 = paylen - 1;
		_bt_prefix_copy(val, payload + 1, c1);
		if (c1 < VARHDRSZ_SHORT || VARSIZE_ANY(val) != c1)
			goto corrupt;
	}
	else
	{
		Size		datalen = shared + paylen - 1;
		char	   *data;

		if (hoff + VARHDRSZ + datalen > sizeof(BTItemBuf))
			goto corrupt;
		if (shared > prefixlen)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg_internal("btree leaf tuple shares %zu bytes of a %zu-byte page prefix",
									 shared, prefixlen)));
		if (datalen + VARHDRSZ_SHORT <= VARATT_SHORT_MAX)
		{
			c1 = datalen + VARHDRSZ_SHORT;
			SET_VARSIZE_SHORT(val, c1);
			data = val + VARHDRSZ_SHORT;
		}
		else
		{
			c1 = datalen + VARHDRSZ;
			SET_VARSIZE(val, c1);
			data = val + VARHDRSZ;
		}
		_bt_prefix_copy(data, prefix, shared);
		_bt_prefix_copy(data + shared, payload + 1, paylen - 1);
	}

	restorig = hoff + c1;
	if (code & BTREE_PREFIX_REST)
	{
		Size		restoff = _bt_prefix_restoff(hoff + vsize, restorig);

		if (restoff > codedkeyend)
			goto corrupt;
		keyend = restorig + (codedkeyend - restoff);
		if (keyend + (size - codedkeyend) > sizeof(BTItemBuf))
			goto corrupt;
		_bt_prefix_copy(out + restorig, (const char *) itup + restoff,
						codedkeyend - restoff);
	}
	else
	{
		keyend = MAXALIGN(restorig);
		if (keyend + (size - codedkeyend) > sizeof(BTItemBuf))
			goto corrupt;
		memset(out + restorig, 0, keyend - restorig);
	}
	_bt_prefix_copy(out + keyend, (const char *) itup + codedkeyend,
					size - codedkeyend);

	((IndexTuple) out)->t_info = (itup->t_info & ~INDEX_SIZE_MASK) |
		(unsigned short) (keyend + (size - codedkeyend));
	if (BTreeTupleIsPosting(itup))
		BTreeTupleSetPosting((IndexTuple) out, BTreeTupleGetNPosting(itup),
							 keyend);
	return (IndexTuple) out;

corrupt:
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg_internal("btree leaf tuple is not a valid prefix-coded tuple")));
	return NULL;				/* keep compiler quiet */
}

/*
 * The bytes of `itup''s first column a page prefix may be taken from, and how
 * many (at most BTREE_PREFIX_MAX) in *len; NULL when the tuple has none: it
 * is not codable, its value is empty, or it would be stored raw.
 */
const char *
_bt_prefix_candidate(IndexTuple itup, Size *len)
{
	const char *val;

	if (!_bt_prefix_codable(itup))
		return NULL;
	val = (const char *) itup + IndexInfoFindDataOffset(itup->t_info);
	if (!_bt_prefix_canonical(val) || VARSIZE_ANY_EXHDR(val) == 0)
		return NULL;
	*len = Min(VARSIZE_ANY_EXHDR(val), BTREE_PREFIX_MAX);
	return VARDATA_ANY(val);
}

/*
 * How many bytes of `prefix' the first column of `itup' would share on a page
 * with that prefix: -1 when the tuple would not be coded, 0 when it would be
 * stored raw.
 */
int
_bt_prefix_shared(const char *prefix, Size prefixlen, IndexTuple itup)
{
	BTPrefixPlan plan;

	if (!_bt_prefix_codable(itup))
		return -1;
	_bt_prefix_plan(prefix, prefixlen, itup, &plan);
	if ((plan.code & BTREE_PREFIX_SHARED_MASK) == BTREE_PREFIX_RAW)
		return 0;
	return plan.code & BTREE_PREFIX_SHARED_MASK;
}

/*
 * May the leaves of `index' have a prefix?  Only when the prefix_compression
 * reloption is on and the first key column is of a varlena type.
 */
bool
_bt_prefix_enabled(Relation index)
{
	return BTGetPrefixCompression(index) &&
		TupleDescAttr(RelationGetDescr(index), 0)->attlen == -1;
}

/*
 * Choose the prefix for a leaf about to be built from the `n' tuples `items',
 * in key order: the first tuple's first-column bytes, cut to the longest run
 * any later tuple shares, provided the tuples share at least
 * BTREE_PREFIX_MIN_SHARED bytes of it on average.  Returns the prefix's
 * length, with *prefix pointing into items[0], or 0 for no prefix.
 */
Size
_bt_prefix_choose(Relation index, IndexTuple *items, int n,
				  const char **prefix)
{
	const char *cand;
	Size		candlen;
	Size		longest = 0;
	Size		total = 0;
	int			ncoded = 0;

	if (!_bt_prefix_enabled(index) ||
		(cand = _bt_prefix_candidate(items[0], &candlen)) == NULL)
		return 0;
	if (n == 1)
	{
		*prefix = cand;
		return candlen;
	}
	for (int i = 1; i < n; i++)
	{
		int			shared = _bt_prefix_shared(cand, candlen, items[i]);

		if (shared < 0)
			continue;
		total += shared;
		ncoded++;
		longest = Max(longest, (Size) shared);
	}
	if (longest == 0 || total < (Size) BTREE_PREFIX_MIN_SHARED * ncoded)
		return 0;
	*prefix = cand;
	return longest;
}

/*
 * Fault a freshly finished leaf `page' forward into the BTP_PREFIX format if
 * a prefix pays off (Strategy 1: compress a page only when it is written as a
 * whole -- CREATE INDEX page finish, a split half, a dedup rewrite -- so that
 * no insert, VACUUM or key-range change ever forces a page rewrite, and an
 * old-format page is converted only when it is next written whole).
 *
 * On entry `page' is a complete leaf: its high key at P_HIKEY if it is not
 * rightmost, then its data items, none of them coded (the page does not yet
 * have BTP_PREFIX).  If the data items share enough of a common first-column
 * prefix, the page is rebuilt in place with the PREFIX item inserted after
 * the high key and every data item coded against it; otherwise the page is
 * left unchanged.  Item order and the high key are preserved.
 *
 * Caller must hold the page's buffer exclusively (or own the page outright,
 * as the build and split paths do with a temp page).  No WAL is written here:
 * callers log the resulting page (a build writes it whole; a split logs each
 * half whole or by full-page image), and redo reaches the same bytes by
 * running _bt_prefix_encode against the same prefix.
 */
void
_bt_prefix_compress_page(Relation index, Page page)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber minoff;
	OffsetNumber maxoff;
	int			n;
	IndexTuple *items;
	Size		prefixlen;
	const char *prefix;
	Page		tmp;
	OffsetNumber o;
	IndexTuple	hikey = NULL;
	Size		hikeysz = 0;

	Assert(P_ISLEAF(opaque) && !P_HASPREFIX(opaque));

	if (!_bt_prefix_enabled(index))
		return;

	minoff = P_FIRSTDATAKEY(opaque);
	maxoff = PageGetMaxOffsetNumber(page);
	n = (int) maxoff - (int) minoff + 1;
	if (n <= 0)
		return;

	items = (IndexTuple *) palloc(n * sizeof(IndexTuple));
	for (int i = 0; i < n; i++)
		items[i] = (IndexTuple) PageGetItem(page,
											PageGetItemId(page, minoff + i));

	prefixlen = _bt_prefix_choose(index, items, n, &prefix);
	if (prefixlen == 0)
	{
		pfree(items);
		return;
	}

	/* The high key (if any) stays at P_HIKEY, uncoded. */
	if (!P_RIGHTMOST(opaque))
	{
		ItemId		hii = PageGetItemId(page, P_HIKEY);

		hikeysz = ItemIdGetLength(hii);
		hikey = (IndexTuple) PageGetItem(page, hii);
	}

	/*
	 * Rebuild into a temp page: high key (if any), PREFIX item, then each
	 * data item coded.  The coded form can be up to MAXALIGN larger than the
	 * plain one, but the PREFIX item replaces far more shared bytes than it
	 * adds, so a page that was not overfull stays within bounds; if a coded
	 * item somehow does not fit, leave the page uncompressed (always valid).
	 */
	tmp = PageGetTempPageCopySpecial(page);
	BTPageGetOpaque(tmp)->btpo_flags &= ~BTP_PREFIX;
	if (hikey != NULL &&
		PageAddItem(tmp, (char *) hikey, hikeysz, P_HIKEY, false, false) ==
		InvalidOffsetNumber)
		goto giveup;
	_bt_page_set_prefix(tmp, prefix, prefixlen);
	o = OffsetNumberNext(PageGetMaxOffsetNumber(tmp));
	for (int i = 0; i < n; i++)
	{
		IndexTuple	coded = _bt_prefix_encode(tmp, items[i]);
		bool		ok;

		ok = PageAddItem(tmp, (char *) coded, IndexTupleSize(coded), o++,
						 false, false) != InvalidOffsetNumber;
		if (coded != items[i])
			pfree(coded);
		if (!ok)
			goto giveup;
	}

	memcpy(page, tmp, BLCKSZ);
	pfree(tmp);
	pfree(items);
	return;

giveup:
	pfree(tmp);
	pfree(items);
}

/*
 * Lay out one half of a leaf split on `page', which the caller has
 * initialized (flags, links) and left empty: the high key `hikey' (NULL for
 * a rightmost half), then the `n' plain tuples `items' in key order.
 *
 * The half is formed as a whole, so it may take a prefix of its own: its
 * first item's leading bytes, if the items share enough (_bt_prefix_choose).
 * Because a prefix-compressed leaf can hold more logical data than fits in a
 * plain page, the half is laid out coded from the start rather than built
 * plain and compressed afterwards.  When the half does not fit with its own
 * prefix (or shares too little to take one) it is laid out plain, and if that
 * does not fit either, with `origprefix' (the splitting page's prefix, zero
 * length when it had none), against which it fits by construction: the items
 * keep the shares they had on the original page.
 *
 * Mirrors BARK's bark_split_leaf_half.  Callers: _bt_split (both halves).
 */
void
_bt_split_leaf_half(Relation index, Page page, IndexTuple hikey,
					IndexTuple *items, int n,
					const char *origprefix, Size origprefixlen,
					char *chosenprefix, Size *chosenprefixlen)
{
	BTPageOpaqueData saved = *BTPageGetOpaque(page);
	const char *prefix[3];
	Size		prefixlen[3];
	int			ntries = 0;

	Assert(P_ISLEAF(&saved) && !P_HASPREFIX(&saved));

	/* Candidate prefixes to try, in order: own, none, the orig page's. */
	if (_bt_prefix_enabled(index))
	{
		prefixlen[ntries] = _bt_prefix_choose(index, items, n, &prefix[ntries]);
		if (prefixlen[ntries] > 0)
			ntries++;
	}
	prefix[ntries] = NULL;
	prefixlen[ntries++] = 0;
	if (origprefixlen > 0)
	{
		prefix[ntries] = origprefix;
		prefixlen[ntries++] = origprefixlen;
	}

	for (int t = 0; t < ntries; t++)
	{
		if (_bt_split_leaf_layout(page, hikey, items, n, prefix[t], prefixlen[t]))
		{
			/* Report the prefix that won, so the split can log it for redo. */
			if (chosenprefixlen != NULL)
			{
				*chosenprefixlen = prefixlen[t];
				if (prefixlen[t] > 0)
					memcpy(chosenprefix, prefix[t], prefixlen[t]);
			}
			return;
		}
		/* restore the empty-page state for the next attempt */
		_bt_pageinit(page, BLCKSZ);
		*BTPageGetOpaque(page) = saved;
	}
	elog(ERROR, "failed to lay out a split half of index \"%s\"",
		 RelationGetRelationName(index));
}

/*
 * Lay out one leaf split half on the caller-initialized empty `page' using a
 * GIVEN prefix (`prefixlen' 0 for none): the high key `hikey' (NULL for a
 * rightmost half), the PREFIX item, then the `n' plain tuples `items' coded
 * against the prefix.  Returns false if they do not fit (caller tries another
 * prefix).  This is Relation-free and deterministic in (hikey, items, prefix),
 * so WAL redo calls it with the logged prefix to reproduce the primary's page
 * byte for byte.
 */
bool
_bt_split_leaf_layout(Page page, IndexTuple hikey, IndexTuple *items, int n,
					  const char *prefix, Size prefixlen)
{
	OffsetNumber o = P_HIKEY;
	bool		fit = true;

	Assert(P_ISLEAF(BTPageGetOpaque(page)) && !P_HASPREFIX(BTPageGetOpaque(page)));
	if (hikey != NULL &&
		PageAddItem(page, (char *) hikey, MAXALIGN(IndexTupleSize(hikey)),
					o++, false, false) == InvalidOffsetNumber)
		return false;
	if (prefixlen > 0)
	{
		_bt_page_set_prefix(page, prefix, prefixlen);
		o++;
	}
	for (int i = 0; i < n; i++)
	{
		IndexTuple	coded = _bt_prefix_encode(page, items[i]);

		if (PageAddItem(page, (char *) coded, IndexTupleSize(coded),
						o++, false, false) == InvalidOffsetNumber)
			fit = false;
		if (coded != items[i])
			pfree(coded);
		if (!fit)
			return false;
	}
#ifdef USE_ASSERT_CHECKING
	if (P_HASPREFIX(BTPageGetOpaque(page)))
	{
		OffsetNumber mx = PageGetMaxOffsetNumber(page);
		OffsetNumber off;

		for (off = P_FIRSTDATAKEY(BTPageGetOpaque(page)); off <= mx; off++)
		{
			BTItemBuf	vb;

			(void) _bt_getitem(page, off, &vb);
		}
	}
#endif
	return true;
}

/*
 * Collect the plain (decoded) data items of a leaf split into the left and
 * right arrays, in the exact order and with the exact substitutions that the
 * split performs: the original page's data items split at `firstrightoff',
 * with `newitem' inserted at `newitemoff' on the side given by `newitemonleft'
 * and, for a posting-list split, the posting at `origpagepostingoff' replaced
 * by `nposting'.  `origpage' is the pre-split page (its items are decoded when
 * it is a BTP_PREFIX page).  The arrays receive pointers that are valid until
 * the caller is done laying out the halves; items taken from the page are
 * copied, so the caller owns and should reset the surrounding memory context.
 *
 * Both the primary (_bt_split) and WAL redo (btree_xlog_split) collect items
 * this way and then lay out each half with _bt_split_leaf_half, so the two
 * produce byte-identical coded pages without logging a full-page image.
 */
void
_bt_split_leaf_collect(Page origpage, OffsetNumber firstrightoff,
					   OffsetNumber newitemoff, IndexTuple newitem,
					   bool newitemonleft, OffsetNumber origpagepostingoff,
					   IndexTuple nposting,
					   IndexTuple *leftitems, int *nleft,
					   IndexTuple *rightitems, int *nright)
{
	BTPageOpaque oopaque = BTPageGetOpaque(origpage);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(origpage);
	bool		hasprefix = P_HASPREFIX(oopaque);
	OffsetNumber i;

	*nleft = 0;
	*nright = 0;
	for (i = P_FIRSTDATAKEY(oopaque); i <= maxoff; i = OffsetNumberNext(i))
	{
		ItemId		itemid = PageGetItemId(origpage, i);
		IndexTuple	dataitem = (IndexTuple) PageGetItem(origpage, itemid);
		BTItemBuf	dibuf;

		if (hasprefix)
			dataitem = _bt_prefix_decode(origpage, dataitem, &dibuf);

		if (i == origpagepostingoff)
			dataitem = nposting;
		else if (i == newitemoff)
		{
			if (newitemonleft)
				leftitems[(*nleft)++] = newitem;
			else
				rightitems[(*nright)++] = newitem;
		}

		if (i < firstrightoff)
			leftitems[(*nleft)++] = CopyIndexTuple(dataitem);
		else
			rightitems[(*nright)++] = CopyIndexTuple(dataitem);
	}
	if (i <= newitemoff)
		rightitems[(*nright)++] = newitem;
}
