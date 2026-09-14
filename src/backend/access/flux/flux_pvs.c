/*-------------------------------------------------------------------------
 *
 * flux_pvs.c
 *	  FLUX per-relation versioned storage (PVS) read path
 *
 * In-place UPDATEs on a FLUX tuple stamp an UndoRecPtr (verptr)
 * into the new on-page image.  Each verptr points to the UNDO
 * record that describes the update which produced its host image; reversing
 * the diff (or full-tuple) in that record yields the immediately prior
 * committed image, whose own trailing verptr -- preserved verbatim through
 * the reverse-apply -- continues the chain one further step back.
 *
 * FluxReconstructVisibleVersion walks that chain on behalf of an MVCC
 * reader, stepping back one version at a time until it finds the image
 * created by an xid that is VISIBLE to the reader's snapshot.  The result is
 * the before-image the reader should see in place of the on-page (newer)
 * data.
 *
 * urec_prevundorec is NOT used here: that field threads a per-relation,
 * per-transaction rollback LIFO, not a per-tuple version chain.  The
 * authoritative per-tuple chain is the verptr threaded through reconstructed
 * images.
 *
 * Visibility per step uses the same rule as the on-page check,
 * FluxWriterVisibleToSnapshot(): the writer of the current candidate is
 * visible only if it committed and is not in the snapshot's in-progress set.
 * Visible -> serve the current candidate; otherwise step back.  An aborted
 * writer whose rollback has not been applied yet (deferred to an undo apply
 * worker, or a crash loser) is therefore stepped past, never served.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_pvs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/flux.h"
#include "access/flux_pbu.h"
#include "access/flux_undo.h"
#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/snapmgr.h"

/*
 * A version-chain record that cannot be stepped past is corruption.  Serving
 * the newer image instead would return a row version the snapshot must not
 * see, so raise an ERROR.
 */
pg_noreturn static void
flux_pvs_corrupt(Relation rel, ItemPointer tid, UndoRecPtr urp,
				 const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg_internal("FLUX version chain of (%u,%u) in relation \"%s\" is broken at undo record %llu: %s",
							 ItemPointerGetBlockNumber(tid),
							 ItemPointerGetOffsetNumber(tid),
							 RelationGetRelationName(rel),
							 (unsigned long long) urp, what)));
}

static bool flux_pvs_walk(Relation rel, ItemPointer tid,
						  const char *onpage_image, Size onpage_len,
						  Snapshot snapshot,
						  char **out_data, int *out_len,
						  FluxSkippedUpdates *skipped, bool conflict_out);

/*
 * flux_pvs_prior_image
 *		Reverse-apply the UNDO record at verptr (already fetched: its subtype
 *		and payload) to candidate, the image that record produced, returning
 *		a palloc'd prior image in *out_image / *out_len.  A record
 *		that cannot be stepped past is corruption and raises an ERROR (see
 *		flux_pvs_corrupt): serving a newer image instead would return a
 *		version the snapshot must not see.
 */
