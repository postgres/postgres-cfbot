/*-------------------------------------------------------------------------
 *
 * pg_threads.c
 *    Out-of-line parts of pg_threads/... headers.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/port/pg_threads.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"
#include "port/pg_threads.h"
#include "port/pg_threads_ext.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>


/*-------------------------------------------------------------------------
 *
 * Error retrieval support.
 *
 * These functions are available even for map_threads.h builds, but no extra
 * error detail is available so you just get the five fixed result strings.
 *
 * XXX That's because when functions in map_threads.h return thrd_error they
 * don't clear stale messages from earlier map_threads_ext.h functions that
 * failed.  We could teach their wrappers to do that, but that seems like an
 * impediment to using <threads.h> directly in the future.
 *
 *-------------------------------------------------------------------------
 */

#if defined(PG_THREADS_USE_PTHREAD_H)
#define PG_THREADS_HAVE_DETAILED_ERRORS
#endif

static int	pg_thrd_format_error(const char *format, ...) pg_attribute_printf(1, 2);

#ifdef PG_THREADS_HAVE_DETAILED_ERRORS
static thread_local char pg_thrd_last_error_detail[64];
#endif

/* Convert the standard errors to fixed strings. */
const char *
pg_thrd_error_string(int error)
{
	switch (error)
	{
		case pg_thrd_success:
			return "success";
		case pg_thrd_nomem:
			return "no memory";
		case pg_thrd_timedout:
			return "timed out";
		case pg_thrd_busy:
			return "busy";
		default:
			return "error";
	}
}

/*
 * Retrieve description of the most recently returned result from a
 * pg_thrd_XXX() function.  Result is only valid until the next call to a
 * pg_threads.h function from the same thread.
 *
 * Except when using map_threads.h, extra details may be available.
 */
const char *
pg_thrd_error_string_with_detail(int error)
{
#ifndef PG_THREADS_HAVE_DETAILED_ERRORS
	return pg_thrd_error_string(error);
#else
	if (error == pg_thrd_success ||
		error == pg_thrd_nomem ||
		error == pg_thrd_timedout ||
		error == pg_thrd_busy ||
		pg_thrd_last_error_detail[0] == 0)
		return pg_thrd_error_string(error);

	return pg_thrd_last_error_detail;
#endif
}

/*-------------------------------------------------------------------------
 *
 * Internal error mapping support.
 *
 * Even when using system <threads.h>, these are reachable from
 * pg_threads_ext.h extension.  In that case detailed messages are not kept.
 *
 *
 *-------------------------------------------------------------------------
 */

/*
 * Return pg_thrd_error, and also store custom error message.
 */
static int
pg_thrd_format_error(const char *format, ...)
{
#ifdef PG_THREADS_HAVE_DETAILED_ERRORS
	va_list		args;

	va_start(args, format);
	vsnprintf(pg_thrd_last_error_detail, sizeof(pg_thrd_last_error_detail),
			  format, args);
#endif

	return pg_thrd_error;
}

/*
 * pg_thrd_error + a detail message originating from implementation code, not
 * the OS.
 */
int
pg_thrd_internal_error(const char *detail)
{
	return pg_thrd_format_error("%s", detail);
}

#if defined(WIN32) && defined(PG_THREADS_USE_THREADS_H)
/*
 * Convert last Windows error number to a pg_thrd_XXX error, and record the
 * underlying code in a generic detail message.
 */
static int
pg_thrd_map_windows_error(DWORD windows_error)
{
	switch (windows_error)
	{
		case ERROR_NOT_ENOUGH_MEMORY:
			return pg_thrd_nomem;
		case ERROR_BUSY:
			return pg_thrd_busy;
		case WAIT_TIMEOUT:
			return pg_thrd_timedout;
		default:
			return pg_thrd_error;
	}
}

/* Convert last Windows error number to a pg_thrd_XXX error. */
int
pg_thrd_map_last_windows_error(void)
{
	return pg_thrd_map_windows_error(GetLastError());
}
#elif defined(PG_THREADS_USE_PTHREAD_H)
/*
 * Convert a POSIX error to a pg_thrd_XXX error, and record the underlying OS
 * error in a detail message.
 */
