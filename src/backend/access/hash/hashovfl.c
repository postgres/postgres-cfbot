/*-------------------------------------------------------------------------
 *
 * hashovfl.c
 *	  Overflow page management code for the Postgres hash access method
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashovfl.c
 *
 * NOTES
 *	  Overflow pages look like ordinary relation pages.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "utils/rel.h"


static uint32 _hash_firstfreebit(uint32 map);
static Buffer _hash_findfreeovflpage(Relation rel, Buffer *metabuf,
									 HashMetaPage *metapp, Buffer *mapbufp,
									 uint32 *bitmap_page_bit, uint32 *bitp,
									 bool metalocked);
static bool _hash_ovflblkno_to_bitno_internal(HashMetaPage metap,
											  BlockNumber ovflblkno,
											  uint32 *bitno);

/*
 * When an overflow page has to be added at the end of the index, add this
 * many, and mark the ones we don't use yet as free.
 */
#define HASH_OVFL_EXTEND_BATCH	64


/*
 * Convert overflow page bit number (its index in the free-page bitmaps)
 * to block number within the index.
 */
static BlockNumber
bitno_to_blkno(HashMetaPage metap, uint32 ovflbitnum)
{
	uint32		splitnum = metap->hashm_ovflpoint;
	uint32		i;

	/* Convert zero-based bitnumber to 1-based page number */
	ovflbitnum += 1;

	/* Determine the split number for this page (must be >= 1) */
	for (i = 1;
		 i < splitnum && ovflbitnum > metap->hashm_spares[i];
		 i++)
		 /* loop */ ;

	/*
	 * Convert to absolute page number by adding the number of bucket pages
	 * that exist before this split point.
	 */
	return (BlockNumber) (_hash_get_totalbuckets(i) + ovflbitnum);
}

/*
 * _hash_ovflblkno_to_bitno
 *
 * Convert overflow page block number to bit number for free-page bitmap.
 */
uint32
_hash_ovflblkno_to_bitno(HashMetaPage metap, BlockNumber ovflblkno)
{
	uint32		bitnum;

	if (!_hash_ovflblkno_to_bitno_internal(metap, ovflblkno, &bitnum))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid overflow block number %u", ovflblkno)));
	return bitnum;
}

/*
 * Workhorse for _hash_ovflblkno_to_bitno; returns false instead of raising an
 * error if ovflblkno isn't an overflow page according to metap.
 */
static bool
_hash_ovflblkno_to_bitno_internal(HashMetaPage metap, BlockNumber ovflblkno,
								  uint32 *bitno)
{
	uint32		splitnum = metap->hashm_ovflpoint;
	uint32		i;
	uint32		bitnum;

	/* Determine the split number containing this page */
	for (i = 1; i <= splitnum; i++)
	{
		if (ovflblkno <= (BlockNumber) _hash_get_totalbuckets(i))
			break;				/* oops */
		bitnum = ovflblkno - _hash_get_totalbuckets(i);

		/*
		 * bitnum has to be greater than number of overflow page added in
		 * previous split point. The overflow page at this splitnum (i) if any
		 * should start from (_hash_get_totalbuckets(i) +
		 * metap->hashm_spares[i - 1] + 1).
		 */
		if (bitnum > metap->hashm_spares[i - 1] &&
			bitnum <= metap->hashm_spares[i])
		{
			*bitno = bitnum - 1;	/* -1 to convert 1-based to 0-based */
			return true;
		}
	}

	return false;
}

/*
 *	_hash_addovflpage
 *
 *	Add an overflow page to the bucket whose last page is pointed to by 'buf'.
 *
 *	On entry, the caller must hold a pin but no lock on 'buf'.  The pin is
 *	dropped before exiting (we assume the caller is not interested in 'buf'
 *	anymore) if not asked to retain.  The pin will be retained only for the
 *	primary bucket.  The returned overflow page will be pinned and
 *	write-locked; it is guaranteed to be empty.
 *
 *	The caller must hold a pin, but no lock, on the metapage buffer.
 *	That buffer is returned in the same state.
 *
 * NB: since this could be executed concurrently by multiple processes,
 * one should not assume that the returned overflow page will be the
 * immediate successor of the originally passed 'buf'.  Additional overflow
 * pages might have been added to the bucket chain in between.
 */