static void
flux_pvs_prior_image(Relation rel, ItemPointer tid,
					 UndoRecPtr verptr, uint16 urec_subtype,
					 const char *payload, Size payload_size,
					 const char *candidate, Size candidate_len,
					 char **out_image, Size *out_len)
{
	char	   *next_image = NULL;
	Size		next_len = 0;

	switch (urec_subtype)
	{
		case FLUX_UNDO_UPDATE:
		case FLUX_UNDO_DELETE:

			/*
			 * Both carry the full prior image.  A DELETE record is the chain
			 * head of a deleted row; stepping past an invisible delete yields
			 * the row as it was before it.
			 */
			{
				FluxUndoPayloadHeader phdr;

				if (payload == NULL ||
					payload_size < SizeOfFluxUndoPayloadHeader)
					flux_pvs_corrupt(rel, tid, verptr, "payload too short");
				memcpy(&phdr, payload, SizeOfFluxUndoPayloadHeader);
				if (!(phdr.flags & FLUX_UNDO_FLAG_HAS_TUPLE) ||
					phdr.tuple_len == 0)
					flux_pvs_corrupt(rel, tid, verptr, "no before-image");
				if (payload_size < SizeOfFluxUndoPayloadHeader + phdr.tuple_len)
					flux_pvs_corrupt(rel, tid, verptr, "before-image truncated");

				next_len = phdr.tuple_len;
				next_image = (char *) palloc(next_len);
				memcpy(next_image,
					   payload + SizeOfFluxUndoPayloadHeader,
					   next_len);
				break;
			}

		case FLUX_UNDO_UPDATE_DELTA:
			{
				FluxUndoPayloadHeader phdr;
				const FluxUndoDelta *delta;
				Size		delta_len;

				if (payload == NULL ||
					payload_size < SizeOfFluxUndoPayloadHeader +
					SizeOfFluxUndoDelta)
					flux_pvs_corrupt(rel, tid, verptr, "delta payload too short");
				memcpy(&phdr, payload, SizeOfFluxUndoPayloadHeader);
				if (!(phdr.flags & FLUX_UNDO_FLAG_HAS_TUPLE))
					flux_pvs_corrupt(rel, tid, verptr, "delta without before-image");

				delta = (const FluxUndoDelta *)
					(payload + SizeOfFluxUndoPayloadHeader);

				/*
				 * tuple_len is the delta's length; a reserved index-revive
				 * area (FLUX_UNDO_FLAG_INDEX_LIST) may follow it.
				 */
				delta_len = phdr.tuple_len;
				if (delta_len < SizeOfFluxUndoDelta ||
					delta_len > payload_size - SizeOfFluxUndoPayloadHeader)
					flux_pvs_corrupt(rel, tid, verptr, "delta length out of range");

				/*
				 * Reconstruct the prior image by reverse-applying the delta
				 * onto the CURRENT candidate image (not onto the page). The
				 * delta always covers the t_verptr at offset 8, so the
				 * reconstructed image carries the prior chain link and the
				 * walk continues one step further back.
				 */
				next_len = delta->old_len;
				next_image = (char *) palloc(next_len);
				if (!FluxUndoDeltaReconstruct(delta, delta_len,
											  candidate, candidate_len,
											  next_image))
					flux_pvs_corrupt(rel, tid, verptr, "delta does not fit the newer image");
				break;
			}

		default:
			flux_pvs_corrupt(rel, tid, verptr, "not an UPDATE or DELETE record");
	}


	*out_image = next_image;
	*out_len = next_len;
}

/*
 * FluxReconstructVisibleVersion
 *		Walk the UNDO version chain and reconstruct the tuple version
 *		that satisfies the reader's MVCC snapshot.
 *
 * Inputs:
 *	rel				- target relation (used to read its UNDO log)
 *	tid				- on-page TID of the row (for error reports)
 *	onpage_image	- pointer to the on-page tuple bytes (read-only)
 *	onpage_len		- length of onpage_image (ItemIdGetLength of the slot)
 *	snapshot		- MVCC snapshot of the reader
 *
 * Outputs (only populated when the function returns true):
 *	out_data		- palloc'd buffer holding the reconstructed image
 *	out_len			- length of *out_data
 *
 * Returns true if a different-from-on-page version should be served and
 * *out_data / *out_len have been populated.  Returns false when the on-page
 * value is what the reader should see (caller must NOT free *out_data in
 * that case; it is left untouched).
 *
 * The chain has no length limit: it holds one record per UPDATE committed
 * since the reader's snapshot, however many that is, and the walk follows it
 * to the version the snapshot sees.  A record that has been discarded ends
 * the walk at the current candidate, which is then the one to serve: UNDO is
 * discarded only once its writer is visible to every snapshot.  A malformed
 * record or a chain that loops back on itself is corruption and raises an
 * ERROR; serving the newer image instead would be a wrong answer.
 *
 * This is the walk of a read that serves the version it finds, so under
 * SERIALIZABLE it reports a rw-conflict out to each writer it steps over,
 * as heap does for every version of an update chain it examines
 * (HeapCheckForSerializableConflictOut): the writer of each image the
 * snapshot cannot see, and the inserter of a row the snapshot cannot see.
 * The caller reports the on-page image itself
 * (FluxCheckForSerializableConflictOut).  Callers walking with a private
 * snapshot, or locking rather than reading, use
 * FluxReconstructVisibleVersionEx, which reports nothing.
 */
