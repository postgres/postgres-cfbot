/*
 * pgbench.c
 *
 * A simple benchmark program for PostgreSQL
 * Originally written by Tatsuo Ishii and enhanced by many contributors.
 *
 * src/bin/pgbench/pgbench.c
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#if defined(WIN32) && FD_SETSIZE < 1024
#error FD_SETSIZE needs to have been increased
#endif

#include "postgres_fe.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>		/* for getrlimit */



#include "catalog/pg_class_d.h"
#include "common/int.h"
#include "common/logging.h"
#include "common/pg_prng.h"
#include "common/string.h"
#include "common/username.h"
#include "fe_utils/cancel.h"
#include "fe_utils/conditional.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "port/pg_bitutils.h"
#include "portability/instr_time.h"

/* X/Open (XSI) requires <math.h> to provide M_PI, but core POSIX does not */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ERRCODE_T_R_SERIALIZATION_FAILURE  "40001"
#define ERRCODE_T_R_DEADLOCK_DETECTED  "40P01"
#define ERRCODE_UNDEFINED_TABLE  "42P01"

/*
 * Multi-platform thread implementations
 */

#ifdef WIN32
/* Use Windows threads */
#include <windows.h>
#define GETERRNO() (_dosmaperr(GetLastError()), errno)
#define THREAD_T HANDLE
#define THREAD_FUNC_RETURN_TYPE unsigned
#define THREAD_FUNC_RETURN return 0
#define THREAD_FUNC_CC __stdcall
#define THREAD_CREATE(handle, function, arg) \
	((*(handle) = (HANDLE) _beginthreadex(NULL, 0, (function), (arg), 0, NULL)) == 0 ? errno : 0)
#define THREAD_JOIN(handle) \
	(WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0 ? \
	GETERRNO() : CloseHandle(handle) ? 0 : GETERRNO())
#define THREAD_BARRIER_T SYNCHRONIZATION_BARRIER
#define THREAD_BARRIER_INIT(barrier, n) \
	(InitializeSynchronizationBarrier((barrier), (n), 0) ? 0 : GETERRNO())
#define THREAD_BARRIER_WAIT(barrier) \
	EnterSynchronizationBarrier((barrier), \
								SYNCHRONIZATION_BARRIER_FLAGS_BLOCK_ONLY)
#define THREAD_BARRIER_DESTROY(barrier)
#else
/* Use POSIX threads */
#include "port/pg_pthread.h"
#define THREAD_T pthread_t
#define THREAD_FUNC_RETURN_TYPE void *
#define THREAD_FUNC_RETURN return NULL
#define THREAD_FUNC_CC
#define THREAD_CREATE(handle, function, arg) \
	pthread_create((handle), NULL, (function), (arg))
#define THREAD_JOIN(handle) \
	pthread_join((handle), NULL)
#define THREAD_BARRIER_T pthread_barrier_t
#define THREAD_BARRIER_INIT(barrier, n) \
	pthread_barrier_init((barrier), NULL, (n))
#define THREAD_BARRIER_WAIT(barrier) pthread_barrier_wait((barrier))
#define THREAD_BARRIER_DESTROY(barrier) pthread_barrier_destroy((barrier))
#endif


/********************************************************************
 * some configurable parameters */

#define DEFAULT_INIT_STEPS "dtgvp"	/* default -I setting */
#define ALL_INIT_STEPS "dtgGvpf"	/* all possible steps */

#define LOG_STEP_SECONDS	5	/* seconds between log messages */
#define DEFAULT_NXACTS	10		/* default nxacts */

static int	nxacts = 0;			/* number of transactions per client */
static int	duration = 0;		/* duration in seconds */
static int64 end_time = 0;		/* when to stop in micro seconds, under -T */

/*
 * scaling factor. for example, scale = 10 will make 1000000 tuples in
 * pgbench_accounts table.
 */
static int	scale = 1;

/*
 * fillfactor. for example, fillfactor = 90 will use only 90 percent
 * space during inserts and leave 10 percent free.
 */
static int	fillfactor = 100;

/*
 * use unlogged tables?
 */
static bool unlogged_tables = false;

/*
 * log sampling rate (1.0 = log everything, 0.0 = option not given)
 */
static double sample_rate = 0.0;

/*
 * When threads are throttled to a given rate limit, this is the target delay
 * to reach that rate in usec.  0 is the default and means no throttling.
 */
static double throttle_delay = 0;

/*
 * Transactions which take longer than this limit (in usec) are counted as
 * late, and reported as such, although they are completed anyway. When
 * throttling is enabled, execution time slots that are more than this late
 * are skipped altogether, and counted separately.
 */
static int64 latency_limit = 0;

/*
 * tablespace selection
 */
static char *tablespace = NULL;
static char *index_tablespace = NULL;

/*
 * Number of "pgbench_accounts" partitions.  0 is the default and means no
 * partitioning.
 */
static int	partitions = 0;

/* partitioning strategy for "pgbench_accounts" */
typedef enum
{
	PART_NONE,					/* no partitioning */
	PART_RANGE,					/* range partitioning */
	PART_HASH,					/* hash partitioning */
} partition_method_t;

static partition_method_t partition_method = PART_NONE;
static const char *const PARTITION_METHOD[] = {"none", "range", "hash"};

/* random seed used to initialize base_random_sequence */
static int64 random_seed = -1;

/*
 * end of configurable parameters
 */

static bool use_log;			/* log transaction latencies to a file */
static bool use_quiet;			/* quiet logging onto stderr */
static int	agg_interval;		/* log aggregates instead of individual
								 * transactions */
static bool per_script_stats = false;	/* whether to collect stats per script */
static int	progress = 0;		/* thread progress report every this seconds */
static bool progress_timestamp = false; /* progress report with Unix time */
static int	nclients = 1;		/* number of clients */
static int	nthreads = 1;		/* number of threads */
static bool is_connect;			/* establish connection for each transaction */
static bool report_per_command = false; /* report per-command latencies,
										 * retries after errors and failures
										 * (errors without retrying) */
static int	main_pid;			/* main process id used in log filename */

/*
 * There are different types of restrictions for deciding that the current
 * transaction with a serialization/deadlock error can no longer be retried and
 * should be reported as failed:
 * - max_tries (--max-tries) can be used to limit the number of tries;
 * - latency_limit (-L) can be used to limit the total time of tries;
 * - duration (-T) can be used to limit the total benchmark time.
 *
 * They can be combined together, and you need to use at least one of them to
 * retry the transactions with serialization/deadlock errors. If none of them is
 * used, the default value of max_tries is 1 and such transactions will not be
 * retried.
 */

/*
 * We cannot retry a transaction after the serialization/deadlock error if its
 * number of tries reaches this maximum; if its value is zero, it is not used.
 */
static uint32 max_tries = 1;

static bool failures_detailed = false;	/* whether to group failures in
										 * reports or logs by basic types */

static const char *pghost = NULL;
static const char *pgport = NULL;
static const char *username = NULL;
static const char *dbName = NULL;
static char *logfile_prefix = NULL;
static const char *progname;
volatile sig_atomic_t timer_exceeded = false;	/* flag from signal
														 * handler */

/*
 * For displaying Unix epoch timestamps, as some time functions may have
 * another reference.
 */
static pg_time_usec_t epoch_shift;

typedef enum TStatus
{
	TSTATUS_IDLE,
	TSTATUS_IN_BLOCK,
	TSTATUS_CONN_ERROR,
	TSTATUS_OTHER_ERROR,
} TStatus;

/* Various random sequences are initialized from this one. */
static pg_prng_state base_random_sequence;

/* Synchronization barrier for start and connection */
static THREAD_BARRIER_T barrier;


/*
 * Thread state
 */
typedef struct TState
{
	int			tid;			/* thread id */
	THREAD_T	thread;			/* thread handle */
	CState	   *state;			/* array of CState */
	int			nstate;			/* length of state[] */

	/*
	 * Separate randomness for each thread. Each thread option uses its own
	 * random state to make all of them independent of each other and
	 * therefore deterministic at the thread level.
	 */
	pg_prng_state ts_choose_rs; /* random state for selecting a script */
	pg_prng_state ts_throttle_rs;	/* random state for transaction throttling */
	pg_prng_state ts_sample_rs; /* random state for log sampling */

	int64		throttle_trigger;	/* previous/next throttling (us) */
	FILE	   *logfile;		/* where to log, or NULL */

	/* per thread collected stats in microseconds */
	pg_time_usec_t create_time; /* thread creation time */
	pg_time_usec_t started_time;	/* thread is running */
	pg_time_usec_t bench_start; /* thread is benchmarking */
	pg_time_usec_t conn_duration;	/* cumulated connection and disconnection
									 * delays */

	StatsData	stats;
	int64		latency_late;	/* count executed but late transactions */
} TState;

/* runtime error control flags */
static bool verbose_errors = false; /* print verbose messages of all errors */

static bool exit_on_abort = false;	/* exit when any client is aborted */
static bool continue_on_error = false;	/* continue after errors */

/* Function prototypes */
static void doLog(TState *thread, CState *st,
				  StatsData *agg, bool tx_skipped, double latency, double lag);
static void processXactStats(TState *thread, CState *st, pg_time_usec_t *now,
							 bool tx_skipped, StatsData *agg);
static THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC threadRun(void *arg);
static void finishCon(CState *st);
static void setalarm(int seconds);

/* callback used to build rows for COPY during data loading */
typedef void (*initRowMethod) (PQExpBufferData *sql, int64 curr);

static char
get_table_relkind(PGconn *con, const char *table)
{
	PGresult   *res;
	char	   *val;
	char		relkind;
	const char *params[1] = {table};
	const char *sql =
		"SELECT relkind FROM pg_catalog.pg_class WHERE oid=$1::pg_catalog.regclass";

	res = PQexecParams(con, sql, 1, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(con));
		pg_log_error_detail("Query was: %s", sql);
		exit(1);
	}
	val = PQgetvalue(res, 0, 0);
	Assert(strlen(val) == 1);
	relkind = val[0];
	PQclear(res);

	return relkind;
}

