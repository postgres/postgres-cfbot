/*-------------------------------------------------------------------------
 *
 * stats.c
 *	  Statistics functions for pgbench
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
#include "port/pg_bitutils.h"
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
 * Record a latency sample (in microseconds) into the HDR histogram of StatsData.
 */
static void
addToLatencyHist(StatsData *sd, double lat_usec)
{
	uint64		u = (lat_usec > 0.0) ? (uint64) llround(lat_usec) : 0;
	int			idx;

	if (u < HDR_SUB_COUNT)
		idx = (int) u;
	else
	{
		int			msb = pg_leftmost_one_pos64(u);
		int			octave = msb - HDR_SUB_BITS;

		if (octave > HDR_MAX_OCTAVE)
			idx = HDR_TOTAL_BUCKETS - 1;
		else
		{
			int			sub = (int) ((u >> octave) & (HDR_SUB_COUNT - 1));

			idx = ((octave + 1) << HDR_SUB_BITS) | sub;
		}
	}

	sd->hist_buckets[idx]++;
	sd->hist_count++;
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
	sd->expected_rollbacks = 0;
	initSimpleStats(&sd->latency);
	initSimpleStats(&sd->lag);
	initSimpleStats(&sd->keying_time);
	initSimpleStats(&sd->think_time);
	initSimpleStats(&sd->recall);
	sd->hist_count = 0;
	memset(sd->hist_buckets, 0, sizeof(sd->hist_buckets));
	sd->lag_hist_count = 0;
	memset(sd->lag_hist_buckets, 0, sizeof(sd->lag_hist_buckets));
	sd->scheduled_count = 0;
	sd->ontime_count = 0;
	memset(sd->recall_buckets, 0, sizeof(sd->recall_buckets));
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
			addToLatencyHist(stats, lat);

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
	mergeSimpleStats(&acc->keying_time, &src->keying_time);
	mergeSimpleStats(&acc->think_time, &src->think_time);
	mergeSimpleStats(&acc->recall, &src->recall);
	acc->cnt += src->cnt;
	acc->cnt_skipped += src->cnt_skipped;
	acc->retries += src->retries;
	acc->retried += src->retried;
	acc->serialization_failures += src->serialization_failures;
	acc->deadlock_failures += src->deadlock_failures;
	acc->other_sql_failures += src->other_sql_failures;
	acc->expected_rollbacks += src->expected_rollbacks;
	acc->hist_count += src->hist_count;
	for (int b = 0; b < HDR_TOTAL_BUCKETS; b++)
		acc->hist_buckets[b] += src->hist_buckets[b];
	acc->lag_hist_count += src->lag_hist_count;
	for (int b = 0; b < HDR_TOTAL_BUCKETS; b++)
		acc->lag_hist_buckets[b] += src->lag_hist_buckets[b];
	acc->scheduled_count += src->scheduled_count;
	acc->ontime_count += src->ontime_count;
	for (int b = 0; b < RECALL_HIST_BUCKETS; b++)
		acc->recall_buckets[b] += src->recall_buckets[b];
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

static double
getHdrHistPercentile(const int64 *buckets, int64 count, double pct, double fallback_max)
{
	int64		target;
	int64		cum = 0;

	if (count <= 0)
		return 0.0;

	if (pct < 0.0)
		pct = 0.0;
	if (pct > 100.0)
		pct = 100.0;

	target = (int64) ceil((pct / 100.0) * (double) count);
	if (target < 1)
		target = 1;
	if (target > count)
		target = count;

	for (int b = 0; b < HDR_TOTAL_BUCKETS; b++)
	{
		if (buckets[b] <= 0)
			continue;
		cum += buckets[b];
		if (cum >= target)
		{
			if (b < HDR_SUB_COUNT)
				return (double) b;
			else
			{
				int			octave = (b >> HDR_SUB_BITS) - 1;
				int			sub = b & (HDR_SUB_COUNT - 1);
				uint64		low = ((UINT64CONST(1) << HDR_SUB_BITS) | (uint64) sub) << octave;
				uint64		width = UINT64CONST(1) << octave;

				if (width <= 1)
					return (double) low;
				else
				{
					int64		prev_cum = cum - buckets[b];
					double		frac = (double) (target - prev_cum) / (double) buckets[b];

					return (double) low + frac * (double) width;
				}
			}
		}
	}

	return fallback_max;
}

/*
 * Compute the given latency percentile (0.0 .. 100.0) in microseconds from
 * the HDR histogram in StatsData.
 */
double
getLatencyPercentile(const StatsData *stats, double pct)
{
	return getHdrHistPercentile(stats->hist_buckets, stats->hist_count,
								pct, stats->latency.max);
}

void
printLatencyPercentiles(const char *prefix, const StatsData *stats)
{
	if (stats->hist_count > 0)
	{
		printf("%s: p50 = %.3f ms, p90 = %.3f ms, p95 = %.3f ms, p99 = %.3f ms, p99.9 = %.3f ms\n",
			   prefix,
			   0.001 * getLatencyPercentile(stats, 50.0),
			   0.001 * getLatencyPercentile(stats, 90.0),
			   0.001 * getLatencyPercentile(stats, 95.0),
			   0.001 * getLatencyPercentile(stats, 99.0),
			   0.001 * getLatencyPercentile(stats, 99.9));
	}
}

/*
 * Record a schedule lag / replay drift sample (in microseconds) into the lag
 * HDR histogram and on-time counter of StatsData.
 *
 * Per Pitfall #8, negative lag values (when a client begins slightly ahead of
 * its scheduled timestamp) are explicitly clamped to 0 BEFORE casting to uint64
 * and calling pg_leftmost_one_pos64(), preventing signed-to-unsigned wraparound
 * to UINT64_MAX.
 */
void
addToLagStats(StatsData *sd, double lag_usec, double ontime_thresh_usec)
{
	uint64		u = (lag_usec > 0.0) ? (uint64) llround(lag_usec) : 0;
	int			idx;

	if (u < HDR_SUB_COUNT)
		idx = (int) u;
	else
	{
		int			msb = pg_leftmost_one_pos64(u);
		int			octave = msb - HDR_SUB_BITS;

		if (octave > HDR_MAX_OCTAVE)
			idx = HDR_TOTAL_BUCKETS - 1;
		else
		{
			int			sub = (int) ((u >> octave) & (HDR_SUB_COUNT - 1));

			idx = ((octave + 1) << HDR_SUB_BITS) | sub;
		}
	}

	sd->lag_hist_buckets[idx]++;
	sd->lag_hist_count++;
	sd->scheduled_count++;
	if (lag_usec <= ontime_thresh_usec)
		sd->ontime_count++;
}

double
getLagPercentile(const StatsData *stats, double pct)
{
	return getHdrHistPercentile(stats->lag_hist_buckets, stats->lag_hist_count,
								pct, stats->lag.max > 0.0 ? stats->lag.max : 0.0);
}

void
printLagPercentiles(const char *prefix, const StatsData *stats)
{
	if (stats->lag_hist_count > 0)
	{
		printf("%s: p50 = %.3f ms, p90 = %.3f ms, p95 = %.3f ms, p99 = %.3f ms, p99.9 = %.3f ms\n",
			   prefix,
			   0.001 * getLagPercentile(stats, 50.0),
			   0.001 * getLagPercentile(stats, 90.0),
			   0.001 * getLagPercentile(stats, 95.0),
			   0.001 * getLagPercentile(stats, 99.0),
			   0.001 * getLagPercentile(stats, 99.9));
	}
}

void
addToRecallStats(StatsData *sd, double recall_val)
{
	double		clamped = recall_val;
	int			b;

	if (clamped < 0.0)
		clamped = 0.0;
	if (clamped > 1.0)
		clamped = 1.0;

	addToSimpleStats(&sd->recall, clamped);
	b = (int) floor(clamped * 100.0 + 0.5);
	if (b < 0)
		b = 0;
	if (b >= RECALL_HIST_BUCKETS)
		b = RECALL_HIST_BUCKETS - 1;
	sd->recall_buckets[b]++;
}

double
getRecallPercentile(const StatsData *stats, double pct)
{
	int64		target;
	int64		cum = 0;

	if (stats->recall.count <= 0)
		return 0.0;

	if (pct < 0.0)
		pct = 0.0;
	if (pct > 100.0)
		pct = 100.0;

	target = (int64) ceil((pct / 100.0) * (double) stats->recall.count);
	if (target < 1)
		target = 1;
	if (target > stats->recall.count)
		target = stats->recall.count;

	for (int b = 0; b < RECALL_HIST_BUCKETS; b++)
	{
		if (stats->recall_buckets[b] <= 0)
			continue;
		cum += stats->recall_buckets[b];
		if (cum >= target)
			return (double) b / 100.0;
	}

	return stats->recall.max;
}

void
printRecallStats(const char *prefix, const StatsData *stats)
{
	if (stats->recall.count > 0)
	{
		double		avg = stats->recall.sum / stats->recall.count;
		double		var = (stats->recall.sum2 / stats->recall.count) - (avg * avg);
		double		stddev = (var > 0.0) ? sqrt(var) : 0.0;

		printf("%s average = %.4f (stddev = %.4f, min = %.4f, max = %.4f, samples = " INT64_FORMAT ")\n",
			   prefix, avg, stddev, stats->recall.min, stats->recall.max,
			   stats->recall.count);
		printf("%s percentiles: p5 = %.4f, p50 = %.4f, p95 = %.4f\n",
			   prefix,
			   getRecallPercentile(stats, 5.0),
			   getRecallPercentile(stats, 50.0),
			   getRecallPercentile(stats, 95.0));
	}
}
