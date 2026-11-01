/*-------------------------------------------------------------------------
 *
 * report.c
 *	  Transaction logging, progress, and result reporting for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/report.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <math.h>

#include "common/pg_prng.h"
#include "engine.h"
#include "fe_utils/string_utils.h"
#include "init.h"
#include "pgbench.h"
#include "poller.h"
#include "report.h"
#include "script.h"
#include "stats.h"

/*
 * Print log entry after completing one transaction.
 *
 * We print Unix-epoch timestamps in the log, so that entries can be
 * correlated against other logs.
 *
 * XXX We could obtain the time from the caller and just shift it here, to
 * avoid the cost of an extra call to pg_time_now().
 */
void
doLog(TState *thread, CState *st,
	  StatsData *agg, bool tx_skipped, double latency, double lag)
{
	FILE	   *logfile = thread->logfile;
	pg_time_usec_t now = pg_time_now() + epoch_shift;

	Assert(use_log);

	/*
	 * Skip the log entry if sampling is enabled and this row doesn't belong
	 * to the random sample.
	 */
	if (sample_rate != 0.0 &&
		pg_prng_double(&thread->ts_sample_rs) > sample_rate)
		return;

	/* should we aggregate the results or not? */
	if (agg_interval > 0)
	{
		pg_time_usec_t next;

		/*
		 * Loop until we reach the interval of the current moment, and print
		 * any empty intervals in between (this may happen with very low tps,
		 * e.g. --rate=0.1).
		 */

		while ((next = agg->start_time + agg_interval * INT64CONST(1000000)) <= now)
		{
			double		lag_sum = 0.0;
			double		lag_sum2 = 0.0;
			double		lag_min = 0.0;
			double		lag_max = 0.0;
			int64		cnt_skipped = 0;
			int64		serialization_failures = 0;
			int64		deadlock_failures = 0;
			int64		other_sql_failures = 0;
			int64		retried = 0;
			int64		retries = 0;

			/* print aggregated report to logfile */
			fprintf(logfile, INT64_FORMAT " " INT64_FORMAT " %.0f %.0f %.0f %.0f",
					agg->start_time / 1000000,	/* seconds since Unix epoch */
					agg->cnt,
					agg->latency.sum,
					agg->latency.sum2,
					agg->latency.min,
					agg->latency.max);

			if (throttle_delay)
			{
				lag_sum = agg->lag.sum;
				lag_sum2 = agg->lag.sum2;
				lag_min = agg->lag.min;
				lag_max = agg->lag.max;
			}
			fprintf(logfile, " %.0f %.0f %.0f %.0f",
					lag_sum,
					lag_sum2,
					lag_min,
					lag_max);

			if (latency_limit)
				cnt_skipped = agg->cnt_skipped;
			fprintf(logfile, " " INT64_FORMAT, cnt_skipped);

			if (max_tries != 1)
			{
				retried = agg->retried;
				retries = agg->retries;
			}
			fprintf(logfile, " " INT64_FORMAT " " INT64_FORMAT, retried, retries);

			if (failures_detailed)
			{
				serialization_failures = agg->serialization_failures;
				deadlock_failures = agg->deadlock_failures;
				other_sql_failures = agg->other_sql_failures;
			}
			fprintf(logfile, " " INT64_FORMAT " " INT64_FORMAT " " INT64_FORMAT,
					serialization_failures,
					deadlock_failures,
					other_sql_failures);

			fputc('\n', logfile);

			/* reset data and move to next interval */
			initStats(agg, next);
		}

		/* accumulate the current transaction */
		accumStats(agg, tx_skipped, latency, lag, st->estatus, st->tries);
	}
	else
	{
		/* no, print raw transactions */
		if (!tx_skipped && st->estatus == ESTATUS_NO_ERROR)
			fprintf(logfile, "%d " INT64_FORMAT " %.0f %d " INT64_FORMAT " "
					INT64_FORMAT,
					st->id, st->cnt, latency, st->use_file,
					now / 1000000, now % 1000000);
		else
			fprintf(logfile, "%d " INT64_FORMAT " %s %d " INT64_FORMAT " "
					INT64_FORMAT,
					st->id, st->cnt, getResultString(tx_skipped, st->estatus),
					st->use_file, now / 1000000, now % 1000000);

		if (throttle_delay)
			fprintf(logfile, " %.0f", lag);
		if (max_tries != 1)
			fprintf(logfile, " %u", st->tries - 1);
		fputc('\n', logfile);
	}
}

/*
 * Accumulate and report statistics at end of a transaction.
 *
 * (This is also called when a transaction is late and thus skipped.
 * Note that even skipped and failed transactions are counted in the CState
 * "cnt" field.)
 */
