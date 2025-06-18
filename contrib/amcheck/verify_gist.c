/*-------------------------------------------------------------------------
 *
 * verify_gist.c
 *		Verifies the integrity of GiST indexes based on invariants.
 *
 * Verification checks that all paths in GiST graph contain
 * consistent keys: tuples on parent pages consistently include tuples
 * from children pages. Also, verification checks graph invariants:
 * internal page must have at least one downlink, internal page can
 * reference either only leaf pages or only internal pages.
 *
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_gist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "access/heaptoast.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "common/pg_prng.h"
#include "lib/bloomfilter.h"
#include "verify_common.h"
#include "utils/memutils.h"


/*
 * GistScanItem represents one item of depth-first scan of GiST index.
 */
typedef struct GistScanItem
{
	int			depth;

	/* Referenced block number to check next */
	BlockNumber blkno;

	/*
	 * Correctness of this parent tuple will be checked against contents of
	 * referenced page. This tuple will be NULL for the root block.
	 */
	IndexTuple	parenttup;

	/*
	 * LSN to handle concurrent scans of the page. It's necessary to avoid
	 * missing some subtrees from the page that was split just before we read it.
	 */
	XLogRecPtr	parentlsn;

	/*
	 * Reference to parent page for re-locking in case of found parent-child
	 * tuple discrepancies.
	 */
	BlockNumber parentblk;

	/* Pointer to the next stack item. */
	struct GistScanItem *next;
}			GistScanItem;

typedef struct GistCheckState
{
	/* GiST state */
	GISTSTATE  *state;
	/* Bloom filter fingerprints index tuples */
	bloom_filter *filter;

	Snapshot	snapshot;
	Relation	rel;
	Relation	heaprel;

	/* Debug counter for reporting percentage of work already done */
	int64		heaptuplespresent;

	/* progress reporting stuff */
	BlockNumber totalblocks;
	BlockNumber reportedblocks;
	BlockNumber scannedblocks;
	BlockNumber deltablocks;

	int			leafdepth;
}			GistCheckState;

PG_FUNCTION_INFO_V1(gist_index_check);

static void giststate_init_heapallindexed(Relation rel, GistCheckState * result);
static void gist_check_parent_keys_consistency(Relation rel, Relation heaprel,
											   void *callback_state, bool readonly);
static void gist_check_page(GistCheckState * check_state, GistScanItem * stack,
							Page page, bool heapallindexed,
							BufferAccessStrategy strategy);
static void check_index_page(Relation rel, Buffer buffer, BlockNumber blockNo);
static IndexTuple gist_refind_parent(Relation rel, BlockNumber parentblkno,
									 BlockNumber childblkno,
									 BufferAccessStrategy strategy);
static ItemId PageGetItemIdCareful(Relation rel, BlockNumber block,
								   Page page, OffsetNumber offset);
static void gist_tuple_present_callback(Relation index, ItemPointer tid,
										Datum *values, bool *isnull,
										bool tupleIsAlive, void *checkstate);
static IndexTuple gistFormNormalizedTuple(GistCheckState *giststate,
										  IndexTuple itup);

/*
 * gist_index_check(index regclass)
 *
 * Verify integrity of GiST index.
 *
 * Acquires AccessShareLock on heap & index relations.
 */
Datum
gist_index_check(PG_FUNCTION_ARGS)
{
	Oid			indrelid = PG_GETARG_OID(0);
	bool		heapallindexed = PG_GETARG_BOOL(1);

	amcheck_lock_relation_and_check(indrelid,
									GIST_AM_OID,
									gist_check_parent_keys_consistency,
									AccessShareLock,
									&heapallindexed);

	PG_RETURN_VOID();
}

/*
 * Initialize GIST state files needed to perform.
 * This initialized bloom filter and snapshot.
 */
