/*-------------------------------------------------------------------------
 *
 * flux_pbu.c
 *	  FLUX per-backend UNDO shim (insert / fetch / redo side).
 *
 * FLUX writes its DML before-images to the per-backend UNDO engine
 * (access/undo/perbackend, PrepareUndoInsert/UndoFetchRecord).  This
 * translation unit is the small "pbu shim" (mirroring fileops_pbu.c): it is
 * the ONLY FLUX file that CAN include the per-backend engine's record/insert
 * headers, because those headers redefine UndoRecordHeader / UndoLogControl
 * and still use the pre-RelFileLocator RelFileNode type, and so conflict with
 * the other UNDO headers that the rest of the FLUX access method pulls in.
 * The rest of FLUX reaches this engine only through the by-prototype entry
 * points declared in access/flux.h.
 *
 * WRITE PROTOCOL (one FLUX UNDO record per INSERT/UPDATE/DELETE before-image,
 * mirroring the FILEOPS crash-safe choreography):
 *   1. FluxPbuPrepareInsert()      -- BEFORE the critical section.  Builds the
 *      UnpackedUndoRecord and calls PrepareUndoInsert(), reserving+pinning the
 *      undo buffers; may palloc.  Returns the UndoRecPtr that becomes the
 *      tuple's version-chain head (t_verptr).
 *   2. FluxPbuInsertUndo()         -- INSIDE the crit section, before the FLUX
 *      forward-op WAL record: WAL-logs undo-log meta then writes the record
 *      into the pinned buffers (InsertPreparedUndo also chains the xact via
 *      SetCurrentUndoLocation).
 *   3. FluxPbuRegisterUndoBuffers() -- INSIDE the crit section, after the FLUX
 *      forward-op XLogRegisterBuffer()/XLogRegisterData() and before its
 *      XLogInsert(): attaches each pinned undo page to the FLUX forward WAL
 *      record.  No full-page image is forced: the forward record also carries
 *      the undo record's own bytes (xl_flux_undo, attached as block data on the
 *      first undo block), and flux_redo() RE-RUNS the undo insert (FluxPbuRedoUndo) to rebuild the
 *      undo page from those bytes.  Registering the buffers still matters --
 *      it makes the undo page a block reference of this record, so recovery
 *      restores it from an ordinary FPI when one was taken for other reasons
 *      (a page's first touch after a checkpoint, or wal_consistency_checking).
 *      THIS is what makes per-backend FLUX undo crash-safe: the undo bytes ride
 *      to disk on the same WAL record as the forward FLUX page mutation.
 *   4. FluxPbuFinishUndo()          -- after XLogInsert(): stamps the record
 *      LSN onto the undo pages, unlocks/releases them, resets state.
 *   FluxPbuCancelUndo()             -- release the pinned undo buffers if the
 *      DML aborts before it commits the record (e.g. a no-op CAS).
 *
 * The record is tagged uur_rmid = UNDO_RMID_FLUX and uur_type = the FLUX undo
 * subtype (FLUX_UNDO_UPDATE/DELETE/UPDATE_DELTA), so on abort execute_undo_actions()
 * -> GetUndoRmgr(UNDO_RMID_FLUX) -> rm_undo reaches flux_undo_apply()
 * (flux_undo.c) with the subtype in its "info" argument.  The whole
 * before-image payload -- FluxUndoPayloadHeader followed by the before-image
 * tuple bytes -- is carried in uur_payload so that (a) flux_undo_apply reads
 * it unchanged (execute_undo_actions_page passes rec->uur_payload to rm_undo),
 * and (b) FluxReconstructVisibleVersion reads the trailing tuple bytes back
 * exactly as before.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/flux/flux_pbu.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "storage/standby.h"

#include "access/perbackend/pbu_compat.h"
#include "access/perbackend/pbu_undoinsert.h"
#include "access/perbackend/pbu_undolog.h"
#include "access/perbackend/pbu_undorecord.h"
#include "access/flux_undo.h"
#include "access/genam.h"
#include "access/amapi.h"
#include "utils/rel.h"
#include "access/flux_pbu.h"
#include "access/flux_xlog.h"
#include "access/undormgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"
#include "utils/injection_point.h"

/*
 * One FLUX UNDO record is prepared/inserted at a time: each DML op emits
 * exactly one, entirely within its own critical section, before returning
 * (prepare -> insert -> register -> set-lsn -> release runs to completion with
 * no nesting), so a single static suffices and there is no re-entrancy.
 */
static UnpackedUndoRecord flux_pbu_uur;
static xl_undolog_meta flux_pbu_undometa;
static UndoRecPtr flux_pbu_urp = InvalidUndoRecPtr;
static xl_flux_undo flux_pbu_walhdr;
static uint64 flux_pbu_last_xact_start;
static bool flux_pbu_prepared = false;

/*
 * FluxPbuPrepareInsert - reserve undo space for one FLUX UNDO record and
 * return the UndoRecPtr that will be stamped into the tuple's t_verptr.
 *
 * Must be called BEFORE the caller's critical section (PrepareUndoInsert may
 * palloc / extend the undo log / ereport).  subtype is a FLUX_UNDO_* code;
 * tid is the target tuple's TID; tuple_bytes/tuple_len is the before-image (or
 * NULL/0 for INSERT records, which carry no image).  relpersistence is the
 * relation's relpersistence ('p'/'u'/'t'), mapped to the undo persistence;
 * see flux_undo_persistence() for a permanent relation that skips WAL.
 *
 * The uur_payload holds a FluxUndoPayloadHeader immediately followed by the
 * before-image tuple bytes -- the exact layout flux_undo_apply() and
 * FluxReconstructVisibleVersion() expect.
 */