static void
usage(void)
{
	printf("%s is a benchmarking tool for PostgreSQL.\n\n"
		   "Usage:\n"
		   "  %s [OPTION]... [DBNAME]\n"
		   "\nInitialization options:\n"
		   "  -i, --initialize         invokes initialization mode\n"
		   "  -I, --init-steps=[" ALL_INIT_STEPS "]+ (default \"" DEFAULT_INIT_STEPS "\")\n"
		   "                           run selected initialization steps, in the specified order\n"
		   "                           d: drop any existing pgbench tables\n"
		   "                           t: create the tables used by the standard pgbench scenario\n"
		   "                           g: generate data, client-side\n"
		   "                           G: generate data, server-side\n"
		   "                           v: invoke VACUUM on the standard tables\n"
		   "                           p: create primary key indexes on the standard tables\n"
		   "                           f: create foreign keys between the standard tables\n"
		   "  -F, --fillfactor=NUM     set fill factor\n"
		   "  -n, --no-vacuum          do not run VACUUM during initialization\n"
		   "  -q, --quiet              quiet logging (one message each 5 seconds)\n"
		   "  -s, --scale=NUM          scaling factor\n"
		   "  --foreign-keys           create foreign key constraints between tables\n"
		   "  --index-tablespace=TABLESPACE\n"
		   "                           create indexes in the specified tablespace\n"
		   "  --partition-method=(range|hash)\n"
		   "                           partition pgbench_accounts with this method (default: range)\n"
		   "  --partitions=NUM         partition pgbench_accounts into NUM parts (default: 0)\n"
		   "  --tablespace=TABLESPACE  create tables in the specified tablespace\n"
		   "  --unlogged-tables        create tables as unlogged tables\n"
		   "\nOptions to select what to run:\n"
		   "  -b, --builtin=NAME[@W]   add builtin script NAME weighted at W (default: 1)\n"
		   "                           (use \"-b list\" to list available scripts)\n"
		   "  -f, --file=FILENAME[@W]  add script FILENAME weighted at W (default: 1)\n"
		   "  -N, --skip-some-updates  skip updates of pgbench_tellers and pgbench_branches\n"
		   "                           (same as \"-b simple-update\")\n"
		   "  -S, --select-only        perform SELECT-only transactions\n"
		   "                           (same as \"-b select-only\")\n"
		   "\nBenchmarking options:\n"
		   "  -c, --client=NUM         number of concurrent database clients (default: 1)\n"
		   "  -C, --connect            establish new connection for each transaction\n"
		   "  -D, --define=VARNAME=VALUE\n"
		   "                           define variable for use by custom script\n"
		   "  -j, --jobs=NUM           number of threads (default: 1)\n"
		   "  -l, --log                write transaction times to log file\n"
		   "  -L, --latency-limit=NUM  count transactions lasting more than NUM ms as late\n"
		   "  -M, --protocol=simple|extended|prepared\n"
		   "                           protocol for submitting queries (default: simple)\n"
		   "  -n, --no-vacuum          do not run VACUUM before tests\n"
		   "  -P, --progress=NUM       show thread progress report every NUM seconds\n"
		   "  -r, --report-per-command report latencies, failures, and retries per command\n"
		   "  -R, --rate=NUM           target rate in transactions per second\n"
		   "  -s, --scale=NUM          report this scale factor in output\n"
		   "  -t, --transactions=NUM   number of transactions each client runs (default: 10)\n"
		   "  -T, --time=NUM           duration of benchmark test in seconds\n"
		   "  -v, --vacuum-all         vacuum all four standard tables before tests\n"
		   "  --aggregate-interval=NUM aggregate data over NUM seconds\n"
		   "  --continue-on-error      continue running after an SQL error\n"
		   "  --exit-on-abort          exit when any client is aborted\n"
		   "  --failures-detailed      report the failures grouped by basic types\n"
		   "  --log-prefix=PREFIX      prefix for transaction time log file\n"
		   "                           (default: \"pgbench_log\")\n"
		   "  --max-tries=NUM          max number of tries to run transaction (default: 1)\n"
		   "  --progress-timestamp     use Unix epoch timestamps for progress\n"
		   "  --random-seed=SEED       set random seed (\"time\", \"rand\", integer)\n"
		   "  --sampling-rate=NUM      fraction of transactions to log (e.g., 0.01 for 1%%)\n"
		   "  --show-script=NAME       show builtin script code, then exit\n"
		   "  --verbose-errors         print messages of all errors\n"
		   "\nCommon options:\n"
		   "  --debug                  print debugging output\n"
		   "  -d, --dbname=DBNAME      database name to connect to\n"
		   "  -h, --host=HOSTNAME      database server host or socket directory\n"
		   "  -p, --port=PORT          database server port number\n"
		   "  -U, --username=USERNAME  connect as specified database user\n"
		   "  -V, --version            output version information, then exit\n"
		   "  -?, --help               show this help, then exit\n"
		   "\n"
		   "Report bugs to <%s>.\n"
		   "%s home page: <%s>\n",
		   progname, progname, PACKAGE_BUGREPORT, PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Initialize a prng state struct.
 *
 * We derive the seed from base_random_sequence, which must be set up already.
 */
static void
initRandomState(pg_prng_state *state)
{
	pg_prng_seed(state, pg_prng_uint64(&base_random_sequence));
}


/*
 * random number generator: generate a value, such that the series of values
 * will approximate a Poisson distribution centered on the given value.
 *
 * Individual results are rounded to integers, though the center value need
 * not be one.
 */
static int64
getPoissonRand(pg_prng_state *state, double center)
{
	/*
	 * Use inverse transform sampling to generate a value > 0, such that the
	 * expected (i.e. average) value is the given argument.
	 */
	double		uniform;

	/* pg_prng_double value in [0, 1), uniform in (0, 1] */
	uniform = 1.0 - pg_prng_double(state);

	return (int64) (-log(uniform) * center + 0.5);
}

/* call PQexec() and exit() on failure */
static void
executeStatement(PGconn *con, const char *sql)
{
	PGresult   *res;

	res = PQexec(con, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(con));
		pg_log_error_detail("Query was: %s", sql);
		exit(1);
	}
	PQclear(res);
}

/* call PQexec() and complain, but without exiting, on failure */
static void
tryExecuteStatement(PGconn *con, const char *sql)
{
	PGresult   *res;

	res = PQexec(con, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("%s", PQerrorMessage(con));
		pg_log_error_detail("(ignoring this error and continuing anyway)");
	}
	PQclear(res);
}

/* set up a connection to the backend */
static PGconn *
doConnect(void)
{
	PGconn	   *conn;
	bool		new_pass;
	static char *password = NULL;

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
#define PARAMS_ARRAY_SIZE	7

		const char *keywords[PARAMS_ARRAY_SIZE];
		const char *values[PARAMS_ARRAY_SIZE];

		keywords[0] = "host";
		values[0] = pghost;
		keywords[1] = "port";
		values[1] = pgport;
		keywords[2] = "user";
		values[2] = username;
		keywords[3] = "password";
		values[3] = password;
		keywords[4] = "dbname";
		values[4] = dbName;
		keywords[5] = "fallback_application_name";
		values[5] = progname;
		keywords[6] = NULL;
		values[6] = NULL;

		new_pass = false;

		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
		{
			pg_log_error("connection to database \"%s\" failed", dbName);
			return NULL;
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			!password)
		{
			PQfinish(conn);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		pg_log_error("%s", PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}

	return conn;
}


static void
getQueryParams(Variables *variables, const Command *command,
			   const char **params)
{
	int			i;

	for (i = 0; i < command->argc - 1; i++)
		params[i] = getVariable(variables, command->argv[i + 1]);
}


/* Send a SQL command, using the chosen querymode */
static bool
sendCommand(CState *st, Command *command)
{
	int			r;

	if (querymode == QUERY_SIMPLE)
	{
		char	   *sql;

		sql = pg_strdup(command->argv[0]);
		sql = assignVariables(&st->variables, sql);

		pg_log_debug("client %d sending %s", st->id, sql);
		r = PQsendQuery(st->con, sql);
		pg_free(sql);
	}
	else if (querymode == QUERY_EXTENDED)
	{
		const char *sql = command->argv[0];
		const char *params[MAX_ARGS];

		getQueryParams(&st->variables, command, params);

		pg_log_debug("client %d sending %s", st->id, sql);
		r = PQsendQueryParams(st->con, sql, command->argc - 1,
							  NULL, params, NULL, NULL, 0);
	}
	else if (querymode == QUERY_PREPARED)
	{
		const char *params[MAX_ARGS];

		prepareCommand(st, st->command);
		getQueryParams(&st->variables, command, params);

		pg_log_debug("client %d sending %s", st->id, command->prepname);
		r = PQsendQueryPrepared(st->con, command->prepname, command->argc - 1,
								params, NULL, NULL, 0);
	}
	else						/* unknown sql mode */
		r = 0;

	if (r == 0)
	{
		pg_log_debug("client %d could not send %s", st->id, command->argv[0]);
		return false;
	}
	else
		return true;
}

/*
 * Read and discard all available results from the connection.
 */
static void
discardAvailableResults(CState *st)
{
	PGresult   *res = NULL;

	for (;;)
	{
		res = PQgetResult(st->con);

		/*
		 * Read and discard results until PQgetResult() returns NULL (no more
		 * results) or a connection failure is detected. If the pipeline
		 * status is PQ_PIPELINE_ABORTED, more results may still be available
		 * even after PQgetResult() returns NULL, so continue reading in that
		 * case.
		 */
		if ((res == NULL && PQpipelineStatus(st->con) != PQ_PIPELINE_ABORTED) ||
			PQstatus(st->con) == CONNECTION_BAD)
			break;

		PQclear(res);
	}
	PQclear(res);
}

/*
 * Determine the error status based on the connection status and error code.
 */
static EStatus
getSQLErrorStatus(CState *st, const char *sqlState)
{
	discardAvailableResults(st);
	if (PQstatus(st->con) == CONNECTION_BAD)
		return ESTATUS_CONN_ERROR;

	if (sqlState != NULL)
	{
		if (strcmp(sqlState, ERRCODE_T_R_SERIALIZATION_FAILURE) == 0)
			return ESTATUS_SERIALIZATION_ERROR;
		else if (strcmp(sqlState, ERRCODE_T_R_DEADLOCK_DETECTED) == 0)
			return ESTATUS_DEADLOCK_ERROR;
	}

	return ESTATUS_OTHER_SQL_ERROR;
}

/*
 * Returns true if this type of error can be retried.
 */
static bool
canRetryError(EStatus estatus)
{
	return (estatus == ESTATUS_SERIALIZATION_ERROR ||
			estatus == ESTATUS_DEADLOCK_ERROR);
}

/*
 * Returns true if --continue-on-error is specified and this error allows
 * processing to continue.
 */
static bool
canContinueOnError(EStatus estatus)
{
	return (continue_on_error &&
			estatus == ESTATUS_OTHER_SQL_ERROR);
}

/*
 * Process query response from the backend.
 *
 * If varprefix is not NULL, it's the variable name prefix where to store
 * the results of the *last* command (META_GSET) or *all* commands
 * (META_ASET).
 *
 * Returns true if everything is A-OK, false if any error occurs.
 */
static bool
readCommandResponse(CState *st, MetaCommand meta, char *varprefix)
{
	PGresult   *res;
	PGresult   *next_res;
	int			qrynum = 0;

	/*
	 * varprefix should be set only with \gset or \aset, and \endpipeline and
	 * SQL commands do not need it.
	 */
	Assert((meta == META_NONE && varprefix == NULL) ||
		   ((meta == META_ENDPIPELINE) && varprefix == NULL) ||
		   ((meta == META_GSET || meta == META_ASET) && varprefix != NULL));

	res = PQgetResult(st->con);

	while (res != NULL)
	{
		bool		is_last;

		/* peek at the next result to know whether the current is last */
		next_res = PQgetResult(st->con);
		is_last = (next_res == NULL);

		switch (PQresultStatus(res))
		{
			case PGRES_COMMAND_OK:	/* non-SELECT commands */
			case PGRES_EMPTY_QUERY: /* may be used for testing no-op overhead */
				if (is_last && meta == META_GSET)
				{
					pg_log_error("client %d script %d command %d query %d: expected one row, got %d",
								 st->id, st->use_file, st->command, qrynum, 0);
					st->estatus = ESTATUS_META_COMMAND_ERROR;
					goto error;
				}
				break;

			case PGRES_TUPLES_OK:
				if ((is_last && meta == META_GSET) || meta == META_ASET)
				{
					if (!processGSetResult(st, sql_script[st->use_file].commands[st->command],
										   res, is_last, qrynum))
						goto error;
				}
				/* otherwise the result is simply thrown away by PQclear below */
				break;

			case PGRES_PIPELINE_SYNC:
				pg_log_debug("client %d pipeline ending, ongoing syncs: %d",
							 st->id, st->num_syncs);
				st->num_syncs--;
				if (st->num_syncs == 0 && PQexitPipelineMode(st->con) != 1)
					pg_log_error("client %d failed to exit pipeline mode: %s", st->id,
								 PQresultErrorMessage(res));
				break;

			case PGRES_COPY_IN:
			case PGRES_COPY_OUT:
			case PGRES_COPY_BOTH:
				pg_log_error("COPY is not supported in pgbench, aborting");

				/*
				 * We need to exit the copy state.  Otherwise, PQgetResult()
				 * will always return an empty PGresult as an effect of
				 * getCopyResult(), leading to an infinite loop in the error
				 * cleanup done below.
				 */
				PQendcopy(st->con);
				goto error;

			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
				st->estatus = getSQLErrorStatus(st, PQresultErrorField(res,
																	   PG_DIAG_SQLSTATE));
				if (canRetryError(st->estatus) || canContinueOnError(st->estatus))
				{
					if (verbose_errors)
						commandError(st, PQresultErrorMessage(res));
					goto error;
				}
				pg_fallthrough;

			default:
				/* anything else is unexpected */
				pg_log_error("client %d script %d aborted in command %d query %d: %s",
							 st->id, st->use_file, st->command, qrynum,
							 PQresultErrorMessage(res));
				goto error;
		}

		PQclear(res);
		qrynum++;
		res = next_res;
	}

	if (qrynum == 0)
	{
		pg_log_error("client %d command %d: no results", st->id, st->command);
		return false;
	}

	return true;

error:
	PQclear(res);
	PQclear(next_res);
	discardAvailableResults(st);

	return false;
}



/*
 * Returns true if the error can be retried.
 */
static bool
doRetry(CState *st, pg_time_usec_t *now)
{
	Assert(st->estatus != ESTATUS_NO_ERROR);

	/* We can only retry serialization or deadlock errors. */
	if (!canRetryError(st->estatus))
		return false;

	/*
	 * We must have at least one option to limit the retrying of transactions
	 * that got an error.
	 */
	Assert(max_tries || latency_limit || duration > 0);

	/*
	 * We cannot retry the error if we have reached the maximum number of
	 * tries.
	 */
	if (max_tries && st->tries >= max_tries)
		return false;

	/*
	 * We cannot retry the error if we spent too much time on this
	 * transaction.
	 */
	if (latency_limit)
	{
		pg_time_now_lazy(now);
		if (*now - st->txn_scheduled > latency_limit)
			return false;
	}

	/*
	 * We cannot retry the error if the benchmark duration is over.
	 */
	if (timer_exceeded)
		return false;

	/* OK */
	return true;
}

/*
 * Read and discard results until the last sync point.
 */
static int
discardUntilSync(CState *st)
{
	bool		received_sync = false;

	/*
	 * Send a Sync message to ensure at least one PGRES_PIPELINE_SYNC is
	 * received and to avoid an infinite loop, since all earlier ones may have
	 * already been received.
	 */
	if (!PQpipelineSync(st->con))
	{
		pg_log_error("client %d aborted: failed to send a pipeline sync",
					 st->id);
		return 0;
	}

	/*
	 * Continue reading results until the last sync point, i.e., until
	 * reaching null just after PGRES_PIPELINE_SYNC.
	 */
	for (;;)
	{
		PGresult   *res = PQgetResult(st->con);

		if (PQstatus(st->con) == CONNECTION_BAD)
		{
			pg_log_error("client %d aborted while rolling back the transaction after an error; perhaps the backend died while processing",
						 st->id);
			PQclear(res);
			return 0;
		}

		if (PQresultStatus(res) == PGRES_PIPELINE_SYNC)
			received_sync = true;
		else if (received_sync && res == NULL)
		{
			/*
			 * Reset ongoing sync count to 0 since all PGRES_PIPELINE_SYNC
			 * results have been discarded.
			 */
			st->num_syncs = 0;
			break;
		}
		else
		{
			/*
			 * If a PGRES_PIPELINE_SYNC is followed by something other than
			 * PGRES_PIPELINE_SYNC or NULL, another PGRES_PIPELINE_SYNC will
			 * appear later. Reset received_sync to false to wait for it.
			 */
			received_sync = false;
		}
		PQclear(res);
	}

	/* exit pipeline */
	if (PQexitPipelineMode(st->con) != 1)
	{
		pg_log_error("client %d aborted: failed to exit pipeline mode for rolling back the failed transaction",
					 st->id);
		return 0;
	}
	return 1;
}

/*
 * Get the transaction status at the end of a command especially for
 * checking if we are in a (failed) transaction block.
 */
static TStatus
getTransactionStatus(PGconn *con)
{
	PGTransactionStatusType tx_status;

	tx_status = PQtransactionStatus(con);
	switch (tx_status)
	{
		case PQTRANS_IDLE:
			return TSTATUS_IDLE;
		case PQTRANS_INTRANS:
		case PQTRANS_INERROR:
			return TSTATUS_IN_BLOCK;
		case PQTRANS_UNKNOWN:
			/* PQTRANS_UNKNOWN is expected given a broken connection */
			if (PQstatus(con) == CONNECTION_BAD)
				return TSTATUS_CONN_ERROR;
			pg_fallthrough;
		case PQTRANS_ACTIVE:
		default:

			/*
			 * We cannot find out whether we are in a transaction block or
			 * not. Internal error which should never occur.
			 */
			pg_log_error("unexpected transaction status %d", tx_status);
			return TSTATUS_OTHER_ERROR;
	}

	/* not reached */
	Assert(false);
	return TSTATUS_OTHER_ERROR;
}

/*
 * Print verbose messages of an error
 */
static void
printVerboseErrorMessages(CState *st, pg_time_usec_t *now, bool is_retry)
{
	PQExpBufferData buf;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf, "client %d ", st->id);
	appendPQExpBufferStr(&buf, (is_retry ?
								"repeats the transaction after the error" :
								"ends the failed transaction"));
	appendPQExpBuffer(&buf, " (try %u", st->tries);

	/* Print max_tries if it is not unlimited. */
	if (max_tries)
		appendPQExpBuffer(&buf, "/%u", max_tries);

	/*
	 * If the latency limit is used, print a percentage of the current
	 * transaction latency from the latency limit.
	 */
	if (latency_limit)
	{
		pg_time_now_lazy(now);
		appendPQExpBuffer(&buf, ", %.3f%% of the maximum time of tries was used",
						  (100.0 * (*now - st->txn_scheduled) / latency_limit));
	}
	appendPQExpBufferStr(&buf, ")\n");

	pg_log_info("%s", buf.data);

	termPQExpBuffer(&buf);
}

/*
 * Advance the state machine of a connection.
 */
static void
advanceConnectionState(TState *thread, CState *st, StatsData *agg)
{

	/*
	 * gettimeofday() isn't free, so we get the current timestamp lazily the
	 * first time it's needed, and reuse the same value throughout this
	 * function after that.  This also ensures that e.g. the calculated
	 * latency reported in the log file and in the totals are the same. Zero
	 * means "not set yet".  Reset "now" when we execute shell commands or
	 * expressions, which might take a non-negligible amount of time, though.
	 */
	pg_time_usec_t now = 0;

	/*
	 * Loop in the state machine, until we have to wait for a result from the
	 * server or have to sleep for throttling or \sleep.
	 *
	 * Note: In the switch-statement below, 'break' will loop back here,
	 * meaning "continue in the state machine".  Return is used to return to
	 * the caller, giving the thread the opportunity to advance another
	 * client.
	 */
	for (;;)
	{
		Command    *command;

		switch (st->state)
		{
				/* Select transaction (script) to run.  */
			case CSTATE_CHOOSE_SCRIPT:
				st->use_file = chooseScript(&thread->ts_choose_rs);
				Assert(conditional_stack_empty(st->cstack));

				/* reset transaction variables to default values */
				st->estatus = ESTATUS_NO_ERROR;
				st->tries = 1;

				pg_log_debug("client %d executing script \"%s\"",
							 st->id, sql_script[st->use_file].desc);

				/*
				 * If time is over, we're done; otherwise, get ready to start
				 * a new transaction, or to get throttled if that's requested.
				 */
				st->state = timer_exceeded ? CSTATE_FINISHED :
					throttle_delay > 0 ? CSTATE_PREPARE_THROTTLE : CSTATE_START_TX;
				break;

				/* Start new transaction (script) */
			case CSTATE_START_TX:
				pg_time_now_lazy(&now);

				/* establish connection if needed, i.e. under --connect */
				if (st->con == NULL)
				{
					pg_time_usec_t start = now;

					if ((st->con = doConnect()) == NULL)
					{
						/*
						 * as the bench is already running, we do not abort
						 * the process
						 */
						pg_log_error("client %d aborted while establishing connection", st->id);
						st->state = CSTATE_ABORTED;
						break;
					}

					/* reset now after connection */
					now = pg_time_now();

					thread->conn_duration += now - start;

					/* Reset session-local state */
					pg_free(st->prepared);
					st->prepared = NULL;
				}

				/*
				 * It is the first try to run this transaction. Remember the
				 * random state: maybe it will get an error and we will need
				 * to run it again.
				 */
				st->random_state = st->cs_func_rs;

				/* record transaction start time */
				st->txn_begin = now;

				/*
				 * When not throttling, this is also the transaction's
				 * scheduled start time.
				 */
				if (!throttle_delay)
					st->txn_scheduled = now;

				/* Begin with the first command */
				st->state = CSTATE_START_COMMAND;
				st->command = 0;
				break;

				/*
				 * Handle throttling once per transaction by sleeping.
				 */
			case CSTATE_PREPARE_THROTTLE:

				/*
				 * Generate a delay such that the series of delays will
				 * approximate a Poisson distribution centered on the
				 * throttle_delay time.
				 *
				 * If transactions are too slow or a given wait is shorter
				 * than a transaction, the next transaction will start right
				 * away.
				 */
				Assert(throttle_delay > 0);

				thread->throttle_trigger +=
					getPoissonRand(&thread->ts_throttle_rs, throttle_delay);
				st->txn_scheduled = thread->throttle_trigger;

				/*
				 * If --latency-limit is used, and this slot is already late
				 * so that the transaction will miss the latency limit even if
				 * it completed immediately, skip this time slot and loop to
				 * reschedule.
				 */
				if (latency_limit)
				{
					pg_time_now_lazy(&now);

					if (thread->throttle_trigger < now - latency_limit)
					{
						processXactStats(thread, st, &now, true, agg);

						/*
						 * Finish client if -T or -t was exceeded.
						 *
						 * Stop counting skipped transactions under -T as soon
						 * as the timer is exceeded. Because otherwise it can
						 * take a very long time to count all of them
						 * especially when quite a lot of them happen with
						 * unrealistically high rate setting in -R, which
						 * would prevent pgbench from ending immediately.
						 * Because of this behavior, note that there is no
						 * guarantee that all skipped transactions are counted
						 * under -T though there is under -t. This is OK in
						 * practice because it's very unlikely to happen with
						 * realistic setting.
						 */
						if (timer_exceeded || (nxacts > 0 && st->cnt >= nxacts))
							st->state = CSTATE_FINISHED;

						/* Go back to top of loop with CSTATE_PREPARE_THROTTLE */
						break;
					}
				}

				/*
				 * stop client if next transaction is beyond pgbench end of
				 * execution; otherwise, throttle it.
				 */
				st->state = end_time > 0 && st->txn_scheduled > end_time ?
					CSTATE_FINISHED : CSTATE_THROTTLE;
				break;

				/*
				 * Wait until it's time to start next transaction.
				 */
			case CSTATE_THROTTLE:
				pg_time_now_lazy(&now);

				if (now < st->txn_scheduled)
					return;		/* still sleeping, nothing to do here */

				/* done sleeping, but don't start transaction if we're done */
				st->state = timer_exceeded ? CSTATE_FINISHED : CSTATE_START_TX;
				break;

				/*
				 * Send a command to server (or execute a meta-command)
				 */
			case CSTATE_START_COMMAND:
				command = sql_script[st->use_file].commands[st->command];

				/*
				 * Transition to script end processing if done, but close up
				 * shop if a pipeline is open at this point.
				 */
				if (command == NULL)
				{
					if (PQpipelineStatus(st->con) == PQ_PIPELINE_OFF)
						st->state = CSTATE_END_TX;
					else
					{
						pg_log_error("client %d aborted: end of script reached with pipeline open",
									 st->id);
						st->state = CSTATE_ABORTED;
					}

					break;
				}

				/* record begin time of next command, and initiate it */
				if (report_per_command)
				{
					pg_time_now_lazy(&now);
					st->stmt_begin = now;
				}

				/* Execute the command */
				if (command->type == SQL_COMMAND)
				{
					/* disallow \aset and \gset in pipeline mode */
					if (PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
					{
						if (command->meta == META_GSET)
						{
							commandFailed(st, "gset", "\\gset is not allowed in pipeline mode");
							st->state = CSTATE_ABORTED;
							break;
						}
						else if (command->meta == META_ASET)
						{
							commandFailed(st, "aset", "\\aset is not allowed in pipeline mode");
							st->state = CSTATE_ABORTED;
							break;
						}
					}

					if (!sendCommand(st, command))
					{
						commandFailed(st, "SQL", "SQL command send failed");
						st->state = CSTATE_ABORTED;
					}
					else
					{
						/* Wait for results, unless in pipeline mode */
						if (PQpipelineStatus(st->con) == PQ_PIPELINE_OFF)
							st->state = CSTATE_WAIT_RESULT;
						else
							st->state = CSTATE_END_COMMAND;
					}
				}
				else if (command->type == META_COMMAND)
				{
					/*-----
					 * Possible state changes when executing meta commands:
					 * - on errors CSTATE_ABORTED
					 * - on sleep CSTATE_SLEEP
					 * - else CSTATE_END_COMMAND
					 */
					st->state = executeMetaCommand(st, &now);
					if (st->state == CSTATE_ABORTED)
						st->estatus = ESTATUS_META_COMMAND_ERROR;
				}

				/*
				 * We're now waiting for an SQL command to complete, or
				 * finished processing a metacommand, or need to sleep, or
				 * something bad happened.
				 */
				Assert(st->state == CSTATE_WAIT_RESULT ||
					   st->state == CSTATE_END_COMMAND ||
					   st->state == CSTATE_SLEEP ||
					   st->state == CSTATE_ABORTED);
				break;

				/*
				 * non executed conditional branch
				 */
			case CSTATE_SKIP_COMMAND:
				skipConditionalCommands(st);
				break;

				/*
				 * Wait for the current SQL command to complete
				 */
			case CSTATE_WAIT_RESULT:
				pg_log_debug("client %d receiving", st->id);

				/*
				 * Only check for new network data if we processed all data
				 * fetched prior. Otherwise we end up doing a syscall for each
				 * individual pipelined query, which has a measurable
				 * performance impact.
				 */
				if (PQisBusy(st->con) && !PQconsumeInput(st->con))
				{
					/* there's something wrong */
					commandFailed(st, "SQL", "perhaps the backend died while processing");
					st->state = CSTATE_ABORTED;
					break;
				}
				if (PQisBusy(st->con))
					return;		/* don't have the whole result yet */

				/* store or discard the query results */
				if (readCommandResponse(st,
										sql_script[st->use_file].commands[st->command]->meta,
										sql_script[st->use_file].commands[st->command]->varprefix))
				{
					/*
					 * outside of pipeline mode: stop reading results.
					 * pipeline mode: continue reading results until an
					 * end-of-pipeline response.
					 */
					if (PQpipelineStatus(st->con) != PQ_PIPELINE_ON)
						st->state = CSTATE_END_COMMAND;
				}
				else if (canRetryError(st->estatus) || canContinueOnError(st->estatus))
					st->state = CSTATE_ERROR;
				else
					st->state = CSTATE_ABORTED;
				break;

				/*
				 * Wait until sleep is done. This state is entered after a
				 * \sleep metacommand. The behavior is similar to
				 * CSTATE_THROTTLE, but proceeds to CSTATE_START_COMMAND
				 * instead of CSTATE_START_TX.
				 */
			case CSTATE_SLEEP:
				pg_time_now_lazy(&now);
				if (now < st->sleep_until)
					return;		/* still sleeping, nothing to do here */
				/* Else done sleeping. */
				st->state = CSTATE_END_COMMAND;
				break;

				/*
				 * End of command: record stats and proceed to next command.
				 */
			case CSTATE_END_COMMAND:

				/*
				 * command completed: accumulate per-command execution times
				 * in thread-local data structure, if per-command latencies
				 * are requested.
				 */
				if (report_per_command)
				{
					pg_time_now_lazy(&now);

					command = sql_script[st->use_file].commands[st->command];
					/* XXX could use a mutex here, but we choose not to */
					addToSimpleStats(&command->stats,
									 PG_TIME_GET_DOUBLE(now - st->stmt_begin));
				}

				/* Go ahead with next command, to be executed or skipped */
				st->command++;
				st->state = conditional_active(st->cstack) ?
					CSTATE_START_COMMAND : CSTATE_SKIP_COMMAND;
				break;

				/*
				 * Clean up after an error.
				 */
			case CSTATE_ERROR:
				{
					TStatus		tstatus;

					Assert(st->estatus != ESTATUS_NO_ERROR);

					/* Clear the conditional stack */
					conditional_stack_reset(st->cstack);

					/* Read and discard until a sync point in pipeline mode */
					if (PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
					{
						if (!discardUntilSync(st))
						{
							st->state = CSTATE_ABORTED;
							break;
						}
					}

					/*
					 * Check if we have a (failed) transaction block or not,
					 * and roll it back if any.
					 */
					tstatus = getTransactionStatus(st->con);
					if (tstatus == TSTATUS_IN_BLOCK)
					{
						/* Try to rollback a (failed) transaction block. */
						if (!PQsendQuery(st->con, "ROLLBACK"))
						{
							pg_log_error("client %d aborted: failed to send sql command for rolling back the failed transaction",
										 st->id);
							st->state = CSTATE_ABORTED;
						}
						else
							st->state = CSTATE_WAIT_ROLLBACK_RESULT;
					}
					else if (tstatus == TSTATUS_IDLE)
					{
						/*
						 * If time is over, we're done; otherwise, check if we
						 * can retry the error.
						 */
						st->state = timer_exceeded ? CSTATE_FINISHED :
							doRetry(st, &now) ? CSTATE_RETRY : CSTATE_FAILURE;
					}
					else
					{
						if (tstatus == TSTATUS_CONN_ERROR)
							pg_log_error("perhaps the backend died while processing");

						pg_log_error("client %d aborted while receiving the transaction status", st->id);
						st->state = CSTATE_ABORTED;
					}
					break;
				}

				/*
				 * Wait for the rollback command to complete
				 */
			case CSTATE_WAIT_ROLLBACK_RESULT:
				{
					PGresult   *res;

					pg_log_debug("client %d receiving", st->id);
					if (!PQconsumeInput(st->con))
					{
						pg_log_error("client %d aborted while rolling back the transaction after an error; perhaps the backend died while processing",
									 st->id);
						st->state = CSTATE_ABORTED;
						break;
					}
					if (PQisBusy(st->con))
						return; /* don't have the whole result yet */

					/*
					 * Read and discard the query result;
					 */
					res = PQgetResult(st->con);
					switch (PQresultStatus(res))
					{
						case PGRES_COMMAND_OK:
							/* OK */
							PQclear(res);
							/* null must be returned */
							res = PQgetResult(st->con);
							Assert(res == NULL);

							/*
							 * If time is over, we're done; otherwise, check
							 * if we can retry the error.
							 */
							st->state = timer_exceeded ? CSTATE_FINISHED :
								doRetry(st, &now) ? CSTATE_RETRY : CSTATE_FAILURE;
							break;
						default:
							pg_log_error("client %d aborted while rolling back the transaction after an error; %s",
										 st->id, PQerrorMessage(st->con));
							PQclear(res);
							st->state = CSTATE_ABORTED;
							break;
					}
					break;
				}

				/*
				 * Retry the transaction after an error.
				 */
			case CSTATE_RETRY:
				command = sql_script[st->use_file].commands[st->command];

				/*
				 * Inform that the transaction will be retried after the
				 * error.
				 */
				if (verbose_errors)
					printVerboseErrorMessages(st, &now, true);

				/* Count tries and retries */
				st->tries++;
				command->retries++;

				/*
				 * Reset the random state as they were at the beginning of the
				 * transaction.
				 */
				st->cs_func_rs = st->random_state;

				/* Process the first transaction command. */
				st->command = 0;
				st->estatus = ESTATUS_NO_ERROR;
				st->state = CSTATE_START_COMMAND;
				break;

				/*
				 * Record a failed transaction.
				 */
			case CSTATE_FAILURE:
				command = sql_script[st->use_file].commands[st->command];

				/* Accumulate the failure. */
				command->failures++;

				/*
				 * Inform that the failed transaction will not be retried.
				 */
				if (verbose_errors)
					printVerboseErrorMessages(st, &now, false);

				/* End the failed transaction. */
				st->state = CSTATE_END_TX;
				break;

				/*
				 * End of transaction (end of script, really).
				 */
			case CSTATE_END_TX:
				{
					TStatus		tstatus;

					/* transaction finished: calculate latency and do log */
					processXactStats(thread, st, &now, false, agg);

					/*
					 * missing \endif... cannot happen if CheckConditional was
					 * okay
					 */
					Assert(conditional_stack_empty(st->cstack));

					/*
					 * We must complete all the transaction blocks that were
					 * started in this script.
					 */
					tstatus = getTransactionStatus(st->con);
					if (tstatus == TSTATUS_IN_BLOCK)
					{
						pg_log_error("client %d aborted: end of script reached without completing the last transaction",
									 st->id);
						st->state = CSTATE_ABORTED;
						break;
					}
					else if (tstatus != TSTATUS_IDLE)
					{
						if (tstatus == TSTATUS_CONN_ERROR)
							pg_log_error("perhaps the backend died while processing");

						pg_log_error("client %d aborted while receiving the transaction status", st->id);
						st->state = CSTATE_ABORTED;
						break;
					}

					if (is_connect)
					{
						pg_time_usec_t start = now;

						pg_time_now_lazy(&start);
						finishCon(st);
						now = pg_time_now();
						thread->conn_duration += now - start;
					}

					if ((st->cnt >= nxacts && duration <= 0) || timer_exceeded)
					{
						/* script completed */
						st->state = CSTATE_FINISHED;
						break;
					}

					/* next transaction (script) */
					st->state = CSTATE_CHOOSE_SCRIPT;

					/*
					 * Ensure that we always return on this point, so as to
					 * avoid an infinite loop if the script only contains meta
					 * commands.
					 */
					return;
				}

				/*
				 * Final states.  Close the connection if it's still open.
				 */
			case CSTATE_ABORTED:
			case CSTATE_FINISHED:

				/*
				 * Don't measure the disconnection delays here even if in
				 * CSTATE_FINISHED and -C/--connect option is specified.
				 * Because in this case all the connections that this thread
				 * established are closed at the end of transactions and the
				 * disconnection delays should have already been measured at
				 * that moment.
				 *
				 * In CSTATE_ABORTED state, the measurement is no longer
				 * necessary because we cannot report complete results anyways
				 * in this case.
				 */
				finishCon(st);
				return;
		}
	}
}

/*
 * Print log entry after completing one transaction.
 *
 * We print Unix-epoch timestamps in the log, so that entries can be
 * correlated against other logs.
 *
 * XXX We could obtain the time from the caller and just shift it here, to
 * avoid the cost of an extra call to pg_time_now().
 */
static void
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
		accumStats(agg, tx_skipped, latency, lag, st->estatus, st->tries, throttle_delay > 0);
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
					st->id, st->cnt, getResultString(tx_skipped, st->estatus, failures_detailed),
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
static void
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
	accumStats(&thread->stats, tx_skipped, latency, lag, st->estatus, st->tries, throttle_delay > 0);

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
				   st->estatus, st->tries, throttle_delay > 0);
}


/* discard connections */
static void
disconnect_all(CState *state, int length)
{
	int			i;

	for (i = 0; i < length; i++)
		finishCon(&state[i]);
}

/*
 * Remove old pgbench tables, if any exist
 */
static void
initDropTables(PGconn *con)
{
	fprintf(stderr, "dropping old tables...\n");

	/*
	 * We drop all the tables in one command, so that whether there are
	 * foreign key dependencies or not doesn't matter.
	 */
	executeStatement(con, "drop table if exists "
					 "pgbench_accounts, "
					 "pgbench_branches, "
					 "pgbench_history, "
					 "pgbench_tellers");
}

/*
 * Create "pgbench_accounts" partitions if needed.
 *
 * This is the larger table of pgbench default tpc-b like schema
 * with a known size, so we choose to partition it.
 */
static void
createPartitions(PGconn *con)
{
	PQExpBufferData query;

	/* we must have to create some partitions */
	Assert(partitions > 0);

	fprintf(stderr, "creating %d partitions...\n", partitions);

	initPQExpBuffer(&query);

	for (int p = 1; p <= partitions; p++)
	{
		if (partition_method == PART_RANGE)
		{
			int64		part_size = (naccounts * (int64) scale + partitions - 1) / partitions;

			printfPQExpBuffer(&query,
							  "create%s table pgbench_accounts_%d\n"
							  "  partition of pgbench_accounts\n"
							  "  for values from (",
							  unlogged_tables ? " unlogged" : "", p);

			/*
			 * For RANGE, we use open-ended partitions at the beginning and
			 * end to allow any valid value for the primary key.  Although the
			 * actual minimum and maximum values can be derived from the
			 * scale, it is more generic and the performance is better.
			 */
			if (p == 1)
				appendPQExpBufferStr(&query, "minvalue");
			else
				appendPQExpBuffer(&query, INT64_FORMAT, (p - 1) * part_size + 1);

			appendPQExpBufferStr(&query, ") to (");

			if (p < partitions)
				appendPQExpBuffer(&query, INT64_FORMAT, p * part_size + 1);
			else
				appendPQExpBufferStr(&query, "maxvalue");

			appendPQExpBufferChar(&query, ')');
		}
		else if (partition_method == PART_HASH)
			printfPQExpBuffer(&query,
							  "create%s table pgbench_accounts_%d\n"
							  "  partition of pgbench_accounts\n"
							  "  for values with (modulus %d, remainder %d)",
							  unlogged_tables ? " unlogged" : "", p,
							  partitions, p - 1);
		else					/* cannot get there */
			Assert(0);

		/*
		 * Per ddlinfo in initCreateTables, fillfactor is needed on table
		 * pgbench_accounts.
		 */
		appendPQExpBuffer(&query, " with (fillfactor=%d)", fillfactor);

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);
}

/*
 * Create pgbench's standard tables
 */
static void
initCreateTables(PGconn *con)
{
	/*
	 * Note: TPC-B requires at least 100 bytes per row, and the "filler"
	 * fields in these table declarations were intended to comply with that.
	 * The pgbench_accounts table complies with that because the "filler"
	 * column is set to blank-padded empty string. But for all other tables
	 * the columns default to NULL and so don't actually take any space.  We
	 * could fix that by giving them non-null default values.  However, that
	 * would completely break comparability of pgbench results with prior
	 * versions. Since pgbench has never pretended to be fully TPC-B compliant
	 * anyway, we stick with the historical behavior.
	 */
	struct ddlinfo
	{
		const char *table;		/* table name */
		const char *smcols;		/* column decls if accountIDs are 32 bits */
		const char *bigcols;	/* column decls if accountIDs are 64 bits */
		int			declare_fillfactor;
	};
	static const struct ddlinfo DDLs[] = {
		{
			"pgbench_history",
			"tid int,bid int,aid    int,delta int,mtime timestamp,filler char(22)",
			"tid int,bid int,aid bigint,delta int,mtime timestamp,filler char(22)",
			0
		},
		{
			"pgbench_tellers",
			"tid int not null,bid int,tbalance int,filler char(84)",
			"tid int not null,bid int,tbalance int,filler char(84)",
			1
		},
		{
			"pgbench_accounts",
			"aid    int not null,bid int,abalance int,filler char(84)",
			"aid bigint not null,bid int,abalance int,filler char(84)",
			1
		},
		{
			"pgbench_branches",
			"bid int not null,bbalance int,filler char(88)",
			"bid int not null,bbalance int,filler char(88)",
			1
		}
	};
	PQExpBufferData query;

	fprintf(stderr, "creating tables...\n");

	initPQExpBuffer(&query);

	for (size_t i = 0; i < lengthof(DDLs); i++)
	{
		const struct ddlinfo *ddl = &DDLs[i];

		/* Construct new create table statement. */
		printfPQExpBuffer(&query, "create%s table %s(%s)",
						  (unlogged_tables && partition_method == PART_NONE) ? " unlogged" : "",
						  ddl->table,
						  (scale >= SCALE_32BIT_THRESHOLD) ? ddl->bigcols : ddl->smcols);

		/* Partition pgbench_accounts table */
		if (partition_method != PART_NONE && strcmp(ddl->table, "pgbench_accounts") == 0)
			appendPQExpBuffer(&query,
							  " partition by %s (aid)", PARTITION_METHOD[partition_method]);
		else if (ddl->declare_fillfactor)
		{
			/* fillfactor is only expected on actual tables */
			appendPQExpBuffer(&query, " with (fillfactor=%d)", fillfactor);
		}

		if (tablespace != NULL)
		{
			char	   *escape_tablespace;

			escape_tablespace = PQescapeIdentifier(con, tablespace, strlen(tablespace));
			appendPQExpBuffer(&query, " tablespace %s", escape_tablespace);
			PQfreemem(escape_tablespace);
		}

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);

	if (partition_method != PART_NONE)
		createPartitions(con);
}

/*
 * Truncate away any old data, in one command in case there are foreign keys
 */
static void
initTruncateTables(PGconn *con)
{
	executeStatement(con, "truncate table "
					 "pgbench_accounts, "
					 "pgbench_branches, "
					 "pgbench_history, "
					 "pgbench_tellers");
}

static void
initBranch(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column uses NULL */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t0\t\\N\n",
					  curr + 1);
}