static void
giststate_init_heapallindexed(Relation rel, GistCheckState * result)
{
	int64		total_pages;
	int64		total_elems;
	uint64		seed;

	/*
	 * Size Bloom filter based on estimated number of tuples in index. This
	 * logic is similar to B-tree, see verify_btree.c .
	 */
	total_pages = result->totalblocks;
	total_elems = Max(total_pages * (MaxOffsetNumber / 5),
					  (int64) rel->rd_rel->reltuples);
	seed = pg_prng_uint64(&pg_global_prng_state);
	result->filter = bloom_create(total_elems, maintenance_work_mem, seed);

	result->snapshot = RegisterSnapshot(GetTransactionSnapshot());


	/*
	 * GetTransactionSnapshot() always acquires a new MVCC snapshot in READ
	 * COMMITTED mode.  A new snapshot is guaranteed to have all the entries
	 * it requires in the index.
	 *
	 * We must defend against the possibility that an old xact snapshot was
	 * returned at higher isolation levels when that snapshot is not safe for
	 * index scans of the target index.  This is possible when the snapshot
	 * sees tuples that are before the index's indcheckxmin horizon.  Throwing
	 * an error here should be very rare.  It doesn't seem worth using a
	 * secondary snapshot to avoid this.
	 */
	if (IsolationUsesXactSnapshot() && rel->rd_index->indcheckxmin &&
		!TransactionIdPrecedes(HeapTupleHeaderGetXmin(rel->rd_indextuple->t_data),
							   result->snapshot->xmin))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("index \"%s\" cannot be verified using transaction snapshot",
						RelationGetRelationName(rel))));
}

/*
 * Main entry point for GiST check.
 *
 * This function verifies that tuples of internal pages cover all
 * the key space of each tuple on the leaf page. To do this we invoke
 * gist_check_internal_page() for every internal page.
 *
 * This check allocates memory context and scans through
 * GiST graph. This scan is performed in a depth-first search using a stack of
 * GistScanItem-s. Initially this stack contains only the root block number. On
 * each iteration the top block number is replaced by referenced block numbers.
 *
 *
 * gist_check_internal_page() in its turn takes every tuple and tries to
 * adjust it by tuples on the referenced child page. Parent gist tuple should
 * never require any adjustments.
 */