/*
 * The undo log a change to rel writes its undo to.
 *
 * Undo goes to the log of the relation's persistence, except for a permanent
 * relation that skips WAL: one created or rewritten in the current
 * transaction at wal_level = minimal, which is synchronized at commit
 * instead.  Its changes write no WAL, so neither may its undo, and undo
 * written to a permanent undo log without WAL would leave that log in a
 * state crash recovery cannot rebuild: the next WAL-logged undo in the log
 * would land at an offset replay never reached, and a WAL-logged discard of
 * it would move the replayed discard pointer past the replayed insert
 * pointer.  Use the unlogged undo log, which is never replayed and is
 * emptied after a crash.  Nothing needs that undo after a crash either: if
 * the transaction committed, its relation was synchronized and no snapshot
 * from before the crash survives to read an older version; if it did not,
 * the relation is dropped.  Rollback and the version-chain readers find the
 * records through the undo record pointer, whichever log it names.
 */
static UndoPersistence
flux_undo_persistence(Relation rel, char relpersistence)
{
	if (relpersistence == RELPERSISTENCE_PERMANENT && !RelationNeedsWAL(rel))
		return UNDO_UNLOGGED;
	return UndoPersistenceForRelPersistence(relpersistence);
}

UndoRecPtr
FluxPbuPrepareInsert(uint16 subtype, Relation rel, ItemPointer tid,
					 const char *tuple_bytes, uint32 tuple_len,
					 char relpersistence, bool reserve_indexes,
					 TransactionId freeze_conflict, CommandId cid)
{
	FullTransactionId fxid = GetTopFullTransactionId();
	Oid			reloid = RelationGetRelid(rel);
	FluxUndoPayloadHeader hdr;
	UndoRecPtr	urp;
	Size		index_area = 0;
	FluxUndoIndex *index_files = NULL;
	uint16		nindexes = 0;
	Size		row_limit;

	Assert(!flux_pbu_prepared);
	flux_pbu_walhdr.freeze_conflict = freeze_conflict;

	/*
	 * Always false for a FLUX relation: FLUX refuses user_catalog_table and
	 * is never a system catalog, so RelationIsAccessibleInLogicalDecoding()
	 * cannot be true here.  The field and the catalog branch it drives in
	 * ResolveRecoveryConflictWithSnapshot() at redo are therefore unreachable
	 * for FLUX records; they are kept so the WAL format and the conflict
	 * resolution stay correct for a genuine catalog relation and so the test
	 * tracks the one place the rule could change.
	 */
	flux_pbu_walhdr.freeze_catalog_rel =
		RelationIsAccessibleInLogicalDecoding(rel);

	MemSet(&flux_pbu_uur, 0, sizeof(flux_pbu_uur));
	flux_pbu_uur.uur_rmid = UNDO_RMID_FLUX;
	flux_pbu_uur.uur_type = (uint8) subtype;
	flux_pbu_uur.uur_info = 0;
	flux_pbu_uur.uur_reloid = reloid;
	flux_pbu_uur.uur_fork = MAIN_FORKNUM;
	flux_pbu_uur.uur_block = ItemPointerGetBlockNumber(tid);
	flux_pbu_uur.uur_offset = ItemPointerGetOffsetNumber(tid);

	/*
	 * The record's xid/epoch identify the owning transaction; the abort path
	 * asserts the fetched record's (uur_xidepoch,uur_xid) equals the aborting
	 * full xid.  UndoRecordAllocate fills uur_xidepoch for the
	 * transaction-header record, but the caller must supply uur_xid.  This is
	 * also the xid FluxReconstructVisibleVersion reads back (the producer of
	 * the CURRENT candidate image) for its snapshot visibility test.
	 */
	flux_pbu_uur.uur_xid = XidFromFullTransactionId(fxid);
	flux_pbu_uur.uur_xidepoch = EpochFromFullTransactionId(fxid);

	/*
	 * Record the command id of the writer that produced the CURRENT on-page
	 * image (the command performing this UPDATE/DELETE).  The version-chain
	 * walker uses it, together with uur_xid, so that a reader in the same
	 * transaction whose snapshot predates this command reconstructs the prior
	 * image instead of seeing the row this command produced -- an in-place
	 * UPDATE overwrites the intermediate version on the page, so it is only
	 * recoverable from here.
	 *
	 * This is the operation's own command id (the executor's es_output_cid,
	 * which heap stores as cmin/cmax), not GetCurrentCommandId(): a BEFORE
	 * STATEMENT trigger running SQL advances the command counter before the
	 * row is modified, and with the current command id the next command of
	 * the transaction took the change for a later command's, stepped back
	 * over it, and served the deleted or overwritten row again.
	 */
	flux_pbu_uur.uur_cid = cid;

	/* Build the FLUX before-image payload: fixed header + tuple bytes. */
	MemSet(&hdr, 0, sizeof(hdr));
	ItemPointerCopy(tid, &hdr.tid);
	hdr.tuple_len = tuple_len;
	hdr.relnumber = rel->rd_locator.relNumber;
	hdr.flags = (tuple_bytes != NULL && tuple_len > 0) ?
		FLUX_UNDO_FLAG_HAS_TUPLE : 0;

	/*
	 * writer_xid is the xid that produced this record's host image -- the
	 * CURRENT (possibly SUBXACT) xid, NOT the top xid.  The version-chain
	 * reader and the lost-update probe test THIS against the snapshot so that
	 * a SAVEPOINT rollback (which aborts a subxact while the top xact keeps
	 * running) resolves visibility correctly.  uur_xid below stays the owning
	 * TOP xid because the per-backend engine's abort path asserts the fetched
	 * record's uur_xid equals the aborting full xid.
	 */
	hdr.writer_xid = GetCurrentTransactionId();
	if (reserve_indexes)
	{
		List	   *indexes = RelationGetIndexList(rel);
		ListCell   *lc;

		index_files = palloc0(list_length(indexes) * sizeof(FluxUndoIndex));

		/*
		 * Only delete-marking indexes can revive an entry.  List them all, an
		 * upper bound on maintenance's changed set: the old key may change
		 * while an UPDATE waits, and predicates and expressions must not be
		 * evaluated while holding a data buffer content lock.  The relation
		 * lock keeps this index list stable through maintenance.  Each entry
		 * names the index file maintenance changes; see FluxUndoIndex.
		 */
		foreach(lc, indexes)
		{
			Relation	index = index_open(lfirst_oid(lc), AccessShareLock);

			if (index->rd_indam->amdeletemark != NULL)
			{
				index_files[nindexes].indexoid = RelationGetRelid(index);
				index_files[nindexes].spcoid = index->rd_locator.spcOid;
				index_files[nindexes].relnumber = index->rd_locator.relNumber;
				nindexes++;
			}
			index_close(index, AccessShareLock);
		}
		list_free(indexes);
		if (nindexes > 0)
			index_area = sizeof(uint16) + (Size) nindexes * sizeof(FluxUndoIndex);
	}

	/* Include the transaction header even if this record will not need it. */
	row_limit = Min(UndoRecordMaxSize, UINT16_MAX) -
		SizeOfUndoRecordHeader - SizeOfUndoRecordBlock -
		SizeOfUndoRecordTransaction - SizeOfUndoRecordPayload -
		sizeof(uint16) - SizeOfFluxUndoPayloadHeader;
	if (index_area > row_limit || tuple_len > row_limit - index_area)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("FLUX undo row size %u exceeds maximum %zu",
						tuple_len, index_area > row_limit ? (Size) 0 :
						row_limit - index_area),
				 errdetail("Index reservations require %zu bytes.", index_area)));
	if (index_area > 0)
	{
		hdr.pad = index_area;
		hdr.flags |= FLUX_UNDO_FLAG_INDEX_LIST;
	}

	initStringInfo(&flux_pbu_uur.uur_payload);
	appendBinaryStringInfo(&flux_pbu_uur.uur_payload,
						   (char *) &hdr, SizeOfFluxUndoPayloadHeader);
	if (tuple_bytes != NULL && tuple_len > 0)
		appendBinaryStringInfo(&flux_pbu_uur.uur_payload,
							   tuple_bytes, tuple_len);

	if (hdr.pad > 0)
	{
		char	   *area = palloc0(hdr.pad);

		memcpy(area, &nindexes, sizeof(nindexes));
		memcpy(area + sizeof(nindexes), index_files,
			   nindexes * sizeof(FluxUndoIndex));
		appendBinaryStringInfo(&flux_pbu_uur.uur_payload, area, hdr.pad);
		pfree(area);
	}

	if (index_files != NULL)
		pfree(index_files);

	MemSet(&flux_pbu_undometa, 0, sizeof(flux_pbu_undometa));

	/* FluxPbuInsertUndo() runs it in a critical section: load it here. */
	INJECTION_POINT_LOAD("flux-undo-before-forward-wal");

	urp = PrepareUndoInsert(&flux_pbu_uur, fxid,
							flux_undo_persistence(rel, relpersistence),
							NULL, &flux_pbu_undometa);

	flux_pbu_prepared = true;
	flux_pbu_urp = urp;
	return urp;
}

