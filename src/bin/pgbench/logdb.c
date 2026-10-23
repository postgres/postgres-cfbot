/*-------------------------------------------------------------------------
 *
 * logdb.c
 *		Direct PostgreSQL database logging and reporting (--log-db) for pgbench.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/logdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <math.h>
#include <unistd.h>

#include "common/logging.h"
#include "common/string.h"
#include "config.h"
#include "engine.h"
#include "init.h"
#include "logdb.h"
#include "script.h"
#include "stats.h"

#define LOGDB_COPY_BATCH_BYTES	65536

char	   *log_db_conninfo = NULL;
LogDbUnloggedMode log_db_unlogged = LOG_DB_UNLOGGED_STREAM;
LogDbMode	log_db_mode = LOG_DB_MODE_STREAM;
char	   *log_db_schema = NULL;
bool		log_db_index = false;
char	   *run_label = NULL;
int64		current_run_id = 0;

static PGconn *log_main_conn = NULL;
static PQExpBufferData progress_spool_buf;
static bool progress_spool_initialized = false;
static int64 progress_dropped_rows = 0;

void
parseLogDbUnloggedOption(const char *val)
{
	if (val == NULL || *val == '\0' ||
		pg_strcasecmp(val, "stream") == 0 ||
		pg_strcasecmp(val, "true") == 0 ||
		pg_strcasecmp(val, "on") == 0 ||
		strcmp(val, "1") == 0)
	{
		log_db_unlogged = LOG_DB_UNLOGGED_STREAM;
	}
	else if (pg_strcasecmp(val, "all") == 0)
	{
		log_db_unlogged = LOG_DB_UNLOGGED_ALL;
	}
	else if (pg_strcasecmp(val, "off") == 0 ||
			 pg_strcasecmp(val, "false") == 0 ||
			 strcmp(val, "0") == 0)
	{
		log_db_unlogged = LOG_DB_UNLOGGED_OFF;
	}
	else
	{
		pg_fatal("invalid --log-db-unlogged mode \"%s\" (expected \"stream\", \"all\", or \"off\")",
				 val);
	}
}

void
parseLogDbModeOption(const char *val)
{
	if (val == NULL || pg_strcasecmp(val, "stream") == 0)
		log_db_mode = LOG_DB_MODE_STREAM;
	else if (pg_strcasecmp(val, "deferred") == 0)
		log_db_mode = LOG_DB_MODE_DEFERRED;
	else
		pg_fatal("invalid --log-db-mode \"%s\" (expected \"stream\" or \"deferred\")",
				 val);
}

const char *
logDbUnloggedToString(LogDbUnloggedMode mode)
{
	switch (mode)
	{
		case LOG_DB_UNLOGGED_STREAM:
			return "stream";
		case LOG_DB_UNLOGGED_ALL:
			return "all";
		case LOG_DB_UNLOGGED_OFF:
		default:
			return "off";
	}
}

const char *
logDbModeToString(LogDbMode mode)
{
	switch (mode)
	{
		case LOG_DB_MODE_DEFERRED:
			return "deferred";
		case LOG_DB_MODE_STREAM:
		default:
			return "stream";
	}
}

static const char *
getEffectiveLogDbSchema(void)
{
	if (log_db_schema != NULL && *log_db_schema != '\0')
		return log_db_schema;
	return "public";
}

static bool
execLogDbSQL(PGconn *conn, const char *sql, bool fail_fast)
{
	PGresult   *res;

	if (conn == NULL)
		return false;

	res = PQexec(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK &&
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		if (fail_fast)
			pg_fatal("--log-db SQL execution failed: %s\nQuery: %s",
					 PQerrorMessage(conn), sql);
		pg_log_warning("--log-db SQL execution failed: %s", PQerrorMessage(conn));
		PQclear(res);
		return false;
	}
	PQclear(res);
	return true;
}

static PGconn *
connectLogDb(bool fail_fast, bool create_schema_if_needed)
{
	PGconn	   *conn;
	const char *schema_name = getEffectiveLogDbSchema();
	char	   *quoted_schema;
	PQExpBufferData qbuf;

	if (log_db_conninfo == NULL)
		return NULL;

	if (pg_strcasecmp(log_db_conninfo, "self") == 0 ||
		*log_db_conninfo == '\0')
		conn = doConnect();
	else
		conn = doConnectCustom(log_db_conninfo);

	if (conn == NULL)
	{
		if (fail_fast)
			pg_fatal("could not connect to --log-db \"%s\"", log_db_conninfo);
		return NULL;
	}

	execLogDbSQL(conn, "SET client_min_messages = warning;", fail_fast);

	quoted_schema = PQescapeIdentifier(conn, schema_name, strlen(schema_name));
	if (quoted_schema == NULL)
	{
		if (fail_fast)
			pg_fatal("could not quote schema identifier \"%s\": %s",
					 schema_name, PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}

	initPQExpBuffer(&qbuf);
	if (create_schema_if_needed && strcmp(schema_name, "public") != 0)
	{
		printfPQExpBuffer(&qbuf, "CREATE SCHEMA IF NOT EXISTS %s;", quoted_schema);
		execLogDbSQL(conn, qbuf.data, fail_fast);
	}
	printfPQExpBuffer(&qbuf, "SET search_path TO %s, pg_catalog;", quoted_schema);
	execLogDbSQL(conn, qbuf.data, fail_fast);

	termPQExpBuffer(&qbuf);
	PQfreemem(quoted_schema);

	return conn;
}

/*
 * Check if an existing table's relpersistence ('p' vs 'u') in the target
 * schema differs from want_unlogged, and alter it if needed.
 */
static void
ensureTablePersistence(PGconn *conn, const char *table_name, bool want_unlogged)
{
	const char *schema_name = getEffectiveLogDbSchema();
	const char *params[2];
	PGresult   *res;

	params[0] = schema_name;
	params[1] = table_name;

	res = PQexecParams(conn,
					   "SELECT c.relpersistence "
					   "FROM pg_catalog.pg_class c "
					   "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
					   "WHERE n.nspname = $1 AND c.relname = $2 AND c.relkind = 'r'",
					   2, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1)
	{
		const char *relp = PQgetvalue(res, 0, 0);
		char		cur = (relp != NULL && *relp != '\0') ? relp[0] : 'p';
		char		target = want_unlogged ? 'u' : 'p';

		if (cur != target)
		{
			PQExpBufferData abuf;

			initPQExpBuffer(&abuf);
			printfPQExpBuffer(&abuf, "ALTER TABLE %s SET %s;",
							  table_name, want_unlogged ? "UNLOGGED" : "LOGGED");
			execLogDbSQL(conn, abuf.data, true);
			termPQExpBuffer(&abuf);
		}
	}
	PQclear(res);
}

#ifdef WIN32
static FILE *
open_temp_json_stream(char *tmppath, size_t tmppath_sz)
{
	static unsigned int seq = 0;
	const char *tmpdir = getenv("TMP");

	if (tmpdir == NULL || tmpdir[0] == '\0')
		tmpdir = getenv("TEMP");
	if (tmpdir == NULL || tmpdir[0] == '\0')
		tmpdir = ".";

	snprintf(tmppath, tmppath_sz, "%s/pgbench_json_%d_%u.tmp",
			 tmpdir, (int) getpid(), ++seq);
	return fopen(tmppath, "w+b");
}

static char *
finish_temp_json_stream(FILE *fp, const char *tmppath)
{
	long		sz;
	char	   *buf;
	size_t		nread;

	fflush(fp);
	if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0 ||
		fseek(fp, 0, SEEK_SET) != 0)
	{
		fclose(fp);
		unlink(tmppath);
		pg_fatal("could not seek temporary stream: %m");
	}
	buf = pg_malloc((size_t) sz + 1);
	nread = fread(buf, 1, (size_t) sz, fp);
	buf[nread] = '\0';
	fclose(fp);
	unlink(tmppath);
	if (nread == 0)
	{
		pg_free(buf);
		return pg_strdup("{}");
	}
	return buf;
}
#endif

/*
 * Serialize configuration JSON object to a heap-allocated string.
 */
static char *
serializeConfigJsonToString(void)
{
#ifdef WIN32
	char		tmppath[MAXPGPATH];
	FILE	   *fp = open_temp_json_stream(tmppath, sizeof(tmppath));

	if (fp == NULL)
		pg_fatal("could not open temporary file \"%s\": %m", tmppath);
	writeConfigJsonObject(fp, 0, false);
	return finish_temp_json_stream(fp, tmppath);
#else
	char	   *buf = NULL;
	size_t		len = 0;
	FILE	   *fp = open_memstream(&buf, &len);

	if (fp == NULL)
		pg_fatal("open_memstream failed: %m");
	writeConfigJsonObject(fp, 0, false);
	fclose(fp);
	return buf ? buf : pg_strdup("{}");
#endif
}

/*
 * Serialize full report JSON object to a heap-allocated string.
 */
static char *
serializeReportJsonToString(StatsData *total, double bench_duration,
							double tpmc, double qphh)
{
#ifdef WIN32
	char		tmppath[MAXPGPATH];
	FILE	   *fp = open_temp_json_stream(tmppath, sizeof(tmppath));

	if (fp == NULL)
		pg_fatal("could not open temporary file \"%s\": %m", tmppath);
	writeReportJsonObject(fp, total, bench_duration, tpmc, qphh);
	return finish_temp_json_stream(fp, tmppath);
#else
	char	   *buf = NULL;
	size_t		len = 0;
	FILE	   *fp = open_memstream(&buf, &len);

	if (fp == NULL)
		pg_fatal("open_memstream failed: %m");
	writeReportJsonObject(fp, total, bench_duration, tpmc, qphh);
	fclose(fp);
	return buf ? buf : pg_strdup("{}");
#endif
}

static void
writeFallbackReportFile(const char *report_json)
{
	char		fallback_path[MAXPGPATH];
	FILE	   *fp;

	if (current_run_id > 0)
		snprintf(fallback_path, sizeof(fallback_path),
				 "pgbench_fallback_report_" INT64_FORMAT ".json", current_run_id);
	else
		snprintf(fallback_path, sizeof(fallback_path),
				 "pgbench_fallback_report_%d.json", main_pid);

	fp = fopen(fallback_path, "w");
	if (fp == NULL)
	{
		pg_log_warning("could not open fallback report file \"%s\": %m", fallback_path);
		return;
	}
	fputs(report_json ? report_json : "{}\n", fp);
	fclose(fp);
	pg_log_warning("--log-db unavailable at end of run; wrote fallback JSON report to \"%s\"",
				   fallback_path);
}

