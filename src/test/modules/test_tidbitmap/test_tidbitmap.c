/*--------------------------------------------------------------------------
 *
 * test_tidbitmap.c
 *		Test TIDBitmap, with several bitmaps alive at once.
 *
 * The executor holds more than one TIDBitmap at a time in BitmapAnd,
 * BitmapOr and GIN partial-match scans.  These functions build bitmaps of
 * any size from synthetic block numbers, keep several of them alive while
 * they grow, and check every bitmap's contents afterwards, so that two
 * bitmaps sharing storage, or a bitmap losing pages, shows up as a wrong
 * answer.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_tidbitmap/test_tidbitmap.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "storage/itemptr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_tidbitmap_check);
PG_FUNCTION_INFO_V1(test_tidbitmap_setop);

/* The offsets expected on a block, as a bitmask of offsets 1..32; 0 = none */
typedef uint32 (*expect_fn) (int arg, BlockNumber blkno);

/*
 * Iterate over "tbm" and check it holds exactly the blocks in [0, nblocks)
 * for which expect() is nonzero, in order, each once, and that every exact
 * page holds exactly the offsets expect() gives.  Returns the number of
 * lossy pages.  A lossy page must have recheck set; an exact page must not
 * (no caller adds tuples with recheck).
 */
static int64
verify_bitmap(TIDBitmap *tbm, expect_fn expect, int arg, BlockNumber nblocks)
{
	TBMIterator it;
	TBMIterateResult res;
	BlockNumber next = 0;
	int64		nlossy = 0;

	it = tbm_begin_iterate(tbm, NULL, InvalidDsaPointer);
	while (tbm_iterate(&it, &res))
	{
		if (res.blockno < next || res.blockno >= nblocks)
			elog(ERROR, "bitmap %d: unexpected block %u", arg, res.blockno);
		for (; next < res.blockno; next++)
			if (expect(arg, next) != 0)
				elog(ERROR, "bitmap %d: block %u missing", arg, next);
		if (expect(arg, res.blockno) == 0)
			elog(ERROR, "bitmap %d: unexpected block %u", arg, res.blockno);

		if (res.lossy)
		{
			if (!res.recheck)
				elog(ERROR, "bitmap %d: lossy block %u without recheck",
					 arg, res.blockno);
			nlossy++;
		}
		else
		{
			OffsetNumber offsets[TBM_MAX_TUPLES_PER_PAGE];
			int			ntuples;
			uint32		mask = 0;

			if (res.recheck)
				elog(ERROR, "bitmap %d: exact block %u with recheck",
					 arg, res.blockno);
			ntuples = tbm_extract_page_tuple(&res, offsets,
											 TBM_MAX_TUPLES_PER_PAGE);
			for (int i = 0; i < ntuples; i++)
			{
				if (offsets[i] < 1 || offsets[i] > 32)
					elog(ERROR, "bitmap %d: block %u: offset %u",
						 arg, res.blockno, offsets[i]);
				mask |= UINT32_C(1) << (offsets[i] - 1);
			}
			if (mask != expect(arg, res.blockno))
				elog(ERROR, "bitmap %d: block %u: offsets 0x%x, expected 0x%x",
					 arg, res.blockno, mask, expect(arg, res.blockno));
		}
		next = res.blockno + 1;
	}
	tbm_end_iterate(&it);

	for (; next < nblocks; next++)
		if (expect(arg, next) != 0)
			elog(ERROR, "bitmap %d: block %u missing", arg, next);

	return nlossy;
}

static void
add_one(TIDBitmap *tbm, BlockNumber blkno, OffsetNumber off)
{
	ItemPointerData tid;

	ItemPointerSet(&tid, blkno, off);
	tbm_add_tuples(tbm, &tid, 1, false);
}

/* Bitmap k holds offset k + 1 on every block */
static uint32
expect_check(int k, BlockNumber blkno)
{
	return UINT32_C(1) << k;
}

/*
 * test_tidbitmap_check(nbitmaps, npages, maxkb, lossy_every, error_at)
 *
 * Builds nbitmaps bitmaps of maxkb kB each at once, adding block p to
 * every bitmap before block p + 1, so they all grow together.  Bitmap k
 * gets offset k + 1 on each block, and every lossy_every'th block whole
 * (tbm_add_page) if lossy_every > 0.  When more than one bitmap is built,
 * the first is checked and freed half-way, and the others go on growing
 * without it.  If error_at >= 0, raises an ERROR after adding block
 * error_at, leaving every bitmap to the memory context reset.  Returns the
 * number of lossy pages seen.
 */
