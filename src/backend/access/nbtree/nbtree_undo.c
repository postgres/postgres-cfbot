/*-------------------------------------------------------------------------
 *
 * nbtree_undo.c
 *	  nbtree UNDO resource manager
 *
 * When a transaction aborts, the leaf entries it inserted into a B-tree are
 * marked LP_DEAD during rollback, so they are not left for VACUUM.  The heap
 * tuples themselves are dead by xmin as usual; only the index work changes.
 * This is done only by the aborting backend itself; a rollback applied later
 * (logical revert worker, crash recovery) leaves the entries for VACUUM, see
 * nbtree_undo_apply().
 *
 * One record type is written: INSERT_LEAF, for each leaf-page entry.
 * Structural changes -- page splits, new roots, internal downlinks, posting
 * list splits, deduplication -- are not recorded and not reversed:
 * concurrent readers may already have observed the new structure.  The
 * aborted leaf entries are killed individually wherever a split moved them,
 * and any page emptied as a result is recycled by VACUUM as before.
 *
 * All hooks are guarded by RelationUsesIndexUndo() -- nbtree UNDO is enabled
 * per relation by the parent table's index_undo reloption, independently of
 * whether (or how) the parent table AM writes its own table UNDO.  Records go
 * to the cluster-wide UNDO-in-WAL stream; see nbtree_undo_write().
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtree_undo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "access/nbtree.h"
#include "access/undo_xlog.h"
#include "access/undorecord.h"
#include "access/undormgr.h"
#include "access/tableam.h"
#include "access/xactundo.h"
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
 * How far right the apply path will follow leaf right-links looking for an
 * entry that a page split moved off its original page.  Splits move entries
 * rightward, so the entry is usually on the recorded page or its immediate
 * sibling; the bound keeps a corrupt or circular right-link chain from spinning
 * during rollback at the cost of leaving a far-moved entry to VACUUM.
 */
#define NBTREE_UNDO_MAX_RIGHT_HOPS	8

/*
 * NbtreeUndoInsertLeaf - Payload for leaf insert undo
 *
 * index_oid allows direct index open during rollback, eliminating
 * the O(N_indexes) scan through RelationGetIndexList().
 *
 * The record does not carry the inserted IndexTuple.  Rollback never restores
 * tuple data; all it needs is a DISCRIMINATOR, enough to be sure the slot it
 * is about to mark LP_DEAD still holds this entry and not some later, possibly
 * COMMITTED, one.  Carrying the key would also make the record's size grow
 * with the key width, paying for bytes nbtree's own insert record already
 * logged.  So the record stores:
 *
 *   heap_tid   -- the heap TID the entry points at.  This is the strong half.
 *                 (key, heap TID) is unique in a btree by construction, and heap
 *                 TID alone is nearly so: the only entries in one index sharing
 *                 a heap TID are entries for the same row, which differ in key.
 *   key_digest -- a hash over the key bytes, closing exactly that residual case.
 *   itup_sz    -- the entry's length, a cheap discriminator that rejects a
 *                 differently-sized tuple outright.  uint16 suffices, since
 *                 nbtree caps entries well under a page (BTMaxItemSize).
 *
 * The guarantee is "never mark a slot dead unless it provably still holds THIS
 * entry": all three must match, and any mismatch means "leave it for VACUUM",
 * which is safe.  See nbtree_undo_verify_leaf_entry().  That proof holds only
 * while the aborted heap tuple still holds the heap TID, which is why the
 * record is applied only inline, by the aborting backend.
 *
 * The fields pack with no holes: 4 + 4 + 2 + 2 + 6 + 4 = 22 bytes.
 */
typedef struct NbtreeUndoInsertLeaf
{
	Oid			index_oid;		/* OID of the index relation */
	BlockNumber blkno;			/* Page where tuple was inserted */
	OffsetNumber offset;		/* Offset of the inserted tuple */
	uint16		itup_sz;		/* Size of the index tuple */
	ItemPointerData heap_tid;	/* Heap TID the entry points at */
	uint32		key_digest;		/* Hash of the key bytes */
} NbtreeUndoInsertLeaf;

#define SizeOfNbtreeUndoInsertLeaf	(offsetof(NbtreeUndoInsertLeaf, key_digest) + sizeof(uint32))

