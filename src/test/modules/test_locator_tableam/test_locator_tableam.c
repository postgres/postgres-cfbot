/*-------------------------------------------------------------------------
 *
 * test_locator_tableam.c
 *		Table access methods for testing the locator contract.
 *
 * Both AMs store rows exactly as heap does and differ from it only in the
 * locator properties they declare; see amlocator.h.
 *
 * heap_noretain says its pre-update row versions are not fetchable by
 * locator, as an AM that updates rows in place would.  To make that claim
 * true, a SnapshotAny fetch follows the update chain to the row's current
 * version, the newest one not written by an aborted transaction, which is
 * what such an AM would hand back.  The old version is still on the page, so heap's own
 * bookkeeping is unaffected, but any caller that relies on fetching it after
 * the UPDATE gets the new row instead.
 *
 * heap_wide declares a locator too wide for an ItemPointerData, and
 * heap_bigoffset one whose offsets exceed what a TID bitmap can hold.  Their
 * TOAST tables are plain heap, so that their indexes are not refused.
 *
 * heap_orrecheck declares a bitmap_or_inexact callback that flags every
 * group, so a BitmapOr over it must recheck every block it returns.
 *
 * heap_getnext() refuses any relation whose AM routine is not heap's, and
 * heap's index build scan uses it, so both AMs present heap's routine for the
 * duration of that scan.  CREATE INDEX CONCURRENTLY is not supported.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/test_locator_tableam/test_locator_tableam.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amlocator.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "fmgr.h"
#include "storage/bufmgr.h"
#include "utils/snapmgr.h"

PG_MODULE_MAGIC;

static TableAmRoutine noretain_methods;
static TableAmRoutine wide_methods;
static TableAmRoutine bigoffset_methods;
static TableAmRoutine orrecheck_methods;

static const LocatorDesc noretain_locator = {
	.width = sizeof(ItemPointerData),
	.max_offset = MaxHeapTuplesPerPage,
	.name = "tid",
	.stable = false,
	.old_version_retained = false,
};

static const LocatorDesc wide_locator = {
	.width = 16,
	.max_offset = MaxHeapTuplesPerPage,
	.name = "wide",
	.stable = false,
	.old_version_retained = true,
};

static bool
orrecheck_bitmap_or_inexact(Relation rel, uint64 group, Buffer *cache)
{
	return true;
}

static const LocatorDesc orrecheck_locator = {
	.width = sizeof(ItemPointerData),
	.max_offset = MaxHeapTuplesPerPage,
	.name = "tid",
	.stable = false,
	.old_version_retained = true,
	.bitmap_or_inexact = orrecheck_bitmap_or_inexact,
};

static const LocatorDesc bigoffset_locator = {
	.width = sizeof(ItemPointerData),
	.max_offset = MaxOffsetNumber,
	.name = "bigoffset",
	.stable = false,
	.old_version_retained = true,
};

static const LocatorDesc *
noretain_relation_locator(Relation rel)
{
	return &noretain_locator;
}

static const LocatorDesc *
wide_relation_locator(Relation rel)
{
	return &wide_locator;
}

static const LocatorDesc *
orrecheck_relation_locator(Relation rel)
{
	return &orrecheck_locator;
}

static const LocatorDesc *
bigoffset_relation_locator(Relation rel)
{
	return &bigoffset_locator;
}

static Oid
heap_relation_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

/*
 * Follow the update chain from *tid, stopping at a version that was not
 * updated, or whose updater aborted.
 */
static void
noretain_latest_version(Relation rel, ItemPointer tid)
{
	TransactionId prior_xmax = InvalidTransactionId;
	ItemPointerData cur = *tid;

	for (;;)
	{
		Buffer		buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&cur));
		Page		page;
		ItemId		lp;
		HeapTupleHeader htup = NULL;
		bool		follow = false;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (ItemPointerGetOffsetNumber(&cur) <= PageGetMaxOffsetNumber(page))
		{
			lp = PageGetItemId(page, ItemPointerGetOffsetNumber(&cur));
			if (ItemIdIsNormal(lp))
				htup = (HeapTupleHeader) PageGetItem(page, lp);
		}
		if (htup != NULL &&
			(!TransactionIdIsValid(prior_xmax) ||
			 TransactionIdEquals(HeapTupleHeaderGetXmin(htup), prior_xmax)))
		{
			*tid = cur;
			prior_xmax = HeapTupleHeaderGetUpdateXid(htup);
			follow = !(htup->t_infomask & HEAP_XMAX_INVALID) &&
				!HEAP_XMAX_IS_LOCKED_ONLY(htup->t_infomask) &&
				!HeapTupleHeaderIndicatesMovedPartitions(htup) &&
				!ItemPointerEquals(&htup->t_ctid, &cur) &&
				!TransactionIdDidAbort(prior_xmax);
			cur = htup->t_ctid;
		}
		UnlockReleaseBuffer(buf);
		if (!follow)
			return;
	}
}

