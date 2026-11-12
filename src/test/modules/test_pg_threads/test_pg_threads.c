/*-------------------------------------------------------------------------
 *
 * test_pg_threads.c
 *	  Test the behavior of port/pg_threads.h and port/pg_threads_ext.h.
 *
 * This is a standalone frontend program, because starting threads inside a
 * backend isn't supported.  It checks the results that callers depend on,
 * not the details of any one implementation, so the same tests run against
 * the <pthread.h>, <threads.h>, and <windows.h> mappings.  Each failure is
 * reported on stdout, and the exit status is nonzero if any test failed.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_pg_threads/test_pg_threads.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <time.h>

#include "port/pg_threads_ext.h"

#define NTHREADS 8

static int	failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) \
		{ \
			printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

/* A deadline "ms" milliseconds from now, for the timed functions. */
static struct timespec
deadline_after_ms(int ms)
{
	struct timespec ts;

	timespec_get(&ts, TIME_UTC);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long) (ms % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000)
	{
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	return ts;
}

/*
 * Thread creation and joining: each thread's return value must come back
 * from pg_thrd_join(), and pg_thrd_current() must identify the thread.
 */
static int
return_arg(void *arg)
{
	return (int) (intptr_t) arg;
}

static int
check_current(void *arg)
{
	pg_thrd_t  *main_thread = arg;

	return pg_thrd_equal(pg_thrd_current(), *main_thread) ? 1 : 0;
}

static void
test_create_join(void)
{
	pg_thrd_t	threads[NTHREADS];
	pg_thrd_t	self = pg_thrd_current();
	pg_thrd_t	other;
	int			result;

	CHECK(pg_thrd_equal(self, pg_thrd_current()));

	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_create(&threads[i], return_arg,
							 (void *) (intptr_t) (i + 100)) == pg_thrd_success);
	for (int i = 0; i < NTHREADS; i++)
	{
		result = -1;
		CHECK(pg_thrd_join(threads[i], &result) == pg_thrd_success);
		CHECK(result == i + 100);
	}

	/* Another thread must not compare equal to this one. */
	CHECK(pg_thrd_create(&other, check_current, &self) == pg_thrd_success);
	CHECK(pg_thrd_join(other, &result) == pg_thrd_success);
	CHECK(result == 0);
}

/* A detached thread must be able to run to completion on its own. */
static pg_mtx_t detach_mutex = PG_MTX_PLAIN_INIT;
static pg_cnd_t detach_cond = PG_CND_INIT;
static bool detach_ran = false;

static int
detached_thread(void *arg)
{
	pg_mtx_lock(&detach_mutex);
	detach_ran = true;
	pg_cnd_signal(&detach_cond);
	pg_mtx_unlock(&detach_mutex);
	return 0;
}

static void
test_detach(void)
{
	pg_thrd_t	thread;

	CHECK(pg_thrd_create(&thread, detached_thread, NULL) == pg_thrd_success);
	CHECK(pg_thrd_detach(thread) == pg_thrd_success);

	pg_mtx_lock(&detach_mutex);
	while (!detach_ran)
		pg_cnd_wait(&detach_cond, &detach_mutex);
	pg_mtx_unlock(&detach_mutex);
	CHECK(detach_ran);
}

/*
 * Mutexes: many threads incrementing a counter under a lock must not lose
 * updates, trylock must report a held lock as busy, and a recursive mutex
 * must allow its owner to relock it.
 */
static pg_mtx_t counter_mutex;
static int	counter;

#define INCREMENTS 10000

static int
increment_counter(void *arg)
{
	for (int i = 0; i < INCREMENTS; i++)
	{
		pg_mtx_lock(&counter_mutex);
		counter++;
		pg_mtx_unlock(&counter_mutex);
	}
	return 0;
}

static int
try_lock_counter_mutex(void *arg)
{
	int			result = pg_mtx_trylock(&counter_mutex);

	if (result == pg_thrd_success)
		pg_mtx_unlock(&counter_mutex);
	return result;
}