/* Persist the complete revive list before any newly listed entry is revived. */
void
FluxPbuAmendRevivedList(UndoRecPtr urp, const Oid *oids, uint16 n)
{
	UnpackedUndoRecord *urec;
	FluxUndoPayloadHeader hdr;
	uint16		nindexes;
	char	   *area;

	urec = UndoFetchRecord(urp, InvalidBlockNumber, InvalidOffsetNumber,
						   InvalidTransactionId, NULL, NULL);
	if (urec == NULL || urec->uur_payload.len < SizeOfFluxUndoPayloadHeader)
		elog(ERROR, "FLUX undo record missing during amendment");
	memcpy(&hdr, urec->uur_payload.data, sizeof(hdr));
	if (!(hdr.flags & FLUX_UNDO_FLAG_INDEX_LIST) || hdr.pad < sizeof(uint16) ||
		SizeOfFluxUndoPayloadHeader + hdr.tuple_len + hdr.pad != urec->uur_payload.len)
		elog(ERROR, "invalid FLUX undo index list reservation");
	area = palloc(hdr.pad);
	memcpy(area, urec->uur_payload.data + SizeOfFluxUndoPayloadHeader +
		   hdr.tuple_len, hdr.pad);
	UndoRecordRelease(urec);
	memcpy(&nindexes, area, sizeof(nindexes));
	if (sizeof(uint16) + nindexes * sizeof(FluxUndoIndex) > hdr.pad)
		elog(ERROR, "invalid FLUX undo index list");
	for (int i = 0; i < nindexes; i++)
	{
		FluxUndoIndex entry;
		char	   *ptr = area + sizeof(uint16) + i * sizeof(entry);

		memcpy(&entry, ptr, sizeof(entry));
		for (int j = 0; j < n; j++)
			if (entry.indexoid == oids[j])
				entry.flags |= FLUX_UNDO_INDEX_REVIVED;
		memcpy(ptr, &entry, sizeof(entry));
	}
	PbuAmendPayload(urp, SizeOfFluxUndoPayloadHeader + hdr.tuple_len,
					area, hdr.pad);
	pfree(area);
}

