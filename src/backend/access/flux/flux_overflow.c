/*-------------------------------------------------------------------------
 *
 * flux_overflow.c
 *	  FLUX-native column overflow: directory-in-root-chunk model.
 *
 * An over-threshold varlena column is stored out of the main tuple as a set
 * of DATA chunk records on normal FLUX data pages, plus one ROOT directory
 * chunk that lists every DATA chunk's (block,offset).  The main tuple holds a
 * compact FluxOverflowPtr naming the ROOT chunk.  Chunk records carry no MVCC
 * fields: they share the visibility of the parent tuple version that
 * references them (a chunk is live iff some live tuple's directory names it).
 *
 * Because the directory lists every block up front, the fetch path can
 * prefetch them through a read_stream; a record->record chain could not (the
 * next block is unknown until the current one is read).
 *
 * Every chunk record is written through the self-logged path: each chunk is
 * placed under its OWN buffer lock and its OWN XLOG_FLUX_OVERFLOW_WRITE record,
 * and the page is released before the next chunk is allocated.  A backend thus
 * never holds more than one overflow chunk-page lock at a time, so concurrent
 * wide writers cannot form a lock-order inversion (see the invariant on
 * FluxStoreOverflowColumn).  A crash between a chunk becoming durable and the
 * tuple ref committing leaks chunks, which VACUUM's orphan sweep reclaims.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_overflow.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/flux.h"
#include "access/flux_pbu.h"
#include "access/flux_undo.h"
#include "access/flux_xlog.h"
#include "miscadmin.h"
#include "access/xact.h"
#include "utils/hsearch.h"
#include "storage/procarray.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/read_stream.h"
#include "utils/memutils.h"
#include "common/hashfn.h"

/*
 * A chunk record as it sits on a FLUX page: a FluxOverflowRecordHeader
 * followed by its payload (value bytes for DATA, FluxOverflowDir + entries
 * for ROOT/INDIRECT).
 */

/*
 * flux_overflow_add_chunk_logged
 *		Allocate a FLUX page with room for one chunk, build the on-page record,
 *		WAL-log it in its OWN record (XLOG_FLUX_OVERFLOW_WRITE), and RELEASE the
 *		buffer before returning.  Because the buffer is released each call, a
 *		backend never holds more than one chunk-page lock.  The chunks become
 *		durable up front and the tuple ref (logged with the tuple op) commits
 *		last; a crash in between leaks chunks, which the orphan sweep reclaims.
 *
 *		held->avoid_block is the one page the caller already holds pinned and
 *		locked (its main tuple page).  This path must never pick that block -- a
 *		second LockBuffer on an already-locked page self-deadlocks (asserts
 *		under cassert) -- so it is marked full in the FSM and skipped.  held may
 *		be NULL.
 */
static bool
flux_overflow_block_is_held(const FluxOverflowBuffers *held, BlockNumber blk)
{
	if (held == NULL || blk == InvalidBlockNumber)
		return false;
	return held->avoid_block == blk;
}

static void
flux_overflow_add_chunk_logged(Relation rel, uint16 kind,
							   const char *payload, uint32 payload_len,
							   uint64 commit_ts,
							   const FluxOverflowBuffers *held,
							   BlockNumber *out_block, OffsetNumber *out_offset)
{
	BlockNumber target_block;
	Buffer		buffer;
	Page		page;
	OffsetNumber offnum;
	uint32		record_len = FLUX_OVERFLOW_RECORD_OVERHEAD + payload_len;
	uint32		aligned_len = MAXALIGN(record_len);
	char	   *record_data;
	FluxOverflowRecordHeader *hdr;

	if (record_len > FLUX_MAX_TUPLE_SIZE)
		elog(ERROR, "FLUX overflow chunk too large: %u", record_len);

	record_data = (char *) palloc0(record_len);
	hdr = (FluxOverflowRecordHeader *) record_data;
	hdr->or_magic = FLUX_OVERFLOW_RECORD_MAGIC;
	hdr->or_data_len = payload_len;
	hdr->or_xid = GetCurrentTransactionId();
	hdr->or_flags = kind;
	memcpy(record_data + FLUX_OVERFLOW_RECORD_OVERHEAD, payload, payload_len);

	/*
	 * Compare the ALIGNED length throughout.  PageAddItem stores each item at
	 * a MAXALIGN boundary, so it needs MAXALIGN(record_len) bytes of free
	 * space, not record_len.  Asking the FSM and rechecking the locked page
	 * with the raw record_len let a chunk up to 7 bytes too large pass on any
	 * page whose free space was not a multiple of 8, after which PageAddItem
	 * returned InvalidOffsetNumber -- the source of the "cannot place logged
	 * chunk" ERROR seen under concurrent force-shrink writes.  The main insert
	 * path (flux_handler.c) has always tolerated a too-full page by extending
	 * a fresh one; mirror that here instead of erroring.
	 */
	for (;;)
	{
		bool		fresh_page = false;

		target_block = FluxGetPageWithFreeSpace(rel, aligned_len);
		if (target_block == InvalidBlockNumber)
		{
			/* Race-safe extension (see FluxExtendRelationBuffer). */
			buffer = FluxExtendRelationBuffer(rel);
			page = BufferGetPage(buffer);
			fresh_page = true;
		}
		else
		{
			/*
			 * Never re-lock a page the caller already holds locked (its main
			 * tuple page): we would self-deadlock, and we release our own buffer below
			 * anyway.  Mark it full in the FSM so the next probe skips it, and
			 * retry.
			 */
			if (flux_overflow_block_is_held(held, target_block))
			{
				FluxRecordFreeSpace(rel, target_block, 0);
				continue;
			}
			buffer = ReadBuffer(rel, target_block);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buffer);
			if (PageIsNew(page))
			{
				FluxInitPage(page, BufferGetPageSize(buffer));
				fresh_page = true;
			}
			if (PageGetHeapFreeSpace(page) < aligned_len)
			{
				/* Stale/bucketed FSM answer: record real free space, retry. */
				FluxRecordFreeSpace(rel, target_block,
									PageGetHeapFreeSpace(page));
				UnlockReleaseBuffer(buffer);
				continue;
			}
		}

		offnum = PageAddItem(page, record_data, record_len,
							 InvalidOffsetNumber, false, false);
		if (offnum != InvalidOffsetNumber)
			break;

		/*
		 * PageAddItem failed despite an adequate aligned-length check.  On an
		 * existing page this must not error: record its real free space, drop
		 * it, and loop to extend a fresh page as the last resort (the insert
		 * path's shape).  On a freshly initialized page the chunk genuinely
		 * cannot fit, which is a real internal invariant violation -- report
		 * it accurately with the sizes, not the misleading legacy message.
		 */
		if (fresh_page)
			elog(ERROR,
				 "FLUX overflow: chunk of %u bytes (aligned %u) does not fit "
				 "on a fresh page with %zu bytes free",
				 record_len, aligned_len, PageGetHeapFreeSpace(page));

		FluxRecordFreeSpace(rel, BufferGetBlockNumber(buffer),
							PageGetHeapFreeSpace(page));
		UnlockReleaseBuffer(buffer);
	}

	START_CRIT_SECTION();

	/*
	 * Stamp the page commit timestamp inside the critical section, together
	 * with the buffer dirty + WAL insert whose redo re-applies it
	 * (flux_xlog_overflow_write_redo), so the page change and its WAL record
	 * are atomic with respect to a checkpoint.
	 */
	{
		FluxPageOpaque phdr = FluxPageGetOpaque(page);

		FluxPageSetCommitTs(phdr, Max(FluxPageGetCommitTs(phdr), commit_ts));
	}

	MarkBufferDirty(buffer);
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	recptr = FluxXLogOverflowWrite(rel, buffer, offnum,
												   record_data, record_len,
												   FLUX_OVERFLOW_WAL_NEW_RECORD,
												   commit_ts);

		PageSetLSN(page, recptr);
	}
	END_CRIT_SECTION();

	*out_block = BufferGetBlockNumber(buffer);
	*out_offset = offnum;
	FluxRecordFreeSpace(rel, BufferGetBlockNumber(buffer),
						PageGetHeapFreeSpace(page));
	UnlockReleaseBuffer(buffer);
	pfree(record_data);
}

/*
 * flux_overflow_store_logged
 *		Store a value's data chunks + directory entirely via self-logged
 *		records (no single-WAL-record size limit), supporting one indirection
 *		level.  Returns the ROOT chunk's (block,offset) via out_root_*.
 */
