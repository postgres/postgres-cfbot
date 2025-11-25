/*-------------------------------------------------------------------------
 *
 * hashsearch.c
 *	  search code for postgres hash tables
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashsearch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/batchscan.h"
#include "access/hash.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "executor/instrument_node.h"
#include "pgstat.h"
#include "storage/predicate.h"
#include "utils/rel.h"

static IndexScanBatch _hash_readfirstpage(IndexScanDesc scan,
										  IndexScanBatch firstbatch, Buffer buf,
										  ScanDirection dir);
static IndexScanBatch _hash_readnextpage(IndexScanDesc scan, BlockNumber blkno,
										 ScanDirection dir, bool bucSplit);
static Buffer _hash_step_to_split_bucket(IndexScanDesc scan);
static Buffer _hash_step_to_populated_bucket(IndexScanDesc scan);
static Buffer _hash_chain_end(IndexScanDesc scan, Buffer buf);
static bool _hash_readpage(IndexScanDesc scan, Buffer buf, ScanDirection dir,
						   IndexScanBatch batch, bool bucSplit);
static inline void _hash_saveitem(IndexScanBatch batch, int itemIndex,
								  OffsetNumber offnum, IndexTuple itup);

/*
 *	_hash_next() -- Get the next batch of items in a scan.
 *
 *		On entry, priorbatch describes the current page batch with items
 *		already returned.
 *
 *		On successful exit, returns a batch containing matching items from
 *		the next page that has any.  Otherwise returns NULL, indicating that
 *		there are no further matches.  No locks are ever held when we return.
 *
 *		Retains pins according to the same rules as _hash_first.
 */
IndexScanBatch
_hash_next(IndexScanDesc scan, ScanDirection dir, IndexScanBatch priorbatch)
{
	HashBatchData *hashpriorbatch = HashBatchGetData(scan, priorbatch);
	BlockNumber blkno;
	bool		bucSplit;

	/*
	 * The core code must deal with cross-batch scan direction changes for us.
	 * A batch management routine that flips priorbatch's scan direction is
	 * used for this.
	 */
	Assert(priorbatch->dir == dir);

	/* Step from priorbatch's page to its neighbor in this scan direction */
	if (ScanDirectionIsForward(dir))
		blkno = hashpriorbatch->nextPage;
	else
		blkno = hashpriorbatch->prevPage;
	bucSplit = hashpriorbatch->bucSplit;

	/*
	 * For bitmap scan callers, release the prior batch now so that
	 * _hash_readnextpage can reuse its memory.  That way bitmap scans never
	 * need more than one batch allocation.
	 */
	if (!scan->usebatchring)
		batchscan_release(scan, priorbatch);

	return _hash_readnextpage(scan, blkno, dir, bucSplit);
}

/*
 *	_hash_readfirstpage() -- Read the first page of a scan, for _hash_first.
 *
 *		firstbatch is the batch allocated for the first page's matches.  buf
 *		is the primary page of the bucket that the scan key maps to,
 *		share-locked and pinned for us (scan maintains its own separate pin).
 *		A forward scan reads it first.  A backward scan reads bucket chains
 *		tail to head, so it starts at the last page of the chain; if it
 *		started during a bucket split, it reads the bucket being split first,
 *		so it starts at the end of that bucket's chain instead.
 *
 *		Returns a batch containing matching items, or NULL at the end of the
 *		scan.  No locks are ever held when we return.
 */
static IndexScanBatch
_hash_readfirstpage(IndexScanDesc scan, IndexScanBatch firstbatch, Buffer buf,
					ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	HashBatchData *hashfirstbatch;
	BlockNumber blkno;
	bool		bucSplit = false;

	if (ScanDirectionIsBackward(dir))
	{
		if (so->hashso_buc_populated)
		{
			_hash_relbuf(rel, buf);
			buf = _hash_step_to_split_bucket(scan);
			bucSplit = true;
		}
		buf = _hash_chain_end(scan, buf);
	}

	if (_hash_readpage(scan, buf, dir, firstbatch, bucSplit))
	{
		/* _hash_readpage saved one or more matches in firstbatch.items[] */
		batchscan_unlock(scan, firstbatch, buf);
		return firstbatch;
	}

	/*
	 * No matching items on the first page.  Go on from its neighbor in the
	 * scan direction, after releasing the page and firstbatch
	 * (_hash_readnextpage will recycle the batch).
	 */
	_hash_relbuf(rel, buf);
	hashfirstbatch = HashBatchGetData(scan, firstbatch);
	if (ScanDirectionIsForward(dir))
		blkno = hashfirstbatch->nextPage;
	else
		blkno = hashfirstbatch->prevPage;
	batchscan_release(scan, firstbatch);

	return _hash_readnextpage(scan, blkno, dir, bucSplit);
}

