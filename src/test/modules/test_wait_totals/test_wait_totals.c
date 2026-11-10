/*--------------------------------------------------------------------------
 *
 * test_wait_totals.c
 *		Producer for the pg_wait_event_tracing instance-totals tests.
 *
 * Emits an exactly known number of timed wait-event pairs of one dedicated
 * extension wait event, so a test can assert conservation (exact counts)
 * of that one event no matter what other waits the server performs.  The
 * pairs go through pgstat_report_wait_start_timed()/..._end_timed(), the
 * same call pair the server's own call sites use, so any installed hook
 * sees them exactly like an ordinary wait.
 *
 * Besides plain emitters the module offers a CPU burner that never waits
 * (to keep a completed wait pending), a background worker that emits and
 * exits without returning to a wait, and helpers for LWLock tranches:
 * real contention on a test lock, and synthetic completed waits on chosen
 * tranche ids (a LWLock wait is timed only when the lock is contended, so
 * filling a tranche dictionary with real contention would be slow).
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_wait_totals/test_wait_totals.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "postmaster/bgworker.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/wait_classes.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_wait_totals_event_key);
PG_FUNCTION_INFO_V1(test_wait_totals_emit);
PG_FUNCTION_INFO_V1(test_wait_totals_emit_row);
PG_FUNCTION_INFO_V1(test_wait_totals_emit_then_cpu);
PG_FUNCTION_INFO_V1(test_wait_totals_bgworker);
PG_FUNCTION_INFO_V1(test_wait_totals_bgworker_hold);
PG_FUNCTION_INFO_V1(test_wait_totals_procnumber);
PG_FUNCTION_INFO_V1(test_wait_totals_new_tranches);
PG_FUNCTION_INFO_V1(test_wait_totals_emit_lwlock);
PG_FUNCTION_INFO_V1(test_wait_totals_lock_tranche);
PG_FUNCTION_INFO_V1(test_wait_totals_lock_hold);
PG_FUNCTION_INFO_V1(test_wait_totals_lock_holder);
PG_FUNCTION_INFO_V1(test_wait_totals_lock_wait);
PG_FUNCTION_INFO_V1(test_wait_totals_gate_reset);
PG_FUNCTION_INFO_V1(test_wait_totals_holding);
PG_FUNCTION_INFO_V1(test_wait_totals_release);
PG_FUNCTION_INFO_V1(test_wait_totals_emit_hold);
PG_FUNCTION_INFO_V1(test_wait_totals_emit_until_stopped);
PG_FUNCTION_INFO_V1(test_wait_totals_wait_writers);
PG_FUNCTION_INFO_V1(test_wait_totals_stop);
PG_FUNCTION_INFO_V1(test_wait_totals_emit_with_capture);
PG_FUNCTION_INFO_V1(test_wait_totals_emit_then_set_capture);
PG_FUNCTION_INFO_V1(test_wait_totals_emit_event);
PG_FUNCTION_INFO_V1(test_wait_totals_wait_progress);
PG_FUNCTION_INFO_V1(test_wait_totals_bgworker_procnumber);

PGDLLEXPORT void test_wait_totals_worker_main(Datum main_arg);

#define TWT_EVENT_NAME "TestWaitTotals"
#define TWT_SHARED_NAME "test_wait_totals"
#define TWT_NLOCKS 4


/* Test locks, one tranche each, in a DSM registry segment. */
typedef struct TwtShared
{
	LWLock		locks[TWT_NLOCKS];
	int			tranche[TWT_NLOCKS];
	pg_atomic_uint32 holder_pid[TWT_NLOCKS];

	/*
	 * Test-only handshake flags, so a test never has to guess how long a
	 * process stays somewhere: gate (released by the observer), holding (set
	 * by an emitter that is inside its CPU stretch), stop and writers (for
	 * writers that run until the observer is done).  Nothing here is
	 * reachable from a production hook.
	 */
	pg_atomic_uint32 gate;
	pg_atomic_uint32 holding;
	pg_atomic_uint32 stop;
	pg_atomic_uint32 writers;
	pg_atomic_uint32 progress;	/* pairs emitted by until_stopped writers */
	pg_atomic_uint32 worker_procno1;	/* last bgworker's ProcNumber + 1 */
} TwtShared;

