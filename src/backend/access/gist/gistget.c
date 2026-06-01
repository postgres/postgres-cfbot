/*-------------------------------------------------------------------------
 *
 * gistget.c
 *	  fetch tuples from a GiST scan.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistget.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/relscan.h"
#include "executor/instrument_node.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * gistkillitemsbatch() -- Mark dead items' index tuples LP_DEAD
 */
void
gistkillitemsbatch(IndexScanDesc scan, IndexScanBatch batch)
{
	GISTBatchData *gbatch = GISTBatchGetData(scan, batch);
	Relation	rel = scan->indexRelation;
	Buffer		buf;
	Page		page;
	bool		killedsomething = false;
	XLogRecPtr	latestlsn;

	Assert(batch->numDead > 0);

	/*
	 * Skip virtual (ordered-scan) batches, since there's no practical way to
	 * visit all of the index pages that these tuples really came from
	 */
	if (gbatch->blkno == InvalidBlockNumber)
		return;

	buf = ReadBuffer(rel, gbatch->blkno);
	LockBuffer(buf, GIST_SHARE);
	gistcheckpage(rel, buf);
	page = BufferGetPage(buf);

	latestlsn = BufferGetLSNAtomic(buf);
	Assert(batch->lsn <= latestlsn);
	if (batch->lsn != latestlsn)
	{
		/* Modified, give up on hinting */
		UnlockReleaseBuffer(buf);
		return;
	}

	/* Iterate through batch->deadItems[] in index page order */
	for (int i = 0; i < batch->numDead; i++)
	{
		int			itemIndex = batch->deadItems[i];
		OffsetNumber offnum = batch->items[itemIndex].indexOffset;
		ItemId		iid = PageGetItemId(page, offnum);

		Assert(itemIndex >= batch->firstItem && itemIndex <= batch->lastItem);
		Assert(i == 0 ||
			   offnum > batch->items[batch->deadItems[i - 1]].indexOffset);
		Assert(offnum <= PageGetMaxOffsetNumber(page));
		Assert(ItemPointerEquals(&((IndexTuple) PageGetItem(page, iid))->t_tid,
								 &batch->items[itemIndex].tableTid));

		/* Mark index item as dead, if it isn't already */
		if (!ItemIdIsDead(iid))
		{
			if (!killedsomething)
			{
				/*
				 * Use the hint bit infrastructure to check if we can update
				 * the page while just holding a share lock. If we are not
				 * allowed, there's no point continuing.
				 */
				if (!BufferBeginSetHintBits(buf))
				{
					UnlockReleaseBuffer(buf);
					return;
				}
			}

			ItemIdMarkDead(iid);
			killedsomething = true;
		}
	}

	if (killedsomething)
	{
		GistMarkPageHasGarbage(page);
		BufferFinishSetHintBits(buf, true, true);
	}

	UnlockReleaseBuffer(buf);
}

/*
 * gistindex_keytest() -- does this index tuple satisfy the scan key(s)?
 *
 * The index tuple might represent either a heap tuple or a lower index page,
 * depending on whether the containing page is a leaf page or not.
 *
 * On success return for a heap tuple, *recheck_p is set to indicate whether
 * the quals need to be rechecked.  We recheck if any of the consistent()
 * functions request it.  recheck is not interesting when examining a non-leaf
 * entry, since we must visit the lower index page if there's any doubt.
 * Similarly, *recheck_distances_p is set to indicate whether the distances
 * need to be rechecked, and it is also ignored for non-leaf entries.
 *
 * If we are doing an ordered scan, so->distances[] is filled with distance
 * data from the distance() functions before returning success.
 *
 * We must decompress the key in the IndexTuple before passing it to the
 * sk_funcs (which actually are the opclass Consistent or Distance methods).
 *
 * Note that this function is always invoked in a short-lived memory context,
 * so we don't need to worry about cleaning up allocated memory, either here
 * or in the implementation of any Consistent or Distance methods.
 */
