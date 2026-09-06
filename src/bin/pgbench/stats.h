/*-------------------------------------------------------------------------
 *
 * stats.h
 *		pgbench statistics and latency tracking
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/stats.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_STATS_H
#define PGBENCH_STATS_H

#include "portability/instr_time.h"

/*
 * The instr_time type is expensive when dealing with time arithmetic.  Define
 * a type to hold microseconds instead.  Type int64 is good enough for about
 * 584500 years.
 */
typedef int64 pg_time_usec_t;

/*
 * Return current timestamp in microseconds.
 */
static inline pg_time_usec_t
pg_time_now(void)
{
	instr_time	now;

	INSTR_TIME_SET_CURRENT(now);

	return (pg_time_usec_t) INSTR_TIME_GET_MICROSEC(now);
}

/*
 * Lazily initialize timestamp if not already set.
 */
static inline void
pg_time_now_lazy(pg_time_usec_t *now)
{
	if ((*now) == 0)
		(*now) = pg_time_now();
}

#define PG_TIME_GET_DOUBLE(t) (0.000001 * (t))

/*
 * Error status for errors during script execution.
 */
typedef enum EStatus
{
	ESTATUS_NO_ERROR = 0,
	ESTATUS_META_COMMAND_ERROR,
	ESTATUS_CONN_ERROR,

	/* SQL errors */
	ESTATUS_SERIALIZATION_ERROR,
	ESTATUS_DEADLOCK_ERROR,
	ESTATUS_OTHER_SQL_ERROR,
} EStatus;

/*
 * Simple data structure to keep stats about something.
 *
 * XXX probably the first value should be kept and used as an offset for
 * better numerical stability...
 */
typedef struct SimpleStats
{
	int64		count;			/* how many values were encountered */
	double		min;			/* the minimum seen */
	double		max;			/* the maximum seen */
	double		sum;			/* sum of values */
	double		sum2;			/* sum of squared values */
} SimpleStats;

/*
 * Data structure to hold various statistics: per-thread and per-script stats
 * are maintained and merged together.
 */
typedef struct StatsData
{
	pg_time_usec_t start_time;	/* interval start time, for aggregates */

	/*----------
	 * Transactions are counted depending on their execution and outcome.
	 * First a transaction may have started or not: skipped transactions occur
	 * under --rate and --latency-limit when the client is too late to execute
	 * them. Secondly, a started transaction may ultimately succeed or fail,
	 * possibly after some retries when --max-tries is not one. Thus
	 *
	 * the number of all transactions =
	 *   'cnt_skipped' (it was too late to execute them) +
	 *   'cnt' (the number of successful transactions) +
	 *   'failed' (the number of failed transactions).
	 *
	 * A successful transaction can have several unsuccessful tries before a
	 * successful run. Thus
	 *
	 * 'cnt' (the number of successful transactions) =
	 *   successfully retried transactions (they got a serialization or a
	 *                                      deadlock error(s), but were
	 *                                      successfully retried from the very
	 *                                      beginning) +
	 *   directly successful transactions (they were successfully completed on
	 *                                     the first try).
	 *
	 * 'failed' (the number of failed transactions) =
	 *   'serialization_failures' (they got a serialization error and were not
	 *                        successfully retried) +
	 *   'deadlock_failures' (they got a deadlock error and were not
	 *                        successfully retried) +
	 *   'other_sql_failures'  (they failed on the first try or after retries
	 *                        due to a SQL error other than serialization or
	 *                        deadlock; they are counted as a failed transaction
	 *                        only when --continue-on-error is specified).
	 *
	 * If the transaction was retried after a serialization or a deadlock
	 * error this does not guarantee that this retry was successful. Thus
	 *
	 * 'retries' (number of retries) =
	 *   number of retries in all retried transactions =
	 *   number of retries in (successfully retried transactions +
	 *                         failed transactions);
	 *
	 * 'retried' (number of all retried transactions) =
	 *   successfully retried transactions +
	 *   unsuccessful retried transactions.
	 *----------
	 */
	int64		cnt;			/* number of successful transactions, not
								 * including 'cnt_skipped' */
	int64		cnt_skipped;	/* number of transactions skipped under --rate
								 * and --latency-limit */
	int64		retries;		/* number of retries after a serialization or
								 * a deadlock error in all the transactions */
	int64		retried;		/* number of all transactions that were
								 * retried after a serialization or a deadlock
								 * error (perhaps the last try was
								 * unsuccessful) */
	int64		serialization_failures; /* number of transactions that were
										 * not successfully retried after a
										 * serialization error */
	int64		deadlock_failures;	/* number of transactions that were not
									 * successfully retried after a deadlock
									 * error */
	int64		other_sql_failures; /* number of failed transactions for
									 * reasons other than
									 * serialization/deadlock failure, which
									 * is counted if --continue-on-error is
									 * specified */
	SimpleStats latency;
	SimpleStats lag;
} StatsData;

extern void initSimpleStats(SimpleStats *ss);
extern void addToSimpleStats(SimpleStats *ss, double val);
extern void mergeSimpleStats(SimpleStats *acc, const SimpleStats *ss);
extern void printSimpleStats(const char *prefix, const SimpleStats *ss);

extern void initStats(StatsData *sd, pg_time_usec_t start);
extern void accumStats(StatsData *stats, bool tx_skipped, double lat, double lag,
					   EStatus estatus, int64 tries, bool has_throttle_delay);
extern void mergeStats(StatsData *acc, const StatsData *src);
extern int64 getFailures(const StatsData *stats);
extern const char *getResultString(bool tx_skipped, EStatus estatus, bool failures_detailed);

#endif							/* PGBENCH_STATS_H */
