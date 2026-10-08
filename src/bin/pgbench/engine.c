/*-------------------------------------------------------------------------
 *
 * engine.c
 *		pgbench execution engine, client state machine, and thread runner
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/engine.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#ifdef WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

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
#include "libpq-fe.h"
#include "pgbench.h"
#include "engine.h"
#include "commands.h"
#include "init.h"
#include "poller.h"
#include "script.h"
#include "stats.h"
#include "variable.h"
#include "port/pg_bitutils.h"
#include "portability/instr_time.h"

#define ERRCODE_T_R_SERIALIZATION_FAILURE  "40001"
#define ERRCODE_T_R_DEADLOCK_DETECTED  "40P01"

/* Benchmark runtime parameters */
int			nxacts = 0;			/* number of transactions per client */
int			duration = 0;		/* duration in seconds */
int64		end_time = 0;		/* when to stop in micro seconds, under -T */

double		sample_rate = 0.0;
double		throttle_delay = 0;
int64		latency_limit = 0;
int64		random_seed = -1;

bool		use_log = false;	/* log transaction latencies to a file */
int			agg_interval = 0;	/* log aggregates instead of individual transactions */
bool		per_script_stats = false;	/* whether to collect stats per script */
int			progress = 0;		/* thread progress report every this seconds */
bool		progress_timestamp = false; /* progress report with Unix time */
int			nclients = 1;		/* number of clients */
int			nthreads = 1;		/* number of threads */
bool		is_connect = false;	/* establish connection for each transaction */
bool		report_per_command = false; /* report per-command latencies */
int			main_pid = 0;		/* main process id used in log filename */

uint32		max_tries = 1;
bool		failures_detailed = false;

const char *pghost = NULL;
const char *pgport = NULL;
const char *username = NULL;
const char *dbName = NULL;
char	   *logfile_prefix = NULL;
const char *progname = NULL;
volatile sig_atomic_t timer_exceeded = false;

pg_time_usec_t epoch_shift;
pg_prng_state base_random_sequence;
THREAD_BARRIER_T barrier;

bool		verbose_errors = false;
bool		exit_on_abort = false;
bool		continue_on_error = false;

/* Forward declarations */
static void doLog(TState *thread, CState *st,
				  StatsData *agg, bool tx_skipped, double latency, double lag);
static void processXactStats(TState *thread, CState *st, pg_time_usec_t *now,
							 bool tx_skipped, StatsData *agg);

/*
 * Initialize a prng state struct.
 *
 * We derive the seed from base_random_sequence, which must be set up already.
 */
void
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

/* set up a connection to the backend */
PGconn *
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
void
disconnect_all(CState *state, int length)
{
	int			i;

	for (i = 0; i < length; i++)
		finishCon(&state[i]);
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

THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC
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

void
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

void
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

void
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


