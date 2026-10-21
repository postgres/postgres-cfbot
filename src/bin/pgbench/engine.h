/*-------------------------------------------------------------------------
 *
 * engine.h
 *	  Client state machine, thread execution, and logging for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/engine.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ENGINE_H
#define ENGINE_H

#include <signal.h>

#include "common/pg_prng.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "stats.h"
#include "variable.h"

#define ERRCODE_T_R_SERIALIZATION_FAILURE  "40001"
#define ERRCODE_T_R_DEADLOCK_DETECTED  "40P01"

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

#define DEFAULT_NXACTS	10		/* default nxacts */

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

#define MAX_GROUPS		32		/* max number of client groups */

/*
 * Heterogeneous client group configuration and statistics (--group).
 */
typedef struct ClientGroup
{
	char	   *name;			/* group name */
	int			nclients;		/* number of clients in group */
	int			nthreads;		/* number of threads in group */
	bool		sequential;		/* sequential script selection */
	double		rate;			/* target TPS rate for group (0 = none) */
	double		throttle_delay; /* per-thread throttle delay in usec */
	int			script_indices[128];	/* indices into sql_script[] */
	int			num_scripts;	/* number of scripts in this group */
	int64		total_weight;	/* sum of weights of group's scripts */
	StatsData	stats;			/* aggregated stats for this group */
} ClientGroup;

/*
 * Thread state
 */
typedef struct
{
	int			tid;			/* thread id */
	int			group_id;		/* client group index, or -1 */
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
	int			wakeup_pipe[2]; /* non-blocking self-pipe for watermark wakeups */
} TState;

#define MAX_SLAS		64		/* max number of SLA rules */

typedef enum SlaMetricType
{
	SLA_METRIC_LATENCY_PCT = 0, /* p<pct> <= <val>[us|ms|s] */
	SLA_METRIC_LATENCY_AVG,		/* avg <= <val>[us|ms|s] */
	SLA_METRIC_LAG_PCT,			/* schedule_lag:p<pct> <= <val>[us|ms|s] */
	SLA_METRIC_ONTIME_PCT,		/* ontime_pct >= <pct> */
	SLA_METRIC_RECALL_AVG,		/* recall >= <val> */
	SLA_METRIC_RECALL_PCT,		/* recall_p<pct> >= <val> */
	SLA_METRIC_TPS,				/* tps >= <val> or qps >= <val> */
	SLA_METRIC_ERROR_RATE,		/* error_rate <= <pct> */
	SLA_METRIC_VALIDATION_FAILURES, /* validation_failures <= <cnt> */
	SLA_METRIC_VALIDATION_PCT	/* validation_pct >= <pct> */
} SlaMetricType;

typedef enum SlaOpType
{
	SLA_OP_LE = 0,				/* <= */
	SLA_OP_GE					/* >= */
} SlaOpType;

/*
 * SLA compliance rule (--sla [scope:]<metric>(<=|>=)<val>[unit]).
 */
typedef struct SlaRule
{
	char		   *raw_spec;		/* original SLA specification string */
	char		   *target;			/* script name/substring, group name, or "total" */
	char		   *named_metric;	/* optional named recall metric (e.g. "r10", "r1") */
	SlaMetricType	metric;			/* metric type */
	SlaOpType		op;				/* comparison operator */
	double			percentile;		/* e.g. 50.0, 90.0, 95.0, 99.0, 99.9 */
	double			threshold_usec; /* max allowed latency/lag in microseconds */
	double			actual_usec;	/* measured latency/lag in microseconds */
	double			threshold_val;	/* generic threshold (recall, pct, tps) */
	double			actual_val;		/* generic measured value */
	bool			evaluated;		/* true if target matched at least 1 sample */
	bool			passed;			/* true if evaluated and condition satisfied */
} SlaRule;

extern ClientGroup client_groups[MAX_GROUPS];
extern int	num_groups;
extern SlaRule sla_rules[MAX_SLAS];
extern int	num_slas;
extern bool sla_failed;
extern char *json_report_file;
extern double replay_speed;
extern double ontime_threshold_ms;
extern char *replay_trace_file;
extern int	max_concurrent_sessions;
extern int	active_sessions_count;

extern int	nxacts;
extern int	duration;
extern int64 end_time;
extern double sample_rate;
extern double throttle_delay;
extern int64 latency_limit;
extern bool use_log;
extern bool use_quiet;
extern int	agg_interval;
extern bool per_script_stats;
extern int	progress;
extern bool progress_timestamp;
extern int	nclients;
extern int	nthreads;
extern bool is_connect;
extern bool report_per_command;
extern int	main_pid;
extern uint32 max_tries;
extern bool failures_detailed;
extern const char *pghost;
extern const char *pgport;
extern const char *username;
extern const char *dbName;
extern char *logfile_prefix;
extern const char *progname;
extern volatile sig_atomic_t timer_exceeded;
extern pg_time_usec_t epoch_shift;
extern pg_prng_state base_random_sequence;
extern THREAD_BARRIER_T barrier;
extern bool verbose_errors;
extern bool exit_on_abort;
extern bool continue_on_error;

extern void initRandomState(pg_prng_state *state);
extern void resetEngineRunState(void);
extern void resolveHostAddrOnce(void);
extern PGconn *doConnect(void);
extern PGconn *doConnectAsync(const char *db_override, const char *user_override);
extern void disconnect_all(CState *state, int length);
extern void printVersion(PGconn *con);
extern void printResults(StatsData *total, pg_time_usec_t total_time,
						 pg_time_usec_t conn_total_time,
						 pg_time_usec_t bench_total_time,
						 int64 latency_late);
extern THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC threadRun(void *arg);
extern void setalarm(int seconds);

#endif							/* ENGINE_H */
