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

#include "fe_utils/psqlscan.h"
#include "common/pg_prng.h"
#include "fe_utils/conditional.h"
#include "libpq-fe.h"
#include "stats.h"
#include "variable.h"

/*
 * This file is included outside exprscan.l, in places where we can't see
 * flex's definition of typedef yyscan_t.  Fortunately, it's documented as
 * being "void *", so we can use typedef to keep the function declarations
 * here looking like the definitions in exprscan.l.  exprparse.y and
 * pgbench.c also use this to be able to declare things as "yyscan_t".
 */
typedef void *yyscan_t;

/*
 * Likewise, we can't see exprparse.y's definition of union YYSTYPE here,
 * but for now there's no need to know what the union contents are.
 */
union YYSTYPE;


/* Types of expression nodes */
typedef enum PgBenchExprType
{
	ENODE_CONSTANT,
	ENODE_VARIABLE,
	ENODE_FUNCTION,
} PgBenchExprType;

/* List of operators and callable functions */
typedef enum PgBenchFunction
{
	PGBENCH_ADD,
	PGBENCH_SUB,
	PGBENCH_MUL,
	PGBENCH_DIV,
	PGBENCH_MOD,
	PGBENCH_DEBUG,
	PGBENCH_ABS,
	PGBENCH_LEAST,
	PGBENCH_GREATEST,
	PGBENCH_INT,
	PGBENCH_DOUBLE,
	PGBENCH_PI,
	PGBENCH_SQRT,
	PGBENCH_LN,
	PGBENCH_EXP,
	PGBENCH_RANDOM,
	PGBENCH_RANDOM_GAUSSIAN,
	PGBENCH_RANDOM_EXPONENTIAL,
	PGBENCH_RANDOM_ZIPFIAN,
	PGBENCH_POW,
	PGBENCH_AND,
	PGBENCH_OR,
	PGBENCH_NOT,
	PGBENCH_BITAND,
	PGBENCH_BITOR,
	PGBENCH_BITXOR,
	PGBENCH_LSHIFT,
	PGBENCH_RSHIFT,
	PGBENCH_EQ,
	PGBENCH_NE,
	PGBENCH_LE,
	PGBENCH_LT,
	PGBENCH_IS,
	PGBENCH_CASE,
	PGBENCH_HASH_FNV1A,
	PGBENCH_HASH_MURMUR2,
	PGBENCH_PERMUTE,
} PgBenchFunction;

typedef struct PgBenchExpr PgBenchExpr;
typedef struct PgBenchExprLink PgBenchExprLink;
typedef struct PgBenchExprList PgBenchExprList;

struct PgBenchExpr
{
	PgBenchExprType etype;
	union
	{
		PgBenchValue constant;
		struct
		{
			char	   *varname;
		}			variable;
		struct
		{
			PgBenchFunction function;
			PgBenchExprLink *args;
		}			function;
	}			u;
};

/* List of expression nodes */
struct PgBenchExprLink
{
	PgBenchExpr *expr;
	PgBenchExprLink *next;
};

struct PgBenchExprList
{
	PgBenchExprLink *head;
	PgBenchExprLink *tail;
};


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

/*
 * Transaction status at the end of a command.
 */
typedef enum TStatus
{
	TSTATUS_IDLE,
	TSTATUS_IN_BLOCK,
	TSTATUS_CONN_ERROR,
	TSTATUS_OTHER_ERROR,
} TStatus;

/*
 * Connection state machine states.
 */