bool
FluxReconstructVisibleVersion(Relation rel, ItemPointer tid,
							  const char *onpage_image, Size onpage_len,
							  Snapshot snapshot,
							  char **out_data, int *out_len)
{
	return flux_pvs_walk(rel, tid, onpage_image, onpage_len, snapshot,
						 out_data, out_len, NULL,
						 CheckForSerializableConflictOutNeeded(rel, snapshot));
}

/*
 * FluxReconstructVisibleVersionEx
 *		FluxReconstructVisibleVersion, also describing the updates the walk
 *		stepped back over.
 *
 * Each step goes back over one update the snapshot does not see: one made
 * after the snapshot by a committed, in-progress or aborted transaction.  If
 * skipped is not NULL it describes those updates, which is what a row locker
 * needs to answer as heap does (heap_lock_tuple and
 * heap_lock_updated_tuple_rec test every version from the caller's to the
 * latest; see flux_tuple_lock):
 *
 *	committed_xid		- writer of the newest committed one
 *	committed_key_xid	- writer of the newest committed one that changed an
 *						  indexed column
 *	inprogress_key_xid	- writer of the newest in-progress one that changed
 *						  an indexed column
 *	complete			- the walk reached the snapshot's version of the row,
 *						  or the start of the chain; if false the history is
 *						  incomplete (a record was unreadable) and the fields
 *						  above may miss an update
 *
 * Whether an update changed an indexed column is the FLUX_TUPLE_KEYS_UPDATED
 * bit of the image it produced.  The bit describes that one update: an
 * in-place UPDATE forms its image with fresh flags and sets the bit only if
 * it changed an indexed column, and the undo record of the next update keeps
 * that image, flags included, byte for byte.  So a key changed and then
 * changed back is two key-changing updates here, as in heap's update chain.
 *
 * Updates by aborted transactions whose rollback is still pending are stepped
 * over but not reported: heap ignores an aborted updater too
 * (test_lockmode_for_conflict).  Neither are our own transaction's.
 */
bool
FluxReconstructVisibleVersionEx(Relation rel, ItemPointer tid,
								const char *onpage_image, Size onpage_len,
								Snapshot snapshot,
								char **out_data, int *out_len,
								FluxSkippedUpdates *skipped)
{
	return flux_pvs_walk(rel, tid, onpage_image, onpage_len, snapshot,
						 out_data, out_len, skipped, false);
}

/*
 * The walk behind FluxReconstructVisibleVersion and
 * FluxReconstructVisibleVersionEx.  conflict_out: report each writer stepped
 * over to SSI (the caller has checked CheckForSerializableConflictOutNeeded).
 */
