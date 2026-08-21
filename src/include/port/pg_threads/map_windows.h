/*-------------------------------------------------------------------------
 *
 * pg_threads/map_windows.h
 *    Map pg_threads.h interface to Windows native APIs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/map_windows.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_MAP_WINDOWS
#define PG_THREADS_MAP_WINDOWS

#ifndef PG_THREADS_H
#error "include pg_threads.h instead"
#endif

#include <windows.h>

enum
{
	pg_thrd_success_impl = 0,
	pg_thrd_nomem_impl,
	pg_thrd_timedout_impl,
	pg_thrd_busy_impl,
	pg_thrd_error_impl,
};

/* These need the definition of pg_thrd_success_impl. */
#include "port/pg_threads/map_windows_error.h"
#include "port/pg_threads/wrap.h"

/*
 * Experimental build option: define PG_THRD_CURRENT_CONFORMING to make
 * pg_thrd_create() a bit slower in exchange for being able to pass its result
 * to pg_thrd_detach() and (after copying it to another thread somehow)
 * pg_thrd_join().
 *
 * The default is that you can't do those things, and that matches the
 * behavior of Visual Studio's <threads.h>.
 *
 * XXX It doesn't seem that useful to do those things anyway, and unlikely
 * that anyone could usefully make use of these "anti-feature" macros.  Hence
 * default behavior of following Visual Studio's non-conforming behavior.  The
 * option is provided as illustration of the tradeoff being made under the
 * covers.
 */
#ifndef PG_THRD_CURRENT_CONFORMING
#define PG_THRD_CURRENT_NOT_DETACHABLE
#define PG_THRD_CURRENT_NOT_JOINABLE
#endif

/*
 * pg_mtx_t is mapped to SRWLOCK or CRITICAL_SECTION depending on whether
 * pg_mtx_recursive is requested.  Neither supports timeouts, so pg_mtx_timed
 * remains unsupported.
 *
 * XXX CreateMutex() could provide third union option that supports timedouts
 * and recursion, but those doesn't support interaction with
 * CONDITION_VARIABLE as required for cnd_wait().  That's a more complicated
 * feature hole, while this feature hole currently coincides with macOS.
 *
 * XXX Visual Studio's mtx_t doesn't have this problem, being built from
 * SRWLOCK + CONDITION_VARIABLE + counters.
 */
#define PG_MTX_TIMED_NOT_SUPPORTED

typedef void (*pg_call_once_function_impl) (void);

typedef CONDITION_VARIABLE pg_cnd_impl;
typedef INIT_ONCE pg_once_flag_impl;

typedef struct pg_mtx_impl
{
	enum
	{
		PG_MTX_SRWLOCK = 0,
		PG_MTX_CRITICAL_SECTION,
	}			type;
	union
	{
		SRWLOCK		srwlock;
		CRITICAL_SECTION critical_section;
	};
} pg_mtx_impl;

typedef struct pg_thrd_impl
{
	HANDLE		handle;
	DWORD		id;
} pg_thrd_impl;

typedef struct pg_tss_impl
{
	int			index;
	int			generation;
} pg_tss_impl;


enum
{
	pg_mtx_plain_impl = 0,
	pg_mtx_recursive_impl = 1 << 1,
	pg_mtx_timed_impl = 2 << 2,
};

#define PG_ONCE_FLAG_INIT_IMPL INIT_ONCE_STATIC_INIT
#define PG_TSS_DTOR_ITERATIONS_IMPL 4	/* typical POSIX value */

/* cnd_t */
PG_THREADS_MAP_SUCCESS(pg_cnd_broadcast, WakeAllConditionVariable, pg_cnd_impl *);
PG_THREADS_EMPTY_VOID(pg_cnd_destroy, pg_cnd_impl *);
PG_THREADS_MAP_SUCCESS(pg_cnd_init, InitializeConditionVariable, pg_cnd_impl *);
PG_THREADS_MAP_SUCCESS(pg_cnd_signal, WakeConditionVariable, pg_cnd_impl *);
PG_THREADS_OUTOFLINE(pg_cnd_timedwait, int, pg_cnd_impl *, pg_mtx_impl *, const struct timespec *);
PG_THREADS_OUTOFLINE(pg_cnd_wait, int, pg_cnd_impl *, pg_mtx_impl *);

/* mtx_t */
PG_THREADS_OUTOFLINE_VOID(pg_mtx_destroy, pg_mtx_impl *);
PG_THREADS_OUTOFLINE(pg_mtx_init, int, pg_mtx_impl *, int);

static inline int
pg_mtx_lock(pg_mtx_impl *mutex)
{
	switch (mutex->type)
	{
		case PG_MTX_SRWLOCK:
			AcquireSRWLockExclusive(&mutex->srwlock);
			break;
		case PG_MTX_CRITICAL_SECTION:
			EnterCriticalSection(&mutex->critical_section);
			break;
	}
	return pg_thrd_success_impl;
}

PG_THREADS_MAP_ERROR_UNSUPPORTED(pg_mtx_timedlock, pg_mtx_impl *, const struct timespec *);

static inline int
pg_mtx_trylock(pg_mtx_impl *mutex)
{
	switch (mutex->type)
	{
		case PG_MTX_SRWLOCK:
			if (!TryAcquireSRWLockExclusive(&mutex->srwlock))
				return pg_thrd_busy_impl;
			break;
		case PG_MTX_CRITICAL_SECTION:
			if (!TryEnterCriticalSection(&mutex->critical_section))
				return pg_thrd_busy_impl;
			break;
	}
	return pg_thrd_success_impl;
}

static inline int
pg_mtx_unlock(pg_mtx_impl *mutex)
{
	switch (mutex->type)
	{
		case PG_MTX_SRWLOCK:
			ReleaseSRWLockExclusive(&mutex->srwlock);
			break;
		case PG_MTX_CRITICAL_SECTION:
			LeaveCriticalSection(&mutex->critical_section);
			break;
	}
	return pg_thrd_success_impl;
}

/* once_flag */
PG_THREADS_OUTOFLINE_VOID(pg_call_once, pg_once_flag_impl *, pg_call_once_function_impl);

/* thrd_t */
PG_THREADS_OUTOFLINE_0ARG(pg_thrd_current, pg_thrd_impl);
PG_THREADS_OUTOFLINE(pg_thrd_create, int, pg_thrd_impl *, pg_thrd_start_t, void *);
PG_THREADS_OUTOFLINE(pg_thrd_detach, int, pg_thrd_impl);
PG_THREADS_OUTOFLINE(pg_thrd_equal, int, pg_thrd_impl, pg_thrd_impl);
PG_THREADS_FORWARD_VOID(pg_thrd_exit, ExitThread, int);
PG_THREADS_OUTOFLINE(pg_thrd_join, int, pg_thrd_impl, int *);
PG_THREADS_OUTOFLINE(pg_thrd_sleep, int, const struct timespec *, struct timespec *);
PG_THREADS_FORWARD_VOID_0ARG(pg_thrd_yield, SwitchToThread);

/* tss_t */
PG_THREADS_OUTOFLINE(pg_tss_create, int, pg_tss_impl *, pg_tss_dtor_t);
PG_THREADS_OUTOFLINE_VOID(pg_tss_delete, pg_tss_impl);
PG_THREADS_OUTOFLINE(pg_tss_get, void *, pg_tss_impl);
PG_THREADS_OUTOFLINE(pg_tss_set, int, pg_tss_impl, void *);

#endif
