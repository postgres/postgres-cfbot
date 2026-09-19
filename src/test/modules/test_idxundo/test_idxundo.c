/*-------------------------------------------------------------------------
 *
 * test_idxundo.c
 *	  SQL-callable probes for the index UNDO tests.
 *
 * The tests drive index UNDO end to end through the real abort path: a table
 * created WITH (index_undo = on) makes its indexes write UNDO, and ROLLBACK
 * reverses the provisional entries.  These functions only observe the result:
 * how many index entries are LP_DEAD, and whether the current transaction has
 * published an UNDO chain for AtAbort_XactUndo() to walk.
 *
 * This is a test-only module.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/atm.h"
#include "access/hash.h"
#include "access/nbtree.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/slog.h"
#include "access/xact.h"
#include "access/undormgr.h"
#include "access/xactundo.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/injection_point.h"
#include "utils/guc.h"
#include "funcapi.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/*
 * idxundo_count_dead_all(index regclass) -> total LP_DEAD items in the index
 *
 * The end-to-end tests do not know which block the aborted entries landed on,
 * so they count over every entry-bearing page of the index: btree leaves, hash
 * bucket and overflow pages.
 */
PG_FUNCTION_INFO_V1(idxundo_count_dead_all);
Datum
idxundo_count_dead_all(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	indexrel;
	BlockNumber blk,
				nblocks;
	Oid			relam;
	int			ndead = 0;

	indexrel = relation_open(indexoid, AccessShareLock);
	nblocks = RelationGetNumberOfBlocks(indexrel);
	relam = indexrel->rd_rel->relam;

	if (relam != BTREE_AM_OID && relam != HASH_AM_OID)
		ereport(ERROR,
				(errmsg("index \"%s\" is neither a btree nor a hash index",
						RelationGetRelationName(indexrel))));

	for (blk = 0; blk < nblocks; blk++)
	{
		Buffer		buffer = ReadBuffer(indexrel, blk);
		Page		page;
		OffsetNumber off,
					maxoff;

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);

		/*
		 * Only count line pointers on pages that actually hold index entries.
		 * Every other page kind must be skipped, not merely tested for
		 * emptiness: a hash bitmap page is a dense run of set bits, which
		 * read as line pointers whose lp_flags happen to be LP_DEAD. Counting
		 * those reports over a thousand phantom dead entries in a freshly
		 * built hash index and makes the before/after comparison meaningless.
		 */
		if (!PageIsNew(page) && !PageIsEmpty(page) &&
			PageGetSpecialSize(page) > 0 &&
			(relam == BTREE_AM_OID
			 ? P_ISLEAF(BTPageGetOpaque(page))
			 : ((HashPageGetOpaque(page)->hasho_flag & LH_PAGE_TYPE) == LH_BUCKET_PAGE ||
				(HashPageGetOpaque(page)->hasho_flag & LH_PAGE_TYPE) == LH_OVERFLOW_PAGE)))
		{
			maxoff = PageGetMaxOffsetNumber(page);
			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				if (ItemIdIsDead(PageGetItemId(page, off)))
					ndead++;
			}
		}
		UnlockReleaseBuffer(buffer);
	}
	relation_close(indexrel, AccessShareLock);

	PG_RETURN_INT32(ndead);
}

/*
 * idxundo_undo_chain_published() -> bool
 *
 * True when the current transaction has UNDO that AtAbort_XactUndo() will act
 * on: either a permanent-persistence batch LSN already recorded in
 * XactUndo.last_batch_lsn[], or records still sitting in the deferred batch that
 * the abort path flushes before it reads that LSN.
 *
 * An insert that produced a valid XLOG_UNDO_BATCH but published no chain head
 * would make ROLLBACK a silent no-op; this returns false in that case.
 *
 * Both states count as published, because index UNDO DEFERS its records so a
 * statement's entries share one batch (DeferXactUndoData).  Mid-transaction
 * the records are normally still pending, and that is correct -- what would be a
 * bug is neither: no pending records AND no chain head means the insert produced
 * no UNDO at all.
 */
