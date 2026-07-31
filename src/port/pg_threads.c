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

#if defined(PG_THREADS_USE_PTHREAD_H) || defined(PG_THREADS_USE_WINDOWS_H)
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

#ifdef WIN32
/*
 * Convert last Windows error number to a pg_thrd_XXX error, and record the
 * underlying code in a generic detail message.  (Places that make multiple
 * system calls can format a custom error message to report which system call
 * failed instead of using this generic conversion.)
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
			return pg_thrd_format_error("Windows error: %u",
										(unsigned int) windows_error);
	}
}

/* Convert last Windows error number to a pg_thrd_XXX error. */
int
pg_thrd_map_last_windows_error(void)
{
	return pg_thrd_map_windows_error(GetLastError());
}
#else
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

#if defined(PG_THREADS_USE_PTHREAD_H) || \
	defined(PG_THREADS_USE_WINDOWS_H)

/*-------------------------------------------------------------------------
 *
 * map_{pthread,windows}.h
 *
 *-------------------------------------------------------------------------
 */

static int
pg_mtx_validate_type(int type)
{
	if (type & ~(pg_mtx_plain | pg_mtx_timed | pg_mtx_recursive))
		return pg_thrd_format_error("pg_mtx_init(): bad type flags %d", type);

#ifdef PG_MTX_TIMED_NOT_SUPPORTED
	if (type & pg_mtx_timed)
		return pg_thrd_internal_error("pg_mtx_init(): pg_mtx_timed not supported on this platform");
#endif

	return pg_thrd_success;
}

#endif

/*-------------------------------------------------------------------------
 *
 * map_{pthread,windows}_ext.h, also reached from map_threads_ext.h
 *
 *-------------------------------------------------------------------------
 */

static int
pg_rwlock_validate_type(int type)
{
	if (type & ~(pg_rwlock_plain | pg_rwlock_timed))
		return pg_thrd_format_error("pg_rwlock_init(): bad type flags %d", type);

#ifdef PG_RWLOCK_TIMED_NOT_SUPPORTED
	if (type & pg_rwlock_timed)
		return pg_thrd_internal_error("pg_rwlock_init(): pg_rwlock_timed not supported on this platform");
#endif

	return pg_thrd_success;
}

#if defined(WIN32)

/*-------------------------------------------------------------------------
 *
 * map_windows_ext.h, also used for map_threads_ext.h on Windows systems
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

#else

/*-------------------------------------------------------------------------
 *
 * map_pthread_ext.h, also used for map_threads_ext.h on POSIX systems
 *
 *-------------------------------------------------------------------------
 */

int
pg_rwlock_init_impl(pg_rwlock_impl *lock, int type)
{
	int			result = pg_rwlock_validate_type(type);

	if (result != pg_thrd_success)
		return result;

	return pg_thrd_map(pthread_rwlock_init(lock, NULL));
}

#endif

#if defined(PG_THREADS_USE_WINDOWS_H)

/*-------------------------------------------------------------------------
 *
 * map_windows.h
 *
 *-------------------------------------------------------------------------
 */

/* Maximum number of active pg_tss_t IDs per process. */
#define PG_TSS_MAX 16

typedef struct pg_tss_value
{
	void	   *value;
	int			generation;
} pg_tss_value;

typedef struct pg_tss_slot
{
	pg_tss_dtor_t destructor;
	int			generation;
	bool		in_use;
} pg_tss_slot;

static thread_local pg_thrd_t pg_thrd_current_thread_local;

static thread_local pg_tss_value pg_tss_values[PG_TSS_MAX];
static pg_tss_slot pg_tss_slots[PG_TSS_MAX];
static DWORD pg_tss_fls = FLS_OUT_OF_INDEXES;
static pg_mtx_t pg_tss_lock = PG_MTX_PLAIN_INIT;

static BOOL CALLBACK
pg_call_once_invoke(pg_once_flag *flag, void *parameter, void **context)
{
	pg_call_once_function_impl function = parameter;

	function();
	return true;
}