static void
flux_overflow_store_logged(Relation rel, const char *data, uint32 total_len,
						   uint32 chunk_size, uint64 commit_ts,
						   const FluxOverflowBuffers *held,
						   BlockNumber *out_root_block,
						   OffsetNumber *out_root_offset)
{
	uint32		nchunks = (total_len + chunk_size - 1) / chunk_size;
	FluxOverflowDirEnt *entries;
	uint32		i;
	uint32		off;

	if (nchunks == 0)
		nchunks = 1;
	entries = (FluxOverflowDirEnt *) palloc0(nchunks * sizeof(FluxOverflowDirEnt));

	/* Write every DATA chunk, each in its own WAL record. */
	off = 0;
	for (i = 0; i < nchunks; i++)
	{
		uint32		this_len = Min(chunk_size, total_len - off);
		BlockNumber blk;
		OffsetNumber offn;

		flux_overflow_add_chunk_logged(rel, FLUX_OVCHUNK_DATA,
									   data + off, this_len, commit_ts,
									   held, &blk, &offn);
		entries[i].block = blk;
		entries[i].offset = offn;
		entries[i].pad = 0;
		off += this_len;
	}

	if (nchunks <= FLUX_OVERFLOW_DIR_MAX_ENTRIES)
	{
		/* Single flat ROOT directory. */
		uint32		root_len = FLUX_OVERFLOW_DIR_HDR + nchunks * sizeof(FluxOverflowDirEnt);
		char	   *root_payload = (char *) palloc0(root_len);
		FluxOverflowDir *dir = (FluxOverflowDir *) root_payload;

		dir->od_total_length = total_len;
		dir->od_chunk_size = chunk_size;
		dir->od_nentries = nchunks;
		dir->od_levels = 0;
		dir->od_pad = 0;
		memcpy(root_payload + FLUX_OVERFLOW_DIR_HDR, entries,
			   nchunks * sizeof(FluxOverflowDirEnt));
		flux_overflow_add_chunk_logged(rel, FLUX_OVCHUNK_ROOT,
									   root_payload, root_len, commit_ts,
									   held, out_root_block, out_root_offset);
		pfree(root_payload);
	}
	else
	{
		/*
		 * One indirection level: group the data-chunk entries into INDIRECT
		 * directory chunks (each holding up to FLUX_OVERFLOW_DIR_MAX_ENTRIES
		 * entries), then a ROOT (od_levels=1) listing the INDIRECT chunks.
		 */
		uint32		fan = FLUX_OVERFLOW_DIR_MAX_ENTRIES;
		uint32		nind = (nchunks + fan - 1) / fan;
		FluxOverflowDirEnt *ind_ents = palloc0(nind * sizeof(FluxOverflowDirEnt));
		uint32		g;
		uint32		root_len;
		char	   *root_payload;
		FluxOverflowDir *dir;

		if (nind > FLUX_OVERFLOW_DIR_MAX_ENTRIES)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("FLUX overflow value exceeds two-level directory capacity")));

		for (g = 0; g < nind; g++)
		{
			uint32		start = g * fan;
			uint32		cnt = Min(fan, nchunks - start);
			uint32		ilen = FLUX_OVERFLOW_DIR_HDR + cnt * sizeof(FluxOverflowDirEnt);
			char	   *ipayload = palloc0(ilen);
			FluxOverflowDir *idir = (FluxOverflowDir *) ipayload;
			BlockNumber iblk;
			OffsetNumber ioffn;

			idir->od_total_length = total_len;
			idir->od_chunk_size = chunk_size;
			idir->od_nentries = cnt;
			idir->od_levels = 0;	/* entries are DATA chunks */
			idir->od_pad = 0;
			memcpy(ipayload + FLUX_OVERFLOW_DIR_HDR, &entries[start],
				   cnt * sizeof(FluxOverflowDirEnt));
			flux_overflow_add_chunk_logged(rel, FLUX_OVCHUNK_INDIRECT,
										   ipayload, ilen, commit_ts,
										   held, &iblk, &ioffn);
			ind_ents[g].block = iblk;
			ind_ents[g].offset = ioffn;
			ind_ents[g].pad = 0;
			pfree(ipayload);
		}

		root_len = FLUX_OVERFLOW_DIR_HDR + nind * sizeof(FluxOverflowDirEnt);
		root_payload = palloc0(root_len);
		dir = (FluxOverflowDir *) root_payload;
		dir->od_total_length = total_len;
		dir->od_chunk_size = chunk_size;
		dir->od_nentries = nind;
		dir->od_levels = 1;		/* entries are INDIRECT dir chunks */
		dir->od_pad = 0;
		memcpy(root_payload + FLUX_OVERFLOW_DIR_HDR, ind_ents,
			   nind * sizeof(FluxOverflowDirEnt));
		flux_overflow_add_chunk_logged(rel, FLUX_OVCHUNK_ROOT,
									   root_payload, root_len, commit_ts,
									   held, out_root_block, out_root_offset);
		pfree(root_payload);
		pfree(ind_ents);
	}

	pfree(entries);
}

/*
 * FluxStoreOverflowColumn
 *		Store a (detoasted) varlena value as DATA chunks + a ROOT directory,
 *		returning the inline varlena that holds the FluxOverflowPtr (+ optional
 *		inline prefix) to put in the main tuple.
 *
 * The caller passes a fully-detoasted, uncompressed varlena.
 *
 * LOCK-ORDERING INVARIANT: a backend never holds more than one overflow
 * chunk-page lock at a time, and never holds one while waiting for the main
 * tuple page.  Every chunk here is written through flux_overflow_store_logged
 * -> flux_overflow_add_chunk_logged, which locks a single chunk page, logs it
 * in its own record, and releases it before allocating the next chunk.  The
 * second half of the invariant is the callers' responsibility: INSERT forms
 * the tuple (writing all chunks) BEFORE locking its target page, and the
 * in-place UPDATE RELEASES the main page lock before forming/writing chunks
 * and re-locks + re-validates afterwards (both the normal and force-shrink
 * paths).  overflow_buffers->avoid_block names the one page a caller still
 * holds (its main tuple page) so a chunk is never placed or re-locked there.
 * This is what keeps concurrent wide writers from a lock-order inversion with
 * no deadlock detector (recovery/164).
 */
Datum
FluxStoreOverflowColumn(Relation rel, Datum value, int attnum,
						Size inline_prefix_size,
						FluxOverflowBuffers *overflow_buffers)
{
	struct varlena *vl = (struct varlena *) DatumGetPointer(value);
	char	   *data;
	uint32		total_len;
	uint32		chunk_size;
	BlockNumber root_block;
	OffsetNumber root_offset;
	uint64		commit_ts;
	Size		ptr_varsize;
	struct varlena *result;
	FluxOverflowPtr *ovp;
	uint32		content_hash;

	Assert(!VARATT_IS_EXTERNAL(vl));
	Assert(!VARATT_IS_COMPRESSED(vl));

	data = VARDATA_ANY(vl);
	total_len = VARSIZE_ANY_EXHDR(vl);

	/*
	 * Ensure per-transaction FLUX MVCC state exists before reading the DML
	 * timestamp.  The non-speculative insert/update paths all call
	 * FluxGetTransactionTimestamp() (which lazily initializes
	 * MyFluxXactState) before touching overflow; the speculative-insert path
	 * (INSERT ... ON CONFLICT) reaches this point first and would otherwise
	 * trap FluxGetDmlTimestamp's Assert(MyFluxXactState != NULL).  The
	 * timestamp is semantically needed: a speculative tuple that is later
	 * confirmed must carry the real DML timestamp in its overflow pages'
	 * commit-ts word.
	 */
	(void) FluxGetTransactionTimestamp();
	commit_ts = (uint64) FluxGetDmlTimestamp();

	/* Cheap COW prefilter over the whole value. */
	content_hash = hash_bytes((const unsigned char *) data, total_len);

	chunk_size = FLUX_OVERFLOW_MAX_CHUNK_SIZE;

	/*
	 * Write every DATA chunk and the (possibly indirect) ROOT directory via
	 * self-logged records: one chunk per lock, released before the next.  The
	 * tuple ref commits last; a crash before it leaves orphan chunks the
	 * VACUUM sweep reclaims, as it reclaims an aborted writer's chunks.
	 */
	flux_overflow_store_logged(rel, data, total_len, chunk_size, commit_ts,
							   overflow_buffers, &root_block, &root_offset);

	/* Build the inline overflow pointer (+ optional inline prefix). */
	if (inline_prefix_size > (Size) total_len)
		inline_prefix_size = total_len;
	ptr_varsize = VARHDRSZ + sizeof(FluxOverflowPtr) + inline_prefix_size;
	result = (struct varlena *) palloc0(ptr_varsize);
	SET_VARSIZE(result, ptr_varsize);
	ovp = (FluxOverflowPtr *) VARDATA(result);
	ovp->ov_magic = FLUX_OVERFLOW_PTR_MAGIC;
	ovp->ov_first_block = root_block;	/* names the ROOT directory chunk */
	ovp->ov_first_offset = root_offset;
	ovp->ov_inline_prefix = (uint16) inline_prefix_size;
	ovp->ov_total_length = total_len;
	ovp->ov_content_hash = content_hash;
	if (inline_prefix_size > 0)
		memcpy((char *) ovp + sizeof(FluxOverflowPtr), data, inline_prefix_size);

	return PointerGetDatum(result);
}