static bool
gistindex_keytest(IndexScanDesc scan,
				  IndexTuple tuple,
				  Page page,
				  OffsetNumber offset,
				  bool *recheck_p,
				  bool *recheck_distances_p)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	GISTSTATE  *giststate = so->giststate;
	ScanKey		key = scan->keyData;
	int			keySize = scan->numberOfKeys;
	IndexOrderByDistance *distance_p;
	Relation	r = scan->indexRelation;

	*recheck_p = false;
	*recheck_distances_p = false;

	/*
	 * If it's a leftover invalid tuple from pre-9.1, treat it as a match with
	 * minimum possible distances.  This means we'll always follow it to the
	 * referenced page.
	 */
	if (GistTupleIsInvalid(tuple))
	{
		int			i;

		if (GistPageIsLeaf(page))	/* shouldn't happen */
			elog(ERROR, "invalid GiST tuple found on leaf page");
		for (i = 0; i < scan->numberOfOrderBys; i++)
		{
			so->distances[i].value = -get_float8_infinity();
			so->distances[i].isnull = false;
		}
		return true;
	}

	/* Check whether it matches according to the Consistent functions */
	while (keySize > 0)
	{
		Datum		datum;
		bool		isNull;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  giststate->leafTupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/*
			 * On non-leaf page we can't conclude that child hasn't NULL
			 * values because of assumption in GiST: union (VAL, NULL) is VAL.
			 * But if on non-leaf page key IS NULL, then all children are
			 * NULL.
			 */
			if (key->sk_flags & SK_SEARCHNULL)
			{
				if (GistPageIsLeaf(page) && !isNull)
					return false;
			}
			else
			{
				Assert(key->sk_flags & SK_SEARCHNOTNULL);
				if (isNull)
					return false;
			}
		}
		else if (isNull)
		{
			return false;
		}
		else
		{
			Datum		test;
			bool		recheck;
			GISTENTRY	de;

			gistdentryinit(giststate, key->sk_attno - 1, &de,
						   datum, r, page, offset,
						   false, isNull);

			/*
			 * Call the Consistent function to evaluate the test.  The
			 * arguments are the index datum (as a GISTENTRY*), the comparison
			 * datum, the comparison operator's strategy number and subtype
			 * from pg_amop, and the recheck flag.
			 *
			 * (Presently there's no need to pass the subtype since it'll
			 * always be zero, but might as well pass it for possible future
			 * use.)
			 *
			 * We initialize the recheck flag to true (the safest assumption)
			 * in case the Consistent function forgets to set it.
			 */
			recheck = true;

			test = FunctionCall5Coll(&key->sk_func,
									 key->sk_collation,
									 PointerGetDatum(&de),
									 key->sk_argument,
									 UInt16GetDatum(key->sk_strategy),
									 ObjectIdGetDatum(key->sk_subtype),
									 PointerGetDatum(&recheck));

			if (!DatumGetBool(test))
				return false;
			*recheck_p |= recheck;
		}

		key++;
		keySize--;
	}

	/* OK, it passes --- now let's compute the distances */
	key = scan->orderByData;
	distance_p = so->distances;
	keySize = scan->numberOfOrderBys;
	while (keySize > 0)
	{
		Datum		datum;
		bool		isNull;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  giststate->leafTupdesc,
							  &isNull);

		if ((key->sk_flags & SK_ISNULL) || isNull)
		{
			/* Assume distance computes as null */
			distance_p->value = 0.0;
			distance_p->isnull = true;
		}
		else
		{
			Datum		dist;
			bool		recheck;
			GISTENTRY	de;

			gistdentryinit(giststate, key->sk_attno - 1, &de,
						   datum, r, page, offset,
						   false, isNull);

			/*
			 * Call the Distance function to evaluate the distance.  The
			 * arguments are the index datum (as a GISTENTRY*), the comparison
			 * datum, the ordering operator's strategy number and subtype from
			 * pg_amop, and the recheck flag.
			 *
			 * (Presently there's no need to pass the subtype since it'll
			 * always be zero, but might as well pass it for possible future
			 * use.)
			 *
			 * If the function sets the recheck flag, the returned distance is
			 * a lower bound on the true distance and needs to be rechecked.
			 * We initialize the flag to 'false'.  This flag was added in
			 * version 9.5; distance functions written before that won't know
			 * about the flag, but are expected to never be lossy.
			 */
			recheck = false;
			dist = FunctionCall5Coll(&key->sk_func,
									 key->sk_collation,
									 PointerGetDatum(&de),
									 key->sk_argument,
									 UInt16GetDatum(key->sk_strategy),
									 ObjectIdGetDatum(key->sk_subtype),
									 PointerGetDatum(&recheck));
			*recheck_distances_p |= recheck;
			distance_p->value = DatumGetFloat8(dist);
			distance_p->isnull = false;
		}

		key++;
		distance_p++;
		keySize--;
	}

	return true;
}

