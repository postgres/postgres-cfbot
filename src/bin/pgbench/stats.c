/*-------------------------------------------------------------------------
 *
 * stats.c
 *		pgbench statistics and latency tracking
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/stats.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <math.h>

#include "common/logging.h"
#include "stats.h"

/*
 * Initialize the given SimpleStats struct to all zeroes
 */
void
initSimpleStats(SimpleStats *ss)
{
	memset(ss, 0, sizeof(SimpleStats));
}

/*
 * Accumulate one value into a SimpleStats struct.
 */
void
addToSimpleStats(SimpleStats *ss, double val)
{
	if (ss->count == 0 || val < ss->min)
		ss->min = val;
	if (ss->count == 0 || val > ss->max)
		ss->max = val;
	ss->count++;
	ss->sum += val;
	ss->sum2 += val * val;
}

/*
 * Merge two SimpleStats objects
 */
void
mergeSimpleStats(SimpleStats *acc, const SimpleStats *ss)
{
	if (acc->count == 0 || ss->min < acc->min)
		acc->min = ss->min;
	if (acc->count == 0 || ss->max > acc->max)
		acc->max = ss->max;
	acc->count += ss->count;
	acc->sum += ss->sum;
	acc->sum2 += ss->sum2;
}

/*
 * Print average and stddev from a SimpleStats struct.
 */
void
printSimpleStats(const char *prefix, const SimpleStats *ss)
{
	if (ss->count > 0)
	{
		double		latency = ss->sum / ss->count;
		double		stddev = sqrt(ss->sum2 / ss->count - latency * latency);

		printf("%s average = %.3f ms\n", prefix, 0.001 * latency);
		printf("%s stddev = %.3f ms\n", prefix, 0.001 * stddev);
	}
}

/*
 * Initialize a StatsData struct to mostly zeroes, with its start time set to
 * the given value.
 */
void
initStats(StatsData *sd, pg_time_usec_t start)
{
	sd->start_time = start;
	sd->cnt = 0;
	sd->cnt_skipped = 0;
	sd->retries = 0;
	sd->retried = 0;
	sd->serialization_failures = 0;
	sd->deadlock_failures = 0;
	sd->other_sql_failures = 0;
	initSimpleStats(&sd->latency);
	initSimpleStats(&sd->lag);
}

/*
 * Accumulate one additional item into the given stats object.
 */
void
accumStats(StatsData *stats, bool tx_skipped, double lat, double lag,
		   EStatus estatus, int64 tries, bool has_throttle_delay)
{
	/* Record the skipped transaction */
	if (tx_skipped)
	{
		/* no latency to record on skipped transactions */
		stats->cnt_skipped++;
		return;
	}

	/*
	 * Record the number of retries regardless of whether the transaction was
	 * successful or failed.
	 */
	if (tries > 1)
	{
		stats->retries += (tries - 1);
		stats->retried++;
	}

	switch (estatus)
	{
			/* Record the successful transaction */
		case ESTATUS_NO_ERROR:
			stats->cnt++;

			addToSimpleStats(&stats->latency, lat);

			/* and possibly the same for schedule lag */
			if (has_throttle_delay)
				addToSimpleStats(&stats->lag, lag);
			break;

			/* Record the failed transaction */
		case ESTATUS_SERIALIZATION_ERROR:
			stats->serialization_failures++;
			break;
		case ESTATUS_DEADLOCK_ERROR:
			stats->deadlock_failures++;
			break;
		case ESTATUS_OTHER_SQL_ERROR:
			stats->other_sql_failures++;
			break;
		default:
			/* internal error which should never occur */
			pg_fatal("unexpected error status: %d", estatus);
	}
}

/*
 * Merge statistics from src into acc.
 */
void
mergeStats(StatsData *acc, const StatsData *src)
{
	mergeSimpleStats(&acc->latency, &src->latency);
	mergeSimpleStats(&acc->lag, &src->lag);
	acc->cnt += src->cnt;
	acc->cnt_skipped += src->cnt_skipped;
	acc->retries += src->retries;
	acc->retried += src->retried;
	acc->serialization_failures += src->serialization_failures;
	acc->deadlock_failures += src->deadlock_failures;
	acc->other_sql_failures += src->other_sql_failures;
}

/*
 * Return the number of failed transactions.
 */
int64
getFailures(const StatsData *stats)
{
	return (stats->serialization_failures +
			stats->deadlock_failures +
			stats->other_sql_failures);
}

/*
 * Return a string constant representing the result of a transaction
 * that is not successfully processed.
 */
const char *
getResultString(bool tx_skipped, EStatus estatus, bool failures_detailed)
{
	if (tx_skipped)
		return "skipped";
	else if (failures_detailed)
	{
	switch (estatus)
	{
		case ESTATUS_SERIALIZATION_ERROR:
			return "serialization";
		case ESTATUS_DEADLOCK_ERROR:
			return "deadlock";
		case ESTATUS_OTHER_SQL_ERROR:
			return "other";
		default:
			/* internal error which should never occur */
			pg_fatal("unexpected error status: %d", estatus);
	}
	}
	else
		return "failed";
}
