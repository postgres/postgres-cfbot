/*-------------------------------------------------------------------------
 *
 * hashinsert.c
 *	  Item insertion in hash tables for Postgres.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Per-index insert state of this backend, mainly the tuples it inserted
 * without adding them to the metapage's hashm_ntuples.  See the description
 * of the insertion algorithm in the README.  Entries are keyed by index OID
 * and never removed; a stale count left by a dropped or truncated index is
 * less than HASH_MAX_PENDING_TUPLES, which is harmless.
 */
typedef struct HashPendingTuples
{
	Oid			indexrelid;		/* hash key */
	uint32		ntuples;		/* tuples not yet in hashm_ntuples */
	uint32		ninserted;		/* inserts, counting up to HASH_BATCH_AFTER */
	bool		batching;		/* seen the metapage lock taken? */
} HashPendingTuples;

static HTAB *hash_pending_tuples = NULL;

/*
 * Once this backend has inserted HASH_BATCH_AFTER tuples into the index and
 * then found the metapage lock taken, its inserts stop looking at the
 * metapage.  Every HASH_PENDING_BATCH inserts it tries the lock again, and
 * counts what is pending if it gets it.  If HASH_MAX_PENDING_TUPLES are
 * pending it waits for the lock.  The tuples left pending when a backend
 * exits are lost, so short sessions, whose share of those would be large,
 * never batch.
 */
#define HASH_BATCH_AFTER		1024
#define HASH_PENDING_BATCH		32
#define HASH_MAX_PENDING_TUPLES	256

static void _hash_vacuum_one_page(Relation rel, Relation hrel,
								  Buffer metabuf, Buffer buf);

/*
 *	_hash_doinsert() -- Handle insertion of a single index tuple.
 *
 *		This routine is called by the public interface routines, hashbuild
 *		and hashinsert.  By here, itup is completely filled in.
 *
 * 'sorted' must only be passed as 'true' when inserts are done in hashkey
 * order.
 */
