/*-------------------------------------------------------------------------
 *
 * flux_undo.h
 *	  Public interface for the FLUX UNDO resource manager
 *
 * FLUX writes its UNDO to the per-backend UNDO engine
 * (access/undo/perbackend) through the flux_pbu.c shim, under its own UNDO
 * resource manager (UNDO_RMID_FLUX).  The engine dispatches rollback to
 * flux_undo_apply() by the rmid stamped into each record.  The same records
 * are the version chain that readers walk through t_verptr.
 *
 * Visibility of an aborted transaction's rows does not depend on the UNDO
 * having been applied: readers treat a writer that did not commit as
 * invisible (CLOG, FluxWriterVisibleToSnapshot) and walk past its versions.
 * Applying the UNDO restores the before-images on the page.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/access/flux_undo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FLUX_UNDO_H
#define FLUX_UNDO_H

#include "common/relpath.h"		/* RelFileNumber */
#include "access/flux.h"
#include "access/undodefs.h"
#include "access/undormgr.h"
#include "nodes/pg_list.h"
#include "storage/buf.h"
#include "storage/itemptr.h"
#include "utils/relcache.h"

/*
 * FLUX's UNDO resource-manager ID.  Defined in FLUX's own header, not in the
 * generic access/undormgr.h, so the UNDO core names no specific consumer.
 * See access/undormgr.h for the shared, WAL-durable ID number space.
 */
#define UNDO_RMID_FLUX		5

/*
 * FLUX UNDO subtypes.  Values occupy the 16-bit urec_info field of
 * the UNDO record header and are orthogonal to the FLUX WAL opcodes
 * in flux_xlog.h.
 */
#define FLUX_UNDO_UPDATE			0x0002	/* full-tuple before-image */
#define FLUX_UNDO_DELETE			0x0003	/* restore deleted tuple */
#define FLUX_UNDO_UPDATE_DELTA		0x0004	/* byte-diff before-image: a
											 * FluxUndoDelta describing the
											 * changed middle region of the
											 * old image relative to the new
											 * on-page image. Reconstructed to
											 * a FULL old image at apply/read
											 * time, then handled by the same
											 * path as FLUX_UNDO_UPDATE. */

/*
 * Common fixed-length header for every FLUX UNDO payload.  The
 * variable-length tuple / diff image (if any) follows immediately
 * after the header.
 *
 * The header is deliberately small and self-describing so the same
 * struct is the FLUX before-image payload header
 * avoiding an intermediate palloc.
 */
/*
 * One entry of the index list area (FLUX_UNDO_FLAG_INDEX_LIST): an index the
 * UPDATE's index maintenance may change, and the file it changes.  A REINDEX
 * later in the transaction gives the index a new file, and rolling it back
 * makes this one current again, so the rollback must reverse the entries here.
 * Stored unaligned; four 4-byte fields, no padding.
 */
typedef struct FluxUndoIndex
{
	Oid			indexoid;
	Oid			spcoid;			/* the file's tablespace */
	RelFileNumber relnumber;
	uint32		flags;			/* FLUX_UNDO_INDEX_* */
} FluxUndoIndex;

StaticAssertDecl(sizeof(FluxUndoIndex) == 16, "FluxUndoIndex has padding");

/* Maintenance revived a delete-marked entry instead of inserting one. */
#define FLUX_UNDO_INDEX_REVIVED		0x0001

typedef struct FluxUndoPayloadHeader
{
	ItemPointerData tid;		/* target tuple id */
	TransactionId writer_xid;	/* xid (possibly a SUBXACT xid) that produced
								 * this record's HOST image.  Distinct from
								 * the per-backend record's uur_xid, which
								 * must stay the owning TOP full xid for the
								 * engine's abort bookkeeping.  The MVCC
								 * version-chain walk and the lost-update
								 * conflict probe key off THIS xid so
								 * subtransaction (SAVEPOINT) rollback
								 * resolves correctly. */
	uint32		tuple_len;		/* length of trailing tuple/diff image */
	uint16		flags;
	uint16		pad;			/* reserved index-list area length */
	RelFileNumber relnumber;	/* relation file the change was made in; the
								 * record does not apply to any other file of
								 * the same relation (TRUNCATE, rewrite) */
} FluxUndoPayloadHeader;

#define SizeOfFluxUndoPayloadHeader	(sizeof(FluxUndoPayloadHeader))

/* flags bits */
#define FLUX_UNDO_FLAG_HAS_TUPLE		0x0001
#define FLUX_UNDO_FLAG_INDEX_LIST		0x0002

/*
 * FluxUndoDelta -- the payload of a FLUX_UNDO_UPDATE_DELTA record (it follows
 * the FluxUndoPayloadHeader, exactly where the full before-image would sit for
 * a FLUX_UNDO_UPDATE record).
 *
 * The old image and the new on-page image are compared as byte strings; the
 * common prefix and common suffix are dropped and only the differing middle of
 * the OLD image is stored.  This is the same prefix/suffix shape FluxXLogUpdate
 * computes for redo, and it covers BOTH the same-size CAS fast path and the
 * size-changing path (old_len may differ from the current on-page length).
 *
 * Reconstruction of the full old image from the current on-page image (cur):
 *
 *   middle_len = old_len - prefixlen - suffixlen
 *   old = cur[0 .. prefixlen)
 *       ++ old_middle[0 .. middle_len)
 *       ++ cur[cur_len - suffixlen .. cur_len)
 *
 * so the result is exactly old_len bytes.  Because every in-place UPDATE
 * stamps a fresh t_verptr at FIXED offset 8 (see flux.h FluxTupleHeader), the
 * changed middle ALWAYS covers offset 8, so the reconstructed image carries
 * the OLD t_verptr and the per-tuple version chain continues verbatim.
 */
