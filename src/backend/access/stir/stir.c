/*-------------------------------------------------------------------------
 *
 * stir.c
 *	  Implementation of Short-Term Index Replacement.
 *
 * STIR is a specialized access method type designed for temporary storage
 * of TID values during concurrent index build operations.
 *
 * The typical lifecycle of a STIR index is:
 * 1. created as an auxiliary index for CIC/RIC
 * 2. accepts inserts for a period
 * 3. stirbulkdelete called during index validation phase
 * 4. gets dropped
 *
 * STIR indexes are always created as unlogged relations: an auxiliary index
 * has no reason to survive a crash, because a crash aborts the concurrent
 * index build it was serving, leaving the index as an invalid leftover that
 * is only fit to be dropped.  Nothing in this AM writes WAL (the asserts on
 * !RelationNeedsWAL() in page-modifying paths verify that invariant), with
 * one exception: the init-fork metapage written by stirbuildempty().  The
 * init fork is the template the main fork is reset to after a crash, so it
 * must be durable; its metapage disables inserts, so a reset leftover index
 * silently ignores new tuples until it gets dropped.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/stir/stir.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/stir.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "storage/procnumber.h"
#include "utils/catcache.h"
#include "utils/fmgrprotos.h"
#include "utils/index_selfuncs.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

/*
 * Stir handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
stirhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	/* Set STIR-specific strategy and procedure numbers */
	amroutine->amstrategies = STIR_NSTRATEGIES;
	amroutine->amsupport = STIR_NPROC;
	amroutine->amoptsprocnum = STIR_OPTIONS_PROC;

	/* STIR doesn't support most index operations */
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = true;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
	amroutine->amcanbuildparallel = false;
	amroutine->amcaninclude = true;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
	amroutine->amkeytype = InvalidOid;

	/* Set up function callbacks */
	amroutine->ambuild = stirbuild;
	amroutine->ambuildempty = stirbuildempty;
	amroutine->aminsert = stirinsert;
	amroutine->aminsertcleanup = NULL;
	amroutine->ambulkdelete = stirbulkdelete;
	amroutine->amvacuumcleanup = stirvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = stircostestimate;
	amroutine->amoptions = stiroptions;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = stirvalidate;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = stirbeginscan;
	amroutine->amrescan = stirrescan;
	amroutine->amgettuple = NULL;
	amroutine->amgetbitmap = NULL;
	amroutine->amendscan = stirendscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}

/*
 * Validates operator class for STIR index.
 *
 * STIR is not a real index, so validate may be skipped.
 * But we do it just for consistency.
 */
bool
stirvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	HeapTuple	familytup;
	Form_pg_opfamily familyform;
	char	   *opfamilyname;
	CatCList   *oprlist;
	int			i;

	/* Fetch opclass information */
	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;

	/* Fetch opfamily information */
	familytup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfamilyoid));
	if (!HeapTupleIsValid(familytup))
		elog(ERROR, "cache lookup failed for operator family %u", opfamilyoid);
	familyform = (Form_pg_opfamily) GETSTRUCT(familytup);

	opfamilyname = NameStr(familyform->opfname);

	/* Fetch all operators and support functions of the opfamily */
	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));

	/* Check individual operators */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* STIR carries no key data, so any operator is out of place */
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("stir opfamily %s should not contain any operators, but found %s",
						opfamilyname,
						format_operator(oprform->amopopr))));
		result = false;
	}

	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * Initialize meta-page of a STIR index.
 * The skipInserts flag determines if new inserts will be accepted or skipped.
 */
void
StirFillMetapage(Relation index, Page metaPage, bool skipInserts)
{
	StirMetaPageData *metadata;

	StirInitPage(metaPage, STIR_META);
	metadata = StirPageGetMeta(metaPage);
	memset(metadata, 0, sizeof(StirMetaPageData));
	metadata->magicNumber = STIR_MAGIC_NUMBER;
	metadata->skipInserts = skipInserts;
	((PageHeader) metaPage)->pd_lower = ((char *) metadata + sizeof(StirMetaPageData)) - (char *) metaPage;
}

/*
 * Create and initialize the metapage of a STIR index in the main fork.
 * This is called during index creation.
 */