Buffer
_hash_addovflpage(Relation rel, Buffer metabuf, Buffer buf, bool retain_pin)
{
	Buffer		ovflbuf;
	Page		page;
	Page		ovflpage;
	HashPageOpaque pageopaque;
	HashPageOpaque ovflopaque;
	HashMetaPage metap;
	HashMetaPage cachedmetap;
	Buffer		mapbuf = InvalidBuffer;
	Buffer		newmapbuf = InvalidBuffer;
	BlockNumber blkno;
	uint32		splitnum = 0;
	uint32	   *freep = NULL;
	uint32		max_ovflpg;
	uint32		bit;
	uint32		bitmap_page_bit = 0;
	uint32		last_bit;
	uint16		nadded = 1;
	bool		page_found = false;
	bool		metalocked = false;
	bool		metamodified = false;
	XLogRecPtr	recptr;

	/*
	 * Write-lock the tail page.  Here, we need to maintain locking order such
	 * that, first acquire the lock on tail page of bucket, then on meta page
	 * to find and lock the bitmap page and if it is found, then lock on meta
	 * page is released, then finally acquire the lock on new overflow buffer.
	 * We need this locking order to avoid deadlock with backends that are
	 * doing inserts.
	 *
	 * Note: We could have avoided locking many buffers here if we made two
	 * WAL records for acquiring an overflow page (one to allocate an overflow
	 * page and another to add it to overflow bucket chain).  However, doing
	 * so can leak an overflow page, if the system crashes after allocation.
	 * Needless to say, it is better to have a single record from a
	 * performance point of view as well.
	 */
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	/* probably redundant... */
	_hash_checkpage(rel, buf, LH_BUCKET_PAGE | LH_OVERFLOW_PAGE);

	/* loop to find current tail page, in case someone else inserted too */
	for (;;)
	{
		BlockNumber nextblkno;

		page = BufferGetPage(buf);
		pageopaque = HashPageGetOpaque(page);
		nextblkno = pageopaque->hasho_nextblkno;

		if (!BlockNumberIsValid(nextblkno))
			break;

		/* we assume we do not need to write the unmodified page */
		if (retain_pin)
		{
			/* pin will be retained only for the primary bucket page */
			Assert((pageopaque->hasho_flag & LH_PAGE_TYPE) == LH_BUCKET_PAGE);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		}
		else
			_hash_relbuf(rel, buf);

		retain_pin = false;

		buf = _hash_getbuf(rel, nextblkno, HASH_WRITE, LH_OVERFLOW_PAGE);
	}

	/*
	 * Look for a free overflow page in the bitmap pages, without the metapage
	 * lock.  The bitmap pages' locations and the range of existing overflow
	 * pages come from our cached copy of the metapage.  A bitmap page is
	 * searched under a share lock, and exclusive-locked only to claim a bit.
	 * Bits for pages beyond the last existing overflow page are always set,
	 * so a free bit always stands for a real page, but a page added since our
	 * copy was taken might be in a splitpoint the copy doesn't know of; so
	 * refresh the copy before claiming a bit beyond its range.
	 */
	cachedmetap = _hash_getcachedmetap(rel, &metabuf, false);
	ovflbuf = _hash_findfreeovflpage(rel, &metabuf, &cachedmetap, &mapbuf,
									 &bitmap_page_bit, &bit, false);
	if (BufferIsValid(ovflbuf))
	{
		page_found = true;
		metap = cachedmetap;
		goto found;
	}

	/*
	 * None found.  Lock the metapage, and search again with its current
	 * contents, from the first bitmap page: this also finds pages freed in
	 * bitmap pages before the one hashm_firstfree points into.
	 */
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metalocked = true;

	_hash_checkpage(rel, metabuf, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	ovflbuf = _hash_findfreeovflpage(rel, &metabuf, &metap, &mapbuf,
									 &bitmap_page_bit, &bit, true);
	if (BufferIsValid(ovflbuf))
	{
		page_found = true;
		goto found;
	}

	/*
	 * No free pages --- have to extend the relation to add an overflow page.
	 * First, check to see if we have to add a new bitmap page too.
	 */
	splitnum = metap->hashm_ovflpoint;
	max_ovflpg = metap->hashm_spares[splitnum] - 1;
	last_bit = max_ovflpg & BMPG_MASK(metap);

	if (last_bit == (uint32) (BMPGSZ_BIT(metap) - 1))
	{
		/*
		 * We create the new bitmap page with all pages marked "in use".
		 * Actually two pages in the new bitmap's range will exist
		 * immediately: the bitmap page itself, and the following page which
		 * is the one we return to the caller.  Both of these are correctly
		 * marked "in use".  Subsequent pages do not exist yet, but it is
		 * convenient to pre-mark them as "in use" too.
		 */
		bit = metap->hashm_spares[splitnum];

		/* metapage already has a write lock */
		if (metap->hashm_nmaps >= HASH_MAX_BITMAPS)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of overflow pages in hash index \"%s\"",
							RelationGetRelationName(rel))));

		newmapbuf = _hash_getnewbuf(rel, bitno_to_blkno(metap, bit), MAIN_FORKNUM);
	}
	else
	{
		/*
		 * Add a batch of pages, so that the next overflow pages can be found
		 * in the bitmap without the metapage lock.  All the pages' bits must
		 * be on the last bitmap page.  Their bits are set, since they are
		 * past the last used page; clear all but the first, which we return.
		 */
		nadded = Min(HASH_OVFL_EXTEND_BATCH,
					 BMPGSZ_BIT(metap) - 1 - last_bit);

		if (nadded > 1)
		{
			BlockNumber firstblk = bitno_to_blkno(metap, max_ovflpg + 1);

			/*
			 * Extend the relation to cover the whole batch.  The pages after
			 * the first are left as zero pages, to be initialized when used.
			 * If a previous attempt crashed after extending, they may exist
			 * already.
			 */
			if (RelationGetNumberOfBlocks(rel) < firstblk + nadded &&
				!_hash_alloc_buckets(rel, firstblk + 1, nadded - 1))
				nadded = 1;
		}

		if (nadded > 1)
		{
			mapbuf = _hash_getbuf(rel, metap->hashm_mapp[metap->hashm_nmaps - 1],
								  HASH_WRITE, LH_BITMAP_PAGE);
			freep = HashPageGetBitmap(BufferGetPage(mapbuf));
			bitmap_page_bit = last_bit + 2;
		}
		else
			nadded = 1;
	}

	/* Calculate address of the new overflow page */
	bit = BufferIsValid(newmapbuf) ?
		metap->hashm_spares[splitnum] + 1 : metap->hashm_spares[splitnum];
	blkno = bitno_to_blkno(metap, bit);

	/*
	 * Fetch the page with _hash_getnewbuf to ensure smgr's idea of the
	 * relation length stays in sync with ours.  XXX It's annoying to do this
	 * with metapage write lock held; would be better to use a lock that
	 * doesn't block incoming searches.
	 *
	 * It is okay to hold two buffer locks here (one on tail page of bucket
	 * and other on new overflow page) since there cannot be anyone else
	 * contending for access to ovflbuf.
	 */
	ovflbuf = _hash_getnewbuf(rel, blkno, MAIN_FORKNUM);