void
logDbBootstrapAndStartRun(void)
{
	bool		tier_a_unlogged = (log_db_unlogged == LOG_DB_UNLOGGED_ALL);
	bool		tier_bc_unlogged = (log_db_unlogged == LOG_DB_UNLOGGED_STREAM ||
									log_db_unlogged == LOG_DB_UNLOGGED_ALL);
	const char *tier_a_kw = tier_a_unlogged ? "UNLOGGED " : "";
	const char *tier_bc_kw = tier_bc_unlogged ? "UNLOGGED " : "";
	const char *schema_name = getEffectiveLogDbSchema();
	char	   *quoted_schema;
	PQExpBufferData ddl;
	char	   *config_json;
	char		scale_buf[32];
	char		clients_buf[32];
	char		threads_buf[32];
	const char *ins_params[7];
	PGresult   *res;

	if (log_db_conninfo == NULL)
		return;

	log_main_conn = connectLogDb(true, true);

	quoted_schema = PQescapeIdentifier(log_main_conn, schema_name, strlen(schema_name));
	if (quoted_schema == NULL)
		pg_fatal("could not quote schema identifier \"%s\"", schema_name);

	initPQExpBuffer(&ddl);

	/* 1. Tier A: Run Catalog & Summary Tables */
	printfPQExpBuffer(&ddl,
		"CREATE %sTABLE IF NOT EXISTS pgbench_runs (\n"
		"    run_id               bigserial PRIMARY KEY,\n"
		"    run_label            text,\n"
		"    status               text NOT NULL DEFAULT 'running',\n"
		"    exit_code            integer,\n"
		"    started_at           timestamptz NOT NULL DEFAULT clock_timestamp(),\n"
		"    ended_at             timestamptz,\n"
		"    host                 text,\n"
		"    target_db            text,\n"
		"    scale                integer,\n"
		"    clients              integer,\n"
		"    threads              integer,\n"
		"    duration_sec         double precision,\n"
		"    transactions_cnt     bigint,\n"
		"    transactions_failed  bigint,\n"
		"    expected_rollbacks   bigint,\n"
		"    transactions_skipped bigint,\n"
		"    retries              bigint,\n"
		"    retried              bigint,\n"
		"    tps                  double precision,\n"
		"    latency_avg_ms       double precision,\n"
		"    latency_stddev_ms    double precision,\n"
		"    latency_min_ms       double precision,\n"
		"    latency_max_ms       double precision,\n"
		"    latency_p50_ms       double precision,\n"
		"    latency_p90_ms       double precision,\n"
		"    latency_p95_ms       double precision,\n"
		"    latency_p99_ms       double precision,\n"
		"    latency_p99_9_ms     double precision,\n"
		"    lag_avg_ms           double precision,\n"
		"    lag_p50_ms           double precision,\n"
		"    lag_p90_ms           double precision,\n"
		"    lag_p95_ms           double precision,\n"
		"    lag_p99_ms           double precision,\n"
		"    ontime_pct           double precision,\n"
		"    recall_samples       bigint,\n"
		"    recall_avg           double precision,\n"
		"    recall_p5            double precision,\n"
		"    recall_p50           double precision,\n"
		"    recall_p95           double precision,\n"
		"    recall_initial_q1    double precision,\n"
		"    recall_mature_q4     double precision,\n"
		"    recall_deg_pct       double precision,\n"
		"    validation_passed    bigint,\n"
		"    validation_failed    bigint,\n"
		"    validation_pct       double precision,\n"
		"    sla_all_passed       boolean,\n"
		"    log_db_dropped_rows  bigint DEFAULT 0,\n"
		"    config               jsonb,\n"
		"    report               jsonb\n"
		");\n"
		"CREATE %sTABLE IF NOT EXISTS pgbench_run_groups (\n"
		"    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,\n"
		"    group_idx            integer NOT NULL,\n"
		"    group_name           text NOT NULL,\n"
		"    clients              integer NOT NULL,\n"
		"    threads              integer NOT NULL,\n"
		"    sequential           boolean NOT NULL,\n"
		"    target_rate          double precision,\n"
		"    transactions_cnt     bigint,\n"
		"    transactions_failed  bigint,\n"
		"    expected_rollbacks   bigint,\n"
		"    tps                  double precision,\n"
		"    latency_avg_ms       double precision,\n"
		"    latency_p50_ms       double precision,\n"
		"    latency_p90_ms       double precision,\n"
		"    latency_p95_ms       double precision,\n"
		"    latency_p99_ms       double precision,\n"
		"    latency_p99_9_ms     double precision,\n"
		"    recall_samples       bigint,\n"
		"    recall_avg           double precision,\n"
		"    recall_p5            double precision,\n"
		"    recall_p50           double precision,\n"
		"    PRIMARY KEY (run_id, group_idx)\n"
		");\n"
		"CREATE %sTABLE IF NOT EXISTS pgbench_run_scripts (\n"
		"    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,\n"
		"    script_idx           integer NOT NULL,\n"
		"    script_name          text NOT NULL,\n"
		"    builtin_name         text,\n"
		"    weight               integer NOT NULL,\n"
		"    transactions_cnt     bigint,\n"
		"    transactions_failed  bigint,\n"
		"    expected_rollbacks   bigint,\n"
		"    tps                  double precision,\n"
		"    latency_avg_ms       double precision,\n"
		"    latency_p50_ms       double precision,\n"
		"    latency_p90_ms       double precision,\n"
		"    latency_p95_ms       double precision,\n"
		"    latency_p99_ms       double precision,\n"
		"    latency_p99_9_ms     double precision,\n"
		"    recall_samples       bigint,\n"
		"    recall_avg           double precision,\n"
		"    recall_p5            double precision,\n"
		"    recall_p50           double precision,\n"
		"    PRIMARY KEY (run_id, script_idx)\n"
		");\n"
		"CREATE %sTABLE IF NOT EXISTS pgbench_run_commands (\n"
		"    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,\n"
		"    script_idx           integer NOT NULL,\n"
		"    cmd_idx              integer NOT NULL,\n"
		"    cmd_type             text NOT NULL,\n"
		"    first_line           text,\n"
		"    cnt                  bigint,\n"
		"    failed               bigint,\n"
		"    retries              bigint,\n"
		"    latency_avg_ms       double precision,\n"
		"    latency_stddev_ms    double precision,\n"
		"    latency_min_ms       double precision,\n"
		"    latency_max_ms       double precision,\n"
		"    PRIMARY KEY (run_id, script_idx, cmd_idx)\n"
		");\n"
		"CREATE %sTABLE IF NOT EXISTS pgbench_run_recalls (\n"
		"    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,\n"
		"    scope                text NOT NULL,\n"
		"    metric_name          text NOT NULL,\n"
		"    samples              bigint,\n"
		"    avg_recall           double precision,\n"
		"    stddev_recall        double precision,\n"
		"    min_recall           double precision,\n"
		"    max_recall           double precision,\n"
		"    p5_recall            double precision,\n"
		"    p50_recall           double precision,\n"
		"    p95_recall           double precision,\n"
		"    initial_q1           double precision,\n"
		"    mature_q4            double precision,\n"
		"    delta                double precision,\n"
		"    degradation_pct      double precision,\n"
		"    PRIMARY KEY (run_id, scope, metric_name)\n"
		");\n"
		"CREATE %sTABLE IF NOT EXISTS pgbench_run_slas (\n"
		"    run_id               bigint NOT NULL REFERENCES pgbench_runs(run_id) ON DELETE CASCADE,\n"
		"    sla_idx              integer NOT NULL,\n"
		"    raw_spec             text NOT NULL,\n"
		"    target_scope         text NOT NULL,\n"
		"    metric               text NOT NULL,\n"
		"    op                   text NOT NULL,\n"
		"    threshold_val        double precision,\n"
		"    actual_val           double precision,\n"
		"    passed               boolean NOT NULL,\n"
		"    PRIMARY KEY (run_id, sla_idx)\n"
		");\n",
		tier_a_kw, tier_a_kw, tier_a_kw, tier_a_kw, tier_a_kw, tier_a_kw);

	/* 2. Tier B & Tier C: High-Throughput Stream Tables */
	appendPQExpBuffer(&ddl,
		"CREATE %sTABLE IF NOT EXISTS pgbench_progress_log (\n"
		"    run_id               bigint NOT NULL,\n"
		"    ts_epoch_sec         double precision NOT NULL,\n"
		"    elapsed_sec          double precision NOT NULL,\n"
		"    tps                  double precision,\n"
		"    latency_avg_ms       double precision,\n"
		"    latency_stddev_ms    double precision,\n"
		"    lag_avg_ms           double precision,\n"
		"    failed_cnt           bigint\n"
		");\n"
		"CREATE %sTABLE IF NOT EXISTS pgbench_agg_log (\n"
		"    run_id                  bigint NOT NULL,\n"
		"    thread_id               integer NOT NULL,\n"
		"    interval_start_sec      bigint NOT NULL,\n"
		"    cnt                     bigint NOT NULL,\n"
		"    latency_sum_us          double precision,\n"
		"    latency_sum2_us         double precision,\n"
		"    latency_min_us          double precision,\n"
		"    latency_max_us          double precision,\n"
		"    lag_sum_us              double precision,\n"
		"    lag_sum2_us             double precision,\n"
		"    lag_min_us              double precision,\n"
		"    lag_max_us              double precision,\n"
		"    skipped_cnt             bigint,\n"
		"    retried_cnt             bigint,\n"
		"    retries_cnt             bigint,\n"
		"    serialization_failures  bigint,\n"
		"    deadlock_failures       bigint,\n"
		"    other_sql_failures      bigint,\n"
		"    recall_samples          bigint,\n"
		"    recall_sum              double precision,\n"
		"    validation_passed       bigint,\n"
		"    validation_failed       bigint\n"
		");\n"
		"CREATE %sTABLE IF NOT EXISTS pgbench_tx_log (\n"
		"    run_id               bigint NOT NULL,\n"
		"    thread_id            integer NOT NULL,\n"
		"    client_id            integer NOT NULL,\n"
		"    group_id             integer NOT NULL,\n"
		"    script_idx           integer NOT NULL,\n"
		"    tx_cnt               bigint NOT NULL,\n"
		"    ts_usec              bigint NOT NULL,\n"
		"    status               text NOT NULL,\n"
		"    latency_us           double precision,\n"
		"    lag_us               double precision,\n"
		"    retries              integer,\n"
		"    recall               double precision,\n"
		"    validation_ok        boolean\n"
		");\n",
		tier_bc_kw, tier_bc_kw, tier_bc_kw);

	execLogDbSQL(log_main_conn, ddl.data, true);

	/*
	 * Ensure existing tables match the requested UNLOGGED/LOGGED persistence
	 * mode in FK-safe dependency order.
	 */
	if (tier_a_unlogged)
	{
		/* Children first when transitioning to UNLOGGED */
		ensureTablePersistence(log_main_conn, "pgbench_run_groups", true);
		ensureTablePersistence(log_main_conn, "pgbench_run_scripts", true);
		ensureTablePersistence(log_main_conn, "pgbench_run_commands", true);
		ensureTablePersistence(log_main_conn, "pgbench_run_recalls", true);
		ensureTablePersistence(log_main_conn, "pgbench_run_slas", true);
		ensureTablePersistence(log_main_conn, "pgbench_runs", true);
	}
	else
	{
		/* Parent first when transitioning to LOGGED */
		ensureTablePersistence(log_main_conn, "pgbench_runs", false);
		ensureTablePersistence(log_main_conn, "pgbench_run_groups", false);
		ensureTablePersistence(log_main_conn, "pgbench_run_scripts", false);
		ensureTablePersistence(log_main_conn, "pgbench_run_commands", false);
		ensureTablePersistence(log_main_conn, "pgbench_run_recalls", false);
		ensureTablePersistence(log_main_conn, "pgbench_run_slas", false);
	}
	ensureTablePersistence(log_main_conn, "pgbench_progress_log", tier_bc_unlogged);
	ensureTablePersistence(log_main_conn, "pgbench_agg_log", tier_bc_unlogged);
	ensureTablePersistence(log_main_conn, "pgbench_tx_log", tier_bc_unlogged);

	/* 3. Views & 5 Overloaded Set-Returning Functions (SRFs) */
	printfPQExpBuffer(&ddl,
		"CREATE OR REPLACE VIEW pgbench_runs_v AS\n"
		"SELECT\n"
		"    run_id,\n"
		"    run_label,\n"
		"    status,\n"
		"    exit_code,\n"
		"    started_at,\n"
		"    ended_at,\n"
		"    host,\n"
		"    target_db,\n"
		"    scale,\n"
		"    clients,\n"
		"    threads,\n"
		"    round(duration_sec::numeric, 3) AS duration_sec,\n"
		"    transactions_cnt,\n"
		"    transactions_failed,\n"
		"    expected_rollbacks,\n"
		"    round(tps::numeric, 2) AS tps,\n"
		"    round(latency_avg_ms::numeric, 3) AS latency_avg_ms,\n"
		"    round(latency_p50_ms::numeric, 3) AS latency_p50_ms,\n"
		"    round(latency_p95_ms::numeric, 3) AS latency_p95_ms,\n"
		"    round(latency_p99_ms::numeric, 3) AS latency_p99_ms,\n"
		"    round(ontime_pct::numeric, 2) AS ontime_pct,\n"
		"    round(recall_avg::numeric, 4) AS recall_avg,\n"
		"    round(recall_p5::numeric, 4) AS recall_p5,\n"
		"    round(recall_deg_pct::numeric, 2) AS recall_deg_pct,\n"
		"    round(validation_pct::numeric, 2) AS validation_pct,\n"
		"    sla_all_passed,\n"
		"    log_db_dropped_rows\n"
		"FROM pgbench_runs\n"
		"ORDER BY run_id DESC;\n"
		"\n"
		"CREATE OR REPLACE VIEW pgbench_live_progress_v AS\n"
		"SELECT\n"
		"    p.run_id,\n"
		"    r.run_label,\n"
		"    r.status,\n"
		"    r.target_db,\n"
		"    to_timestamp(p.ts_epoch_sec) AS progress_at,\n"
		"    p.ts_epoch_sec,\n"
		"    round(p.elapsed_sec::numeric, 2) AS elapsed_sec,\n"
		"    round(p.tps::numeric, 2) AS tps,\n"
		"    round(p.latency_avg_ms::numeric, 3) AS latency_avg_ms,\n"
		"    round(p.latency_stddev_ms::numeric, 3) AS latency_stddev_ms,\n"
		"    round(p.lag_avg_ms::numeric, 3) AS lag_avg_ms,\n"
		"    p.failed_cnt\n"
		"FROM pgbench_progress_log p\n"
		"JOIN pgbench_runs r ON r.run_id = p.run_id\n"
		"ORDER BY p.run_id DESC, p.elapsed_sec ASC;\n"
		"\n"
		/* SRF 1: pgbench_show_run(p_run_id bigint DEFAULT NULL) */
		"CREATE OR REPLACE FUNCTION pgbench_show_run(p_run_id bigint DEFAULT NULL)\n"
		"RETURNS TABLE (\n"
		"    section text,\n"
		"    scope   text,\n"
		"    metric  text,\n"
		"    value   text,\n"
		"    unit    text,\n"
		"    details text\n"
		") LANGUAGE sql STABLE SET search_path TO %s, pg_catalog AS $func$\n"
		"WITH target_run AS (\n"
		"    SELECT r.*\n"
		"    FROM pgbench_runs r\n"
		"    WHERE r.run_id = COALESCE(p_run_id, (SELECT max(r2.run_id) FROM pgbench_runs r2))\n"
		"),\n"
		"all_rows AS (\n"
		"    SELECT 100 AS ord, '1. Run Info'::text AS section, 'run'::text AS scope,\n"
		"           'run_id'::text AS metric, r.run_id::text AS value, ''::text AS unit,\n"
		"           COALESCE('label=' || r.run_label, 'unlabeled') AS details\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 101, '1. Run Info', 'run', 'run_label', COALESCE(r.run_label, ''), '', ''\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 102, '1. Run Info', 'run', 'status', r.status, '',\n"
		"           'exit_code=' || COALESCE(r.exit_code::text, 'NULL')\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 103, '1. Run Info', 'run', 'target_db', COALESCE(r.target_db, ''), '',\n"
		"           'host=' || COALESCE(r.host, 'local') || ', scale=' || COALESCE(r.scale::text, '1')\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 104, '1. Run Info', 'run', 'concurrency',\n"
		"           COALESCE(r.clients::text, '0') || ' clients / ' || COALESCE(r.threads::text, '0') || ' threads',\n"
		"           '', 'started=' || r.started_at::text\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 105, '1. Run Info', 'run', 'duration_sec',\n"
		"           COALESCE(round(r.duration_sec::numeric, 4)::text, '0'), 's',\n"
		"           'ended=' || COALESCE(r.ended_at::text, 'running')\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 200, '2. Overall Metrics', 'overall', 'tps',\n"
		"           COALESCE(round(r.tps::numeric, 4)::text, '0'), 'tx/s',\n"
		"           'processed=' || COALESCE(r.transactions_cnt::text, '0')\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 201, '2. Overall Metrics', 'overall', 'transactions_cnt',\n"
		"           COALESCE(r.transactions_cnt::text, '0'), 'tx',\n"
		"           'failed=' || COALESCE(r.transactions_failed::text, '0') ||\n"
		"           ', expected_rollbacks=' || COALESCE(r.expected_rollbacks::text, '0') ||\n"
		"           ', skipped=' || COALESCE(r.transactions_skipped::text, '0')\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 202, '2. Overall Metrics', 'overall', 'latency_avg_ms',\n"
		"           COALESCE(round(r.latency_avg_ms::numeric, 3)::text, '0'), 'ms',\n"
		"           'stddev=' || COALESCE(round(r.latency_stddev_ms::numeric, 3)::text, '0') ||\n"
		"           ' ms, min=' || COALESCE(round(r.latency_min_ms::numeric, 3)::text, '0') ||\n"
		"           ' ms, max=' || COALESCE(round(r.latency_max_ms::numeric, 3)::text, '0') || ' ms'\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 203, '2. Overall Metrics', 'overall', 'latency_p50_ms',\n"
		"           COALESCE(round(r.latency_p50_ms::numeric, 3)::text, '0'), 'ms',\n"
		"           'p90=' || COALESCE(round(r.latency_p90_ms::numeric, 3)::text, '0') || ' ms'\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 204, '2. Overall Metrics', 'overall', 'latency_p95_ms',\n"
		"           COALESCE(round(r.latency_p95_ms::numeric, 3)::text, '0'), 'ms',\n"
		"           'p99=' || COALESCE(round(r.latency_p99_ms::numeric, 3)::text, '0') ||\n"
		"           ' ms, p99.9=' || COALESCE(round(r.latency_p99_9_ms::numeric, 3)::text, '0') || ' ms'\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 205, '2. Overall Metrics', 'overall', 'validation_pct',\n"
		"           COALESCE(round(r.validation_pct::numeric, 2)::text, '100.00'), '%%',\n"
		"           'passed=' || COALESCE(r.validation_passed::text, '0') ||\n"
		"           ', failed=' || COALESCE(r.validation_failed::text, '0')\n"
		"    FROM target_run r\n"
		"    WHERE COALESCE(r.validation_passed, 0) + COALESCE(r.validation_failed, 0) > 0\n"
		"    UNION ALL\n"
		"    SELECT 206, '2. Overall Metrics', 'overall', 'sla_all_passed',\n"
		"           COALESCE(r.sla_all_passed::text, 'true'), '',\n"
		"           'dropped_log_rows=' || COALESCE(r.log_db_dropped_rows::text, '0')\n"
		"    FROM target_run r\n"
		"    UNION ALL\n"
		"    SELECT 300 + g.group_idx * 10 + 1, '3. Client Groups',\n"
		"           'group:' || g.group_name, 'tps',\n"
		"           COALESCE(round(g.tps::numeric, 4)::text, '0'), 'tx/s',\n"
		"           'clients=' || g.clients || ', threads=' || g.threads ||\n"
		"           ', seq=' || g.sequential || ', cnt=' || COALESCE(g.transactions_cnt, 0)\n"
		"    FROM pgbench_run_groups g\n"
		"    JOIN target_run r ON r.run_id = g.run_id\n"
		"    UNION ALL\n"
		"    SELECT 300 + g.group_idx * 10 + 2, '3. Client Groups',\n"
		"           'group:' || g.group_name, 'latency_p95_ms',\n"
		"           COALESCE(round(g.latency_p95_ms::numeric, 3)::text, '0'), 'ms',\n"
		"           'avg=' || COALESCE(round(g.latency_avg_ms::numeric, 3)::text, '0') ||\n"
		"           ' ms, p50=' || COALESCE(round(g.latency_p50_ms::numeric, 3)::text, '0') ||\n"
		"           ' ms, p99=' || COALESCE(round(g.latency_p99_ms::numeric, 3)::text, '0') || ' ms'\n"
		"    FROM pgbench_run_groups g\n"
		"    JOIN target_run r ON r.run_id = g.run_id\n"
		"    UNION ALL\n"
		"    SELECT 400 + s.script_idx * 10 + 1, '4. Scripts',\n"
		"           'script:' || s.script_name, 'tps',\n"
		"           COALESCE(round(s.tps::numeric, 4)::text, '0'), 'tx/s',\n"
		"           'weight=' || s.weight || ', cnt=' || COALESCE(s.transactions_cnt, 0) ||\n"
		"           ', failed=' || COALESCE(s.transactions_failed, 0)\n"
		"    FROM pgbench_run_scripts s\n"
		"    JOIN target_run r ON r.run_id = s.run_id\n"
		"    UNION ALL\n"
		"    SELECT 400 + s.script_idx * 10 + 2, '4. Scripts',\n"
		"           'script:' || s.script_name, 'latency_p95_ms',\n"
		"           COALESCE(round(s.latency_p95_ms::numeric, 3)::text, '0'), 'ms',\n"
		"           'avg=' || COALESCE(round(s.latency_avg_ms::numeric, 3)::text, '0') ||\n"
		"           ' ms, p50=' || COALESCE(round(s.latency_p50_ms::numeric, 3)::text, '0') ||\n"
		"           ' ms, p99=' || COALESCE(round(s.latency_p99_ms::numeric, 3)::text, '0') || ' ms'\n"
		"    FROM pgbench_run_scripts s\n"
		"    JOIN target_run r ON r.run_id = s.run_id\n"
		"    UNION ALL\n"
		"    SELECT 500, '5. Recall & Aging',\n"
		"           rec.scope || ':' || rec.metric_name, 'avg_recall',\n"
		"           COALESCE(round(rec.avg_recall::numeric, 4)::text, '0'), 'ratio',\n"
		"           'samples=' || COALESCE(rec.samples, 0) ||\n"
		"           ', p5=' || COALESCE(round(rec.p5_recall::numeric, 4)::text, '0') ||\n"
		"           ', p50=' || COALESCE(round(rec.p50_recall::numeric, 4)::text, '0') ||\n"
		"           CASE WHEN rec.initial_q1 IS NOT NULL THEN\n"
		"               ', q1=' || round(rec.initial_q1::numeric, 4)::text ||\n"
		"               ', q4=' || round(rec.mature_q4::numeric, 4)::text ||\n"
		"               ', deg_pct=' || round(rec.degradation_pct::numeric, 2)::text || '%%'\n"
		"           ELSE '' END\n"
		"    FROM pgbench_run_recalls rec\n"
		"    JOIN target_run r ON r.run_id = rec.run_id\n"
		"    UNION ALL\n"
		"    SELECT 600 + c.script_idx * 100 + c.cmd_idx, '6. Commands',\n"
		"           'script#' || c.script_idx || ':cmd#' || c.cmd_idx, c.cmd_type,\n"
		"           COALESCE(round(c.latency_avg_ms::numeric, 3)::text, '0'), 'ms',\n"
		"           COALESCE(c.first_line, '') || ' (cnt=' || COALESCE(c.cnt, 0) ||\n"
		"           ', failed=' || COALESCE(c.failed, 0) || ', retries=' || COALESCE(c.retries, 0) || ')'\n"
		"    FROM pgbench_run_commands c\n"
		"    JOIN target_run r ON r.run_id = c.run_id\n"
		"    UNION ALL\n"
		"    SELECT 700 + sla.sla_idx, '7. SLA Verifications',\n"
		"           sla.target_scope, sla.raw_spec,\n"
		"           CASE WHEN sla.passed THEN 'PASS' ELSE 'FAIL' END, '',\n"
		"           'actual=' || COALESCE(round(sla.actual_val::numeric, 4)::text, '0') ||\n"
		"           ' ' || sla.op || ' threshold=' || COALESCE(round(sla.threshold_val::numeric, 4)::text, '0')\n"
		"    FROM pgbench_run_slas sla\n"
		"    JOIN target_run r ON r.run_id = sla.run_id\n"
		")\n"
		"SELECT section, scope, metric, value, unit, details\n"
		"FROM all_rows\n"
		"ORDER BY ord, scope, metric;\n"
		"$func$;\n"
		"\n"
		/* SRF 2: pgbench_show_run(p_run_label text) */
		"CREATE OR REPLACE FUNCTION pgbench_show_run(p_run_label text)\n"
		"RETURNS TABLE (\n"
		"    section text,\n"
		"    scope   text,\n"
		"    metric  text,\n"
		"    value   text,\n"
		"    unit    text,\n"
		"    details text\n"
		") LANGUAGE sql STABLE SET search_path TO %s, pg_catalog AS $func$\n"
		"SELECT s.section, s.scope, s.metric, s.value, s.unit, s.details\n"
		"FROM pgbench_show_run(\n"
		"    (SELECT r.run_id FROM pgbench_runs r WHERE r.run_label = p_run_label ORDER BY r.run_id DESC LIMIT 1)\n"
		") s\n"
		"WHERE EXISTS (SELECT 1 FROM pgbench_runs r WHERE r.run_label = p_run_label);\n"
		"$func$;\n"
		"\n"
		/* SRF 3: pgbench_compare_runs(p_base_run_id bigint, p_cand_run_id bigint) */
		"CREATE OR REPLACE FUNCTION pgbench_compare_runs(p_base_run_id bigint, p_cand_run_id bigint)\n"
		"RETURNS TABLE (\n"
		"    scope      text,\n"
		"    metric     text,\n"
		"    unit       text,\n"
		"    base_val   double precision,\n"
		"    cand_val   double precision,\n"
		"    abs_delta  double precision,\n"
		"    pct_change double precision,\n"
		"    verdict    text\n"
		") LANGUAGE sql STABLE SET search_path TO %s, pg_catalog AS $func$\n"
		"WITH base_r AS (\n"
		"    SELECT * FROM pgbench_runs WHERE run_id = p_base_run_id\n"
		"),\n"
		"cand_r AS (\n"
		"    SELECT * FROM pgbench_runs WHERE run_id = p_cand_run_id\n"
		"),\n"
		"pairs AS (\n"
		"    SELECT 10 AS ord, 'overall'::text AS scope, 'tps'::text AS metric, 'tx/s'::text AS unit,\n"
		"           true AS higher_better, b.tps AS base_val, c.tps AS cand_val\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    UNION ALL\n"
		"    SELECT 11, 'overall', 'latency_avg_ms', 'ms', false, b.latency_avg_ms, c.latency_avg_ms\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    UNION ALL\n"
		"    SELECT 12, 'overall', 'latency_p50_ms', 'ms', false, b.latency_p50_ms, c.latency_p50_ms\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    UNION ALL\n"
		"    SELECT 13, 'overall', 'latency_p90_ms', 'ms', false, b.latency_p90_ms, c.latency_p90_ms\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    UNION ALL\n"
		"    SELECT 14, 'overall', 'latency_p95_ms', 'ms', false, b.latency_p95_ms, c.latency_p95_ms\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    UNION ALL\n"
		"    SELECT 15, 'overall', 'latency_p99_ms', 'ms', false, b.latency_p99_ms, c.latency_p99_ms\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    UNION ALL\n"
		"    SELECT 16, 'overall', 'latency_p99_9_ms', 'ms', false, b.latency_p99_9_ms, c.latency_p99_9_ms\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    UNION ALL\n"
		"    SELECT 17, 'overall', 'transactions_failed', 'tx', false,\n"
		"           b.transactions_failed::double precision, c.transactions_failed::double precision\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    UNION ALL\n"
		"    SELECT 18, 'overall', 'ontime_pct', '%%', true, b.ontime_pct, c.ontime_pct\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    WHERE b.ontime_pct IS NOT NULL OR c.ontime_pct IS NOT NULL\n"
		"    UNION ALL\n"
		"    SELECT 19, 'overall', 'recall_avg', 'ratio', true, b.recall_avg, c.recall_avg\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    WHERE COALESCE(b.recall_samples, 0) > 0 OR COALESCE(c.recall_samples, 0) > 0\n"
		"    UNION ALL\n"
		"    SELECT 20, 'overall', 'recall_p5', 'ratio', true, b.recall_p5, c.recall_p5\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    WHERE COALESCE(b.recall_samples, 0) > 0 OR COALESCE(c.recall_samples, 0) > 0\n"
		"    UNION ALL\n"
		"    SELECT 21, 'overall', 'validation_pct', '%%', true, b.validation_pct, c.validation_pct\n"
		"    FROM base_r b CROSS JOIN cand_r c\n"
		"    WHERE COALESCE(b.validation_passed, 0) + COALESCE(b.validation_failed, 0) > 0\n"
		"       OR COALESCE(c.validation_passed, 0) + COALESCE(c.validation_failed, 0) > 0\n"
		"    UNION ALL\n"
		"    SELECT 100, 'group:' || bg.group_name, 'tps', 'tx/s', true, bg.tps, cg.tps\n"
		"    FROM pgbench_run_groups bg\n"
		"    JOIN pgbench_run_groups cg ON bg.run_id = p_base_run_id AND cg.run_id = p_cand_run_id\n"
		"      AND bg.group_name = cg.group_name\n"
		"    UNION ALL\n"
		"    SELECT 101, 'group:' || bg.group_name, 'latency_avg_ms', 'ms', false, bg.latency_avg_ms, cg.latency_avg_ms\n"
		"    FROM pgbench_run_groups bg\n"
		"    JOIN pgbench_run_groups cg ON bg.run_id = p_base_run_id AND cg.run_id = p_cand_run_id\n"
		"      AND bg.group_name = cg.group_name\n"
		"    UNION ALL\n"
		"    SELECT 102, 'group:' || bg.group_name, 'latency_p95_ms', 'ms', false, bg.latency_p95_ms, cg.latency_p95_ms\n"
		"    FROM pgbench_run_groups bg\n"
		"    JOIN pgbench_run_groups cg ON bg.run_id = p_base_run_id AND cg.run_id = p_cand_run_id\n"
		"      AND bg.group_name = cg.group_name\n"
		"    UNION ALL\n"
		"    SELECT 200, 'script:' || bs.script_name, 'tps', 'tx/s', true, bs.tps, cs.tps\n"
		"    FROM pgbench_run_scripts bs\n"
		"    JOIN pgbench_run_scripts cs ON bs.run_id = p_base_run_id AND cs.run_id = p_cand_run_id\n"
		"      AND bs.script_name = cs.script_name\n"
		"    UNION ALL\n"
		"    SELECT 201, 'script:' || bs.script_name, 'latency_avg_ms', 'ms', false, bs.latency_avg_ms, cs.latency_avg_ms\n"
		"    FROM pgbench_run_scripts bs\n"
		"    JOIN pgbench_run_scripts cs ON bs.run_id = p_base_run_id AND cs.run_id = p_cand_run_id\n"
		"      AND bs.script_name = cs.script_name\n"
		"    UNION ALL\n"
		"    SELECT 202, 'script:' || bs.script_name, 'latency_p95_ms', 'ms', false, bs.latency_p95_ms, cs.latency_p95_ms\n"
		"    FROM pgbench_run_scripts bs\n"
		"    JOIN pgbench_run_scripts cs ON bs.run_id = p_base_run_id AND cs.run_id = p_cand_run_id\n"
		"      AND bs.script_name = cs.script_name\n"
		"    UNION ALL\n"
		"    SELECT 300, 'recall:' || br.scope || ':' || br.metric_name, 'avg_recall', 'ratio', true,\n"
		"           br.avg_recall, cr.avg_recall\n"
		"    FROM pgbench_run_recalls br\n"
		"    JOIN pgbench_run_recalls cr ON br.run_id = p_base_run_id AND cr.run_id = p_cand_run_id\n"
		"      AND br.scope = cr.scope AND br.metric_name = cr.metric_name\n"
		"    UNION ALL\n"
		"    SELECT 301, 'recall:' || br.scope || ':' || br.metric_name, 'p5_recall', 'ratio', true,\n"
		"           br.p5_recall, cr.p5_recall\n"
		"    FROM pgbench_run_recalls br\n"
		"    JOIN pgbench_run_recalls cr ON br.run_id = p_base_run_id AND cr.run_id = p_cand_run_id\n"
		"      AND br.scope = cr.scope AND br.metric_name = cr.metric_name\n"
		")\n"
		"SELECT\n"
		"    p.scope,\n"
		"    p.metric,\n"
		"    p.unit,\n"
		"    p.base_val,\n"
		"    p.cand_val,\n"
		"    (p.cand_val - p.base_val) AS abs_delta,\n"
		"    CASE WHEN p.base_val IS NOT NULL AND abs(p.base_val) > 1e-12\n"
		"         THEN ((p.cand_val - p.base_val) / abs(p.base_val)) * 100.0\n"
		"         ELSE 0.0\n"
		"    END AS pct_change,\n"
		"    CASE\n"
		"        WHEN p.base_val IS NULL OR p.cand_val IS NULL THEN 'N/A'\n"
		"        WHEN abs(p.cand_val - p.base_val) <= 1e-9 THEN 'UNCHANGED'\n"
		"        WHEN abs(p.base_val) > 1e-12 AND abs(((p.cand_val - p.base_val) / abs(p.base_val)) * 100.0) < 1.0 THEN 'UNCHANGED'\n"
		"        WHEN p.higher_better AND p.cand_val > p.base_val THEN 'IMPROVED'\n"
		"        WHEN NOT p.higher_better AND p.cand_val < p.base_val THEN 'IMPROVED'\n"
		"        ELSE 'REGRESSED'\n"
		"    END AS verdict\n"
		"FROM pairs p\n"
		"ORDER BY p.ord, p.scope, p.metric;\n"
		"$func$;\n"
		"\n"
		/* SRF 4: pgbench_compare_runs(p_base_label text, p_cand_label text) */
		"CREATE OR REPLACE FUNCTION pgbench_compare_runs(p_base_label text, p_cand_label text)\n"
		"RETURNS TABLE (\n"
		"    scope      text,\n"
		"    metric     text,\n"
		"    unit       text,\n"
		"    base_val   double precision,\n"
		"    cand_val   double precision,\n"
		"    abs_delta  double precision,\n"
		"    pct_change double precision,\n"
		"    verdict    text\n"
		") LANGUAGE sql STABLE SET search_path TO %s, pg_catalog AS $func$\n"
		"SELECT c.*\n"
		"FROM pgbench_compare_runs(\n"
		"    (SELECT r.run_id FROM pgbench_runs r WHERE r.run_label = p_base_label ORDER BY r.run_id DESC LIMIT 1),\n"
		"    (SELECT r.run_id FROM pgbench_runs r WHERE r.run_label = p_cand_label ORDER BY r.run_id DESC LIMIT 1)\n"
		") c;\n"
		"$func$;\n"
		"\n"
		/* SRF 5: pgbench_compare_runs(p_run_ids bigint[]) */
		"CREATE OR REPLACE FUNCTION pgbench_compare_runs(p_run_ids bigint[])\n"
		"RETURNS TABLE (\n"
		"    scope          text,\n"
		"    metric         text,\n"
		"    unit           text,\n"
		"    run_id         bigint,\n"
		"    run_label      text,\n"
		"    val            double precision,\n"
		"    delta_vs_base  double precision,\n"
		"    pct_vs_base    double precision,\n"
		"    sla_all_passed boolean\n"
		") LANGUAGE sql STABLE SET search_path TO %s, pg_catalog AS $func$\n"
		"WITH run_list AS (\n"
		"    SELECT u.rid, u.ord AS run_ord\n"
		"    FROM unnest(p_run_ids) WITH ORDINALITY AS u(rid, ord)\n"
		"),\n"
		"metrics_per_run AS (\n"
		"    SELECT 10 AS m_ord, 'overall'::text AS scope, 'tps'::text AS metric, 'tx/s'::text AS unit,\n"
		"           r.run_id, r.run_label, rl.run_ord, r.tps AS val, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    UNION ALL\n"
		"    SELECT 11, 'overall', 'latency_avg_ms', 'ms', r.run_id, r.run_label, rl.run_ord,\n"
		"           r.latency_avg_ms, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    UNION ALL\n"
		"    SELECT 12, 'overall', 'latency_p50_ms', 'ms', r.run_id, r.run_label, rl.run_ord,\n"
		"           r.latency_p50_ms, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    UNION ALL\n"
		"    SELECT 13, 'overall', 'latency_p95_ms', 'ms', r.run_id, r.run_label, rl.run_ord,\n"
		"           r.latency_p95_ms, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    UNION ALL\n"
		"    SELECT 14, 'overall', 'latency_p99_ms', 'ms', r.run_id, r.run_label, rl.run_ord,\n"
		"           r.latency_p99_ms, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    UNION ALL\n"
		"    SELECT 15, 'overall', 'transactions_failed', 'tx', r.run_id, r.run_label, rl.run_ord,\n"
		"           r.transactions_failed::double precision, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    UNION ALL\n"
		"    SELECT 16, 'overall', 'recall_avg', 'ratio', r.run_id, r.run_label, rl.run_ord,\n"
		"           r.recall_avg, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    WHERE COALESCE(r.recall_samples, 0) > 0\n"
		"    UNION ALL\n"
		"    SELECT 100, 'group:' || g.group_name, 'tps', 'tx/s', r.run_id, r.run_label, rl.run_ord,\n"
		"           g.tps, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    JOIN pgbench_run_groups g ON g.run_id = r.run_id\n"
		"    UNION ALL\n"
		"    SELECT 101, 'group:' || g.group_name, 'latency_p95_ms', 'ms', r.run_id, r.run_label, rl.run_ord,\n"
		"           g.latency_p95_ms, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    JOIN pgbench_run_groups g ON g.run_id = r.run_id\n"
		"    UNION ALL\n"
		"    SELECT 200, 'script:' || s.script_name, 'tps', 'tx/s', r.run_id, r.run_label, rl.run_ord,\n"
		"           s.tps, r.sla_all_passed\n"
		"    FROM run_list rl JOIN pgbench_runs r ON r.run_id = rl.rid\n"
		"    JOIN pgbench_run_scripts s ON s.run_id = r.run_id\n"
		")\n"
		"SELECT\n"
		"    m.scope,\n"
		"    m.metric,\n"
		"    m.unit,\n"
		"    m.run_id,\n"
		"    m.run_label,\n"
		"    m.val,\n"
		"    (m.val - b.val) AS delta_vs_base,\n"
		"    CASE WHEN b.val IS NOT NULL AND abs(b.val) > 1e-12\n"
		"         THEN ((m.val - b.val) / abs(b.val)) * 100.0\n"
		"         ELSE 0.0\n"
		"    END AS pct_vs_base,\n"
		"    m.sla_all_passed\n"
		"FROM metrics_per_run m\n"
		"LEFT JOIN metrics_per_run b\n"
		"  ON b.scope = m.scope AND b.metric = m.metric AND b.run_ord = 1\n"
		"ORDER BY m.m_ord, m.scope, m.metric, m.run_ord;\n"
		"$func$;\n"
		"\n"
		/* SRF 6: Single-JSON payload helper for the Benchmark Viewer */
		"CREATE OR REPLACE FUNCTION pgbench_viewer_payload_json(\n"
		"    p_mode text DEFAULT 'overview',\n"
		"    p_run_id bigint DEFAULT NULL,\n"
		"    p_base_run_id bigint DEFAULT NULL,\n"
		"    p_cand_run_id bigint DEFAULT NULL,\n"
		"    p_run_ids bigint[] DEFAULT NULL,\n"
		"    p_limit int DEFAULT 50\n"
		") RETURNS jsonb\n"
		"LANGUAGE sql STABLE SET search_path TO %s, pg_catalog AS $func$\n"
		"WITH resolved_target AS (\n"
		"    SELECT COALESCE(\n"
		"        p_run_id,\n"
		"        CASE WHEN p_mode = 'live' THEN\n"
		"            (SELECT r.run_id FROM pgbench_runs r WHERE r.status = 'running' ORDER BY r.run_id DESC LIMIT 1)\n"
		"        ELSE NULL END,\n"
		"        (SELECT max(r.run_id) FROM pgbench_runs r)\n"
		"    ) AS run_id\n"
		"),\n"
		"runs_list AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(rv) ORDER BY rv.run_id DESC), '[]'::jsonb) AS data\n"
		"    FROM (\n"
		"        SELECT * FROM pgbench_runs_v ORDER BY run_id DESC LIMIT GREATEST(COALESCE(p_limit, 50), 1)\n"
		"    ) rv\n"
		"),\n"
		"active_list AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(ar) ORDER BY ar.run_id DESC), '[]'::jsonb) AS data\n"
		"    FROM (\n"
		"        SELECT\n"
		"            r.run_id,\n"
		"            r.run_label,\n"
		"            r.status,\n"
		"            r.started_at,\n"
		"            r.host,\n"
		"            r.target_db,\n"
		"            r.scale,\n"
		"            r.clients,\n"
		"            r.threads,\n"
		"            round(EXTRACT(EPOCH FROM (clock_timestamp() - r.started_at))::numeric, 1) AS live_elapsed_sec,\n"
		"            lp.elapsed_sec AS latest_progress_sec,\n"
		"            round(lp.tps::numeric, 2) AS latest_tps,\n"
		"            round(lp.latency_avg_ms::numeric, 3) AS latest_latency_avg_ms,\n"
		"            round(lp.latency_stddev_ms::numeric, 3) AS latest_latency_stddev_ms,\n"
		"            round(lp.lag_avg_ms::numeric, 3) AS latest_lag_avg_ms,\n"
		"            lp.failed_cnt AS latest_failed_cnt\n"
		"        FROM pgbench_runs r\n"
		"        LEFT JOIN LATERAL (\n"
		"            SELECT p.elapsed_sec, p.tps, p.latency_avg_ms, p.latency_stddev_ms, p.lag_avg_ms, p.failed_cnt\n"
		"            FROM pgbench_progress_log p\n"
		"            WHERE p.run_id = r.run_id\n"
		"            ORDER BY p.elapsed_sec DESC\n"
		"            LIMIT 1\n"
		"        ) lp ON true\n"
		"        WHERE r.status = 'running'\n"
		"    ) ar\n"
		"),\n"
		"sel_run AS (\n"
		"    SELECT to_jsonb(r) AS data\n"
		"    FROM pgbench_runs r\n"
		"    JOIN resolved_target rt ON rt.run_id = r.run_id\n"
		"),\n"
		"show_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(sr)), '[]'::jsonb) AS data\n"
		"    FROM resolved_target rt\n"
		"    CROSS JOIN LATERAL pgbench_show_run(rt.run_id) sr\n"
		"    WHERE rt.run_id IS NOT NULL\n"
		"),\n"
		"grp_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(g) ORDER BY g.group_idx), '[]'::jsonb) AS data\n"
		"    FROM pgbench_run_groups g\n"
		"    JOIN resolved_target rt ON rt.run_id = g.run_id\n"
		"),\n"
		"scr_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(s) ORDER BY s.script_idx), '[]'::jsonb) AS data\n"
		"    FROM pgbench_run_scripts s\n"
		"    JOIN resolved_target rt ON rt.run_id = s.run_id\n"
		"),\n"
		"cmd_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(c) ORDER BY c.script_idx, c.cmd_idx), '[]'::jsonb) AS data\n"
		"    FROM pgbench_run_commands c\n"
		"    JOIN resolved_target rt ON rt.run_id = c.run_id\n"
		"),\n"
		"rec_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(rc) ORDER BY rc.scope, rc.metric_name), '[]'::jsonb) AS data\n"
		"    FROM pgbench_run_recalls rc\n"
		"    JOIN resolved_target rt ON rt.run_id = rc.run_id\n"
		"),\n"
		"sla_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(sl) ORDER BY sl.sla_idx), '[]'::jsonb) AS data\n"
		"    FROM pgbench_run_slas sl\n"
		"    JOIN resolved_target rt ON rt.run_id = sl.run_id\n"
		"),\n"
		"prog_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(pl) ORDER BY pl.elapsed_sec ASC), '[]'::jsonb) AS data\n"
		"    FROM pgbench_progress_log pl\n"
		"    JOIN resolved_target rt ON rt.run_id = pl.run_id\n"
		"),\n"
		"agg_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(ag) ORDER BY ag.interval_start_sec ASC), '[]'::jsonb) AS data\n"
		"    FROM (\n"
		"        SELECT\n"
		"            a.interval_start_sec,\n"
		"            min(a.interval_start_sec) OVER () AS first_sec,\n"
		"            (a.interval_start_sec - min(a.interval_start_sec) OVER ()) AS elapsed_sec,\n"
		"            sum(a.cnt)::bigint AS cnt,\n"
		"            CASE WHEN sum(a.cnt) > 0 THEN (sum(a.latency_sum_us) / sum(a.cnt)) / 1000.0 ELSE 0.0 END AS latency_avg_ms,\n"
		"            max(a.latency_max_us) / 1000.0 AS latency_max_ms,\n"
		"            CASE WHEN sum(a.cnt) > 0 THEN (sum(a.lag_sum_us) / sum(a.cnt)) / 1000.0 ELSE 0.0 END AS lag_avg_ms,\n"
		"            sum(a.serialization_failures + a.deadlock_failures + a.other_sql_failures)::bigint AS failed_cnt,\n"
		"            CASE WHEN sum(a.recall_samples) > 0 THEN sum(a.recall_sum) / sum(a.recall_samples) ELSE NULL END AS recall_avg\n"
		"        FROM pgbench_agg_log a\n"
		"        JOIN resolved_target rt ON rt.run_id = a.run_id\n"
		"        GROUP BY a.interval_start_sec\n"
		"        ORDER BY a.interval_start_sec ASC\n"
		"    ) ag\n"
		"),\n"
		"tx_rows AS (\n"
		"    SELECT COALESCE(jsonb_agg(to_jsonb(txb) ORDER BY txb.bucket_sec ASC), '[]'::jsonb) AS data\n"
		"    FROM (\n"
		"        SELECT\n"
		"            (t.ts_usec / 1000000)::bigint AS bucket_sec,\n"
		"            ((t.ts_usec / 1000000)::bigint - min((t.ts_usec / 1000000)::bigint) OVER ()) AS elapsed_sec,\n"
		"            count(*) FILTER (WHERE t.status = 'ok')::bigint AS ok_cnt,\n"
		"            count(*) FILTER (WHERE t.status <> 'ok')::bigint AS err_cnt,\n"
		"            COALESCE(avg(t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0, 0.0) AS latency_avg_ms,\n"
		"            COALESCE(max(t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0, 0.0) AS latency_max_ms,\n"
		"            avg(t.recall) FILTER (WHERE t.recall IS NOT NULL) AS recall_avg\n"
		"        FROM pgbench_tx_log t\n"
		"        JOIN resolved_target rt ON rt.run_id = t.run_id\n"
		"        GROUP BY (t.ts_usec / 1000000)::bigint\n"
		"        ORDER BY bucket_sec DESC\n"
		"        LIMIT 120\n"
		"    ) txb\n"
		"),\n"
		"live_tx_pcts AS (\n"
		"    SELECT to_jsonb(lt) AS data\n"
		"    FROM (\n"
		"        SELECT\n"
		"            count(*) FILTER (WHERE t.status = 'ok')::bigint AS tx_ok_cnt,\n"
		"            count(*) FILTER (WHERE t.status <> 'ok')::bigint AS tx_err_cnt,\n"
		"            round((avg(t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS avg_ms,\n"
		"            round((percentile_cont(0.50) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p50_ms,\n"
		"            round((percentile_cont(0.90) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p90_ms,\n"
		"            round((percentile_cont(0.95) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p95_ms,\n"
		"            round((percentile_cont(0.99) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p99_ms,\n"
		"            round((percentile_cont(0.999) WITHIN GROUP (ORDER BY t.latency_us) FILTER (WHERE t.status = 'ok') / 1000.0)::numeric, 3) AS p99_9_ms,\n"
		"            round((avg(t.recall) FILTER (WHERE t.recall IS NOT NULL))::numeric, 4) AS recall_avg\n"
		"        FROM pgbench_tx_log t\n"
		"        JOIN resolved_target rt ON rt.run_id = t.run_id\n"
		"    ) lt\n"
		"),\n"
		"pair_diff AS (\n"
		"    SELECT CASE WHEN p_base_run_id IS NOT NULL AND p_cand_run_id IS NOT NULL THEN\n"
		"        jsonb_build_object(\n"
		"            'base_run', (SELECT to_jsonb(r) FROM pgbench_runs r WHERE r.run_id = p_base_run_id),\n"
		"            'cand_run', (SELECT to_jsonb(r) FROM pgbench_runs r WHERE r.run_id = p_cand_run_id),\n"
		"            'diff_rows', COALESCE((\n"
		"                SELECT jsonb_agg(to_jsonb(c))\n"
		"                FROM pgbench_compare_runs(p_base_run_id, p_cand_run_id) c\n"
		"            ), '[]'::jsonb),\n"
		"            'base_progress', COALESCE((\n"
		"                SELECT jsonb_agg(to_jsonb(pl) ORDER BY pl.elapsed_sec ASC)\n"
		"                FROM pgbench_progress_log pl WHERE pl.run_id = p_base_run_id\n"
		"            ), '[]'::jsonb),\n"
		"            'cand_progress', COALESCE((\n"
		"                SELECT jsonb_agg(to_jsonb(pl) ORDER BY pl.elapsed_sec ASC)\n"
		"                FROM pgbench_progress_log pl WHERE pl.run_id = p_cand_run_id\n"
		"            ), '[]'::jsonb)\n"
		"        )\n"
		"    ELSE NULL END AS data\n"
		"),\n"
		"multi_diff AS (\n"
		"    SELECT CASE WHEN p_run_ids IS NOT NULL AND cardinality(p_run_ids) > 0 THEN\n"
		"        jsonb_build_object(\n"
		"            'run_ids', to_jsonb(p_run_ids),\n"
		"            'selected_runs', COALESCE((\n"
		"                SELECT jsonb_agg(to_jsonb(r) ORDER BY u.ord)\n"
		"                FROM unnest(p_run_ids) WITH ORDINALITY AS u(rid, ord)\n"
		"                JOIN pgbench_runs r ON r.run_id = u.rid\n"
		"            ), '[]'::jsonb),\n"
		"            'matrix_rows', COALESCE((\n"
		"                SELECT jsonb_agg(to_jsonb(m))\n"
		"                FROM pgbench_compare_runs(p_run_ids) m\n"
		"            ), '[]'::jsonb)\n"
		"        )\n"
		"    ELSE NULL END AS data\n"
		")\n"
		"SELECT jsonb_build_object(\n"
		"    'schema', current_schema(),\n"
		"    'generated_at', clock_timestamp(),\n"
		"    'mode', COALESCE(p_mode, 'overview'),\n"
		"    'target_run_id', (SELECT run_id FROM resolved_target),\n"
		"    'runs', (SELECT data FROM runs_list),\n"
		"    'active_runs', (SELECT data FROM active_list),\n"
		"    'selected_run', (SELECT data FROM sel_run),\n"
		"    'show_run_rows', (SELECT data FROM show_rows),\n"
		"    'groups', (SELECT data FROM grp_rows),\n"
		"    'scripts', (SELECT data FROM scr_rows),\n"
		"    'commands', (SELECT data FROM cmd_rows),\n"
		"    'recalls', (SELECT data FROM rec_rows),\n"
		"    'slas', (SELECT data FROM sla_rows),\n"
		"    'progress_series', (SELECT data FROM prog_rows),\n"
		"    'agg_series', (SELECT data FROM agg_rows),\n"
		"    'tx_series', (SELECT data FROM tx_rows),\n"
		"    'live_tx_summary', (SELECT data FROM live_tx_pcts),\n"
		"    'compare_pair', (SELECT data FROM pair_diff),\n"
		"    'compare_multi', (SELECT data FROM multi_diff)\n"
		");\n"
		"$func$;\n",
		quoted_schema, quoted_schema, quoted_schema, quoted_schema, quoted_schema, quoted_schema);

	execLogDbSQL(log_main_conn, ddl.data, true);
	termPQExpBuffer(&ddl);
	PQfreemem(quoted_schema);

	/* 4. Insert initial 'running' row into pgbench_runs and capture run_id */
	config_json = serializeConfigJsonToString();
	snprintf(scale_buf, sizeof(scale_buf), "%d", scale);
	snprintf(clients_buf, sizeof(clients_buf), "%d", nclients);
	snprintf(threads_buf, sizeof(threads_buf), "%d", nthreads);

	ins_params[0] = run_label;
	ins_params[1] = pghost;
	ins_params[2] = dbName;
	ins_params[3] = scale_buf;
	ins_params[4] = clients_buf;
	ins_params[5] = threads_buf;
	ins_params[6] = config_json;

	res = PQexecParams(log_main_conn,
					   "INSERT INTO pgbench_runs ("
					   "  run_label, status, started_at, host, target_db, "
					   "  scale, clients, threads, config"
					   ") VALUES ("
					   "  $1, 'running', clock_timestamp(), $2, $3, "
					   "  $4::int4, $5::int4, $6::int4, $7::jsonb"
					   ") RETURNING run_id",
					   7, NULL, ins_params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1)
		pg_fatal("could not insert initial run record into pgbench_runs: %s",
				 PQerrorMessage(log_main_conn));

	current_run_id = strtoi64(PQgetvalue(res, 0, 0), NULL, 10);
	PQclear(res);
	pg_free(config_json);

	if (log_db_mode == LOG_DB_MODE_DEFERRED && progress > 0)
	{
		initPQExpBuffer(&progress_spool_buf);
		progress_spool_initialized = true;
	}
}

