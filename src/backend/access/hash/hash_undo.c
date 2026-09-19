/*-------------------------------------------------------------------------
 *
 * hash_undo.c
 *	  Hash index UNDO resource manager
 *
 * When a transaction aborts, the entries it inserted into a hash index are
 * marked LP_DEAD during rollback, so they are not left for VACUUM.  The heap
 * tuples themselves are dead by xmin as usual; only the index work changes.
 * This is done only by the aborting backend itself; a rollback applied later
 * (logical revert worker, crash recovery) leaves the entries for VACUUM, see
 * hash_undo_apply().
 *
 * One record type is written: INSERT, for each bucket or overflow page entry.
 *
 * All hooks are gated by RelationUsesIndexUndo(): hash UNDO is enabled per
 * relation by the parent table's index_undo reloption, exactly as for nbtree.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hash_undo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/relation.h"
#include "access/undo_xlog.h"
#include "access/undorecord.h"
#include "access/undormgr.h"
#include "access/xactundo.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"
#include "utils/rel.h"
#include "utils/relcache.h"

/*
 * Hash UNDO subtypes (stored in urec_info)
 */
#define HASH_UNDO_INSERT	0x0001	/* bucket/overflow page tuple insertion */

/*
 * HashUndoInsert - Payload for hash insert undo
 *
 * Like the nbtree leaf record, this stores a DISCRIMINATOR rather than the
 * inserted tuple: the apply path only ever uses it to prove a slot still holds
 * this entry before marking it LP_DEAD, never to restore data.
 *
 *   heap_tid   -- the heap pointer (a hash entry's t_tid).  The strong half.
 *   key_digest -- a hash over the stored hashkey bytes, which distinguishes two
 *                 entries pointing at the same heap TID.
 *   itup_sz    -- rejects a differently-shaped tuple outright.
 *
 * All three must match before a slot is marked; any mismatch means "leave it
 * for VACUUM", which is safe.  See hash_undo_verify_entry().  That proof holds
 * only while the aborted heap tuple still holds the heap TID, which is why the
 * record is applied only inline, by the aborting backend.
 */
typedef struct HashUndoInsert
{
	Oid			index_oid;		/* OID of the hash index relation */
	BlockNumber blkno;			/* Page where tuple was inserted */
	OffsetNumber offset;		/* Offset of the inserted tuple */
	uint16		itup_sz;		/* Size of the index tuple */
	ItemPointerData heap_tid;	/* Heap TID the entry points at */
	uint32		key_digest;		/* Hash of the stored key bytes */
} HashUndoInsert;

#define SizeOfHashUndoInsert \
	(offsetof(HashUndoInsert, key_digest) + sizeof(uint32))

/*
 * hash_undo_key_digest - hash the key bytes of a hash index entry
 *
 * Covers everything after the IndexTuple header -- for a hash index that is the
 * stored hashkey -- and deliberately not t_tid, which is compared separately and
 * exactly.
 */
static inline uint32
hash_undo_key_digest(IndexTuple itup, Size itup_sz)
{
	if (itup_sz <= sizeof(IndexTupleData))
		return 0;

	return hash_bytes((const unsigned char *) itup + sizeof(IndexTupleData),
					  (int) (itup_sz - sizeof(IndexTupleData)));
}

/* Forward declarations */
static UndoApplyResult hash_undo_apply(uint8 rmid, uint16 info,
									   TransactionId xid, Oid reloid,
									   const char *payload, Size payload_len,
									   UndoRecPtr urec_ptr);
static void hash_undo_desc(StringInfo buf, uint8 rmid, uint16 info,
						   const char *payload, Size payload_len);

/* The hash UNDO RM registration entry */
static const UndoRmgrData hash_undo_rmgr = {
	.rm_name = "hash",
	.rm_undo = hash_undo_apply,
	.rm_desc = hash_undo_desc,
};

/*
 * HashUndoRmgrInit - Register the hash UNDO resource manager
 */
void
HashUndoRmgrInit(void)
{
	RegisterUndoRmgr(UNDO_RMID_HASH, &hash_undo_rmgr);
}