/*
 * FluxPbuInsertUndo - WAL-log the undo-log meta and write the prepared record
 * into the undo buffers.  Call INSIDE the crit section, before the FLUX
 * forward-op WAL record.
 *
 * LogUndoMetaData() emits the xid->logno attach/meta record when this is the
 * first undo insert after a checkpoint, which is what lets
 * UndoLogAllocateInRecovery() resolve this xid's log during redo.  Because
 * flux_redo() re-runs this same insert (FluxPbuRedoUndo), replaying the record
 * re-derives the advanced insert pointer itself -- no post-insert meta record
 * is needed, unlike an FPI-based producer such as fileops_pbu.c.
 *
 * The undo of a temporary, unlogged or WAL-skipping relation goes to a log
 * that is never replayed (flux_undo_persistence), so its meta is not logged.
 */
void
FluxPbuInsertUndo(void)
{
	Assert(flux_pbu_prepared);

	if (flux_pbu_undometa.meta.persistence == UNDO_PERMANENT)
		LogUndoMetaData(&flux_pbu_undometa);

	InsertPreparedUndo();

	/*
	 * The undo is in its buffers and the forward-op WAL record that carries
	 * it is next.  A checkpoint that starts here puts its redo point between
	 * the two; FluxPbuRedoUndo() copes (see recovery/t/154).
	 */
	INJECTION_POINT_CACHED("flux-undo-before-forward-wal", NULL);
}

/*
 * FluxPbuRegisterUndoBuffers - attach the pinned undo buffers, and the undo
 * record's own description, to the caller's forward-op WAL record.
 *
 * Call INSIDE the crit section, after the caller's XLogRegisterBuffer()/
 * XLogRegisterData() and before its XLogInsert().  first_block_id is the WAL
 * block id at which to start registering the undo buffers -- it must be past
 * any block id the FLUX forward-op record already uses (block 0 = data page,
 * plus any overflow / cross-page destination blocks).  Returns the number of
 * undo blocks registered.
 *
 * No full-page image is forced.  Instead an xl_flux_undo header plus the undo
 * record's payload is attached as block data on the first undo block, and
 * flux_redo() re-runs the undo insert from it (FluxPbuRedoUndo).  That removes
 * the 8 KB undo-page image per undo record which dominated FLUX's UPDATE WAL
 * volume.  Registering the buffers is still required: it makes each undo page a
 * block reference of this record, so the page gets an ordinary full-page image
 * when it needs one (first touch after a checkpoint, i.e. torn-page protection)
 * and the replay is verified under wal_consistency_checking.
 */
int
FluxPbuRegisterUndoBuffers(uint8 first_block_id)
{
	int			nblocks;

	Assert(flux_pbu_prepared);

	/*
	 * The header must outlive this call: XLogRegisterBufData only records a
	 * pointer and the bytes are read at XLogInsert(), so it lives in a static
	 * that stays valid until FluxPbuFinishUndo(), not on the stack.
	 */
	flux_pbu_walhdr.urec_ptr = (uint64) flux_pbu_urp;
	flux_pbu_walhdr.reloid = flux_pbu_uur.uur_reloid;
	flux_pbu_walhdr.subtype = flux_pbu_uur.uur_type;
	flux_pbu_walhdr.payload_len = (uint16) flux_pbu_uur.uur_payload.len;
	flux_pbu_walhdr.top_xid = flux_pbu_uur.uur_xid;
	flux_pbu_walhdr.top_xid_epoch = flux_pbu_uur.uur_xidepoch;
	flux_pbu_walhdr.cid = flux_pbu_uur.uur_cid;

	/*
	 * Where PrepareUndoInsert() placed the record: the undo log state just
	 * before it, for UndoLogRecoverySetPlacement() at redo.
	 */
	flux_pbu_walhdr.flags =
		(flux_pbu_uur.uur_info & UREC_INFO_TRANSACTION) ?
		FLUX_UNDO_WAL_FIRST_REC : 0;
	flux_pbu_walhdr.dbid = MyDatabaseId;
	flux_pbu_last_xact_start = (uint64) flux_pbu_undometa.meta.last_xact_start;

	nblocks = RegisterUndoLogBuffersWithData(first_block_id,
											 (const char *) &flux_pbu_walhdr,
											 SizeOfFluxUndo);

	/*
	 * Append the payload to the same block's data.  The prepared record's
	 * payload buffer stays alive until FluxPbuFinishUndo(), i.e. past
	 * XLogInsert().
	 */
	if (nblocks > 0 && flux_pbu_uur.uur_payload.len > 0)
		XLogRegisterBufData(first_block_id, flux_pbu_uur.uur_payload.data,
							flux_pbu_uur.uur_payload.len);
	if (nblocks > 0 && (flux_pbu_walhdr.flags & FLUX_UNDO_WAL_FIRST_REC))
		XLogRegisterBufData(first_block_id, &flux_pbu_last_xact_start,
							sizeof(flux_pbu_last_xact_start));

	return nblocks;
}