void
StirInitMetapage(Relation index)
{
	Buffer		metaBuffer;
	Page		metaPage;

	Assert(!RelationNeedsWAL(index));

	/*
	 * Make a new page; since it is the first page it should be associated
	 * with block number 0 (STIR_METAPAGE_BLKNO).  No need to hold the
	 * extension lock because there cannot be concurrent inserters yet.
	 */
	metaBuffer = ReadBufferExtended(index, MAIN_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);
	Assert(BufferGetBlockNumber(metaBuffer) == STIR_METAPAGE_BLKNO);

	metaPage = BufferGetPage(metaBuffer);
	StirFillMetapage(index, metaPage, false);

	MarkBufferDirty(metaBuffer);
	UnlockReleaseBuffer(metaBuffer);
}

/*
 * Initialize any page of a stir index.
 */
void
StirInitPage(Page page, uint16 flags)
{
	StirPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(StirPageOpaqueData));

	opaque = StirPageGetOpaque(page);
	opaque->flags = flags;
	opaque->stir_page_id = STIR_PAGE_ID;
}

/*
 * Add a tuple to a STIR page. Returns false if the tuple doesn't fit.
 * The tuple is added to the end of the page.
 */
static bool
StirPageAddItem(Page page, StirTuple *tuple)
{
	StirTuple  *itup;
	StirPageOpaque opaque;
	char	   *ptr;

	/* We shouldn't be pointed to an invalid page */
	Assert(!PageIsNew(page));

	/* Does the new tuple fit on the page? */
	if (StirPageGetFreeSpace(page) < sizeof(StirTuple))
		return false;

	/* Copy a new tuple to the end of the page */
	opaque = StirPageGetOpaque(page);
	itup = StirPageGetTuple(page, opaque->maxoff + 1);
	memcpy(itup, tuple, sizeof(StirTuple));

	/* Adjust maxoff and pd_lower */
	opaque->maxoff++;
	ptr = (char *) StirPageGetTuple(page, opaque->maxoff + 1);
	((PageHeader) page)->pd_lower = ptr - page;

	/* Assert we didn't overrun available space */
	Assert(((PageHeader) page)->pd_lower <= ((PageHeader) page)->pd_upper);
	return true;
}

/*
 * Insert a new tuple into a STIR index.
 *
 * To avoid contention on a single insertion point, the metapage maintains
 * STIR_INSERT_SLOTS independent insertion pages; each backend picks a slot
 * by its ProcNumber.  In addition, the current insertion page of the slot
 * is cached in rd_amcache, so the common path does not touch the metapage
 * at all.
 *
 * A backend with a warm cache may keep inserting into its page for a while
 * after skipInserts has been set: that is fine, the flag is only a safety
 * net for leftover indexes.  The regular way to stop inserts during
 * concurrent index builds is clearing indisready, and that invalidates the
 * relcache entry, which also resets rd_amcache.
 */
bool
stirinsert(Relation index, Datum *values, bool *isnull,
		   ItemPointer ht_ctid, Relation heapRel,
		   IndexUniqueCheck checkUnique,
		   bool indexUnchanged,
		   struct IndexInfo *indexInfo)
{
	StirTuple	itup;
	StirMetaPageData *metaData;
	Buffer		buffer,
				metaBuffer;
	Page		page;
	BlockNumber blkNo;
	BlockNumber triedBlkNo = 0;
	BlockNumber *cachedBlkNo;
	int			myslot;

	itup.heapPtr = *ht_ctid;

	Assert(!RelationNeedsWAL(index));
	Assert(MyProcNumber >= 0);
	myslot = MyProcNumber % STIR_INSERT_SLOTS;

	/* Lazily set up the per-backend cache of our slot's insertion page. */
	if (index->rd_amcache == NULL)
		index->rd_amcache = MemoryContextAllocZero(index->rd_indexcxt,
												   sizeof(BlockNumber));
	cachedBlkNo = (BlockNumber *) index->rd_amcache;