static void
test_mutex(void)
{
	pg_thrd_t	threads[NTHREADS];
	pg_thrd_t	thread;
	pg_mtx_t	recursive;
	int			result;

	CHECK(pg_mtx_init(&counter_mutex, pg_mtx_plain) == pg_thrd_success);

	counter = 0;
	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_create(&threads[i], increment_counter, NULL) == pg_thrd_success);
	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_join(threads[i], NULL) == pg_thrd_success);
	CHECK(counter == NTHREADS * INCREMENTS);

	/* While we hold the mutex, another thread's trylock must fail. */
	CHECK(pg_mtx_lock(&counter_mutex) == pg_thrd_success);
	CHECK(pg_thrd_create(&thread, try_lock_counter_mutex, NULL) == pg_thrd_success);
	CHECK(pg_thrd_join(thread, &result) == pg_thrd_success);
	CHECK(result == pg_thrd_busy);
	CHECK(pg_mtx_unlock(&counter_mutex) == pg_thrd_success);

	/* Once released, it must succeed. */
	CHECK(pg_thrd_create(&thread, try_lock_counter_mutex, NULL) == pg_thrd_success);
	CHECK(pg_thrd_join(thread, &result) == pg_thrd_success);
	CHECK(result == pg_thrd_success);

	pg_mtx_destroy(&counter_mutex);

	/*
	 * The owner of a recursive mutex may lock it again.  Use trylock for the
	 * relock, so that a mutex that isn't really recursive fails the check
	 * instead of deadlocking.
	 */
	CHECK(pg_mtx_init(&recursive, pg_mtx_recursive) == pg_thrd_success);
	CHECK(pg_mtx_lock(&recursive) == pg_thrd_success);
	if (pg_mtx_trylock(&recursive) == pg_thrd_success)
	{
		CHECK(pg_mtx_lock(&recursive) == pg_thrd_success);
		CHECK(pg_mtx_unlock(&recursive) == pg_thrd_success);
		CHECK(pg_mtx_unlock(&recursive) == pg_thrd_success);
	}
	else
		CHECK(!"pg_mtx_recursive mutex could not be relocked");
	CHECK(pg_mtx_unlock(&recursive) == pg_thrd_success);
	pg_mtx_destroy(&recursive);

	/*
	 * Unknown type flags must be rejected.  C11 doesn't say what mtx_init()
	 * does with them, so a system <threads.h> may accept them.
	 */
#ifndef PG_THREADS_USE_THREADS_H
	CHECK(pg_mtx_init(&recursive, 0x4000) == pg_thrd_error);
#endif

#ifdef PG_MTX_TIMED_NOT_SUPPORTED
	CHECK(pg_mtx_init(&recursive, pg_mtx_timed) == pg_thrd_error);
#else
	{
		pg_mtx_t	timed;
		struct timespec deadline;

		CHECK(pg_mtx_init(&timed, pg_mtx_timed) == pg_thrd_success);
		deadline = deadline_after_ms(10);
		CHECK(pg_mtx_timedlock(&timed, &deadline) == pg_thrd_success);
		CHECK(pg_mtx_unlock(&timed) == pg_thrd_success);
		pg_mtx_destroy(&timed);
	}
#endif
}

/*
 * Condition variables: a timed wait with nobody signaling must time out,
 * and a broadcast must wake every waiter.
 */
static pg_mtx_t cv_mutex = PG_MTX_PLAIN_INIT;
static pg_cnd_t cv = PG_CND_INIT;
static bool cv_go = false;
static int	cv_waiting = 0;
static int	cv_woken = 0;

static int
cv_waiter(void *arg)
{
	pg_mtx_lock(&cv_mutex);
	cv_waiting++;
	while (!cv_go)
		pg_cnd_wait(&cv, &cv_mutex);
	cv_woken++;
	pg_mtx_unlock(&cv_mutex);
	return 0;
}

static void
test_condition_variable(void)
{
	pg_thrd_t	threads[NTHREADS];
	struct timespec deadline;
	int			result;

	/* Nobody signals, so this must time out, allowing for spurious wakeups. */
	pg_mtx_lock(&cv_mutex);
	deadline = deadline_after_ms(10);
	for (int i = 0; i < 100; i++)
	{
		result = pg_cnd_timedwait(&cv, &cv_mutex, &deadline);
		if (result != pg_thrd_success)
			break;
	}
	CHECK(result == pg_thrd_timedout);
	pg_mtx_unlock(&cv_mutex);

	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_create(&threads[i], cv_waiter, NULL) == pg_thrd_success);

	/* Wait for all of them to be waiting, then wake them all. */
	for (;;)
	{
		struct timespec delay = {0, 1000000};
		bool		ready;

		pg_mtx_lock(&cv_mutex);
		ready = cv_waiting == NTHREADS;
		if (ready)
		{
			cv_go = true;
			CHECK(pg_cnd_broadcast(&cv) == pg_thrd_success);
		}
		pg_mtx_unlock(&cv_mutex);
		if (ready)
			break;
		pg_thrd_sleep(&delay, NULL);
	}

	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_join(threads[i], NULL) == pg_thrd_success);
	CHECK(cv_woken == NTHREADS);
}