void
_hash_doinsert(Relation rel, IndexTuple itup, Relation heapRel, bool sorted)
{
	Buffer		buf = InvalidBuffer;
	Buffer		bucket_buf;
	Buffer		metabuf = InvalidBuffer;
	HashMetaPage metap;
	HashMetaPage usedmetap = NULL;
	Page		page;
	HashPageOpaque pageopaque;
	Size		itemsz;
	bool		do_expand;
	bool		added_ovfl = false;
	bool		metalocked;
	HashPendingTuples *pending = NULL;
	uint32		ntup_delta = 0;
	uint32		hashkey;
	Bucket		bucket;
	OffsetNumber itup_off;
	XLogRecPtr	recptr;

	/*
	 * Get the hash key for the item (it's stored in the index tuple itself).
	 */
	hashkey = _hash_get_indextuple_hashkey(itup);

	/* compute item size too */
	itemsz = IndexTupleSize(itup);
	itemsz = MAXALIGN(itemsz);	/* be safe, PageAddItem will do this but we
								 * need to be consistent */

restart_insert:

	/*
	 * Lock the primary bucket page for the target bucket.  Most inserts never
	 * touch the metapage, not even to pin it, because under concurrency every
	 * pin and lock attempt on that one buffer moves its header between CPUs.
	 * We pin it only when we need it.
	 */
	buf = _hash_getbucketbuf_from_hashkey(rel, hashkey, HASH_WRITE,
										  &usedmetap);
	Assert(usedmetap != NULL);

	/*
	 * Check whether the item can fit on a hash page at all. (Eventually, we
	 * ought to try to apply TOAST methods if not.)  Note that at this point,
	 * itemsz doesn't include the ItemId.
	 *
	 * XXX this is useless code if we are only storing hash keys.
	 */
	if (itemsz > HashMaxItemSize(BufferGetPage(buf)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("index row size %zu exceeds hash maximum %zu",
						itemsz, HashMaxItemSize(BufferGetPage(buf))),
				 errhint("Values larger than a buffer page cannot be indexed.")));

	CheckForSerializableConflictIn(rel, NULL, BufferGetBlockNumber(buf));

	/* remember the primary bucket buffer to release the pin on it at end. */
	bucket_buf = buf;

	page = BufferGetPage(buf);
	pageopaque = HashPageGetOpaque(page);
	bucket = pageopaque->hasho_bucket;

	/*
	 * If this bucket is in the process of being split, try to finish the
	 * split before inserting, because that might create room for the
	 * insertion to proceed without allocating an additional overflow page.
	 * It's only interesting to finish the split if we're trying to insert
	 * into the bucket from which we're removing tuples (the "old" bucket),
	 * not if we're trying to insert into the bucket into which tuples are
	 * being moved (the "new" bucket).
	 */
	if (H_BUCKET_BEING_SPLIT(pageopaque) && IsBufferCleanupOK(buf))
	{
		/* release the lock on bucket buffer, before completing the split. */
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK, LH_META_PAGE);
		_hash_finish_split(rel, metabuf, buf, bucket,
						   usedmetap->hashm_maxbucket,
						   usedmetap->hashm_highmask,
						   usedmetap->hashm_lowmask);

		/* release the pin on old and meta buffer.  retry for insert. */
		_hash_dropbuf(rel, buf);
		_hash_dropbuf(rel, metabuf);
		metabuf = InvalidBuffer;
		goto restart_insert;
	}

	/* Do the insertion */
	while (PageGetFreeSpace(page) < itemsz)
	{
		BlockNumber nextblkno;

		/*
		 * Check if current page has any DEAD tuples. If yes, delete these
		 * tuples and see if we can get a space for the new item to be
		 * inserted before moving to the next page in the bucket chain.
		 */
		if (H_HAS_DEAD_TUPLES(pageopaque))
		{

			if (IsBufferCleanupOK(buf))
			{
				if (!BufferIsValid(metabuf))
					metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK,
										   LH_META_PAGE);
				_hash_vacuum_one_page(rel, heapRel, metabuf, buf);

				if (PageGetFreeSpace(page) >= itemsz)
					break;		/* OK, now we have enough space */
			}
		}

		/*
		 * no space on this page; check for an overflow page
		 */
		nextblkno = pageopaque->hasho_nextblkno;

		if (BlockNumberIsValid(nextblkno))
		{
			/*
			 * ovfl page exists; go get it.  if it doesn't have room, we'll
			 * find out next pass through the loop test above.  we always
			 * release both the lock and pin if this is an overflow page, but
			 * only the lock if this is the primary bucket page, since the pin
			 * on the primary bucket must be retained throughout the scan.
			 */
			if (buf != bucket_buf)
				_hash_relbuf(rel, buf);
			else
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			buf = _hash_getbuf(rel, nextblkno, HASH_WRITE, LH_OVERFLOW_PAGE);
			page = BufferGetPage(buf);
		}
		else
		{
			/*
			 * we're at the end of the bucket chain and we haven't found a
			 * page with enough room.  allocate a new overflow page.
			 */

			/* release our write lock without modifying buffer */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			/* chain to a new overflow page */
			if (!BufferIsValid(metabuf))
				metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK,
									   LH_META_PAGE);
			buf = _hash_addovflpage(rel, metabuf, buf, (buf == bucket_buf));
			page = BufferGetPage(buf);
			added_ovfl = true;

			/* should fit now, given test above */
			Assert(PageGetFreeSpace(page) >= itemsz);
		}
		pageopaque = HashPageGetOpaque(page);
		Assert((pageopaque->hasho_flag & LH_PAGE_TYPE) == LH_OVERFLOW_PAGE);
		Assert(pageopaque->hasho_bucket == bucket);
	}

	/*
	 * Write-lock the metapage so we can increment the tuple count. After
	 * incrementing it, check to see if it's time for a split.
	 *
	 * Every insert wants this lock, so under concurrency it serializes the
	 * inserters.  If someone else holds it, don't wait: leave the tuple
	 * pending.  Once this backend has done HASH_BATCH_AFTER inserts into the
	 * index, a failed try also makes it start batching: it only tries the
	 * lock on every HASH_PENDING_BATCH'th insert, since even a failed try
	 * writes to the buffer header and moves that cache line between CPUs. An
	 * insert that added an overflow page tries anyway, since the bucket may
	 * be due for a split.  Wait for the lock if HASH_MAX_PENDING_TUPLES are
	 * pending.
	 */
	{
		Oid			indexrelid = RelationGetRelid(rel);
		bool		found;

		if (hash_pending_tuples == NULL)
		{
			HASHCTL		ctl;

			ctl.keysize = sizeof(Oid);
			ctl.entrysize = sizeof(HashPendingTuples);
			ctl.hcxt = TopMemoryContext;
			hash_pending_tuples = hash_create("hash index pending tuples",
											  16, &ctl,
											  HASH_ELEM | HASH_BLOBS |
											  HASH_CONTEXT);
		}
		pending = hash_search(hash_pending_tuples, &indexrelid, HASH_ENTER,
							  &found);
		if (!found)
		{
			pending->ntuples = 0;
			pending->ninserted = 0;
			pending->batching = false;
		}
		if (pending->ninserted < HASH_BATCH_AFTER)
			pending->ninserted++;
	}

	if (!added_ovfl && pending->batching &&
		pending->ntuples % HASH_PENDING_BATCH != HASH_PENDING_BATCH - 1)
		metalocked = false;
	else
	{
		if (!BufferIsValid(metabuf))
			metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_NOLOCK,
								   LH_META_PAGE);