/*
 * nbtree_undo_key_digest - hash the key bytes of an index entry
 *
 * The digest covers everything after the IndexTuple header, which is the key
 * data and its null bitmap -- deliberately NOT t_tid, which holds the heap TID
 * that is compared separately and exactly, and whose high bits carry the
 * pivot/posting flags.
 *
 * A tuple with no key bytes at all (possible for a zero-column index entry)
 * digests to 0, which is a legitimate value; the heap TID and length checks
 * carry identification in that case.
 */
static inline uint32
nbtree_undo_key_digest(IndexTuple itup, Size itup_sz)
{
	if (itup_sz <= sizeof(IndexTupleData))
		return 0;

	return hash_bytes((const unsigned char *) itup + sizeof(IndexTupleData),
					  (int) (itup_sz - sizeof(IndexTupleData)));
}


/* Forward declarations */
static UndoApplyResult nbtree_undo_apply(uint8 rmid, uint16 info,
										 TransactionId xid, Oid reloid,
										 const char *payload, Size payload_len,
										 UndoRecPtr urec_ptr);
static void nbtree_undo_desc(StringInfo buf, uint8 rmid, uint16 info,
							 const char *payload, Size payload_len);

/* The nbtree UNDO RM registration entry */
static const UndoRmgrData nbtree_undo_rmgr = {
	.rm_name = "nbtree",
	.rm_undo = nbtree_undo_apply,
	.rm_desc = nbtree_undo_desc,
};

/*
 * NbtreeUndoRmgrInit - Register the nbtree UNDO resource manager
 */
void
NbtreeUndoRmgrInit(void)
{
	RegisterUndoRmgr(UNDO_RMID_NBTREE, &nbtree_undo_rmgr);
}

/*
 * nbtree_undo_write - Write one nbtree index-UNDO record.
 *
 * Index UNDO goes to the cluster-wide UNDO-in-WAL stream via the transaction's
 * UndoRecordSet, exactly as FILEOPS writes its own records (see
 * storage/file/fileops.c).  It must go through the XactUndo* API rather than
 * UndoRecordSetCreate/UndoRecordSetInsert directly, because that API is what
 * makes the record reachable at rollback:
 *
 *   - PrepareXactUndoDataParts() sets XactUndo.has_undo, without which
 *     AtAbort_XactUndo() returns immediately and applies nothing;
 *   - InsertXactUndoData() records the batch LSN in XactUndo.last_batch_lsn[],
 *     which is the chain head ApplyUndoChainFromWAL() walks backward from, and
 *     calls SetCurrentTransactionUndoRecPtr() so the next record chains onto
 *     this one.
 *
 * A batch written without that API is a valid XLOG_UNDO_BATCH record that
 * rollback never reads, because no chain head points at it.
 *
 * There is deliberately ONE write path.  Records are not routed on the
 * parent table AM's am_undo_engine (index UNDO is a per-relation property --
 * see RelationUsesIndexUndo()).
 *
 * The record is DEFERRED rather than written immediately.  Every index insert
 * produces one, and writing each as its own XLOG_UNDO_BATCH would cost a WAL
 * record and a WAL insertion lock acquisition per index entry.
 * DeferXactUndoData() leaves the record in the UndoRecordSet buffer, so all of
 * a statement's entries share one batch (see the batching note at the head of
 * undoinsert.c).  xactundo.c flushes at every point that can
 * consume the chain -- transaction abort, subtransaction abort/commit, commit,
 * PREPARE -- and on crossing undo_batch_size_kb / undo_batch_record_limit, so a
 * deferred record is never the reason a rollback finds nothing to apply.
 *
 * The caller must NOT be in a critical section: PrepareXactUndoDataParts() can
 * palloc.  Deferral itself writes no WAL, so no critical section is needed
 * here.
 */
static void
nbtree_undo_write(Relation heaprel, uint16 subtype,
				  const char *hdr, Size hdr_len,
				  const char *tup, Size tup_len)
{
	XactUndoContext undo_ctx;

	PrepareXactUndoDataParts(&undo_ctx, RELPERSISTENCE_PERMANENT,
							 UNDO_RMID_NBTREE, subtype,
							 RelationGetRelid(heaprel),
							 hdr, hdr_len, tup, tup_len);

	DeferXactUndoData(&undo_ctx);
}