static void
gist_check_parent_keys_consistency(Relation rel, Relation heaprel,
								   void *callback_state, bool readonly)
{
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	GistScanItem *stack;
	MemoryContext mctx;
	MemoryContext oldcontext;
	GISTSTATE  *state;
	bool		heapallindexed = *((bool *) callback_state);
	GistCheckState *check_state = palloc0(sizeof(GistCheckState));

	mctx = AllocSetContextCreate(CurrentMemoryContext,
								 "amcheck context",
								 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(mctx);

	state = initGISTstate(rel);

	check_state->state = state;
	check_state->rel = rel;
	check_state->heaprel = heaprel;

	/*
	 * We don't know the height of the tree yet, but as soon as we encounter a
	 * leaf page, we will set 'leafdepth' to its depth.
	 */
	check_state->leafdepth = -1;

	check_state->totalblocks = RelationGetNumberOfBlocks(rel);
	/* report every 100 blocks or 5%, whichever is bigger */
	check_state->deltablocks = Max(check_state->totalblocks / 20, 100);

	if (heapallindexed)
		giststate_init_heapallindexed(rel, check_state);

	/* Start the scan at the root page */
	stack = (GistScanItem *) palloc0(sizeof(GistScanItem));
	stack->depth = 0;
	stack->parenttup = NULL;
	stack->parentblk = InvalidBlockNumber;
	stack->parentlsn = InvalidXLogRecPtr;
	stack->blkno = GIST_ROOT_BLKNO;

	/*
	 * This GiST scan is effectively an "old" VACUUM version before commit
	 * fe280694d which introduced physical order scanning.
	 */

	while (stack)
	{
		GistScanItem *stack_next;
		Buffer		buffer;
		Page		page;
		XLogRecPtr	lsn;

		CHECK_FOR_INTERRUPTS();

		/* Report progress */
		if (check_state->scannedblocks > check_state->reportedblocks +
			check_state->deltablocks)
		{
			elog(DEBUG1, "verified level %u blocks of approximately %u total",
				 check_state->scannedblocks, check_state->totalblocks);
			check_state->reportedblocks = check_state->scannedblocks;
		}
		check_state->scannedblocks++;

		buffer = ReadBufferExtended(rel, MAIN_FORKNUM, stack->blkno,
									RBM_NORMAL, strategy);
		LockBuffer(buffer, GIST_SHARE);
		page = (Page) BufferGetPage(buffer);
		lsn = BufferGetLSNAtomic(buffer);

		/* Do basic sanity checks on the page headers */
		check_index_page(rel, buffer, stack->blkno);

		/*
		 * It's possible that the page was split since we looked at the
		 * parent, so that we didn't miss the downlink of the right sibling
		 * when we scanned the parent.  If so, add the right sibling to the
		 * stack now.
		 */
		if (GistFollowRight(page) || stack->parentlsn < GistPageGetNSN(page))
		{
			/* split page detected, install right link to the stack */
			GistScanItem *ptr = (GistScanItem *) palloc(sizeof(GistScanItem));

			ptr->depth = stack->depth;
			ptr->parenttup = CopyIndexTuple(stack->parenttup);
			ptr->parentblk = stack->parentblk;
			ptr->parentlsn = stack->parentlsn;
			ptr->blkno = GistPageGetOpaque(page)->rightlink;
			ptr->next = stack->next;
			stack->next = ptr;
		}

		gist_check_page(check_state, stack, page, heapallindexed, strategy);

		if (!GistPageIsLeaf(page))
		{
			OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

			for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				/* Internal page, so recurse to the child */
				GistScanItem *ptr;
				ItemId		iid = PageGetItemIdCareful(rel, stack->blkno, page, i);
				IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);

				ptr = (GistScanItem *) palloc(sizeof(GistScanItem));
				ptr->depth = stack->depth + 1;
				ptr->parenttup = CopyIndexTuple(idxtuple);
				ptr->parentblk = stack->blkno;
				ptr->blkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));
				ptr->parentlsn = lsn;
				ptr->next = stack->next;
				stack->next = ptr;
			}
		}

		LockBuffer(buffer, GIST_UNLOCK);
		ReleaseBuffer(buffer);

		/* Step to next item in the queue */
		stack_next = stack->next;
		if (stack->parenttup)
			pfree(stack->parenttup);
		pfree(stack);
		stack = stack_next;
	}

	if (heapallindexed)
	{
		IndexInfo  *indexinfo = BuildIndexInfo(rel);
		TableScanDesc scan;

		scan = table_beginscan_strat(heaprel,	/* relation */
									 check_state->snapshot, /* snapshot */
									 0, /* number of keys */
									 NULL,	/* scan key */
									 true,	/* buffer access strategy OK */
									 true); /* syncscan OK? */

		/*
		 * Scan will behave as the first scan of a CREATE INDEX CONCURRENTLY.
		 */
		indexinfo->ii_Concurrent = true;

		indexinfo->ii_Unique = false;
		indexinfo->ii_ExclusionOps = NULL;
		indexinfo->ii_ExclusionProcs = NULL;
		indexinfo->ii_ExclusionStrats = NULL;

		elog(DEBUG1, "verifying that tuples from index \"%s\" are present in \"%s\"",
			 RelationGetRelationName(rel),
			 RelationGetRelationName(heaprel));

		table_index_build_scan(heaprel, rel, indexinfo, true, false,
							   gist_tuple_present_callback, (void *) check_state, scan);

		ereport(DEBUG1,
				(errmsg_internal("finished verifying presence of " INT64_FORMAT " tuples from table \"%s\" with bitset %.2f%% set",
								 check_state->heaptuplespresent,
								 RelationGetRelationName(heaprel),
								 100.0 * bloom_prop_bits_set(check_state->filter))));

		UnregisterSnapshot(check_state->snapshot);
		bloom_free(check_state->filter);
	}

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(mctx);
	pfree(check_state);
}