/*
 *	_hash_readnextpage() -- Read the next page with matching items.
 *
 *		blkno is the next page in the scan direction, or InvalidBlockNumber
 *		when the page the scan is leaving has no neighbor in that direction.
 *		bucSplit says whether that page is in the bucket being split, which
 *		only matters when the scan started during a split: such a scan reads
 *		both buckets of the split, so at the end of a chain it crosses to the
 *		other bucket, a forward scan from the end of the bucket being
 *		populated to the start of the bucket being split, a backward scan
 *		the other way around.  We read pages in the scan direction until one
 *		has a matching item, or until the scan runs out of pages.
 *
 *		On entry, no page is locked.  Returns a batch containing matching
 *		items, or NULL at the end of the scan.  No locks are ever held when
 *		we return.
 */
static IndexScanBatch
_hash_readnextpage(IndexScanDesc scan, BlockNumber blkno, ScanDirection dir,
				   bool bucSplit)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	IndexScanBatch newbatch;
	HashBatchData *hashnewbatch;
	Buffer		buf;

	/* only a scan that started during a split can be in either bucket */
	Assert(!bucSplit || so->hashso_buc_populated);

	/* Allocate space for new batch before locking anything */
	newbatch = batchscan_alloc(scan);
	hashnewbatch = HashBatchGetData(scan, newbatch);

	for (;;)
	{
		/* check for interrupts while we're not holding any buffer lock */
		CHECK_FOR_INTERRUPTS();

		if (ScanDirectionIsForward(dir))
		{
			if (BlockNumberIsValid(blkno))
				buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
			else if (so->hashso_buc_populated && !bucSplit)
			{
				/* Switch from populated bucket to split bucket */
				buf = _hash_step_to_split_bucket(scan);
				bucSplit = true;
			}
			else
				break;
		}
		else
		{
			if (BlockNumberIsValid(blkno))
				buf = _hash_getbuf(rel, blkno, HASH_READ,
								   LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
			else if (so->hashso_buc_populated && bucSplit)
			{
				/* Switch from split bucket to populated bucket */
				buf = _hash_step_to_populated_bucket(scan);
				bucSplit = false;
			}
			else
				break;
		}

		if (_hash_readpage(scan, buf, dir, newbatch, bucSplit))
		{
			/* _hash_readpage saved one or more matches in newbatch.items[] */
			batchscan_unlock(scan, newbatch, buf);
			return newbatch;
		}

		/* No matching items on that page; release it, go on from its neighbor */
		_hash_relbuf(rel, buf);
		if (ScanDirectionIsForward(dir))
			blkno = hashnewbatch->nextPage;
		else
			blkno = hashnewbatch->prevPage;
	}

	batchscan_release(scan, newbatch);

	return NULL;
}

/*
 * Cross over from the bucket being populated to the bucket being split, on
 * whose primary page we have held a pin since _hash_first.
 *
 * Returns the split bucket's primary page, share-locked and pinned for the
 * caller.  Caller gets their own pin, not the scan's pin.
 */
static Buffer
_hash_step_to_split_bucket(IndexScanDesc scan)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Buffer		buf = so->hashso_split_bucket_buf;

	/*
	 * buffer for bucket being split must be valid as we acquire the pin on it
	 * before the start of scan and retain it till end of scan.
	 */
	Assert(BufferIsValid(buf));

	/*
	 * Pin and share-lock the page for the traversal, which is what
	 * _hash_getbuf would do given its block number.  We hold the buffer
	 * already, so just take another reference on it, next to the scan's own.
	 */
	IncrBufferRefCount(buf);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);

	return buf;
}