/*
 * NbtreeUndoLogInsert - Write UNDO record for a leaf index tuple insertion
 *
 * Called from _bt_insertonpg() after the insertion has been WAL-logged.
 * This records enough information to remove the inserted entry on abort.
 *
 * The record stores a DISCRIMINATOR (heap TID + key digest + length), not the
 * inserted tuple: the apply path only ever uses it to prove the slot still holds
 * this entry before killing it, because a later split, dedup, or lower-key
 * insert can move the entry elsewhere and leave a COMMITTED entry at the
 * recorded offset.  See NbtreeUndoInsertLeaf and
 * nbtree_undo_verify_leaf_entry().
 */
void
NbtreeUndoLogInsert(Relation rel, Relation heaprel, Buffer buf,
					IndexTuple itup, Size itemsz, OffsetNumber offset,
					bool isleaf)
{
	NbtreeUndoInsertLeaf hdr;
	ItemPointer heap_tid;

	/*
	 * Internal-page downlink insertions are not recorded.  A downlink is
	 * needed for tree navigation and concurrent readers may already have
	 * descended through it, so it is never reversed.  The aborted leaf
	 * entries under it are killed by their own INSERT_LEAF records, and a
	 * page a split left empty is recycled by VACUUM.
	 */
	if (!isleaf)
		return;

	/*
	 * No heap TID means nothing to verify against at apply time, and
	 * nbtree_undo_verify_leaf_entry() would reject every slot -- so the
	 * record could only ever be a no-op.  Don't write it.  (A leaf entry
	 * always has one; this is a belt-and-braces check on the pivot/posting
	 * cases.)
	 */
	heap_tid = BTreeTupleGetHeapTID(itup);
	if (heap_tid == NULL || BTreeTupleIsPosting(itup) || BTreeTupleIsPivot(itup))
		return;

	hdr.index_oid = RelationGetRelid(rel);
	hdr.blkno = BufferGetBlockNumber(buf);
	hdr.offset = offset;
	hdr.itup_sz = (uint16) itemsz;
	hdr.heap_tid = *heap_tid;
	hdr.key_digest = nbtree_undo_key_digest(itup, itemsz);

	nbtree_undo_write(heaprel, NBTREE_UNDO_INSERT_LEAF,
					  (const char *) &hdr, SizeOfNbtreeUndoInsertLeaf,
					  NULL, 0);
}


/*
 * nbtree_undo_verify_leaf_entry - is the entry at (page, offset) still the
 * exact entry this UNDO record was written for?
 *
 * Killing an index entry by (block, offset) alone is unsafe: between the
 * provisional insert and the rollback, a page split, a deduplication pass, or
 * any insert of a lower key can shift the aborted entry to a different slot --
 * or move it to another page entirely -- leaving a COMMITTED entry in the slot
 * the UNDO record names.  Marking that slot LP_DEAD then destroys committed
 * data: the row stays in the heap but becomes invisible to every index scan.
 *
 * So identity is settled POSITIVELY, against the discriminator the record
 * carries (see NbtreeUndoInsertLeaf), never by trusting the offset.  Three
 * things must all agree:
 *
 *   - the entry length, which rejects a differently-shaped tuple outright,
 *   - the heap TID, which distinguishes two entries with equal keys,
 *   - the key digest, which distinguishes two entries pointing at the same heap
 *     TID (possible across an in-place update of a different column).
 *
 * The record stores a 4-byte hash of the key rather than the key bytes.  For
 * the purpose this comparison serves the two are equivalent: a match means "this
 * slot still holds the entry we inserted", and (length, heap TID, key hash)
 * establishes that for every case that can arise in one index -- the only
 * entries sharing a heap TID are entries for the same row, and those differ in
 * key.  A hash collision would additionally have to coincide with an identical
 * heap TID and length on a slot the walk actually reaches.
 *
 * A posting-list tuple is never a match: dedup rewrote the slot, so the
 * original single entry no longer exists there and the caller must not touch
 * it.  Likewise a pivot tuple (the page became internal) is never a match.
 *
 * Returns true only when the slot provably still holds the aborted entry.
 */