found:

	if (page_found)
		freep = HashPageGetBitmap(BufferGetPage(mapbuf));

	/*
	 * Do the update.  No ereport(ERROR) until changes are logged. We want to
	 * log the changes for bitmap page and overflow page together to avoid
	 * loss of pages in case the new page is added.
	 */
	START_CRIT_SECTION();

	if (page_found)
	{
		Assert(BufferIsValid(mapbuf));

		/* mark page "in use" in the bitmap */
		SETBIT(freep, bitmap_page_bit);
		MarkBufferDirty(mapbuf);

		/*
		 * If we searched with the metapage locked, start the next searches at
		 * this page's bitmap page.
		 */
		if (metalocked && bit < metap->hashm_firstfree)
		{
			metap->hashm_firstfree = bit;
			MarkBufferDirty(metabuf);
			metamodified = true;
		}
	}
	else
	{
		/* update the count to indicate new overflow pages are added */
		metap->hashm_spares[splitnum] += nadded;

		/* the pages after the first are free */
		for (uint32 k = 0; k < nadded - 1; k++)
			CLRBIT(freep, bitmap_page_bit + k);
		if (nadded > 1)
			MarkBufferDirty(mapbuf);

		if (BufferIsValid(newmapbuf))
		{
			_hash_initbitmapbuffer(newmapbuf, metap->hashm_bmsize, false);
			MarkBufferDirty(newmapbuf);

			/* add the new bitmap page to the metapage's list of bitmaps */
			metap->hashm_mapp[metap->hashm_nmaps] = BufferGetBlockNumber(newmapbuf);
			metap->hashm_nmaps++;
			metap->hashm_spares[splitnum]++;
		}

		/* start the next searches after this page */
		metap->hashm_firstfree = bit + 1;

		MarkBufferDirty(metabuf);
		metamodified = true;

		/*
		 * for new overflow page, we don't need to explicitly set the bit in
		 * bitmap page, as by default that will be set to "in use".
		 */
	}

	/* initialize new overflow page */
	ovflpage = BufferGetPage(ovflbuf);
	ovflopaque = HashPageGetOpaque(ovflpage);
	ovflopaque->hasho_prevblkno = BufferGetBlockNumber(buf);
	ovflopaque->hasho_nextblkno = InvalidBlockNumber;
	ovflopaque->hasho_bucket = pageopaque->hasho_bucket;
	ovflopaque->hasho_flag = LH_OVERFLOW_PAGE;
	ovflopaque->hasho_page_id = HASHO_PAGE_ID;

	MarkBufferDirty(ovflbuf);

	/* logically chain overflow page to previous page */
	pageopaque->hasho_nextblkno = BufferGetBlockNumber(ovflbuf);

	MarkBufferDirty(buf);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_add_ovfl_page xlrec;

		xlrec.bmpage_found = page_found;
		xlrec.bmsize = metap->hashm_bmsize;
		xlrec.nadded = page_found ? 0 : nadded;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHashAddOvflPage);

		XLogRegisterBuffer(0, ovflbuf, REGBUF_WILL_INIT);
		XLogRegisterBufData(0, &pageopaque->hasho_bucket, sizeof(Bucket));

		XLogRegisterBuffer(1, buf, REGBUF_STANDARD);

		if (BufferIsValid(mapbuf))
		{
			XLogRegisterBuffer(2, mapbuf, REGBUF_STANDARD);
			XLogRegisterBufData(2, &bitmap_page_bit, sizeof(uint32));
		}

		if (BufferIsValid(newmapbuf))
			XLogRegisterBuffer(3, newmapbuf, REGBUF_WILL_INIT);

		if (metamodified)
		{
			XLogRegisterBuffer(4, metabuf, REGBUF_STANDARD);
			XLogRegisterBufData(4, &metap->hashm_firstfree, sizeof(uint32));
		}

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_ADD_OVFL_PAGE);
	}
	else
		recptr = XLogGetFakeLSN(rel);

	PageSetLSN(BufferGetPage(ovflbuf), recptr);
	PageSetLSN(BufferGetPage(buf), recptr);

	if (BufferIsValid(mapbuf))
		PageSetLSN(BufferGetPage(mapbuf), recptr);

	if (BufferIsValid(newmapbuf))
		PageSetLSN(BufferGetPage(newmapbuf), recptr);

	if (metamodified)
		PageSetLSN(BufferGetPage(metabuf), recptr);

	END_CRIT_SECTION();

	if (retain_pin)
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, buf);

	if (BufferIsValid(mapbuf))
		_hash_relbuf(rel, mapbuf);

	if (metalocked)
		LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

	if (BufferIsValid(newmapbuf))
		_hash_relbuf(rel, newmapbuf);

	return ovflbuf;
}