/*
 * Cross over from the bucket being split to the bucket being populated, on
 * whose primary page we have held a pin since _hash_first, and walk to the
 * end of its chain (backward scans read chains tail to head).
 *
 * Returns the last page in the populated bucket's chain, share-locked and
 * pinned for the caller.  Caller gets their own pin.
 */
static Buffer
_hash_step_to_populated_bucket(IndexScanDesc scan)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Buffer		buf = so->hashso_bucket_buf;

	/*
	 * buffer for bucket being populated must be valid as we acquire the pin
	 * on it before the start of scan and retain it till end of scan.
	 */
	Assert(BufferIsValid(buf));

	/*
	 * Pin and share-lock the page for the traversal, which is what
	 * _hash_getbuf would do given its block number.  We hold the buffer
	 * already, so just take another reference on it, next to the scan's own.
	 */
	IncrBufferRefCount(buf);
	LockBuffer(buf, BUFFER_LOCK_SHARE);

	return _hash_chain_end(scan, buf);
}

/*
 * Walk from buf, a pinned and share-locked page of a bucket, to the last page
 * of that bucket's chain, and return it pinned and share-locked.
 */
static Buffer
_hash_chain_end(IndexScanDesc scan, Buffer buf)
{
	Relation	rel = scan->indexRelation;
	HashPageOpaque opaque = HashPageGetOpaque(BufferGetPage(buf));

	while (BlockNumberIsValid(opaque->hasho_nextblkno))
	{
		BlockNumber blkno = opaque->hasho_nextblkno;

		_hash_relbuf(rel, buf);

		/* check for interrupts while we're not holding any buffer lock */
		CHECK_FOR_INTERRUPTS();

		buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);
		opaque = HashPageGetOpaque(BufferGetPage(buf));
	}

	return buf;
}

/*
 *	_hash_first() -- Find the first batch of items in a scan.
 *
 *		We find the first batch of items (or, if backward scan, the last
 *		batch) in the index that satisfies the qualification associated with
 *		the scan descriptor.
 *
 *		On successful exit, returns a batch containing matching items.
 *		Otherwise returns NULL, indicating that there are no further matches.
 *		No locks are ever held when we return.
 *
 *		We keep our own pin on the primary bucket page, and on the primary
 *		page of the bucket being split when a split is in progress, until the
 *		scan is restarted or ended (except when we return NULL).  A returned
 *		batch holds its own, separate pin on its page.
 */
