/*-------------------------------------------------------------------------
 *
 * pg_threads.h
 *    Portable implementation of C11 <threads.h> with pg_ prefixes.
 *
 * This provides a minimal multithreading API that works on all our target
 * systems.  By following <threads.h> even when we're not using it directly,
 * we avoid the need to make up our own names and details, and also gain some
 * future-proofing.  Adding a prefix avoids collision with <threads.h>, if
 * included by client programs.
 *
 * See also pg_threads_ext.h, which adds a small number of extra facilities in
 * the same style, most usefully static initializers for mutexes.  This header
 * restricts itself to faithfully copying <threads.h>.
 *
 * Three implementations are available:
 *
 * (1) map_pthread.h for <pthread.h> on POSIX systems
 * (2) map_threads.h for <threads.h> on Windows + Visual Studio
 * (3) map_windows.h for <windows.h> on Windows
 *
 * The choice is made automatically, but can be overridden for testing.
 * PG_THREADS_USE_THREADS_H works on common POSIX systems, but such builds are
 * intended strictly for testing (see map_threads_ext.h for details).
 *
 * The following macros are defined to signal known failures to conform to the
 * <threads.h> specification on some systems.  See the implementation headers
 * for details.
 *
 * - PG_MTX_TIMED_NOT_SUPPORTED:		macOS (1), Windows (3)
 * - PG_THRD_CURRENT_NOT_DETACHABLE:	Windows (2 and 3)
 * - PG_THRD_CURRENT_NOT_JOINABLE:		Windows (2 and 3)
 *
 * https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1548.pdf
 * https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/threads.h.html
 * https://en.cppreference.com/c/header/threads
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_H
#define PG_THREADS_H

/* select implementation, if not defined manually for testing. */
#if !defined(PG_THREADS_USE_PTHREAD_H) &&		\
	!defined(PG_THREADS_USE_THREADS_H) &&		\
	!defined(PG_THREADS_USE_WINDOWS_H)
#if !defined(WIN32)
#define PG_THREADS_USE_PTHREAD_H
#elif defined(_MSC_VER)
#define PG_THREADS_USE_THREADS_H
#else
#define PG_THREADS_USE_WINDOWS_H
#endif
#endif

#include <time.h>

/* standard thread_local macro if needed */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ < 202311L
#ifndef thread_local
#define thread_local _Thread_local
#endif
#endif

/* standard function pointer types with prefix */
typedef int (*pg_thrd_start_t) (void *);
typedef void (*pg_tss_dtor_t) (void *);

#if   defined(PG_THREADS_USE_PTHREAD_H)
#include "port/pg_threads/map_pthread.h"
#elif defined(PG_THREADS_USE_THREADS_H)
#include "port/pg_threads/map_threads.h"
#elif defined(PG_THREADS_USE_WINDOWS_H)
#include "port/pg_threads/map_windows.h"
#endif

/* standard types with prefix */
typedef pg_cnd_impl pg_cnd_t;
typedef pg_mtx_impl pg_mtx_t;
typedef pg_once_flag_impl pg_once_flag;
typedef pg_thrd_impl pg_thrd_t;
typedef pg_tss_impl pg_tss_t;

/* standard return values with prefix */
enum
{
	pg_thrd_success = pg_thrd_success_impl,
	pg_thrd_nomem = pg_thrd_nomem_impl,
	pg_thrd_timedout = pg_thrd_timedout_impl,
	pg_thrd_busy = pg_thrd_busy_impl,
	pg_thrd_error = pg_thrd_error_impl,
};

/* standard mtx_t type flags with prefix */
enum
{
	pg_mtx_plain = pg_mtx_plain_impl,
	pg_mtx_recursive = pg_mtx_plain_impl,
	pg_mtx_timed = pg_mtx_timed_impl,
};

/* standard macros with prefix */
#define PG_ONCE_FLAG_INIT		PG_ONCE_FLAG_INIT_IMPL
#define PG_TSS_DTOR_ITERATIONS	PG_TSS_DTOR_ITERARATIONS_IMPL

/* standard cnd_t functions with prefix */
static inline int pg_cnd_broadcast(pg_cnd_t *cnd);
static inline void pg_cnd_destroy(pg_cnd_t *cnd);
static inline int pg_cnd_init(pg_cnd_t *cnd);
static inline int pg_cnd_signal(pg_cnd_t *cnd);
static inline int pg_cnd_timedwait(pg_cnd_t *cnd, pg_mtx_t *mtx, const struct timespec *time);
static inline int pg_cnd_wait(pg_cnd_t *cnd, pg_mtx_t *mtx);

/* standard mtx_t functions with prefix */
static inline void pg_mtx_destroy(pg_mtx_t *mtx);
static inline int pg_mtx_init(pg_mtx_t *mtx, int type);
static inline int pg_mtx_lock(pg_mtx_t *mtx);
static inline int pg_mtx_timedlock(pg_mtx_t *mtx, const struct timespec *time);
static inline int pg_mtx_trylock(pg_mtx_t *mtx);
static inline int pg_mtx_unlock(pg_mtx_t *mtx);

/* standard once_flag functions with prefix */
static inline void pg_call_once(pg_once_flag *flag, void (*fun) (void));

/* standard thrd_t functions with prefix */
static inline int pg_thrd_create(pg_thrd_t *thr, pg_thrd_start_t fun, void *arg);
static inline pg_thrd_t pg_thrd_current(void);
static inline int pg_thrd_detach(pg_thrd_t thr);
static inline int pg_thrd_equal(pg_thrd_t thr0, pg_thrd_t thr1);
static inline void pg_thrd_exit(int result);
static inline int pg_thrd_join(pg_thrd_t thr, int *result);
static inline int pg_thrd_sleep(const struct timespec *duration, struct timespec *remaining);
static inline void pg_thrd_yield(void);

/* standard tss_t functions with prefix */
static inline int pg_tss_create(pg_tss_t *id, pg_tss_dtor_t);
static inline void pg_tss_delete(pg_tss_t id);
static inline void *pg_tss_get(pg_tss_t id);
static inline int pg_tss_set(pg_tss_t id, void *value);

#endif