void
processXactStats(TState *thread, CState *st, pg_time_usec_t *now,
				 bool tx_skipped, StatsData *agg)
{
	double		latency = 0.0,
				lag = 0.0;
	bool		detailed = progress || throttle_delay || latency_limit ||
		use_log || per_script_stats;

	if (detailed && !tx_skipped && st->estatus == ESTATUS_NO_ERROR)
	{
		pg_time_now_lazy(now);

		/* compute latency & lag */
		latency = (*now) - st->txn_scheduled;
		lag = st->txn_begin - st->txn_scheduled;
	}

	/* keep detailed thread stats */
	accumStats(&thread->stats, tx_skipped, latency, lag, st->estatus, st->tries);

	/* count transactions over the latency limit, if needed */
	if (latency_limit && latency > latency_limit)
		thread->latency_late++;

	/* client stat is just counting */
	st->cnt++;

	if (use_log)
		doLog(thread, st, agg, tx_skipped, latency, lag);

	/* XXX could use a mutex here, but we choose not to */
	if (per_script_stats)
		accumStats(&sql_script[st->use_file].stats, tx_skipped, latency, lag,
				   st->estatus, st->tries);
}

/*
 * Print progress report.
 *
 * On entry, *last and *last_report contain the statistics and time of last
 * progress report.  On exit, they are updated with the new stats.
 */
void
printProgressReport(TState *threads, int64 test_start, pg_time_usec_t now,
					StatsData *last, int64 *last_report)
{
	/* generate and show report */
	pg_time_usec_t run = now - *last_report;
	int64		cnt,
				failures,
				retried;
	double		tps,
				total_run,
				latency,
				sqlat,
				lag,
				stdev;
	char		tbuf[315];
	StatsData	cur;

	/*
	 * Add up the statistics of all threads.
	 *
	 * XXX: No locking.  There is no guarantee that we get an atomic snapshot
	 * of the transaction count and latencies, so these figures can well be
	 * off by a small amount.  The progress report's purpose is to give a
	 * quick overview of how the test is going, so that shouldn't matter too
	 * much.  (If a read from a 64-bit integer is not atomic, you might get a
	 * "torn" read and completely bogus latencies though!)
	 */
	initStats(&cur, 0);
	for (int i = 0; i < nthreads; i++)
	{
		mergeSimpleStats(&cur.latency, &threads[i].stats.latency);
		mergeSimpleStats(&cur.lag, &threads[i].stats.lag);
		cur.cnt += threads[i].stats.cnt;
		cur.cnt_skipped += threads[i].stats.cnt_skipped;
		cur.retries += threads[i].stats.retries;
		cur.retried += threads[i].stats.retried;
		cur.serialization_failures +=
			threads[i].stats.serialization_failures;
		cur.deadlock_failures += threads[i].stats.deadlock_failures;
		cur.other_sql_failures += threads[i].stats.other_sql_failures;
	}

	/* we count only actually executed transactions */
	cnt = cur.cnt - last->cnt;
	total_run = (now - test_start) / 1000000.0;
	tps = 1000000.0 * cnt / run;
	if (cnt > 0)
	{
		latency = 0.001 * (cur.latency.sum - last->latency.sum) / cnt;
		sqlat = 1.0 * (cur.latency.sum2 - last->latency.sum2) / cnt;
		stdev = 0.001 * sqrt(sqlat - 1000000.0 * latency * latency);
		lag = 0.001 * (cur.lag.sum - last->lag.sum) / cnt;
	}
	else
	{
		latency = sqlat = stdev = lag = 0;
	}
	failures = getFailures(&cur) - getFailures(last);
	retried = cur.retried - last->retried;

	if (progress_timestamp)
	{
		snprintf(tbuf, sizeof(tbuf), "%.3f s",
				 PG_TIME_GET_DOUBLE(now + epoch_shift));
	}
	else
	{
		/* round seconds are expected, but the thread may be late */
		snprintf(tbuf, sizeof(tbuf), "%.1f s", total_run);
	}

	fprintf(stderr,
			"progress: %s, %.1f tps, lat %.3f ms stddev %.3f, " INT64_FORMAT " failed",
			tbuf, tps, latency, stdev, failures);

	if (throttle_delay)
	{
		fprintf(stderr, ", lag %.3f ms", lag);
		if (latency_limit)
			fprintf(stderr, ", " INT64_FORMAT " skipped",
					cur.cnt_skipped - last->cnt_skipped);
	}

	/* it can be non-zero only if max_tries is not equal to one */
	if (max_tries != 1)
		fprintf(stderr,
				", " INT64_FORMAT " retried, " INT64_FORMAT " retries",
				retried, cur.retries - last->retries);
	fprintf(stderr, "\n");

	*last = cur;
	*last_report = now;
}

