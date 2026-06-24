/*-------------------------------------------------------------------------
 *
 * map_pthread_ext.h
 *    Map pg_threads_ext.h to POSIX <pthread.h>.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/map_pthread_extensions.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_MAP_PTHREAD_EXT_H
#define PG_THREADS_MAP_PTHREAD_EXT_H

#ifndef PG_THREADS_EXT_H
#error "include pg_threads_ext.h instead"
#endif

#include <pthread.h>
#include <unistd.h>

#include "port/pg_threads/map_pthread_error.h"
#include "port/pg_threads/wrap.h"

#if !defined(_POSIX_TIMEOUTS) || _POSIX_TIMEOUTS < 0
/* macOS lacks _POSIX_TIMEOUTS (required since POSIX:2018) */
#define PG_RWLOCK_TIMED_NOT_SUPPORTED
#endif

typedef pthread_rwlock_t pg_rwlock_impl;

#if !defined(_POSIX_BARRIERS) || _POSIX_BARRIERS < 0
/* macOS lacks _POSIX_BARRIERS (required since POSIX:2018), so use fallback */
#include "port/pg_threads/pg_thrd_barrier.h"
#else
typedef pthread_barrier_t pg_thrd_barrier_impl;
#endif

enum
{
	pg_rwlock_plain_impl = 0,
	pg_rwlock_timed_impl = 1 << 1,
};

#ifndef PG_THREADS_USE_THREADS_H
#define PG_CND_INIT_IMPL			PTHREAD_COND_INITIALIZER
#define PG_MTX_PLAIN_INIT_IMPL		PTHREAD_MUTEX_INITIALIZER
#endif
#define PG_RWLOCK_INIT_IMPL			PTHREAD_RWLOCK_INITIALIZER

/* pg_rwlock_t */
PG_THREADS_FORWARD_VOID(pg_rwlock_destroy, pthread_rwlock_destroy, pg_rwlock_impl *);
PG_THREADS_OUTOFLINE(pg_rwlock_init, int, pg_rwlock_impl *, int);
PG_THREADS_MAP(pg_rwlock_rdlock, pthread_rwlock_rdlock, pg_rwlock_impl *);
#ifndef PG_RWLOCK_TIMED_NOT_SUPPORTED
PG_THREADS_MAP(pg_rwlock_timedrdlock, pthread_rwlock_timedrdlock, pg_rwlock_impl *, const struct timespec *);
PG_THREADS_MAP(pg_rwlock_timedwrlock, pthread_rwlock_timedwrlock, pg_rwlock_impl *, const struct timespec *);
#else
PG_THREADS_MAP_ERROR_UNSUPPORTED(pg_rwlock_timedrdlock, pg_rwlock_impl *, const struct timespec *);
PG_THREADS_MAP_ERROR_UNSUPPORTED(pg_rwlock_timedwrlock, pg_rwlock_impl *, const struct timespec *);
#endif
PG_THREADS_MAP(pg_rwlock_tryrdlock, pthread_rwlock_tryrdlock, pg_rwlock_impl *);
PG_THREADS_MAP(pg_rwlock_trywrlock, pthread_rwlock_trywrlock, pg_rwlock_impl *);
PG_THREADS_MAP(pg_rwlock_unlock, pthread_rwlock_unlock, pg_rwlock_impl *);
PG_THREADS_MAP(pg_rwlock_wrlock, pthread_rwlock_wrlock, pg_rwlock_impl *);

#if defined(_POSIX_BARRIERS) && _POSIX_BARRIERS > 0
/* pg_thrd_barrier_t */
PG_THREADS_FORWARD_VOID(pg_thrd_barrier_destroy, pthread_barrier_destroy, pg_thrd_barrier_impl *);

static inline int
pg_thrd_barrier_init(pg_thrd_barrier_impl *barrier, int count)
{
	return pg_thrd_map(pthread_barrier_init(barrier, NULL, count));
}

PG_THREADS_MAP(pg_thrd_barrier_wait, pthread_barrier_wait, pg_thrd_barrier_impl *);
#endif

#ifndef PG_THREADS_USE_THREADS_H
PG_THREADS_EMPTY_VOID_0ARG(pg_threads_ext_assertions);
#endif

#endif