/*
 *	_hash_findfreeovflpage()
 *
 *	Find a free overflow page in the bitmap pages, and return it pinned,
 *	write-locked and initialized, with the bitmap page that has its bit
 *	write-locked in *mapbufp.  The bit is not set yet: the caller does that
 *	in its critical section.  *bitmap_page_bit is the bit's number within its
 *	bitmap page, and *bitp the overflow page's bit number.  Returns
 *	InvalidBuffer if there is no free page.
 *
 *	If metalocked, *metapp points to the metapage, which the caller holds
 *	write-locked, and we search all the bitmap pages.  Otherwise *metapp is
 *	our cached copy of the metapage, and we search from the bitmap page that
 *	hashm_firstfree is in.  A bit beyond the range of overflow pages the copy
 *	knows of can stand for a page in a splitpoint added since, so we refresh
 *	the copy before using such a bit.  Bits for pages that don't exist yet
 *	are set, so a free bit always stands for an existing page.
 *
 *	Lock order: we lock the metapage only if no bitmap page lock is held.
 */
static Buffer
_hash_findfreeovflpage(Relation rel, Buffer *metabuf, HashMetaPage *metapp,
					   Buffer *mapbufp, uint32 *bitmap_page_bit, uint32 *bitp,
					   bool metalocked)
{
	HashMetaPage metap = *metapp;
	uint32		i;

	i = metalocked ? 0 : metap->hashm_firstfree >> BMPG_SHIFT(metap);

	while (i < metap->hashm_nmaps)
	{
		Buffer		mapbuf;
		uint32	   *freep;
		uint32		nwords = metap->hashm_bmsize / sizeof(uint32);
		uint32		j;
		bool		exclusive = false;

		mapbuf = _hash_getbuf(rel, metap->hashm_mapp[i], HASH_READ,
							  LH_BITMAP_PAGE);
		freep = HashPageGetBitmap(BufferGetPage(mapbuf));

		for (j = 0; j < nwords; j++)
		{
			uint32		bit;

			if (freep[j] == ALL_SET)
				continue;

			if (!exclusive)
			{
				/* lock it exclusively and look at this word again */
				LockBuffer(mapbuf, BUFFER_LOCK_UNLOCK);
				LockBuffer(mapbuf, BUFFER_LOCK_EXCLUSIVE);
				exclusive = true;
				if (freep[j] == ALL_SET)
					continue;
			}

			bit = (i << BMPG_SHIFT(metap)) + j * BITS_PER_MAP +
				_hash_firstfreebit(freep[j]);

			if (metalocked &&
				bit >= metap->hashm_spares[metap->hashm_ovflpoint])
				elog(ERROR, "free bit %u past the last overflow page in hash index \"%s\"",
					 bit, RelationGetRelationName(rel));

			if (!metalocked &&
				bit >= metap->hashm_spares[metap->hashm_ovflpoint])
			{
				/* refresh our copy of the metapage, and retry this page */
				_hash_relbuf(rel, mapbuf);
				metap = _hash_getcachedmetap(rel, metabuf, true);
				*metapp = metap;
				if (bit >= metap->hashm_spares[metap->hashm_ovflpoint])
					elog(ERROR, "free bit %u past the last overflow page in hash index \"%s\"",
						 bit, RelationGetRelationName(rel));
				break;
			}

			*mapbufp = mapbuf;
			*bitmap_page_bit = bit & BMPG_MASK(metap);
			*bitp = bit;

			/* Fetch and init the recycled page */
			return _hash_getinitbuf(rel, bitno_to_blkno(metap, bit));
		}

		if (j < nwords)
			continue;			/* refreshed the metapage copy; retry page */

		_hash_relbuf(rel, mapbuf);
		i++;
	}

	return InvalidBuffer;
}

/*
 *	_hash_firstfreebit()
 *
 *	Return the number of the first bit that is not set in the word 'map'.
 */
static uint32
_hash_firstfreebit(uint32 map)
{
	uint32		i,
				mask;

	mask = 0x1;
	for (i = 0; i < BITS_PER_MAP; i++)
	{
		if (!(mask & map))
			return i;
		mask <<= 1;
	}

	elog(ERROR, "firstfreebit found no free bit");

	return 0;					/* keep compiler quiet */
}

/*
 *	_hash_freeovflpage() -
 *
 *	Remove this overflow page from its bucket's chain, and mark the page as
 *	free.  On entry, ovflbuf is write-locked; it is released before exiting.
 *
 *	Add the tuples (itups) to wbuf in this function.  We could do that in the
 *	caller as well, but the advantage of doing it here is we can easily write
 *	the WAL for XLOG_HASH_SQUEEZE_PAGE operation.  Addition of tuples and
 *	removal of overflow page has to done as an atomic operation, otherwise
 *	during replay on standby users might find duplicate records.
 *
 *	Since this function is invoked in VACUUM, we provide an access strategy
 *	parameter that controls fetches of the bucket pages.
 *
 *	Returns the block number of the page that followed the given page
 *	in the bucket, or InvalidBlockNumber if no following page.
 *
 *	NB: caller must not hold lock on metapage, nor on page, that's next to
 *	ovflbuf in the bucket chain.  We don't acquire the lock on page that's
 *	prior to ovflbuf in chain if it is same as wbuf because the caller already
 *	has a lock on same.
 */
