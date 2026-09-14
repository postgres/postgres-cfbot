/*-------------------------------------------------------------------------
 *
 * test_deletemark.c
 *		Synthetic exerciser for nbtree delete-marking.
 *
 * No in-tree table AM enables delete-marking yet (it is dormant until a
 * later phase flips it on for FLUX/ZHEAP).  This module drives the
 * nbtree delete-marking mechanism directly against an ordinary btree index
 * so the mechanism itself (classification, heap-TID retrieval, scan recheck,
 * uniqueness handling, WAL/redo) can be tested end to end.
 *
 * SQL-callable primitives:
 *   dm_mark(regclass idx, tid t)          -> mark the (single) entry with TID t
 *   dm_classify(regclass idx, tid t)      -> text: classification of entry at t
 *   dm_heaptid(regclass idx, tid t)       -> tid: heap TID recovered from entry
 *   dm_scan_recheck(regclass idx)         -> bool: any returned tuple set recheck
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_deletemark/test_deletemark.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

/*
 * Locate the single leaf entry whose heap TID equals 'target' by a linear
 * scan over all leaf pages.  Returns the buffer (pinned+share-locked) and the
 * offset, or InvalidBuffer if not found.  Intended for tiny test indexes.
 */
static Buffer
find_leaf_entry(Relation irel, ItemPointer target, OffsetNumber *offout)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(irel);
	BlockNumber blk;

	for (blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf = ReadBuffer(irel, blk);
		Page		page;
		BTPageOpaque opaque;
		OffsetNumber off,
					minoff,
					maxoff;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);

		if (!P_ISLEAF(opaque) || P_IGNORE(opaque))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		minoff = P_FIRSTDATAKEY(opaque);
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = minoff; off <= maxoff; off = OffsetNumberNext(off))
		{
			IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
			ItemPointer htid = BTreeTupleGetHeapTID(itup);

			if (htid != NULL && ItemPointerIsValid(htid) &&
				ItemPointerEquals(htid, target))
			{
				*offout = off;
				return buf;		/* caller unlocks/releases */
			}
		}
		UnlockReleaseBuffer(buf);
	}
	return InvalidBuffer;
}

PG_FUNCTION_INFO_V1(dm_classify);
Datum
dm_classify(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	ItemPointer target = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	irel = index_open(idxoid, AccessShareLock);
	OffsetNumber off;
	Buffer		buf;
	const char *res;

	buf = find_leaf_entry(irel, target, &off);
	if (buf == InvalidBuffer)
	{
		index_close(irel, AccessShareLock);
		PG_RETURN_TEXT_P(cstring_to_text("not found"));
	}
	{
		Page		page = BufferGetPage(buf);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		if (BTreeTupleIsDeleteMarked(itup))
		{
			/* Must NOT also classify as pivot or posting */
			if (BTreeTupleIsPivot(itup) || BTreeTupleIsPosting(itup))
				res = "BROKEN: delete-marked also pivot/posting";
			else
				res = "delete-marked";
		}
		else if (BTreeTupleIsPivot(itup))
			res = "pivot";
		else if (BTreeTupleIsPosting(itup))
			res = "posting";
		else
			res = "plain";

		UnlockReleaseBuffer(buf);
	}
	index_close(irel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(res));
}

PG_FUNCTION_INFO_V1(dm_heaptid);
Datum
dm_heaptid(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	ItemPointer target = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	irel = index_open(idxoid, AccessShareLock);
	OffsetNumber off;
	Buffer		buf;
	ItemPointer result;

	buf = find_leaf_entry(irel, target, &off);
	if (buf == InvalidBuffer)
	{
		index_close(irel, AccessShareLock);
		PG_RETURN_NULL();
	}
	result = palloc(sizeof(ItemPointerData));
	{
		Page		page = BufferGetPage(buf);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));

		ItemPointerCopy(BTreeTupleGetHeapTID(itup), result);
		UnlockReleaseBuffer(buf);
	}
	index_close(irel, AccessShareLock);
	PG_RETURN_POINTER(result);
}

/*
 * dm_mark(idx, heaptid, values...) is awkward to pass key datums through SQL,
 * so instead we mark the entry located purely by heap TID: read the existing
 * plain entry's key from the leaf, then call btdeletemark with those datums.
 * This exercises the full re-descend-by-(key,TID) path in btdeletemark.
 */
PG_FUNCTION_INFO_V1(dm_mark);
Datum
dm_mark(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	ItemPointer target = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	irel = index_open(idxoid, RowExclusiveLock);
	Relation	heapRel;
	Oid			heapoid;
	OffsetNumber off;
	Buffer		buf;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		ok;

	heapoid = irel->rd_index->indrelid;
	heapRel = relation_open(heapoid, RowExclusiveLock);

	buf = find_leaf_entry(irel, target, &off);
	if (buf == InvalidBuffer)
	{
		relation_close(heapRel, RowExclusiveLock);
		index_close(irel, RowExclusiveLock);
		PG_RETURN_BOOL(false);
	}
	{
		Page		page = BufferGetPage(buf);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		IndexTuple	copy;

		/* copy the tuple out before unlocking to deform its key */
		copy = CopyIndexTuple(itup);
		UnlockReleaseBuffer(buf);

		index_deform_tuple(copy, RelationGetDescr(irel), values, isnull);
		pfree(copy);
	}

	/* Direct call to the AM method (bypasses the dormant capability gate) */
	ok = irel->rd_indam->amdeletemark(irel, heapRel, values, isnull, target);

	relation_close(heapRel, RowExclusiveLock);
	index_close(irel, RowExclusiveLock);
	PG_RETURN_BOOL(ok);
}