/* Failure watchdog for every handshake wait; never the success criterion. */
#define TWT_WATCHDOG_MS 120000

static TwtShared *twt_shared;

/* The dedicated wait event; 0 until first needed in this process. */
static uint32 twt_event;

static TwtShared *twt_get_shared(void);

static uint32
twt_get_event(void)
{
	if (twt_event == 0)
		twt_event = WaitEventExtensionNew(TWT_EVENT_NAME);
	return twt_event;
}

/*
 * Busy-wait for us microseconds.  Called with a timed pair open, so it must
 * not raise an error: if an interrupt becomes pending it just returns early,
 * and the caller closes its pair and then calls CHECK_FOR_INTERRUPTS().
 */
static pg_atomic_uint32 *twt_spin_stop;	/* also ends a spin when set */
static bool twt_have_deadline;
static instr_time twt_deadline;		/* absolute time that ends a spin */

static void
twt_spin_us(int64 us)
{
	instr_time	start,
				now;

	INSTR_TIME_SET_CURRENT(start);
	do
	{
		if (InterruptPending)
			return;
		if (twt_spin_stop != NULL && pg_atomic_read_u32(twt_spin_stop) != 0)
			return;
		if (twt_have_deadline)
		{
			instr_time	t;

			INSTR_TIME_SET_CURRENT(t);
			if (INSTR_TIME_GET_NANOSEC(t) >= INSTR_TIME_GET_NANOSEC(twt_deadline))
				return;
		}
		now = start;
		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, start);
	} while (INSTR_TIME_GET_MICROSEC(now) < us);
}

/* n timed pairs of the dedicated event, each busy for at least spin_us. */
static void
twt_emit(int32 n, int32 spin_us)
{
	uint32		ev = twt_get_event();
	int32		i;

	for (i = 0; i < n; i++)
	{
		pgstat_report_wait_start_timed(ev);
		twt_spin_us(spin_us);
		pgstat_report_wait_end_timed();
		CHECK_FOR_INTERRUPTS();
	}
}

/* Burn CPU for ms milliseconds, with no wait, but cancelable. */
static void
twt_burn_cpu_ms(int32 ms)
{
	instr_time	start,
				now;

	INSTR_TIME_SET_CURRENT(start);
	do
	{
		CHECK_FOR_INTERRUPTS();
		now = start;
		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, start);
	} while (INSTR_TIME_GET_MILLISEC(now) < ms);
}

/* Sane limits for the test producers, checked before any pair opens. */
#define TWT_MAX_COUNT 1000000000
#define TWT_MAX_SPIN_US 10000000

static void
twt_check_count(int32 n, int32 spin_us)
{
	if (n < 0 || spin_us < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("count and spin duration must not be negative")));
	if (n > TWT_MAX_COUNT || spin_us > TWT_MAX_SPIN_US)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("count must be at most %d and spin duration at most %d microseconds",
						TWT_MAX_COUNT, TWT_MAX_SPIN_US)));
}

/* test_wait_totals_event_key() -> the dedicated event's wait_event_info */
Datum
test_wait_totals_event_key(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) twt_get_event());
}

/* test_wait_totals_emit(n, spin_us): exactly n timed pairs. */
Datum
test_wait_totals_emit(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	int32		spin_us = PG_GETARG_INT32(1);

	twt_check_count(n, spin_us);
	twt_emit(n, spin_us);
	PG_RETURN_VOID();
}