static void
initTeller(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column uses NULL */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t" INT64_FORMAT "\t0\t\\N\n",
					  curr + 1, curr / ntellers + 1);
}

static void
initAccount(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column defaults to blank padded empty string */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t" INT64_FORMAT "\t0\t\n",
					  curr + 1, curr / naccounts + 1);
}

static void
initPopulateTable(PGconn *con, const char *table, int64 base,
				  initRowMethod init_row)
{
	int			n;
	int64		k;
	int			chars = 0;
	int			prev_chars = 0;
	PGresult   *res;
	PQExpBufferData sql;
	char		copy_statement[256];
	const char *copy_statement_fmt = "copy %s from stdin";
	int64		total = base * scale;

	/* used to track elapsed time and estimate of the remaining time */
	pg_time_usec_t start;
	int			log_interval = 1;

	/* Stay on the same line if reporting to a terminal */
	char		eol = isatty(fileno(stderr)) ? '\r' : '\n';

	initPQExpBuffer(&sql);

	/* Use COPY with FREEZE on v14 and later for all ordinary tables */
	if ((PQserverVersion(con) >= 140000) &&
		get_table_relkind(con, table) == RELKIND_RELATION)
		copy_statement_fmt = "copy %s from stdin with (freeze on)";


	n = pg_snprintf(copy_statement, sizeof(copy_statement), copy_statement_fmt, table);
	if (n >= sizeof(copy_statement))
		pg_fatal("invalid buffer size: must be at least %d characters long", n);
	else if (n == -1)
		pg_fatal("invalid format string");

	res = PQexec(con, copy_statement);

	if (PQresultStatus(res) != PGRES_COPY_IN)
		pg_fatal("unexpected copy in result: %s", PQerrorMessage(con));
	PQclear(res);

	start = pg_time_now();

	for (k = 0; k < total; k++)
	{
		int64		j = k + 1;

		init_row(&sql, k);
		if (PQputline(con, sql.data))
			pg_fatal("PQputline failed");

		if (CancelRequested)
			break;

		/*
		 * If we want to stick with the original logging, print a message each
		 * 100k inserted rows.
		 */
		if ((!use_quiet) && (j % 100000 == 0))
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);
			double		remaining_sec = ((double) total - j) * elapsed_sec / j;

			chars = fprintf(stderr, INT64_FORMAT " of " INT64_FORMAT " tuples (%d%%) of %s done (elapsed %.2f s, remaining %.2f s)",
							j, total,
							(int) ((j * 100) / total),
							table, elapsed_sec, remaining_sec);

			/*
			 * If the previous progress message is longer than the current
			 * one, add spaces to the current line to fully overwrite any
			 * remaining characters from the previous message.
			 */
			if (prev_chars > chars)
				fprintf(stderr, "%*c", prev_chars - chars, ' ');
			fputc(eol, stderr);
			prev_chars = chars;
		}
		/* let's not call the timing for each row, but only each 100 rows */
		else if (use_quiet && (j % 100 == 0))
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);
			double		remaining_sec = ((double) total - j) * elapsed_sec / j;

			/* have we reached the next interval (or end)? */
			if ((j == total) || (elapsed_sec >= log_interval * LOG_STEP_SECONDS))
			{
				chars = fprintf(stderr, INT64_FORMAT " of " INT64_FORMAT " tuples (%d%%) of %s done (elapsed %.2f s, remaining %.2f s)",
								j, total,
								(int) ((j * 100) / total),
								table, elapsed_sec, remaining_sec);

				/*
				 * If the previous progress message is longer than the current
				 * one, add spaces to the current line to fully overwrite any
				 * remaining characters from the previous message.
				 */
				if (prev_chars > chars)
					fprintf(stderr, "%*c", prev_chars - chars, ' ');
				fputc(eol, stderr);
				prev_chars = chars;

				/* skip to the next interval */
				log_interval = (int) ceil(elapsed_sec / LOG_STEP_SECONDS);
			}
		}
	}

	if (chars != 0 && eol != '\n')
		fprintf(stderr, "%*c\r", chars, ' ');	/* Clear the current line */

	if (PQputline(con, "\\.\n"))
		pg_fatal("very last PQputline failed");
	if (PQendcopy(con))
		pg_fatal("PQendcopy failed");

	termPQExpBuffer(&sql);
}