/*
 * dm_scan_recheck(idx) -- run a full index scan and report whether ANY
 * returned tuple carried scan->xs_recheck (which delete-marked entries set).
 */
PG_FUNCTION_INFO_V1(dm_scan_recheck);
Datum
dm_scan_recheck(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	Relation	irel = index_open(idxoid, AccessShareLock);
	Relation	heapRel = relation_open(irel->rd_index->indrelid, AccessShareLock);
	IndexScanDesc scan;
	bool		any_recheck = false;

	/*
	 * Post-ddce1da5b1b (slot-based table-AM index scan): index_getnext_tid is
	 * gone and index_beginscan takes an index_only_scan flag.  We only want
	 * to observe scan->xs_recheck (set by delete-marked entries in
	 * nbtsearch.c), not fetch heap tuples, so drive the index AM's amgettuple
	 * directly -- exactly what tableam_index_getnext_tid does internally.
	 */
	scan = index_beginscan(heapRel, irel, false, GetActiveSnapshot(),
						   NULL, 0, 0, SO_NONE);
	index_rescan(scan, NULL, 0, NULL, 0);
	while (scan->indexRelation->rd_indam->amgettuple(scan, ForwardScanDirection))
	{
		scan->kill_prior_tuple = false;
		if (scan->xs_recheck)
			any_recheck = true;
	}
	index_endscan(scan);

	relation_close(heapRel, AccessShareLock);
	index_close(irel, AccessShareLock);
	PG_RETURN_BOOL(any_recheck);
}

/*
 * dm_set_dead(idx, heaptid) -- set the LP_DEAD hint on the leaf entry for
 * heaptid, the way _bt_killitems does.  Lets a test build the state where a
 * plain entry or a tombstone carries LP_DEAD.
 */
PG_FUNCTION_INFO_V1(dm_set_dead);
Datum
dm_set_dead(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	ItemPointer target = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	irel = index_open(idxoid, RowExclusiveLock);
	OffsetNumber off;
	Buffer		buf;
	Page		page;

	buf = find_leaf_entry(irel, target, &off);
	if (buf == InvalidBuffer)
	{
		index_close(irel, RowExclusiveLock);
		PG_RETURN_BOOL(false);
	}
	/* Tiny single-session test indexes: the entry cannot move meanwhile. */
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);
	ItemIdMarkDead(PageGetItemId(page, off));
	BTPageGetOpaque(page)->btpo_flags |= BTP_HAS_GARBAGE;
	MarkBufferDirtyHint(buf, true);
	UnlockReleaseBuffer(buf);
	index_close(irel, RowExclusiveLock);
	PG_RETURN_BOOL(true);
}

/* dm_is_dead(idx, heaptid) -- does the leaf entry for heaptid carry LP_DEAD? */
PG_FUNCTION_INFO_V1(dm_is_dead);
Datum
dm_is_dead(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	ItemPointer target = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	irel = index_open(idxoid, AccessShareLock);
	OffsetNumber off;
	Buffer		buf;
	bool		dead;

	buf = find_leaf_entry(irel, target, &off);
	if (buf == InvalidBuffer)
	{
		index_close(irel, AccessShareLock);
		PG_RETURN_NULL();
	}
	dead = ItemIdIsDead(PageGetItemId(BufferGetPage(buf), off));
	UnlockReleaseBuffer(buf);
	index_close(irel, AccessShareLock);
	PG_RETURN_BOOL(dead);
}

/*
 * dm_unmark(idx, heaptid) -- demote the tombstone for heaptid back to a plain
 * entry through amundomark(clear_mark = true), as a rollback does.
 */
PG_FUNCTION_INFO_V1(dm_unmark);
Datum
dm_unmark(PG_FUNCTION_ARGS)
{
	Oid			idxoid = PG_GETARG_OID(0);
	ItemPointer target = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	irel = index_open(idxoid, RowExclusiveLock);
	Relation	heapRel;
	OffsetNumber off;
	Buffer		buf;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	IndexTuple	copy;
	bool		ok;

	heapRel = relation_open(irel->rd_index->indrelid, RowExclusiveLock);
	buf = find_leaf_entry(irel, target, &off);
	if (buf == InvalidBuffer)
	{
		relation_close(heapRel, RowExclusiveLock);
		index_close(irel, RowExclusiveLock);
		PG_RETURN_BOOL(false);
	}
	copy = CopyIndexTuple((IndexTuple) PageGetItem(BufferGetPage(buf),
												   PageGetItemId(BufferGetPage(buf), off)));
	UnlockReleaseBuffer(buf);
	index_deform_tuple(copy, RelationGetDescr(irel), values, isnull);

	ok = irel->rd_indam->amundomark(irel, heapRel, values, isnull, target,
									true, false);

	relation_close(heapRel, RowExclusiveLock);
	index_close(irel, RowExclusiveLock);
	PG_RETURN_BOOL(ok);
}