void
logDbInitThread(TState *thread)
{
	initPQExpBuffer(&thread->log_buf);
	thread->log_buf_rows = 0;
	thread->log_db_dropped_rows = 0;
	thread->log_conn = NULL;
	thread->log_spool_fp = NULL;
	thread->log_spool_path = NULL;
	thread->log_db_failed = false;
	thread->log_last_flush_us = 0;

	if (log_db_conninfo == NULL || !use_log || current_run_id <= 0)
		return;

	enlargePQExpBuffer(&thread->log_buf, LOGDB_COPY_BATCH_BYTES);

	if (log_db_mode == LOG_DB_MODE_STREAM)
	{
		thread->log_conn = connectLogDb(true, false);
	}
	else
	{
		thread->log_spool_path = psprintf(".pgbench_logdb_spool.%d.%d",
										  main_pid, thread->tid);
		thread->log_spool_fp = fopen(thread->log_spool_path, "w");
		if (thread->log_spool_fp == NULL)
			pg_fatal("thread %d: could not open deferred log spool file \"%s\": %m",
					 thread->tid, thread->log_spool_path);
	}
}

void
logDbFlushThreadBuffer(TState *thread)
{
	if (thread->log_buf.len == 0)
		return;

	if (thread->log_db_failed)
	{
		thread->log_db_dropped_rows += thread->log_buf_rows;
		resetPQExpBuffer(&thread->log_buf);
		thread->log_buf_rows = 0;
		return;
	}

	if (log_db_mode == LOG_DB_MODE_STREAM)
	{
		if (thread->log_conn == NULL)
		{
			thread->log_db_dropped_rows += thread->log_buf_rows;
			thread->log_db_failed = true;
		}
		else
		{
			const char *copy_sql;
			PGresult   *res;
			bool		batch_ok = true;

			if (agg_interval > 0)
			{
				copy_sql =
					"COPY pgbench_agg_log ("
					"  run_id, thread_id, interval_start_sec, cnt, "
					"  latency_sum_us, latency_sum2_us, latency_min_us, latency_max_us, "
					"  lag_sum_us, lag_sum2_us, lag_min_us, lag_max_us, "
					"  skipped_cnt, retried_cnt, retries_cnt, "
					"  serialization_failures, deadlock_failures, other_sql_failures, "
					"  recall_samples, recall_sum, validation_passed, validation_failed"
					") FROM STDIN";
			}
			else
			{
				copy_sql =
					"COPY pgbench_tx_log ("
					"  run_id, thread_id, client_id, group_id, script_idx, "
					"  tx_cnt, ts_usec, status, latency_us, lag_us, "
					"  retries, recall, validation_ok"
					") FROM STDIN";
			}

			res = PQexec(thread->log_conn, copy_sql);
			if (PQresultStatus(res) != PGRES_COPY_IN)
			{
				pg_log_warning("thread %d: could not start COPY batch on --log-db (%s); dropping " INT64_FORMAT " buffered rows",
							   thread->tid, PQerrorMessage(thread->log_conn),
							   thread->log_buf_rows);
				batch_ok = false;
			}
			PQclear(res);

			if (batch_ok &&
				PQputCopyData(thread->log_conn, thread->log_buf.data,
							  (int) thread->log_buf.len) != 1)
			{
				pg_log_warning("thread %d: COPY data to --log-db failed (%s); dropping " INT64_FORMAT " buffered rows",
							   thread->tid, PQerrorMessage(thread->log_conn),
							   thread->log_buf_rows);
				batch_ok = false;
			}

			if (batch_ok && PQputCopyEnd(thread->log_conn, NULL) != 1)
			{
				pg_log_warning("thread %d: PQputCopyEnd on --log-db failed (%s); dropping " INT64_FORMAT " buffered rows",
							   thread->tid, PQerrorMessage(thread->log_conn),
							   thread->log_buf_rows);
				batch_ok = false;
			}

			if (batch_ok)
			{
				while ((res = PQgetResult(thread->log_conn)) != NULL)
				{
					if (PQresultStatus(res) != PGRES_COMMAND_OK)
					{
						pg_log_warning("thread %d: COPY batch commit on --log-db failed (%s); dropping " INT64_FORMAT " buffered rows",
									   thread->tid, PQerrorMessage(thread->log_conn),
									   thread->log_buf_rows);
						batch_ok = false;
					}
					PQclear(res);
				}
			}

			if (!batch_ok)
			{
				thread->log_db_dropped_rows += thread->log_buf_rows;
				thread->log_db_failed = true;
				PQfinish(thread->log_conn);
				thread->log_conn = NULL;
			}
		}
	}
	else
	{
		if (thread->log_spool_fp == NULL ||
			fwrite(thread->log_buf.data, 1, thread->log_buf.len,
				   thread->log_spool_fp) != thread->log_buf.len)
		{
			pg_log_warning("thread %d: write to deferred spool file \"%s\" failed: %m",
						   thread->tid,
						   thread->log_spool_path ? thread->log_spool_path : "");
			thread->log_db_dropped_rows += thread->log_buf_rows;
			thread->log_db_failed = true;
		}
	}

	resetPQExpBuffer(&thread->log_buf);
	thread->log_buf_rows = 0;
}

