/*-------------------------------------------------------------------------
 *
 * map_pthread.h
 *    Map pg_threads.h to POSIX <pthread.h>.
 *
 * <threads.h> is effectively <pthread.h>-light, and is also part of
 * POSIX:2024.  See especially its RATIONALE sections for discussion of how
 * the two interfaces interact.  In brief:
 *
 * - return value of thread functions is int rather than void pointer
 * - threads can't be canceled
 * - errors are simplified
 * - several useful facilities are omitted (but see pg_threads_ext.h)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/map_pthread.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_PTHREAD_H
#define PG_THREADS_PTHREAD_H

#ifndef PG_THREADS_H
#error "include pg_threads.h instead"
#endif

#include <pthread.h>
#include <unistd.h>

enum
{
	pg_thrd_success_impl = 0,
	pg_thrd_nomem_impl,
	pg_thrd_timedout_impl,
	pg_thrd_busy_impl,
	pg_thrd_error_impl,
};

/* These need the definition of pg_thrd_success_impl. */
#include "port/pg_threads/map_pthread_error.h"
#include "port/pg_threads/wrap.h"

#if !defined(_POSIX_TIMEOUTS) || _POSIX_TIMEOUTS < 0
/* macOS lacks _POSIX_TIMEOUTS (required since POSIX:2018) */
#define PG_MTX_TIMED_NOT_SUPPORTED
#endif

typedef void (*pg_call_once_function_impl) (void);

typedef pthread_cond_t pg_cnd_impl;
typedef pthread_mutex_t pg_mtx_impl;
typedef pthread_once_t pg_once_flag_impl;
typedef pthread_t pg_thrd_impl;
typedef pthread_key_t pg_tss_impl;

enum
{
	pg_mtx_plain_impl = 0,
	pg_mtx_recursive_impl = 1 << 1,
	pg_mtx_timed_impl = 2 << 2,
};

#define PG_ONCE_FLAG_INIT_IMPL		PTHREAD_ONCE_INIT
#ifdef PTHREAD_DESTRUCTOR_ITERATIONS
#define PG_TSS_DTOR_ITERATIONS_IMPL	PTHREAD_DESTRUCTOR_ITERATIONS
#else
#define PG_TSS_DTOR_ITERATIONS_IMPL	_POSIX_THREAD_DESTRUCTOR_ITERATIONS
#endif

/* cnd_t */
PG_THREADS_MAP(pg_cnd_broadcast, pthread_cond_broadcast, pg_cnd_impl *);
PG_THREADS_FORWARD_VOID(pg_cnd_destroy, pthread_cond_destroy, pg_cnd_impl *);
PG_THREADS_OUTOFLINE(pg_cnd_init, int, pg_cnd_impl *);
PG_THREADS_MAP(pg_cnd_signal, pthread_cond_signal, pg_cnd_impl *);
PG_THREADS_MAP(pg_cnd_timedwait, pthread_cond_timedwait, pg_cnd_impl *, pg_mtx_impl *, const struct timespec *);
PG_THREADS_MAP(pg_cnd_wait, pthread_cond_wait, pg_cnd_impl *, pg_mtx_impl *);

/* mtx_t */
PG_THREADS_FORWARD_VOID(pg_mtx_destroy, pthread_mutex_destroy, pg_mtx_impl *);
PG_THREADS_OUTOFLINE(pg_mtx_init, int, pg_mtx_impl *, int);
PG_THREADS_MAP(pg_mtx_lock, pthread_mutex_lock, pg_mtx_impl *);
#ifndef PG_MTX_TIMED_NOT_SUPPORTED
PG_THREADS_MAP(pg_mtx_timedlock, pthread_mutex_timedlock, pg_mtx_impl *, const struct timespec *);
#else
PG_THREADS_MAP_ERROR_UNSUPPORTED(pg_mtx_timedlock, pg_mtx_impl *, const struct timespec *);
#endif
PG_THREADS_MAP(pg_mtx_trylock, pthread_mutex_trylock, pg_mtx_impl *);
PG_THREADS_MAP(pg_mtx_unlock, pthread_mutex_unlock, pg_mtx_impl *);

/* once_flag */
PG_THREADS_FORWARD_VOID(pg_call_once, pthread_once, pg_once_flag_impl *, pg_call_once_function_impl);

/* thrd_t */
PG_THREADS_FORWARD_0ARG(pg_thrd_current, pthread_self, pg_thrd_impl);
PG_THREADS_OUTOFLINE(pg_thrd_create, int, pg_thrd_impl *, pg_thrd_start_t, void *);
PG_THREADS_MAP(pg_thrd_detach, pthread_detach, pg_thrd_impl);
PG_THREADS_OUTOFLINE_VOID(pg_thrd_exit, int);
PG_THREADS_FORWARD(pg_thrd_equal, pthread_equal, int, pg_thrd_impl, pg_thrd_impl);
PG_THREADS_OUTOFLINE(pg_thrd_join, int, pg_thrd_impl, int *);
PG_THREADS_FORWARD(pg_thrd_sleep, nanosleep, int, const struct timespec *, struct timespec *);
PG_THREADS_FORWARD_VOID_0ARG(pg_thrd_yield, sched_yield);

/* tss_t */
PG_THREADS_MAP(pg_tss_create, pthread_key_create, pg_tss_impl *, pg_tss_dtor_t);
PG_THREADS_FORWARD(pg_tss_get, pthread_getspecific, void *, pg_tss_impl);
PG_THREADS_MAP(pg_tss_set, pthread_setspecific, pg_tss_impl, void *);
PG_THREADS_FORWARD_VOID(pg_tss_delete, pthread_key_delete, pg_tss_impl);

#endif