/*
 * test_wait_totals_emit_row(n, spin_us): the same, but returns 1 when it ran
 * in a parallel worker and 0 otherwise.  It is PARALLEL SAFE: it touches no
 * shared state beyond WaitEventExtensionNew(), which is built for concurrent
 * use.  Called once per row of a forced parallel scan it makes the workers
 * emit, and summing the result counts the rows that really ran in a worker.
 */
Datum
test_wait_totals_emit_row(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	int32		spin_us = PG_GETARG_INT32(1);

	twt_check_count(n, spin_us);
	twt_emit(n, spin_us);
	PG_RETURN_INT32(IsParallelWorker() ? 1 : 0);
}

/*
 * test_wait_totals_emit_then_cpu(n, cpu_ms, spin_us): n pairs, then CPU for
 * cpu_ms without any wait, so the last completion stays unflushed.
 */
Datum
test_wait_totals_emit_then_cpu(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	int32		cpu_ms = PG_GETARG_INT32(1);
	int32		spin_us = PG_GETARG_INT32(2);

	twt_check_count(n, spin_us);
	twt_emit(n, spin_us);
	twt_burn_cpu_ms(cpu_ms);
	PG_RETURN_VOID();
}

/* Background worker: emit n pairs and exit, with no wait in between. */
void
test_wait_totals_worker_main(Datum main_arg)
{
	TwtShared  *s;

	int32		arg = DatumGetInt32(main_arg);
	bool		hold = MyBgworkerEntry->bgw_extra[0] != 0;

	BackgroundWorkerUnblockSignals();
	/* Look the shared state up first: the lookup may take a lock. */
	s = twt_get_shared();
	pg_atomic_write_u32(&s->worker_procno1, (uint32) MyProcNumber + 1);
	twt_emit(arg, 1);

	if (hold)
	{
		instr_time	start,
					now;

		/* Stay alive, without any wait, until released. */
		INSTR_TIME_SET_CURRENT(start);
		pg_atomic_write_u32(&s->holding, 1);
		while (pg_atomic_read_u32(&s->gate) == 0)
		{
			CHECK_FOR_INTERRUPTS();
			now = start;
			INSTR_TIME_SET_CURRENT(now);
			INSTR_TIME_SUBTRACT(now, start);
			if (INSTR_TIME_GET_MILLISEC(now) > TWT_WATCHDOG_MS)
				break;
			pg_usleep(1000);
		}
		pg_atomic_write_u32(&s->holding, 0);
	}
}

/*
 * test_wait_totals_bgworker(n): run a worker that emits n pairs and exits
 * right after the last one; return once it has exited.
 */
static void
twt_run_worker(int32 n, bool hold)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	twt_check_count(n, 0);
	/* Register the name now: the worker then only looks it up. */
	(void) twt_get_event();

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "test_wait_totals");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "test_wait_totals_worker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "test_wait_totals worker");
	snprintf(worker.bgw_type, BGW_MAXLEN, "test_wait_totals worker");
	worker.bgw_main_arg = Int32GetDatum(n);
	/* bgw_extra[0]: stay alive after emitting, until released */
	worker.bgw_extra[0] = hold ? 1 : 0;
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register background worker")));

	if (hold)
		status = WaitForBackgroundWorkerStartup(handle, &pid);
	else
		status = WaitForBackgroundWorkerShutdown(handle);
	if (status == BGWH_POSTMASTER_DIED)
		ereport(ERROR,
				(errcode(ERRCODE_ADMIN_SHUTDOWN),
				 errmsg("postmaster died while waiting for the worker")));
}

Datum
test_wait_totals_bgworker(PG_FUNCTION_ARGS)
{
	twt_run_worker(PG_GETARG_INT32(0), false);
	PG_RETURN_VOID();
}

/*
 * test_wait_totals_bgworker_hold(n): start a worker that emits n pairs and
 * then stays alive (in a loop with no wait) until test_wait_totals_release();
 * return once it has started.  Watch test_wait_totals_holding().
 */
