/*-------------------------------------------------------------------------
 *
 * engine.h
 *		pgbench execution engine, client state machine, and thread runner
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/engine.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_ENGINE_H
#define PGBENCH_ENGINE_H

#include <signal.h>
#include "libpq-fe.h"
#include "common/pg_prng.h"
#include "portability/instr_time.h"
#include "stats.h"

typedef struct CState CState;

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

typedef enum TStatus
{
	TSTATUS_IDLE,
	TSTATUS_IN_BLOCK,
	TSTATUS_CONN_ERROR,
	TSTATUS_OTHER_ERROR,
} TStatus;

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

/* Benchmark runtime parameters */
extern int	nxacts;
extern int	duration;
extern int64 end_time;
extern double sample_rate;
extern double throttle_delay;
extern int64 latency_limit;
extern int64 random_seed;
extern bool use_log;
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
extern bool verbose_errors;
extern bool exit_on_abort;
extern bool continue_on_error;

extern THREAD_BARRIER_T barrier;
extern pg_prng_state base_random_sequence;
extern pg_time_usec_t epoch_shift;

/* Core engine functions */
extern void initRandomState(pg_prng_state *state);
extern PGconn *doConnect(void);
extern void finishCon(CState *st);
extern void disconnect_all(CState *state, int length);
extern void setalarm(int seconds);

extern THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC threadRun(void *arg);

extern void printVersion(PGconn *con);
extern void printResults(StatsData *total,
						 pg_time_usec_t total_duration,
						 pg_time_usec_t conn_total_duration,
						 pg_time_usec_t conn_elapsed_duration,
						int64 latency_late);

#endif							/* PGBENCH_ENGINE_H */