BlockNumber
_hash_freeovflpage(Relation rel, Buffer bucketbuf, Buffer ovflbuf,
				   Buffer wbuf, IndexTuple *itups, OffsetNumber *itup_offsets,
				   Size *tups_size, uint16 nitups,
				   BufferAccessStrategy bstrategy)
{
	HashMetaPage metap;
	Buffer		metabuf = InvalidBuffer;
	Buffer		mapbuf;
	BlockNumber ovflblkno;
	BlockNumber prevblkno;
	BlockNumber blkno;
	BlockNumber nextblkno;
	BlockNumber writeblkno;
	HashPageOpaque ovflopaque;
	Page		ovflpage;
	Page		mappage;
	uint32	   *freep;
	uint32		ovflbitno;
	int32		bitmappage,
				bitmapbit;
	Bucket		bucket PG_USED_FOR_ASSERTS_ONLY;
	Buffer		prevbuf = InvalidBuffer;
	Buffer		nextbuf = InvalidBuffer;
	bool		mod_wbuf,
				is_prim_bucket_same_wrt,
				is_prev_bucket_same_wrt;
	XLogRecPtr	recptr;

	/* Get information from the doomed page */
	_hash_checkpage(rel, ovflbuf, LH_OVERFLOW_PAGE);
	ovflblkno = BufferGetBlockNumber(ovflbuf);
	ovflpage = BufferGetPage(ovflbuf);
	ovflopaque = HashPageGetOpaque(ovflpage);
	nextblkno = ovflopaque->hasho_nextblkno;
	prevblkno = ovflopaque->hasho_prevblkno;
	writeblkno = BufferGetBlockNumber(wbuf);
	bucket = ovflopaque->hasho_bucket;

	/*
	 * Fix up the bucket chain.  this is a doubly-linked list, so we must fix
	 * up the bucket chain members behind and ahead of the overflow page being
	 * deleted.  Concurrency issues are avoided by using lock chaining as
	 * described atop hashbucketcleanup.
	 */
	if (BlockNumberIsValid(prevblkno))
	{
		if (prevblkno == writeblkno)
			prevbuf = wbuf;
		else
			prevbuf = _hash_getbuf_with_strategy(rel,
												 prevblkno,
												 HASH_WRITE,
												 LH_BUCKET_PAGE | LH_OVERFLOW_PAGE,
												 bstrategy);
	}
	if (BlockNumberIsValid(nextblkno))
		nextbuf = _hash_getbuf_with_strategy(rel,
											 nextblkno,
											 HASH_WRITE,
											 LH_OVERFLOW_PAGE,
											 bstrategy);

	/* Note: bstrategy is intentionally not used for metapage and bitmap */

	/*
	 * Determine which bitmap page to use from our cached copy of the
	 * metapage.  If the page was added after the copy was taken, refresh it.
	 * We don't update hashm_firstfree, so we don't lock the metapage; see
	 * "Free Space Management" in the README.
	 */
	metap = _hash_getcachedmetap(rel, &metabuf, false);
	if (!_hash_ovflblkno_to_bitno_internal(metap, ovflblkno, &ovflbitno))
	{
		metap = _hash_getcachedmetap(rel, &metabuf, true);
		ovflbitno = _hash_ovflblkno_to_bitno(metap, ovflblkno);
	}

	bitmappage = ovflbitno >> BMPG_SHIFT(metap);
	bitmapbit = ovflbitno & BMPG_MASK(metap);

	if (bitmappage >= metap->hashm_nmaps)
		elog(ERROR, "invalid overflow bit number %u", ovflbitno);
	blkno = metap->hashm_mapp[bitmappage];

	/* read the bitmap page to clear the bitmap bit */
	mapbuf = _hash_getbuf(rel, blkno, HASH_WRITE, LH_BITMAP_PAGE);
	mappage = BufferGetPage(mapbuf);
	freep = HashPageGetBitmap(mappage);
	Assert(ISSET(freep, bitmapbit));

	/* This operation needs to log multiple tuples, prepare WAL for that */
	if (RelationNeedsWAL(rel))
		XLogEnsureRecordSpace(HASH_XLOG_FREE_OVFL_BUFS, 4 + nitups);

	START_CRIT_SECTION();

	/*
	 * we have to insert tuples on the "write" page, being careful to preserve
	 * hashkey ordering.  (If we insert many tuples into the same "write" page
	 * it would be worth qsort'ing them).
	 */
	if (nitups > 0)
	{
		_hash_pgaddmultitup(rel, wbuf, itups, itup_offsets, nitups);
		MarkBufferDirty(wbuf);
	}

	/*
	 * Reinitialize the freed overflow page.  Just zeroing the page won't
	 * work, because WAL replay routines expect pages to be initialized. See
	 * explanation of RBM_NORMAL mode atop XLogReadBufferExtended.  We are
	 * careful to make the special space valid here so that tools like
	 * pageinspect won't get confused.
	 */
	_hash_pageinit(ovflpage, BufferGetPageSize(ovflbuf));

	ovflopaque = HashPageGetOpaque(ovflpage);

	ovflopaque->hasho_prevblkno = InvalidBlockNumber;
	ovflopaque->hasho_nextblkno = InvalidBlockNumber;
	ovflopaque->hasho_bucket = InvalidBucket;
	ovflopaque->hasho_flag = LH_UNUSED_PAGE;
	ovflopaque->hasho_page_id = HASHO_PAGE_ID;

	MarkBufferDirty(ovflbuf);

	if (BufferIsValid(prevbuf))
	{
		Page		prevpage = BufferGetPage(prevbuf);
		HashPageOpaque prevopaque = HashPageGetOpaque(prevpage);

		Assert(prevopaque->hasho_bucket == bucket);
		prevopaque->hasho_nextblkno = nextblkno;
		MarkBufferDirty(prevbuf);
	}
	if (BufferIsValid(nextbuf))
	{
		Page		nextpage = BufferGetPage(nextbuf);
		HashPageOpaque nextopaque = HashPageGetOpaque(nextpage);

		Assert(nextopaque->hasho_bucket == bucket);
		nextopaque->hasho_prevblkno = prevblkno;
		MarkBufferDirty(nextbuf);
	}

	/* Clear the bitmap bit to indicate that this overflow page is free */
	CLRBIT(freep, bitmapbit);
	MarkBufferDirty(mapbuf);

	/* Determine which pages are modified */
	is_prim_bucket_same_wrt = (wbuf == bucketbuf);
	is_prev_bucket_same_wrt = (wbuf == prevbuf);
	mod_wbuf = (nitups > 0 || is_prev_bucket_same_wrt);

	/* XLOG stuff */
	if (RelationNeedsWAL(rel))
	{
		xl_hash_squeeze_page xlrec;

		xlrec.prevblkno = prevblkno;
		xlrec.nextblkno = nextblkno;
		xlrec.ntups = nitups;
		xlrec.is_prim_bucket_same_wrt = is_prim_bucket_same_wrt;
		xlrec.is_prev_bucket_same_wrt = is_prev_bucket_same_wrt;

		XLogBeginInsert();
		XLogRegisterData(&xlrec, SizeOfHashSqueezePage);

		/*
		 * bucket buffer was not changed, but still needs to be registered to
		 * ensure that we can acquire a cleanup lock on it during replay.
		 */
		if (!is_prim_bucket_same_wrt)
		{
			uint8		flags = REGBUF_STANDARD | REGBUF_NO_IMAGE | REGBUF_NO_CHANGE;

			XLogRegisterBuffer(0, bucketbuf, flags);
		}

		if (nitups > 0)
		{
			XLogRegisterBuffer(1, wbuf, REGBUF_STANDARD);
			XLogRegisterBufData(1, itup_offsets,
								nitups * sizeof(OffsetNumber));
			for (int i = 0; i < nitups; i++)
				XLogRegisterBufData(1, itups[i], tups_size[i]);
		}
		else if (is_prim_bucket_same_wrt || is_prev_bucket_same_wrt)
		{
			uint8		wbuf_flags;

			/*
			 * A write buffer needs to be registered even if no tuples are
			 * added to it to ensure that we can acquire a cleanup lock on it
			 * if it is the same as primary bucket buffer or update the
			 * nextblkno if it is same as the previous bucket buffer.
			 */
			Assert(nitups == 0);

			wbuf_flags = REGBUF_STANDARD;
			if (!is_prev_bucket_same_wrt)
				wbuf_flags |= REGBUF_NO_CHANGE;
			else
				Assert(mod_wbuf);
			XLogRegisterBuffer(1, wbuf, wbuf_flags);
		}

		XLogRegisterBuffer(2, ovflbuf, REGBUF_STANDARD);

		/*
		 * If prevpage and the writepage (block in which we are moving tuples
		 * from overflow) are same, then no need to separately register
		 * prevpage.  During replay, we can directly update the nextblock in
		 * writepage.
		 */
		if (BufferIsValid(prevbuf) && !is_prev_bucket_same_wrt)
			XLogRegisterBuffer(3, prevbuf, REGBUF_STANDARD);

		if (BufferIsValid(nextbuf))
			XLogRegisterBuffer(4, nextbuf, REGBUF_STANDARD);

		XLogRegisterBuffer(5, mapbuf, REGBUF_STANDARD);
		XLogRegisterBufData(5, &bitmapbit, sizeof(uint32));

		recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_SQUEEZE_PAGE);
	}
	else						/* !RelationNeedsWAL(rel) */
		recptr = XLogGetFakeLSN(rel);

	/* Set LSN iff wbuf is modified. */
	if (mod_wbuf)
		PageSetLSN(BufferGetPage(wbuf), recptr);

	PageSetLSN(BufferGetPage(ovflbuf), recptr);

	if (BufferIsValid(prevbuf) && !is_prev_bucket_same_wrt)
		PageSetLSN(BufferGetPage(prevbuf), recptr);
	if (BufferIsValid(nextbuf))
		PageSetLSN(BufferGetPage(nextbuf), recptr);

	PageSetLSN(BufferGetPage(mapbuf), recptr);

	END_CRIT_SECTION();

	/* release previous bucket if it is not same as write bucket */
	if (BufferIsValid(prevbuf) && prevblkno != writeblkno)
		_hash_relbuf(rel, prevbuf);

	if (BufferIsValid(ovflbuf))
		_hash_relbuf(rel, ovflbuf);

	if (BufferIsValid(nextbuf))
		_hash_relbuf(rel, nextbuf);

	_hash_relbuf(rel, mapbuf);
	if (BufferIsValid(metabuf))
		_hash_dropbuf(rel, metabuf);

	return nextblkno;
}