/*
 * Read one chunk record body into caller memory.  Returns the kind and copies
 * the payload (or_data_len bytes) into *out at *out_off, advancing *out_off.
 * For a ROOT/INDIRECT chunk the caller handles the payload itself via the
 * returned pointer in *payload (valid only while the buffer is pinned).
 */

/*
 * Lock an overflow chunk page for reading, unless the caller already holds it
 * locked.  A FLUX-native overflow chunk can share the very data page the
 * caller has content-locked -- e.g. an in-place UPDATE that resolves the old
 * tuple's overflow value for its logical-decoding image while holding the main
 * page's buffer lock, or a write-side store that already pinned+locked a page.
 * Re-locking a page we already hold trips the LockBuffer re-entry assertion.
 * Return true if we took the lock (the caller must release it with
 * flux_overflow_unlock_chunk); false if the page was already held by us.
 */
static bool
flux_overflow_lock_chunk(Buffer buf)
{
	if (BufferIsLockedByMe(buf))
		return false;
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	return true;
}

/*
 * Release an overflow chunk page read with flux_overflow_lock_chunk.  Drops
 * the content lock only if we took it (locked); always drops the pin this
 * fetch added.
 */
static void
flux_overflow_unlock_chunk(Buffer buf, bool locked)
{
	if (locked)
		UnlockReleaseBuffer(buf);
	else
		ReleaseBuffer(buf);
}

/*
 * flux_overflow_validate_record
 *		Validate an on-page overflow record before any reader trusts a length
 *		it carries.  One validator is used by every reader of an overflow
 *		record -- the fetch/reassembly path, the COW-verify deform, and the
 *		VACUUM reachability walk -- so a corrupt or truncated page can never
 *		drive an over-large palloc, an over-read past the item, or a memcpy that
 *		runs off the result buffer (the three unbounded reads this file shipped).
 *
 * item_len is ItemIdGetLength(iid) for the record's line pointer.  The checks,
 * in order, are: the header fits the item; the magic matches; the chunk kind
 * is the one the caller expects; and the record's own or_data_len fits inside
 * the item after the header.  For a directory kind (ROOT/INDIRECT) the payload
 * is a FluxOverflowDir, so we also require the directory header to fit and the
 * declared entry count (od_nentries) to fit within or_data_len -- the bound the
 * callers formerly trusted blind.
 *
 * Returns true and, for directory kinds, sets *nentries_out to the validated
 * entry count; returns false and sets *what to a short reason otherwise.  The
 * caller decides the consequence: a reader ereports ERRCODE_DATA_CORRUPTED, the
 * VACUUM sweep fails safe (WARNING, abandon reclaim).  This never ereports
 * itself, so a caller holding a buffer can release it first.
 */
static bool
flux_overflow_validate_record(const FluxOverflowRecordHeader *hdr, Size item_len,
							  uint16 expected_kind, uint32 *nentries_out,
							  const char **what)
{
	uint32		data_len;

	if (nentries_out != NULL)
		*nentries_out = 0;

	if (item_len < FLUX_OVERFLOW_RECORD_OVERHEAD)
	{
		*what = "item shorter than record header";
		return false;
	}
	if (hdr->or_magic != FLUX_OVERFLOW_RECORD_MAGIC)
	{
		*what = "bad record magic";
		return false;
	}
	if ((hdr->or_flags & FLUX_OVCHUNK_KIND_MASK) != expected_kind)
	{
		*what = "unexpected chunk kind";
		return false;
	}

	/* or_data_len must fit inside the item after the header. */
	data_len = hdr->or_data_len;
	if (data_len > item_len - FLUX_OVERFLOW_RECORD_OVERHEAD)
	{
		*what = "data length exceeds item";
		return false;
	}

	/* Directory records carry a FluxOverflowDir + od_nentries entries. */
	if (expected_kind == FLUX_OVCHUNK_ROOT ||
		expected_kind == FLUX_OVCHUNK_INDIRECT)
	{
		const FluxOverflowDir *dir;
		uint32		n;

		if (data_len < FLUX_OVERFLOW_DIR_HDR)
		{
			*what = "directory payload shorter than header";
			return false;
		}
		dir = (const FluxOverflowDir *) ((const char *) hdr +
										 FLUX_OVERFLOW_RECORD_OVERHEAD);
		n = dir->od_nentries;
		if (n > (data_len - FLUX_OVERFLOW_DIR_HDR) / sizeof(FluxOverflowDirEnt))
		{
			*what = "directory entry count exceeds payload";
			return false;
		}
		if (nentries_out != NULL)
			*nentries_out = n;
	}

	return true;
}

/*
 * Below this many data chunks a value's read_stream is too short to amortise
 * AIO-worker dispatch, so a plain per-chunk ReadBuffer is faster; at or above
 * it the stream's prefetch and I/O coalescing win.  Measured by cold full-scan
 * (io_method=worker, this box's NVMe): the crossover sits between ndata=131
 * (stream ~27% slower) and ndata=262 (stream ~11% faster).  See REPORT.md.
 */
#define FLUX_OVERFLOW_STREAM_MIN_CHUNKS	256

/*
 * Private state for the data-chunk read_stream: the ordered block list and a
 * cursor the callback advances.  One yielded block number == one pinned
 * buffer returned in order; read_stream itself tolerates repeated and
 * non-ascending blocks (it only coalesces a consecutive ascending run into a
 * single I/O), so the directory's natural order is handed through as-is.
 */
typedef struct FluxOverflowStreamPrivate
{
	BlockNumber *blocks;
	uint32		nblocks;
	uint32		next;
}			FluxOverflowStreamPrivate;

static BlockNumber
flux_overflow_stream_read_next(ReadStream *stream,
							   void *callback_private_data,
							   void *per_buffer_data)
{
	FluxOverflowStreamPrivate *p = (FluxOverflowStreamPrivate *) callback_private_data;

	(void) stream;
	(void) per_buffer_data;
	if (p->next >= p->nblocks)
		return InvalidBlockNumber;
	return p->blocks[p->next++];
}

/*
 * FluxFetchOverflowColumn
 *		Reassemble the full value named by an inline overflow pointer.
 *
 * Reads the ROOT directory (following one INDIRECT level if present) to gather
 * the ordered DATA chunk locations, then reads each chunk and concatenates its
 * payload.  Returns a palloc'd fully-detoasted varlena.
 */
