/*-------------------------------------------------------------------------
 *
 * pg_threads_ext.h
 *    Portable extensions to pg_threads.h.
 *
 * This header includes pg_threads.h but also declares some extensions on top
 * that are not based on the spartan <threads.h> standard.
 *
 * Three implementations are available:
 *
 * (1) map_pthread_ext.h for <pthread.h>
 * (2) map_threads_ext.h to forward to (1) or (3)
 * (3) map_windows_ext.h for <windows.h>
 *
 * The <threads.h> option is only available on Windows and a subset of POSIX
 * systems (see map_threads_ext.h for details) to support developer-only
 * testing.
 *
 * The following macros are defined on some platforms:
 *
 * - PG_RWLOCK_TIMED_NOT_SUPPORTED:		macOS, Windows
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads_ext.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_EXT_H
#define PG_THREADS_EXT_H

#ifndef PG_THREADS_EXT_H
#error "include pg_threads_ext.h instead"
#endif

#include "port/pg_threads.h"

#if defined(PG_THREADS_USE_PTHREAD_H)
#include "port/pg_threads/map_pthread_ext.h"
#elif defined(PG_THREADS_USE_THREADS_H)
#include "port/pg_threads/map_threads_ext.h"
#endif

typedef pg_rwlock_impl pg_rwlock_t;
typedef pg_thrd_barrier_impl pg_thrd_barrier_t;

/* type flags for pg_rwlock_t */
enum
{
	pg_rwlock_plain = pg_rwlock_plain_impl,
	pg_rwlock_timed = pg_rwlock_timed_impl,
	/* policy flags could potentially appear here */
};

/* static initializer macros */
#define PG_CND_INIT				PG_CND_INIT_IMPL
#define PG_MTX_PLAIN_INIT		PG_MTX_PLAIN_INIT_IMPL
#define PG_RWLOCK_PLAIN_INIT	PG_RWLOCK_INIT_IMPL

/* pg_rwlock_t functions */
static inline int pg_rwlock_init(pg_rwlock_t *lock, int type);
static inline int pg_rwlock_rdlock(pg_rwlock_t *lock);
static inline int pg_rwlock_timedrdlock(pg_rwlock_t *lock, const struct timespec *time);
static inline int pg_rwlock_timedwrlock(pg_rwlock_t *lock, const struct timespec *time);
static inline int pg_rwlock_tryrdlock(pg_rwlock_t *lock);
static inline int pg_rwlock_trywrlock(pg_rwlock_t *lock);
static inline int pg_rwlock_unlock(pg_rwlock_t *lock);
static inline int pg_rwlock_wrlock(pg_rwlock_t *lock);

/* pg_thrd_barrier_t functions */
static inline int pg_thrd_barrier_init(pg_thrd_barrier_t *barrier, int count);
static inline void pg_thrd_barrier_destroy(pg_thrd_barrier_t *barrier);
static inline int pg_thrd_barrier_wait(pg_thrd_barrier_t *barrier);

/* error string retrieval functions */
extern const char *pg_thrd_error_string(int error);
extern const char *pg_thrd_error_string_with_detail(int error);

/* internal self-test */
static inline void pg_threads_ext_assertions(void);

#endif