/*
 *	_hash_initbitmapbuffer()
 *
 *	 Initialize a new bitmap page.  All bits in the new bitmap page are set to
 *	 "1", indicating "in use".
 */
void
_hash_initbitmapbuffer(Buffer buf, uint16 bmsize, bool initpage)
{
	Page		pg;
	HashPageOpaque op;
	uint32	   *freep;

	pg = BufferGetPage(buf);

	/* initialize the page */
	if (initpage)
		_hash_pageinit(pg, BufferGetPageSize(buf));

	/* initialize the page's special space */
	op = HashPageGetOpaque(pg);
	op->hasho_prevblkno = InvalidBlockNumber;
	op->hasho_nextblkno = InvalidBlockNumber;
	op->hasho_bucket = InvalidBucket;
	op->hasho_flag = LH_BITMAP_PAGE;
	op->hasho_page_id = HASHO_PAGE_ID;

	/* set all of the bits to 1 */
	freep = HashPageGetBitmap(pg);
	memset(freep, 0xFF, bmsize);

	/*
	 * Set pd_lower just past the end of the bitmap page data.  We could even
	 * set pd_lower equal to pd_upper, but this is more precise and makes the
	 * page look compressible to xlog.c.
	 */
	((PageHeader) pg)->pd_lower = ((char *) freep + bmsize) - (char *) pg;
}