/*
 * Scan all items on the GiST index page identified by *pageItem, and insert
 * them into the queue (or directly to output areas)
 *
 * scan: index scan we are executing
 * pageItem: search queue item identifying an index page to scan
 * myDistances: distances array associated with pageItem, or NULL at the root
 * newbatch: caller's batch to fill, for a non-ordered scan; NULL when ordered
 *
 * For a non-ordered scan (newbatch isn't NULL, which is the case for both
 * unordered gistgetbatch and gistgetbitmap), matching item TIDs from a leaf
 * page are stored into caller's newbatch to return via gistgetbatch.  If we
 * don't save any items in newbatch, caller needs to find the next leaf page
 * that has matches and save its items in newbatch instead (if there is none
 * then caller should release newbatch).
 *
 * For an ordered (nearest-neighbor) scan (newbatch is NULL), matching leaf heap
 * tuples are pushed onto the search queue as GISTSearchItems carrying their
 * distances, so the queue can later be drained in distance order.  The page's
 * buffer pin is dropped before returning.  This can only happen during
 * batchImmediateUnguard scans, which is what makes it safe.  Groups of enqueued
 * items will eventually be returned (in the expected order) as "virtual
 * batches", but we don't do that here.
 *
 * In all cases, lower index pages are pushed onto the search queue to be
 * visited later.
 *
 * If we detect that the index page has split since we saw its downlink
 * in the parent, we push its new right sibling onto the queue so the
 * sibling will be processed next.
 */