int
pg_thrd_map_pthread_error(int pthread_error)
{
	switch (pthread_error)
	{
		case ENOMEM:
			return pg_thrd_nomem;
		case EBUSY:
			return pg_thrd_busy;
		case ETIMEDOUT:
			return pg_thrd_timedout;
		default:
			return pg_thrd_format_error("pthread error: %s",
										strerror(pthread_error));
	}
}
#endif

#if defined(WIN32) && !defined(PG_THREADS_USE_PTHREAD_H)

/*-------------------------------------------------------------------------
 *
 * map_windows_ext.h
 *
 *-------------------------------------------------------------------------
 */

int
pg_rwlock_init_impl(pg_rwlock_impl *lock, int type)
{
	int			result = pg_rwlock_validate_type(type);

	if (result != pg_thrd_success)
		return result;

	InitializeSRWLock(&lock->lock);
	lock->exclusive = false;
	return pg_thrd_success_impl;
}

#elif defined(PG_THREADS_USE_PTHREAD_H)

/*-------------------------------------------------------------------------
 *
 * map_pthread_ext.h, also used for map_threads_ext.h on POSIX systems
 *
 *-------------------------------------------------------------------------
 */

int
pg_rwlock_init_impl(pg_rwlock_impl *lock, int type)
{
	if (type & ~(pg_rwlock_plain | pg_rwlock_timed))
		return pg_thrd_format_error("pg_rwlock_init(): bad type flags %d", type);

#ifdef PG_RWLOCK_TIMED_NOT_SUPPORTED
	if (type & pg_rwlock_timed)
		return pg_thrd_internal_error("pg_rwlock_init(): pg_rwlock_timed not supported on this platform");
#endif

	return pg_thrd_map(pthread_rwlock_init(lock, NULL));
}

#endif

#ifdef PG_THREADS_USE_PTHREAD_H

/*-------------------------------------------------------------------------
 *
 * map_pthread.h
 *
 *-------------------------------------------------------------------------
 */

typedef struct pg_thrd_start_info
{
	pg_thrd_start_t function;
	void	   *argument;
} pg_thrd_start_info;

static void *
pg_thrd_invoke(void *argument)
{
	pg_thrd_start_info start = *(pg_thrd_start_info *) argument;

	free(argument);
	return (void *) (intptr_t) start.function(start.argument);
}

int
pg_thrd_create_impl(pg_thrd_t *thread, pg_thrd_start_t function, void *argument)
{
	pg_thrd_start_info *start;
	int			pthread_result;

	/*
	 * Thread start function has a different return type, so we need an
	 * intermediate invoker function that casts the return value.  (Simply
	 * casting the function pointer type might work in practice, but that'd be
	 * undefined behavior.)
	 */
	start = malloc(sizeof(*start));
	if (start == NULL)
		return pg_thrd_nomem;
	start->function = function;
	start->argument = argument;

	pthread_result = pthread_create(thread, NULL, pg_thrd_invoke, start);
	if (pthread_result != 0)
		free(start);

	return pg_thrd_map(pthread_result);
}

int
pg_thrd_join_impl(pg_thrd_t thread, int *result)
{
	void	   *exit_value = NULL;
	int			error = pg_thrd_map(pthread_join(thread, &exit_value));

	/*
	 * All bits survive casting through the pthread exit value type.
	 *
	 * Joining a thread that exits via pthread_exit() or is cancelled with
	 * pthread_cancel() is undefined behavior.  See POSIX's discussion of
	 * thrd_join().
	 */
	static_assert(sizeof(*result) <= sizeof(exit_value),
				  "thread exit value type is not big enough");

	if (result)
		*result = (int) (intptr_t) exit_value;

	return error;
}

void
pg_thrd_exit_impl(int result)
{
	pthread_exit((void *) (intptr_t) result);
}

int
pg_mtx_init_impl(pg_mtx_impl *mtx, int type)
{
	pthread_mutexattr_t attr;
	int			result = pg_mtx_validate_type(type);

	if (result != pg_thrd_success)
		return result;

	pthread_mutexattr_init(&attr);
	if (type & pg_mtx_recursive_impl)
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	result = pg_thrd_map(pthread_mutex_init(mtx, &attr));
	pthread_mutexattr_destroy(&attr);

	return result;
}

int
pg_cnd_init_impl(pg_cnd_impl *cnd)
{
	return pg_thrd_map(pthread_cond_init(cnd, NULL));
}

#endif							/* PG_THREAD_USE_PTHREAD_H */