typedef enum
{
	/*
	 * The client must first choose a script to execute.  Once chosen, it can
	 * either be throttled (state CSTATE_PREPARE_THROTTLE under --rate), start
	 * right away (state CSTATE_START_TX) or not start at all if the timer was
	 * exceeded (state CSTATE_FINISHED).
	 */
	CSTATE_CHOOSE_SCRIPT,

	/*
	 * CSTATE_START_TX performs start-of-transaction processing.  Establishes
	 * a new connection for the transaction in --connect mode, records the
	 * transaction start time, and proceed to the first command.
	 *
	 * Note: once a script is started, it will either error or run till its
	 * end, where it may be interrupted. It is not interrupted while running,
	 * so pgbench --time is to be understood as tx are allowed to start in
	 * that time, and will finish when their work is completed.
	 */
	CSTATE_START_TX,

	/*
	 * In CSTATE_PREPARE_THROTTLE state, we calculate when to begin the next
	 * transaction, and advance to CSTATE_THROTTLE.  CSTATE_THROTTLE state
	 * sleeps until that moment, then advances to CSTATE_START_TX, or
	 * CSTATE_FINISHED if the next transaction would start beyond the end of
	 * the run.
	 */
	CSTATE_PREPARE_THROTTLE,
	CSTATE_THROTTLE,

	/*
	 * We loop through these states, to process each command in the script:
	 *
	 * CSTATE_START_COMMAND starts the execution of a command.  On a SQL
	 * command, the command is sent to the server, and we move to
	 * CSTATE_WAIT_RESULT state unless in pipeline mode. On a \sleep
	 * meta-command, the timer is set, and we enter the CSTATE_SLEEP state to
	 * wait for it to expire. Other meta-commands are executed immediately. If
	 * the command about to start is actually beyond the end of the script,
	 * advance to CSTATE_END_TX.
	 *
	 * CSTATE_WAIT_RESULT waits until we get a result set back from the server
	 * for the current command.
	 *
	 * CSTATE_SLEEP waits until the end of \sleep.
	 *
	 * CSTATE_END_COMMAND records the end-of-command timestamp, increments the
	 * command counter, and loops back to CSTATE_START_COMMAND state.
	 *
	 * CSTATE_SKIP_COMMAND is used by conditional branches which are not
	 * executed. It quickly skip commands that do not need any evaluation.
	 * This state can move forward several commands, till there is something
	 * to do or the end of the script.
	 */
	CSTATE_START_COMMAND,
	CSTATE_WAIT_RESULT,
	CSTATE_SLEEP,
	CSTATE_END_COMMAND,
	CSTATE_SKIP_COMMAND,

	/*
	 * States for failed commands.
	 *
	 * If the SQL/meta command fails, in CSTATE_ERROR clean up after an error:
	 * (1) clear the conditional stack; (2) if we have an unterminated
	 * (possibly failed) transaction block, send the rollback command to the
	 * server and wait for the result in CSTATE_WAIT_ROLLBACK_RESULT.  If
	 * something goes wrong with rolling back, go to CSTATE_ABORTED.
	 *
	 * But if everything is ok we are ready for future transactions: if this
	 * is a serialization or deadlock error and we can re-execute the
	 * transaction from the very beginning, go to CSTATE_RETRY; otherwise go
	 * to CSTATE_FAILURE.
	 *
	 * In CSTATE_RETRY report an error, set the same parameters for the
	 * transaction execution as in the previous tries and process the first
	 * transaction command in CSTATE_START_COMMAND.
	 *
	 * In CSTATE_FAILURE report a failure, set the parameters for the
	 * transaction execution as they were before the first run of this
	 * transaction (except for a random state) and go to CSTATE_END_TX to
	 * complete this transaction.
	 */
	CSTATE_ERROR,
	CSTATE_WAIT_ROLLBACK_RESULT,
	CSTATE_RETRY,
	CSTATE_FAILURE,

	/*
	 * CSTATE_END_TX performs end-of-transaction processing.  It calculates
	 * latency, and logs the transaction.  In --connect mode, it closes the
	 * current connection.
	 *
	 * Then either starts over in CSTATE_CHOOSE_SCRIPT, or enters
	 * CSTATE_FINISHED if we have no more work to do.
	 */
	CSTATE_END_TX,

	/*
	 * Final states.  CSTATE_ABORTED means that the script execution was
	 * aborted because a command failed, CSTATE_FINISHED means success.
	 */
	CSTATE_ABORTED,
	CSTATE_FINISHED,
} ConnectionStateEnum;

/*
 * Connection state.
 */
typedef struct
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
} CState;

/*
 * Thread state
 */
typedef struct
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

extern int	expr_yyparse(PgBenchExpr **expr_parse_result_p, yyscan_t yyscanner);
extern int	expr_yylex(union YYSTYPE *yylval_param, yyscan_t yyscanner);
pg_noreturn extern void expr_yyerror(PgBenchExpr **expr_parse_result_p, yyscan_t yyscanner, const char *message);
pg_noreturn extern void expr_yyerror_more(yyscan_t yyscanner, const char *message,
										  const char *more);
extern bool expr_lex_one_word(PsqlScanState state, PQExpBuffer word_buf,
							  int *offset);
extern yyscan_t expr_scanner_init(PsqlScanState state,
								  const char *source, int lineno, int start_offset,
								  const char *command);
extern void expr_scanner_finish(yyscan_t yyscanner);
extern char *expr_scanner_get_substring(PsqlScanState state,
										int start_offset,
										bool chomp);

pg_noreturn extern void syntax_error(const char *source, int lineno, const char *line,
									 const char *command, const char *msg,
									 const char *more, int column);

#endif							/* PGBENCH_H */