Datum
test_wait_totals_bgworker_hold(PG_FUNCTION_ARGS)
{
	twt_run_worker(PG_GETARG_INT32(0), true);
	PG_RETURN_VOID();
}

/* test_wait_totals_procnumber() -> this backend's ProcNumber */
Datum
test_wait_totals_procnumber(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyProcNumber);
}

/*
 * test_wait_totals_new_tranches(n, prefix) -> int[]: n newly registered
 * LWLock tranche ids, named prefix_0 ... prefix_(n-1).  They stay registered
 * for the life of the server (there are at most MAX_USER_DEFINED_TRANCHES).
 */
Datum
test_wait_totals_new_tranches(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	char	   *prefix = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Datum	   *ids;
	int32		i;

	twt_check_count(n, 0);
	ids = palloc_array(Datum, Max(n, 1));
	for (i = 0; i < n; i++)
	{
		char		name[NAMEDATALEN];

		snprintf(name, sizeof(name), "%s_%d", prefix, i);
		ids[i] = Int32GetDatum(LWLockNewTrancheId(name));
	}
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(ids, n, INT4OID));
}

/*
 * test_wait_totals_emit_lwlock(tranche_ids, reps): reps synthetic completed
 * waits for each tranche, through the same timed pair a contended LWLock
 * acquisition uses.  Only tranches that exist may be passed.
 */
Datum
test_wait_totals_emit_lwlock(PG_FUNCTION_ARGS)
{
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(0);
	int32		reps = PG_GETARG_INT32(1);
	Datum	   *ids;
	int			nids;
	int			i;
	int32		r;

	twt_check_count(reps, 0);
	deconstruct_array_builtin(arr, INT4OID, &ids, NULL, &nids);
	for (i = 0; i < nids; i++)
	{
		uint32		info = PG_WAIT_LWLOCK | (uint32) DatumGetInt32(ids[i]);

		for (r = 0; r < reps; r++)
		{
			pgstat_report_wait_start_timed(info);
			twt_spin_us(1);
			pgstat_report_wait_end_timed();
			CHECK_FOR_INTERRUPTS();
		}
	}
	PG_RETURN_VOID();
}

static void
twt_init_shared(void *ptr, void *arg)
{
	TwtShared  *s = (TwtShared *) ptr;
	int			i;

	for (i = 0; i < TWT_NLOCKS; i++)
	{
		char		name[NAMEDATALEN];

		snprintf(name, sizeof(name), "test_wait_totals_lock_%d", i);
		s->tranche[i] = LWLockNewTrancheId(name);
		LWLockInitialize(&s->locks[i], s->tranche[i]);
		pg_atomic_init_u32(&s->holder_pid[i], 0);
	}
	pg_atomic_init_u32(&s->gate, 0);
	pg_atomic_init_u32(&s->holding, 0);
	pg_atomic_init_u32(&s->stop, 0);
	pg_atomic_init_u32(&s->writers, 0);
	pg_atomic_init_u32(&s->progress, 0);
	pg_atomic_init_u32(&s->worker_procno1, 0);
}

static TwtShared *
twt_get_shared(void)
{
	bool		found;

	if (twt_shared == NULL)
		twt_shared = GetNamedDSMSegment(TWT_SHARED_NAME, sizeof(TwtShared),
										twt_init_shared, &found, NULL);
	return twt_shared;
}