/* pg_call_once() must run its function exactly once, despite racing. */
static pg_once_flag test_once_flag = PG_ONCE_FLAG_INIT;
static pg_mtx_t once_mutex = PG_MTX_PLAIN_INIT;
static int	once_calls = 0;

static void
once_function(void)
{
	pg_mtx_lock(&once_mutex);
	once_calls++;
	pg_mtx_unlock(&once_mutex);
}

static int
call_once_thread(void *arg)
{
	pg_call_once(&test_once_flag, once_function);
	return 0;
}

static void
test_call_once(void)
{
	pg_thrd_t	threads[NTHREADS];

	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_create(&threads[i], call_once_thread, NULL) == pg_thrd_success);
	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_join(threads[i], NULL) == pg_thrd_success);
	CHECK(once_calls == 1);
}

/*
 * Thread-specific storage: each thread sees only its own value, and the
 * destructor runs at thread exit for non-NULL values only.
 */
static pg_tss_t tss_key;
static pg_mtx_t tss_mutex = PG_MTX_PLAIN_INIT;
static int	tss_destructor_calls = 0;
static int	tss_values[NTHREADS];

static void
tss_destructor(void *value)
{
	pg_mtx_lock(&tss_mutex);
	tss_destructor_calls++;
	pg_mtx_unlock(&tss_mutex);
}

static int
tss_thread(void *arg)
{
	int		   *mine = arg;

	if (pg_tss_get(tss_key) != NULL)
		return 1;
	if (pg_tss_set(tss_key, mine) != pg_thrd_success)
		return 2;
	pg_thrd_yield();
	if (pg_tss_get(tss_key) != mine)
		return 3;
	return 0;
}

static int
tss_thread_null(void *arg)
{
	return pg_tss_set(tss_key, NULL) == pg_thrd_success ? 0 : 1;
}

static void
test_tss(void)
{
	pg_thrd_t	threads[NTHREADS];
	pg_thrd_t	thread;
	int			result;

	CHECK(pg_tss_create(&tss_key, tss_destructor) == pg_thrd_success);

	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_create(&threads[i], tss_thread, &tss_values[i]) == pg_thrd_success);
	for (int i = 0; i < NTHREADS; i++)
	{
		result = -1;
		CHECK(pg_thrd_join(threads[i], &result) == pg_thrd_success);
		CHECK(result == 0);
	}
	CHECK(tss_destructor_calls == NTHREADS);

	/* No destructor call for a NULL value. */
	CHECK(pg_thrd_create(&thread, tss_thread_null, NULL) == pg_thrd_success);
	CHECK(pg_thrd_join(thread, &result) == pg_thrd_success);
	CHECK(result == 0);
	CHECK(tss_destructor_calls == NTHREADS);

	pg_tss_delete(tss_key);
}

/*
 * Barriers: every waiter must see success, and nobody may pass the barrier
 * before all have arrived.
 */
static pg_thrd_barrier_t barrier;
static pg_mtx_t barrier_mutex = PG_MTX_PLAIN_INIT;
static int	barrier_arrived = 0;

#define BARRIER_ROUNDS 20

/*
 * Returns a bitmask of the problems seen.  A thread keeps going after a
 * failure, so that every thread makes the same number of waits and nobody is
 * left stranded at the barrier.
 */
static int
barrier_thread(void *arg)
{
	int			problems = 0;

	for (int round = 0; round < BARRIER_ROUNDS; round++)
	{
		int			arrived;

		pg_mtx_lock(&barrier_mutex);
		arrived = ++barrier_arrived;
		pg_mtx_unlock(&barrier_mutex);
		if (arrived > (round + 1) * NTHREADS)
			problems |= 1;

		if (pg_thrd_barrier_wait(&barrier) != pg_thrd_success)
			problems |= 2;

		pg_mtx_lock(&barrier_mutex);
		arrived = barrier_arrived;
		pg_mtx_unlock(&barrier_mutex);
		if (arrived < (round + 1) * NTHREADS)
			problems |= 4;

		/* Keep the next round from starting before everyone has checked. */
		if (pg_thrd_barrier_wait(&barrier) != pg_thrd_success)
			problems |= 8;
	}
	return problems;
}