void
pg_call_once_impl(pg_once_flag *flag, pg_call_once_function_impl function)
{
	InitOnceExecuteOnce(flag, pg_call_once_invoke, function, NULL);
}

/* Convert an absolute TIME_UTC time to a millisecond delay. */
static DWORD
pg_threads_get_ms_delay(const struct timespec *time)
{
	struct timespec now;
	int64_t		now_ms;
	int64_t		time_ms;
	int64_t		wait_ms;

	if (time == NULL)
		return INFINITE;

	timespec_get(&now, TIME_UTC);
	now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	time_ms = time->tv_sec * 1000 + time->tv_nsec / 1000000;
	wait_ms = time_ms - now_ms;
	if (wait_ms < 0)
		wait_ms = 0;
	else if (wait_ms > INFINITE)
		wait_ms = INFINITE;
	return wait_ms;
}

int
pg_cnd_timedwait_impl(pg_cnd_impl *cnd, pg_mtx_impl *mutex, const struct timespec *time)
{
	DWORD		wait_ms = pg_threads_get_ms_delay(time);

	switch (mutex->type)
	{
		case PG_MTX_SRWLOCK:
			return pg_thrd_map(SleepConditionVariableSRW(cnd,
														 &mutex->srwlock,
														 wait_ms,
														 0));
		case PG_MTX_CRITICAL_SECTION:
		default:
			return pg_thrd_map(SleepConditionVariableCS(cnd,
														&mutex->critical_section,
														wait_ms));
	}
}

int
pg_cnd_wait_impl(pg_cnd_impl *cnd, pg_mtx_impl *mutex)
{
	return pg_cnd_timedwait_impl(cnd, mutex, NULL);
}

typedef struct pg_thrd_start_info
{
	pg_thrd_start_t function;
	void	   *argument;
#ifdef PG_THRD_CURRENT_CONFORMING
	pg_thrd_t	self;
#endif
} pg_thrd_start_info;

static DWORD CALLBACK
pg_thrd_invoke(void *argument)
{
	pg_thrd_start_info start = *(pg_thrd_start_info *) argument;
#ifdef PG_THRD_CURRENT_CONFORMING
	pg_thrd_current_thread_local = start.self;
#endif
	free(argument);
	return start.function(start.argument);
}

int
pg_thrd_create_impl(pg_thrd_t *thread, pg_thrd_start_t function, void *argument)
{
	pg_thrd_start_info *start;
	DWORD		flags;

	start = malloc(sizeof(*start));
	if (start == NULL)
		return pg_thrd_nomem;
	start->function = function;
	start->argument = argument;

#ifdef PG_THRD_CURRENT_CONFORMING
	flags = CREATE_SUSPENDED;	/* wait for "self" to be stored */
#else
	flags = 0;					/* run immediately */
#endif

	thread->handle = CreateThread(NULL, 0, pg_thrd_invoke, start, flags, 0);
	if (thread->handle == NULL)
	{
		free(start);
		return pg_thrd_map_windows_error(GetLastError());
	}

	thread->id = GetThreadId(thread->handle);
	if (thread->id == 0)
	{
		/*
		 * The only documented failures involve bad handles and rights.
		 * CreateThread() grants THREAD_ALL_ACCESS, so failure shouldn't be
		 * possible.  We can't even recover from an error at this point, given
		 * advice that even suspended threads shouldn't be terminated, and we
		 * surely can't just leave a thread behind.  Log the error and abort.
		 */
		fprintf(stderr,
				"GetThreadId() unexpectedly failed with error %u\n",
				(unsigned int) GetLastError());
		abort();
	}

#ifdef PG_THRD_CURRENT_CONFORMING
	start->self = *thread;
	if (!ResumeThread(thread->handle))
	{
		/* Previous comment applies here too. */
		fprintf(stderr,
				"ResumeThread() unexpectedly failed with error %u\n",
				(unsigned int) GetLastError());
		abort();
	}
#endif

	return pg_thrd_success;
}