/*
 * HashUndoLogInsert - Write UNDO record for a hash index tuple insertion
 *
 * Called from _hash_doinsert() after the insertion has been WAL-logged.
 * This records enough information to mark the inserted entry LP_DEAD on abort.
 *
 * Records go to the cluster-wide UNDO-in-WAL stream through the transaction's
 * UndoRecordSet, the same way FILEOPS writes its records.  The XactUndo* API is
 * what publishes the chain head (XactUndo.has_undo and
 * XactUndo.last_batch_lsn[]) that AtAbort_XactUndo() and
 * ApplyUndoChainFromWAL() need.  See nbtree_undo_write() in
 * access/nbtree/nbtree_undo.c for the full note.
 *
 * The record carries a discriminator (see HashUndoInsert) so the apply path can
 * prove the slot still holds this entry -- a bucket split or a later insert in
 * hashkey order can move it and leave a COMMITTED entry at the recorded offset.
 * See hash_undo_verify_entry().
 *
 * The caller must not be in a critical section; PrepareXactUndoDataParts() can
 * palloc.
 *
 * The record is DEFERRED (DeferXactUndoData) rather than written immediately, so
 * all of a statement's index entries share one XLOG_UNDO_BATCH instead of taking
 * one WAL record each.  See the fuller note in nbtree_undo_write().
 */
void
HashUndoLogInsert(Relation rel, Relation heapRel, Buffer buf,
				  OffsetNumber offset, IndexTuple itup, Size itemsz)
{
	HashUndoInsert hdr;
	XactUndoContext undo_ctx;

	hdr.index_oid = RelationGetRelid(rel);
	hdr.blkno = BufferGetBlockNumber(buf);
	hdr.offset = offset;
	hdr.itup_sz = (uint16) itemsz;
	hdr.heap_tid = itup->t_tid;
	hdr.key_digest = hash_undo_key_digest(itup, itemsz);

	PrepareXactUndoDataParts(&undo_ctx, RELPERSISTENCE_PERMANENT,
							 UNDO_RMID_HASH, HASH_UNDO_INSERT,
							 RelationGetRelid(heapRel),
							 (const char *) &hdr, SizeOfHashUndoInsert,
							 NULL, 0);

	DeferXactUndoData(&undo_ctx);
}

/*
 * hash_undo_verify_entry - is the entry at (page, offset) still the exact entry
 * this UNDO record was written for?
 *
 * Killing by (block, offset) alone is unsafe for a hash index: a bucket split
 * moves entries between the primary bucket page and its overflow pages, and
 * _hash_pgaddtup places new entries in hashkey order, so both the page and the
 * offset an aborted entry occupied can later belong to a different -- possibly
 * COMMITTED -- entry.  Marking that slot LP_DEAD would silently lose committed
 * data: the row stays in the heap but no hash scan can find it.
 *
 * The record carries a discriminator -- the heap TID (t_tid, which for a hash
 * index entry is the heap pointer), a digest of the stored hashkey bytes, and the
 * entry length -- and all three must agree.  It does not carry the tuple: the
 * comparison is the only use, so a fixed-size digest serves it as well as the
 * bytes would, and no two legitimate entries in one index share all three.
 *
 * Returns true only when the slot provably still holds the aborted entry.
 */
static bool
hash_undo_verify_entry(Page page, OffsetNumber offset,
					   const HashUndoInsert *rec)
{
	ItemId		lp;
	IndexTuple	pagetup;

	if (offset < FirstOffsetNumber || offset > PageGetMaxOffsetNumber(page))
		return false;

	lp = PageGetItemId(page, offset);
	if (!ItemIdIsNormal(lp))
		return false;

	/* A record with no recorded length cannot be verified; refuse to guess. */
	if (rec->itup_sz < sizeof(IndexTupleData))
		return false;

	if (ItemIdGetLength(lp) != rec->itup_sz)
		return false;

	pagetup = (IndexTuple) PageGetItem(page, lp);

	if (IndexTupleSize(pagetup) != rec->itup_sz)
		return false;

	/* Heap TID must match. */
	if (!ItemPointerEquals(&pagetup->t_tid, &rec->heap_tid))
		return false;

	/* Key digest (covering the stored hashkey) must match. */
	if (hash_undo_key_digest(pagetup, rec->itup_sz) != rec->key_digest)
		return false;

	return true;
}