/*
 * FluxPbuUndoPending - true iff a FLUX undo record has been prepared+inserted
 * for the current op and is awaiting its WAL registration + LSN stamp.  The
 * FLUX forward-op WAL functions (flux_xlog.c) query this to decide whether to
 * fold the undo pages onto their record.
 */
bool
FluxPbuUndoPending(void)
{
	return flux_pbu_prepared;
}

/*
 * FluxPbuFinishUndo - stamp the WAL LSN onto the undo pages, then
 * unlock/release the undo buffers and reset state.  Call after the caller's
 * XLogInsert().  lsn is the LSN of the FLUX forward-op WAL record that carried
 * the undo buffers.
 */
void
FluxPbuFinishUndo(XLogRecPtr lsn)
{
	Assert(flux_pbu_prepared);

	UndoLogBuffersSetLSN(lsn);
	UnlockReleaseUndoBuffers();

	if (flux_pbu_uur.uur_payload.data != NULL)
	{
		pfree(flux_pbu_uur.uur_payload.data);
		flux_pbu_uur.uur_payload.data = NULL;
	}
	flux_pbu_prepared = false;
	flux_pbu_urp = InvalidUndoRecPtr;
}

/*
 * FluxPbuCancelUndo - release the pinned undo buffers without writing the
 * record.  Used when the DML op decides not to commit the reserved record
 * (e.g. a same-size CAS update that finds no byte actually changed).  Because
 * InsertPreparedUndo() has NOT been called, no undo bytes were written and the
 * log's insert pointer was not advanced; releasing the buffers leaves the
 * reserved space unused (identical to the FILEOPS no-op path).
 */
void
FluxPbuCancelUndo(void)
{
	if (!flux_pbu_prepared)
		return;

	UnlockReleaseUndoBuffers();

	if (flux_pbu_uur.uur_payload.data != NULL)
	{
		pfree(flux_pbu_uur.uur_payload.data);
		flux_pbu_uur.uur_payload.data = NULL;
	}
	flux_pbu_prepared = false;
	flux_pbu_urp = InvalidUndoRecPtr;
}

/*
 * FluxPbuBlockIsUndo - true iff the given block reference of a WAL record is a
 * per-backend UNDO page rather than a FLUX relation page.
 *
 * Undo pages live in the reserved undo database OID, so they are
 * distinguishable from any relation block regardless of which block id they
 * landed at.  The FLUX redo handlers' overflow-replay loops iterate block ids
 * 1..N looking for FLUX overflow pages, and the undo pages folded onto the same
 * record sit in that range; without this test such a loop would treat an undo
 * page as an overflow page -- FluxInitPage over it, parse undo bytes as overflow
 * records -- destroying the before-image the rollback needs.  Exposed here
 * because UndoLogDatabaseOid lives in the per-backend engine headers, which the
 * rest of FLUX cannot include.
 */
bool
FluxPbuBlockIsUndo(void *record, uint8 block_id)
{
	XLogReaderState *xlogrec = (XLogReaderState *) record;
	RelFileLocator rlocator;

	if (!XLogRecGetBlockTagExtended(xlogrec, block_id, &rlocator, NULL, NULL,
									NULL))
		return false;

	return rlocator.dbOid == UndoLogDatabaseOid;
}

/*
 * FluxPbuRedoUndo - re-run the undo insert carried on a FLUX forward-op WAL
 * record.
 *
 * Called from flux_redo() for every FLUX opcode that can emit undo, mirroring
 * zheap's redo routines (zheapamxlog.c): find the undo region, rebuild the
 * UnpackedUndoRecord from it, PrepareUndoInsert() (which in recovery resolves
 * the log from the xid->logno map that XLOG_UNDOLOG_ATTACH/META replay
 * maintains, and pins/locks the undo buffers), then InsertPreparedUndo() to
 * write the bytes.  The undo page content is thus reconstructed, not copied from
 * an 8 KB image.
 *
 * first_block_id is where the forward-op emitter started registering undo
 * buffers.  Undo blocks are always the LAST blocks of a FLUX record, and they
 * are the only ones in the reserved undo database (UndoLogDatabaseOid), so the
 * search is unambiguous even for opcodes whose forward block count varies with
 * overflow / cross-page destinations.  A record that carries no undo simply has
 * no such block and this returns InvalidUndoRecPtr without doing anything;
 * otherwise it returns the replayed record's UndoRecPtr (the one DO stamped
 * into the tuple's t_verptr).
 *
 * IDEMPOTENCE: the sequence of undo allocations during redo mirrors the sequence
 * during DO for a given xid, so on replay the record lands at the same offset in
 * the same log and the insert rewrites byte-identical content.  Replaying a
 * record whose undo was already flushed therefore rewrites the same bytes rather
 * than double-applying, and a page that has since been discarded yields an
 * invalid buffer that InsertPreparedUndo() skips.
 */