static void
gistScanPage(IndexScanDesc scan, GISTSearchItem *pageItem,
			 IndexOrderByDistance *myDistances, IndexScanBatch newbatch)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	Relation	r = scan->indexRelation;
	Buffer		buffer;
	Page		page;
	GISTPageOpaque opaque;
	OffsetNumber maxoff;
	OffsetNumber i;
	MemoryContext oldcxt;

	/* state used when saving matching items into caller's newbatch */
	int			itemIndex = 0;
	int			tupleOffset = 0;
	bool	   *recheckarr = NULL;

	Assert(!GISTSearchItemIsHeap(*pageItem));
	Assert((scan->numberOfOrderBys == 0) == (newbatch != NULL));

	if (newbatch)
		recheckarr = GISTBatchGetRecheck(scan, newbatch);

	buffer = ReadBuffer(scan->indexRelation, pageItem->blkno);
	LockBuffer(buffer, GIST_SHARE);
	PredicateLockPage(r, BufferGetBlockNumber(buffer), scan->xs_snapshot);
	gistcheckpage(scan->indexRelation, buffer);
	page = BufferGetPage(buffer);
	opaque = GistPageGetOpaque(page);

	/*
	 * Check if we need to follow the rightlink. We need to follow it if the
	 * page was concurrently split since we visited the parent (in which case
	 * parentlsn < nsn), or if the system crashed after a page split but
	 * before the downlink was inserted into the parent.
	 */
	if (XLogRecPtrIsValid(pageItem->data.parentlsn) &&
		(GistFollowRight(page) ||
		 pageItem->data.parentlsn < GistPageGetNSN(page)) &&
		opaque->rightlink != InvalidBlockNumber /* sanity check */ )
	{
		/* There was a page split, follow right link to add pages */
		GISTSearchItem *item;

		/* This can't happen when starting at the root */
		Assert(myDistances != NULL);

		oldcxt = MemoryContextSwitchTo(so->queueCxt);

		/* Create new GISTSearchItem for the right sibling index page */
		item = palloc(SizeOfGISTSearchItem(scan->numberOfOrderBys));
		item->blkno = opaque->rightlink;
		item->data.parentlsn = pageItem->data.parentlsn;

		/* Insert it into the queue using same distances as for this page */
		memcpy(item->distances, myDistances,
			   sizeof(item->distances[0]) * scan->numberOfOrderBys);

		pairingheap_add(so->queue, &item->phNode);

		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * Check if the page was deleted after we saw the downlink. There's
	 * nothing of interest on a deleted page. Note that we must do this after
	 * checking the NSN for concurrent splits! It's possible that the page
	 * originally contained some tuples that are visible to us, but was split
	 * so that all the visible tuples were moved to another page, and then
	 * this page was deleted.
	 */
	if (GistPageIsDeleted(page))
	{
		Assert(!newbatch || newbatch->firstItem > newbatch->lastItem);
		UnlockReleaseBuffer(buffer);
		return;
	}

	/*
	 * check all tuples on page
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		ItemId		iid = PageGetItemId(page, i);
		IndexTuple	it;
		bool		match;
		bool		recheck;
		bool		recheck_distances;

		/*
		 * If the scan specifies not to return killed tuples, then we treat a
		 * killed tuple as not passing the qual.
		 */
		if (scan->ignore_killed_tuples && ItemIdIsDead(iid))
		{
			Assert(GistPageIsLeaf(page));
			continue;
		}

		it = (IndexTuple) PageGetItem(page, iid);

		/*
		 * Must call gistindex_keytest in tempCxt, and clean up any leftover
		 * junk afterward.
		 */
		oldcxt = MemoryContextSwitchTo(so->giststate->tempCxt);

		match = gistindex_keytest(scan, it, page, i,
								  &recheck, &recheck_distances);

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(so->giststate->tempCxt);

		/* Ignore tuple if it doesn't match */
		if (!match)
			continue;

		if (scan->numberOfOrderBys == 0 && GistPageIsLeaf(page))
		{
			/*
			 * Non-ordered scan (unordered amgetbatch or bitmap), so just
			 * store another matching item in caller's batch without worrying
			 * about ordering
			 */
			newbatch->items[itemIndex].tableTid = it->t_tid;
			newbatch->items[itemIndex].indexOffset = i;
			newbatch->items[itemIndex].tupleOffset = 0;
			recheckarr[itemIndex] = recheck;

			if (scan->xs_want_itup)
			{
				/* Copy on-disk format index tuple into currTuples */
				Size		itupsz = IndexTupleSize(it);

				newbatch->items[itemIndex].tupleOffset = tupleOffset;
				memcpy(newbatch->currTuples + tupleOffset, it, itupsz);
				tupleOffset += MAXALIGN(itupsz);
			}
			itemIndex++;
		}
		else
		{
			/*
			 * Must push item into search queue.  We get here for any lower
			 * index page, and also for heap tuples if doing an ordered
			 * search.
			 */
			GISTSearchItem *item;
			int			nOrderBys = scan->numberOfOrderBys;

			oldcxt = MemoryContextSwitchTo(so->queueCxt);

			/* Create new GISTSearchItem for this item */
			item = palloc(SizeOfGISTSearchItem(scan->numberOfOrderBys));

			if (GistPageIsLeaf(page))
			{
				/* Creating heap-tuple GISTSearchItem for ordered search */
				Assert(scan->numberOfOrderBys > 0);
				Assert(newbatch == NULL);
				Assert(scan->batchImmediateUnguard);

				item->blkno = InvalidBlockNumber;
				item->data.heap.heapPtr = it->t_tid;
				item->data.heap.recheck = recheck;
				item->data.heap.recheckDistances = recheck_distances;
			}
			else
			{
				/* Creating index-page GISTSearchItem */
				item->blkno = ItemPointerGetBlockNumber(&it->t_tid);

				/*
				 * LSN of current page is lsn of parent page for child. We
				 * only have a shared lock, so we need to get the LSN
				 * atomically.
				 */
				item->data.parentlsn = BufferGetLSNAtomic(buffer);
			}

			/* Insert it into the queue using new distance data */
			memcpy(item->distances, so->distances,
				   sizeof(item->distances[0]) * nOrderBys);

			pairingheap_add(so->queue, &item->phNode);

			MemoryContextSwitchTo(oldcxt);
		}
	}

	if (newbatch)
	{
		/* Finalize result batch during a non-ordered scan */
		Assert(scan->numberOfOrderBys == 0);

		newbatch->firstItem = 0;
		newbatch->lastItem = itemIndex - 1;

		if (itemIndex > 0)
		{
			GISTBatchData *gnewbatch;

			Assert(GistPageIsLeaf(page));

			gnewbatch = GISTBatchGetData(scan, newbatch);
			gnewbatch->buf = buffer;
			gnewbatch->blkno = BufferGetBlockNumber(buffer);

			indexam_util_unlock_batch(scan, newbatch, buffer);
			return;
		}
		/* else caller needs to find another page to fill newbatch */
	}

	UnlockReleaseBuffer(buffer);
}