static bool
flux_pvs_walk(Relation rel, ItemPointer tid,
			  const char *onpage_image, Size onpage_len,
			  Snapshot snapshot,
			  char **out_data, int *out_len,
			  FluxSkippedUpdates *skipped, bool conflict_out)
{
	const char *candidate = onpage_image;
	Size		candidate_len = onpage_len;
	char	   *allocated = NULL;
	UndoRecPtr	cycle_mark = InvalidUndoRecPtr;
	uint64		cycle_steps = 0;
	uint64		cycle_limit = 1;

	if (skipped != NULL)
		memset(skipped, 0, sizeof(FluxSkippedUpdates));

	if (snapshot == NULL || onpage_image == NULL || onpage_len == 0)
		return false;

	for (;;)
	{
		const FluxTupleHeader *hdr = (const FluxTupleHeader *) candidate;
		UndoRecPtr	verptr;
		TransactionId urec_xid;
		CommandId	urec_cid;
		uint16		urec_subtype;
		char	   *payload = NULL;
		Size		payload_size = 0;
		char	   *next_image = NULL;
		Size		next_len = 0;

		CHECK_FOR_INTERRUPTS();

		/*
		 * A frozen candidate is a terminal, universally visible version.
		 * Never fetch an obsolete undo record (or its writer's CLOG entry)
		 * behind it.  Frozen before-images are detached before logging.
		 */
		if (TransactionIdEquals(FluxTupleGetXmin(hdr), FrozenTransactionId) &&
			!TransactionIdIsValid(FluxTupleGetXmax(hdr)))
		{
			if (skipped != NULL)
				skipped->complete = true;
			break;
		}

		/*
		 * The header identifies the writer without consulting undo: xmin for
		 * an in-place update, xmax for a delete or superseded version. Once
		 * that writer is committed and visible to this snapshot, no earlier
		 * image is needed.  In particular, a new relation synced at
		 * wal_level=minimal can survive a crash without its unwritten undo.
		 * Our own writer still needs the undo command id below.
		 */
		{
			TransactionId xmax = FluxTupleGetXmax(hdr);
			bool		removed = TransactionIdIsValid(xmax) &&
				(hdr->t_flags & (FLUX_TUPLE_DELETED | FLUX_TUPLE_UPDATED));
			TransactionId writer = removed ? xmax : FluxTupleGetXmin(hdr);
			bool		committed = (hdr->t_flags & (removed ?
													 FLUX_TUPLE_XMAX_COMMITTED : FLUX_TUPLE_XMIN_COMMITTED)) != 0;

			if (TransactionIdIsValid(writer) &&
				!TransactionIdIsCurrentTransactionId(writer) &&
				FluxWriterVisibleToSnapshot(writer, snapshot, committed))
			{
				if (skipped != NULL)
					skipped->complete = true;
				if (removed)
				{
					if (allocated != NULL)
						pfree(allocated);
					*out_data = NULL;
					*out_len = 0;
					return false;
				}
				break;
			}
		}

		verptr = FluxTupleGetVersionPtr(hdr);
		if (!UndoRecPtrIsValid(verptr))
		{
			/*
			 * End of the version chain: this candidate is the original
			 * inserted image (an INSERT writes no undo record and leaves an
			 * invalid verptr).  There is no older version to fall back to, so
			 * the row is visible only if the inserter itself is visible to
			 * the reader.  If the inserter is invisible (aborted, crashed, or
			 * in-progress and not our own), the row was never really there
			 * for this snapshot: report no visible version rather than
			 * serving a phantom before-image.
			 */
			TransactionId ins_xid = FluxTupleGetXmin(hdr);
			bool		ins_visible;

			if (!TransactionIdIsValid(ins_xid) ||
				TransactionIdIsCurrentTransactionId(ins_xid))
			{
				/* pre-upgrade tuple (no xmin) or our own insert: visible */
				ins_visible = true;
			}
			else
			{
				/*
				 * Heap-style: the inserted row is visible only if its
				 * inserter committed AND committed before this snapshot (not
				 * in the snapshot's in-progress set).  An aborted or crashed
				 * inserter reports !DidCommit, so its image is never served
				 * -- this is the rollback signal now that INSERT writes no
				 * undo record.
				 */
				ins_visible = FluxWriterVisibleToSnapshot(ins_xid, snapshot,
														  (hdr->t_flags & FLUX_TUPLE_XMIN_COMMITTED) != 0);
			}

			if (skipped != NULL)
				skipped->complete = true;

			if (!ins_visible)
			{
				/* heap's invisible LIVE version: conflict out to xmin */
				if (conflict_out)
					FluxSerializableConflictOutXid(rel, ins_xid, snapshot);
				if (allocated != NULL)
					pfree(allocated);
				*out_data = NULL;
				*out_len = 0;
				return false;
			}
			break;				/* no further history; serve this candidate */
		}

		/*
		 * A chain that returns to a record it has already passed would loop
		 * forever.  Detect that with Brent's algorithm: compare each link
		 * with one remembered link, re-remembered after 1, 2, 4, ... steps.
		 */
		if (verptr == cycle_mark)
			flux_pvs_corrupt(rel, tid, verptr, "the chain loops");
		if (++cycle_steps == cycle_limit)
		{
			cycle_mark = verptr;
			cycle_steps = 0;
			cycle_limit *= 2;
		}

		if (!FluxPbuFetchVersion(verptr, &urec_xid, &urec_cid, &urec_subtype,
								 &payload, &payload_size))
			break;				/* discarded: visible to every snapshot */

		/*
		 * urec_xid/urec_cid identify the writer that produced the CURRENT
		 * candidate image.  Decide whether that writer is visible to the
		 * reader; if it is, the candidate is what we should serve.
		 *
		 * For a writer in the reader's OWN transaction, XidInMVCCSnapshot is
		 * always false (our xid is never in our own in-progress set), so the
		 * command id decides instead: a candidate produced by a command at or
		 * after the reader's command id is not visible to this snapshot -- an
		 * earlier command's cursor must see the version as of its own
		 * command, not the row a later command in the same transaction
		 * produced.  This is heap's HeapTupleHeaderGetCmin(candidate) >=
		 * curcid rule, applied to the in-place AM where the intermediate
		 * version survives only in the undo chain.
		 */
		if (TransactionIdIsCurrentTransactionId(urec_xid))
		{
			if (snapshot->curcid == InvalidCommandId ||
				urec_cid == InvalidCommandId ||
				urec_cid < snapshot->curcid)
			{
				/* our own earlier-or-current command produced it: visible */
				if (payload != NULL)
					pfree(payload);
				if (skipped != NULL)
					skipped->complete = true;
				break;
			}
			/* our own later command: fall through and reverse-apply */
		}
		else if (FluxWriterVisibleToSnapshot(urec_xid, snapshot, false))
		{
			if (payload != NULL)
				pfree(payload);
			if (skipped != NULL)
				skipped->complete = true;
			break;
		}

		/*
		 * We are about to step back over the update that produced this
		 * candidate; report it (see FluxReconstructVisibleVersionEx).  Test
		 * in-progress before committed, as heap does: a committing writer is
		 * in CLOG before it leaves the proc array.
		 */
		if (skipped != NULL && !TransactionIdIsCurrentTransactionId(urec_xid))
		{
			bool		key_changed =
				(hdr->t_flags & FLUX_TUPLE_SUPERSEDED) ?
				!(hdr->t_flags & FLUX_TUPLE_KEY_INTACT) :
				(hdr->t_flags & FLUX_TUPLE_KEYS_UPDATED) != 0;

			if (TransactionIdIsInProgress(urec_xid))
			{
				if (key_changed &&
					!TransactionIdIsValid(skipped->inprogress_key_xid))
					skipped->inprogress_key_xid = urec_xid;
			}
			else if (TransactionIdDidCommit(urec_xid))
			{
				if (!TransactionIdIsValid(skipped->committed_xid))
					skipped->committed_xid = urec_xid;
				if (key_changed &&
					!TransactionIdIsValid(skipped->committed_key_xid))
					skipped->committed_key_xid = urec_xid;
			}
		}

		/*
		 * Heap would have examined the version this writer produced and, not
		 * seeing it, reported a conflict out to its xmin.  Running and
		 * committed-concurrent writers count; aborted ones and our own do not
		 * (FluxSerializableConflictOutXid).
		 */
		if (conflict_out)
			FluxSerializableConflictOutXid(rel, urec_xid, snapshot);

		/*
		 * Updater invisible to reader (in progress, committed after the
		 * snapshot, or aborted with its rollback still pending):
		 * reverse-apply the record to obtain the prior image and continue.
		 * The per-backend UNDO record's uur_payload is
		 * [FluxUndoPayloadHeader][old tuple bytes] (built by
		 * FluxPbuPrepareInsert), so the before-image is the tuple_len bytes
		 * immediately after the fixed header.
		 */
		flux_pvs_prior_image(rel, tid, verptr, urec_subtype,
							 payload, payload_size,
							 candidate, candidate_len,
							 &next_image, &next_len);

		pfree(payload);

		/* Replace the candidate with the reconstructed prior image. */
		if (allocated != NULL)
			pfree(allocated);
		allocated = next_image;
		candidate = allocated;
		candidate_len = next_len;
	}

	if (allocated == NULL)
		return false;			/* no reconstruction; caller serves on-page */

	*out_data = allocated;
	*out_len = (int) candidate_len;
	return true;
}