static void
test_barrier(void)
{
	pg_thrd_t	threads[NTHREADS];
	int			result;

	CHECK(pg_thrd_barrier_init(&barrier, NTHREADS) == pg_thrd_success);
	for (int i = 0; i < NTHREADS; i++)
		CHECK(pg_thrd_create(&threads[i], barrier_thread, NULL) == pg_thrd_success);
	for (int i = 0; i < NTHREADS; i++)
	{
		result = -1;
		CHECK(pg_thrd_join(threads[i], &result) == pg_thrd_success);
		CHECK(result == 0);
	}
	pg_thrd_barrier_destroy(&barrier);
}

/*
 * Reader-writer locks: readers may share, a writer excludes everyone, and
 * the static initializer must produce a working lock.
 */
static pg_rwlock_t static_rwlock = PG_RWLOCK_PLAIN_INIT;
static pg_rwlock_t rwlock;

static int
try_rwlock(void *arg)
{
	bool		write = arg != NULL;
	int			result;

	result = write ? pg_rwlock_trywrlock(&rwlock) : pg_rwlock_tryrdlock(&rwlock);
	if (result == pg_thrd_success)
		pg_rwlock_unlock(&rwlock);
	return result;
}

static int
run_try_rwlock(bool write)
{
	pg_thrd_t	thread;
	int			result = -1;

	CHECK(pg_thrd_create(&thread, try_rwlock, write ? (void *) 1 : NULL) == pg_thrd_success);
	CHECK(pg_thrd_join(thread, &result) == pg_thrd_success);
	return result;
}

static void
test_rwlock(void)
{
	CHECK(pg_rwlock_rdlock(&static_rwlock) == pg_thrd_success);
	CHECK(pg_rwlock_unlock(&static_rwlock) == pg_thrd_success);
	CHECK(pg_rwlock_wrlock(&static_rwlock) == pg_thrd_success);
	CHECK(pg_rwlock_unlock(&static_rwlock) == pg_thrd_success);

	CHECK(pg_rwlock_init(&rwlock, pg_rwlock_plain) == pg_thrd_success);

	/* Readers share. */
	CHECK(pg_rwlock_rdlock(&rwlock) == pg_thrd_success);
	CHECK(run_try_rwlock(false) == pg_thrd_success);
	CHECK(run_try_rwlock(true) == pg_thrd_busy);
	CHECK(pg_rwlock_unlock(&rwlock) == pg_thrd_success);

	/* A writer excludes readers and writers. */
	CHECK(pg_rwlock_wrlock(&rwlock) == pg_thrd_success);
	CHECK(run_try_rwlock(false) == pg_thrd_busy);
	CHECK(run_try_rwlock(true) == pg_thrd_busy);
	CHECK(pg_rwlock_unlock(&rwlock) == pg_thrd_success);

	CHECK(run_try_rwlock(true) == pg_thrd_success);

	pg_rwlock_destroy(&rwlock);
}

/* Errors must come back with a usable description. */
static void
test_error_strings(void)
{
	pg_rwlock_t lock;
	int			result;

	CHECK(strcmp(pg_thrd_error_string(pg_thrd_success), "success") == 0);
	CHECK(strcmp(pg_thrd_error_string(pg_thrd_busy), "busy") == 0);

	/* pg_rwlock_t is our own extension, so its error is ours too. */
	result = pg_rwlock_init(&lock, 0x4000);
	CHECK(result == pg_thrd_error);
	CHECK(pg_thrd_error_string_with_detail(result) != NULL);
	CHECK(strlen(pg_thrd_error_string_with_detail(result)) > 0);
}

int
main(int argc, char **argv)
{
	pg_threads_ext_assertions();

	test_create_join();
	test_detach();
	test_mutex();
	test_condition_variable();
	test_call_once();
	test_tss();
	test_barrier();
	test_rwlock();
	test_error_strings();

	if (failures > 0)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	return 0;
}