Datum
FluxFetchOverflowColumn(Relation rel, const void *overflow_varlena)
{
	const FluxOverflowPtr *ovp = FluxGetOverflowPtr(overflow_varlena);
	Buffer		root_buf;
	bool		root_locked;
	Page		root_page;
	ItemId		root_iid;
	FluxOverflowRecordHeader *root_hdr;
	FluxOverflowDir *dir;
	FluxOverflowDirEnt *entries;
	uint32		nentries;
	uint32		total_len;
	char	   *out;
	struct varlena *result;
	FluxOverflowDirEnt *data_ents;
	uint32		ndata;
	uint32		i;
	uint32		written;
	ReadStream *stream = NULL;
	FluxOverflowStreamPrivate priv;
	BlockNumber *blocks = NULL;
	MemoryContext caller_cxt = CurrentMemoryContext;
	MemoryContext tmp_cxt;
	MemoryContext old_cxt;

	/*
	 * Do the working allocations (the directory entry arrays) in a private
	 * context we delete at the end, not in the caller's context: this fetch
	 * runs on ordinary read paths (seqscan deform, aggregation) whose current
	 * context can be a bump allocator, and bump forbids pfree().  The result
	 * varlena is allocated in the caller's context so it outlives us.
	 */
	tmp_cxt = AllocSetContextCreate(caller_cxt, "flux overflow fetch",
									ALLOCSET_SMALL_SIZES);
	old_cxt = MemoryContextSwitchTo(tmp_cxt);

	/* Read the ROOT directory chunk. */
	root_buf = ReadBuffer(rel, ovp->ov_first_block);
	root_locked = flux_overflow_lock_chunk(root_buf);
	root_page = BufferGetPage(root_buf);
	if (ovp->ov_first_offset < FirstOffsetNumber ||
		ovp->ov_first_offset > PageGetMaxOffsetNumber(root_page) ||
		!ItemIdIsNormal(PageGetItemId(root_page, ovp->ov_first_offset)))
	{
		bool		past_end = ovp->ov_first_offset < FirstOffsetNumber ||
			ovp->ov_first_offset > PageGetMaxOffsetNumber(root_page);

		flux_overflow_unlock_chunk(root_buf, root_locked);
		MemoryContextSwitchTo(old_cxt);
		MemoryContextDelete(tmp_cxt);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("FLUX overflow root chunk at %u/%u in relation \"%s\" %s",
								 ovp->ov_first_block, ovp->ov_first_offset,
								 RelationGetRelationName(rel),
								 past_end ? "is past the end of its page" :
								 "has no normal line pointer")));
	}
	root_iid = PageGetItemId(root_page, ovp->ov_first_offset);
	root_hdr = (FluxOverflowRecordHeader *) PageGetItem(root_page, root_iid);
	{
		const char *what = NULL;

		if (!flux_overflow_validate_record(root_hdr, ItemIdGetLength(root_iid),
										   FLUX_OVCHUNK_ROOT, &nentries, &what))
		{
			flux_overflow_unlock_chunk(root_buf, root_locked);
			MemoryContextSwitchTo(old_cxt);
			MemoryContextDelete(tmp_cxt);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("FLUX overflow root directory at %u/%u in relation \"%s\" is corrupt: %s",
									 ovp->ov_first_block, ovp->ov_first_offset,
									 RelationGetRelationName(rel), what)));
		}
	}

	dir = (FluxOverflowDir *) ((char *) root_hdr + FLUX_OVERFLOW_RECORD_OVERHEAD);
	total_len = dir->od_total_length;

	/* A varlena is at most MaxAllocSize; a longer length is corruption. */
	if (total_len > MaxAllocSize - VARHDRSZ)
	{
		flux_overflow_unlock_chunk(root_buf, root_locked);
		MemoryContextSwitchTo(old_cxt);
		MemoryContextDelete(tmp_cxt);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("FLUX overflow root directory at %u/%u in relation \"%s\" is corrupt: value length %u is too large",
								 ovp->ov_first_block, ovp->ov_first_offset,
								 RelationGetRelationName(rel), total_len)));
	}

	if (dir->od_levels == 0)
	{
		/* Flat directory: root entries ARE the data chunks. */
		entries = (FluxOverflowDirEnt *) palloc(nentries * sizeof(FluxOverflowDirEnt));
		memcpy(entries,
			   (char *) dir + FLUX_OVERFLOW_DIR_HDR,
			   nentries * sizeof(FluxOverflowDirEnt));
		flux_overflow_unlock_chunk(root_buf, root_locked);
		data_ents = entries;
		ndata = nentries;
	}
	else if (dir->od_levels == 1)
	{
		/*
		 * One indirection level: root entries name INDIRECT dir chunks; read
		 * each and concatenate its DATA entries into a flat list.
		 */
		FluxOverflowDirEnt *ind_ents;
		uint32		nind = nentries;
		uint32		cap;
		uint32		g;

		ind_ents = (FluxOverflowDirEnt *) palloc(nind * sizeof(FluxOverflowDirEnt));
		memcpy(ind_ents, (char *) dir + FLUX_OVERFLOW_DIR_HDR,
			   nind * sizeof(FluxOverflowDirEnt));
		flux_overflow_unlock_chunk(root_buf, root_locked);

		cap = (total_len + FLUX_OVERFLOW_MAX_CHUNK_SIZE - 1) / FLUX_OVERFLOW_MAX_CHUNK_SIZE + 1;
		entries = (FluxOverflowDirEnt *) palloc(cap * sizeof(FluxOverflowDirEnt));
		ndata = 0;
		for (g = 0; g < nind; g++)
		{
			Buffer		ib = ReadBuffer(rel, ind_ents[g].block);
			bool		ib_locked;
			Page		ipage;
			ItemId		iid;
			FluxOverflowRecordHeader *ihdr;
			FluxOverflowDir *idir;
			uint32		j;

			ib_locked = flux_overflow_lock_chunk(ib);
			ipage = BufferGetPage(ib);
			if (ind_ents[g].offset < FirstOffsetNumber ||
				ind_ents[g].offset > PageGetMaxOffsetNumber(ipage) ||
				!ItemIdIsNormal(PageGetItemId(ipage, ind_ents[g].offset)))
			{
				bool		past_end = ind_ents[g].offset < FirstOffsetNumber ||
					ind_ents[g].offset > PageGetMaxOffsetNumber(ipage);

				flux_overflow_unlock_chunk(ib, ib_locked);
				MemoryContextSwitchTo(old_cxt);
				MemoryContextDelete(tmp_cxt);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg_internal("FLUX overflow indirect chunk at %u/%u in relation \"%s\" %s",
										 ind_ents[g].block, ind_ents[g].offset,
										 RelationGetRelationName(rel),
										 past_end ? "is past the end of its page" :
										 "has no normal line pointer")));
			}
			iid = PageGetItemId(ipage, ind_ents[g].offset);
			ihdr = (FluxOverflowRecordHeader *) PageGetItem(ipage, iid);
			{
				const char *what = NULL;
				uint32		nind_ents;

				if (!flux_overflow_validate_record(ihdr, ItemIdGetLength(iid),
												   FLUX_OVCHUNK_INDIRECT,
												   &nind_ents, &what))
				{
					flux_overflow_unlock_chunk(ib, ib_locked);
					MemoryContextSwitchTo(old_cxt);
					MemoryContextDelete(tmp_cxt);
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg_internal("FLUX overflow indirect directory at %u/%u in relation \"%s\" is corrupt: %s",
											 ind_ents[g].block, ind_ents[g].offset,
											 RelationGetRelationName(rel), what)));
				}
				idir = (FluxOverflowDir *) ((char *) ihdr + FLUX_OVERFLOW_RECORD_OVERHEAD);
				for (j = 0; j < nind_ents; j++)
				{
					if (ndata >= cap)
					{
						flux_overflow_unlock_chunk(ib, ib_locked);
						MemoryContextSwitchTo(old_cxt);
						MemoryContextDelete(tmp_cxt);
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg_internal("FLUX overflow in relation \"%s\": indirect directory names more data chunks than the value's length allows",
												 RelationGetRelationName(rel))));
					}
					memcpy(&entries[ndata],
						   (char *) idir + FLUX_OVERFLOW_DIR_HDR + j * sizeof(FluxOverflowDirEnt),
						   sizeof(FluxOverflowDirEnt));
					ndata++;
				}
			}
			flux_overflow_unlock_chunk(ib, ib_locked);
		}
		pfree(ind_ents);
		data_ents = entries;
	}
	else
	{
		flux_overflow_unlock_chunk(root_buf, root_locked);
		MemoryContextSwitchTo(old_cxt);
		MemoryContextDelete(tmp_cxt);
		elog(ERROR, "FLUX overflow: unsupported directory depth %u", dir->od_levels);
	}

	result = (struct varlena *) MemoryContextAlloc(caller_cxt, VARHDRSZ + total_len);
	SET_VARSIZE(result, VARHDRSZ + total_len);
	out = VARDATA(result);

	/*
	 * Read each DATA chunk in directory order and copy its payload into
	 * place.  A value with many data chunks (>=
	 * FLUX_OVERFLOW_STREAM_MIN_CHUNKS) is read through a read_stream so AIO
	 * prefetches the pages ahead of us; shorter values use a plain per-chunk
	 * ReadBuffer, because a short stream costs more in AIO-worker dispatch
	 * than it saves (see the #define).  The stream yields one pinned buffer
	 * per block in the order the callback emits them, so buffer i belongs to
	 * data_ents[i].  We never re-lock a page this backend already holds (an
	 * in-place UPDATE resolving its own old value can content-lock a data
	 * page before fetching): the held-aware lock helper reads straight from
	 * the pinned buffer and the unlock helper drops the pin the stream added.
	 */
	written = 0;
	if (ndata >= FLUX_OVERFLOW_STREAM_MIN_CHUNKS)
	{
		blocks = (BlockNumber *) palloc(ndata * sizeof(BlockNumber));
		for (i = 0; i < ndata; i++)
			blocks[i] = data_ents[i].block;
		priv.blocks = blocks;
		priv.nblocks = ndata;
		priv.next = 0;
		stream = read_stream_begin_relation(READ_STREAM_DEFAULT,
											NULL,
											rel,
											MAIN_FORKNUM,
											flux_overflow_stream_read_next,
											&priv,
											0);
	}
	for (i = 0; i < ndata; i++)
	{
		Buffer		buf;
		bool		buf_locked;
		Page		page;
		ItemId		iid;
		FluxOverflowRecordHeader *hdr;
		const char *what = NULL;

		CHECK_FOR_INTERRUPTS();

		if (stream != NULL)
		{
			buf = read_stream_next_buffer(stream, NULL);
			if (!BufferIsValid(buf))
				elog(ERROR, "FLUX overflow: stream exhausted at chunk %u of %u",
					 i, ndata);
		}
		else
			buf = ReadBuffer(rel, data_ents[i].block);
		buf_locked = flux_overflow_lock_chunk(buf);
		page = BufferGetPage(buf);
		if (data_ents[i].offset < FirstOffsetNumber ||
			data_ents[i].offset > PageGetMaxOffsetNumber(page))
		{
			flux_overflow_unlock_chunk(buf, buf_locked);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("FLUX overflow data chunk at %u/%u in relation \"%s\" is past the end of its page",
									 data_ents[i].block, data_ents[i].offset,
									 RelationGetRelationName(rel))));
		}
		iid = PageGetItemId(page, data_ents[i].offset);
		if (!ItemIdIsNormal(iid))
		{
			flux_overflow_unlock_chunk(buf, buf_locked);
			elog(ERROR, "FLUX overflow: data chunk lp invalid at %u/%u",
				 data_ents[i].block, data_ents[i].offset);
		}
		hdr = (FluxOverflowRecordHeader *) PageGetItem(page, iid);

		/*
		 * Validate the DATA record before trusting or_data_len: it must fit
		 * the item, and the running total must stay <= total_len so the
		 * memcpy can never run off the result buffer (the sum == total_len
		 * guard below runs only AFTER the copies, too late to catch an
		 * oversized chunk).
		 */
		if (!flux_overflow_validate_record(hdr, ItemIdGetLength(iid),
										   FLUX_OVCHUNK_DATA, NULL, &what) ||
			(uint64) written + hdr->or_data_len > total_len)
		{
			flux_overflow_unlock_chunk(buf, buf_locked);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("FLUX overflow data chunk %u/%u in relation \"%s\" is corrupt: %s",
									 data_ents[i].block, data_ents[i].offset,
									 RelationGetRelationName(rel),
									 what ? what : "chunk length exceeds total value length")));
		}
		memcpy(out + written,
			   (char *) hdr + FLUX_OVERFLOW_RECORD_OVERHEAD,
			   hdr->or_data_len);
		written += hdr->or_data_len;
		flux_overflow_unlock_chunk(buf, buf_locked);
	}

	if (stream != NULL)
	{
		Buffer		extra = read_stream_next_buffer(stream, NULL);

		if (BufferIsValid(extra))
		{
			ReleaseBuffer(extra);
			read_stream_end(stream);
			elog(ERROR, "FLUX overflow: read_stream returned more buffers than chunks");
		}
		read_stream_end(stream);
	}

	if (written != total_len)
		elog(ERROR, "FLUX overflow: reassembled %u of %u bytes",
			 written, total_len);

	MemoryContextSwitchTo(old_cxt);
	MemoryContextDelete(tmp_cxt);
	return PointerGetDatum(result);
}