IndexScanBatch
_hash_first(IndexScanDesc scan, ScanDirection dir)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	ScanKey		cur;
	uint32		hashkey;
	Bucket		bucket;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	IndexScanBatch firstbatch;

	pgstat_count_index_scan(rel);
	if (scan->instrument)
		scan->instrument->nsearches++;

	/*
	 * We do not support hash scans with no index qualification, because we
	 * would have to read the whole index rather than just one bucket. That
	 * creates a whole raft of problems, since we haven't got a practical way
	 * to lock all the buckets against splits or compactions.
	 */
	if (scan->numberOfKeys < 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("hash indexes do not support whole-index scans")));

	/* There may be more than one index qual, but we hash only the first */
	cur = &scan->keyData[0];

	/* We support only single-column hash indexes */
	Assert(cur->sk_attno == 1);
	/* And there's only one operator strategy, too */
	Assert(cur->sk_strategy == HTEqualStrategyNumber);

	/*
	 * If the constant in the index qual is NULL, assume it cannot match any
	 * items in the index.
	 */
	if (cur->sk_flags & SK_ISNULL)
		return NULL;

	/*
	 * Okay to compute the hash key.  We want to do this before acquiring any
	 * locks, in case a user-defined hash function happens to be slow.
	 *
	 * If scankey operator is not a cross-type comparison, we can use the
	 * cached hash function; otherwise gotta look it up in the catalogs.
	 *
	 * We support the convention that sk_subtype == InvalidOid means the
	 * opclass input type; this is a hack to simplify life for ScanKeyInit().
	 */
	if (cur->sk_subtype == rel->rd_opcintype[0] ||
		cur->sk_subtype == InvalidOid)
		hashkey = _hash_datum2hashkey(rel, cur->sk_argument);
	else
		hashkey = _hash_datum2hashkey_type(rel, cur->sk_argument,
										   cur->sk_subtype);

	so->hashso_sk_hash = hashkey;

	/* Allocate space for first batch before locking anything */
	firstbatch = batchscan_alloc(scan);

	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_READ, NULL);
	PredicateLockPage(rel, BufferGetBlockNumber(buf), scan->xs_snapshot);
	page = BufferGetPage(buf);
	opaque = HashPageGetOpaque(page);
	bucket = opaque->hasho_bucket;

	/*
	 * Keep our own pin on the primary bucket page until the scan is restarted
	 * or ended (see _hash_dropscanbuf), not just until we return the last
	 * batch.  buf's original pin belongs to the page traversal, which
	 * releases it or passes it on to a batch.
	 *
	 * Holding the pin for the whole scan also gives index scans that use a
	 * scrollable cursor a consistent order.  We make no guarantee about the
	 * order _across_ scans, though: a bucket split relocates tuples into the
	 * new bucket in a different order, and a squeeze repacks the chain (the
	 * bucket pin at least prevents that for the duration of a single scan).
	 */
	so->hashso_bucket_buf = buf;
	IncrBufferRefCount(buf);

	/*
	 * If a bucket split is in progress, then while scanning the bucket being
	 * populated, we need to skip tuples that were copied from bucket being
	 * split.  We also need to maintain a pin on the bucket being split to
	 * ensure that split-cleanup work done by vacuum doesn't remove tuples
	 * from it till this scan is done.  We need to maintain a pin on the
	 * bucket being populated to ensure that vacuum doesn't squeeze that
	 * bucket till this scan is complete; otherwise, the ordering of tuples
	 * can't be maintained during forward and backward scans.  Here, we have
	 * to be cautious about locking order: first, acquire the lock on bucket
	 * being split; then, release the lock on it but not the pin; then,
	 * acquire a lock on bucket being populated and again re-verify whether
	 * the bucket split is still in progress.  Acquiring the lock on bucket
	 * being split first ensures that the vacuum waits for this scan to
	 * finish.
	 */
	if (H_BUCKET_BEING_POPULATED(opaque))
	{
		BlockNumber old_blkno;
		Buffer		old_buf;

		old_blkno = _hash_get_oldblock_from_newbucket(rel, bucket);

		/*
		 * release the lock on new bucket and re-acquire it after acquiring
		 * the lock on old bucket.
		 */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		old_buf = _hash_getbuf(rel, old_blkno, HASH_READ, LH_BUCKET_PAGE);

		/*
		 * remember the split bucket buffer so as to use it later for
		 * scanning.
		 */
		so->hashso_split_bucket_buf = old_buf;
		LockBuffer(old_buf, BUFFER_LOCK_UNLOCK);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		opaque = HashPageGetOpaque(page);
		Assert(opaque->hasho_bucket == bucket);

		if (H_BUCKET_BEING_POPULATED(opaque))
			so->hashso_buc_populated = true;
		else
		{
			_hash_dropbuf(rel, so->hashso_split_bucket_buf);
			so->hashso_split_bucket_buf = InvalidBuffer;
		}
	}

	return _hash_readfirstpage(scan, firstbatch, buf, dir);
}

/*
 *	_hash_readpage() -- Load data from an index page into batch
 *
 *	Caller must have pinned and share-locked buf; the buffer's state is not
 *	changed here.  We save the items on the page that satisfy the
 *	qualification into batch, along with the page's neighbors, from which
 *	_hash_readnextpage continues the scan.  bucSplit says whether the page is
 *	in the bucket being split rather than the one being populated, for a scan
 *	that started during a split.
 *
 *	Return true if any matching items are found else return false.
 */
