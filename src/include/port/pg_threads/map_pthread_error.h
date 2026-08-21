/*-------------------------------------------------------------------------
 *
 * map_pthread_error.h
 *    Support for mapping <pthread.h> errors.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/map_pthread_error.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_MAP_PTHREAD_ERROR_H
#define PG_THREADS_MAP_PTHREAD_ERROR_H

#ifndef PG_THREADS_H
#error "include pg_threads.h instead"
#endif

extern int	pg_thrd_map_pthread_error(int pthread_error);
extern int	pg_thrd_internal_error(const char *detail);

static inline int
pg_thrd_map(int pthread_result)
{
	return pthread_result == 0 ?
		pg_thrd_success_impl :
		pg_thrd_map_pthread_error(pthread_result);
}

#endif
