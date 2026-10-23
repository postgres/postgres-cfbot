/*-------------------------------------------------------------------------
 *
 * logdb.h
 *		Direct PostgreSQL database logging and reporting (--log-db) for pgbench.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/logdb.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_LOGDB_H
#define PGBENCH_LOGDB_H

#include "engine.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "pqexpbuffer.h"
#include "stats.h"

typedef enum LogDbUnloggedMode
{
	LOG_DB_UNLOGGED_OFF = 0,	/* all tables LOGGED */
	LOG_DB_UNLOGGED_STREAM,		/* Tier B & C UNLOGGED, Tier A LOGGED */
	LOG_DB_UNLOGGED_ALL			/* all tables UNLOGGED */
} LogDbUnloggedMode;

typedef enum LogDbMode
{
	LOG_DB_MODE_STREAM = 0,		/* live per-thread 64KB buffered COPY FROM STDIN */
	LOG_DB_MODE_DEFERRED		/* spool to local files during run, bulk COPY after */
} LogDbMode;

extern char *log_db_conninfo;
extern LogDbUnloggedMode log_db_unlogged;
extern LogDbMode log_db_mode;
extern char *log_db_schema;
extern bool log_db_index;
extern char *run_label;
extern int64 current_run_id;

extern void parseLogDbUnloggedOption(const char *val);
extern void parseLogDbModeOption(const char *val);
extern const char *logDbUnloggedToString(LogDbUnloggedMode mode);
extern const char *logDbModeToString(LogDbMode mode);

#define parseLogDbUnloggedMode(val) parseLogDbUnloggedOption(val)
#define parseLogDbMode(val) parseLogDbModeOption(val)
#define logDbUnloggedModeToString(mode) logDbUnloggedToString(mode)
#define logDbGetRunId() (current_run_id)

extern void logDbBootstrapAndStartRun(void);
extern void logDbInitThread(TState *thread);
extern void logDbFlushThreadBuffer(TState *thread);
extern void logDbFinishThread(TState *thread);

extern void logDbRecordProgress(double ts_epoch_sec, double elapsed_sec,
								double tps, double latency_avg_ms,
								double latency_stddev_ms, double lag_avg_ms,
								int64 failed_cnt);

extern void logDbRecordAggInterval(TState *thread, const StatsData *agg,
								   bool has_throttle);

extern void logDbRecordTransaction(TState *thread, const CState *st,
								   pg_time_usec_t now_epoch_usec,
								   bool tx_skipped, double latency_us,
								   double lag_us, bool has_schedule);

extern void logDbFinalizeRun(StatsData *total, pg_time_usec_t total_duration,
							 TState *threads, int exit_code);

extern void logDbMarkRunAbortedOnExit(TState *thread, int exit_code);

#endif							/* PGBENCH_LOGDB_H */