/*
 * Fill the standard tables with some data generated and sent from the client.
 *
 * The filler column is NULL in pgbench_branches and pgbench_tellers, and is
 * a blank-padded string in pgbench_accounts.
 */
static void
initGenerateDataClientSide(PGconn *con)
{
	fprintf(stderr, "generating data (client-side)...\n");

	/*
	 * we do all of this in one transaction to enable the backend's
	 * data-loading optimizations
	 */
	executeStatement(con, "begin");

	/* truncate away any old data */
	initTruncateTables(con);

	/*
	 * fill branches, tellers, accounts in that order in case foreign keys
	 * already exist
	 */
	initPopulateTable(con, "pgbench_branches", nbranches, initBranch);
	initPopulateTable(con, "pgbench_tellers", ntellers, initTeller);
	initPopulateTable(con, "pgbench_accounts", naccounts, initAccount);

	executeStatement(con, "commit");
}

/*
 * Fill the standard tables with some data generated on the server
 *
 * As already the case with the client-side data generation, the filler
 * column defaults to NULL in pgbench_branches and pgbench_tellers,
 * and is a blank-padded string in pgbench_accounts.
 */
static void
initGenerateDataServerSide(PGconn *con)
{
	PQExpBufferData sql;

	fprintf(stderr, "generating data (server-side)...\n");

	/*
	 * we do all of this in one transaction to enable the backend's
	 * data-loading optimizations
	 */
	executeStatement(con, "begin");

	/* truncate away any old data */
	initTruncateTables(con);

	initPQExpBuffer(&sql);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_branches(bid,bbalance) "
					  "select bid, 0 "
					  "from generate_series(1, %d) as bid", nbranches * scale);
	executeStatement(con, sql.data);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_tellers(tid,bid,tbalance) "
					  "select tid, (tid - 1) / %d + 1, 0 "
					  "from generate_series(1, %d) as tid", ntellers, ntellers * scale);
	executeStatement(con, sql.data);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_accounts(aid,bid,abalance,filler) "
					  "select aid, (aid - 1) / %d + 1, 0, '' "
					  "from generate_series(1, " INT64_FORMAT ") as aid",
					  naccounts, (int64) naccounts * scale);
	executeStatement(con, sql.data);

	termPQExpBuffer(&sql);

	executeStatement(con, "commit");
}

