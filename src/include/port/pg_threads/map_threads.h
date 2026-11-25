/*-------------------------------------------------------------------------
 *
 * map_threads.h
 *    Map pg_threads.h API to system-provided <threads.h>.
 *
 * This header effectively renames all the identifiers in <threads.h> to add
 * pg_ prefixes, but does so with inlined wrapper functions rather than simple
 * renaming macros.  This provides better detection of omissions and
 * differences, and systemic mapping to system-provided <threads.h>.
 *
 * On POSIX systems, this is intended for developer-testing only.  See
 * map_threads_ext.h for explanation.
 *
 * References:
 * - https://devblogs.microsoft.com/cppblog/c11-threads-in-visual-studio-2022-version-17-8-previ
 * - https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/threads.h.html
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/map_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_MAP_THREADS
#define PG_THREADS_MAP_THREADS

#ifndef PG_THREADS_H
#error "include pg_threads.h instead"
#endif

#include <assert.h>
#include <threads.h>

#include "port/pg_threads/wrap.h"

#ifdef WIN32
/*
 * Visual Studio's <threads.h> has these limitations, per the above reference.
 * (See also map_windows.h, which copies this behavior.)
 */
#define PG_THRD_CURRENT_NOT_DETACHABLE
#define PG_THRD_CURRENT_NOT_JOINABLE
#endif

typedef void (*pg_call_once_function_impl) (void);

typedef cnd_t pg_cnd_impl;
typedef mtx_t pg_mtx_impl;
typedef once_flag pg_once_flag_impl;
typedef thrd_t pg_thrd_impl;
typedef tss_t pg_tss_impl;

enum
{
	pg_thrd_success_impl = thrd_success,
	pg_thrd_nomem_impl = thrd_nomem,
	pg_thrd_timedout_impl = thrd_timedout,
	pg_thrd_busy_impl = thrd_busy,
	pg_thrd_error_impl = thrd_error,
};

enum
{
	pg_mtx_plain_impl = mtx_plain,
	pg_mtx_recursive_impl = mtx_recursive,
	pg_mtx_timed_impl = mtx_timed,
};

#define PG_ONCE_FLAG_INIT_IMPL ONCE_FLAG_INIT
#define PG_TSS_DTOR_ITERATIONS_IMPL TSS_DTOR_ITERATIONS

/* Forward pg_##fun(...) -> R to fun(...) -> R. */
#define PG_THREADS_PREFIX(fun, R, ...)					\
	PG_THREADS_FORWARD(pg_##fun, fun, R, __VA_ARGS__)
#define PG_THREADS_PREFIX_0ARG(fun, R)					\
	PG_THREADS_FORWARD_0ARG(pg_##fun, fun, R)
#define PG_THREADS_PREFIX_VOID(fun, ...)				\
	PG_THREADS_FORWARD_VOID(pg_##fun, fun, __VA_ARGS__)
#define PG_THREADS_PREFIX_VOID_0ARG(fun)		\
	PG_THREADS_FORWARD_VOID_0ARG(pg_##fun, fun)

/* cnd_t */
PG_THREADS_PREFIX(cnd_broadcast, int, cnd_t *);
PG_THREADS_PREFIX_VOID(cnd_destroy, cnd_t *);
PG_THREADS_PREFIX(cnd_init, int, cnd_t *);
PG_THREADS_PREFIX(cnd_signal, int, cnd_t *);
PG_THREADS_PREFIX(cnd_timedwait, int, cnd_t *, mtx_t *, const struct timespec *);
PG_THREADS_PREFIX(cnd_wait, int, cnd_t *, mtx_t *);

/* mtx_t */
PG_THREADS_PREFIX(mtx_init, int, mtx_t *, int);
PG_THREADS_PREFIX(mtx_lock, int, mtx_t *);
PG_THREADS_PREFIX(mtx_trylock, int, mtx_t *);
PG_THREADS_PREFIX(mtx_timedlock, int, mtx_t *, const struct timespec *);
PG_THREADS_PREFIX(mtx_unlock, int, mtx_t *);
PG_THREADS_PREFIX_VOID(mtx_destroy, mtx_t *);

/* once_flag */
PG_THREADS_PREFIX_VOID(call_once, once_flag *, pg_call_once_function_impl);

/* thrd_t */
PG_THREADS_PREFIX_0ARG(thrd_current, thrd_t);
PG_THREADS_PREFIX(thrd_create, int, thrd_t *, pg_thrd_start_t, void *);
PG_THREADS_PREFIX(thrd_detach, int, thrd_t);
PG_THREADS_PREFIX(thrd_equal, int, thrd_t, thrd_t);
PG_THREADS_PREFIX_VOID(thrd_exit, int);
PG_THREADS_PREFIX(thrd_join, int, thrd_t, int *);
PG_THREADS_PREFIX(thrd_sleep, int, const struct timespec *, struct timespec *);
PG_THREADS_PREFIX_VOID_0ARG(thrd_yield);

/* tss_t */
PG_THREADS_PREFIX(tss_create, int, tss_t *, pg_tss_dtor_t);
PG_THREADS_PREFIX_VOID(tss_delete, tss_t);
PG_THREADS_PREFIX(tss_get, void *, tss_t);
PG_THREADS_PREFIX(tss_set, int, tss_t, void *);

#endif
