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
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/*
 * idxundo_count_dead_all(index regclass) -> total LP_DEAD items in the index
 *
 * The end-to-end tests do not know which block the aborted entries landed on,
 * so they count over every leaf page of the index.
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

	if (relam != BTREE_AM_OID)
		ereport(ERROR,
				(errmsg("index \"%s\" is not a btree index",
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
		 * Only leaf pages hold index entries; skip the metapage and
		 * internals.
		 */
		if (!PageIsNew(page) && !PageIsEmpty(page) &&
			PageGetSpecialSize(page) > 0 &&
			P_ISLEAF(BTPageGetOpaque(page)))
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

	ATMEnsureCreated();
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

	/*
	 * The rmgr registration above is per-process and must run in every
	 * process, but the injection point lives in shared memory: attach it
	 * once, from the postmaster.  Under EXEC_BACKEND every child runs this
	 * hook again, and a second attach fails with "already defined".
	 */
	if (recovery_error && !IsUnderPostmaster)
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

/*
 * Sleep for the number of milliseconds given, as text, in private_data,
 * without checking for interrupts, reporting wait event PgSleep: makes the
 * code around an injection point slow without giving it a chance to notice a
 * signal.  A signal still cuts the sleep short.
 */
extern PGDLLEXPORT void idxundo_sleep(const char *name,
									  const void *private_data, void *arg);
void
idxundo_sleep(const char *name, const void *private_data, void *arg)
{
	pgstat_report_wait_start(WAIT_EVENT_PG_SLEEP);
	pg_usleep(atol((const char *) private_data) * 1000L);
	pgstat_report_wait_end();
}


/*
 * idxundo_launcher_dropped_db() -> bool
 *
 * Run the undo launcher's UndoGetWork(false, false, ...) against a planted
 * rollback request whose database (InvalidOid) does not exist.  Before the
 * fix this self-deadlocked: UndoGetWork ran a transaction and re-entered
 * RollbackRequestLock -- both under that lock.  Returns whether work was
 * reported (expected false: the request is for a dropped database).
 */
PG_FUNCTION_INFO_V1(idxundo_plant_dropped_db_request);
Datum
idxundo_plant_dropped_db_request(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	/*
	 * The per-backend UNDO headers (pbu_undorequest.h) clash with the
	 * cluster-wide undorecord.h this file already pulls in via xactundo.h, so
	 * declare the probes we need here instead of including that header.
	 */
	extern void PbuTestPlantDroppedDbRequest(void);

	/*
	 * Plant a request for a database that does not exist, then return: the
	 * always-running undo launcher reaches its dropped-database branch on its
	 * own next cycle (outside any transaction, as in production) and, before
	 * the fix, self-deadlocks there.
	 */
	PbuTestPlantDroppedDbRequest();
	PG_RETURN_VOID();
#else
	elog(ERROR, "test_idxundo built without injection points");
	PG_RETURN_VOID();
#endif
}

/*
 * idxundo_round_robin_finds_xid() -> bool
 *
 * Plant one servable request (this backend's database) in XID_QUEUE, point
 * the round robin at the empty ERROR_QUEUE, and run a worker's UndoGetWork.
 * Before the fix the empty-error-queue branch advanced the round-robin cursor
 * itself, and the caller advanced it again, so the three-iteration scan
 * skipped XID_QUEUE and found nothing.  Returns whether the request was found.
 */
PG_FUNCTION_INFO_V1(idxundo_round_robin_finds_xid);
Datum
idxundo_round_robin_finds_xid(PG_FUNCTION_ARGS)
{
#ifdef USE_INJECTION_POINTS
	extern bool PbuTestPlantAndGetWork(Oid dbid, bool is_undo_launcher,
									   int start_queue);

	PG_RETURN_BOOL(PbuTestPlantAndGetWork(MyDatabaseId, false, 2 /* ERROR_QUEUE */));
#else
	elog(ERROR, "test_idxundo built without injection points");
	PG_RETURN_BOOL(false);
#endif
}


/*
 * Counting how many batches an inline rollback fetches.
 *
 * execute_undo_actions() fires pbu-undo-apply-batch once per
 * UndoRecordBulkFetch() call.  Attaching idxundo_count_batch to it and
 * reading idxundo_batch_count() after an inline (same-backend) rollback tells
 * a test how many batches the rollback took: one when the per-batch memory
 * bound (maintenance_work_mem) is honored, one per record when the bound is
 * computed wrong and collapses to zero.
 */
static int	idxundo_batch_fires = 0;

extern PGDLLEXPORT void idxundo_count_batch(const char *name,
										   const void *private_data, void *arg);
void
idxundo_count_batch(const char *name, const void *private_data, void *arg)
{
	idxundo_batch_fires++;
}

/* idxundo_reset_batch_count() -> void : zero the batch counter. */
PG_FUNCTION_INFO_V1(idxundo_reset_batch_count);
Datum
idxundo_reset_batch_count(PG_FUNCTION_ARGS)
{
	idxundo_batch_fires = 0;
	PG_RETURN_VOID();
}

/* idxundo_batch_count() -> int : batches fetched since the last reset. */
PG_FUNCTION_INFO_V1(idxundo_batch_count);
Datum
idxundo_batch_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(idxundo_batch_fires);
}

/*
 * idxundo_bulk_fetch_stale_log() -> int
 *
 * Call UndoRecordBulkFetch() with a from_urecptr whose undo-log number decodes
 * to 0 -- a valid (non-zero) UndoRecPtr that names a log that was never
 * allocated: the "stale UndoRecPtr whose log number decodes to 0" that
 * UndoLogGet() returns NULL for (see get_undo_log_by_number()).  Every other
 * UndoLogGet() caller treats that NULL as "the record is gone"; this was the
 * one path that dereferenced it.  Returns the number of records fetched (0:
 * the chain's log is gone).  Crashes before the fix.
 *
 * UndoRecordBulkFetch is declared here (returning void *) rather than via
 * pbu_undorequest.h, whose UndoRecInfo/RelFileNode shim clashes with the
 * cluster-wide undorecord.h this file already pulls in.
 */
PG_FUNCTION_INFO_V1(idxundo_bulk_fetch_stale_log);
Datum
idxundo_bulk_fetch_stale_log(PG_FUNCTION_ARGS)
{
	extern void *UndoRecordBulkFetch(UndoRecPtr *from_urecptr,
									 UndoRecPtr to_urecptr,
									 Size undo_apply_size,
									 int *nrecords, bool one_page);
	/* log number 0, a non-zero offset: valid pointer, log never allocated. */
	UndoRecPtr	from = UINT64CONST(0x1000);
	int			nrecords = -1;

	(void) UndoRecordBulkFetch(&from, InvalidUndoRecPtr, 1024 * 1024,
							   &nrecords, false);
	PG_RETURN_INT32(nrecords);
}