PG_FUNCTION_INFO_V1(idxundo_undo_chain_published);
Datum
idxundo_undo_chain_published(PG_FUNCTION_ARGS)
{
	XLogRecPtr	lsn = GetCurrentXactLastBatchLSN(UNDOPERSISTENCE_PERMANENT);

	PG_RETURN_BOOL(!XLogRecPtrIsInvalid(lsn) || XactUndoHasPendingData());
}

/*
 * idxundo_atm_entries() -> setof (xid, dboid, last_batch_lsn, stop_lsn,
 *								   revert_complete)
 *
 * Every entry in the Aborted Transaction Map.  Lets a test see whether an abort
 * was handed to the logical revert worker (and for which database) or applied
 * inline, whether a crash-recovery deferral was ever drained, and whether an
 * entry covers a bounded (subtransaction) chain.
 */
PG_FUNCTION_INFO_V1(idxundo_atm_entries);
Datum
idxundo_atm_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	SLogTxnEntry *entries;
	int			n;

	InitMaterializedSRF(fcinfo, 0);

	n = SLogTxnSnapshotForCheckpoint(&entries);
	for (int i = 0; i < n; i++)
	{
		Datum		values[5];
		bool		nulls[5] = {0};

		values[0] = TransactionIdGetDatum(entries[i].xid);
		values[1] = ObjectIdGetDatum(entries[i].dboid);
		values[2] = LSNGetDatum(entries[i].last_batch_lsn);
		values[3] = LSNGetDatum(entries[i].stop_lsn);
		nulls[3] = XLogRecPtrIsInvalid(entries[i].stop_lsn);
		values[4] = BoolGetDatum(entries[i].revert_complete);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}

/*
 * idxundo_atm_add(xid xid, lsn pg_lsn) -> bool
 *
 * Queue xid in the ATM for this database with an UNDO chain at lsn, which
 * need not be an UNDO batch: pointing it at some other WAL record makes a
 * malformed chain.
 */
PG_FUNCTION_INFO_V1(idxundo_atm_add);
Datum
idxundo_atm_add(PG_FUNCTION_ARGS)
{
	TransactionId xid = DatumGetTransactionId(PG_GETARG_DATUM(0));
	XLogRecPtr	lsn = PG_GETARG_LSN(1);

	PG_RETURN_BOOL(ATMAddAborted(xid, MyDatabaseId, lsn, lsn, false));
}

/*
 * A test UNDO resource manager whose records always fail to apply.
 *
 * It is registered in the calling backend only, on first use, so only that
 * backend's own (inline) rollback can dispatch to it; anywhere else -- the
 * logical revert worker -- its records are of an unknown resource manager,
 * which fails just the same.
 */
#define TEST_IDXUNDO_RMID	200

static UndoApplyResult
test_idxundo_failing_apply(uint8 rmid, uint16 info, TransactionId xid,
						   Oid reloid, const char *payload, Size payload_len,
						   UndoRecPtr urec_ptr)
{
	return UNDO_APPLY_ERROR;
}

static const UndoRmgrData test_idxundo_failing_rmgr = {
	.rm_name = "test_idxundo_failing",
	.rm_undo = test_idxundo_failing_apply,
};

/*
 * idxundo_write_failing_undo(persistence "char" DEFAULT 'p') -> void
 *
 * Write one UNDO record, in the current (sub)transaction, that cannot be
 * applied, into the chain of the given relpersistence ('p', 'u' or 't').
 * Any index UNDO still deferred is written in the same batch.
 */