static void
gist_check_page(GistCheckState * check_state, GistScanItem * stack,
				Page page, bool heapallindexed, BufferAccessStrategy strategy)
{
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	/* Check that the tree has the same height in all branches */
	if (GistPageIsLeaf(page))
	{
		if (check_state->leafdepth == -1)
			check_state->leafdepth = stack->depth;
		else if (stack->depth != check_state->leafdepth)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\": internal pages traversal encountered leaf page unexpectedly on block %u",
							RelationGetRelationName(check_state->rel), stack->blkno)));
	}

	/*
	 * Check that each tuple looks valid, and is consistent with the downlink
	 * we followed when we stepped on this page.
	 */
	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		ItemId		iid = PageGetItemIdCareful(check_state->rel, stack->blkno, page, i);
		IndexTuple	idxtuple = (IndexTuple) PageGetItem(page, iid);
		IndexTuple  tmpTuple = NULL;

		/*
		 * Check that it's not a leftover invalid tuple from pre-9.1 See also
		 * gistdoinsert() and gistbulkdelete() handling of such tuples. We do
		 * consider it an error here.
		 */
		if (GistTupleIsInvalid(idxtuple))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("index \"%s\" contains an inner tuple marked as invalid, block %u, offset %u",
							RelationGetRelationName(check_state->rel), stack->blkno, i),
					 errdetail("This is caused by an incomplete page split at crash recovery before upgrading to PostgreSQL 9.1."),
					 errhint("Please REINDEX it.")));

		if (MAXALIGN(ItemIdGetLength(iid)) != MAXALIGN(IndexTupleSize(idxtuple)))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has inconsistent tuple sizes, block %u, offset %u",
							RelationGetRelationName(check_state->rel), stack->blkno, i)));

		/*
		 * Check if this tuple is consistent with the downlink in the parent.
		 */
		if (stack->parenttup)
			tmpTuple = gistgetadjusted(check_state->rel, stack->parenttup, idxtuple, check_state->state);

		if (tmpTuple)
		{
			/*
			 * There was a discrepancy between parent and child tuples. We
			 * need to verify it is not a result of concurrent call of
			 * gistplacetopage(). So, lock parent and try to find a downlink for
			 * current page. It may be missing due to concurrent page split,
			 * this is OK.
			 *
			 * Note that when we acquire parent tuple now we hold lock for both
			 * parent and child buffers. Thus the parent tuple must include the
			 * keyspace of the child.
			 */

			pfree(tmpTuple);
			pfree(stack->parenttup);
			stack->parenttup = gist_refind_parent(check_state->rel, stack->parentblk,
												  stack->blkno, strategy);

			/* We found it - make a final check before failing */
			if (!stack->parenttup)
				elog(NOTICE, "Unable to find parent tuple for block %u on block %u due to concurrent split",
					 stack->blkno, stack->parentblk);
			else if (gistgetadjusted(check_state->rel, stack->parenttup, idxtuple, check_state->state))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" has inconsistent records on page %u offset %u",
								RelationGetRelationName(check_state->rel), stack->blkno, i)));
			else
			{
				/*
				 * But now it is properly adjusted - nothing to do here.
				 */
			}
		}

		if (GistPageIsLeaf(page))
		{
			if (heapallindexed)
			{
				IndexTuple norm;

				norm = gistFormNormalizedTuple(check_state, idxtuple);

				bloom_add_element(check_state->filter,
								  (unsigned char *) norm,
								  IndexTupleSize(norm));

				/* Be tidy */
				if (norm != idxtuple)
					pfree(norm);
			}
		}
		else
		{
			OffsetNumber off = ItemPointerGetOffsetNumber(&(idxtuple->t_tid));

			if (off != TUPLE_IS_VALID)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" has on page %u offset %u has item id not pointing to 0xffff, but %hu",
								RelationGetRelationName(check_state->rel), stack->blkno, i, off)));
		}
	}
}

/*
 * gistFormNormalizedTuple - analogue to gistFormTuple, but performs deTOASTing
 * of all included data (for covering indexes). While we do not expect
 * toasted attributes in normal indexes, this can happen as a result of
 * intervention into system catalog. Detoasting of key attributes is expected
 * to be done by opclass decompression methods, if the indexed type might be
 * toasted.
 */
static IndexTuple
gistFormNormalizedTuple(GistCheckState *giststate,
						IndexTuple itup)
{
	return amcheck_normalize_tuple(giststate->rel, itup);
}

static void
gist_tuple_present_callback(Relation index, ItemPointer tid, Datum *values,
							bool *isnull, bool tupleIsAlive, void *checkstate)
{
	GistCheckState *state = (GistCheckState *) checkstate;
	IndexTuple itup, norm;
	Datum		compatt[INDEX_MAX_KEYS];

	/* Generate a normalized index tuple for fingerprinting */
	gistCompressValues(state->state, index, values, isnull, true, compatt);
	itup = index_form_tuple(RelationGetDescr(index), compatt, isnull);
	itup->t_tid = *tid;

	norm = gistFormNormalizedTuple(state, itup);

	/* Probe Bloom filter -- tuple should be present */
	if (bloom_lacks_element(state->filter, (unsigned char *) norm,
							IndexTupleSize(norm)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("heap tuple (%u,%u) from table \"%s\" lacks matching index tuple within index \"%s\"",
						ItemPointerGetBlockNumber(&(norm->t_tid)),
						ItemPointerGetOffsetNumber(&(norm->t_tid)),
						RelationGetRelationName(state->heaprel),
						RelationGetRelationName(state->rel))));

	state->heaptuplespresent++;

	pfree(itup);
	/* Be tidy */
	if (norm != itup)
		pfree(norm);
}