/*
 * FluxCollectOverflowPtrsByAttr
 *		Fill out_ptrs[attnum]/out_present[attnum] with each attribute's inline
 *		overflow pointer, for the in-place UPDATE COW path.  Deforms with a
 *		NULL relation so overflow attributes come back as their raw inline
 *		FluxOverflowPtr varlena (not fetched), then records those.
 */
void
FluxCollectOverflowPtrsByAttr(FluxTupleHeader *tuple_hdr, TupleDesc tupdesc,
							  FluxOverflowPtr *out_ptrs, bool *out_present,
							  int natts)
{
	FluxTupleData tup;
	Datum	   *values;
	bool	   *isnull;
	int			i;

	for (i = 0; i < natts; i++)
		out_present[i] = false;

	if (tuple_hdr == NULL ||
		(tuple_hdr->t_flags & FLUX_TUPLE_HAS_OVERFLOW) == 0)
		return;

	values = (Datum *) palloc(tupdesc->natts * sizeof(Datum));
	isnull = (bool *) palloc(tupdesc->natts * sizeof(bool));

	tup.t_len = 0;				/* length is derived from the header on deform */
	tup.t_data = tuple_hdr;
	ItemPointerSetInvalid(&tup.t_self);

	/* NULL rel: overflow attrs returned verbatim (as FluxOverflowPtr varlena) */
	FluxDeformTuple(NULL, &tup, tupdesc, values, isnull);

	for (i = 0; i < natts && i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (isnull[i] || att->attisdropped || att->attlen != -1)
			continue;
		if (FluxIsOverflowPtr(DatumGetPointer(values[i])))
		{
			out_ptrs[i] = *FluxGetOverflowPtr(DatumGetPointer(values[i]));
			out_present[i] = true;
		}
	}

	pfree(values);
	pfree(isnull);
}

/*
 * FluxOverflowReachKey -- (block,offset) identity of a chunk record.
 *
 * This is a HASH_BLOBS key: dynahash hashes and compares all sizeof() bytes,
 * including any padding.  BlockNumber (4 bytes) + OffsetNumber (2 bytes) would
 * leave 2 implicit pad bytes that the compiler never initializes; a key built
 * on the stack would then carry stack garbage in them, so HASH_ENTER (marking
 * a chunk reachable) and a later HASH_FIND for the same (block,offset) could
 * land in different buckets / fail the memcmp -- and VACUUM would free a still
 * reachable chunk as an orphan (data loss: "root chunk lp invalid").  Make the
 * pad explicit so every construction site can zero it, and assert there is no
 * other hidden padding.
 */
typedef struct FluxOverflowReachKey
{
	BlockNumber block;
	OffsetNumber offset;
	uint16		pad;			/* must be zeroed; see comment above */
}			FluxOverflowReachKey;

StaticAssertDecl(sizeof(FluxOverflowReachKey) ==
				 sizeof(BlockNumber) + sizeof(OffsetNumber) + sizeof(uint16),
				 "FluxOverflowReachKey must have no implicit padding");

/*
 * flux_overflow_mark_reachable
 *		Insert every chunk (DATA + directory, all levels) that a live tuple's
 *		overflow reference reaches into the reachable set.
 */
static bool flux_overflow_mark_reachable(Relation rel, const FluxOverflowPtr *ovp, HTAB *reach);
static bool flux_overflow_mark_chain_reachable(Relation rel,
											   const char *onpage_image, Size onpage_len,
											   TupleDesc tupdesc, HTAB *reach,
											   TransactionId oldest_xmin);