/*
 * FluxFindVersionSuperseder
 *		Find the version of the row at tid whose header identity is
 *		(xmin, cmin), and the writer that superseded it.
 *
 * A FLUX in-place UPDATE keeps the row at its TID, so a TID alone names only
 * the newest version; a version is identified by the writer stamped in its
 * header, t_xmin with t_cmin (see FluxTupleHeader.t_cmin).  Walks the
 * version chain from the on-page image, the same chain
 * FluxReconstructVisibleVersion walks, until it reaches an image with that
 * identity.  On success returns true and sets *sup_xid / *sup_cid to the
 * writer of the UNDO record whose reverse-apply produced it: the transaction
 * (subtransaction XID, as stamped) and command that replaced the version,
 * heap's xmax of that version.  The caller has already seen that the
 * on-page image is not the version sought.
 *
 * Returns false if the chain ends, or cannot be read, before the version is
 * reached.
 */
bool
FluxFindVersionSuperseder(Relation rel, ItemPointer tid,
						  const char *onpage_image, Size onpage_len,
						  TransactionId xmin, CommandId cmin,
						  TransactionId *sup_xid, CommandId *sup_cid)
{
	const char *candidate = onpage_image;
	Size		candidate_len = onpage_len;
	char	   *allocated = NULL;
	bool		found = false;
	UndoRecPtr	cycle_mark = InvalidUndoRecPtr;
	uint64		cycle_steps = 0;
	uint64		cycle_limit = 1;

	for (;;)
	{
		const FluxTupleHeader *hdr = (const FluxTupleHeader *) candidate;
		UndoRecPtr	verptr = FluxTupleGetVersionPtr(hdr);
		TransactionId urec_xid;
		CommandId	urec_cid;
		uint16		urec_subtype;
		char	   *payload = NULL;
		Size		payload_size = 0;
		char	   *next_image = NULL;
		Size		next_len = 0;

		CHECK_FOR_INTERRUPTS();

		if (!UndoRecPtrIsValid(verptr))
			break;

		/* Same loop guard as FluxReconstructVisibleVersionEx (Brent). */
		if (verptr == cycle_mark)
			flux_pvs_corrupt(rel, tid, verptr, "the chain loops");
		if (++cycle_steps == cycle_limit)
		{
			cycle_mark = verptr;
			cycle_steps = 0;
			cycle_limit *= 2;
		}

		if (!FluxPbuFetchVersion(verptr, &urec_xid, &urec_cid, &urec_subtype,
								 &payload, &payload_size))
			break;				/* discarded: the version is older than any
								 * undo */
		flux_pvs_prior_image(rel, tid, verptr, urec_subtype,
							 payload, payload_size,
							 candidate, candidate_len,
							 &next_image, &next_len);
		if (payload != NULL)
			pfree(payload);

		if (allocated != NULL)
			pfree(allocated);
		allocated = next_image;
		candidate = allocated;
		candidate_len = next_len;
		hdr = (const FluxTupleHeader *) candidate;

		if (TransactionIdEquals(FluxTupleGetXmin(hdr), xmin) &&
			hdr->t_cmin == cmin)
		{
			*sup_xid = urec_xid;
			*sup_cid = urec_cid;
			found = true;
			break;
		}
	}

	if (allocated != NULL)
		pfree(allocated);
	return found;
}