/*
 * How far along a bucket's overflow chain the apply path will look for an entry
 * that moved off its original page.  Bounded so a corrupt or circular
 * hasho_nextblkno chain cannot spin during rollback; a far-moved entry is left
 * to VACUUM instead.
 */
#define HASH_UNDO_MAX_CHAIN_HOPS	8

/*
 * hash_undo_find_entry - locate the aborted entry on this page
 *
 * As in nbtree, the recorded offset is only a hint.  _hash_pgaddtup() inserts
 * in hashkey order, so any later insert with a smaller hashkey on the same page
 * shifts the aborted entry up a slot, leaving a different -- possibly COMMITTED
 * -- entry at the recorded offset.  Verification then fails and the aborted
 * entry survives the rollback, which is safe but useless: measured on a
 * 500-row hash index with 200 aborted inserts, offset-only matching reversed 28
 * of 200 entries.
 *
 * Check the hint first, then scan the page.  Verification requires agreement on
 * the length, the heap TID and the digest of the stored hashkey, and no two
 * legitimate entries share all three, so the scan cannot mis-identify an entry.
 *
 * Returns InvalidOffsetNumber if the entry is not on this page.
 */
static OffsetNumber
hash_undo_find_entry(Page page, OffsetNumber hint,
					 const HashUndoInsert *rec)
{
	OffsetNumber off,
				maxoff;

	if (hash_undo_verify_entry(page, hint, rec))
		return hint;

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		if (off != hint &&
			hash_undo_verify_entry(page, off, rec))
			return off;
	}

	return InvalidOffsetNumber;
}

/*
 * hash_undo_apply - Apply a single hash UNDO record
 *
 * This is the rm_undo callback for the hash RM.  On abort, marks the
 * inserted index entry as LP_DEAD.
 */