UndoRecPtr
FluxPbuRedoUndo(void *record, uint8 first_block_id)
{
	XLogReaderState *xlogrec = (XLogReaderState *) record;
	FullTransactionId fxid;
	UnpackedUndoRecord uur;
	xl_flux_undo undohdr;
	FluxUndoPayloadHeader phdr;
	UndoRecPtr	urp;
	uint8		block_id;
	char	   *data = NULL;
	Size		datalen = 0;
	const char *payload;
	Size		trailer;
	uint64		last_xact_start = 0;

	/*
	 * Locate the undo region.  Probe forward from first_block_id for a block
	 * that both lives in the undo database and carries block data (the region
	 * is attached to the FIRST undo block only).
	 */
	for (block_id = first_block_id;
		 block_id < first_block_id + FLUX_PBU_MAX_UNDO_BLOCKS;
		 block_id++)
	{
		if (!XLogRecHasBlockRef(xlogrec, block_id))
			break;				/* no more blocks at all */

		if (!FluxPbuBlockIsUndo(xlogrec, block_id))
			continue;			/* a forward-op block, keep looking */

		data = XLogRecGetBlockData(xlogrec, block_id, &datalen);
		break;
	}

	/* No undo on this record. */
	if (data == NULL || datalen == 0)
		return InvalidUndoRecPtr;

	if (datalen < SizeOfFluxUndo)
		elog(PANIC, "FLUX undo redo: undo region too short (%zu bytes, need %zu)",
			 datalen, SizeOfFluxUndo);

	memcpy(&undohdr, data, SizeOfFluxUndo);
	payload = data + SizeOfFluxUndo;

	/*
	 * Resolve the undo log by the top-level xid that DO recorded, not by this
	 * WAL record's own xid.  A write performed inside a subtransaction tags
	 * its record with the subxact xid, but XLOG_UNDOLOG_ATTACH mapped the log
	 * under the top-level xid.  Looking the log up by the subxact xid reads
	 * an unpopulated slot of the xid->logno map, which is indistinguishable
	 * from log number 0 and ends recovery with "cannot find undo log number
	 * 0". The same xid is written into the rebuilt record below, so that it
	 * is byte-identical to the one DO wrote.
	 */
	fxid = FullTransactionIdFromEpochAndXid(undohdr.top_xid_epoch,
											undohdr.top_xid);

	/*
	 * The before-image is the only copy of the pre-update bytes, so refuse to
	 * read past the WAL data rather than fabricate a record.
	 */
	trailer = (undohdr.flags & FLUX_UNDO_WAL_FIRST_REC) ? sizeof(uint64) : 0;
	if (datalen != SizeOfFluxUndo + (Size) undohdr.payload_len + trailer ||
		undohdr.payload_len < SizeOfFluxUndoPayloadHeader)
		elog(PANIC, "FLUX undo redo: inconsistent undo region "
			 "(region %zu bytes, payload_len %u)",
			 datalen, undohdr.payload_len);

	/*
	 * Rebuild the record EXACTLY as FluxPbuPrepareInsert() built it.  Any
	 * difference in the fields UndoRecordSetInfo() keys on (fork, block,
	 * payload length) would change UndoRecordExpectedSize(), so this record
	 * -- and every record after it in this log -- would land at a different
	 * offset than the original, and the version pointers already stamped into
	 * replayed tuples would dangle.
	 */
	MemSet(&uur, 0, sizeof(uur));
	uur.uur_rmid = UNDO_RMID_FLUX;
	uur.uur_type = (uint8) undohdr.subtype;
	uur.uur_info = 0;
	uur.uur_reloid = undohdr.reloid;
	uur.uur_fork = MAIN_FORKNUM;
	uur.uur_xid = XidFromFullTransactionId(fxid);
	uur.uur_xidepoch = EpochFromFullTransactionId(fxid);
	uur.uur_cid = undohdr.cid;

	memcpy(&phdr, payload, SizeOfFluxUndoPayloadHeader);
	uur.uur_block = ItemPointerGetBlockNumber(&phdr.tid);
	uur.uur_offset = ItemPointerGetOffsetNumber(&phdr.tid);

	/*
	 * The payload is carried verbatim (FluxUndoPayloadHeader + before-image
	 * bytes), so the replayed record is byte-identical to the original --
	 * including writer_xid, which the version-chain reader
	 * (FluxReconstructVisibleVersion) and the lost-update probe read back.
	 */
	initStringInfo(&uur.uur_payload);
	appendBinaryStringInfo(&uur.uur_payload, payload, undohdr.payload_len);

	/*
	 * Place the record from this WAL record, not from the undo log state
	 * recovery started with: a checkpoint that began after DO reserved the
	 * record's undo space, and before this record, left a snapshot already
	 * past it, and the XLOG_UNDOLOG_META that would correct it may not have
	 * been emitted (see UndoLogRecoverySetPlacement()).  This also maps the
	 * xid to its log, which an ATTACH/META replayed before this record would
	 * otherwise have to.
	 */
	if (trailer > 0)
		memcpy(&last_xact_start, payload + undohdr.payload_len, trailer);
	UndoLogRecoverySetPlacement(XidFromFullTransactionId(fxid), undohdr.dbid,
								(UndoRecPtr) undohdr.urec_ptr,
								(undohdr.flags & FLUX_UNDO_WAL_FIRST_REC) != 0,
								(UndoLogOffset) last_xact_start);

	urp = PrepareUndoInsert(&uur, fxid, UNDO_PERMANENT, xlogrec, NULL);

	/*
	 * The replayed insert must land where the DO-time insert did.  A mismatch
	 * means the recovered undo-log metadata disagrees with the WAL, and
	 * continuing would write this transaction's before-image at an offset the
	 * rollback walk will not find -- silently losing the ability to undo it.
	 */
	if (urp != (UndoRecPtr) undohdr.urec_ptr)
		elog(PANIC, "FLUX undo redo: undo record pointer mismatch "
			 "(replay " UINT64_FORMAT ", original " UINT64_FORMAT ")",
			 (uint64) urp, undohdr.urec_ptr);

	InsertPreparedUndo();

	/*
	 * Release the undo buffers.  The page LSNs are deliberately NOT stamped,
	 * matching zheap's redo routines: InsertPreparedUndo() has dirtied the
	 * buffers, so the content reaches disk via the end-of-recovery checkpoint
	 * (or an intervening restartpoint), and WAL replayed a second time
	 * re-inserts the identical bytes.  It skips a page whose LSN is at or
	 * past this record's: that page was written out after a later LSN-gated
	 * change to these bytes (PbuAmendPayload, a transaction header update),
	 * which re-inserting would undo.
	 */
	UnlockReleaseUndoBuffers();

	pfree(uur.uur_payload.data);
	return urp;
}