	/* Fast path: try the cached insertion page, bypassing the metapage. */
	if (*cachedBlkNo > 0)
	{
		buffer = ReadBuffer(index, *cachedBlkNo);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

		page = BufferGetPage(buffer);

		Assert(!PageIsNew(page));

		if (StirPageAddItem(page, &itup))
		{
			MarkBufferDirty(buffer);
			UnlockReleaseBuffer(buffer);
			return false;
		}

		UnlockReleaseBuffer(buffer);

		/* The page is full; remember it to not try it again below. */
		triedBlkNo = *cachedBlkNo;
		*cachedBlkNo = 0;
	}

	metaBuffer = ReadBuffer(index, STIR_METAPAGE_BLKNO);

	for (;;)
	{
		LockBuffer(metaBuffer, BUFFER_LOCK_SHARE);
		metaData = StirPageGetMeta(BufferGetPage(metaBuffer));
		/* Check if inserts are allowed */
		if (metaData->skipInserts)
		{
			UnlockReleaseBuffer(metaBuffer);
			return false;
		}
		blkNo = metaData->slotBlkNo[myslot];
		/* Don't hold metabuffer lock while doing insert */
		LockBuffer(metaBuffer, BUFFER_LOCK_UNLOCK);

		if (blkNo > 0 && blkNo != triedBlkNo)
		{
			buffer = ReadBuffer(index, blkNo);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

			page = BufferGetPage(buffer);

			Assert(!PageIsNew(page));

			/* Try to add tuple to the existing page */
			if (StirPageAddItem(page, &itup))
			{
				/* Success!  Apply the change, clean up, and exit */
				MarkBufferDirty(buffer);

				UnlockReleaseBuffer(buffer);
				ReleaseBuffer(metaBuffer);

				*cachedBlkNo = blkNo;
				return false;
			}

			UnlockReleaseBuffer(buffer);

			/* The page is full, remember that */
			triedBlkNo = blkNo;
		}

		/* Need to add a new page - get exclusive lock on meta-page */
		LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);

		metaData = StirPageGetMeta(BufferGetPage(metaBuffer));

		/* Re-check after acquiring exclusive lock */
		if (metaData->skipInserts)
		{
			UnlockReleaseBuffer(metaBuffer);
			return false;
		}

		/* Check if another backend already extended the slot */
		if (blkNo != metaData->slotBlkNo[myslot])
		{
			Assert(blkNo < metaData->slotBlkNo[myslot]);

			/*
			 * Someone else inserted the new page into the slot, let's try
			 * again
			 */
			LockBuffer(metaBuffer, BUFFER_LOCK_UNLOCK);
		}
		else
		{
			/* Must extend the file */
			buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL,
									   EB_LOCK_FIRST);
			page = BufferGetPage(buffer);

			StirInitPage(page, 0);

			if (!StirPageAddItem(page, &itup))
			{
				/* We shouldn't be here since we're inserting to an empty page */
				elog(ERROR, "could not add new stir tuple to empty page");
			}

			/* Update meta-page with the new slot block number */
			metaData->slotBlkNo[myslot] = BufferGetBlockNumber(buffer);
			*cachedBlkNo = metaData->slotBlkNo[myslot];

			MarkBufferDirty(metaBuffer);
			MarkBufferDirty(buffer);

			UnlockReleaseBuffer(buffer);
			UnlockReleaseBuffer(metaBuffer);

			return false;
		}
	}
}

/*
 * STIR doesn't support scans - these functions all error out
 */
IndexScanDesc
stirbeginscan(Relation r, int nkeys, int norderbys)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is not implemented", __func__)));
}

void
stirrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		   ScanKey orderbys, int norderbys)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is not implemented", __func__)));
}

void
stirendscan(IndexScanDesc scan)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is not implemented", __func__)));
}

/*
 * Build a STIR index - not supported so far.
 * Following commits will allow it for auxiliary indexes only.
 */
IndexBuildResult *
stirbuild(Relation heap, Relation index,
		  struct IndexInfo *indexInfo)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Building STIR indexes is not supported")));

	return NULL;				/* keep compiler quiet */
}

/*
 * Build an empty STIR index in the initialization fork.
 *
 * The init fork must be durable: it is what the main fork is reset to after
 * a crash.  Write the metapage through the bulk-write facility, which
 * WAL-logs and syncs init-fork pages regardless of relation persistence.
 * The metapage has skipInserts = true, so the reset leftover index silently
 * ignores inserts (it is only fit to be dropped anyway).
 */