/*
 * Bound a relation's freeze horizon by the history a reader can still expose.
 * New before-images are normalized at creation, but old records and images of
 * our own transaction can contain normal XIDs.  Do not assume record writers
 * are ordered by XID.  Fetching a discarded record is a safe end of history.
 */
TransactionId
FluxVersionChainOldestXid(Relation rel, ItemPointer tid,
						  const char *image, Size len, TransactionId oldest)
{
	const char *candidate = image;
	char	   *allocated = NULL;
	UndoRecPtr	mark = InvalidUndoRecPtr;
	uint64		steps = 0;
	uint64		limit = 1;

	for (;;)
	{
		const FluxTupleHeader *hdr = (const FluxTupleHeader *) candidate;
		TransactionId xids[2] = {FluxTupleGetXmin(hdr), FluxTupleGetXmax(hdr)};
		UndoRecPtr	ptr = FluxTupleGetVersionPtr(hdr);
		TransactionId writer;
		CommandId	cid;
		uint16		subtype;
		char	   *payload = NULL;
		Size		payload_len;
		char	   *prior;
		Size		prior_len;

		CHECK_FOR_INTERRUPTS();
		for (int i = 0; i < 2; i++)
			if (TransactionIdIsNormal(xids[i]) && TransactionIdPrecedes(xids[i], oldest))
				oldest = xids[i];
		if (!UndoRecPtrIsValid(ptr))
			break;
		if (ptr == mark)
			flux_pvs_corrupt(rel, tid, ptr, "the chain loops");
		if (++steps == limit)
		{
			mark = ptr;
			steps = 0;
			limit *= 2;
		}
		if (!FluxPbuFetchVersion(ptr, &writer, &cid, &subtype, &payload, &payload_len))
			break;
		if (TransactionIdIsNormal(writer) && TransactionIdPrecedes(writer, oldest))
			oldest = writer;
		flux_pvs_prior_image(rel, tid, ptr, subtype, payload, payload_len,
							 candidate, len, &prior, &prior_len);
		pfree(payload);
		if (allocated != NULL)
			pfree(allocated);
		allocated = prior;
		candidate = prior;
		len = prior_len;
	}
	if (allocated != NULL)
		pfree(allocated);
	return oldest;
}