/*
 *	_hash_squeezebucket(rel, bucket)
 *
 *	Try to squeeze the tuples onto pages occurring earlier in the
 *	bucket chain in an attempt to free overflow pages. When we start
 *	the "squeezing", the page from which we start taking tuples (the
 *	"read" page) is the last bucket in the bucket chain and the page
 *	onto which we start squeezing tuples (the "write" page) is the
 *	first page in the bucket chain.  The read page works backward and
 *	the write page works forward; the procedure terminates when the
 *	read page and write page are the same page.
 *
 *	At completion of this procedure, it is guaranteed that all pages in
 *	the bucket are nonempty, unless the bucket is totally empty (in
 *	which case all overflow pages will be freed).  The original implementation
 *	required that to be true on entry as well, but it's a lot easier for
 *	callers to leave empty overflow pages and let this guy clean it up.
 *
 *	Caller must acquire cleanup lock on the primary page of the target
 *	bucket to exclude any scans that are in progress, which could easily
 *	be confused into returning the same tuple more than once or some tuples
 *	not at all by the rearrangement we are performing here.  To prevent
 *	any concurrent scan to cross the squeeze scan we use lock chaining
 *	similar to hashbucketcleanup.  Refer comments atop hashbucketcleanup.
 *
 *	We need to retain a pin on the primary bucket to ensure that no concurrent
 *	split can start.
 *
 *	Since this function is invoked in VACUUM, we provide an access strategy
 *	parameter that controls fetches of the bucket pages.
 */