/*
 * flux_overflow_mark_chain_reachable
 *		Mark reachable the overflow chunks of every before-image in a tuple's
 *		UNDO version chain, so VACUUM does not reclaim a superseded wide value
 *		an old snapshot can still read through the chain.
 *
 * A FLUX in-place UPDATE keeps the newest version on the page and pushes the
 * prior image to the per-backend UNDO log (t_verptr).  When the wide column
 * changes, the OLD version's inline FluxOverflowPtr -- naming the old chunks
 * -- lives only in that before-image, not in any on-page tuple, so Pass 1's
 * page scan would miss it and Pass 2 would reclaim the old chunks while an
 * old snapshot still needs them (the "root chunk lp invalid" / stale read).
 *
 * We carry the current candidate image (first the on-page tuple, then each
 * reconstructed prior image) exactly as the MVCC read path does, and at each
 * step reverse-apply the UNDO record with the shared FluxUndoPriorImage.  A
 * FLUX_UNDO_UPDATE_DELTA is reconstructed onto the candidate, NOT skipped: a
 * delta is emitted whenever the byte diff is smaller than the full image, for
 * ANY column including the wide one, so stopping at a delta would leave an old
 * wide value's chunks unmarked and let Pass 2 free them under a live snapshot.
 *
 * Where the walk stops: once it reaches a record whose writer -- the xid that
 * SUPERSEDED the prior image -- is committed and precedes oldest_xmin, every
 * snapshot sees that supersession or later, so the prior image and everything
 * older is invisible to all and its chunks are reclaimable; the walk stops
 * there WITHOUT marking that prior image.  This is the same visibility horizon
 * Pass 2 reclaims against (FluxGetOldestXminHorizon), so reachability and
 * reclaim use one criterion and cannot disagree.  The walk MUST NOT instead
 * key off FluxPbuFetchVersion returning "discarded": that tracks the
 * asynchronous discard worker's progress, not the sweep's horizon, so a slow
 * discard would leave a dead-to-all version's chunks marked reachable this
 * cycle and freed only on a later VACUUM -- which, if that VACUUM runs while an
 * old snapshot is held, reports freeing chunks the sweep should have freed
 * earlier (observed on 32-bit, where the discard worker lags deterministically:
 * recovery/167).  A fetch that returns false is still a valid end (the record
 * really is gone, hence older than any horizon).
 *
 * Any OTHER early exit (a malformed/unreadable record) is uncertainty: we must
 * never free on uncertainty, so we return false and the caller abandons
 * reclaim for this relation rather than risk freeing a reachable chunk.  A
 * frozen/all-visible version has had its t_verptr cleared, so the walk ends
 * and the now-unreferenced old chunks become reclaimable.  The walk is bounded
 * against a corrupt cycle.
 *
 * Returns true if the chain was walked to its natural end (reclaim may
 * proceed); false if a record could not be stepped past (reclaim must be
 * abandoned for safety).
 */
static bool
flux_overflow_mark_chain_reachable(Relation rel,
								   const char *onpage_image, Size onpage_len,
								   TupleDesc tupdesc, HTAB *reach,
								   TransactionId oldest_xmin)
{
	uint64		steps = 0;
	const uint64 step_limit = 1u << 20; /* bound a corrupt chain */
	char	   *candidate;
	Size		candidate_len;
	char	   *allocated = NULL;
	UndoRecPtr	verptr;
	bool		ok = true;

	if (onpage_image == NULL || onpage_len < SizeOfFluxTupleHeader)
		return true;

	/*
	 * Start from the on-page image.  Its own overflow pointers are already
	 * marked by Pass 1's page scan; here we chase the versions behind it.
	 */
	candidate = (char *) onpage_image;
	candidate_len = onpage_len;
	verptr = FluxTupleGetVersionPtr((const FluxTupleHeader *) candidate);

	while (UndoRecPtrIsValid(verptr) && steps++ < step_limit)
	{
		TransactionId urec_xid;
		CommandId	urec_cid;
		uint16		subtype;
		char	   *payload = NULL;
		Size		payload_size = 0;
		char	   *prior = NULL;
		Size		prior_len = 0;
		const char *what = NULL;
		FluxTupleHeader *before;
		FluxOverflowPtr *ovp;
		bool	   *present;
		int			nov,
					a;

		CHECK_FOR_INTERRUPTS();

		if (!FluxPbuFetchVersion(verptr, &urec_xid, &urec_cid, &subtype,
								 &payload, &payload_size))
			break;				/* the record is gone: older than any horizon */

		/*
		 * urec_xid is the writer that superseded the PRIOR image this record
		 * carries.  If it is committed and precedes oldest_xmin, every snapshot
		 * sees that supersession or later, so the prior image is invisible to
		 * all and reclaimable: stop here without marking it.  Stopping on the
		 * sweep's own horizon -- not on whether the undo record has been
		 * discarded yet -- makes reachability match Pass 2's reclaim criterion
		 * and no longer depend on the asynchronous discard worker's progress.
		 */
		if (TransactionIdIsNormal(urec_xid) &&
			TransactionIdPrecedes(urec_xid, oldest_xmin) &&
			!TransactionIdIsInProgress(urec_xid) &&
			TransactionIdDidCommit(urec_xid))
		{
			if (payload != NULL)
				pfree(payload);
			break;
		}

		/*
		 * Reverse-apply the record onto the candidate to get the prior image,
		 * reconstructing a delta if that is what this record is.  A record we
		 * cannot step past is uncertainty, not an end: fail safe.
		 */
		if (!FluxUndoPriorImage(subtype, payload, payload_size,
								candidate, candidate_len, &prior, &prior_len, &what))
		{
			ereport(WARNING,
					(errmsg("FLUX overflow in \"%s\": version chain unreadable at undo %llu (%s); skipping orphan reclaim",
							RelationGetRelationName(rel),
							(unsigned long long) verptr, what)));
			if (payload != NULL)
				pfree(payload);
			ok = false;
			break;
		}
		if (payload != NULL)
			pfree(payload);

		if (prior_len < SizeOfFluxTupleHeader)
		{
			/* A short image we cannot interpret: fail safe. */
			ereport(WARNING,
					(errmsg("FLUX overflow in \"%s\": prior image too short at undo %llu; skipping orphan reclaim",
							RelationGetRelationName(rel),
							(unsigned long long) verptr)));
			pfree(prior);
			ok = false;
			break;
		}

		/* Mark this before-image's overflow chunks reachable. */
		before = (FluxTupleHeader *) prior;
		nov = tupdesc->natts;
		ovp = palloc(nov * sizeof(FluxOverflowPtr));
		present = palloc(nov * sizeof(bool));
		FluxCollectOverflowPtrsByAttr(before, tupdesc, ovp, present, nov);
		for (a = 0; a < nov; a++)
			if (present[a])
				if (!flux_overflow_mark_reachable(rel, &ovp[a], reach))
					ok = false;
		pfree(ovp);
		pfree(present);

		/*
		 * A corrupt directory in this before-image means we could not
		 * enumerate its live chunks: fail safe (the end-of-function cleanup
		 * frees the adopted candidate; free this not-yet-adopted image).
		 */
		if (!ok)
		{
			pfree(prior);
			break;
		}

		/* The prior image becomes the candidate; step to its own prior. */
		if (allocated != NULL)
			pfree(allocated);
		allocated = prior;
		candidate = prior;
		candidate_len = prior_len;
		verptr = FluxTupleGetVersionPtr(before);
	}

	/* Stopped at the bound with chain left: a cycle or corruption, not an end. */
	if (ok && steps > step_limit)
	{
		ereport(WARNING,
				(errmsg("FLUX overflow in \"%s\": version chain longer than %llu records; skipping orphan reclaim",
						RelationGetRelationName(rel), (unsigned long long) step_limit)));
		ok = false;
	}

	if (allocated != NULL)
		pfree(allocated);
	return ok;
}


/*
 * flux_overflow_mark_reachable
 *		Insert every chunk (DATA + directory, all levels) that a live tuple's
 *		overflow reference reaches into the reachable set.
 *
 * Fail-safe for the VACUUM sweep: returns true if the whole directory could
 * be walked (reclaim may proceed), false on ANY uncertainty -- a bad magic,
 * an unexpected chunk kind, or an entry count that does not fit the record.
 * On false the caller must abandon reclaim for this relation, because a chunk
 * the sweep could not enumerate as reachable would otherwise be freed while a
 * live tuple still names it.  Never frees and never ERRORs out of VACUUM over
 * a corrupt page; it warns and lets the caller fail safe.
 */