/*
 * Invoke vacuum on the standard tables
 */
static void
initVacuum(PGconn *con)
{
	fprintf(stderr, "vacuuming...\n");
	executeStatement(con, "vacuum analyze pgbench_branches");
	executeStatement(con, "vacuum analyze pgbench_tellers");
	executeStatement(con, "vacuum analyze pgbench_accounts");
	executeStatement(con, "vacuum analyze pgbench_history");
}

/*
 * Create primary keys on the standard tables
 */
static void
initCreatePKeys(PGconn *con)
{
	static const char *const DDLINDEXes[] = {
		"alter table pgbench_branches add primary key (bid)",
		"alter table pgbench_tellers add primary key (tid)",
		"alter table pgbench_accounts add primary key (aid)"
	};
	PQExpBufferData query;

	fprintf(stderr, "creating primary keys...\n");
	initPQExpBuffer(&query);

	for (size_t i = 0; i < lengthof(DDLINDEXes); i++)
	{
		resetPQExpBuffer(&query);
		appendPQExpBufferStr(&query, DDLINDEXes[i]);

		if (index_tablespace != NULL)
		{
			char	   *escape_tablespace;

			escape_tablespace = PQescapeIdentifier(con, index_tablespace,
												   strlen(index_tablespace));
			appendPQExpBuffer(&query, " using index tablespace %s", escape_tablespace);
			PQfreemem(escape_tablespace);
		}

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);
}

/*
 * Create foreign key constraints between the standard tables
 */
static void
initCreateFKeys(PGconn *con)
{
	static const char *const DDLKEYs[] = {
		"alter table pgbench_tellers add constraint pgbench_tellers_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_accounts add constraint pgbench_accounts_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_history add constraint pgbench_history_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_history add constraint pgbench_history_tid_fkey foreign key (tid) references pgbench_tellers",
		"alter table pgbench_history add constraint pgbench_history_aid_fkey foreign key (aid) references pgbench_accounts"
	};

	fprintf(stderr, "creating foreign keys...\n");
	for (size_t i = 0; i < lengthof(DDLKEYs); i++)
	{
		executeStatement(con, DDLKEYs[i]);
	}
}

/*
 * Validate an initialization-steps string
 *
 * (We could just leave it to runInitSteps() to fail if there are wrong
 * characters, but since initialization can take awhile, it seems friendlier
 * to check during option parsing.)
 */
static void
checkInitSteps(const char *initialize_steps)
{
	if (initialize_steps[0] == '\0')
		pg_fatal("no initialization steps specified");

	for (const char *step = initialize_steps; *step != '\0'; step++)
	{
		if (strchr(ALL_INIT_STEPS " ", *step) == NULL)
		{
			pg_log_error("unrecognized initialization step \"%c\"", *step);
			pg_log_error_detail("Allowed step characters are: \"" ALL_INIT_STEPS "\".");
			exit(1);
		}
	}
}

/*
 * Invoke each initialization step in the given string
 */