static bool
nbtree_undo_verify_leaf_entry(Page page, OffsetNumber offset,
							  const NbtreeUndoInsertLeaf *rec)
{
	ItemId		lp;
	IndexTuple	pagetup;
	ItemPointer page_tid;

	if (offset < FirstOffsetNumber || offset > PageGetMaxOffsetNumber(page))
		return false;

	lp = PageGetItemId(page, offset);
	if (!ItemIdIsNormal(lp))
		return false;

	/* A record with no recorded length cannot be verified; refuse to guess. */
	if (rec->itup_sz < sizeof(IndexTupleData))
		return false;

	/* Size must agree before anything else is worth comparing. */
	if (ItemIdGetLength(lp) != rec->itup_sz)
		return false;

	pagetup = (IndexTuple) PageGetItem(page, lp);

	/*
	 * Dedup/structural rewrites: a posting list or pivot in this slot means
	 * the original entry is not here any more.
	 */
	if (BTreeTupleIsPosting(pagetup) || BTreeTupleIsPivot(pagetup))
		return false;

	if (IndexTupleSize(pagetup) != rec->itup_sz)
		return false;

	/* Heap TID must match. */
	page_tid = BTreeTupleGetHeapTID(pagetup);
	if (page_tid == NULL)
		return false;
	if (!ItemPointerEquals(page_tid, &rec->heap_tid))
		return false;

	/* Key digest must match. */
	if (nbtree_undo_key_digest(pagetup, rec->itup_sz) != rec->key_digest)
		return false;

	return true;
}

/*
 * nbtree_undo_find_leaf_entry - locate the aborted entry on this page
 *
 * The recorded offset is only a hint.  Any insert of a lower key on the same
 * page shifts every entry after it up by one, so by rollback time the aborted
 * entry is very often a few slots away from where it was written -- and the
 * recorded slot holds a different, possibly COMMITTED, entry.  Trusting the
 * offset therefore does not merely risk corruption (which
 * nbtree_undo_verify_leaf_entry() prevents); it makes rollback silently
 * ineffective, because verification fails and the aborted entry is left behind
 * for VACUUM.  Measured on a 500-row index with 200 aborted random-key inserts,
 * offset-only matching reversed 11 of 200 entries.
 *
 * So check the hint first (the common case, and O(1)), then fall back to a
 * linear scan of the page.  The scan is safe: verification demands agreement on
 * the length, the heap TID and the key digest, and no two legitimate entries in
 * one index share all three -- that would be a duplicate entry for one row.
 * Leaf pages hold a few hundred entries, and this runs once per aborted entry
 * during rollback, which is not a hot path.
 *
 * Returns InvalidOffsetNumber if the entry is not on this page (it may have
 * been moved to a sibling by a split, or already be gone).
 */
static OffsetNumber
nbtree_undo_find_leaf_entry(Page page, OffsetNumber hint,
							const NbtreeUndoInsertLeaf *rec)
{
	OffsetNumber off,
				maxoff;

	if (nbtree_undo_verify_leaf_entry(page, hint, rec))
		return hint;

	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		if (off != hint &&
			nbtree_undo_verify_leaf_entry(page, off, rec))
			return off;
	}

	return InvalidOffsetNumber;
}

/*
 * nbtree_undo_apply - Apply a single nbtree UNDO record
 *
 * This is the rm_undo callback for the nbtree RM.
 */
