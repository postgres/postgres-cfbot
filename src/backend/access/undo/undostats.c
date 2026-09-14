/*-------------------------------------------------------------------------
 *
 * undostats.c
 *	  UNDO log statistics collection and reporting
 *
 * This module provides monitoring and observability for the UNDO
 * subsystem, including:
 *   - Per-log statistics (insert/discard pointers, size, oldest xid, state)
 *   - Buffer cache statistics (hits, misses, evictions)
 *   - Aggregate counters (total records, bytes generated)
 *   - Force discard and rotation SQL function
 *
 * Statistics can be queried via SQL functions pg_stat_get_undo_logs()
 * and pg_stat_get_undo_buffers(), registered in pg_proc.dat.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/undo/undostats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/undolog.h"
#include "access/undostats.h"
#include "access/undoworker.h"
#include "access/undo_xlog.h"
#include "catalog/pg_authid.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/acl.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(pg_stat_get_undo_logs);
PG_FUNCTION_INFO_V1(pg_stat_get_undo_buffers);
PG_FUNCTION_INFO_V1(pg_undo_force_discard);

/*
 * UndoLogStateToString - Convert lifecycle state to display string
 */
static const char *
UndoLogStateToString(UndoLogState state)
{
	switch (state)
	{
		case UNDO_LOG_FREE:
			return "free";
		case UNDO_LOG_ACTIVE:
			return "active";
		case UNDO_LOG_SEALED:
			return "sealed";
		case UNDO_LOG_DISCARDABLE:
			return "discardable";
	}
	return "unknown";
}

/*
 * GetUndoLogStats - Get statistics for all active UNDO logs
 *
 * Fills the provided array with stats for each active log.
 * Returns the number of active logs found.
 */
int
GetUndoLogStats(UndoLogStat *stats, int max_stats)
{
	int			count = 0;
	int			i;

	if (UndoLogShared == NULL)
		return 0;

	for (i = 0; i < MAX_UNDO_LOGS && count < max_stats; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (!log->in_use)
			continue;

		LWLockAcquire(&log->lock, LW_SHARED);

		stats[count].log_number = log->log_number;
		stats[count].insert_ptr = pg_atomic_read_u64(&log->insert_ptr);
		stats[count].discard_ptr = log->discard_ptr;
		stats[count].oldest_xid = log->oldest_xid;
		stats[count].state = log->state;

		/* Calculate size as difference between insert and discard offsets */
		stats[count].size_bytes =
			UndoRecPtrGetOffset(stats[count].insert_ptr) -
			UndoRecPtrGetOffset(log->discard_ptr);

		LWLockRelease(&log->lock);

		count++;
	}

	return count;
}

/*
 * GetUndoBufferStats - Get UNDO buffer statistics
 *
 * With the shared_buffers integration, UNDO pages are managed by the
 * standard buffer pool.  Dedicated UNDO buffer statistics are no longer
 * tracked separately.  This function returns zeros for all counters.
 * Use pg_buffercache to inspect UNDO pages in shared_buffers if needed.
 */
void
GetUndoBufferStats(UndoBufferStat *stats)
{
	stats->num_buffers = 0;
	stats->cache_hits = 0;
	stats->cache_misses = 0;
	stats->cache_evictions = 0;
	stats->cache_writes = 0;
}

/*
 * pg_stat_get_undo_logs - SQL-callable function returning UNDO log stats
 *
 * Returns a set of rows, one per active UNDO log, with columns:
 *   log_number, insert_offset, discard_offset, size_bytes, oldest_xid, state
 */