/*
 * Extract next item (in order) from search queue
 *
 * Returns a GISTSearchItem or NULL.  Caller must pfree item when done with it.
 */
static GISTSearchItem *
getNextGISTSearchItem(GISTScanOpaque so)
{
	GISTSearchItem *item;

	if (!pairingheap_is_empty(so->queue))
	{
		item = (GISTSearchItem *) pairingheap_remove_first(so->queue);
	}
	else
	{
		/* Done when both heaps are empty */
		item = NULL;
	}

	/* Return item; caller is responsible to pfree it */
	return item;
}

/*
 * gistScanStart() -- begin a scan by queueing its root page
 *
 * Called on the first amgetbatch/amgetbitmap call of a scan (the caller having
 * already checked that the qual is satisfiable).  Counts the scan for stats and
 * queues the root page as the first work item, so the scan drivers are
 * otherwise pure queue drainers.  The root carries a zeroed parentlsn (it has
 * no parent, so gistScanPage's split-detection is a no-op for it) and zeroed
 * distances (so it sorts first in an ordered scan).
 *
 * Starting the scan here, rather than in gistrescan, follows the convention
 * that amrescan only sets up scan keys while the scan proper (counting it,
 * reading index pages) begins on the first fetch.
 */
static void
gistScanStart(IndexScanDesc scan)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	GISTSearchItem *root;
	MemoryContext oldcxt;

	pgstat_count_index_scan(scan->indexRelation);
	if (scan->instrument)
		scan->instrument->nsearches++;

	oldcxt = MemoryContextSwitchTo(so->queueCxt);
	root = palloc(SizeOfGISTSearchItem(scan->numberOfOrderBys));
	root->blkno = GIST_ROOT_BLKNO;
	memset(&root->data.parentlsn, 0, sizeof(GistNSN));
	memset(root->distances, 0,
		   sizeof(root->distances[0]) * scan->numberOfOrderBys);
	pairingheap_add(so->queue, &root->phNode);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * getNextBatch() -- read the next leaf page with matches into a fresh batch
 *
 * gistgetbatch's non-ordered walker, also driven by gistgetbitmap.  Allocates a
 * batch and drains the queue, scanning each queued index page until one
 * produces matching leaf items, then returns that batch.  When the queue is
 * exhausted without a match, releases the batch and returns NULL.
 */
static IndexScanBatch
getNextBatch(IndexScanDesc scan)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	IndexScanBatch newbatch = indexam_util_alloc_batch(scan);

	/* GiST only ever scans forward; set the batch's direction up front */
	newbatch->dir = ForwardScanDirection;

	for (;;)
	{
		GISTSearchItem *item = getNextGISTSearchItem(so);

		if (item == NULL)
		{
			/* No more index pages to scan; the scan is exhausted */
			indexam_util_release_batch(scan, newbatch);
			return NULL;
		}

		CHECK_FOR_INTERRUPTS();

		/* Scan this queued index page; matching leaf items go into the batch */
		gistScanPage(scan, item, item->distances, newbatch);
		pfree(item);

		/* If this leaf page produced matching items, return the batch */
		if (newbatch->firstItem <= newbatch->lastItem)
			return newbatch;
	}

	pg_unreachable();

	return NULL;
}

/*
 * getNextNearestBatch() -- drain the queue into a fresh batch in distance order
 *
 * gistgetbatch's ordered (nearest-neighbor) walker.  The pairing-heap queue
 * (so->queue) holds both unvisited index pages and matching leaf heap tuples,
 * ordered by (lower-bound) distance.  We pop items in that order, dispatching
 * on the item type.  A popped heap tuple is appended to the batch.  We stop
 * once the batch is full (maxitemsbatch items) or the queue is exhausted,
 * leaving any remaining items queued for the next call.
 *
 * Because the queue is drained in nondecreasing distance order across the whole
 * scan (a downlink's distance is a lower bound on its subtree, so items pushed
 * while scanning a page never sort ahead of items already popped), the
 * batches we emit are globally distance-ordered.
 */