void
_hash_squeezebucket(Relation rel,
					Bucket bucket,
					BlockNumber bucket_blkno,
					Buffer bucket_buf,
					BufferAccessStrategy bstrategy)
{
	BlockNumber wblkno;
	BlockNumber rblkno;
	Buffer		wbuf;
	Buffer		rbuf;
	Page		wpage;
	Page		rpage;
	HashPageOpaque wopaque;
	HashPageOpaque ropaque;

	/*
	 * start squeezing into the primary bucket page.
	 */
	wblkno = bucket_blkno;
	wbuf = bucket_buf;
	wpage = BufferGetPage(wbuf);
	wopaque = HashPageGetOpaque(wpage);

	/*
	 * if there aren't any overflow pages, there's nothing to squeeze. caller
	 * is responsible for releasing the pin on primary bucket page.
	 */
	if (!BlockNumberIsValid(wopaque->hasho_nextblkno))
	{
		LockBuffer(wbuf, BUFFER_LOCK_UNLOCK);
		return;
	}

	/*
	 * Find the last page in the bucket chain by starting at the base bucket
	 * page and working forward.  Note: we assume that a hash bucket chain is
	 * usually smaller than the buffer ring being used by VACUUM, else using
	 * the access strategy here would be counterproductive.
	 */
	rbuf = InvalidBuffer;
	ropaque = wopaque;
	do
	{
		rblkno = ropaque->hasho_nextblkno;
		if (rbuf != InvalidBuffer)
			_hash_relbuf(rel, rbuf);
		rbuf = _hash_getbuf_with_strategy(rel,
										  rblkno,
										  HASH_WRITE,
										  LH_OVERFLOW_PAGE,
										  bstrategy);
		rpage = BufferGetPage(rbuf);
		ropaque = HashPageGetOpaque(rpage);
		Assert(ropaque->hasho_bucket == bucket);
	} while (BlockNumberIsValid(ropaque->hasho_nextblkno));

	/*
	 * squeeze the tuples.
	 */
	for (;;)
	{
		OffsetNumber roffnum;
		OffsetNumber maxroffnum;
		OffsetNumber deletable[MaxOffsetNumber];
		IndexTuple	itups[MaxIndexTuplesPerPage];
		Size		tups_size[MaxIndexTuplesPerPage];
		OffsetNumber itup_offsets[MaxIndexTuplesPerPage];
		uint16		ndeletable = 0;
		uint16		nitups = 0;
		Size		all_tups_size = 0;
		int			i;
		bool		retain_pin = false;

readpage:
		/* Scan each tuple in "read" page */
		maxroffnum = PageGetMaxOffsetNumber(rpage);
		for (roffnum = FirstOffsetNumber;
			 roffnum <= maxroffnum;
			 roffnum = OffsetNumberNext(roffnum))
		{
			IndexTuple	itup;
			Size		itemsz;

			/* skip dead tuples */
			if (ItemIdIsDead(PageGetItemId(rpage, roffnum)))
				continue;

			itup = (IndexTuple) PageGetItem(rpage,
											PageGetItemId(rpage, roffnum));
			itemsz = IndexTupleSize(itup);
			itemsz = MAXALIGN(itemsz);

			/*
			 * Walk up the bucket chain, looking for a page big enough for
			 * this item and all other accumulated items.  Exit if we reach
			 * the read page.
			 */
			while (PageGetFreeSpaceForMultipleTuples(wpage, nitups + 1) < (all_tups_size + itemsz))
			{
				Buffer		next_wbuf = InvalidBuffer;
				bool		tups_moved = false;

				Assert(!PageIsEmpty(wpage));

				if (wblkno == bucket_blkno)
					retain_pin = true;

				wblkno = wopaque->hasho_nextblkno;
				Assert(BlockNumberIsValid(wblkno));

				/* don't need to move to next page if we reached the read page */
				if (wblkno != rblkno)
					next_wbuf = _hash_getbuf_with_strategy(rel,
														   wblkno,
														   HASH_WRITE,
														   LH_OVERFLOW_PAGE,
														   bstrategy);

				if (nitups > 0)
				{
					XLogRecPtr	recptr;

					Assert(nitups == ndeletable);

					/*
					 * This operation needs to log multiple tuples, prepare
					 * WAL for that.
					 */
					if (RelationNeedsWAL(rel))
						XLogEnsureRecordSpace(0, 3 + nitups);

					START_CRIT_SECTION();

					/*
					 * we have to insert tuples on the "write" page, being
					 * careful to preserve hashkey ordering.  (If we insert
					 * many tuples into the same "write" page it would be
					 * worth qsort'ing them).
					 */
					_hash_pgaddmultitup(rel, wbuf, itups, itup_offsets, nitups);
					MarkBufferDirty(wbuf);

					/* Delete tuples we already moved off read page */
					PageIndexMultiDelete(rpage, deletable, ndeletable);
					MarkBufferDirty(rbuf);

					/* XLOG stuff */
					if (RelationNeedsWAL(rel))
					{
						xl_hash_move_page_contents xlrec;

						xlrec.ntups = nitups;
						xlrec.is_prim_bucket_same_wrt = (wbuf == bucket_buf);

						XLogBeginInsert();
						XLogRegisterData(&xlrec, SizeOfHashMovePageContents);

						/*
						 * bucket buffer was not changed, but still needs to
						 * be registered to ensure that we can acquire a
						 * cleanup lock on it during replay.
						 */
						if (!xlrec.is_prim_bucket_same_wrt)
						{
							int			flags = REGBUF_STANDARD | REGBUF_NO_IMAGE | REGBUF_NO_CHANGE;

							XLogRegisterBuffer(0, bucket_buf, flags);
						}

						XLogRegisterBuffer(1, wbuf, REGBUF_STANDARD);
						XLogRegisterBufData(1, itup_offsets,
											nitups * sizeof(OffsetNumber));
						for (i = 0; i < nitups; i++)
							XLogRegisterBufData(1, itups[i], tups_size[i]);

						XLogRegisterBuffer(2, rbuf, REGBUF_STANDARD);
						XLogRegisterBufData(2, deletable,
											ndeletable * sizeof(OffsetNumber));

						recptr = XLogInsert(RM_HASH_ID, XLOG_HASH_MOVE_PAGE_CONTENTS);
					}
					else
						recptr = XLogGetFakeLSN(rel);

					PageSetLSN(BufferGetPage(wbuf), recptr);
					PageSetLSN(BufferGetPage(rbuf), recptr);

					END_CRIT_SECTION();

					tups_moved = true;
				}

				/*
				 * release the lock on previous page after acquiring the lock
				 * on next page
				 */
				if (retain_pin)
					LockBuffer(wbuf, BUFFER_LOCK_UNLOCK);
				else
					_hash_relbuf(rel, wbuf);

				/* nothing more to do if we reached the read page */
				if (rblkno == wblkno)
				{
					_hash_relbuf(rel, rbuf);
					return;
				}

				wbuf = next_wbuf;
				wpage = BufferGetPage(wbuf);
				wopaque = HashPageGetOpaque(wpage);
				Assert(wopaque->hasho_bucket == bucket);
				retain_pin = false;

				/* be tidy */
				for (i = 0; i < nitups; i++)
					pfree(itups[i]);
				nitups = 0;
				all_tups_size = 0;
				ndeletable = 0;

				/*
				 * after moving the tuples, rpage would have been compacted,
				 * so we need to rescan it.
				 */
				if (tups_moved)
					goto readpage;
			}

			/* remember tuple for deletion from "read" page */
			deletable[ndeletable++] = roffnum;

			/*
			 * we need a copy of index tuples as they can be freed as part of
			 * overflow page, however we need them to write a WAL record in
			 * _hash_freeovflpage.
			 */
			itups[nitups] = CopyIndexTuple(itup);
			tups_size[nitups++] = itemsz;
			all_tups_size += itemsz;
		}

		/*
		 * If we reach here, there are no live tuples on the "read" page ---
		 * it was empty when we got to it, or we moved them all.  So we can
		 * just free the page without bothering with deleting tuples
		 * individually.  Then advance to the previous "read" page.
		 *
		 * Tricky point here: if our read and write pages are adjacent in the
		 * bucket chain, our write lock on wbuf will conflict with
		 * _hash_freeovflpage's attempt to update the sibling links of the
		 * removed page.  In that case, we don't need to lock it again.
		 */
		rblkno = ropaque->hasho_prevblkno;
		Assert(BlockNumberIsValid(rblkno));

		/* free this overflow page (releases rbuf) */
		_hash_freeovflpage(rel, bucket_buf, rbuf, wbuf, itups, itup_offsets,
						   tups_size, nitups, bstrategy);

		/* be tidy */
		for (i = 0; i < nitups; i++)
			pfree(itups[i]);

		/* are we freeing the page adjacent to wbuf? */
		if (rblkno == wblkno)
		{
			/* retain the pin on primary bucket page till end of bucket scan */
			if (wblkno == bucket_blkno)
				LockBuffer(wbuf, BUFFER_LOCK_UNLOCK);
			else
				_hash_relbuf(rel, wbuf);
			return;
		}

		rbuf = _hash_getbuf_with_strategy(rel,
										  rblkno,
										  HASH_WRITE,
										  LH_OVERFLOW_PAGE,
										  bstrategy);
		rpage = BufferGetPage(rbuf);
		ropaque = HashPageGetOpaque(rpage);
		Assert(ropaque->hasho_bucket == bucket);
	}

	/* NOTREACHED */
}
