/*-------------------------------------------------------------------------
 *
 * report.h
 *	  Transaction logging, progress, and result reporting for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/report.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPORT_H
#define REPORT_H

#include "engine.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "stats.h"

extern void doLog(TState *thread, CState *st,
				  StatsData *agg, bool tx_skipped, double latency, double lag);
extern void processXactStats(TState *thread, CState *st, pg_time_usec_t *now,
							 bool tx_skipped, StatsData *agg);
extern void printProgressReport(TState *threads, int64 test_start, pg_time_usec_t now,
								StatsData *last, int64 *last_report);
extern void printVersion(PGconn *con);
extern void printResults(StatsData *total,
						 pg_time_usec_t total_duration,
						 pg_time_usec_t conn_total_duration,
						 pg_time_usec_t conn_elapsed_duration,
						 int64 latency_late);

#endif							/* REPORT_H */
