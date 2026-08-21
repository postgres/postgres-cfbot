/*-------------------------------------------------------------------------
 *
 * pg_threads/map_windows_error.h
 *    Support for mapping <windows.h> errors.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/map_windows_error.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_MAP_WINDOWS_ERROR
#define PG_THREADS_MAP_WINDOWS_ERROR

#ifndef PG_THREADS_H
#error "include pg_threads.h instead"
#endif

extern int	pg_thrd_map_last_windows_error(void);
extern int	pg_thrd_internal_error(const char *detail);

static inline int
pg_thrd_map(bool success)
{
	return success ?
		pg_thrd_success_impl :
		pg_thrd_map_last_windows_error();
}

#endif