static bool
noretain_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot,
						   TupleTableSlot *slot)
{
	ItemPointerData latest = *tid;

	if (snapshot == SnapshotAny)
		noretain_latest_version(rel, &latest);

	if (!GetHeapamTableAmRoutine()->tuple_fetch_row_version(rel, &latest,
															snapshot, slot))
		return false;

	/* Callers expect the slot to describe the locator they asked for. */
	slot->tts_tid = *tid;
	return true;
}

static double
as_heap_index_build_range_scan(Relation table_rel, Relation index_rel,
							   IndexInfo *index_info, bool allow_sync,
							   bool anyvisible, bool progress,
							   BlockNumber start_blockno,
							   BlockNumber numblocks,
							   IndexBuildCallback callback,
							   void *callback_state, TableScanDesc scan)
{
	const TableAmRoutine *saved = table_rel->rd_tableam;
	volatile double result = 0;

	/* Cache our own locator before anything can ask heap for one. */
	(void) RelationGetLocatorDesc(table_rel);

	table_rel->rd_tableam = GetHeapamTableAmRoutine();
	PG_TRY();
	{
		result = table_rel->rd_tableam->index_build_range_scan(table_rel,
															   index_rel,
															   index_info,
															   allow_sync,
															   anyvisible,
															   progress,
															   start_blockno,
															   numblocks,
															   callback,
															   callback_state,
															   scan);
	}
	PG_FINALLY();
	{
		table_rel->rd_tableam = saved;
	}
	PG_END_TRY();

	return result;
}

PG_FUNCTION_INFO_V1(heap_noretain_handler);
Datum
heap_noretain_handler(PG_FUNCTION_ARGS)
{
	if (noretain_methods.type == 0)
	{
		noretain_methods = *GetHeapamTableAmRoutine();
		noretain_methods.relation_locator = noretain_relation_locator;
		noretain_methods.tuple_fetch_row_version = noretain_fetch_row_version;
		noretain_methods.index_build_range_scan = as_heap_index_build_range_scan;
	}
	PG_RETURN_POINTER(&noretain_methods);
}

PG_FUNCTION_INFO_V1(heap_wide_handler);
Datum
heap_wide_handler(PG_FUNCTION_ARGS)
{
	if (wide_methods.type == 0)
	{
		wide_methods = *GetHeapamTableAmRoutine();
		wide_methods.relation_locator = wide_relation_locator;
		wide_methods.index_build_range_scan = as_heap_index_build_range_scan;
		wide_methods.relation_toast_am = heap_relation_toast_am;
	}
	PG_RETURN_POINTER(&wide_methods);
}

PG_FUNCTION_INFO_V1(heap_bigoffset_handler);
Datum
heap_bigoffset_handler(PG_FUNCTION_ARGS)
{
	if (bigoffset_methods.type == 0)
	{
		bigoffset_methods = *GetHeapamTableAmRoutine();
		bigoffset_methods.relation_locator = bigoffset_relation_locator;
		bigoffset_methods.index_build_range_scan = as_heap_index_build_range_scan;
		bigoffset_methods.relation_toast_am = heap_relation_toast_am;
	}
	PG_RETURN_POINTER(&bigoffset_methods);
}

PG_FUNCTION_INFO_V1(heap_orrecheck_handler);
Datum
heap_orrecheck_handler(PG_FUNCTION_ARGS)
{
	if (orrecheck_methods.type == 0)
	{
		orrecheck_methods = *GetHeapamTableAmRoutine();
		orrecheck_methods.relation_locator = orrecheck_relation_locator;
		orrecheck_methods.index_build_range_scan = as_heap_index_build_range_scan;
		orrecheck_methods.relation_toast_am = heap_relation_toast_am;
	}
	PG_RETURN_POINTER(&orrecheck_methods);
}