/*
 * FluxPbuFetchVersion - fetch one FLUX UNDO record for the version-chain
 * reader (FluxReconstructVisibleVersion).
 *
 * Returns false if the record was discarded / unreadable.  On success:
 *   *out_xid       = the xid that produced the CURRENT candidate image
 *                    (the payload's writer_xid, a possibly-SUBXACT xid) --
 *                    the reader tests this against its snapshot.  This is NOT
 *                    uur_xid (which is the owning top xid).
 *   *out_subtype   = the FLUX undo subtype (uur_type).
 *   *out_payload   = a palloc'd copy of the record's uur_payload (the
 *                    FluxUndoPayloadHeader + before-image tuple bytes); the
 *                    caller must pfree it.
 *   *out_payload_len = length of *out_payload.
 *
 * The payload is copied out (not aliased into the undo buffer) because
 * UndoRecordRelease frees the fetched record's buffers immediately.
 */
bool
FluxPbuFetchVersion(UndoRecPtr verptr, TransactionId *out_xid,
					CommandId *out_cid,
					uint16 *out_subtype, char **out_payload,
					Size *out_payload_len)
{
	UnpackedUndoRecord *urec;

	if (!UndoRecPtrIsValid(verptr))
		return false;

	urec = UndoFetchRecord(verptr, InvalidBlockNumber, InvalidOffsetNumber,
						   InvalidTransactionId, NULL, NULL);
	if (urec == NULL)
		return false;

	if (out_cid != NULL)
		*out_cid = urec->uur_cid;

	if (out_xid != NULL)
	{
		/*
		 * The producing (possibly SUBXACT) xid lives in the payload's
		 * FluxUndoPayloadHeader.writer_xid, not in uur_xid (which is the
		 * owning top xid the engine keys on).  Fall back to uur_xid if the
		 * payload is somehow too short.
		 */
		if (urec->uur_payload.len >= (int) SizeOfFluxUndoPayloadHeader)
		{
			FluxUndoPayloadHeader phdr;

			memcpy(&phdr, urec->uur_payload.data, SizeOfFluxUndoPayloadHeader);
			*out_xid = phdr.writer_xid;
		}
		else
			*out_xid = urec->uur_xid;
	}
	if (out_subtype != NULL)
		*out_subtype = urec->uur_type;

	if (out_payload != NULL)
	{
		Size		len = urec->uur_payload.len;

		if (len > 0)
		{
			char	   *buf = palloc(len);

			memcpy(buf, urec->uur_payload.data, len);
			*out_payload = buf;
		}
		else
			*out_payload = NULL;
		if (out_payload_len != NULL)
			*out_payload_len = len;
	}

	UndoRecordRelease(urec);
	return true;
}

/*
 * FluxPbuFetchForRevert - fetch one FLUX UNDO record for a writer that is
 * about to reverse-apply it (FluxRevertAbortedVersion).
 *
 * Like FluxPbuFetchVersion, but also returns the owning top-level xid
 * (uur_xid), which the apply callback and its CLR are keyed on, and copies
 * the whole payload.  Returns false if the record is gone.
 */