static bool
flux_overflow_mark_reachable(Relation rel, const FluxOverflowPtr *ovp, HTAB *reach)
{
	FluxOverflowReachKey key;
	Buffer		rb;
	Page		rp;
	ItemId		riid;
	FluxOverflowRecordHeader *rhdr;
	FluxOverflowDir *dir;
	uint32		n,
				g;
	FluxOverflowDirEnt *ents;
	uint16		levels;
	const char *what = NULL;
	bool		ok = true;

	/* Zero the padding so HASH_ENTER here and HASH_FIND in pass 2 agree. */
	memset(&key, 0, sizeof(key));

	/* The ROOT chunk itself is reachable. */
	key.block = ovp->ov_first_block;
	key.offset = ovp->ov_first_offset;
	hash_search(reach, &key, HASH_ENTER, NULL);

	rb = ReadBuffer(rel, ovp->ov_first_block);
	LockBuffer(rb, BUFFER_LOCK_SHARE);
	rp = BufferGetPage(rb);
	if (ovp->ov_first_offset > PageGetMaxOffsetNumber(rp))
	{
		UnlockReleaseBuffer(rb);
		what = "root offset past end of page";
		goto fail;
	}
	riid = PageGetItemId(rp, ovp->ov_first_offset);
	if (!ItemIdIsNormal(riid))
	{
		UnlockReleaseBuffer(rb);
		what = "root chunk line pointer not normal";
		goto fail;
	}
	rhdr = (FluxOverflowRecordHeader *) PageGetItem(rp, riid);
	if (!flux_overflow_validate_record(rhdr, ItemIdGetLength(riid),
									   FLUX_OVCHUNK_ROOT, &n, &what))
	{
		UnlockReleaseBuffer(rb);
		goto fail;
	}
	dir = (FluxOverflowDir *) ((char *) rhdr + FLUX_OVERFLOW_RECORD_OVERHEAD);
	levels = dir->od_levels;
	ents = (FluxOverflowDirEnt *) palloc(n * sizeof(FluxOverflowDirEnt));
	memcpy(ents, (char *) dir + FLUX_OVERFLOW_DIR_HDR,
		   n * sizeof(FluxOverflowDirEnt));
	UnlockReleaseBuffer(rb);

	if (levels == 0)
	{
		/* entries are DATA chunks */
		for (g = 0; g < n; g++)
		{
			key.block = ents[g].block;
			key.offset = ents[g].offset;
			hash_search(reach, &key, HASH_ENTER, NULL);
		}
	}
	else
	{
		/* entries are INDIRECT dir chunks; each lists DATA chunks */
		for (g = 0; g < n; g++)
		{
			Buffer		ib;
			Page		ip;
			ItemId		iiid;
			FluxOverflowRecordHeader *ihdr;
			FluxOverflowDir *idir;
			uint32		j,
						m;
			FluxOverflowDirEnt *ients;

			CHECK_FOR_INTERRUPTS();

			key.block = ents[g].block;	/* the indirect chunk is reachable */
			key.offset = ents[g].offset;
			hash_search(reach, &key, HASH_ENTER, NULL);

			ib = ReadBuffer(rel, ents[g].block);
			LockBuffer(ib, BUFFER_LOCK_SHARE);
			ip = BufferGetPage(ib);
			if (ents[g].offset > PageGetMaxOffsetNumber(ip))
			{
				UnlockReleaseBuffer(ib);
				what = "indirect offset past end of page";
				pfree(ents);
				goto fail;
			}
			iiid = PageGetItemId(ip, ents[g].offset);
			if (!ItemIdIsNormal(iiid))
			{
				UnlockReleaseBuffer(ib);
				what = "indirect chunk line pointer not normal";
				pfree(ents);
				goto fail;
			}
			ihdr = (FluxOverflowRecordHeader *) PageGetItem(ip, iiid);

			/*
			 * Validate the INDIRECT record and bound its entry count before
			 * trusting od_nentries -- the asymmetry with the fetch path that
			 * finding 6 flags: an unchecked od_nentries here over-reads the
			 * page and, worse, leaves real chunks unmarked so the sweep frees
			 * live data.
			 */
			if (!flux_overflow_validate_record(ihdr, ItemIdGetLength(iiid),
											   FLUX_OVCHUNK_INDIRECT, &m, &what))
			{
				UnlockReleaseBuffer(ib);
				pfree(ents);
				goto fail;
			}
			idir = (FluxOverflowDir *) ((char *) ihdr + FLUX_OVERFLOW_RECORD_OVERHEAD);
			ients = (FluxOverflowDirEnt *) palloc(m * sizeof(FluxOverflowDirEnt));
			memcpy(ients, (char *) idir + FLUX_OVERFLOW_DIR_HDR,
				   m * sizeof(FluxOverflowDirEnt));
			UnlockReleaseBuffer(ib);
			for (j = 0; j < m; j++)
			{
				key.block = ients[j].block;
				key.offset = ients[j].offset;
				hash_search(reach, &key, HASH_ENTER, NULL);
			}
			pfree(ients);
		}
	}
	pfree(ents);
	return ok;

fail:
	ereport(WARNING,
			(errmsg("FLUX overflow in \"%s\": overflow directory at %u/%u unreadable (%s); skipping orphan reclaim",
					RelationGetRelationName(rel),
					ovp->ov_first_block, ovp->ov_first_offset,
					what ? what : "corrupt")));
	return false;
}

/*
 * FluxOverflowCheckOrphans
 *		Detect (and optionally reclaim) overflow chunk records reachable by no
 *		live tuple's overflow directory -- leaked by a crash between a large
 *		value's self-logged chunks and the tuple-ref commit, or by an
 *		interrupted VACUUM/UPDATE.  These are NOT corruption (a crash can
 *		create them legitimately), so they are reported as a NOTICE.
 *
 * Each chunk carries its creating XID (FluxOverflowRecordHeader.or_xid); an
 * orphan is reclaimed only once that XID is older than the
 * oldest snapshot that could still be mid-insert, so a chunk written by an
 * in-flight transaction that has not yet committed its tuple ref is never
 * stolen.
 *
 * Returns the number of orphans found; *reclaimed gets how many were freed.
 */