static void
runInitSteps(const char *initialize_steps)
{
	PQExpBufferData stats;
	PGconn	   *con;
	const char *step;
	double		run_time = 0.0;
	bool		first = true;

	initPQExpBuffer(&stats);

	if ((con = doConnect()) == NULL)
		pg_fatal("could not create connection for initialization");

	setup_cancel_handler(NULL);
	SetCancelConn(con);

	for (step = initialize_steps; *step != '\0'; step++)
	{
		char	   *op = NULL;
		pg_time_usec_t start = pg_time_now();

		switch (*step)
		{
			case 'd':
				op = "drop tables";
				initDropTables(con);
				break;
			case 't':
				op = "create tables";
				initCreateTables(con);
				break;
			case 'g':
				op = "client-side generate";
				initGenerateDataClientSide(con);
				break;
			case 'G':
				op = "server-side generate";
				initGenerateDataServerSide(con);
				break;
			case 'v':
				op = "vacuum";
				initVacuum(con);
				break;
			case 'p':
				op = "primary keys";
				initCreatePKeys(con);
				break;
			case 'f':
				op = "foreign keys";
				initCreateFKeys(con);
				break;
			case ' ':
				break;			/* ignore */
			default:
				pg_log_error("unrecognized initialization step \"%c\"", *step);
				PQfinish(con);
				exit(1);
		}

		if (op != NULL)
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);

			if (!first)
				appendPQExpBufferStr(&stats, ", ");
			else
				first = false;

			appendPQExpBuffer(&stats, "%s %.2f s", op, elapsed_sec);

			run_time += elapsed_sec;
		}
	}

	fprintf(stderr, "done in %.2f s (%s).\n", run_time, stats.data);
	ResetCancelConn();
	PQfinish(con);
	termPQExpBuffer(&stats);
}

/*
 * Extract pgbench table information into global variables scale,
 * partition_method and partitions.
 */
static void
GetTableInfo(PGconn *con, bool scale_given)
{
	PGresult   *res;

	/*
	 * get the scaling factor that should be same as count(*) from
	 * pgbench_branches if this is not a custom query
	 */
	res = PQexec(con, "select count(*) from pgbench_branches");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		char	   *sqlState = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		pg_log_error("could not count number of branches: %s", PQerrorMessage(con));

		if (sqlState && strcmp(sqlState, ERRCODE_UNDEFINED_TABLE) == 0)
			pg_log_error_hint("Perhaps you need to do initialization (\"pgbench -i\") in database \"%s\".",
							  PQdb(con));

		exit(1);
	}
	scale = atoi(PQgetvalue(res, 0, 0));
	if (scale < 0)
		pg_fatal("invalid count(*) from pgbench_branches: \"%s\"",
				 PQgetvalue(res, 0, 0));
	PQclear(res);

	/* warn if we override user-given -s switch */
	if (scale_given)
		pg_log_warning("scale option ignored, using count from pgbench_branches table (%d)",
					   scale);

	/*
	 * Get the partition information for the first "pgbench_accounts" table
	 * found in search_path.
	 *
	 * The result is empty if no "pgbench_accounts" is found.
	 *
	 * Otherwise, it always returns one row even if the table is not
	 * partitioned (in which case the partition strategy is NULL).
	 *
	 * The number of partitions can be 0 even for partitioned tables, if no
	 * partition is attached.
	 *
	 * We assume no partitioning on any failure, so as to avoid failing on an
	 * old version without "pg_partitioned_table".
	 */
	res = PQexec(con,
				 "select o.n, p.partstrat, pg_catalog.count(i.inhparent) "
				 "from pg_catalog.pg_class as c "
				 "join pg_catalog.pg_namespace as n on (n.oid = c.relnamespace) "
				 "cross join lateral (select pg_catalog.array_position(pg_catalog.current_schemas(true), n.nspname)) as o(n) "
				 "left join pg_catalog.pg_partitioned_table as p on (p.partrelid = c.oid) "
				 "left join pg_catalog.pg_inherits as i on (c.oid = i.inhparent) "
				 "where c.relname = 'pgbench_accounts' and o.n is not null "
				 "group by 1, 2 "
				 "order by 1 asc "
				 "limit 1");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		/* probably an older version, coldly assume no partitioning */
		partition_method = PART_NONE;
		partitions = 0;
	}
	else if (PQntuples(res) == 0)
	{
		/*
		 * This case is unlikely as pgbench already found "pgbench_branches"
		 * above to compute the scale.
		 */
		pg_log_error("no pgbench_accounts table found in \"search_path\"");
		pg_log_error_hint("Perhaps you need to do initialization (\"pgbench -i\") in database \"%s\".", PQdb(con));
		exit(1);
	}
	else						/* PQntuples(res) == 1 */
	{
		/* normal case, extract partition information */
		if (PQgetisnull(res, 0, 1))
			partition_method = PART_NONE;
		else
		{
			char	   *ps = PQgetvalue(res, 0, 1);

			/* column must be there */
			Assert(ps != NULL);

			if (strcmp(ps, "r") == 0)
				partition_method = PART_RANGE;
			else if (strcmp(ps, "h") == 0)
				partition_method = PART_HASH;
			else
			{
				/* possibly a newer version with new partition method */
				pg_fatal("unexpected partition method: \"%s\"", ps);
			}
		}

		partitions = atoi(PQgetvalue(res, 0, 2));
	}

	PQclear(res);
}


/*
 * Print progress report.
 *
 * On entry, *last and *last_report contain the statistics and time of last
 * progress report.  On exit, they are updated with the new stats.
 */
static void
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
		mergeStats(&cur, &threads[i].stats);

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
static void
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
static void
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

/*
 * Set up a random seed according to seed parameter (NULL means default),
 * and initialize base_random_sequence for use in initializing other sequences.
 */
static bool
set_random_seed(const char *seed)
{
	uint64		iseed;

	if (seed == NULL || strcmp(seed, "time") == 0)
	{
		/* rely on current time */
		iseed = pg_time_now();
	}
	else if (strcmp(seed, "rand") == 0)
	{
		/* use some "strong" random source */
		if (!pg_strong_random(&iseed, sizeof(iseed)))
		{
			pg_log_error("could not generate random seed");
			return false;
		}
	}
	else
	{
		char		garbage;

		if (sscanf(seed, "%" SCNu64 "%c", &iseed, &garbage) != 1)
		{
			pg_log_error("unrecognized random seed option \"%s\"", seed);
			pg_log_error_detail("Expecting an unsigned integer, \"time\" or \"rand\".");
			return false;
		}
	}

	if (seed != NULL)
		pg_log_info("setting random seed to %" PRIu64, iseed);

	random_seed = iseed;

	/* Initialize base_random_sequence using seed */
	pg_prng_seed(&base_random_sequence, iseed);

	return true;
}

int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		/* systematic long/short named options */
		{"builtin", required_argument, NULL, 'b'},
		{"client", required_argument, NULL, 'c'},
		{"connect", no_argument, NULL, 'C'},
		{"dbname", required_argument, NULL, 'd'},
		{"define", required_argument, NULL, 'D'},
		{"file", required_argument, NULL, 'f'},
		{"fillfactor", required_argument, NULL, 'F'},
		{"host", required_argument, NULL, 'h'},
		{"initialize", no_argument, NULL, 'i'},
		{"init-steps", required_argument, NULL, 'I'},
		{"jobs", required_argument, NULL, 'j'},
		{"log", no_argument, NULL, 'l'},
		{"latency-limit", required_argument, NULL, 'L'},
		{"no-vacuum", no_argument, NULL, 'n'},
		{"port", required_argument, NULL, 'p'},
		{"progress", required_argument, NULL, 'P'},
		{"protocol", required_argument, NULL, 'M'},
		{"quiet", no_argument, NULL, 'q'},
		{"report-per-command", no_argument, NULL, 'r'},
		{"rate", required_argument, NULL, 'R'},
		{"scale", required_argument, NULL, 's'},
		{"select-only", no_argument, NULL, 'S'},
		{"skip-some-updates", no_argument, NULL, 'N'},
		{"time", required_argument, NULL, 'T'},
		{"transactions", required_argument, NULL, 't'},
		{"username", required_argument, NULL, 'U'},
		{"vacuum-all", no_argument, NULL, 'v'},
		/* long-named only options */
		{"unlogged-tables", no_argument, NULL, 1},
		{"tablespace", required_argument, NULL, 2},
		{"index-tablespace", required_argument, NULL, 3},
		{"sampling-rate", required_argument, NULL, 4},
		{"aggregate-interval", required_argument, NULL, 5},
		{"progress-timestamp", no_argument, NULL, 6},
		{"log-prefix", required_argument, NULL, 7},
		{"foreign-keys", no_argument, NULL, 8},
		{"random-seed", required_argument, NULL, 9},
		{"show-script", required_argument, NULL, 10},
		{"partitions", required_argument, NULL, 11},
		{"partition-method", required_argument, NULL, 12},
		{"failures-detailed", no_argument, NULL, 13},
		{"max-tries", required_argument, NULL, 14},
		{"verbose-errors", no_argument, NULL, 15},
		{"exit-on-abort", no_argument, NULL, 16},
		{"debug", no_argument, NULL, 17},
		{"continue-on-error", no_argument, NULL, 18},
		{NULL, 0, NULL, 0}
	};

	int			c;
	bool		is_init_mode = false;	/* initialize mode? */
	char	   *initialize_steps = NULL;
	bool		foreign_keys = false;
	bool		is_no_vacuum = false;
	bool		do_vacuum_accounts = false; /* vacuum accounts table? */
	int			optindex;
	bool		scale_given = false;

	bool		benchmarking_option_set = false;
	bool		initialization_option_set = false;
	bool		internal_script_used = false;

	CState	   *state;			/* status of clients */
	TState	   *threads;		/* array of thread */

	pg_time_usec_t
				start_time,		/* start up time */
				bench_start = 0,	/* first recorded benchmarking time */
				conn_total_duration;	/* cumulated connection time in
										 * threads */
	int64		latency_late = 0;
	StatsData	stats;
	int			weight;

	int			i;
	int			nclients_dealt;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