void
logDbFinishThread(TState *thread)
{
	logDbFlushThreadBuffer(thread);

	if (log_db_mode == LOG_DB_MODE_STREAM && thread->log_conn != NULL)
	{
		PQfinish(thread->log_conn);
		thread->log_conn = NULL;
	}

	if (thread->log_spool_fp != NULL)
	{
		fclose(thread->log_spool_fp);
		thread->log_spool_fp = NULL;
	}

	termPQExpBuffer(&thread->log_buf);
}

void
logDbRecordProgress(double ts_epoch_sec, double elapsed_sec,
					double tps, double latency_avg_ms,
					double latency_stddev_ms, double lag_avg_ms,
					int64 failed_cnt)
{
	if (log_db_conninfo == NULL || current_run_id <= 0)
		return;

	if (log_db_mode == LOG_DB_MODE_DEFERRED)
	{
		if (!progress_spool_initialized)
		{
			initPQExpBuffer(&progress_spool_buf);
			progress_spool_initialized = true;
		}
		appendPQExpBuffer(&progress_spool_buf,
						  INT64_FORMAT "\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t" INT64_FORMAT "\n",
						  current_run_id,
						  ts_epoch_sec,
						  elapsed_sec,
						  tps,
						  latency_avg_ms,
						  latency_stddev_ms,
						  lag_avg_ms,
						  failed_cnt);
		return;
	}

	if (log_main_conn != NULL && PQstatus(log_main_conn) == CONNECTION_OK)
	{
		char		run_id_s[32];
		char		ts_s[64];
		char		el_s[64];
		char		tps_s[64];
		char		lat_s[64];
		char		std_s[64];
		char		lag_s[64];
		char		fail_s[32];
		const char *params[8];
		PGresult   *res;

		snprintf(run_id_s, sizeof(run_id_s), INT64_FORMAT, current_run_id);
		snprintf(ts_s, sizeof(ts_s), "%.6f", ts_epoch_sec);
		snprintf(el_s, sizeof(el_s), "%.6f", elapsed_sec);
		snprintf(tps_s, sizeof(tps_s), "%.6f", tps);
		snprintf(lat_s, sizeof(lat_s), "%.6f", latency_avg_ms);
		snprintf(std_s, sizeof(std_s), "%.6f", latency_stddev_ms);
		snprintf(lag_s, sizeof(lag_s), "%.6f", lag_avg_ms);
		snprintf(fail_s, sizeof(fail_s), INT64_FORMAT, failed_cnt);

		params[0] = run_id_s;
		params[1] = ts_s;
		params[2] = el_s;
		params[3] = tps_s;
		params[4] = lat_s;
		params[5] = std_s;
		params[6] = lag_s;
		params[7] = fail_s;

		res = PQexecParams(log_main_conn,
						   "INSERT INTO pgbench_progress_log ("
						   "  run_id, ts_epoch_sec, elapsed_sec, tps, "
						   "  latency_avg_ms, latency_stddev_ms, lag_avg_ms, failed_cnt"
						   ") VALUES ("
						   "  $1::int8, $2::float8, $3::float8, $4::float8, "
						   "  $5::float8, $6::float8, $7::float8, $8::int8"
						   ")",
						   8, NULL, params, NULL, NULL, 0);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			pg_log_warning("progress log insert to --log-db failed: %s",
						   PQerrorMessage(log_main_conn));
			progress_dropped_rows++;
		}
		PQclear(res);
	}
	else
	{
		progress_dropped_rows++;
	}
}