/*
 * check_index_page - verification of basic invariants about GiST page data
 * This function does not do any tuple analysis.
 */
static void
check_index_page(Relation rel, Buffer buffer, BlockNumber blockNo)
{
	Page		page = BufferGetPage(buffer);

	gistcheckpage(rel, buffer);

	if (GistPageGetOpaque(page)->gist_page_id != GIST_PAGE_ID)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has corrupted page %u",
						RelationGetRelationName(rel), blockNo)));

	if (GistPageIsDeleted(page))
	{
		if (!GistPageIsLeaf(page))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has deleted internal page %u",
							RelationGetRelationName(rel), blockNo)));
		if (PageGetMaxOffsetNumber(page) > InvalidOffsetNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has deleted page %u with tuples",
							RelationGetRelationName(rel), blockNo)));
	}
	else if (PageGetMaxOffsetNumber(page) > MaxIndexTuplesPerPage)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has page %u with exceeding count of tuples",
						RelationGetRelationName(rel), blockNo)));
}

/*
 * Try to re-find the downlink pointing to 'blkno', in 'parentblkno'.
 *
 * If found, returns a palloc'd copy of the downlink tuple. Otherwise,
 * returns NULL.
 */
static IndexTuple
gist_refind_parent(Relation rel,
				   BlockNumber parentblkno, BlockNumber childblkno,
				   BufferAccessStrategy strategy)
{
	Buffer		parentbuf;
	Page		parentpage;
	OffsetNumber parent_maxoff,
						off;
	IndexTuple	result = NULL;

	parentbuf = ReadBufferExtended(rel, MAIN_FORKNUM, parentblkno, RBM_NORMAL,
								   strategy);

	LockBuffer(parentbuf, GIST_SHARE);
	parentpage = BufferGetPage(parentbuf);

	if (GistPageIsLeaf(parentpage))
	{
		/*
		 * Currently GiST never deletes internal pages, thus they can never
		 * become leaf.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" internal page %u became leaf",
						RelationGetRelationName(rel), parentblkno)));
	}

	parent_maxoff = PageGetMaxOffsetNumber(parentpage);
	for (off = FirstOffsetNumber; off <= parent_maxoff; off = OffsetNumberNext(off))
	{
		ItemId		p_iid = PageGetItemIdCareful(rel, parentblkno, parentpage, off);
		IndexTuple	itup = (IndexTuple) PageGetItem(parentpage, p_iid);

		if (ItemPointerGetBlockNumber(&(itup->t_tid)) == childblkno)
		{
			/*
			 * Found it! Make a copy and return it while both parent and child
			 * pages are locked. This guarantees that at this particular
			 * moment tuples must be coherent to each other.
			 */
			result = CopyIndexTuple(itup);
			break;
		}
	}

	UnlockReleaseBuffer(parentbuf);

	return result;
}

static ItemId
PageGetItemIdCareful(Relation rel, BlockNumber block, Page page,
					 OffsetNumber offset)
{
	ItemId		itemid = PageGetItemId(page, offset);

	if (ItemIdGetOffset(itemid) + ItemIdGetLength(itemid) >
		BLCKSZ - MAXALIGN(sizeof(GISTPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("line pointer points past end of tuple space in index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	/*
	 * Verify that the line pointer isn't LP_REDIRECT or LP_UNUSED, since gist
	 * never uses either. Verify that the line pointer has storage, too, since
	 * even LP_DEAD items should.
	 */
	if (ItemIdIsRedirected(itemid) || !ItemIdIsUsed(itemid) ||
		ItemIdGetLength(itemid) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("invalid line pointer storage in index \"%s\"",
						RelationGetRelationName(rel)),
				 errdetail_internal("Index tid=(%u,%u) lp_off=%u, lp_len=%u lp_flags=%u.",
									block, offset, ItemIdGetOffset(itemid),
									ItemIdGetLength(itemid),
									ItemIdGetFlags(itemid))));

	return itemid;
}