int
FluxOverflowCheckOrphans(Relation rel, bool reclaim, int64 *reclaimed_out)
{
	HASHCTL		ctl;
	HTAB	   *reach;
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	BlockNumber blk;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			norphans = 0;
	int64		nreclaimed = 0;
	TransactionId oldest = GetOldestNonRemovableTransactionId(rel);
	TransactionId oldest_xmin = FluxGetOldestXminHorizon(rel);
	bool		abort_reclaim = false;	/* a chain was unreadable: do not free */

	/*
	 * snapshotConflictHorizon for the pages this sweep frees, so a hot
	 * standby cancels a query that could still reach a freed chunk through
	 * the replayed UNDO chain (heap logs the newest removed xid on prune for
	 * the same reason; FLUX VACUUM Phase III passes vac_horizon).  The exact
	 * value is the newest xid that made any freed chunk unreachable -- the
	 * superseding writer that stopped a chain walk, a dead tuple's deleter,
	 * or an orphan chunk's creating xid.  Pass 2 frees per page but only sees
	 * a chunk's creating or_xid there, not the xid that unreferenced it, so
	 * threading the exact per-chunk value through is awkward.
	 *
	 * shortcut: use TransactionIdRetreat(oldest_xmin) as a conservative upper
	 * bound instead of the exact newest-removed xid.  Every chunk this sweep
	 * frees is unreachable precisely because the xid that unreferenced it
	 * precedes oldest_xmin (the chain walk stops, and Pass 2 reclaims, only
	 * below that horizon), so the newest such xid is <= oldest_xmin - 1; a
	 * larger horizon only cancels standby queries that did not strictly need
	 * cancelling, never too few.  Upgrade to the exact per-page newest xid if
	 * the extra standby-query cancellations ever matter: track it alongside
	 * the reachable set (a (block,offset) -> unreachability-xid map filled in
	 * Pass 1's walk and the dead-tuple/orphan branches) and Max() it per page
	 * in Pass 2.
	 */
	TransactionId sweep_conflict_horizon = oldest_xmin;

	if (TransactionIdIsValid(sweep_conflict_horizon))
		TransactionIdRetreat(sweep_conflict_horizon);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(FluxOverflowReachKey);
	ctl.entrysize = sizeof(FluxOverflowReachKey);
	ctl.hcxt = CurrentMemoryContext;
	reach = hash_create("flux overflow reachable", 1024, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * Pass 1: collect every live tuple's overflow refs, then (after releasing
	 * the page lock) mark the chunks they reach.  The collect runs under the
	 * page SHARE lock; the mark must NOT -- it locks other (or the same)
	 * overflow pages and would self-deadlock if a chunk shares this page.
	 */
	for (blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf = ReadBuffer(rel, blk);
		Page		page;
		OffsetNumber off,
					maxoff;
		FluxOverflowPtr *refs = NULL;
		int			nrefs = 0;
		int			caprefs = 0;
		int			r;
		char	  **chainimgs = NULL;	/* on-page image of each chained tuple */
		Size	   *chainlens = NULL;
		int			nchain = 0;
		int			capchain = 0;
		int			c;

		CHECK_FOR_INTERRUPTS();

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			FluxTupleHeader *hdr;
			int			nov,
						a;
			FluxOverflowPtr *ovp;
			bool	   *present;

			if (!ItemIdIsNormal(iid))
				continue;
			if (FluxIsOverflowRecordInline(PageGetItem(page, iid),
										   ItemIdGetLength(iid)))
				continue;		/* overflow records handled in pass 2 */
			hdr = (FluxTupleHeader *) PageGetItem(page, iid);

			/*
			 * A version that is dead to every snapshot keeps nothing alive:
			 * its own chunks and its whole UNDO chain are reclaimable.  Any
			 * other version -- live, or deleted/superseded but still visible
			 * to some snapshot -- must keep both its current wide value AND
			 * every superseded wide value its UNDO chain can still reach.
			 * (Pass 1 formerly skipped every FLUX_TUPLE_DELETED tuple, which
			 * dropped the before-image chunks an old snapshot still needs.)
			 */
			if (FluxTupleDeadToAll(hdr, oldest_xmin))
				continue;

			if (hdr->t_flags & FLUX_TUPLE_HAS_OVERFLOW)
			{
				nov = tupdesc->natts;
				ovp = palloc(nov * sizeof(FluxOverflowPtr));
				present = palloc(nov * sizeof(bool));
				FluxCollectOverflowPtrsByAttr(hdr, tupdesc, ovp, present, nov);
				for (a = 0; a < nov; a++)
				{
					if (!present[a])
						continue;
					if (nrefs >= caprefs)
					{
						caprefs = caprefs ? caprefs * 2 : 16;
						refs = refs ? repalloc(refs, caprefs * sizeof(FluxOverflowPtr))
							: palloc(caprefs * sizeof(FluxOverflowPtr));
					}
					refs[nrefs++] = ovp[a];
				}
				pfree(ovp);
				pfree(present);
			}

			/*
			 * Copy the on-page image of every chained tuple to walk after the
			 * page lock is dropped (the walk reads UNDO and other overflow
			 * pages).  We must copy, not keep a page pointer: the walk needs the
			 * current image as the starting candidate for reverse-applying the
			 * UNDO chain (including reconstructing deltas), and the page lock is
			 * released below.  The copy is palloc'd, hence MAXALIGNed, so the
			 * FluxTupleHeader cast and FluxCollectOverflowPtrsByAttr see an
			 * aligned image.  A version with no chain, or one whose chain was
			 * trimmed when it froze, contributes nothing and the walk ends at
			 * once.
			 */
			if ((hdr->t_flags & FLUX_TUPLE_HAS_VERSION_PTR) &&
				UndoRecPtrIsValid(FluxTupleGetVersionPtr(hdr)))
			{
				Size		imglen = ItemIdGetLength(iid);
				char	   *imgcopy;

				if (nchain >= capchain)
				{
					capchain = capchain ? capchain * 2 : 16;
					chainimgs = chainimgs ?
						repalloc(chainimgs, capchain * sizeof(char *))
						: palloc(capchain * sizeof(char *));
					chainlens = chainlens ?
						repalloc(chainlens, capchain * sizeof(Size))
						: palloc(capchain * sizeof(Size));
				}
				imgcopy = palloc(imglen);
				memcpy(imgcopy, (char *) hdr, imglen);
				chainimgs[nchain] = imgcopy;
				chainlens[nchain] = imglen;
				nchain++;
			}
		}
		UnlockReleaseBuffer(buf);

		/* Mark reachable with NO page lock held. */
		for (r = 0; r < nrefs; r++)
			if (!flux_overflow_mark_reachable(rel, &refs[r], reach))
				abort_reclaim = true;
		for (c = 0; c < nchain; c++)
		{
			if (!flux_overflow_mark_chain_reachable(rel, chainimgs[c],
														chainlens[c], tupdesc, reach,
														oldest_xmin))
				abort_reclaim = true;
			pfree(chainimgs[c]);
		}
		if (refs)
			pfree(refs);
		if (chainimgs)
			pfree(chainimgs);
		if (chainlens)
			pfree(chainlens);
	}

	/*
	 * Pass 2: any overflow record not in the reachable set is an orphan.
	 *
	 * Freeing must be OFFSET-STABLE.  Overflow chunks live on ordinary FLUX
	 * data pages alongside regular tuples and other values' chunks, all named
	 * by (block,offset) in directories and by TID in indexes.  The stock
	 * PageIndexTupleDelete / PageIndexMultiDelete renumber the surviving line
	 * pointers (and assert every one has storage, which a FLUX page's
	 * LP_UNUSED/LP_DEAD items violate): using them corrupts those references
	 * -- the "invalid index offnum" / ItemIdHasStorage crash and the stale
	 * "root chunk lp invalid" reads.  Instead we free each orphan with
	 * ItemIdSetUnused and reclaim the space with FluxPageDefragment, exactly
	 * as FLUX VACUUM frees dead tuples, which keeps every surviving offset
	 * fixed.
	 */
	for (blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf = ReadBuffer(rel, blk);
		Page		page;
		OffsetNumber off,
					maxoff;
		OffsetNumber deloffs[MaxOffsetNumber];
		int			ndel = 0;

		CHECK_FOR_INTERRUPTS();

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			FluxOverflowRecordHeader *ohdr;
			FluxOverflowReachKey key;
			bool		found;
			TransactionId cxid;

			if (!ItemIdIsNormal(iid))
				continue;
			if (!FluxIsOverflowRecordInline(PageGetItem(page, iid),
											ItemIdGetLength(iid)))
				continue;
			/* Zero the padding to match the HASH_ENTER key exactly. */
			memset(&key, 0, sizeof(key));
			key.block = blk;
			key.offset = off;
			(void) hash_search(reach, &key, HASH_FIND, &found);
			if (found)
				continue;		/* reachable: live */

			/* Orphan. */
			norphans++;
			ohdr = (FluxOverflowRecordHeader *) PageGetItem(page, iid);
			cxid = ohdr->or_xid;
			ereport(DEBUG2,
					(errmsg("FLUX overflow: orphan chunk at %u/%u (creating xid %u)",
							blk, off, cxid)));

			/*
			 * Reclaim only when the creating XID can no longer be mid-insert:
			 * older than the oldest non-removable xid (so no in-flight writer
			 * still owns these not-yet-referenced chunks).  Collect the
			 * offset now; the whole page is freed and defragmented once
			 * below.
			 */
			if (reclaim && !abort_reclaim &&
				(!TransactionIdIsValid(cxid) ||
				 TransactionIdPrecedes(cxid, oldest)))
				deloffs[ndel++] = off;
		}
		if (ndel > 0)
		{
			int			d;

			/*
			 * Free the orphan chunks the way FLUX VACUUM frees dead tuples
			 * (Phase III): ItemIdSetUnused each one, then FluxPageDefragment
			 * (PageRepairFragmentation) to reclaim the data space.  This is
			 * OFFSET-STABLE: unlike PageIndexTupleDelete, it never renumbers
			 * the surviving line pointers, so the directories of other live
			 * tuples' chunks on this page -- and any regular FLUX tuple whose
			 * TID an index references -- stay valid.  Logged with a forced
			 * full-page image via FluxXLogDefrag, so a standby and crash
			 * recovery reproduce the exact page and wal_consistency_checking
			 * stays clean.
			 */
			START_CRIT_SECTION();
			for (d = 0; d < ndel; d++)
				ItemIdSetUnused(PageGetItemId(page, deloffs[d]));
			FluxPageDefragment(page);
			MarkBufferDirty(buf);
			if (RelationNeedsWAL(rel))
			{
				XLogRecPtr	recptr;

				recptr = FluxXLogDefrag(rel, buf, NULL, 0, 0,
										sweep_conflict_horizon);
				PageSetLSN(page, recptr);
			}
			END_CRIT_SECTION();
			nreclaimed += ndel;
		}
		UnlockReleaseBuffer(buf);
	}

	hash_destroy(reach);
	if (reclaimed_out)
		*reclaimed_out = nreclaimed;

	/*
	 * One summary line (a crash can create orphans legitimately, so this is a
	 * NOTICE, not a corruption report).  Per-chunk detail is at DEBUG2.
	 */
	if (norphans > 0)
		ereport(NOTICE,
				(errmsg("FLUX overflow in \"%s\": %d orphan chunk(s) found, %lld reclaimed",
						RelationGetRelationName(rel), norphans,
						(long long) nreclaimed)));
	return norphans;
}