void
logDbRecordAggInterval(TState *thread, const StatsData *agg, bool has_throttle)
{
	double		lag_sum = has_throttle ? agg->lag.sum : 0.0;
	double		lag_sum2 = has_throttle ? agg->lag.sum2 : 0.0;
	double		lag_min = has_throttle ? agg->lag.min : 0.0;
	double		lag_max = has_throttle ? agg->lag.max : 0.0;
	int64		cnt_skipped = latency_limit ? agg->cnt_skipped : 0;
	int64		retried = (max_tries != 1) ? agg->retried : 0;
	int64		retries = (max_tries != 1) ? agg->retries : 0;
	int64		ser_fail = failures_detailed ? agg->serialization_failures : 0;
	int64		dl_fail = failures_detailed ? agg->deadlock_failures : 0;
	int64		oth_fail = failures_detailed ? agg->other_sql_failures : 0;

	if (log_db_conninfo == NULL || !use_log || current_run_id <= 0)
		return;

	if (thread->log_db_failed)
	{
		thread->log_db_dropped_rows++;
		return;
	}

	appendPQExpBuffer(&thread->log_buf,
					  INT64_FORMAT "\t%d\t" INT64_FORMAT "\t" INT64_FORMAT
					  "\t%.0f\t%.0f\t%.0f\t%.0f"
					  "\t%.0f\t%.0f\t%.0f\t%.0f"
					  "\t" INT64_FORMAT "\t" INT64_FORMAT "\t" INT64_FORMAT
					  "\t" INT64_FORMAT "\t" INT64_FORMAT "\t" INT64_FORMAT
					  "\t" INT64_FORMAT "\t%.6f\t" INT64_FORMAT "\t" INT64_FORMAT "\n",
					  current_run_id,
					  thread->tid,
					  agg->start_time / 1000000,
					  agg->cnt,
					  agg->latency.sum,
					  agg->latency.sum2,
					  agg->latency.min,
					  agg->latency.max,
					  lag_sum,
					  lag_sum2,
					  lag_min,
					  lag_max,
					  cnt_skipped,
					  retried,
					  retries,
					  ser_fail,
					  dl_fail,
					  oth_fail,
					  agg->recall.count,
					  agg->recall.sum,
					  agg->validation_passed,
					  agg->validation_failed);
	thread->log_buf_rows++;

	/*
	 * In stream mode, flush and commit every aggregate interval tick so live
	 * benchmark viewers immediately see each interval in pgbench_agg_log.
	 */
	if (log_db_mode == LOG_DB_MODE_STREAM ||
		thread->log_buf.len >= LOGDB_COPY_BATCH_BYTES)
		logDbFlushThreadBuffer(thread);
}