pg_thrd_t
pg_thrd_current_impl(void)
{
	if (pg_thrd_current_thread_local.id == 0)
	{
		/*
		 * Alien thread, or result of pg_thrd_current (and
		 * PG_THRD_CURRENT_CONFORMING is not defined).  Populate just the
		 * thread ID on first call from this thread.  Result cannot be used to
		 * join or detach.
		 */
		pg_thrd_current_thread_local.id = GetCurrentThreadId();
	}

	return pg_thrd_current_thread_local;
}

int
pg_thrd_join_impl(pg_thrd_t thread, int *result)
{
	if (thread.handle == NULL)
	{
		/*
		 * This thread value must have come from pg_thrd_current() (and
		 * PG_THRD_CURRENT_CONFORMING is not defined, or it was called from
		 * an alien thread not created by pg_thrd_create()).
		 *
		 * We can't just use OpenThread(thread.id) to get a handle.  We'd have
		 * to trust the caller to hold a handle somewhere else and not close
		 * it concurrently, and then close it later to avoid leaking kernel
		 * resources.  That doesn't seem workable.
		 */
		return pg_thrd_format_error("pg_thrd_join(): no handle for thread %u",
									(unsigned int) thread.id);
	}

	if (WaitForSingleObject(thread.handle, INFINITE) == WAIT_OBJECT_0)
	{
		if (result)
		{
			DWORD		exit_value;

			/*
			 * All bits survive casting through the Windows exit value type.
			 * In practice, only the sign differs, and you can't join alien
			 * threads in this implementation (see above).
			 */
			static_assert(sizeof(*result) <= sizeof(exit_value),
						  "thread exit value type is not big enough");

			if (!GetExitCodeThread(thread.handle, &exit_value))
				return pg_thrd_format_error("pg_thrd_join(): GetExitCodeThread() failed with error %u",
											(unsigned int) GetLastError());

			if (result)
				*result = exit_value;
		}
		return pg_thrd_map(CloseHandle(thread.handle));
	}
	return pg_thrd_format_error("pg_thrd_join(): WaitForSingleObject() failed with error %u",
								(unsigned int) GetLastError());
}

int
pg_thrd_equal_impl(pg_thrd_t lhs, pg_thrd_t rhs)
{
	return lhs.id == rhs.id;
}

int
pg_thrd_detach_impl(pg_thrd_t thread)
{
	if (thread.handle == NULL)
	{
		/*
		 * See also pg_thrd_join_impl().
		 *
		 * Windows threads are detached by closing all open handles, but we
		 * don't know anything about the handles of alien threads, or the
		 * result of pg_thrd_current() (unless PG_THRD_CURRENT_CONFORMING is
		 * defined).
		 */
		return pg_thrd_format_error("pg_thrd_detach(): no handle for thread %u",
									(unsigned int) thread.id);
	}

	return pg_thrd_map(CloseHandle(thread.handle));
}

int
pg_thrd_sleep_impl(const struct timespec *duration, struct timespec *remaining)
{
	long long	ms = duration->tv_sec * 1000 + duration->tv_nsec / 1000000;

	if (ms < 0)
		ms = 0;
	if (ms > INFINITE)
		ms = INFINITE;

	Sleep(ms);

	return 0;
}

int
pg_mtx_init_impl(pg_mtx_impl *mutex, int type)
{
	int			result = pg_mtx_validate_type(type);

	if (result != pg_thrd_success)
		return result;

	if (type & pg_mtx_recursive)
	{
		mutex->type = PG_MTX_CRITICAL_SECTION;
		InitializeCriticalSection(&mutex->critical_section);
	}
	else
	{
		mutex->type = PG_MTX_SRWLOCK;
		InitializeSRWLock(&mutex->srwlock);
	}
	return pg_thrd_success;
}