static IndexScanBatch
getNextNearestBatch(IndexScanDesc scan)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	IndexScanBatch newbatch = indexam_util_alloc_batch(scan);
	GISTBatchData *gnewbatch;
	int			nitems = 0;

	/* GiST only ever scans forward; set the batch's direction up front */
	newbatch->dir = ForwardScanDirection;

	for (;;)
	{
		GISTSearchItem *item = getNextGISTSearchItem(so);

		if (!item)
			break;

		if (GISTSearchItemIsHeap(*item))
		{
			/* found a heap item at currently minimal distance */
			GISTBatchItem *bitem = GISTBatchGetItem(scan, newbatch, nitems);

			newbatch->items[nitems].tableTid = item->data.heap.heapPtr;
			newbatch->items[nitems].indexOffset = -1;	/* meaningless here */
			newbatch->items[nitems].tupleOffset = 0;

			bitem->recheck = item->data.heap.recheck;
			bitem->recheckDistances = item->data.heap.recheckDistances;
			memcpy(bitem->distances, item->distances,
				   sizeof(item->distances[0]) * scan->numberOfOrderBys);

			nitems++;
			pfree(item);

			if (nitems == scan->maxitemsbatch)
				break;			/* batch full; remaining items stay queued */
		}
		else
		{
			/* visit an index page, extract its items into queue */
			CHECK_FOR_INTERRUPTS();

			gistScanPage(scan, item, item->distances, NULL);
			pfree(item);
		}
	}

	if (nitems == 0)
	{
		/* No matching items remain: the scan is exhausted */
		indexam_util_release_batch(scan, newbatch);
		return NULL;
	}

	/*
	 * An ordered batch is "virtual": its items come from many leaf pages,
	 * whose pins gistScanPage already dropped, so it holds no TID recycling
	 * interlock.  It has no single originating page, and we don't track those
	 * index pages in any case (gistkillitemsbatch will just skip it).
	 */
	Assert(!newbatch->isGuarded);

	newbatch->firstItem = 0;
	newbatch->lastItem = nitems - 1;

	gnewbatch = GISTBatchGetData(scan, newbatch);
	gnewbatch->buf = InvalidBuffer;
	gnewbatch->blkno = InvalidBlockNumber;

	return newbatch;
}

/*
 * gistgetbatch() -- Get the first or next batch of items in a scan
 *
 * Dispatches to the ordered or non-ordered walker.  Persistent traversal state
 * lives in so->queue, so priorbatch is unused except to recognize the scan's
 * first call, when we queue the root page (gistScanStart).
 */
IndexScanBatch
gistgetbatch(IndexScanDesc scan, IndexScanBatch priorbatch, ScanDirection dir)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	IndexScanBatch batch;

	if (dir != ForwardScanDirection)
		elog(ERROR, "GiST only supports forward scan direction");

	if (!so->qual_ok)
		return NULL;

	if (priorbatch == NULL)
		gistScanStart(scan);

	if (scan->numberOfOrderBys > 0)
		batch = getNextNearestBatch(scan);
	else
		batch = getNextBatch(scan);

	/*
	 * When the search queue was left empty, the scan already ended on the
	 * returned batch; mark the batch accordingly
	 */
	if (batch && pairingheap_is_empty(so->queue))
		batch->knownEndForward = true;

	return batch;
}

/*
 * gistunguardbatch() -- Drop a batch's TID recycling interlock (buffer pin)
 *
 * Called by the table AM when it's safe to drop the buffer pin held to
 * prevent concurrent TID recycling by VACUUM.
 */
void
gistunguardbatch(IndexScanDesc scan, IndexScanBatch batch)
{
	GISTBatchData *gbatch = GISTBatchGetData(scan, batch);

	/* Should be called exactly once iff !batchImmediateUnguard */
	Assert(!scan->batchImmediateUnguard);
	Assert(batch->isGuarded);

	ReleaseBuffer(gbatch->buf);
}