void
logDbRecordTransaction(TState *thread, const CState *st,
					   pg_time_usec_t now_epoch_usec,
					   bool tx_skipped, double latency_us,
					   double lag_us, bool has_schedule)
{
	const char *status_str;
	char		lat_buf[32];
	char		lag_buf[32];
	char		rec_buf[32];
	const char *val_str;

	if (log_db_conninfo == NULL || !use_log || current_run_id <= 0)
		return;

	if (thread->log_db_failed)
	{
		thread->log_db_dropped_rows++;
		return;
	}

	if (!tx_skipped && st->estatus == ESTATUS_NO_ERROR)
	{
		status_str = "ok";
		snprintf(lat_buf, sizeof(lat_buf), "%.0f", latency_us);
	}
	else
	{
		status_str = getResultString(tx_skipped, st->estatus, failures_detailed);
		strcpy(lat_buf, "\\N");
	}

	if (has_schedule)
		snprintf(lag_buf, sizeof(lag_buf), "%.0f", lag_us);
	else
		strcpy(lag_buf, "0");

	if (!tx_skipped && st->estatus == ESTATUS_NO_ERROR && st->txn_recall_count > 0)
		snprintf(rec_buf, sizeof(rec_buf), "%.6f", st->txn_recalls[0]);
	else
		strcpy(rec_buf, "\\N");

	if (!tx_skipped && st->estatus == ESTATUS_NO_ERROR &&
		(st->txn_validation_passed > 0 || st->txn_validation_failed > 0))
		val_str = (st->txn_validation_failed == 0) ? "t" : "f";
	else
		val_str = "\\N";

	appendPQExpBuffer(&thread->log_buf,
					  INT64_FORMAT "\t%d\t%d\t%d\t%d\t" INT64_FORMAT "\t" INT64_FORMAT
					  "\t%s\t%s\t%s\t%u\t%s\t%s\n",
					  current_run_id,
					  thread->tid,
					  st->id,
					  st->group_id,
					  st->use_file,
					  st->cnt,
					  now_epoch_usec,
					  status_str,
					  lat_buf,
					  lag_buf,
					  st->tries - 1,
					  rec_buf,
					  val_str);
	thread->log_buf_rows++;

	if (thread->log_last_flush_us == 0)
		thread->log_last_flush_us = now_epoch_usec;

	/*
	 * Flush when the batch buffer reaches 64 KB, or in stream mode at least
	 * once per second so the live viewer sees in-flight transaction streams.
	 */
	if (thread->log_buf.len >= LOGDB_COPY_BATCH_BYTES ||
		(log_db_mode == LOG_DB_MODE_STREAM &&
		 (now_epoch_usec - thread->log_last_flush_us) >= 1000000))
	{
		logDbFlushThreadBuffer(thread);
		thread->log_last_flush_us = now_epoch_usec;
	}
}

/*
 * Bulk-load a deferred local spool file into pgbench_tx_log or pgbench_agg_log
 * and remove the temporary spool file.
 */
static void
bulkLoadDeferredSpoolFile(PGconn *conn, TState *thread)
{
	FILE	   *fp;
	const char *copy_sql;
	PGresult   *res;
	char		buf[LOGDB_COPY_BATCH_BYTES];
	size_t		nread;

	if (thread->log_spool_path == NULL)
		return;

	fp = fopen(thread->log_spool_path, "r");
	if (fp == NULL)
	{
		unlink(thread->log_spool_path);
		pg_free(thread->log_spool_path);
		thread->log_spool_path = NULL;
		return;
	}

	if (agg_interval > 0)
	{
		copy_sql =
			"COPY pgbench_agg_log ("
			"  run_id, thread_id, interval_start_sec, cnt, "
			"  latency_sum_us, latency_sum2_us, latency_min_us, latency_max_us, "
			"  lag_sum_us, lag_sum2_us, lag_min_us, lag_max_us, "
			"  skipped_cnt, retried_cnt, retries_cnt, "
			"  serialization_failures, deadlock_failures, other_sql_failures, "
			"  recall_samples, recall_sum, validation_passed, validation_failed"
			") FROM STDIN";
	}
	else
	{
		copy_sql =
			"COPY pgbench_tx_log ("
			"  run_id, thread_id, client_id, group_id, script_idx, "
			"  tx_cnt, ts_usec, status, latency_us, lag_us, "
			"  retries, recall, validation_ok"
			") FROM STDIN";
	}

	res = PQexec(conn, copy_sql);
	if (PQresultStatus(res) != PGRES_COPY_IN)
	{
		pg_log_warning("deferred COPY start on --log-db failed: %s",
					   PQerrorMessage(conn));
		PQclear(res);
		fclose(fp);
		unlink(thread->log_spool_path);
		pg_free(thread->log_spool_path);
		thread->log_spool_path = NULL;
		return;
	}
	PQclear(res);

	while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0)
	{
		if (PQputCopyData(conn, buf, (int) nread) != 1)
		{
			pg_log_warning("deferred COPY data on --log-db failed: %s",
						   PQerrorMessage(conn));
			break;
		}
	}
	fclose(fp);

	if (PQputCopyEnd(conn, NULL) == 1)
	{
		while ((res = PQgetResult(conn)) != NULL)
		{
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
				pg_log_warning("deferred COPY end on --log-db failed: %s",
							   PQerrorMessage(conn));
			PQclear(res);
		}
	}

	unlink(thread->log_spool_path);
	pg_free(thread->log_spool_path);
	thread->log_spool_path = NULL;
}

static void
insertRecallRow(PGconn *conn, const char *scope_str, const char *metric_name,
				int64 samples, double avg_rec, double stddev_rec,
				double min_rec, double max_rec,
				double p5_rec, double p50_rec, double p95_rec,
				bool has_aging, double q1_val, double q4_val,
				double delta_val, double deg_pct)
{
	char		run_id_s[32];
	char		samples_s[32];
	char		avg_s[32];
	char		std_s[32];
	char		min_s[32];
	char		max_s[32];
	char		p5_s[32];
	char		p50_s[32];
	char		p95_s[32];
	char		q1_s[32];
	char		q4_s[32];
	char		del_s[32];
	char		deg_s[32];
	const char *params[15];
	PGresult   *res;

	snprintf(run_id_s, sizeof(run_id_s), INT64_FORMAT, current_run_id);
	snprintf(samples_s, sizeof(samples_s), INT64_FORMAT, samples);
	snprintf(avg_s, sizeof(avg_s), "%.6f", avg_rec);
	snprintf(std_s, sizeof(std_s), "%.6f", stddev_rec);
	snprintf(min_s, sizeof(min_s), "%.6f", min_rec);
	snprintf(max_s, sizeof(max_s), "%.6f", max_rec);
	snprintf(p5_s, sizeof(p5_s), "%.6f", p5_rec);
	snprintf(p50_s, sizeof(p50_s), "%.6f", p50_rec);
	snprintf(p95_s, sizeof(p95_s), "%.6f", p95_rec);

	params[0] = run_id_s;
	params[1] = scope_str;
	params[2] = metric_name;
	params[3] = samples_s;
	params[4] = avg_s;
	params[5] = std_s;
	params[6] = min_s;
	params[7] = max_s;
	params[8] = p5_s;
	params[9] = p50_s;
	params[10] = p95_s;

	if (has_aging)
	{
		snprintf(q1_s, sizeof(q1_s), "%.6f", q1_val);
		snprintf(q4_s, sizeof(q4_s), "%.6f", q4_val);
		snprintf(del_s, sizeof(del_s), "%.6f", delta_val);
		snprintf(deg_s, sizeof(deg_s), "%.6f", deg_pct);
		params[11] = q1_s;
		params[12] = q4_s;
		params[13] = del_s;
		params[14] = deg_s;
	}
	else
	{
		params[11] = NULL;
		params[12] = NULL;
		params[13] = NULL;
		params[14] = NULL;
	}

	res = PQexecParams(conn,
					   "INSERT INTO pgbench_run_recalls ("
					   "  run_id, scope, metric_name, samples, "
					   "  avg_recall, stddev_recall, min_recall, max_recall, "
					   "  p5_recall, p50_recall, p95_recall, "
					   "  initial_q1, mature_q4, delta, degradation_pct"
					   ") VALUES ("
					   "  $1::int8, $2, $3, $4::int8, "
					   "  $5::float8, $6::float8, $7::float8, $8::float8, "
					   "  $9::float8, $10::float8, $11::float8, "
					   "  $12::float8, $13::float8, $14::float8, $15::float8"
					   ") ON CONFLICT (run_id, scope, metric_name) DO UPDATE SET "
					   "  samples = EXCLUDED.samples, "
					   "  avg_recall = EXCLUDED.avg_recall, "
					   "  p5_recall = EXCLUDED.p5_recall, "
					   "  p50_recall = EXCLUDED.p50_recall, "
					   "  p95_recall = EXCLUDED.p95_recall",
					   15, NULL, params, NULL, NULL, 0);
	PQclear(res);
}

static void
insertScopeRecalls(PGconn *conn, const char *scope_str, const StatsData *sd)
{
	if (sd->recall.count > 0)
	{
		double		avg = sd->recall.sum / sd->recall.count;
		double		var = (sd->recall.sum2 / sd->recall.count) - (avg * avg);
		double		std = (var > 0.0) ? sqrt(var) : 0.0;
		bool		has_aging = (sd->recall.count >= 4 &&
								 sd->q1_recall_count > 0 &&
								 sd->q4_recall_count > 0);
		double		q1 = 0.0,
					q4 = 0.0,
					delta = 0.0,
					deg = 0.0;

		if (has_aging)
		{
			double		q4_sum = 0.0;

			q1 = sd->q1_recall_sum / sd->q1_recall_count;
			for (int j = 0; j < sd->q4_recall_count; j++)
				q4_sum += sd->q4_recall_ring[j];
			q4 = q4_sum / sd->q4_recall_count;
			delta = q4 - q1;
			deg = (q1 > 0.000001) ? (delta / q1 * 100.0) : 0.0;
		}

		insertRecallRow(conn, scope_str, "default",
						sd->recall.count, avg, std,
						sd->recall.min, sd->recall.max,
						getRecallPercentile(sd, 5.0),
						getRecallPercentile(sd, 50.0),
						getRecallPercentile(sd, 95.0),
						has_aging, q1, q4, delta, deg);
	}

	for (int m = 0; m < sd->num_named_recalls; m++)
	{
		const NamedRecallStats *ns = &sd->named_recalls[m];

		if (ns->stats.count > 0)
		{
			double		avg = ns->stats.sum / ns->stats.count;
			double		var = (ns->stats.sum2 / ns->stats.count) - (avg * avg);
			double		std = (var > 0.0) ? sqrt(var) : 0.0;
			bool		has_aging = (ns->stats.count >= 4 &&
									 ns->q1_count > 0 &&
									 ns->q4_count > 0);
			double		q1 = 0.0,
						q4 = 0.0,
						delta = 0.0,
						deg = 0.0;

			if (has_aging)
			{
				double		q4_sum = 0.0;

				q1 = ns->q1_sum / ns->q1_count;
				for (int j = 0; j < ns->q4_count; j++)
					q4_sum += ns->q4_ring[j];
				q4 = q4_sum / ns->q4_count;
				delta = q4 - q1;
				deg = (q1 > 0.000001) ? (delta / q1 * 100.0) : 0.0;
			}

			insertRecallRow(conn, scope_str, ns->name,
							ns->stats.count, avg, std,
							ns->stats.min, ns->stats.max,
							getNamedRecallPercentile(sd, ns->name, 5.0),
							getNamedRecallPercentile(sd, ns->name, 50.0),
							getNamedRecallPercentile(sd, ns->name, 95.0),
							has_aging, q1, q4, delta, deg);
		}
	}
}

void
logDbMarkRunAbortedOnExit(TState *thread, int exit_code)
{
	PGconn	   *conn;
	char		run_id_s[32];
	char		exit_s[32];
	const char *params[2];
	PGresult   *res;

	if (log_db_conninfo == NULL || current_run_id <= 0)
		return;

	if (thread != NULL)
		logDbFinishThread(thread);

	conn = connectLogDb(false, false);
	if (conn == NULL)
		return;

	snprintf(run_id_s, sizeof(run_id_s), INT64_FORMAT, current_run_id);
	snprintf(exit_s, sizeof(exit_s), "%d", exit_code);
	params[0] = run_id_s;
	params[1] = exit_s;

	res = PQexecParams(conn,
					   "UPDATE pgbench_runs "
					   "SET status = 'aborted', exit_code = $2::int4, ended_at = clock_timestamp() "
					   "WHERE run_id = $1::int8",
					   2, NULL, params, NULL, NULL, 0);
	PQclear(res);
	PQfinish(conn);
}