void
pg_mtx_destroy_impl(pg_mtx_impl *mutex)
{
	if (mutex->type == PG_MTX_CRITICAL_SECTION)
		DeleteCriticalSection(&mutex->critical_section);
}

/*
 * Windows own FLS destructors don't have the right semantics to use directly
 * (among other problems, they call destructors on all values from all
 * threads, from the wrong thread, when you delete IDs).  We make our own
 * destructor table and per-thread value array to implement the standard
 * semantics.
 */
static void CALLBACK
pg_tss_fls_destructor(void *data)
{
	for (int iter = 0; iter < PG_TSS_DTOR_ITERATIONS_IMPL; ++iter)
	{
		bool		called_destructor = false;

		for (int i = 0; i < lengthof(pg_tss_values); ++i)
		{
			void	   *value = pg_tss_values[i].value;
			pg_tss_dtor_t destructor = NULL;

			if (value == NULL)
				continue;

			pg_tss_values[i].value = NULL;

			pg_mtx_lock(&pg_tss_lock);
			if (pg_tss_slots[i].in_use &&
				pg_tss_slots[i].generation == pg_tss_values[i].generation)
				destructor = pg_tss_slots[i].destructor;
			pg_mtx_unlock(&pg_tss_lock);

			if (destructor)
			{
				called_destructor = true;
				destructor(value);
			}
		}

		if (!called_destructor)
			break;
	}
}

static bool
pg_tss_ensure_fls_registered(void)
{
	bool		success;

	if (pg_tss_fls != FLS_OUT_OF_INDEXES)
		return true;			/* fast exit, already set */

	pg_mtx_lock(&pg_tss_lock);
	if (pg_tss_fls != FLS_OUT_OF_INDEXES)
	{
		success = true;
	}
	else
	{
		pg_tss_fls = FlsAlloc(pg_tss_fls_destructor);
		if (pg_tss_fls != FLS_OUT_OF_INDEXES)
			success = true;
	}
	pg_mtx_unlock(&pg_tss_lock);

	return success;
}

int
pg_tss_create_impl(pg_tss_t *id, pg_tss_dtor_t dtor)
{
	bool		created = false;

	if (!pg_tss_ensure_fls_registered())
		return pg_thrd_map_windows_error(GetLastError());

	pg_mtx_lock(&pg_tss_lock);
	for (int i = 0; i < lengthof(pg_tss_slots); ++i)
	{
		if (!pg_tss_slots[i].in_use)
		{
			id->generation = ++pg_tss_slots[i].generation;
			id->index = i;
			pg_tss_slots[i].destructor = dtor;
			pg_tss_slots[i].in_use = true;
			created = true;
			break;
		}
	}
	pg_mtx_unlock(&pg_tss_lock);

	if (!created)
		return pg_thrd_format_error("pg_tss_create(): limit of %d IDs exceeded",
									PG_TSS_MAX);

	return pg_thrd_success;
}

void
pg_tss_delete_impl(pg_tss_t id)
{
	pg_mtx_lock(&pg_tss_lock);
	pg_tss_slots[id.index].in_use = false;
	pg_mtx_unlock(&pg_tss_lock);
}

int
pg_tss_set_impl(pg_tss_t id, void *value)
{
	/*
	 * The generation prevents destructors from being called with leftover
	 * junk values if id is deleted and then a new one is created with the
	 * same index.
	 */
	pg_tss_values[id.index].value = value;
	pg_tss_values[id.index].generation = id.generation;

	/* Dummy value so that pg_tss_fls_destructor() runs in this thread. */
	FlsSetValue(pg_tss_fls, (void *) 1);

	return pg_thrd_success;
}

void *
pg_tss_get_impl(pg_tss_t id)
{
	/*
	 * pg_tss_{get,set} seem like candidates for inlining, but for now we keep
	 * all pg_tss_t functions out-of-line to avoid potential problems with
	 * cross-DLL thread-locals (not investigated).
	 */
	return pg_tss_values[id.index].value;
}

#endif							/* PG_THREAD_USE_WINDOWS_H */

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