Datum
pg_stat_get_undo_logs(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	UndoLogStat *stats;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;
		TupleDesc	tupdesc;
		int			nstats;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Use the result type declared for this function in the catalog. */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/* Collect stats snapshot */
		stats = (UndoLogStat *) palloc(sizeof(UndoLogStat) * MAX_UNDO_LOGS);
		nstats = GetUndoLogStats(stats, MAX_UNDO_LOGS);

		funcctx->user_fctx = stats;
		funcctx->max_calls = nstats;

		MemoryContextSwitchTo(oldcxt);
	}

	funcctx = SRF_PERCALL_SETUP();
	stats = (UndoLogStat *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		UndoLogStat *stat = &stats[funcctx->call_cntr];
		Datum		values[6];
		bool		nulls[6];
		HeapTuple	tuple;

		MemSet(nulls, 0, sizeof(nulls));

		values[0] = Int32GetDatum(stat->log_number);
		values[1] = Int64GetDatum(UndoRecPtrGetOffset(stat->insert_ptr));
		values[2] = Int64GetDatum(UndoRecPtrGetOffset(stat->discard_ptr));
		values[3] = Int64GetDatum(stat->size_bytes);
		values[4] = TransactionIdGetDatum(stat->oldest_xid);
		values[5] = CStringGetTextDatum(UndoLogStateToString(stat->state));

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * pg_stat_get_undo_buffers - SQL-callable function returning buffer stats
 *
 * Returns a single row with UNDO buffer cache statistics:
 *   num_buffers, cache_hits, cache_misses, cache_evictions, cache_writes,
 *   hit_ratio
 */
Datum
pg_stat_get_undo_buffers(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6];
	HeapTuple	tuple;
	UndoBufferStat stats;

	/* Use the result type declared for this function in the catalog. */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	/* Get statistics */
	GetUndoBufferStats(&stats);

	MemSet(nulls, 0, sizeof(nulls));

	values[0] = Int32GetDatum(stats.num_buffers);
	values[1] = Int64GetDatum(stats.cache_hits);
	values[2] = Int64GetDatum(stats.cache_misses);
	values[3] = Int64GetDatum(stats.cache_evictions);
	values[4] = Int64GetDatum(stats.cache_writes);

	/* Calculate hit ratio */
	{
		uint64		total = stats.cache_hits + stats.cache_misses;

		if (total > 0)
			values[5] = Float4GetDatum((float4) stats.cache_hits / total);
		else
			values[5] = Float4GetDatum(0.0);
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * pg_undo_force_discard - Force UNDO log discard and optional rotation
 *
 * SQL-callable function that performs immediate discard of reclaimable
 * UNDO records and optionally rotates the active log segment.
 *
 * Arguments:
 *   force_rotate (bool) - If true, seal and rotate the active log first
 *
 * Returns the number of log segments freed (int4).
 *
 * Requires the pg_maintain role for access.
 */
Datum
pg_undo_force_discard(PG_FUNCTION_ARGS)
{
	bool		force_rotate = PG_GETARG_BOOL(0);
	int			freed_count = 0;
	TransactionId oldest_xid;
	int			i;

	/* Permission check: require pg_maintain role */
	if (!has_privs_of_role(GetUserId(), ROLE_PG_MAINTAIN))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be a member of pg_maintain to force UNDO discard")));

	if (UndoLogShared == NULL)
		ereport(ERROR,
				(errmsg("UNDO subsystem is not initialized")));

	/* Optional rotation */
	if (force_rotate)
		UndoLogSealAndRotate(UNDO_ROTATE_MANUAL);

	/* Perform inline discard (same two passes as the discard worker) */
	oldest_xid = UndoWorkerGetOldestXid();
	if (!TransactionIdIsValid(oldest_xid))
		oldest_xid = ReadNextTransactionId();

	/* First pass: advance discard pointers */
	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (!log->in_use)
			continue;

		LWLockAcquire(&log->lock, LW_EXCLUSIVE);

		if (TransactionIdIsValid(log->oldest_xid) &&
			TransactionIdPrecedes(log->oldest_xid, oldest_xid))
		{
			UndoRecPtr	insert_ptr = pg_atomic_read_u64(&log->insert_ptr);

			if (UndoRecPtrGetOffset(insert_ptr) >
				UndoRecPtrGetOffset(log->discard_ptr))
			{
				log->discard_ptr = insert_ptr;
				log->oldest_xid = oldest_xid;
			}
		}

		LWLockRelease(&log->lock);
	}

	/* Second pass: lifecycle transitions */
	for (i = 0; i < MAX_UNDO_LOGS; i++)
	{
		UndoLogControl *log = &UndoLogShared->logs[i];

		if (!log->in_use)
			continue;

		LWLockAcquire(&log->lock, LW_EXCLUSIVE);

		/* SEALED -> DISCARDABLE if fully discarded */
		if (log->state == UNDO_LOG_SEALED)
		{
			UndoRecPtr	seal = pg_atomic_read_u64(&log->seal_ptr);
			UndoRecPtr	discard = log->discard_ptr;

			if (UndoRecPtrIsValid(seal) &&
				UndoRecPtrGetOffset(discard) >= UndoRecPtrGetOffset(seal))
			{
				log->state = UNDO_LOG_DISCARDABLE;
			}
		}

		/* DISCARDABLE -> FREE: clean up */
		if (log->state == UNDO_LOG_DISCARDABLE)
		{
			uint32		log_number = log->log_number;

			log->in_use = false;
			log->state = UNDO_LOG_FREE;
			log->log_number = 0;
			pg_atomic_write_u64(&log->insert_ptr, InvalidUndoRecPtr);
			log->discard_ptr = InvalidUndoRecPtr;
			log->oldest_xid = InvalidTransactionId;
			pg_atomic_write_u64(&log->seal_ptr, InvalidUndoRecPtr);
			log->sealed_time = 0;

			LWLockRelease(&log->lock);

			UndoLogDeleteSegmentFile(log_number);
			freed_count++;
			continue;
		}

		LWLockRelease(&log->lock);
	}

	/* Wake the background worker for any remaining work */
	WakeUndoDiscardWorker();

	PG_RETURN_INT32(freed_count);
}
