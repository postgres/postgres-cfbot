/*-------------------------------------------------------------------------
 *
 * test_session_buffer_usage.c
 *	  show buffer usage statistics for the current session
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *	  src/test/modules/test_session_buffer_usage/test_session_buffer_usage.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_session_buffer_usage",
					.version = PG_VERSION
);

#define NUM_BUFFER_USAGE_COLUMNS 16

PG_FUNCTION_INFO_V1(test_session_buffer_usage);
PG_FUNCTION_INFO_V1(test_session_buffer_usage_reset);

/*
 * test_session_buffer_usage.track_queries
 *
 * When on, request query-level buffer/WAL instrumentation for every query
 * executed in this session, the same way pg_stat_statements does.  This lets
 * the tests exercise the QueryInstrumentation code paths (resource owner
 * registration, abort handling, accumulation into the session totals)
 * without depending on a contrib module.
 */
static bool track_queries = false;

static ExecutorStart_hook_type prev_ExecutorStart = NULL;

static void
tsbu_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (track_queries)
		queryDesc->query_instr_options |= INSTRUMENT_BUFFERS | INSTRUMENT_WAL;

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("test_session_buffer_usage.track_queries",
							 "Request query-level buffer/WAL instrumentation for all queries.",
							 NULL,
							 &track_queries,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	MarkGUCPrefixReserved("test_session_buffer_usage");

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = tsbu_ExecutorStart;
}

#define HAVE_INSTR_STACK 1		/* Change to 0 when testing before stack
								 * change */

/*
 * Snapshot of the session counters taken by test_session_buffer_usage_reset().
 *
 * The session-level totals must never be reset, since the cumulative stats
 * system (pg_stat_database I/O timings) relies on them only ever increasing,
 * so report everything relative to this baseline instead.
 */
static BufferUsage baseline;

#define DIFF_COUNTER(fld) ((int64) (usage->fld - baseline.fld))
#define DIFF_TIME_MS(fld) \
	(INSTR_TIME_GET_MILLISEC(usage->fld) - INSTR_TIME_GET_MILLISEC(baseline.fld))

/*
 * SQL function: test_session_buffer_usage()
 *
 * Returns a single row with all BufferUsage counters accumulated since the
 * start of the session, or since the last call to
 * test_session_buffer_usage_reset(). Excludes any usage not yet added to the
 * top of the stack (e.g. if this gets called inside a statement that also had
 * buffer activity).
 */
Datum
test_session_buffer_usage(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[NUM_BUFFER_USAGE_COLUMNS];
	bool		nulls[NUM_BUFFER_USAGE_COLUMNS];
	BufferUsage *usage;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(nulls, 0, sizeof(nulls));

#if HAVE_INSTR_STACK
	usage = &instr_top.bufusage;
#else
	usage = &pgBufferUsage;
#endif

	values[0] = Int64GetDatum(DIFF_COUNTER(shared_blks_hit));
	values[1] = Int64GetDatum(DIFF_COUNTER(shared_blks_read));
	values[2] = Int64GetDatum(DIFF_COUNTER(shared_blks_dirtied));
	values[3] = Int64GetDatum(DIFF_COUNTER(shared_blks_written));
	values[4] = Int64GetDatum(DIFF_COUNTER(local_blks_hit));
	values[5] = Int64GetDatum(DIFF_COUNTER(local_blks_read));
	values[6] = Int64GetDatum(DIFF_COUNTER(local_blks_dirtied));
	values[7] = Int64GetDatum(DIFF_COUNTER(local_blks_written));
	values[8] = Int64GetDatum(DIFF_COUNTER(temp_blks_read));
	values[9] = Int64GetDatum(DIFF_COUNTER(temp_blks_written));
	values[10] = Float8GetDatum(DIFF_TIME_MS(shared_blk_read_time));
	values[11] = Float8GetDatum(DIFF_TIME_MS(shared_blk_write_time));
	values[12] = Float8GetDatum(DIFF_TIME_MS(local_blk_read_time));
	values[13] = Float8GetDatum(DIFF_TIME_MS(local_blk_write_time));
	values[14] = Float8GetDatum(DIFF_TIME_MS(temp_blk_read_time));
	values[15] = Float8GetDatum(DIFF_TIME_MS(temp_blk_write_time));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * SQL function: test_session_buffer_usage_reset()
 *
 * Makes test_session_buffer_usage() report counters relative to the current
 * session totals. Useful in tests to avoid the baseline/delta pattern.
 */
Datum
test_session_buffer_usage_reset(PG_FUNCTION_ARGS)
{
#if HAVE_INSTR_STACK
	baseline = instr_top.bufusage;
#else
	baseline = pgBufferUsage;
#endif

	PG_RETURN_VOID();
}
