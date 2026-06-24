/*-------------------------------------------------------------------------
 *
 * pg_threads/map_windows_ext.h
 *    Map pg_threads_ext.h interfaces to Windows native APIs.
 *
 * This included by both map_threads_ext.h and map_windows_ext.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/map_windows_ext.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_MAP_WINDOW_EXT_H
#define PG_THREADS_MAP_WINDOW_EXT_H

#ifndef PG_THREADS_EXT_H
#error "include pg_threads_ext.h instead"
#endif

#include <windows.h>

#include "port/pg_threads/map_windows_error.h"
#include "port/pg_threads/wrap.h"

/*
 * SRWLOCK doesn't have a timed wait mode, so we'd have to hand-roll an
 * implementation with a futex or lock + cv.
 */
#define PG_RWLOCK_TIMED_NOT_SUPPORTED

typedef struct pg_rwlock_impl
{
	SRWLOCK		lock;
	bool		exclusive;
} pg_rwlock_impl;

typedef SYNCHRONIZATION_BARRIER pg_thrd_barrier_impl;


enum
{
	pg_rwlock_plain_impl = 0,
	pg_rwlock_timed_impl = 1 << 1,
};

#ifndef PG_THREADS_USE_THREADS_H
#define PG_CND_INIT_IMPL CONDITION_VARIABLE_INIT
#define PG_MTX_PLAIN_INIT_IMPL {.type = PG_MTX_SRWLOCK, .srwlock = SRWLOCK_INIT}
#endif
#define PG_RWLOCK_PLAIN_INIT_IMPL {.lock = SRWLOCK_INIT, .exclusive = false}


/* pg_rwlock_t */
PG_THREADS_EMPTY_VOID(pg_rwlock_destroy, pg_rwlock_impl *);
PG_THREADS_OUTOFLINE(pg_rwlock_init, int, pg_rwlock_impl *, int);

static inline int
pg_rwlock_unlock(pg_rwlock_impl *lock)
{
	if (lock->exclusive)
	{
		lock->exclusive = false;
		ReleaseSRWLockExclusive(&lock->lock);
	}
	else
	{
		ReleaseSRWLockShared(&lock->lock);
	}
	return pg_thrd_success_impl;
}

static inline int
pg_rwlock_rdlock(pg_rwlock_impl *lock)
{
	AcquireSRWLockShared(&lock->lock);
	return pg_thrd_success_impl;
}

PG_THREADS_MAP_ERROR_UNSUPPORTED(pg_rwlock_timedrdlock, pg_rwlock_impl *, const struct timespec *);
PG_THREADS_MAP_ERROR_UNSUPPORTED(pg_rwlock_timedwrlock, pg_rwlock_impl *, const struct timespec *);

static inline int
pg_rwlock_tryrdlock(pg_rwlock_impl *lock)
{
	if (TryAcquireSRWLockShared(&lock->lock))
		return pg_thrd_success_impl;
	return pg_thrd_busy_impl;
}

static inline int
pg_rwlock_trywrlock(pg_rwlock_impl *lock)
{
	if (TryAcquireSRWLockExclusive(&lock->lock))
	{
		lock->exclusive = true;
		return pg_thrd_success_impl;
	}
	return pg_thrd_busy_impl;
}

static inline int
pg_rwlock_wrlock(pg_rwlock_impl *lock)
{
	AcquireSRWLockExclusive(&lock->lock);
	lock->exclusive = true;
	return pg_thrd_success_impl;
}


/* pg_thrd_barrier_t */
PG_THREADS_EMPTY_VOID(pg_thrd_barrier_destroy, pg_thrd_barrier_impl *);

static inline int
pg_thrd_barrier_init(pg_thrd_barrier_impl *barrier, int count)
{
	return pg_thrd_map(InitializeSynchronizationBarrier(barrier, count, -1));
}

static inline int
pg_thrd_barrier_wait(pg_thrd_barrier_impl *barrier)
{
	EnterSynchronizationBarrier(barrier,
								SYNCHRONIZATION_BARRIER_FLAGS_NO_DELETE);
	return pg_thrd_success_impl;
}

#ifndef PG_THREADS_USE_THREADS_H
PG_THREADS_EMPTY_VOID_0ARG(pg_threads_ext_assertions);
#endif

#endif