static bool
_hash_readpage(IndexScanDesc scan, Buffer buf, ScanDirection dir,
			   IndexScanBatch batch, bool bucSplit)
{
	Relation	rel = scan->indexRelation;
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	HashBatchData *hashbatch = HashBatchGetData(scan, batch);
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber offnum,
				maxoff;
	IndexTuple	itup;
	int			itemIndex;
	bool		skipmoved;

	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);
	opaque = HashPageGetOpaque(page);
	maxoff = PageGetMaxOffsetNumber(page);

	hashbatch->buf = buf;
	hashbatch->batchPage = BufferGetBlockNumber(buf);
	hashbatch->bucSplit = bucSplit;
	batch->dir = dir;

	/*
	 * A scan that started during a split skips the moved-by-split tuples in
	 * the bucket being populated, and reads their originals in the bucket
	 * being split instead.
	 */
	Assert(so->hashso_buc_populated || !bucSplit);
	skipmoved = (so->hashso_buc_populated && !bucSplit);

	/* locate starting position by binary search, then load the items */
	if (ScanDirectionIsForward(dir))
	{
		/* load items[] in ascending order */
		itemIndex = 0;

		offnum = _hash_binsearch(page, so->hashso_sk_hash);
		Assert(offnum >= FirstOffsetNumber);
		while (offnum <= maxoff)
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * skip the tuples that are moved by split operation for the scan
			 * that has started when split was in progress. Also, skip the
			 * tuples that are marked as dead.
			 */
			if ((skipmoved && (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(scan->ignore_killed_tuples &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberNext(offnum);	/* move forward */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup))
			{
				/* tuple is qualified, so remember it */
				_hash_saveitem(batch, itemIndex, offnum, itup);
				itemIndex++;
			}
			else
			{
				/* No more matching tuples exist in this page */
				break;
			}

			offnum = OffsetNumberNext(offnum);
		}

		Assert(itemIndex <= MaxIndexTuplesPerPage);
		batch->firstItem = 0;
		batch->lastItem = itemIndex - 1;
	}
	else
	{
		/* load items[] in descending order */
		itemIndex = MaxIndexTuplesPerPage;

		offnum = _hash_binsearch_last(page, so->hashso_sk_hash);
		Assert(offnum <= maxoff);
		while (offnum >= FirstOffsetNumber)
		{
			itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, offnum));

			/*
			 * skip the tuples that are moved by split operation for the scan
			 * that has started when split was in progress. Also, skip the
			 * tuples that are marked as dead.
			 */
			if ((skipmoved && (itup->t_info & INDEX_MOVED_BY_SPLIT_MASK)) ||
				(scan->ignore_killed_tuples &&
				 (ItemIdIsDead(PageGetItemId(page, offnum)))))
			{
				offnum = OffsetNumberPrev(offnum);	/* move back */
				continue;
			}

			if (so->hashso_sk_hash == _hash_get_indextuple_hashkey(itup))
			{
				itemIndex--;
				/* tuple is qualified, so remember it */
				_hash_saveitem(batch, itemIndex, offnum, itup);
			}
			else
			{
				/* No more matching tuples exist in this page */
				break;
			}

			offnum = OffsetNumberPrev(offnum);
		}

		Assert(itemIndex >= 0);
		batch->firstItem = itemIndex;
		batch->lastItem = MaxIndexTuplesPerPage - 1;
	}

	/*
	 * Remember the page's neighbors.  A primary bucket page has none before
	 * it: its hasho_prevblkno holds a hashm_maxbucket value, not a block
	 * number (see HashPageOpaqueData).
	 */
	hashbatch->nextPage = opaque->hasho_nextblkno;
	if (opaque->hasho_flag & LH_BUCKET_PAGE)
		hashbatch->prevPage = InvalidBlockNumber;
	else
		hashbatch->prevPage = opaque->hasho_prevblkno;

	return (batch->firstItem <= batch->lastItem);
}

/* Save an index item into batch->items[itemIndex] */
static inline void
_hash_saveitem(IndexScanBatch batch, int itemIndex,
			   OffsetNumber offnum, IndexTuple itup)
{
	BatchMatchingItem *currItem = &batch->items[itemIndex];

	currItem->tableTid = itup->t_tid;
	currItem->indexOffset = offnum;
	currItem->tupleOffset = 0;
}