#ifdef USE_INJECTION_POINTS
		/* let tests act as if the lock was taken */
		if (IS_INJECTION_POINT_ATTACHED("hash-insert-defer-count"))
			metalocked = false;
		else
#endif
			metalocked = ConditionalLockBuffer(metabuf);

		if (!metalocked && pending->ntuples >= HASH_MAX_PENDING_TUPLES - 1)
		{
			LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
			metalocked = true;
		}
	}

	if (!metalocked && pending->ninserted >= HASH_BATCH_AFTER)
		pending->batching = true;

	/* Do the update.  No ereport(ERROR) until changes are logged */
	START_CRIT_SECTION();

	/* found page with enough space, so add the item here */
	itup_off = _hash_pgaddtup(rel, buf, itemsz, itup, sorted);
	MarkBufferDirty(buf);

	do_expand = false;
	if (metalocked)
	{
		/* metapage operations */
		ntup_delta = 1 + pending->ntuples;
		pending->ntuples = 0;

		metap = HashPageGetMeta(BufferGetPage(metabuf));
		metap->hashm_ntuples += ntup_delta;

		/* Make sure this stays in sync with _hash_expandtable() */
		do_expand = metap->hashm_ntuples >
			(double) metap->hashm_ffactor * (metap->hashm_maxbucket + 1);

		MarkBufferDirty(metabuf);
	}
	else
		pending->ntuples++;

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_insert xlrec;

		xlrec.offnum = itup_off;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHashInsert);

		if (metalocked)
		{
			XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);
			if (ntup_delta > 1)
				XLogRegisterBufData(1, &ntup_delta, sizeof(uint32));
		}

		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterBufData(0, itup, IndexTupleSize(itup));

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_INSERT);
	}
	else
		recptr = XLogGetFakeLSN(rel);

	PageSetLSN(BufferGetPage(buf), recptr);
	if (metalocked)
		PageSetLSN(BufferGetPage(metabuf), recptr);

	END_CRIT_SECTION();

	/* drop lock on metapage, but keep pin */
	if (metalocked)
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	/*
	 * Release the modified page and ensure to release the pin on primary
	 * page.
	 */
	_hash_relbuf(rel, buf);
	if (buf != bucket_buf)
		_hash_dropbuf(rel, bucket_buf);

	/* Attempt to split if a split is needed */
	if (do_expand)
		_hash_expandtable(rel, metabuf);

	/* Finally drop our pin on the metapage, if we took one */
	if (BufferIsValid(metabuf))
		_hash_dropbuf(rel, metabuf);
}

/*
 *	_hash_pgaddtup() -- add a tuple to a particular page in the index.
 *
 * This routine adds the tuple to the page as requested; it does not write out
 * the page.  It is an error to call this function without pin and write lock
 * on the target buffer.
 *
 * Returns the offset number at which the tuple was inserted.  This function
 * is responsible for preserving the condition that tuples in a hash index
 * page are sorted by hashkey value, however, if the caller is certain that
 * the hashkey for the tuple being added is >= the hashkeys of all existing
 * tuples on the page, then the 'appendtup' flag may be passed as true.  This
 * saves from having to binary search for the correct location to insert the
 * tuple.
 */
OffsetNumber
_hash_pgaddtup(Relation rel, Buffer buf, Size itemsize, IndexTuple itup,
			   bool appendtup)
{
	OffsetNumber itup_off;
	Page		page;

	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);

	/*
	 * Find where to insert the tuple (preserving page's hashkey ordering). If
	 * 'appendtup' is true then we just insert it at the end.
	 */
	if (appendtup)
	{
		itup_off = PageGetMaxOffsetNumber(page) + 1;

#ifdef USE_ASSERT_CHECKING
		/* ensure this tuple's hashkey is >= the final existing tuple */
		if (PageGetMaxOffsetNumber(page) > 0)
		{
			IndexTuple	lasttup;
			ItemId		itemid;

			itemid = PageGetItemId(page, PageGetMaxOffsetNumber(page));
			lasttup = (IndexTuple) PageGetItem(page, itemid);

			Assert(_hash_get_indextuple_hashkey(lasttup) <=
				   _hash_get_indextuple_hashkey(itup));
		}
#endif
	}
	else
	{
		uint32		hashkey = _hash_get_indextuple_hashkey(itup);

		itup_off = _hash_binsearch(page, hashkey);
	}

	if (PageAddItem(page, itup, itemsize, itup_off, false, false) == InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(rel));

	return itup_off;
}