typedef struct FluxUndoDelta
{
	uint32		old_len;		/* length of the reconstructed old image */
	uint16		prefixlen;		/* bytes of common prefix kept from current */
	uint16		suffixlen;		/* bytes of common suffix kept from current */
	/* old_middle[old_len - prefixlen - suffixlen] follows */
} FluxUndoDelta;

#define SizeOfFluxUndoDelta	(offsetof(FluxUndoDelta, suffixlen) + sizeof(uint16))

/*
 * FluxUndoDeltaCompute
 *		Compute the prefix/suffix delta of old_image (old_len bytes) relative
 *		to new_image (new_len bytes) into *out and its trailing middle bytes.
 *
 * out_cap is the size of the out buffer; it must be at least
 * SizeOfFluxUndoDelta + old_len for the worst case.  Returns the total delta
 * record size on success, or 0 if the delta is not strictly smaller than the
 * full old image (caller must then emit a full FLUX_UNDO_UPDATE).
 *
 * force_offset, if in-range, is a byte offset that MUST fall inside the
 * recorded middle (prefixlen <= force_offset < old_len - suffixlen).  Callers
 * pass the t_verptr offset (8) so the middle always covers the version pointer,
 * which the version chain relies on even in the rare case the stamped verptr
 * happens to equal the old one.  Pass a value >= old_len to disable.
 */
static inline Size
FluxUndoDeltaCompute(const char *old_image, uint32 old_len,
					 const char *new_image, uint32 new_len,
					 uint32 force_offset,
					 char *out, Size out_cap)
{
	uint32		minlen = (old_len < new_len) ? old_len : new_len;
	uint32		prefixlen = 0;
	uint32		suffixlen = 0;
	uint32		middle_len;
	FluxUndoDelta *d;
	Size		total;

	/* Common prefix. */
	while (prefixlen < minlen &&
		   old_image[prefixlen] == new_image[prefixlen])
		prefixlen++;

	/* Common suffix, not overlapping the prefix on either image. */
	while (suffixlen < minlen - prefixlen &&
		   old_image[old_len - 1 - suffixlen] ==
		   new_image[new_len - 1 - suffixlen])
		suffixlen++;

	/*
	 * Force the given offset into the middle so the recorded old bytes always
	 * include it (used for the fixed t_verptr offset).
	 */
	if (force_offset < old_len)
	{
		if (prefixlen > force_offset)
			prefixlen = force_offset;
		if (old_len - suffixlen <= force_offset)
			suffixlen = old_len - 1 - force_offset;
	}

	middle_len = old_len - prefixlen - suffixlen;
	total = SizeOfFluxUndoDelta + middle_len;

	/*
	 * Only worthwhile if the delta record is strictly smaller than the full
	 * before-image would be, and it fits the caller's buffer.
	 */
	if (total >= old_len || total > out_cap)
		return 0;

	d = (FluxUndoDelta *) out;
	d->old_len = old_len;
	d->prefixlen = (uint16) prefixlen;
	d->suffixlen = (uint16) suffixlen;
	memcpy(out + SizeOfFluxUndoDelta, old_image + prefixlen, middle_len);
	return total;
}

/*
 * FluxUndoDeltaReconstruct
 *		Reconstruct the full old image from a delta record and the current
 *		on-page image (cur, cur_len bytes).  Writes into out (caller-allocated,
 *		at least delta->old_len bytes) and returns true on success.
 *
 * Fails (returns false) if the delta is malformed or the current image is too
 * short to supply the kept prefix/suffix.
 */
static inline bool
FluxUndoDeltaReconstruct(const FluxUndoDelta *delta, Size delta_len,
						 const char *cur, Size cur_len,
						 char *out)
{
	uint32		prefixlen;
	uint32		suffixlen;
	uint32		old_len;
	uint32		middle_len;

	/*
	 * Validate the length before reading any field: prefixlen, suffixlen and
	 * old_len live in the first SizeOfFluxUndoDelta bytes, so a shorter
	 * (corrupt or forged) delta would make the reads below out of bounds.
	 */
	if (delta_len < SizeOfFluxUndoDelta)
		return false;

	prefixlen = delta->prefixlen;
	suffixlen = delta->suffixlen;
	old_len = delta->old_len;

	if ((Size) prefixlen + suffixlen > old_len)
		return false;
	middle_len = old_len - prefixlen - suffixlen;
	if (delta_len < SizeOfFluxUndoDelta + middle_len)
		return false;
	if ((Size) prefixlen + suffixlen > cur_len)
		return false;

	memcpy(out, cur, prefixlen);
	memcpy(out + prefixlen,
		   (const char *) delta + SizeOfFluxUndoDelta, middle_len);
	memcpy(out + prefixlen + middle_len,
		   cur + cur_len - suffixlen, suffixlen);
	return true;
}

extern UndoApplyResult flux_undo_apply_locked(Relation rel, Buffer buffer,
											  uint16 info,
											  TransactionId xid,
											  const char *payload,
											  Size payload_len,
											  UndoRecPtr urec_ptr);
extern bool FluxTupleHasAbortedVersion(FluxTupleHeader *tup);
extern bool FluxRevertAbortedVersions(Relation rel, Buffer buffer,
									  OffsetNumber offnum);

/*
 * Registration entry point, called once at postmaster startup from
 * InitializeUndoSubsystem() alongside HeapUndoRmgrInit and friends.
 */
extern void FluxUndoRmgrInit(void);

#endif							/* FLUX_UNDO_H */