static UndoApplyResult
hash_undo_apply(uint8 rmid, uint16 info, TransactionId xid, Oid reloid,
				const char *payload, Size payload_len, UndoRecPtr urec_ptr)
{
	Assert(rmid == UNDO_RMID_HASH);

	/*
	 * Apply only inline, while the aborting (sub)transaction is still
	 * running: after the abort is visible the heap TID can hold a committed
	 * row with the same key.  See nbtree_undo_apply().
	 */
	if (!UndoApplyInline)
		return UNDO_APPLY_SKIPPED;

	switch (info)
	{
		case HASH_UNDO_INSERT:
			{
				HashUndoInsert hdr;
				Relation	indexrel;
				Buffer		buffer;
				Page		page;

				if (payload_len < SizeOfHashUndoInsert)
					return UNDO_APPLY_ERROR;

				memcpy(&hdr, payload, SizeOfHashUndoInsert);

				/*
				 * Open the index directly using the OID stored in the UNDO
				 * payload.
				 */
				indexrel = try_relation_open(hdr.index_oid, RowExclusiveLock);
				if (indexrel == NULL)
				{
					ereport(DEBUG2,
							(errmsg("hash UNDO INSERT: index %u no longer exists",
									hdr.index_oid)));
					return UNDO_APPLY_SKIPPED;
				}

				if (RelationGetNumberOfBlocks(indexrel) <= hdr.blkno)
				{
					ereport(DEBUG2,
							(errmsg("hash UNDO INSERT: block %u beyond end of index %u",
									hdr.blkno, hdr.index_oid)));
					relation_close(indexrel, RowExclusiveLock);
					return UNDO_APPLY_SKIPPED;
				}

				/*
				 * Find the entry, starting at the recorded page and following
				 * the bucket's overflow chain while it is not found.  A
				 * bucket split or an overflow allocation can move the aborted
				 * entry to another page in the same bucket; without walking
				 * the chain those entries are never reversed.
				 *
				 * Only a slot that provably still holds THIS entry is killed:
				 * a relocated slot may hold a COMMITTED entry, and marking
				 * that dead would make its heap row unreachable by any hash
				 * scan. An entry that is genuinely gone needs no action, so
				 * "not found" is a successful no-op.
				 */
				{
					BlockNumber blkno = hdr.blkno;
					OffsetNumber found = InvalidOffsetNumber;
					int			hops;

					buffer = InvalidBuffer;

					for (hops = 0; hops < HASH_UNDO_MAX_CHAIN_HOPS; hops++)
					{
						HashPageOpaque opaque;
						BlockNumber next;

						buffer = ReadBuffer(indexrel, blkno);
						LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
						page = BufferGetPage(buffer);

						if (PageIsNew(page) || PageGetSpecialSize(page) == 0)
							break;

						opaque = HashPageGetOpaque(page);
						if ((opaque->hasho_flag & LH_PAGE_TYPE) != LH_BUCKET_PAGE &&
							(opaque->hasho_flag & LH_PAGE_TYPE) != LH_OVERFLOW_PAGE)
							break;

						found = hash_undo_find_entry(page, hdr.offset,
													 &hdr);
						if (found != InvalidOffsetNumber)
							break;

						next = opaque->hasho_nextblkno;
						if (!BlockNumberIsValid(next) || next == blkno ||
							next >= RelationGetNumberOfBlocks(indexrel))
							break;

						UnlockReleaseBuffer(buffer);
						buffer = InvalidBuffer;
						blkno = next;
					}

					if (!BufferIsValid(buffer))
					{
						relation_close(indexrel, RowExclusiveLock);
						return UNDO_APPLY_SUCCESS;
					}

					page = BufferGetPage(buffer);

					if (found == InvalidOffsetNumber)
					{
						UnlockReleaseBuffer(buffer);
						relation_close(indexrel, RowExclusiveLock);
						return UNDO_APPLY_SUCCESS;
					}

					hdr.blkno = blkno;
					hdr.offset = found;
				}

				{
					ItemId		lp = PageGetItemId(page, hdr.offset);

					START_CRIT_SECTION();

					ItemIdMarkDead(lp);

					/*
					 * Set the page's dead-tuples hint, as _hash_kill_items
					 * does, so that an insert that finds the page full
					 * reclaims the entry (_hash_vacuum_one_page) rather than
					 * leaving it for VACUUM.  Like that hint, it is not
					 * replayed; hash_mask masks it.
					 */
					HashPageGetOpaque(page)->hasho_flag |= LH_PAGE_HAS_DEAD_TUPLES;

					MarkBufferDirty(buffer);

					/* Generate physiological CLR for crash recovery */
					if (RelationNeedsWAL(indexrel))
					{
						XLogRecPtr	clr_lsn;
						xl_undo_apply xlrec;

						xlrec.urec_ptr = urec_ptr;
						xlrec.xid = xid;
						xlrec.target_locator = indexrel->rd_locator;
						xlrec.target_block = hdr.blkno;
						xlrec.target_offset = hdr.offset;
						xlrec.operation_type = info;
						xlrec.clr_flags = UNDO_CLR_LP_DEAD;
						xlrec.tuple_len = 0;

						XLogBeginInsert();
						XLogRegisterData((char *) &xlrec,
										 SizeOfUndoApply);
						XLogRegisterBuffer(0, buffer,
										   REGBUF_STANDARD);
						clr_lsn = XLogInsert(RM_UNDO_ID,
											 XLOG_UNDO_APPLY_RECORD);
						PageSetLSN(page, clr_lsn);
					}

					END_CRIT_SECTION();
				}

				UnlockReleaseBuffer(buffer);
				relation_close(indexrel, RowExclusiveLock);
				return UNDO_APPLY_SUCCESS;
			}

		default:
			return UNDO_APPLY_SKIPPED;
	}
}

/*
 * hash_undo_desc - Describe a hash UNDO record for debugging
 */
static void
hash_undo_desc(StringInfo buf, uint8 rmid, uint16 info,
			   const char *payload, Size payload_len)
{
	const char *opname;

	switch (info)
	{
		case HASH_UNDO_INSERT:
			opname = "INSERT";
			break;
		default:
			opname = "UNKNOWN";
			break;
	}

	appendStringInfo(buf, "hash %s", opname);

	if (payload_len >= sizeof(Oid) && info == HASH_UNDO_INSERT)
	{
		Oid			index_oid;

		memcpy(&index_oid, payload, sizeof(Oid));
		appendStringInfo(buf, " index %u", index_oid);
	}
}