/*
 *	_hash_pgaddmultitup() -- add a tuple vector to a particular page in the
 *							 index.
 *
 * This routine has same requirements for locking and tuple ordering as
 * _hash_pgaddtup().
 *
 * Returns the offset number array at which the tuples were inserted.
 */
void
_hash_pgaddmultitup(Relation rel, Buffer buf, IndexTuple *itups,
					OffsetNumber *itup_offsets, uint16 nitups)
{
	OffsetNumber itup_off;
	Page		page;
	uint32		hashkey;
	int			i;

	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);
	page = BufferGetPage(buf);

	for (i = 0; i < nitups; i++)
	{
		Size		itemsize;

		itemsize = IndexTupleSize(itups[i]);
		itemsize = MAXALIGN(itemsize);

		/* Find where to insert the tuple (preserving page's hashkey ordering) */
		hashkey = _hash_get_indextuple_hashkey(itups[i]);
		itup_off = _hash_binsearch(page, hashkey);

		itup_offsets[i] = itup_off;

		if (PageAddItem(page, itups[i], itemsize, itup_off, false, false) == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"", RelationGetRelationName(rel));
	}
}

/*
 * _hash_vacuum_one_page - vacuum just one index page.
 *
 * Try to remove LP_DEAD items from the given page. We must acquire cleanup
 * lock on the page being modified before calling this function.
 */

static void
_hash_vacuum_one_page(Relation rel, Relation hrel, Buffer metabuf, Buffer buf)
{
	OffsetNumber deletable[MaxOffsetNumber];
	int			ndeletable = 0;
	OffsetNumber offnum,
				maxoff;
	Page		page = BufferGetPage(buf);
	HashPageOpaque pageopaque;
	HashMetaPage metap;
	XLogRecPtr	recptr;

	/* Scan each tuple in page to see if it is marked as LP_DEAD */
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsDead(itemId))
			deletable[ndeletable++] = offnum;
	}

	if (ndeletable > 0)
	{
		TransactionId snapshotConflictHorizon;

		snapshotConflictHorizon =
			index_compute_xid_horizon_for_tuples(rel, hrel, buf,
												 deletable, ndeletable);

		/*
		 * Write-lock the meta page so that we can decrement tuple count.
		 */
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

		/* No ereport(ERROR) until changes are logged */
		START_CRIT_SECTION();

		PageIndexMultiDelete(page, deletable, ndeletable);

		/*
		 * Mark the page as not containing any LP_DEAD items. This is not
		 * certainly true (there might be some that have recently been marked,
		 * but weren't included in our target-item list), but it will almost
		 * always be true and it doesn't seem worth an additional page scan to
		 * check it. Remember that LH_PAGE_HAS_DEAD_TUPLES is only a hint
		 * anyway.
		 */
		pageopaque = HashPageGetOpaque(page);
		pageopaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;

		/* the count can be low by pending tuples; don't let it go negative */
		metap = HashPageGetMeta(BufferGetPage(metabuf));
		if (metap->hashm_ntuples > ndeletable)
			metap->hashm_ntuples -= ndeletable;
		else
			metap->hashm_ntuples = 0;

		MarkBufferDirty(buf);
		MarkBufferDirty(metabuf);

		/* XLOG stuff */
		if (RelationNeedsWAL(rel))
		{
			xl_hash_vacuum_one_page xlrec;

			xlrec.isCatalogRel = RelationIsAccessibleInLogicalDecoding(hrel);
			xlrec.snapshotConflictHorizon = snapshotConflictHorizon;
			xlrec.ntuples = ndeletable;

			XLogBeginInsert();
			XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
			XLogRegisterData(&xlrec, SizeOfHashVacuumOnePage);

			/*
			 * We need the target-offsets array whether or not we store the
			 * whole buffer, to allow us to find the snapshotConflictHorizon
			 * on a standby server.
			 */
			XLogRegisterData(deletable,
							 ndeletable * sizeof(OffsetNumber));

			XLogRegisterBuffer(1, metabuf, REGBUF_STANDARD);

			recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_VACUUM_ONE_PAGE);
		}
		else
			recptr = XLogGetFakeLSN(rel);

		PageSetLSN(BufferGetPage(buf), recptr);
		PageSetLSN(BufferGetPage(metabuf), recptr);

		END_CRIT_SECTION();

		/*
		 * Releasing write lock on meta page as we have updated the tuple
		 * count.
		 */
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
	}
}
