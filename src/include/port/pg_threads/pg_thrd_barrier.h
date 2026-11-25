/*-------------------------------------------------------------------------
 *
 * pg_thrd_barrier.h
 *    Fallback implementation of pg_thrd_barrier_t.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/pg_thrd_barrier.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THRD_BARRIER_H
#define PG_THRD_BARRIER_H

#ifndef PG_THREADS_H
#error "include pg_thread.h instead"
#endif

typedef struct pg_thrd_barrier_impl
{
	bool		sense;
	int			expected;
	int			arrived;
	pg_mtx_impl mutex;
	pg_cnd_impl cond;
} pg_thrd_barrier_impl;

static inline int
pg_thrd_barrier_init(pg_thrd_barrier_impl *barrier, int count)
{
	barrier->sense = false;
	barrier->expected = count;
	barrier->arrived = 0;
	if (pg_cnd_init(&barrier->cond) != pg_thrd_success_impl)
		return pg_thrd_error_impl;
	if (pg_mtx_init(&barrier->mutex, pg_mtx_plain_impl) != pg_thrd_success_impl)
	{
		pg_cnd_destroy(&barrier->cond);
		return pg_thrd_error_impl;
	}
	return pg_thrd_success_impl;
}

static inline int
pg_thrd_barrier_wait(pg_thrd_barrier_impl *barrier)
{
	bool		initial_sense;

	pg_mtx_lock(&barrier->mutex);
	barrier->arrived++;
	if (barrier->arrived == barrier->expected)
	{
		barrier->arrived = 0;
		barrier->sense = !barrier->sense;
		pg_mtx_unlock(&barrier->mutex);
		pg_cnd_broadcast(&barrier->cond);
		return pg_thrd_success_impl;
	}
	initial_sense = barrier->sense;
	do
	{
		pg_cnd_wait(&barrier->cond, &barrier->mutex);
	} while (barrier->sense == initial_sense);
	pg_mtx_unlock(&barrier->mutex);
	return pg_thrd_success_impl;
}

static inline void
pg_thrd_barrier_destroy(pg_thrd_barrier_impl *barrier)
{
	pg_mtx_destroy(&barrier->mutex);
	pg_cnd_destroy(&barrier->cond);
}

#endif
