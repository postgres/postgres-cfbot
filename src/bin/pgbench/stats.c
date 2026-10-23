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
	memset(sd->recall_dist10, 0, sizeof(sd->recall_dist10));
	sd->q1_recall_count = 0;
	sd->q1_recall_sum = 0.0;
	sd->q4_recall_count = 0;
	sd->q4_recall_head = 0;
	memset(sd->q4_recall_ring, 0, sizeof(sd->q4_recall_ring));
	sd->num_named_recalls = 0;
	memset(sd->named_recalls, 0, sizeof(sd->named_recalls));
	sd->validation_passed = 0;
	sd->validation_failed = 0;
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
	for (int d = 0; d < RECALL_DIST10_BUCKETS; d++)
		acc->recall_dist10[d] += src->recall_dist10[d];
	acc->q1_recall_sum += src->q1_recall_sum;
	acc->q1_recall_count += src->q1_recall_count;
	for (int k = 0; k < src->q4_recall_count; k++)
	{
		acc->q4_recall_ring[acc->q4_recall_head] = src->q4_recall_ring[k];
		acc->q4_recall_head = (acc->q4_recall_head + 1) % AGING_WINDOW_SAMPLES;
		if (acc->q4_recall_count < AGING_WINDOW_SAMPLES)
			acc->q4_recall_count++;
	}
	for (int i = 0; i < src->num_named_recalls; i++)
	{
		const NamedRecallStats *s = &src->named_recalls[i];
		NamedRecallStats *dst = NULL;

		for (int j = 0; j < acc->num_named_recalls; j++)
		{
			if (strcmp(acc->named_recalls[j].name, s->name) == 0)
			{
				dst = &acc->named_recalls[j];
				break;
			}
		}
		if (dst == NULL && acc->num_named_recalls < MAX_NAMED_RECALL_METRICS)
		{
			dst = &acc->named_recalls[acc->num_named_recalls++];
			memset(dst, 0, sizeof(*dst));
			strlcpy(dst->name, s->name, sizeof(dst->name));
			initSimpleStats(&dst->stats);
		}
		if (dst != NULL)
		{
			mergeSimpleStats(&dst->stats, &s->stats);
			dst->q1_sum += s->q1_sum;
			dst->q1_count += s->q1_count;
			for (int k = 0; k < s->q4_count; k++)
			{
				dst->q4_ring[dst->q4_head] = s->q4_ring[k];
				dst->q4_head = (dst->q4_head + 1) % AGING_WINDOW_SAMPLES;
				if (dst->q4_count < AGING_WINDOW_SAMPLES)
					dst->q4_count++;
			}
			for (int b = 0; b < RECALL_HIST_BUCKETS; b++)
				dst->buckets[b] += s->buckets[b];
			for (int d = 0; d < RECALL_DIST10_BUCKETS; d++)
				dst->dist10[d] += s->dist10[d];
		}
	}
	acc->validation_passed += src->validation_passed;
	acc->validation_failed += src->validation_failed;
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
	int			d;

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

	d = (int) (clamped * 10.0);
	if (d >= RECALL_DIST10_BUCKETS)
		d = RECALL_DIST10_BUCKETS - 1;
	if (d < 0)
		d = 0;
	sd->recall_dist10[d]++;

	if (sd->q1_recall_count < AGING_WINDOW_SAMPLES)
	{
		sd->q1_recall_sum += clamped;
		sd->q1_recall_count++;
	}
	sd->q4_recall_ring[sd->q4_recall_head] = clamped;
	sd->q4_recall_head = (sd->q4_recall_head + 1) % AGING_WINDOW_SAMPLES;
	if (sd->q4_recall_count < AGING_WINDOW_SAMPLES)
		sd->q4_recall_count++;
}

void
addToNamedRecallStats(StatsData *sd, const char *name, double recall_val)
{
	NamedRecallStats *slot = NULL;
	double		clamped = recall_val;
	int			b;
	int			d;

	if (name == NULL || name[0] == '\0')
	{
		addToRecallStats(sd, recall_val);
		return;
	}

	if (clamped < 0.0)
		clamped = 0.0;
	if (clamped > 1.0)
		clamped = 1.0;

	for (int i = 0; i < sd->num_named_recalls; i++)
	{
		if (strcmp(sd->named_recalls[i].name, name) == 0)
		{
			slot = &sd->named_recalls[i];
			break;
		}
	}

	if (slot == NULL && sd->num_named_recalls < MAX_NAMED_RECALL_METRICS)
	{
		slot = &sd->named_recalls[sd->num_named_recalls++];
		memset(slot, 0, sizeof(*slot));
		strlcpy(slot->name, name, sizeof(slot->name));
		initSimpleStats(&slot->stats);
	}

	if (slot != NULL)
	{
		addToSimpleStats(&slot->stats, clamped);
		b = (int) floor(clamped * 100.0 + 0.5);
		if (b < 0)
			b = 0;
		if (b >= RECALL_HIST_BUCKETS)
			b = RECALL_HIST_BUCKETS - 1;
		slot->buckets[b]++;

		d = (int) (clamped * 10.0);
		if (d >= RECALL_DIST10_BUCKETS)
			d = RECALL_DIST10_BUCKETS - 1;
		if (d < 0)
			d = 0;
		slot->dist10[d]++;

		if (slot->q1_count < AGING_WINDOW_SAMPLES)
		{
			slot->q1_sum += clamped;
			slot->q1_count++;
		}
		slot->q4_ring[slot->q4_head] = clamped;
		slot->q4_head = (slot->q4_head + 1) % AGING_WINDOW_SAMPLES;
		if (slot->q4_count < AGING_WINDOW_SAMPLES)
			slot->q4_count++;
	}
}