Datum
test_tidbitmap_check(PG_FUNCTION_ARGS)
{
	int			nbitmaps = PG_GETARG_INT32(0);
	int			npages = PG_GETARG_INT32(1);
	int			maxkb = PG_GETARG_INT32(2);
	int			lossy_every = PG_GETARG_INT32(3);
	int			error_at = PG_GETARG_INT32(4);
	int			half = npages / 2;
	TIDBitmap **tbms;
	int64		nlossy = 0;

	if (nbitmaps < 1 || nbitmaps > 32 || npages < 0 || maxkb < 64)
		elog(ERROR, "invalid arguments");

	tbms = palloc_array(TIDBitmap *, nbitmaps);
	for (int k = 0; k < nbitmaps; k++)
		tbms[k] = tbm_create((Size) maxkb * 1024, NULL);

	for (int p = 0; p < npages; p++)
	{
		CHECK_FOR_INTERRUPTS();

		if (p == half && nbitmaps > 1)
		{
			nlossy += verify_bitmap(tbms[0], expect_check, 0, half);
			tbm_free(tbms[0]);
			tbms[0] = NULL;
		}

		for (int k = 0; k < nbitmaps; k++)
		{
			if (tbms[k] == NULL)
				continue;
			if (lossy_every > 0 && p % lossy_every == 0)
				tbm_add_page(tbms[k], p);
			else
				add_one(tbms[k], p, k + 1);
		}

		if (p == error_at)
			elog(ERROR, "error after block %d", p);
	}

	for (int k = 0; k < nbitmaps; k++)
	{
		if (tbms[k] == NULL)
			continue;
		nlossy += verify_bitmap(tbms[k], expect_check, k,
								(k == 0 && nbitmaps > 1) ? half : npages);
		tbm_free(tbms[k]);
	}
	pfree(tbms);

	PG_RETURN_INT64(nlossy);
}

/*
 * For test_tidbitmap_setop: a holds offsets 1 and 2 on blocks [0, n);
 * b holds offsets 2 and 3 on blocks [n / 2, n / 2 + n).
 */
static int	setop_n;

static uint32
expect_a(BlockNumber blkno)
{
	return blkno < setop_n ? 0x3 : 0;
}

static uint32
expect_b(BlockNumber blkno)
{
	return blkno >= setop_n / 2 && blkno < setop_n / 2 + setop_n ? 0x6 : 0;
}

static uint32
expect_union(int arg, BlockNumber blkno)
{
	return expect_a(blkno) | expect_b(blkno);
}

static uint32
expect_intersect(int arg, BlockNumber blkno)
{
	return expect_a(blkno) & expect_b(blkno);
}

/*
 * test_tidbitmap_setop(op, n)
 *
 * Builds two overlapping bitmaps of n blocks, one after the other, and
 * combines them with tbm_union or tbm_intersect, as BitmapOr and BitmapAnd
 * do.  Checks the result and returns its number of blocks.
 */
Datum
test_tidbitmap_setop(PG_FUNCTION_ARGS)
{
	char	   *op = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			n = PG_GETARG_INT32(1);
	TIDBitmap  *a;
	TIDBitmap  *b;
	expect_fn	expect;
	int64		nblocks = 0;

	if (n < 0 || n > MaxBlockNumber / 2)
		elog(ERROR, "invalid arguments");
	setop_n = n;

	a = tbm_create((Size) work_mem * 1024, NULL);
	for (int p = 0; p < n; p++)
	{
		add_one(a, p, 1);
		add_one(a, p, 2);
	}
	b = tbm_create((Size) work_mem * 1024, NULL);
	for (int p = n / 2; p < n / 2 + n; p++)
	{
		add_one(b, p, 2);
		add_one(b, p, 3);
	}

	if (strcmp(op, "union") == 0)
	{
		tbm_union(a, b);
		expect = expect_union;
	}
	else if (strcmp(op, "intersect") == 0)
	{
		tbm_intersect(a, b);
		expect = expect_intersect;
	}
	else
		elog(ERROR, "unknown operation \"%s\"", op);
	tbm_free(b);

	if (verify_bitmap(a, expect, 0, n / 2 + n) != 0)
		elog(ERROR, "unexpected lossy pages");
	tbm_free(a);

	for (BlockNumber p = 0; p < n / 2 + n; p++)
		nblocks += expect(0, p) != 0;

	PG_RETURN_INT64(nblocks);
}