bool
FluxPbuFetchForRevert(UndoRecPtr urp, TransactionId *out_top_xid,
					  uint16 *out_subtype, char **out_payload,
					  Size *out_payload_len)
{
	UnpackedUndoRecord *urec;

	if (!UndoRecPtrIsValid(urp))
		return false;

	urec = UndoFetchRecord(urp, InvalidBlockNumber, InvalidOffsetNumber,
						   InvalidTransactionId, NULL, NULL);
	if (urec == NULL)
		return false;

	if (urec->uur_rmid != UNDO_RMID_FLUX ||
		urec->uur_payload.len < (int) SizeOfFluxUndoPayloadHeader)
	{
		UndoRecordRelease(urec);
		return false;
	}

	*out_top_xid = urec->uur_xid;
	*out_subtype = urec->uur_type;
	*out_payload_len = urec->uur_payload.len;
	*out_payload = palloc(urec->uur_payload.len);
	memcpy(*out_payload, urec->uur_payload.data, urec->uur_payload.len);

	UndoRecordRelease(urec);
	return true;
}

/*
 * FluxPbuFetchXid - header-only fetch: return just the producing xid of the
 * record at verptr (the payload's writer_xid, a possibly-SUBXACT xid).
 * Replaces the earlier fork-engine header probe on the lost-update conflict
 * probe (flux_mvcc.c) and the ANALYZE dead-tuple proxy (flux_handler.c).  Returns
 * false if the record was discarded / unreadable.
 */
bool
FluxPbuFetchXid(UndoRecPtr verptr, TransactionId *out_xid)
{
	UnpackedUndoRecord *urec;

	if (!UndoRecPtrIsValid(verptr))
		return false;

	urec = UndoFetchRecord(verptr, InvalidBlockNumber, InvalidOffsetNumber,
						   InvalidTransactionId, NULL, NULL);
	if (urec == NULL)
		return false;

	if (out_xid != NULL)
	{
		if (urec->uur_payload.len >= (int) SizeOfFluxUndoPayloadHeader)
		{
			FluxUndoPayloadHeader phdr;

			memcpy(&phdr, urec->uur_payload.data, SizeOfFluxUndoPayloadHeader);
			*out_xid = phdr.writer_xid;
		}
		else
			*out_xid = urec->uur_xid;
	}

	UndoRecordRelease(urec);
	return true;
}

/*
 * FluxPbuRecordExists - true iff the record at verptr has not been discarded.
 * Used by the ANALYZE dead-tuple proxy (flux_handler.c) as a cheap
 * "is there retained history?" probe.
 */
bool
FluxPbuRecordExists(UndoRecPtr verptr)
{
	UndoLogControl *log;
	bool		valid;

	if (!UndoRecPtrIsValid(verptr))
		return false;

	/*
	 * UndoRecordIsValid() asserts the caller holds log->discard_lock in
	 * shared mode (it protects the record from being discarded concurrently,
	 * and it reads log->oldest_data under that lock).  Acquire it here,
	 * matching every other UndoRecordIsValid() caller.  This path runs in
	 * autovacuum/ANALYZE workers concurrently with inserting backends and the
	 * discard worker.
	 *
	 * If the whole log has already been dropped, UndoLogGet() returns NULL
	 * and the record is certainly gone.
	 */
	log = UndoLogGet(UndoRecPtrGetLogNo(verptr));
	if (log == NULL)
		return false;

	LWLockAcquire(&log->discard_lock, LW_SHARED);
	valid = UndoRecordIsValid(verptr);

	/*
	 * UndoRecordIsValid() releases discard_lock itself when it returns false;
	 * on true it leaves the lock held for the caller to release.
	 */
	if (valid)
		LWLockRelease(&log->discard_lock);

	return valid;
}

/* See PbuInSubxactApply(). */
bool
FluxPbuInSubxactApply(void)
{
	return PbuInSubxactApply();
}

/* DML may freeze its before-image even without VACUUM. */
void
FluxPbuResolveRecoveryConflict(void *record)
{
	XLogReaderState *rec = (XLogReaderState *) record;

	if (!InHotStandby)
		return;
	for (int i = 1; i <= XLogRecMaxBlockId(rec); i++)
	{
		char	   *data;
		Size		len;
		xl_flux_undo hdr;
		RelFileLocator locator;

		if (!XLogRecHasBlockRef(rec, i) || !FluxPbuBlockIsUndo(rec, i))
			continue;
		data = XLogRecGetBlockData(rec, i, &len);
		if (data == NULL)
			continue;
		if (len < SizeOfFluxUndo)
			elog(PANIC, "FLUX undo freeze header truncated");
		memcpy(&hdr, data, SizeOfFluxUndo);
		XLogRecGetBlockTag(rec, 0, &locator, NULL, NULL);

		/*
		 * Resolve the snapshot conflict the freeze creates on the standby:
		 * read-only queries whose snapshot still needs the before-image the
		 * freeze cut off must be cancelled.  hdr.freeze_catalog_rel is always
		 * false for FLUX (user_catalog_table is refused and FLUX is never a
		 * system catalog), so the catalog branch inside
		 * ResolveRecoveryConflictWithSnapshot() is unreachable for FLUX; the
		 * flag is passed through unchanged to keep that resolution correct
		 * for a genuine catalog relation.
		 */
		ResolveRecoveryConflictWithSnapshot(hdr.freeze_conflict,
											hdr.freeze_catalog_rel, locator);
		break;
	}
}