/*
 * FluxRewriteChain
 *		The older versions of the row at tid that a CLUSTER / VACUUM FULL
 *		rewrite must copy along with image, its version on the page.
 *
 * A row updated in place keeps its older versions only in UNDO, which a
 * rewrite does not carry over: the new relation's rows have no history.
 * So the rewrite copies each older version some snapshot may still see as a
 * row of its own, deleted by the update that replaced it, as heap's
 * rewriteheap.c copies every RECENTLY_DEAD version of an update chain.
 * Walk back from image while the update that replaced the version reached is
 * not visible to every snapshot: it is ours, or it committed at or after
 * oldest_xmin.  The first update that is visible to everyone, the start of
 * the chain, a discarded record (UNDO is discarded only once its writer is
 * visible to everyone) or a frozen image ends the walk.
 *
 * A DELETE record heads the chain of a deleted row.  Stepping past it gives
 * the same version undeleted; the page version already carries the delete
 * (its xmax), so that version is not returned again.
 *
 * The caller has reverted every aborted change (flux_revert_pending), and
 * holds AccessExclusiveLock, so every other writer in the chain committed.
 *
 * Returns a List of FluxChainVersion, newest first, allocated in the
 * current memory context.
 */
List *
FluxRewriteChain(Relation rel, ItemPointer tid, const char *image, Size len,
				 TransactionId oldest_xmin)
{
	List	   *versions = NIL;
	const char *candidate = image;
	char	   *undeleted = NULL;
	UndoRecPtr	mark = InvalidUndoRecPtr;
	uint64		steps = 0;
	uint64		limit = 1;

	for (;;)
	{
		UndoRecPtr	ptr = FluxTupleGetVersionPtr((const FluxTupleHeader *) candidate);
		TransactionId writer;
		CommandId	cid;
		uint16		subtype;
		char	   *payload = NULL;
		Size		payload_len;
		FluxChainVersion *v;

		CHECK_FOR_INTERRUPTS();
		if (!UndoRecPtrIsValid(ptr))
			break;
		if (ptr == mark)
			flux_pvs_corrupt(rel, tid, ptr, "the chain loops");
		if (++steps == limit)
		{
			mark = ptr;
			steps = 0;
			limit *= 2;
		}
		if (!FluxPbuFetchVersion(ptr, &writer, &cid, &subtype, &payload, &payload_len))
			break;
		if (subtype != FLUX_UNDO_DELETE &&
			!TransactionIdIsCurrentTransactionId(writer))
		{
			if (!TransactionIdDidCommit(writer))
				elog(ERROR, "FLUX row (%u,%u) of \"%s\" has a change by transaction %u, which did not commit",
					 ItemPointerGetBlockNumber(tid),
					 ItemPointerGetOffsetNumber(tid),
					 RelationGetRelationName(rel), writer);
			if (TransactionIdPrecedes(writer, oldest_xmin))
			{
				pfree(payload);
				break;
			}
		}

		v = palloc_object(FluxChainVersion);
		flux_pvs_prior_image(rel, tid, ptr, subtype, payload, payload_len,
							 candidate, len, &v->image, &v->len);
		pfree(payload);
		if (undeleted != NULL)
			pfree(undeleted);
		undeleted = NULL;
		candidate = v->image;
		len = v->len;
		if (subtype == FLUX_UNDO_DELETE)
		{
			undeleted = v->image;
			pfree(v);
			continue;
		}
		v->xmax = writer;
		versions = lappend(versions, v);
	}
	if (undeleted != NULL)
		pfree(undeleted);
	return versions;
}