const NamedRecallStats *
findNamedRecallStats(const StatsData *stats, const char *name)
{
	if (stats == NULL || name == NULL || name[0] == '\0')
		return NULL;
	for (int i = 0; i < stats->num_named_recalls; i++)
	{
		if (pg_strcasecmp(stats->named_recalls[i].name, name) == 0)
			return &stats->named_recalls[i];
	}
	return NULL;
}

double
getNamedRecallPercentile(const StatsData *stats, const char *name, double pct)
{
	const NamedRecallStats *slot = findNamedRecallStats(stats, name);
	int64		target;
	int64		cum = 0;

	if (slot == NULL || slot->stats.count <= 0)
		return 0.0;

	if (pct < 0.0)
		pct = 0.0;
	if (pct > 100.0)
		pct = 100.0;

	target = (int64) ceil((pct / 100.0) * (double) slot->stats.count);
	if (target < 1)
		target = 1;
	if (target > slot->stats.count)
		target = slot->stats.count;

	for (int b = 0; b < RECALL_HIST_BUCKETS; b++)
	{
		if (slot->buckets[b] <= 0)
			continue;
		cum += slot->buckets[b];
		if (cum >= target)
			return (double) b / 100.0;
	}

	return slot->stats.max;
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

static void
print10BucketDist(const char *prefix, const int64 dist10[RECALL_DIST10_BUCKETS])
{
	bool		has_dist = false;

	for (int i = 0; i < RECALL_DIST10_BUCKETS; i++)
	{
		if (dist10[i] > 0)
		{
			if (!has_dist)
			{
				printf("%s distribution:", prefix);
				has_dist = true;
			}
			if (i == RECALL_DIST10_BUCKETS - 1)
				printf(" [0.%d-1.0]: " INT64_FORMAT, i, dist10[i]);
			else
				printf(" [0.%d-0.%d): " INT64_FORMAT, i, i + 1, dist10[i]);
		}
	}
	if (has_dist)
		printf("\n");
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
		print10BucketDist(prefix, stats->recall_dist10);
		if (stats->recall.count >= 4 && stats->q1_recall_count > 0 && stats->q4_recall_count > 0)
		{
			double		q1_avg = stats->q1_recall_sum / stats->q1_recall_count;
			double		q4_sum = 0.0;
			double		q4_avg;
			double		delta;
			double		deg_pct;

			for (int j = 0; j < stats->q4_recall_count; j++)
				q4_sum += stats->q4_recall_ring[j];
			q4_avg = q4_sum / stats->q4_recall_count;
			delta = q4_avg - q1_avg;
			deg_pct = (q1_avg > 0.000001) ? (delta / q1_avg * 100.0) : 0.0;
			printf("%s aging degradation: initial = %.4f, mature = %.4f, delta = %+.4f (%+.2f%%)\n",
				   prefix, q1_avg, q4_avg, delta, deg_pct);
		}
	}

	for (int i = 0; i < stats->num_named_recalls; i++)
	{
		const NamedRecallStats *s = &stats->named_recalls[i];

		if (s->stats.count > 0)
		{
			char		named_prefix[64];
			double		avg = s->stats.sum / s->stats.count;
			double		var = (s->stats.sum2 / s->stats.count) - (avg * avg);
			double		stddev = (var > 0.0) ? sqrt(var) : 0.0;

			snprintf(named_prefix, sizeof(named_prefix), "%s[%s]", prefix, s->name);
			printf("%s average = %.4f (stddev = %.4f, min = %.4f, max = %.4f, samples = " INT64_FORMAT ")\n",
				   named_prefix, avg, stddev, s->stats.min, s->stats.max,
				   s->stats.count);
			printf("%s percentiles: p5 = %.4f, p50 = %.4f, p95 = %.4f\n",
				   named_prefix,
				   getNamedRecallPercentile(stats, s->name, 5.0),
				   getNamedRecallPercentile(stats, s->name, 50.0),
				   getNamedRecallPercentile(stats, s->name, 95.0));
			print10BucketDist(named_prefix, s->dist10);
			if (s->stats.count >= 4 && s->q1_count > 0 && s->q4_count > 0)
			{
				double		q1_avg = s->q1_sum / s->q1_count;
				double		q4_sum = 0.0;
				double		q4_avg;
				double		delta;
				double		deg_pct;

				for (int j = 0; j < s->q4_count; j++)
					q4_sum += s->q4_ring[j];
				q4_avg = q4_sum / s->q4_count;
				delta = q4_avg - q1_avg;
				deg_pct = (q1_avg > 0.000001) ? (delta / q1_avg * 100.0) : 0.0;
				printf("%s aging degradation: initial = %.4f, mature = %.4f, delta = %+.4f (%+.2f%%)\n",
					   named_prefix, q1_avg, q4_avg, delta, deg_pct);
			}
		}
	}
}

void
printValidationStats(const char *prefix, const StatsData *stats)
{
	int64		total_val = stats->validation_passed + stats->validation_failed;

	if (total_val > 0)
	{
		printf("%s: passed = " INT64_FORMAT ", failed = " INT64_FORMAT " (%.2f%%)\n",
			   prefix,
			   stats->validation_passed,
			   stats->validation_failed,
			   100.0 * (double) stats->validation_passed / (double) total_val);
	}
}