static int
twt_check_lock(int32 idx)
{
	if (idx < 0 || idx >= TWT_NLOCKS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lock index must be between 0 and %d", TWT_NLOCKS - 1)));
	return idx;
}

/* test_wait_totals_lock_tranche(idx) -> the tranche id of test lock idx */
Datum
test_wait_totals_lock_tranche(PG_FUNCTION_ARGS)
{
	int			idx = twt_check_lock(PG_GETARG_INT32(0));

	PG_RETURN_INT32(twt_get_shared()->tranche[idx]);
}

/*
 * test_wait_totals_lock_hold(idx, waiters, timeout_ms): take test lock idx
 * exclusively and keep it until `waiters` other processes are queued on it
 * (seen through their published wait event) or the timeout passes; return
 * how many were queued.  LWLocks hold off interrupts, so the loop polls
 * with short sleeps and always ends.
 */
Datum
test_wait_totals_lock_hold(PG_FUNCTION_ARGS)
{
	TwtShared  *s = twt_get_shared();
	int			idx = twt_check_lock(PG_GETARG_INT32(0));
	int32		want = PG_GETARG_INT32(1);
	int32		timeout_ms = PG_GETARG_INT32(2);
	uint32		info = PG_WAIT_LWLOCK | (uint32) s->tranche[idx];
	instr_time	start,
				now;
	int			seen = 0;

	LWLockAcquire(&s->locks[idx], LW_EXCLUSIVE);
	pg_atomic_write_u32(&s->holder_pid[idx], (uint32) MyProcPid);

	INSTR_TIME_SET_CURRENT(start);
	for (;;)
	{
		int			i;

		seen = 0;
		for (i = 0; i < ProcGlobal->allProcCount; i++)
		{
			PGPROC	   *proc = GetPGProcByNumber(i);

			if (proc != MyProc && proc->pid != 0 &&
				proc->wait_event_info == info)
				seen++;
		}
		if (seen >= want)
			break;

		now = start;
		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, start);
		if (INSTR_TIME_GET_MILLISEC(now) >= timeout_ms)
			break;
		pg_usleep(1000);
	}

	pg_atomic_write_u32(&s->holder_pid[idx], 0);
	LWLockRelease(&s->locks[idx]);
	PG_RETURN_INT32(seen);
}

/* test_wait_totals_lock_holder(idx) -> pid holding test lock idx, or 0 */
Datum
test_wait_totals_lock_holder(PG_FUNCTION_ARGS)
{
	TwtShared  *s = twt_get_shared();
	int			idx = twt_check_lock(PG_GETARG_INT32(0));

	PG_RETURN_INT32((int32) pg_atomic_read_u32(&s->holder_pid[idx]));
}

/* test_wait_totals_lock_wait(idx): acquire and release test lock idx. */
Datum
test_wait_totals_lock_wait(PG_FUNCTION_ARGS)
{
	TwtShared  *s = twt_get_shared();
	int			idx = twt_check_lock(PG_GETARG_INT32(0));

	LWLockAcquire(&s->locks[idx], LW_EXCLUSIVE);
	LWLockRelease(&s->locks[idx]);
	PG_RETURN_VOID();
}

/* test_wait_totals_gate_reset(): clear all handshake flags. */
Datum
test_wait_totals_gate_reset(PG_FUNCTION_ARGS)
{
	TwtShared  *s = twt_get_shared();

	pg_atomic_write_u32(&s->gate, 0);
	pg_atomic_write_u32(&s->holding, 0);
	pg_atomic_write_u32(&s->stop, 0);
	pg_atomic_write_u32(&s->writers, 0);
	pg_atomic_write_u32(&s->progress, 0);
	PG_RETURN_VOID();
}

/* test_wait_totals_holding() -> is an emitter inside its CPU stretch? */
Datum
test_wait_totals_holding(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(pg_atomic_read_u32(&twt_get_shared()->holding) != 0);
}

/* test_wait_totals_release(): let the held emitter leave its CPU stretch. */
Datum
test_wait_totals_release(PG_FUNCTION_ARGS)
{
	pg_atomic_write_u32(&twt_get_shared()->gate, 1);
	PG_RETURN_VOID();
}

/*
 * test_wait_totals_emit_hold(n, spin_us): n pairs, then stay in an
 * interruptible CPU loop without any wait until test_wait_totals_release()
 * is called.  holding is set once the loop is entered, so an observer that
 * sees it knows every observation it makes next happens inside the stretch.
 * The shared state is looked up first: its lookup may take a lock.
 */