void
logDbFinalizeRun(StatsData *total, pg_time_usec_t total_duration,
				 TState *threads, int exit_code)
{
	int64		failures = getFailures(total);
	int64		total_cnt = total->cnt + total->cnt_skipped + failures;
	double		bench_duration = PG_TIME_GET_DOUBLE(total_duration);
	double		tps = (bench_duration > 0.0) ? (total->cnt / bench_duration) : 0.0;
	double		tpmc = 0.0;
	double		qphh = 0.0;
	int64		total_dropped = progress_dropped_rows;
	char	   *report_json;
	const char *status_str;

	if (log_db_conninfo == NULL)
		return;

	/* Compute composite metrics if applicable and ensure SLAs are evaluated */
	{
		int64		new_order_cnt = 0;
		bool		has_tpmc = false;
		int64		tpch_query_cnt = 0;
		bool		has_qphh = false;

		for (int i = 0; i < num_scripts; i++)
		{
			const char *desc = sql_script[i].desc;

			if (strstr(desc, "new_order") != NULL ||
				strstr(desc, "new-order") != NULL ||
				strstr(desc, "NewOrder") != NULL)
			{
				has_tpmc = true;
				new_order_cnt += (per_script_stats ? sql_script[i].stats.cnt : total->cnt);
			}
		}
		for (int g = 0; g < num_groups; g++)
		{
			if (client_groups[g].sequential)
			{
				has_qphh = true;
				tpch_query_cnt += client_groups[g].stats.cnt;
			}
		}
		if (!has_qphh)
		{
			for (int i = 0; i < num_scripts; i++)
			{
				const char *desc = sql_script[i].desc;

				if (strstr(desc, "tpch") != NULL || strstr(desc, "qphh") != NULL ||
					strstr(desc, "pgbench-h") != NULL || strstr(desc, "pgbench_h") != NULL)
				{
					has_qphh = true;
					tpch_query_cnt += (per_script_stats ? sql_script[i].stats.cnt : total->cnt);
				}
			}
		}
		if (has_tpmc && bench_duration > 0.0)
			tpmc = (new_order_cnt * 60.0) / bench_duration;
		if (has_qphh && bench_duration > 0.0)
			qphh = (tpch_query_cnt * 3600.0) / bench_duration;

		if (total_cnt <= 0 && num_slas > 0)
			evaluateSlaRules(total, bench_duration, false);
	}

	for (int i = 0; i < nthreads; i++)
		total_dropped += threads[i].log_db_dropped_rows;

	report_json = serializeReportJsonToString(total, bench_duration, tpmc, qphh);

	/* Ensure log_main_conn is alive; reconnect if needed */
	if (log_main_conn == NULL || PQstatus(log_main_conn) != CONNECTION_OK)
	{
		if (log_main_conn != NULL)
			PQfinish(log_main_conn);
		log_main_conn = connectLogDb(false, false);
	}

	if (log_main_conn == NULL || PQstatus(log_main_conn) != CONNECTION_OK)
	{
		writeFallbackReportFile(report_json);
		pg_free(report_json);
		if (log_main_conn != NULL)
		{
			PQfinish(log_main_conn);
			log_main_conn = NULL;
		}
		return;
	}

	/* 1. In deferred mode, bulk-load progress and per-thread transaction/agg logs */
	if (log_db_mode == LOG_DB_MODE_DEFERRED)
	{
		if (progress_spool_initialized && progress_spool_buf.len > 0)
		{
			PGresult   *res = PQexec(log_main_conn,
									 "COPY pgbench_progress_log ("
									 "  run_id, ts_epoch_sec, elapsed_sec, tps, "
									 "  latency_avg_ms, latency_stddev_ms, lag_avg_ms, failed_cnt"
									 ") FROM STDIN");

			if (PQresultStatus(res) == PGRES_COPY_IN)
			{
				PQclear(res);
				(void) PQputCopyData(log_main_conn, progress_spool_buf.data,
									 (int) progress_spool_buf.len);
				if (PQputCopyEnd(log_main_conn, NULL) == 1)
				{
					while ((res = PQgetResult(log_main_conn)) != NULL)
						PQclear(res);
				}
			}
			else
				PQclear(res);
		}
		if (progress_spool_initialized)
		{
			termPQExpBuffer(&progress_spool_buf);
			progress_spool_initialized = false;
		}

		if (use_log)
		{
			for (int i = 0; i < nthreads; i++)
				bulkLoadDeferredSpoolFile(log_main_conn, &threads[i]);
		}
	}

	/* 2. Update pgbench_runs with final status and summary statistics */
	if (exit_code == 0)
		status_str = "completed";
	else if (exit_code == 1 && sla_failed)
		status_str = "sla_failed";
	else
		status_str = "aborted";

	{
		double		lat_avg_ms = (total->latency.count > 0) ?
			(0.001 * total->latency.sum / total->latency.count) :
			((total_cnt > 0) ? (0.001 * total_duration * nclients / total_cnt) : 0.0);
		double		lat_var = (total->latency.count > 0) ?
			(total->latency.sum2 / total->latency.count) -
			((total->latency.sum / total->latency.count) *
			 (total->latency.sum / total->latency.count)) : 0.0;
		double		lat_std_ms = (lat_var > 0.0) ? (0.001 * sqrt(lat_var)) : 0.0;
		double		lat_min_ms = (total->latency.count > 0) ? (0.001 * total->latency.min) : 0.0;
		double		lat_max_ms = (total->latency.count > 0) ? (0.001 * total->latency.max) : 0.0;
		double		lat_p50_ms = 0.001 * getLatencyPercentile(total, 50.0);
		double		lat_p90_ms = 0.001 * getLatencyPercentile(total, 90.0);
		double		lat_p95_ms = 0.001 * getLatencyPercentile(total, 95.0);
		double		lat_p99_ms = 0.001 * getLatencyPercentile(total, 99.0);
		double		lat_p99_9_ms = 0.001 * getLatencyPercentile(total, 99.9);
		bool		has_lag = (total->lag.count > 0 || total->lag_hist_count > 0 || throttle_delay > 0);
		bool		has_recall = (total->recall.count > 0);
		bool		has_aging = (has_recall && total->recall.count >= 4 &&
								 total->q1_recall_count > 0 && total->q4_recall_count > 0);
		double		q1_avg = 0.0,
					q4_avg = 0.0,
					deg_pct = 0.0;
		double		val_pct = (total->validation_passed + total->validation_failed > 0) ?
			(100.0 * (double) total->validation_passed /
			 (double) (total->validation_passed + total->validation_failed)) : 100.0;
		char		b_run_id[32],
					b_exit[32],
					b_dur[32],
					b_cnt[32],
					b_fail[32],
					b_eroll[32],
					b_skip[32],
					b_retries[32],
					b_retried[32],
					b_tps[32],
					b_lavg[32],
					b_lstd[32],
					b_lmin[32],
					b_lmax[32],
					b_lp50[32],
					b_lp90[32],
					b_lp95[32],
					b_lp99[32],
					b_lp999[32],
					b_gavg[32],
					b_gp50[32],
					b_gp90[32],
					b_gp95[32],
					b_gp99[32],
					b_ontime[32],
					b_rsamp[32],
					b_ravg[32],
					b_rp5[32],
					b_rp50[32],
					b_rp95[32],
					b_rq1[32],
					b_rq4[32],
					b_rdeg[32],
					b_vpass[32],
					b_vfail[32],
					b_vpct[32],
					b_drop[32];
		const char *params[40];
		PGresult   *res;

		if (has_aging)
		{
			double		q4_sum = 0.0;

			q1_avg = total->q1_recall_sum / total->q1_recall_count;
			for (int j = 0; j < total->q4_recall_count; j++)
				q4_sum += total->q4_recall_ring[j];
			q4_avg = q4_sum / total->q4_recall_count;
			deg_pct = (q1_avg > 0.000001) ? ((q4_avg - q1_avg) / q1_avg * 100.0) : 0.0;
		}

		snprintf(b_run_id, sizeof(b_run_id), INT64_FORMAT, current_run_id);
		snprintf(b_exit, sizeof(b_exit), "%d", exit_code);
		snprintf(b_dur, sizeof(b_dur), "%.6f", bench_duration);
		snprintf(b_cnt, sizeof(b_cnt), INT64_FORMAT, total->cnt);
		snprintf(b_fail, sizeof(b_fail), INT64_FORMAT, failures);
		snprintf(b_eroll, sizeof(b_eroll), INT64_FORMAT, total->expected_rollbacks);
		snprintf(b_skip, sizeof(b_skip), INT64_FORMAT, total->cnt_skipped);
		snprintf(b_retries, sizeof(b_retries), INT64_FORMAT, total->retries);
		snprintf(b_retried, sizeof(b_retried), INT64_FORMAT, total->retried);
		snprintf(b_tps, sizeof(b_tps), "%.6f", tps);
		snprintf(b_lavg, sizeof(b_lavg), "%.6f", lat_avg_ms);
		snprintf(b_lstd, sizeof(b_lstd), "%.6f", lat_std_ms);
		snprintf(b_lmin, sizeof(b_lmin), "%.6f", lat_min_ms);
		snprintf(b_lmax, sizeof(b_lmax), "%.6f", lat_max_ms);
		snprintf(b_lp50, sizeof(b_lp50), "%.6f", lat_p50_ms);
		snprintf(b_lp90, sizeof(b_lp90), "%.6f", lat_p90_ms);
		snprintf(b_lp95, sizeof(b_lp95), "%.6f", lat_p95_ms);
		snprintf(b_lp99, sizeof(b_lp99), "%.6f", lat_p99_ms);
		snprintf(b_lp999, sizeof(b_lp999), "%.6f", lat_p99_9_ms);

		snprintf(b_rsamp, sizeof(b_rsamp), INT64_FORMAT, total->recall.count);
		snprintf(b_vpass, sizeof(b_vpass), INT64_FORMAT, total->validation_passed);
		snprintf(b_vfail, sizeof(b_vfail), INT64_FORMAT, total->validation_failed);
		snprintf(b_vpct, sizeof(b_vpct), "%.6f", val_pct);
		snprintf(b_drop, sizeof(b_drop), INT64_FORMAT, total_dropped);

		params[0] = b_run_id;
		params[1] = status_str;
		params[2] = b_exit;
		params[3] = b_dur;
		params[4] = b_cnt;
		params[5] = b_fail;
		params[6] = b_eroll;
		params[7] = b_skip;
		params[8] = b_retries;
		params[9] = b_retried;
		params[10] = b_tps;
		params[11] = b_lavg;
		params[12] = b_lstd;
		params[13] = b_lmin;
		params[14] = b_lmax;
		params[15] = b_lp50;
		params[16] = b_lp90;
		params[17] = b_lp95;
		params[18] = b_lp99;
		params[19] = b_lp999;

		if (has_lag)
		{
			snprintf(b_gavg, sizeof(b_gavg), "%.6f",
					 total->lag.count > 0 ? 0.001 * total->lag.sum / total->lag.count : 0.0);
			snprintf(b_gp50, sizeof(b_gp50), "%.6f", 0.001 * getLagPercentile(total, 50.0));
			snprintf(b_gp90, sizeof(b_gp90), "%.6f", 0.001 * getLagPercentile(total, 90.0));
			snprintf(b_gp95, sizeof(b_gp95), "%.6f", 0.001 * getLagPercentile(total, 95.0));
			snprintf(b_gp99, sizeof(b_gp99), "%.6f", 0.001 * getLagPercentile(total, 99.0));
			snprintf(b_ontime, sizeof(b_ontime), "%.6f",
					 total->scheduled_count > 0 ? 100.0 * total->ontime_count / total->scheduled_count : 100.0);
			params[20] = b_gavg;
			params[21] = b_gp50;
			params[22] = b_gp90;
			params[23] = b_gp95;
			params[24] = b_gp99;
			params[25] = b_ontime;
		}
		else
		{
			params[20] = NULL;
			params[21] = NULL;
			params[22] = NULL;
			params[23] = NULL;
			params[24] = NULL;
			params[25] = NULL;
		}

		params[26] = b_rsamp;
		if (has_recall)
		{
			snprintf(b_ravg, sizeof(b_ravg), "%.6f", total->recall.sum / total->recall.count);
			snprintf(b_rp5, sizeof(b_rp5), "%.6f", getRecallPercentile(total, 5.0));
			snprintf(b_rp50, sizeof(b_rp50), "%.6f", getRecallPercentile(total, 50.0));
			snprintf(b_rp95, sizeof(b_rp95), "%.6f", getRecallPercentile(total, 95.0));
			params[27] = b_ravg;
			params[28] = b_rp5;
			params[29] = b_rp50;
			params[30] = b_rp95;
		}
		else
		{
			params[27] = NULL;
			params[28] = NULL;
			params[29] = NULL;
			params[30] = NULL;
		}

		if (has_aging)
		{
			snprintf(b_rq1, sizeof(b_rq1), "%.6f", q1_avg);
			snprintf(b_rq4, sizeof(b_rq4), "%.6f", q4_avg);
			snprintf(b_rdeg, sizeof(b_rdeg), "%.6f", deg_pct);
			params[31] = b_rq1;
			params[32] = b_rq4;
			params[33] = b_rdeg;
		}
		else
		{
			params[31] = NULL;
			params[32] = NULL;
			params[33] = NULL;
		}

		params[34] = b_vpass;
		params[35] = b_vfail;
		params[36] = b_vpct;
		params[37] = sla_failed ? "f" : "t";
		params[38] = b_drop;
		params[39] = report_json;

		res = PQexecParams(log_main_conn,
						   "UPDATE pgbench_runs SET "
						   "  status = $2, "
						   "  exit_code = $3::int4, "
						   "  ended_at = clock_timestamp(), "
						   "  duration_sec = $4::float8, "
						   "  transactions_cnt = $5::int8, "
						   "  transactions_failed = $6::int8, "
						   "  expected_rollbacks = $7::int8, "
						   "  transactions_skipped = $8::int8, "
						   "  retries = $9::int8, "
						   "  retried = $10::int8, "
						   "  tps = $11::float8, "
						   "  latency_avg_ms = $12::float8, "
						   "  latency_stddev_ms = $13::float8, "
						   "  latency_min_ms = $14::float8, "
						   "  latency_max_ms = $15::float8, "
						   "  latency_p50_ms = $16::float8, "
						   "  latency_p90_ms = $17::float8, "
						   "  latency_p95_ms = $18::float8, "
						   "  latency_p99_ms = $19::float8, "
						   "  latency_p99_9_ms = $20::float8, "
						   "  lag_avg_ms = $21::float8, "
						   "  lag_p50_ms = $22::float8, "
						   "  lag_p90_ms = $23::float8, "
						   "  lag_p95_ms = $24::float8, "
						   "  lag_p99_ms = $25::float8, "
						   "  ontime_pct = $26::float8, "
						   "  recall_samples = $27::int8, "
						   "  recall_avg = $28::float8, "
						   "  recall_p5 = $29::float8, "
						   "  recall_p50 = $30::float8, "
						   "  recall_p95 = $31::float8, "
						   "  recall_initial_q1 = $32::float8, "
						   "  recall_mature_q4 = $33::float8, "
						   "  recall_deg_pct = $34::float8, "
						   "  validation_passed = $35::int8, "
						   "  validation_failed = $36::int8, "
						   "  validation_pct = $37::float8, "
						   "  sla_all_passed = $38::bool, "
						   "  log_db_dropped_rows = $39::int8, "
						   "  report = $40::jsonb "
						   "WHERE run_id = $1::int8",
						   40, NULL, params, NULL, NULL, 0);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			pg_log_warning("could not update pgbench_runs on --log-db: %s",
						   PQerrorMessage(log_main_conn));
			writeFallbackReportFile(report_json);
		}
		PQclear(res);
	}

	/* 3. Populate pgbench_run_groups */
	for (int g = 0; g < num_groups; g++)
	{
		ClientGroup *grp = &client_groups[g];
		StatsData  *gs = &grp->stats;
		char		b_run_id[32],
					b_gidx[32],
					b_cli[32],
					b_thr[32],
					b_rate[32],
					b_cnt[32],
					b_fail[32],
					b_eroll[32],
					b_tps[32],
					b_lavg[32],
					b_lp50[32],
					b_lp90[32],
					b_lp95[32],
					b_lp99[32],
					b_lp999[32],
					b_rsamp[32],
					b_ravg[32],
					b_rp5[32],
					b_rp50[32];
		const char *params[21];
		PGresult   *res;

		snprintf(b_run_id, sizeof(b_run_id), INT64_FORMAT, current_run_id);
		snprintf(b_gidx, sizeof(b_gidx), "%d", g + 1);
		snprintf(b_cli, sizeof(b_cli), "%d", grp->nclients);
		snprintf(b_thr, sizeof(b_thr), "%d", grp->nthreads);
		snprintf(b_rate, sizeof(b_rate), "%.6f", grp->rate);
		snprintf(b_cnt, sizeof(b_cnt), INT64_FORMAT, gs->cnt);
		snprintf(b_fail, sizeof(b_fail), INT64_FORMAT, getFailures(gs));
		snprintf(b_eroll, sizeof(b_eroll), INT64_FORMAT, gs->expected_rollbacks);
		snprintf(b_tps, sizeof(b_tps), "%.6f", bench_duration > 0 ? gs->cnt / bench_duration : 0.0);
		snprintf(b_lavg, sizeof(b_lavg), "%.6f", gs->latency.count > 0 ? 0.001 * gs->latency.sum / gs->latency.count : 0.0);
		snprintf(b_lp50, sizeof(b_lp50), "%.6f", 0.001 * getLatencyPercentile(gs, 50.0));
		snprintf(b_lp90, sizeof(b_lp90), "%.6f", 0.001 * getLatencyPercentile(gs, 90.0));
		snprintf(b_lp95, sizeof(b_lp95), "%.6f", 0.001 * getLatencyPercentile(gs, 95.0));
		snprintf(b_lp99, sizeof(b_lp99), "%.6f", 0.001 * getLatencyPercentile(gs, 99.0));
		snprintf(b_lp999, sizeof(b_lp999), "%.6f", 0.001 * getLatencyPercentile(gs, 99.9));
		snprintf(b_rsamp, sizeof(b_rsamp), INT64_FORMAT, gs->recall.count);
		snprintf(b_ravg, sizeof(b_ravg), "%.6f", gs->recall.count > 0 ? gs->recall.sum / gs->recall.count : 0.0);
		snprintf(b_rp5, sizeof(b_rp5), "%.6f", getRecallPercentile(gs, 5.0));
		snprintf(b_rp50, sizeof(b_rp50), "%.6f", getRecallPercentile(gs, 50.0));

		params[0] = b_run_id;
		params[1] = b_gidx;
		params[2] = grp->name;
		params[3] = b_cli;
		params[4] = b_thr;
		params[5] = grp->sequential ? "t" : "f";
		params[6] = b_rate;
		params[7] = b_cnt;
		params[8] = b_fail;
		params[9] = b_eroll;
		params[10] = b_tps;
		params[11] = b_lavg;
		params[12] = b_lp50;
		params[13] = b_lp90;
		params[14] = b_lp95;
		params[15] = b_lp99;
		params[16] = b_lp999;
		params[17] = b_rsamp;
		params[18] = (gs->recall.count > 0) ? b_ravg : NULL;
		params[19] = (gs->recall.count > 0) ? b_rp5 : NULL;
		params[20] = (gs->recall.count > 0) ? b_rp50 : NULL;

		res = PQexecParams(log_main_conn,
						   "INSERT INTO pgbench_run_groups ("
						   "  run_id, group_idx, group_name, clients, threads, "
						   "  sequential, target_rate, transactions_cnt, transactions_failed, "
						   "  expected_rollbacks, tps, latency_avg_ms, latency_p50_ms, "
						   "  latency_p90_ms, latency_p95_ms, latency_p99_ms, latency_p99_9_ms, "
						   "  recall_samples, recall_avg, recall_p5, recall_p50"
						   ") VALUES ("
						   "  $1::int8, $2::int4, $3, $4::int4, $5::int4, "
						   "  $6::bool, $7::float8, $8::int8, $9::int8, "
						   "  $10::int8, $11::float8, $12::float8, $13::float8, "
						   "  $14::float8, $15::float8, $16::float8, $17::float8, "
						   "  $18::int8, $19::float8, $20::float8, $21::float8"
						   ")",
						   21, NULL, params, NULL, NULL, 0);
		PQclear(res);
	}

	/* 4. Populate pgbench_run_scripts and pgbench_run_commands */
	for (int i = 0; i < num_scripts; i++)
	{
		StatsData  *ss = per_script_stats ? &sql_script[i].stats : total;
		char		b_run_id[32],
					b_sidx[32],
					b_wt[32],
					b_cnt[32],
					b_fail[32],
					b_eroll[32],
					b_tps[32],
					b_lavg[32],
					b_lp50[32],
					b_lp90[32],
					b_lp95[32],
					b_lp99[32],
					b_lp999[32],
					b_rsamp[32],
					b_ravg[32],
					b_rp5[32],
					b_rp50[32];
		const char *params[19];
		PGresult   *res;

		snprintf(b_run_id, sizeof(b_run_id), INT64_FORMAT, current_run_id);
		snprintf(b_sidx, sizeof(b_sidx), "%d", i + 1);
		snprintf(b_wt, sizeof(b_wt), "%d", sql_script[i].weight);
		snprintf(b_cnt, sizeof(b_cnt), INT64_FORMAT, ss->cnt);
		snprintf(b_fail, sizeof(b_fail), INT64_FORMAT, getFailures(ss));
		snprintf(b_eroll, sizeof(b_eroll), INT64_FORMAT, ss->expected_rollbacks);
		snprintf(b_tps, sizeof(b_tps), "%.6f", bench_duration > 0 ? ss->cnt / bench_duration : 0.0);
		snprintf(b_lavg, sizeof(b_lavg), "%.6f", ss->latency.count > 0 ? 0.001 * ss->latency.sum / ss->latency.count : 0.0);
		snprintf(b_lp50, sizeof(b_lp50), "%.6f", 0.001 * getLatencyPercentile(ss, 50.0));
		snprintf(b_lp90, sizeof(b_lp90), "%.6f", 0.001 * getLatencyPercentile(ss, 90.0));
		snprintf(b_lp95, sizeof(b_lp95), "%.6f", 0.001 * getLatencyPercentile(ss, 95.0));
		snprintf(b_lp99, sizeof(b_lp99), "%.6f", 0.001 * getLatencyPercentile(ss, 99.0));
		snprintf(b_lp999, sizeof(b_lp999), "%.6f", 0.001 * getLatencyPercentile(ss, 99.9));
		snprintf(b_rsamp, sizeof(b_rsamp), INT64_FORMAT, ss->recall.count);
		snprintf(b_ravg, sizeof(b_ravg), "%.6f", ss->recall.count > 0 ? ss->recall.sum / ss->recall.count : 0.0);
		snprintf(b_rp5, sizeof(b_rp5), "%.6f", getRecallPercentile(ss, 5.0));
		snprintf(b_rp50, sizeof(b_rp50), "%.6f", getRecallPercentile(ss, 50.0));

		params[0] = b_run_id;
		params[1] = b_sidx;
		params[2] = sql_script[i].desc;
		params[3] = sql_script[i].builtin_name;
		params[4] = b_wt;
		params[5] = b_cnt;
		params[6] = b_fail;
		params[7] = b_eroll;
		params[8] = b_tps;
		params[9] = b_lavg;
		params[10] = b_lp50;
		params[11] = b_lp90;
		params[12] = b_lp95;
		params[13] = b_lp99;
		params[14] = b_lp999;
		params[15] = b_rsamp;
		params[16] = (ss->recall.count > 0) ? b_ravg : NULL;
		params[17] = (ss->recall.count > 0) ? b_rp5 : NULL;
		params[18] = (ss->recall.count > 0) ? b_rp50 : NULL;

		res = PQexecParams(log_main_conn,
						   "INSERT INTO pgbench_run_scripts ("
						   "  run_id, script_idx, script_name, builtin_name, weight, "
						   "  transactions_cnt, transactions_failed, expected_rollbacks, "
						   "  tps, latency_avg_ms, latency_p50_ms, latency_p90_ms, "
						   "  latency_p95_ms, latency_p99_ms, latency_p99_9_ms, "
						   "  recall_samples, recall_avg, recall_p5, recall_p50"
						   ") VALUES ("
						   "  $1::int8, $2::int4, $3, $4, $5::int4, "
						   "  $6::int8, $7::int8, $8::int8, "
						   "  $9::float8, $10::float8, $11::float8, $12::float8, "
						   "  $13::float8, $14::float8, $15::float8, "
						   "  $16::int8, $17::float8, $18::float8, $19::float8"
						   ")",
						   19, NULL, params, NULL, NULL, 0);
		PQclear(res);

		if (report_per_command && sql_script[i].commands != NULL)
		{
			for (int c = 0; sql_script[i].commands[c] != NULL; c++)
			{
				Command    *cmd = sql_script[i].commands[c];
				SimpleStats *cs = &cmd->stats;
				/* Note: command stats are stored in seconds by PG_TIME_GET_DOUBLE */
				double		c_avg_ms = (cs->count > 0) ? (1000.0 * cs->sum / cs->count) : 0.0;
				double		c_var_s2 = (cs->count > 0) ?
					((cs->sum2 / cs->count) - ((cs->sum / cs->count) * (cs->sum / cs->count))) : 0.0;
				double		c_std_ms = (c_var_s2 > 0.0) ? (1000.0 * sqrt(c_var_s2)) : 0.0;
				double		c_min_ms = (cs->count > 0) ? (1000.0 * cs->min) : 0.0;
				double		c_max_ms = (cs->count > 0) ? (1000.0 * cs->max) : 0.0;
				char		b_cidx[32],
							b_ccnt[32],
							b_cfail[32],
							b_cret[32],
							b_cavg[32],
							b_cstd[32],
							b_cmin[32],
							b_cmax[32];
				const char *cparams[12];
				PGresult   *cres;

				snprintf(b_cidx, sizeof(b_cidx), "%d", c + 1);
				snprintf(b_ccnt, sizeof(b_ccnt), INT64_FORMAT, cs->count);
				snprintf(b_cfail, sizeof(b_cfail), INT64_FORMAT, cmd->failures);
				snprintf(b_cret, sizeof(b_cret), INT64_FORMAT, cmd->retries);
				snprintf(b_cavg, sizeof(b_cavg), "%.6f", c_avg_ms);
				snprintf(b_cstd, sizeof(b_cstd), "%.6f", c_std_ms);
				snprintf(b_cmin, sizeof(b_cmin), "%.6f", c_min_ms);
				snprintf(b_cmax, sizeof(b_cmax), "%.6f", c_max_ms);

				cparams[0] = b_run_id;
				cparams[1] = b_sidx;
				cparams[2] = b_cidx;
				cparams[3] = (cmd->type == SQL_COMMAND) ? "sql" : "meta";
				cparams[4] = cmd->first_line;
				cparams[5] = b_ccnt;
				cparams[6] = b_cfail;
				cparams[7] = b_cret;
				cparams[8] = b_cavg;
				cparams[9] = b_cstd;
				cparams[10] = b_cmin;
				cparams[11] = b_cmax;

				cres = PQexecParams(log_main_conn,
									"INSERT INTO pgbench_run_commands ("
									"  run_id, script_idx, cmd_idx, cmd_type, first_line, "
									"  cnt, failed, retries, latency_avg_ms, "
									"  latency_stddev_ms, latency_min_ms, latency_max_ms"
									") VALUES ("
									"  $1::int8, $2::int4, $3::int4, $4, $5, "
									"  $6::int8, $7::int8, $8::int8, $9::float8, "
									"  $10::float8, $11::float8, $12::float8"
									")",
									12, NULL, cparams, NULL, NULL, 0);
				PQclear(cres);
			}
		}
	}

	/* 5. Populate pgbench_run_recalls (overall, per-group, per-script) */
	insertScopeRecalls(log_main_conn, "overall", total);
	for (int g = 0; g < num_groups; g++)
	{
		char	   *gscope = psprintf("group:%s", client_groups[g].name);

		insertScopeRecalls(log_main_conn, gscope, &client_groups[g].stats);
		pg_free(gscope);
	}
	for (int i = 0; i < num_scripts; i++)
	{
		char	   *sscope = psprintf("script:%s", sql_script[i].desc);

		insertScopeRecalls(log_main_conn, sscope,
						   per_script_stats ? &sql_script[i].stats : total);
		pg_free(sscope);
	}

	/* 6. Populate pgbench_run_slas */
	for (int k = 0; k < num_slas; k++)
	{
		SlaRule    *rule = &sla_rules[k];
		char		b_run_id[32],
					b_sidx[32],
					b_thr[32],
					b_act[32],
					metric_buf[64];
		double		thr_val = (rule->metric == SLA_METRIC_LATENCY_PCT ||
							   rule->metric == SLA_METRIC_LATENCY_AVG ||
							   rule->metric == SLA_METRIC_LAG_PCT) ?
			(0.001 * rule->threshold_usec) : rule->threshold_val;
		const char *params[9];
		PGresult   *res;

		switch (rule->metric)
		{
			case SLA_METRIC_LATENCY_PCT:
				snprintf(metric_buf, sizeof(metric_buf), "p%g", rule->percentile);
				break;
			case SLA_METRIC_LATENCY_AVG:
				strcpy(metric_buf, "avg");
				break;
			case SLA_METRIC_LAG_PCT:
				snprintf(metric_buf, sizeof(metric_buf), "schedule_lag:p%g", rule->percentile);
				break;
			case SLA_METRIC_ONTIME_PCT:
				strcpy(metric_buf, "ontime_pct");
				break;
			case SLA_METRIC_RECALL_AVG:
				if (rule->named_metric != NULL)
					snprintf(metric_buf, sizeof(metric_buf), "%s:recall", rule->named_metric);
				else
					strcpy(metric_buf, "recall");
				break;
			case SLA_METRIC_RECALL_PCT:
				if (rule->named_metric != NULL)
					snprintf(metric_buf, sizeof(metric_buf), "%s:recall_p%g",
							 rule->named_metric, rule->percentile);
				else
					snprintf(metric_buf, sizeof(metric_buf), "recall_p%g", rule->percentile);
				break;
			case SLA_METRIC_TPS:
				strcpy(metric_buf, "tps");
				break;
			case SLA_METRIC_ERROR_RATE:
				strcpy(metric_buf, "error_rate");
				break;
			case SLA_METRIC_VALIDATION_FAILURES:
				strcpy(metric_buf, "validation_failures");
				break;
			case SLA_METRIC_VALIDATION_PCT:
			default:
				strcpy(metric_buf, "validation_pct");
				break;
		}

		snprintf(b_run_id, sizeof(b_run_id), INT64_FORMAT, current_run_id);
		snprintf(b_sidx, sizeof(b_sidx), "%d", k + 1);
		snprintf(b_thr, sizeof(b_thr), "%.6f", thr_val);
		snprintf(b_act, sizeof(b_act), "%.6f", rule->actual_val);

		params[0] = b_run_id;
		params[1] = b_sidx;
		params[2] = rule->raw_spec ? rule->raw_spec : "";
		params[3] = rule->target ? rule->target : "total";
		params[4] = metric_buf;
		params[5] = (rule->op == SLA_OP_GE) ? ">=" : "<=";
		params[6] = b_thr;
		params[7] = b_act;
		params[8] = rule->passed ? "t" : "f";

		res = PQexecParams(log_main_conn,
						   "INSERT INTO pgbench_run_slas ("
						   "  run_id, sla_idx, raw_spec, target_scope, metric, "
						   "  op, threshold_val, actual_val, passed"
						   ") VALUES ("
						   "  $1::int8, $2::int4, $3, $4, $5, "
						   "  $6, $7::float8, $8::float8, $9::bool"
						   ")",
						   9, NULL, params, NULL, NULL, 0);
		PQclear(res);
	}

	/* 7. Optional post-run index creation and ANALYZE (--log-db-index) */
	if (log_db_index)
	{
		execLogDbSQL(log_main_conn,
					 "CREATE INDEX IF NOT EXISTS pgbench_tx_log_run_ts_idx "
					 "  ON pgbench_tx_log (run_id, ts_usec);\n"
					 "CREATE INDEX IF NOT EXISTS pgbench_agg_log_run_ts_idx "
					 "  ON pgbench_agg_log (run_id, interval_start_sec);\n"
					 "CREATE INDEX IF NOT EXISTS pgbench_progress_log_run_ts_idx "
					 "  ON pgbench_progress_log (run_id, ts_epoch_sec);\n"
					 "ANALYZE pgbench_runs, pgbench_tx_log, pgbench_agg_log, pgbench_progress_log;",
					 false);
	}

	pg_free(report_json);
	PQfinish(log_main_conn);
	log_main_conn = NULL;
}