PG_FUNCTION_INFO_V1(idxundo_write_failing_undo);
Datum
idxundo_write_failing_undo(PG_FUNCTION_ARGS)
{
	XactUndoContext ctx;

	if (GetUndoRmgr(TEST_IDXUNDO_RMID) == NULL)
		RegisterUndoRmgr(TEST_IDXUNDO_RMID, &test_idxundo_failing_rmgr);

	PrepareXactUndoData(&ctx, PG_GETARG_CHAR(0), TEST_IDXUNDO_RMID, 0,
						InvalidOid, NULL, 0);
	START_CRIT_SECTION();
	InsertXactUndoData(&ctx);
	CleanupXactUndoInsertion(&ctx);
	END_CRIT_SECTION();

	PG_RETURN_VOID();
}

/*
 * A test UNDO resource manager that logs the order its records are applied in.
 *
 * Registered only when the module is in shared_preload_libraries, from
 * shmem_startup_hook: the core zeroes the dispatch table while it sets up
 * shared memory, after _PG_init has run.  Every process forked by the
 * postmaster -- the startup process included -- inherits the registration.
 */
#define TEST_IDXUNDO_ORDER_RMID	201

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static bool recovery_error = false;

static UndoApplyResult
test_idxundo_order_apply(uint8 rmid, uint16 info, TransactionId xid,
						 Oid reloid, const char *payload, Size payload_len,
						 UndoRecPtr urec_ptr)
{
	int32		seq;

	if (payload_len != sizeof(int32))
		return UNDO_APPLY_ERROR;
	memcpy(&seq, payload, sizeof(int32));
	elog(LOG, "test_idxundo: applying UNDO record %d of transaction %u",
		 seq, xid);
	return UNDO_APPLY_SUCCESS;
}

static const UndoRmgrData test_idxundo_order_rmgr = {
	.rm_name = "test_idxundo_order",
	.rm_undo = test_idxundo_order_apply,
};

static void
test_idxundo_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	RegisterUndoRmgr(TEST_IDXUNDO_ORDER_RMID, &test_idxundo_order_rmgr);
#ifdef USE_INJECTION_POINTS
	if (recovery_error)
		InjectionPointAttach("pbu-recovery-error", "test_idxundo",
							 "idxundo_recovery_error", NULL, 0);
#endif
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;
	DefineCustomBoolVariable("test_idxundo.recovery_error",
							 "Inject an error during per-backend undo recovery.",
							 NULL, &recovery_error, false, PGC_POSTMASTER, 0,
							 NULL, NULL, NULL);
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_idxundo_shmem_startup;
}

/*
 * idxundo_write_ordered_undo(n int) -> void
 *
 * Write records 1..n of the order-logging resource manager, all in ONE batch.
 * Requires the module in shared_preload_libraries.
 */
PG_FUNCTION_INFO_V1(idxundo_write_ordered_undo);
Datum
idxundo_write_ordered_undo(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	XactUndoContext ctx;

	if (GetUndoRmgr(TEST_IDXUNDO_ORDER_RMID) == NULL)
		elog(ERROR, "test_idxundo must be in shared_preload_libraries");

	for (int32 seq = 1; seq <= n; seq++)
		PrepareXactUndoData(&ctx, RELPERSISTENCE_PERMANENT,
							TEST_IDXUNDO_ORDER_RMID, 0, InvalidOid,
							(const char *) &seq, sizeof(int32));
	START_CRIT_SECTION();
	InsertXactUndoData(&ctx);
	CleanupXactUndoInsertion(&ctx);
	END_CRIT_SECTION();

	PG_RETURN_VOID();
}

/* Throw with both a buffer pin and an LWLock outstanding. */
extern PGDLLEXPORT void idxundo_error_with_resources(const char *name,
													const void *private_data,
													void *arg);
void
idxundo_error_with_resources(const char *name, const void *private_data, void *arg)
{
	Relation rel = table_open(RelationRelationId, AccessShareLock);

	(void) ReadBuffer(rel, 0);
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	elog(ERROR, "test_idxundo: error with resources at %s", name);
}

extern PGDLLEXPORT void idxundo_recovery_error(const char *name,
											 const void *private_data, void *arg);
void
idxundo_recovery_error(const char *name, const void *private_data, void *arg)
{
	elog(ERROR, "test_idxundo: recovery error");
}