#endif

	PGconn	   *con;
	char	   *env;

	int			exit_code = 0;
	struct timeval tv;

	/* initialize timing infrastructure (required for INSTR_* calls) */
	pg_initialize_timing();

	/*
	 * Record difference between Unix time and instr_time time.  We'll use
	 * this for logging and aggregation.
	 */
	gettimeofday(&tv, NULL);
	epoch_shift = tv.tv_sec * INT64CONST(1000000) + tv.tv_usec - pg_time_now();

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pgbench (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	state = pg_malloc0_object(CState);

	/* set random seed early, because it may be used while parsing scripts. */
	if (!set_random_seed(getenv("PGBENCH_RANDOM_SEED")))
		pg_fatal("error while setting random seed from PGBENCH_RANDOM_SEED environment variable");

	while ((c = getopt_long(argc, argv, "b:c:Cd:D:f:F:h:iI:j:lL:M:nNp:P:qrR:s:St:T:U:v", long_options, &optindex)) != -1)
	{
		char	   *script;

		switch (c)
		{
			case 'b':
				if (strcmp(optarg, "list") == 0)
				{
					listAvailableScripts();
					exit(0);
				}
				weight = parseScriptWeight(optarg, &script);
				process_builtin(findBuiltin(script), weight);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 'c':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-c/--client", 1, INT_MAX,
									  &nclients))
				{
					exit(1);
				}
#ifdef HAVE_GETRLIMIT
				if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
					pg_fatal("getrlimit failed: %m");

				if (rlim.rlim_max < nclients + 3)
				{
					pg_log_error("need at least %d open files, but system limit is %ld",
								 nclients + 3, (long) rlim.rlim_max);
					pg_log_error_hint("Reduce number of clients, or use limit/ulimit to increase the system limit.");
					exit(1);
				}

				if (rlim.rlim_cur < nclients + 3)
				{
					rlim.rlim_cur = nclients + 3;
					if (setrlimit(RLIMIT_NOFILE, &rlim) == -1)
					{
						pg_log_error("need at least %d open files, but couldn't raise the limit: %m",
									 nclients + 3);
						pg_log_error_hint("Reduce number of clients, or use limit/ulimit to increase the system limit.");
						exit(1);
					}
				}
#endif							/* HAVE_GETRLIMIT */
				break;
			case 'C':
				benchmarking_option_set = true;
				is_connect = true;
				break;
			case 'd':
				dbName = pg_strdup(optarg);
				break;
			case 'D':
				{
					char	   *p;

					benchmarking_option_set = true;

					if ((p = strchr(optarg, '=')) == NULL || p == optarg || *(p + 1) == '\0')
						pg_fatal("invalid variable definition: \"%s\"", optarg);

					*p++ = '\0';
					if (!putVariable(&state[0].variables, "option", optarg, p))
						exit(1);
				}
				break;
			case 'f':
				weight = parseScriptWeight(optarg, &script);
				process_file(script, weight);
				benchmarking_option_set = true;
				break;
			case 'F':
				initialization_option_set = true;
				if (!option_parse_int(optarg, "-F/--fillfactor", 10, 100,
									  &fillfactor))
					exit(1);
				break;
			case 'h':
				pghost = pg_strdup(optarg);
				break;
			case 'i':
				is_init_mode = true;
				break;
			case 'I':
				pg_free(initialize_steps);
				initialize_steps = pg_strdup(optarg);
				checkInitSteps(initialize_steps);
				initialization_option_set = true;
				break;
			case 'j':			/* jobs */
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-j/--jobs", 1, INT_MAX,
									  &nthreads))
				{
					exit(1);
				}
				break;
			case 'l':
				benchmarking_option_set = true;
				use_log = true;
				break;
			case 'L':
				{
					double		limit_ms = atof(optarg);

					if (limit_ms <= 0.0)
						pg_fatal("invalid latency limit: \"%s\"", optarg);
					benchmarking_option_set = true;
					latency_limit = (int64) (limit_ms * 1000);
				}
				break;
			case 'M':
				benchmarking_option_set = true;
				for (querymode = 0; querymode < NUM_QUERYMODE; querymode++)
					if (strcmp(optarg, QUERYMODE[querymode]) == 0)
						break;
				if (querymode >= NUM_QUERYMODE)
					pg_fatal("invalid query mode (-M): \"%s\"", optarg);
				break;
			case 'n':
				is_no_vacuum = true;
				break;
			case 'N':
				process_builtin(findBuiltin("simple-update"), 1);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 'p':
				pgport = pg_strdup(optarg);
				break;
			case 'P':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-P/--progress", 1, INT_MAX,
									  &progress))
					exit(1);
				break;
			case 'q':
				initialization_option_set = true;
				use_quiet = true;
				break;
			case 'r':
				benchmarking_option_set = true;
				report_per_command = true;
				break;
			case 'R':
				{
					/* get a double from the beginning of option value */
					double		throttle_value = atof(optarg);

					benchmarking_option_set = true;

					if (throttle_value <= 0.0)
						pg_fatal("invalid rate limit: \"%s\"", optarg);
					/* Invert rate limit into per-transaction delay in usec */
					throttle_delay = 1000000.0 / throttle_value;
				}
				break;
			case 's':
				scale_given = true;
				if (!option_parse_int(optarg, "-s/--scale", 1, INT_MAX,
									  &scale))
					exit(1);
				break;
			case 'S':
				process_builtin(findBuiltin("select-only"), 1);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 't':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-t/--transactions", 1, INT_MAX,
									  &nxacts))
					exit(1);
				break;
			case 'T':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-T/--time", 1, INT_MAX,
									  &duration))
					exit(1);
				break;
			case 'U':
				username = pg_strdup(optarg);
				break;
			case 'v':
				benchmarking_option_set = true;
				do_vacuum_accounts = true;
				break;
			case 1:				/* unlogged-tables */
				initialization_option_set = true;
				unlogged_tables = true;
				break;
			case 2:				/* tablespace */
				initialization_option_set = true;
				tablespace = pg_strdup(optarg);
				break;
			case 3:				/* index-tablespace */
				initialization_option_set = true;
				index_tablespace = pg_strdup(optarg);
				break;
			case 4:				/* sampling-rate */
				benchmarking_option_set = true;
				sample_rate = atof(optarg);
				if (sample_rate <= 0.0 || sample_rate > 1.0)
					pg_fatal("invalid sampling rate: \"%s\"", optarg);
				break;
			case 5:				/* aggregate-interval */
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "--aggregate-interval", 1, INT_MAX,
									  &agg_interval))
					exit(1);
				break;
			case 6:				/* progress-timestamp */
				progress_timestamp = true;
				benchmarking_option_set = true;
				break;
			case 7:				/* log-prefix */
				benchmarking_option_set = true;
				logfile_prefix = pg_strdup(optarg);
				break;
			case 8:				/* foreign-keys */
				initialization_option_set = true;
				foreign_keys = true;
				break;
			case 9:				/* random-seed */
				benchmarking_option_set = true;
				if (!set_random_seed(optarg))
					pg_fatal("error while setting random seed from --random-seed option");
				break;
			case 10:			/* list */
				{
					const BuiltinScript *s = findBuiltin(optarg);

					fprintf(stderr, "-- %s: %s\n%s\n", s->name, s->desc, s->script);
					exit(0);
				}
				break;
			case 11:			/* partitions */
				initialization_option_set = true;
				if (!option_parse_int(optarg, "--partitions", 0, INT_MAX,
									  &partitions))
					exit(1);
				break;
			case 12:			/* partition-method */
				initialization_option_set = true;
				if (pg_strcasecmp(optarg, "range") == 0)
					partition_method = PART_RANGE;
				else if (pg_strcasecmp(optarg, "hash") == 0)
					partition_method = PART_HASH;
				else
					pg_fatal("invalid partition method, expecting \"range\" or \"hash\", got: \"%s\"",
							 optarg);
				break;
			case 13:			/* failures-detailed */
				benchmarking_option_set = true;
				failures_detailed = true;
				break;
			case 14:			/* max-tries */
				{
					int32		max_tries_arg = atoi(optarg);

					if (max_tries_arg < 0)
						pg_fatal("invalid number of maximum tries: \"%s\"", optarg);

					benchmarking_option_set = true;
					max_tries = (uint32) max_tries_arg;
				}
				break;
			case 15:			/* verbose-errors */
				benchmarking_option_set = true;
				verbose_errors = true;
				break;
			case 16:			/* exit-on-abort */
				benchmarking_option_set = true;
				exit_on_abort = true;
				break;
			case 17:			/* debug */
				pg_logging_increase_verbosity();
				break;
			case 18:			/* continue-on-error */
				benchmarking_option_set = true;
				continue_on_error = true;
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/* set default script if none */
	if (num_scripts == 0 && !is_init_mode)
	{
		process_builtin(findBuiltin("tpcb-like"), 1);
		benchmarking_option_set = true;
		internal_script_used = true;
	}

	/* complete SQL command initialization and compute total weight */
	for (i = 0; i < num_scripts; i++)
	{
		Command   **commands = sql_script[i].commands;

		for (int j = 0; commands[j] != NULL; j++)
			if (commands[j]->type == SQL_COMMAND)
				postprocess_sql_command(commands[j]);

		/* cannot overflow: weight is 32b, total_weight 64b */
		total_weight += sql_script[i].weight;
	}

	if (total_weight == 0 && !is_init_mode)
		pg_fatal("total script weight must not be zero");

	/* show per script stats if several scripts are used */
	if (num_scripts > 1)
		per_script_stats = true;

	/*
	 * Don't need more threads than there are clients.  (This is not merely an
	 * optimization; throttle_delay is calculated incorrectly below if some
	 * threads have no clients assigned to them.)
	 */
	if (nthreads > nclients)
		nthreads = nclients;

	/*
	 * Convert throttle_delay to a per-thread delay time.  Note that this
	 * might be a fractional number of usec, but that's OK, since it's just
	 * the center of a Poisson distribution of delays.
	 */
	throttle_delay *= nthreads;

	if (dbName == NULL)
	{
		if (argc > optind)
			dbName = argv[optind++];
		else
		{
			if ((env = getenv("PGDATABASE")) != NULL && *env != '\0')
				dbName = env;
			else if ((env = getenv("PGUSER")) != NULL && *env != '\0')
				dbName = env;
			else
				dbName = get_user_name_or_exit(progname);
		}
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (is_init_mode)
	{
		if (benchmarking_option_set)
			pg_fatal("some of the specified options cannot be used in initialization (-i) mode");

		if (partitions == 0 && partition_method != PART_NONE)
			pg_fatal("--partition-method requires greater than zero --partitions");

		/* set default method */
		if (partitions > 0 && partition_method == PART_NONE)
			partition_method = PART_RANGE;

		if (initialize_steps == NULL)
			initialize_steps = pg_strdup(DEFAULT_INIT_STEPS);

		if (is_no_vacuum)
		{
			/* Remove any vacuum step in initialize_steps */
			char	   *p;

			while ((p = strchr(initialize_steps, 'v')) != NULL)
				*p = ' ';
		}

		if (foreign_keys)
		{
			/* Add 'f' to end of initialize_steps, if not already there */
			if (strchr(initialize_steps, 'f') == NULL)
			{
				initialize_steps = (char *)
					pg_realloc(initialize_steps,
							   strlen(initialize_steps) + 2);
				strcat(initialize_steps, "f");
			}
		}

		runInitSteps(initialize_steps);
		exit(0);
	}
	else
	{
		if (initialization_option_set)
			pg_fatal("some of the specified options cannot be used in benchmarking mode");
	}

	if (nxacts > 0 && duration > 0)
		pg_fatal("specify either a number of transactions (-t) or a duration (-T), not both");

	/* Use DEFAULT_NXACTS if neither nxacts nor duration is specified. */
	if (nxacts <= 0 && duration <= 0)
		nxacts = DEFAULT_NXACTS;

	/* --sampling-rate may be used only with -l */
	if (sample_rate > 0.0 && !use_log)
		pg_fatal("log sampling (--sampling-rate) is allowed only when logging transactions (-l)");

	/* --sampling-rate may not be used with --aggregate-interval */
	if (sample_rate > 0.0 && agg_interval > 0)
		pg_fatal("log sampling (--sampling-rate) and aggregation (--aggregate-interval) cannot be used at the same time");

	if (agg_interval > 0 && !use_log)
		pg_fatal("log aggregation is allowed only when actually logging transactions");

	if (!use_log && logfile_prefix)
		pg_fatal("log file prefix (--log-prefix) is allowed only when logging transactions (-l)");

	if (duration > 0 && agg_interval > duration)
		pg_fatal("number of seconds for aggregation (%d) must not be higher than test duration (%d)", agg_interval, duration);

	if (duration > 0 && agg_interval > 0 && duration % agg_interval != 0)
		pg_fatal("duration (%d) must be a multiple of aggregation interval (%d)", duration, agg_interval);

	if (progress_timestamp && progress == 0)
		pg_fatal("--progress-timestamp is allowed only under --progress");

	if (!max_tries)
	{
		if (!latency_limit && duration <= 0)
			pg_fatal("an unlimited number of transaction tries can only be used with --latency-limit or a duration (-T)");
	}

	/*
	 * save main process id in the global variable because process id will be
	 * changed after fork.
	 */
	main_pid = (int) getpid();

	if (nclients > 1)
	{
		state = pg_realloc_array(state, CState, nclients);
		memset(state + 1, 0, sizeof(CState) * (nclients - 1));

		/* copy any -D switch values to all clients */
		for (i = 1; i < nclients; i++)
		{
			state[i].id = i;
			if (!copyVariables(&state[i].variables, &state[0].variables))
				exit(1);
		}
	}

	/* other CState initializations */
	for (i = 0; i < nclients; i++)
	{
		state[i].cstack = conditional_stack_create();
		initRandomState(&state[i].cs_func_rs);
	}

	/* opening connection... */
	con = doConnect();
	if (con == NULL)
		pg_fatal("could not create connection for setup");

	/* report pgbench and server versions */
	printVersion(con);

	pg_log_debug("pghost: %s pgport: %s nclients: %d %s: %d dbName: %s",
				 PQhost(con), PQport(con), nclients,
				 duration <= 0 ? "nxacts" : "duration",
				 duration <= 0 ? nxacts : duration, PQdb(con));

	if (internal_script_used)
		GetTableInfo(con, scale_given);

	/*
	 * :scale variables normally get -s or database scale, but don't override
	 * an explicit -D switch
	 */
	if (lookupVariable(&state[0].variables, "scale") == NULL)
	{
		for (i = 0; i < nclients; i++)
		{
			if (!putVariableInt(&state[i].variables, "startup", "scale", scale))
				exit(1);
		}
	}

	/*
	 * Define a :client_id variable that is unique per connection. But don't
	 * override an explicit -D switch.
	 */
	if (lookupVariable(&state[0].variables, "client_id") == NULL)
	{
		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "client_id", i))
				exit(1);
	}

	/* set default seed for hash functions */
	if (lookupVariable(&state[0].variables, "default_seed") == NULL)
	{
		uint64		seed = pg_prng_uint64(&base_random_sequence);

		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "default_seed",
								(int64) seed))
				exit(1);
	}

	/* set random seed unless overwritten */
	if (lookupVariable(&state[0].variables, "random_seed") == NULL)
	{
		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "random_seed",
								random_seed))
				exit(1);
	}

	if (!is_no_vacuum)
	{
		fprintf(stderr, "starting vacuum...");
		tryExecuteStatement(con, "vacuum pgbench_branches");
		tryExecuteStatement(con, "vacuum pgbench_tellers");
		tryExecuteStatement(con, "truncate pgbench_history");
		fprintf(stderr, "end.\n");

		if (do_vacuum_accounts)
		{
			fprintf(stderr, "starting vacuum pgbench_accounts...");
			tryExecuteStatement(con, "vacuum analyze pgbench_accounts");
			fprintf(stderr, "end.\n");
		}
	}
	PQfinish(con);

	/* set up thread data structures */
	threads = pg_malloc_array(TState, nthreads);
	nclients_dealt = 0;

	for (i = 0; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		thread->tid = i;
		thread->state = &state[nclients_dealt];
		thread->nstate =
			(nclients - nclients_dealt + nthreads - i - 1) / (nthreads - i);
		initRandomState(&thread->ts_choose_rs);
		initRandomState(&thread->ts_throttle_rs);
		initRandomState(&thread->ts_sample_rs);
		thread->logfile = NULL; /* filled in later */
		thread->latency_late = 0;
		initStats(&thread->stats, 0);

		nclients_dealt += thread->nstate;
	}

	/* all clients must be assigned to a thread */
	Assert(nclients_dealt == nclients);

	/* get start up time for the whole computation */
	start_time = pg_time_now();

	/* set alarm if duration is specified. */
	if (duration > 0)
		setalarm(duration);

	errno = THREAD_BARRIER_INIT(&barrier, nthreads);
	if (errno != 0)
		pg_fatal("could not initialize barrier: %m");

	/* start all threads but thread 0 which is executed directly later */
	for (i = 1; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		thread->create_time = pg_time_now();
		errno = THREAD_CREATE(&thread->thread, threadRun, thread);

		if (errno != 0)
			pg_fatal("could not create thread: %m");
	}

	/* compute when to stop */
	threads[0].create_time = pg_time_now();
	if (duration > 0)
		end_time = threads[0].create_time + (int64) 1000000 * duration;

	/* run thread 0 directly */
	(void) threadRun(&threads[0]);

	/* wait for other threads and accumulate results */
	initStats(&stats, 0);
	conn_total_duration = 0;

	for (i = 0; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		if (i > 0)
			THREAD_JOIN(thread->thread);

		for (int j = 0; j < thread->nstate; j++)
			if (thread->state[j].state != CSTATE_FINISHED)
				exit_code = 2;

		/* aggregate thread level stats */
		mergeStats(&stats, &thread->stats);
		latency_late += thread->latency_late;
		conn_total_duration += thread->conn_duration;

		/* first recorded benchmarking start time */
		if (bench_start == 0 || thread->bench_start < bench_start)
			bench_start = thread->bench_start;
	}

	/*
	 * All connections should be already closed in threadRun(), so this
	 * disconnect_all() will be a no-op, but clean up the connections just to
	 * be sure. We don't need to measure the disconnection delays here.
	 */
	disconnect_all(state, nclients);

	/*
	 * Beware that performance of short benchmarks with many threads and
	 * possibly long transactions can be deceptive because threads do not
	 * start and finish at the exact same time. The total duration computed
	 * here encompasses all transactions so that tps shown is somehow slightly
	 * underestimated.
	 */
	printResults(&stats, pg_time_now() - bench_start, conn_total_duration,
				 bench_start - start_time, latency_late);

	THREAD_BARRIER_DESTROY(&barrier);

	if (exit_code != 0)
		pg_log_error("Run was aborted; the above results are incomplete.");

	return exit_code;
}

static THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC
threadRun(void *arg)
{
	TState	   *thread = (TState *) arg;
	CState	   *state = thread->state;
	pg_time_usec_t start;
	int			nstate = thread->nstate;
	int			remains = nstate;	/* number of remaining clients */
	socket_set *sockets = alloc_socket_set(nstate);
	int64		thread_start,
				last_report,
				next_report;
	StatsData	last,
				aggs;

	/* open log file if requested */
	if (use_log)
	{
		char		logpath[MAXPGPATH];
		char	   *prefix = logfile_prefix ? logfile_prefix : "pgbench_log";

		if (thread->tid == 0)
			snprintf(logpath, sizeof(logpath), "%s.%d", prefix, main_pid);
		else
			snprintf(logpath, sizeof(logpath), "%s.%d.%d", prefix, main_pid, thread->tid);

		thread->logfile = fopen(logpath, "w");

		if (thread->logfile == NULL)
			pg_fatal("could not open logfile \"%s\": %m", logpath);
	}

	/* explicitly initialize the state machines */
	for (int i = 0; i < nstate; i++)
		state[i].state = CSTATE_CHOOSE_SCRIPT;

	/* READY */
	THREAD_BARRIER_WAIT(&barrier);

	thread_start = pg_time_now();
	thread->started_time = thread_start;
	thread->conn_duration = 0;
	last_report = thread_start;
	next_report = last_report + (int64) 1000000 * progress;

	/* STEADY */
	if (!is_connect)
	{
		/* make connections to the database before starting */
		for (int i = 0; i < nstate; i++)
		{
			if ((state[i].con = doConnect()) == NULL)
			{
				/* coldly abort on initial connection failure */
				pg_fatal("could not create connection for client %d",
						 state[i].id);
			}
		}
	}

	/* GO */
	THREAD_BARRIER_WAIT(&barrier);

	start = pg_time_now();
	thread->bench_start = start;
	thread->throttle_trigger = start;

	/*
	 * The log format currently has Unix epoch timestamps with whole numbers
	 * of seconds.  Round the first aggregate's start time down to the nearest
	 * Unix epoch second (the very first aggregate might really have started a
	 * fraction of a second later, but later aggregates are measured from the
	 * whole number time that is actually logged).
	 */
	initStats(&aggs, (start + epoch_shift) / 1000000 * 1000000);
	last = aggs;

	/* loop till all clients have terminated */
	while (remains > 0)
	{
		int			nsocks;		/* number of sockets to be waited for */
		pg_time_usec_t min_usec;
		pg_time_usec_t now = 0; /* set this only if needed */

		/*
		 * identify which client sockets should be checked for input, and
		 * compute the nearest time (if any) at which we need to wake up.
		 */
		clear_socket_set(sockets);
		nsocks = 0;
		min_usec = PG_INT64_MAX;
		for (int i = 0; i < nstate; i++)
		{
			CState	   *st = &state[i];

			if (st->state == CSTATE_SLEEP || st->state == CSTATE_THROTTLE)
			{
				/* a nap from the script, or under throttling */
				pg_time_usec_t this_usec;

				/* get current time if needed */
				pg_time_now_lazy(&now);

				/* min_usec should be the minimum delay across all clients */
				this_usec = (st->state == CSTATE_SLEEP ?
							 st->sleep_until : st->txn_scheduled) - now;
				if (min_usec > this_usec)
					min_usec = this_usec;
			}
			else if (st->state == CSTATE_WAIT_RESULT ||
					 st->state == CSTATE_WAIT_ROLLBACK_RESULT)
			{
				/*
				 * waiting for result from server - nothing to do unless the
				 * socket is readable
				 */
				int			sock = PQsocket(st->con);

				if (sock < 0)
				{
					pg_log_error("invalid socket: %s", PQerrorMessage(st->con));
					goto done;
				}

				add_socket_to_set(sockets, sock, nsocks++);
			}
			else if (st->state != CSTATE_ABORTED &&
					 st->state != CSTATE_FINISHED)
			{
				/*
				 * This client thread is ready to do something, so we don't
				 * want to wait.  No need to examine additional clients.
				 */
				min_usec = 0;
				break;
			}
		}

		/* also wake up to print the next progress report on time */
		if (progress && min_usec > 0 && thread->tid == 0)
		{
			pg_time_now_lazy(&now);

			if (now >= next_report)
				min_usec = 0;
			else if ((next_report - now) < min_usec)
				min_usec = next_report - now;
		}

		/*
		 * If no clients are ready to execute actions, sleep until we receive
		 * data on some client socket or the timeout (if any) elapses.
		 */
		if (min_usec > 0)
		{
			int			rc = 0;

			if (min_usec != PG_INT64_MAX)
			{
				if (nsocks > 0)
				{
					rc = wait_on_socket_set(sockets, min_usec);
				}
				else			/* nothing active, simple sleep */
				{
					pg_usleep(min_usec);
				}
			}
			else				/* no explicit delay, wait without timeout */
			{
				rc = wait_on_socket_set(sockets, 0);
			}

			if (rc < 0)
			{
				if (errno == EINTR)
				{
					/* On EINTR, go back to top of loop */
					continue;
				}
				/* must be something wrong */
				pg_log_error("%s() failed: %m", SOCKET_WAIT_METHOD);
				goto done;
			}
		}
		else
		{
			/* min_usec <= 0, i.e. something needs to be executed now */

			/* If we didn't wait, don't try to read any data */
			clear_socket_set(sockets);
		}

		/* ok, advance the state machine of each connection */
		nsocks = 0;
		for (int i = 0; i < nstate; i++)
		{
			CState	   *st = &state[i];

			if (st->state == CSTATE_WAIT_RESULT ||
				st->state == CSTATE_WAIT_ROLLBACK_RESULT)
			{
				/* don't call advanceConnectionState unless data is available */
				int			sock = PQsocket(st->con);

				if (sock < 0)
				{
					pg_log_error("invalid socket: %s", PQerrorMessage(st->con));
					goto done;
				}

				if (!socket_has_input(sockets, sock, nsocks++))
					continue;
			}
			else if (st->state == CSTATE_FINISHED ||
					 st->state == CSTATE_ABORTED)
			{
				/* this client is done, no need to consider it anymore */
				continue;
			}

			advanceConnectionState(thread, st, &aggs);

			/*
			 * If --exit-on-abort is used, the program is going to exit when
			 * any client is aborted.
			 */
			if (exit_on_abort && st->state == CSTATE_ABORTED)
				goto done;

			/*
			 * If advanceConnectionState changed client to finished state,
			 * that's one fewer client that remains.
			 */
			else if (st->state == CSTATE_FINISHED ||
					 st->state == CSTATE_ABORTED)
				remains--;
		}

		/* progress report is made by thread 0 for all threads */
		if (progress && thread->tid == 0)
		{
			pg_time_usec_t now2 = pg_time_now();

			if (now2 >= next_report)
			{
				/*
				 * Horrible hack: this relies on the thread pointer we are
				 * passed to be equivalent to threads[0], that is the first
				 * entry of the threads array.  That is why this MUST be done
				 * by thread 0 and not any other.
				 */
				printProgressReport(thread, thread_start, now2,
									&last, &last_report);

				/*
				 * Ensure that the next report is in the future, in case
				 * pgbench/postgres got stuck somewhere.
				 */
				do
				{
					next_report += (int64) 1000000 * progress;
				} while (now2 >= next_report);
			}
		}
	}

done:
	if (exit_on_abort)
	{
		/*
		 * Abort if any client is not finished, meaning some error occurred.
		 */
		for (int i = 0; i < nstate; i++)
		{
			if (state[i].state != CSTATE_FINISHED)
			{
				pg_log_error("Run was aborted due to an error in thread %d",
							 thread->tid);
				exit(2);
			}
		}
	}

	disconnect_all(state, nstate);

	if (thread->logfile)
	{
		if (agg_interval > 0)
		{
			/* log aggregated but not yet reported transactions */
			doLog(thread, state, &aggs, false, 0, 0);
		}
		fclose(thread->logfile);
		thread->logfile = NULL;
	}
	free_socket_set(sockets);
	THREAD_FUNC_RETURN;
}

static void
finishCon(CState *st)
{
	if (st->con != NULL)
	{
		PQfinish(st->con);
		st->con = NULL;
	}
}

/*
 * Support for duration option: set timer_exceeded after so many seconds.
 */

#ifndef WIN32

static void
handle_sig_alarm(SIGNAL_ARGS)
{
	timer_exceeded = true;
}

static void
setalarm(int seconds)
{
	pqsignal(SIGALRM, handle_sig_alarm);
	alarm(seconds);
}

#else							/* WIN32 */

static VOID CALLBACK
win32_timer_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
	timer_exceeded = true;
}

static void
setalarm(int seconds)
{
	HANDLE		queue;
	HANDLE		timer;

	/* This function will be called at most once, so we can cheat a bit. */
	queue = CreateTimerQueue();
	if (seconds > ((DWORD) -1) / 1000 ||
		!CreateTimerQueueTimer(&timer, queue,
							   win32_timer_callback, NULL, seconds * 1000, 0,
							   WT_EXECUTEINTIMERTHREAD | WT_EXECUTEONLYONCE))
		pg_fatal("failed to set timer");
}

#endif							/* WIN32 */