Datum
test_wait_totals_emit_hold(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	int32		spin_us = PG_GETARG_INT32(1);
	TwtShared  *s = twt_get_shared();
	instr_time	start,
				now;

	twt_check_count(n, spin_us);
	(void) twt_get_event();
	twt_emit(n, spin_us);

	INSTR_TIME_SET_CURRENT(start);
	pg_atomic_write_u32(&s->holding, 1);
	while (pg_atomic_read_u32(&s->gate) == 0)
	{
		now = start;
		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, start);
		if (INSTR_TIME_GET_MILLISEC(now) > TWT_WATCHDOG_MS)
		{
			pg_atomic_write_u32(&s->holding, 0);
			ereport(ERROR,
					(errmsg("test_wait_totals_emit_hold: never released")));
		}
		PG_TRY();
		{
			CHECK_FOR_INTERRUPTS();
		}
		PG_CATCH();
		{
			pg_atomic_write_u32(&s->holding, 0);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	pg_atomic_write_u32(&s->holding, 0);
	PG_RETURN_VOID();
}

/*
 * test_wait_totals_emit_until_stopped(spin_us) -> bigint: emit pairs
 * (in batches) until test_wait_totals_stop(); return how many.
 */
Datum
test_wait_totals_emit_until_stopped(PG_FUNCTION_ARGS)
{
	int32		spin_us = PG_GETARG_INT32(0);
	TwtShared  *s = twt_get_shared();
	instr_time	start,
				now;
	int64		count = 0;

	twt_check_count(0, spin_us);
	(void) twt_get_event();
	INSTR_TIME_SET_CURRENT(start);
	/* the watchdog and the stop flag also end a long spin, not only a pair */
	twt_deadline = start;
	INSTR_TIME_ADD_NANOSEC(twt_deadline, (int64) TWT_WATCHDOG_MS * 1000000);
	twt_have_deadline = true;
	twt_spin_stop = &s->stop;
	pg_atomic_fetch_add_u32(&s->writers, 1);
	PG_TRY();
	{
		for (;;)
		{
			twt_emit(1, spin_us);
			count++;
			if (count % 1000 == 0)
				pg_atomic_fetch_add_u32(&s->progress, 1000);
			if (pg_atomic_read_u32(&s->stop) != 0)
			{
				pg_atomic_fetch_add_u32(&s->progress, (uint32) (count % 1000));
				break;
			}
			now = start;
			INSTR_TIME_SET_CURRENT(now);
			INSTR_TIME_SUBTRACT(now, start);
			if (INSTR_TIME_GET_MILLISEC(now) > TWT_WATCHDOG_MS)
				ereport(ERROR,
						(errmsg("test_wait_totals_emit_until_stopped: never stopped")));
		}
	}
	PG_FINALLY();
	{
		twt_spin_stop = NULL;
		twt_have_deadline = false;
	}
	PG_END_TRY();
	PG_RETURN_INT64(count);
}

/* test_wait_totals_wait_writers(n): wait until n writers have started. */
Datum
test_wait_totals_wait_writers(PG_FUNCTION_ARGS)
{
	uint32		want = (uint32) PG_GETARG_INT32(0);
	TwtShared  *s = twt_get_shared();
	instr_time	start,
				now;

	INSTR_TIME_SET_CURRENT(start);
	while (pg_atomic_read_u32(&s->writers) < want)
	{
		CHECK_FOR_INTERRUPTS();
		now = start;
		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, start);
		if (INSTR_TIME_GET_MILLISEC(now) > TWT_WATCHDOG_MS)
			ereport(ERROR,
					(errmsg("test_wait_totals_wait_writers: writers never started")));
		pg_usleep(1000);
	}
	PG_RETURN_VOID();
}