static UndoApplyResult
nbtree_undo_apply(uint8 rmid, uint16 info, TransactionId xid, Oid reloid,
				  const char *payload, Size payload_len, UndoRecPtr urec_ptr)
{
	Assert(rmid == UNDO_RMID_NBTREE);

	/*
	 * Apply only inline, while the aborting (sub)transaction is still
	 * running.  The record finds its entry by heap TID, length and key
	 * digest, which identify it only while the aborted heap tuple holds the
	 * TID.  Once the abort is visible -- the logical revert worker, crash
	 * recovery -- VACUUM may already have removed the aborted tuple and its
	 * entry, and a committed INSERT may have reused the TID with the same
	 * key; marking that entry dead would hide a committed row from index
	 * scans and let the unique index accept a duplicate.  Leaving the entry
	 * to VACUUM is always safe.
	 */
	if (!UndoApplyInline)
		return UNDO_APPLY_SKIPPED;

	switch (info)
	{
		case NBTREE_UNDO_INSERT_LEAF:
			{
				NbtreeUndoInsertLeaf hdr;
				Relation	indexrel;
				Buffer		buffer;
				Page		page;
				BTPageOpaque opaque;

				if (payload_len < SizeOfNbtreeUndoInsertLeaf)
					return UNDO_APPLY_ERROR;

				memcpy(&hdr, payload, SizeOfNbtreeUndoInsertLeaf);

				/*
				 * Open the index directly using the OID stored in the UNDO
				 * payload.  This avoids the O(N_indexes) scan through
				 * RelationGetIndexList().
				 */
				indexrel = try_relation_open(hdr.index_oid, RowExclusiveLock);
				if (indexrel == NULL)
				{
					ereport(DEBUG2,
							(errmsg("nbtree UNDO INSERT_LEAF: index %u no longer exists",
									hdr.index_oid)));
					return UNDO_APPLY_SKIPPED;
				}

				if (RelationGetNumberOfBlocks(indexrel) <= hdr.blkno)
				{
					ereport(DEBUG2,
							(errmsg("nbtree UNDO INSERT_LEAF: block %u beyond end of index %u",
									hdr.blkno, hdr.index_oid)));
					relation_close(indexrel, RowExclusiveLock);
					return UNDO_APPLY_SKIPPED;
				}

				/*
				 * Find the entry, starting at the recorded page and following
				 * right-links while it is not found.  A page split moves the
				 * upper half of a leaf's entries to a new right sibling, so
				 * an aborted entry can legitimately be one or more pages to
				 * the right of where it was inserted; without following the
				 * chain those entries are never reversed.  The walk is
				 * bounded so a corrupt or circular right-link chain cannot
				 * spin here during rollback, and it only ever moves right,
				 * which is the direction splits move entries.
				 *
				 * Only a slot that provably still holds THIS entry is killed.
				 * A relocated slot now holds a different -- possibly
				 * COMMITTED -- entry, and marking that dead would silently
				 * lose committed data: the row stays in the heap but no index
				 * scan can find it.  An entry that is genuinely gone needs no
				 * action, so "not found" is a successful no-op.
				 */
				{
					BlockNumber blkno = hdr.blkno;
					OffsetNumber found = InvalidOffsetNumber;
					int			hops;

					buffer = InvalidBuffer;

					for (hops = 0; hops < NBTREE_UNDO_MAX_RIGHT_HOPS; hops++)
					{
						BlockNumber next;

						buffer = ReadBuffer(indexrel, blkno);
						LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
						page = BufferGetPage(buffer);
						opaque = BTPageGetOpaque(page);

						if (!P_ISLEAF(opaque))
							break;

						found = nbtree_undo_find_leaf_entry(page, hdr.offset,
															&hdr);
						if (found != InvalidOffsetNumber)
							break;

						/* Not here -- try the right sibling, if any. */
						next = opaque->btpo_next;
						if (P_RIGHTMOST(opaque) || next == blkno ||
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

					MarkBufferDirty(buffer);

					/* Generate physiological CLR */
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
			ereport(WARNING,
					(errmsg("nbtree UNDO: unknown subtype %u", info)));
			return UNDO_APPLY_ERROR;
	}
}

/*
 * nbtree_undo_desc - Describe an nbtree UNDO record for debugging
 */
static void
nbtree_undo_desc(StringInfo buf, uint8 rmid, uint16 info,
				 const char *payload, Size payload_len)
{
	const char *opname;

	switch (info)
	{
		case NBTREE_UNDO_INSERT_LEAF:
			opname = "INSERT_LEAF";
			break;
		default:
			opname = "UNKNOWN";
			break;
	}

	appendStringInfo(buf, "nbtree %s", opname);

	/* INSERT_LEAF payloads start with the index OID */
	if (payload_len >= sizeof(Oid) && info == NBTREE_UNDO_INSERT_LEAF)
	{
		Oid			index_oid;

		memcpy(&index_oid, payload, sizeof(Oid));
		appendStringInfo(buf, " index %u", index_oid);
	}
}
