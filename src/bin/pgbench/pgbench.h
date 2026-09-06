/*-------------------------------------------------------------------------
 *
 * pgbench.h
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_H
#define PGBENCH_H

#include <signal.h>
#include "libpq-fe.h"
#include "fe_utils/conditional.h"
#include "fe_utils/psqlscan.h"
#include "common/pg_prng.h"
#include "portability/instr_time.h"
#include "poller.h"
#include "stats.h"
#include "variable.h"
#include "script.h"

/* Connection state machine enumeration */
typedef enum ConnectionStateEnum
{
	CSTATE_CHOOSE_SCRIPT,
	CSTATE_START_TX,
	CSTATE_PREPARE_THROTTLE,
	CSTATE_THROTTLE,
	CSTATE_START_COMMAND,
	CSTATE_WAIT_RESULT,
	CSTATE_SLEEP,
	CSTATE_END_COMMAND,
	CSTATE_SKIP_COMMAND,
	CSTATE_ERROR,
	CSTATE_WAIT_ROLLBACK_RESULT,
	CSTATE_RETRY,
	CSTATE_FAILURE,
	CSTATE_END_TX,
	CSTATE_ABORTED,
	CSTATE_FINISHED,
} ConnectionStateEnum;

/*
 * Connection state.
 */
struct CState
{
	PGconn	   *con;			/* connection handle to DB */
	int			id;				/* client No. */
	ConnectionStateEnum state;	/* state machine's current state. */
	ConditionalStack cstack;	/* enclosing conditionals state */

	/*
	 * Separate randomness for each client. This is used for random functions
	 * PGBENCH_RANDOM_* during the execution of the script.
	 */
	pg_prng_state cs_func_rs;

	int			use_file;		/* index in sql_script for this client */
	int			command;		/* command number in script */
	int			num_syncs;		/* number of ongoing sync commands */

	/* client variables */
	Variables	variables;

	/* various times about current transaction in microseconds */
	pg_time_usec_t txn_scheduled;	/* scheduled start time of transaction */
	pg_time_usec_t sleep_until; /* scheduled start time of next cmd */
	pg_time_usec_t txn_begin;	/* used for measuring schedule lag times */
	pg_time_usec_t stmt_begin;	/* used for measuring statement latencies */

	/* whether client prepared each command of each script */
	bool	  **prepared;

	/*
	 * For processing failures and repeating transactions with serialization
	 * or deadlock errors:
	 */
	EStatus		estatus;		/* the error status of the current transaction
								 * execution; this is ESTATUS_NO_ERROR if
								 * there were no errors */
	pg_prng_state random_state; /* random state */
	uint32		tries;			/* how many times have we already tried the
								 * current transaction? */

	/* per client collected stats */
	int64		cnt;			/* client transaction count, for -t; skipped
								 * and failed transactions are also counted
								 * here */
};

extern volatile sig_atomic_t timer_exceeded;

#include "commands.h"

#endif							/* PGBENCH_H */