Datum
test_wait_totals_stop(PG_FUNCTION_ARGS)
{
	pg_atomic_write_u32(&twt_get_shared()->stop, 1);
	PG_RETURN_VOID();
}

/*
 * test_wait_totals_emit_with_capture(mode, tranche_ids, reps): set the
 * capture level, emit synthetic LWLock waits, and put the previous level back
 * before returning to SQL.  With capture off elsewhere, nothing but these
 * emissions can be recorded in between, since no other statement and no
 * wait of this process separates the three steps.
 */
Datum
test_wait_totals_emit_with_capture(PG_FUNCTION_ARGS)
{
	char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nestlevel = NewGUCNestLevel();

	/*
	 * A nested GUC scope: leaving it restores the previous value together
	 * with its scope (a SET LOCAL stays a SET LOCAL).
	 */
	(void) set_config_option("pg_wait_event_tracing.capture", mode,
							 PGC_SUSET, PGC_S_SESSION, GUC_ACTION_SAVE,
							 true, 0, false);
	DirectFunctionCall2(test_wait_totals_emit_lwlock,
						PG_GETARG_DATUM(1), PG_GETARG_DATUM(2));
	AtEOXact_GUC(true, nestlevel);
	PG_RETURN_VOID();
}

/*
 * test_wait_totals_emit_then_set_capture(n, mode): n pairs, then change the
 * capture level before returning to SQL, so the last completion is still
 * pending when the assign hook runs.
 */
Datum
test_wait_totals_emit_then_set_capture(PG_FUNCTION_ARGS)
{
	int32		n = PG_GETARG_INT32(0);
	char	   *mode = text_to_cstring(PG_GETARG_TEXT_PP(1));

	twt_check_count(n, 0);
	(void) twt_get_event();
	twt_emit(n, 1);
	SetConfigOption("pg_wait_event_tracing.capture", mode, PGC_SUSET,
					PGC_S_SESSION);
	PG_RETURN_VOID();
}

/* test_wait_totals_emit_event(event_key, n): n pairs of an arbitrary code. */
Datum
test_wait_totals_emit_event(PG_FUNCTION_ARGS)
{
	uint32		info = (uint32) PG_GETARG_INT64(0);
	int32		n = PG_GETARG_INT32(1);
	int32		i;

	twt_check_count(n, 0);
	for (i = 0; i < n; i++)
	{
		pgstat_report_wait_start_timed(info);
		twt_spin_us(1);
		pgstat_report_wait_end_timed();
		CHECK_FOR_INTERRUPTS();
	}
	PG_RETURN_VOID();
}

/*
 * test_wait_totals_wait_progress(prev) -> bigint: wait until the writers'
 * progress counter exceeds prev and return it, so two samples taken after
 * successive calls are separated by known writer progress.
 */
Datum
test_wait_totals_wait_progress(PG_FUNCTION_ARGS)
{
	uint32		prev = (uint32) PG_GETARG_INT64(0);
	TwtShared  *s = twt_get_shared();
	instr_time	start,
				now;
	uint32		cur;

	INSTR_TIME_SET_CURRENT(start);
	while ((cur = pg_atomic_read_u32(&s->progress)) <= prev)
	{
		CHECK_FOR_INTERRUPTS();
		now = start;
		INSTR_TIME_SET_CURRENT(now);
		INSTR_TIME_SUBTRACT(now, start);
		if (INSTR_TIME_GET_MILLISEC(now) > TWT_WATCHDOG_MS)
			ereport(ERROR,
					(errmsg("test_wait_totals_wait_progress: writers made no progress")));
		pg_usleep(100);
	}
	PG_RETURN_INT64((int64) cur);
}

/* test_wait_totals_bgworker_procnumber() -> ProcNumber of the last worker, or -1 */
Datum
test_wait_totals_bgworker_procnumber(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32((int32) pg_atomic_read_u32(&twt_get_shared()->worker_procno1) - 1);
}