/*
 * gistgettransform() -- Set up the scan's per-tuple output for one batch item
 *
 * Implements the amgettransform interface.  The table AM calls this as it
 * returns each item of a GiST scan, to set the scan descriptor's per-tuple
 * output from the item's per-item data.
 *
 *   - We always apply the item's qual recheck flag to scan->xs_recheck.
 *   - For ordered scans, we report the item's own ORDER BY distances (stored in
 *     the per-item index AM area by getNextNearestBatch) as xs_orderbyvals.
 *     They are flagged for recheck only when the distance function was lossy
 *     for that item; an exact distance is reported as final, while a lossy
 *     lower bound is rechecked by the executor's reorder queue to recompute
 *     the true order.
 *   - For index-only scans, we reconstruct the originally indexed values from
 *     the stored on-disk index tuple into a heap tuple, exposed as xs_hitup.
 *
 * The reconstructed tuple lives in the scan's memory context and only needs to
 * outlive a single table_index_getnext_slot call (the executor copies it into
 * the scan slot).  We free the previously returned tuple before building the
 * next one.
 */
void
gistgettransform(IndexScanDesc scan, IndexScanBatch batch, int item)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;

	Assert(item >= batch->firstItem && item <= batch->lastItem);

	/* Ordered scan (must be a plain index scan) */
	if (scan->numberOfOrderBys > 0)
	{
		GISTBatchItem *bitem = GISTBatchGetItem(scan, batch, item);

		Assert(!scan->xs_want_itup);

		/* Apply this item's qual recheck flag */
		scan->xs_recheck = bitem->recheck;

		/*
		 * Note: This is a "virtual" batch.  The items from caller's batch
		 * were stored in the batch in distance order by getNextNearestBatch,
		 * right before gistgetbatch returned it.
		 */
		Assert(GISTBatchGetData(scan, batch)->blkno == InvalidBlockNumber);
		index_store_float8_orderby_distances(scan, so->orderByTypes,
											 bitem->distances,
											 bitem->recheckDistances);
		return;
	}

	/*
	 * Unordered scan.
	 *
	 * Always uses simple bool array for item recheck flags.
	 */
	scan->xs_recheck = GISTBatchGetRecheck(scan, batch)[item];

	/* Index-only scan */
	if (scan->xs_want_itup)
	{
		/* Reconstruct a returnable heap tuple from stashed index tuple */
		IndexTuple	itup = (IndexTuple) (batch->currTuples +
										 batch->items[item].tupleOffset);
		MemoryContext oldcxt;

		if (scan->xs_hitup)
		{
			pfree(scan->xs_hitup);
			scan->xs_hitup = NULL;
		}

		/* reconstruct the originally indexed values as a heap tuple */
		oldcxt = MemoryContextSwitchTo(so->giststate->scanCxt);
		scan->xs_hitup = gistFetchTuple(so->giststate, scan->indexRelation, itup);
		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * gistgetbitmap() -- Get a bitmap of all heap tuple locations
 */
int64
gistgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	int64		ntids = 0;
	IndexScanBatch batch;

	if (!so->qual_ok)
		return 0;

	/* Begin the scan by queueing the root page */
	gistScanStart(scan);

	/*
	 * Drive the same non-ordered walker as gistgetbatch, one leaf page at a
	 * time, draining each batch into the bitmap and releasing it before
	 * fetching the next, so only one batch is ever live (cf. spggetbitmap).
	 */
	while ((batch = getNextBatch(scan)) != NULL)
	{
		bool	   *recheck = GISTBatchGetRecheck(scan, batch);

		for (int i = batch->firstItem; i <= batch->lastItem; i++)
		{
			tbm_add_tuples(tbm, &batch->items[i].tableTid, 1, recheck[i]);
			ntids++;
		}

		/*
		 * Return the batch to the single-slot bitmap cache, to be reused by
		 * the next getNextBatch
		 */
		indexam_util_release_batch(scan, batch);
	}

	return ntids;
}

/*
 * Can we do index-only scans on the given index column?
 *
 * Opclasses that implement a fetch function support index-only scans.
 * Opclasses without compression functions also support index-only scans.
 * Included attributes always can be fetched for index-only scans.
 */
bool
gistcanreturn(Relation index, int attno)
{
	if (attno > IndexRelationGetNumberOfKeyAttributes(index) ||
		OidIsValid(index_getprocid(index, attno, GIST_FETCH_PROC)) ||
		!OidIsValid(index_getprocid(index, attno, GIST_COMPRESS_PROC)))
		return true;
	else
		return false;
}