/* print version banner */
void
printVersion(PGconn *con)
{
	int			server_ver = PQserverVersion(con);
	int			client_ver = PG_VERSION_NUM;

	if (server_ver != client_ver)
	{
		const char *server_version;
		char		sverbuf[32];

		/* Try to get full text form, might include "devel" etc */
		server_version = PQparameterStatus(con, "server_version");
		/* Otherwise fall back on server_ver */
		if (!server_version)
		{
			formatPGVersionNumber(server_ver, true,
								  sverbuf, sizeof(sverbuf));
			server_version = sverbuf;
		}

		printf(_("%s (%s, server %s)\n"),
			   "pgbench", PG_VERSION, server_version);
	}
	/* For version match, only print pgbench version */
	else
		printf("%s (%s)\n", "pgbench", PG_VERSION);
	fflush(stdout);
}

/* print out results */
void
printResults(StatsData *total,
			 pg_time_usec_t total_duration, /* benchmarking time */
			 pg_time_usec_t conn_total_duration,	/* is_connect */
			 pg_time_usec_t conn_elapsed_duration,	/* !is_connect */
			 int64 latency_late)
{
	/* tps is about actually executed transactions during benchmarking */
	int64		failures = getFailures(total);
	int64		total_cnt = total->cnt + total->cnt_skipped + failures;
	double		bench_duration = PG_TIME_GET_DOUBLE(total_duration);
	double		tps = total->cnt / bench_duration;

	/* Report test parameters. */
	printf("transaction type: %s\n",
		   num_scripts == 1 ? sql_script[0].desc : "multiple scripts");
	printf("scaling factor: %d\n", scale);
	/* only print partitioning information if some partitioning was detected */
	if (partition_method != PART_NONE)
		printf("partition method: %s\npartitions: %d\n",
			   PARTITION_METHOD[partition_method], partitions);
	printf("query mode: %s\n", QUERYMODE[querymode]);
	printf("number of clients: %d\n", nclients);
	printf("number of threads: %d\n", nthreads);

	if (max_tries)
		printf("maximum number of tries: %u\n", max_tries);

	if (duration <= 0)
	{
		printf("number of transactions per client: %d\n", nxacts);
		printf("number of transactions actually processed: " INT64_FORMAT "/%d\n",
			   total->cnt, nxacts * nclients);
	}
	else
	{
		printf("duration: %d s\n", duration);
		printf("number of transactions actually processed: " INT64_FORMAT "\n",
			   total->cnt);
	}

	/*
	 * Remaining stats are nonsensical if we failed to execute any xacts due
	 * to other than serialization or deadlock errors and --continue-on-error
	 * is not set.
	 */
	if (total_cnt <= 0)
		return;

	printf("number of failed transactions: " INT64_FORMAT " (%.3f%%)\n",
		   failures, 100.0 * failures / total_cnt);

	if (failures_detailed)
	{
		printf("number of serialization failures: " INT64_FORMAT " (%.3f%%)\n",
			   total->serialization_failures,
			   100.0 * total->serialization_failures / total_cnt);
		printf("number of deadlock failures: " INT64_FORMAT " (%.3f%%)\n",
			   total->deadlock_failures,
			   100.0 * total->deadlock_failures / total_cnt);
		printf("number of other failures: " INT64_FORMAT " (%.3f%%)\n",
			   total->other_sql_failures,
			   100.0 * total->other_sql_failures / total_cnt);
	}

	/* it can be non-zero only if max_tries is not equal to one */
	if (max_tries != 1)
	{
		printf("number of transactions retried: " INT64_FORMAT " (%.3f%%)\n",
			   total->retried, 100.0 * total->retried / total_cnt);
		printf("total number of retries: " INT64_FORMAT "\n", total->retries);
	}

	if (throttle_delay && latency_limit)
		printf("number of transactions skipped: " INT64_FORMAT " (%.3f%%)\n",
			   total->cnt_skipped, 100.0 * total->cnt_skipped / total_cnt);

	if (latency_limit)
		printf("number of transactions above the %.1f ms latency limit: " INT64_FORMAT "/" INT64_FORMAT " (%.3f%%)\n",
			   latency_limit / 1000.0, latency_late, total->cnt,
			   (total->cnt > 0) ? 100.0 * latency_late / total->cnt : 0.0);

	if (throttle_delay || progress || latency_limit)
		printSimpleStats("latency", &total->latency);
	else
	{
		/* no measurement, show average latency computed from run time */
		printf("latency average = %.3f ms%s\n",
			   0.001 * total_duration * nclients / total_cnt,
			   failures > 0 ? " (including failures)" : "");
	}

	if (throttle_delay)
	{
		/*
		 * Report average transaction lag under rate limit throttling.  This
		 * is the delay between scheduled and actual start times for the
		 * transaction.  The measured lag may be caused by thread/client load,
		 * the database load, or the Poisson throttling process.
		 */
		printf("rate limit schedule lag: avg %.3f (max %.3f) ms\n",
			   0.001 * total->lag.sum / total->cnt, 0.001 * total->lag.max);
	}

	/*
	 * Under -C/--connect, each transaction incurs a significant connection
	 * cost, it would not make much sense to ignore it in tps, and it would
	 * not be tps anyway.
	 *
	 * Otherwise connections are made just once at the beginning of the run
	 * and should not impact performance but for very short run, so they are
	 * (right)fully ignored in tps.
	 */
	if (is_connect)
	{
		printf("average connection time = %.3f ms\n", 0.001 * conn_total_duration / (total->cnt + failures));
		printf("tps = %f (including reconnection times)\n", tps);
	}
	else
	{
		printf("initial connection time = %.3f ms\n", 0.001 * conn_elapsed_duration);
		printf("tps = %f (without initial connection time)\n", tps);
	}

	/* Report per-script/command statistics */
	if (per_script_stats || report_per_command)
	{
		int			i;

		for (i = 0; i < num_scripts; i++)
		{
			if (per_script_stats)
			{
				StatsData  *sstats = &sql_script[i].stats;
				int64		script_failures = getFailures(sstats);
				int64		script_total_cnt =
					sstats->cnt + sstats->cnt_skipped + script_failures;

				printf("SQL script %d: %s\n"
					   " - weight: %d (targets %.1f%% of total)\n"
					   " - " INT64_FORMAT " transactions (%.1f%% of total)\n",
					   i + 1, sql_script[i].desc,
					   sql_script[i].weight,
					   100.0 * sql_script[i].weight / total_weight,
					   script_total_cnt,
					   100.0 * script_total_cnt / total_cnt);

				if (script_total_cnt > 0)
				{
					printf(" - number of transactions actually processed: " INT64_FORMAT " (tps = %f)\n",
						   sstats->cnt, sstats->cnt / bench_duration);

					printf(" - number of failed transactions: " INT64_FORMAT " (%.3f%%)\n",
						   script_failures,
						   100.0 * script_failures / script_total_cnt);

					if (failures_detailed)
					{
						printf(" - number of serialization failures: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->serialization_failures,
							   (100.0 * sstats->serialization_failures /
								script_total_cnt));
						printf(" - number of deadlock failures: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->deadlock_failures,
							   (100.0 * sstats->deadlock_failures /
								script_total_cnt));
						printf(" - number of other failures: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->other_sql_failures,
							   (100.0 * sstats->other_sql_failures /
								script_total_cnt));
					}

					/*
					 * it can be non-zero only if max_tries is not equal to
					 * one
					 */
					if (max_tries != 1)
					{
						printf(" - number of transactions retried: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->retried,
							   100.0 * sstats->retried / script_total_cnt);
						printf(" - total number of retries: " INT64_FORMAT "\n",
							   sstats->retries);
					}

					if (throttle_delay && latency_limit)
						printf(" - number of transactions skipped: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->cnt_skipped,
							   100.0 * sstats->cnt_skipped / script_total_cnt);

				}
				printSimpleStats(" - latency", &sstats->latency);
			}

			/*
			 * Report per-command statistics: latencies, retries after errors,
			 * failures (errors without retrying).
			 */
			if (report_per_command)
			{
				Command   **commands;

				printf("%sstatement latencies in milliseconds%s:\n",
					   per_script_stats ? " - " : "",
					   (max_tries == 1 ?
						" and failures" :
						", failures and retries"));

				for (commands = sql_script[i].commands;
					 *commands != NULL;
					 commands++)
				{
					SimpleStats *cstats = &(*commands)->stats;

					if (max_tries == 1)
						printf("   %11.3f  %10" PRId64 " %s\n",
							   (cstats->count > 0) ?
							   1000.0 * cstats->sum / cstats->count : 0.0,
							   (*commands)->failures,
							   (*commands)->first_line);
					else
						printf("   %11.3f  %10" PRId64 " %10" PRId64 " %s\n",
							   (cstats->count > 0) ?
							   1000.0 * cstats->sum / cstats->count : 0.0,
							   (*commands)->failures,
							   (*commands)->retries,
							   (*commands)->first_line);
				}
			}
		}
	}
}