void
stirbuildempty(Relation index)
{
	BulkWriteState *bulkstate;
	BulkWriteBuffer metabuf;

	bulkstate = smgr_bulk_start_rel(index, INIT_FORKNUM);

	metabuf = smgr_bulk_get_buf(bulkstate);
	StirFillMetapage(index, (Page) metabuf, true);
	smgr_bulk_write(bulkstate, STIR_METAPAGE_BLKNO, metabuf, true);

	smgr_bulk_finish(bulkstate);
}

IndexBulkDeleteResult *
stirbulkdelete(IndexVacuumInfo *info,
			   IndexBulkDeleteResult *stats,
			   IndexBulkDeleteCallback callback,
			   void *callback_state)
{
	Relation	index = info->index;
	BlockNumber blkno,
				npages;
	Buffer		buffer;
	Page		page;

	/*
	 * For normal VACUUM, mark to skip inserts and warn about an index drop
	 * needed.  In practice this path is not reachable during CREATE INDEX
	 * CONCURRENTLY because the table-level locks held by CIC prevent
	 * concurrent VACUUM from opening the auxiliary index.  It can only be
	 * reached if a leftover STIR index somehow survives after a failed CIC
	 * and a later VACUUM encounters it.
	 */
	if (!info->validate_index)
	{
		StirMarkAsSkipInserts(index);

		ereport(WARNING, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						  errmsg("\"%s\" is not implemented, seems like this index needs to be dropped", __func__)));
		return NULL;
	}

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/*
	 * Iterate over the pages. We don't care about concurrently added pages,
	 * because the index is marked as not-ready for that moment and the index
	 * is not used for insert.
	 */
	npages = RelationGetNumberOfBlocks(index);
	for (blkno = STIR_HEAD_BLKNO; blkno < npages; blkno++)
	{
		StirTuple  *itup,
				   *itupEnd;

		vacuum_delay_point(false);

		buffer = ReadBufferExtended(index, MAIN_FORKNUM, blkno,
									RBM_NORMAL, info->strategy);

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}

		itup = StirPageGetTuple(page, FirstOffsetNumber);
		itupEnd = StirPageGetTuple(page, OffsetNumberNext(StirPageGetMaxOffset(page)));
		while (itup < itupEnd)
		{
			/* Do we have to delete this tuple? */
			if (callback(&itup->heapPtr, callback_state))
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("we never delete in stir")));
			}

			itup = StirPageGetNextTuple(itup);
		}

		UnlockReleaseBuffer(buffer);
	}

	return stats;
}

/*
 * Mark a STIR index to skip future inserts
 */
void
StirMarkAsSkipInserts(Relation index)
{
	StirMetaPageData *metaData;
	Buffer		metaBuffer;
	Page		metaPage;

	Assert(!RelationNeedsWAL(index));
	metaBuffer = ReadBuffer(index, STIR_METAPAGE_BLKNO);
	LockBuffer(metaBuffer, BUFFER_LOCK_EXCLUSIVE);

	metaPage = BufferGetPage(metaBuffer);
	metaData = StirPageGetMeta(metaPage);

	if (!metaData->skipInserts)
	{
		metaData->skipInserts = true;
		MarkBufferDirty(metaBuffer);
	}
	UnlockReleaseBuffer(metaBuffer);
}

/*
 * As with stirbulkdelete, this is not reachable during a normal CIC due to
 * table-level locking.  It serves as a safety net for leftover STIR indexes
 * from failed concurrent index builds.
 */
IndexBulkDeleteResult *
stirvacuumcleanup(IndexVacuumInfo *info,
				  IndexBulkDeleteResult *stats)
{
	StirMarkAsSkipInserts(info->index);
	ereport(WARNING, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					  errmsg("\"%s\" is not implemented, seems like this index needs to be dropped", __func__)));
	return NULL;
}

bytea *
stiroptions(Datum reloptions, bool validate)
{
	return NULL;
}

void
stircostestimate(PlannerInfo *root, IndexPath *path,
				 double loop_count, Cost *indexStartupCost,
				 Cost *indexTotalCost, Selectivity *indexSelectivity,
				 double *indexCorrelation, double *indexPages)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" is not implemented", __func__)));
}
