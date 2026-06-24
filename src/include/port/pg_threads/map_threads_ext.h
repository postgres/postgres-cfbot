/*-------------------------------------------------------------------------
 *
 * pg_threads/map_threads_ext.h
 *    Implements pg_threads_ext.h when using system <threads.h>.
 *
 * These facilities are by definition not in <threads.h>, and underlying
 * native APIs are used for pg_rwlock_t and pg_thrd_barrier_t.  Static
 * initializers are also conjured for cnd_t and mtx_t.
 *
 * C has so far declined to standardize static initializer values.  Values for
 * Visual Studio are documented, but for developer testing on POSIX systems
 * (which normally use map_pthread_ext.h instead), we have to take some
 * liberties to provide values.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/map_threads_ext.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_MAP_THREADS_EXT_H
#define PG_THREADS_MAP_THREADS_EXT_H

#if !defined(_MSC_VER)
#include <pthread.h>
#endif

#if defined(_MSC_VER)
/*
 * Visual Studio's <thread.h>: cnd_t and mtx_t can be zero-initialized per
 * published documentation and example code, so this is a supported deployment
 * configuration.
 */
#define PG_CND_INIT_IMPL			{0}
#define PG_MTX_PLAIN_INIT_IMPL		{0}
#elif defined(__GLIBC__) || defined(__linux__)
/*
 * Undocumented developer-only support for testing PG_THREADS_USE_THREADS_H on
 * Glibc and Musl.
 *
 * Those <threads.h> implementations use <pthread.h> types, but obfuscate and
 * cast internally to prevent this type of abuse.  We drill through that plan
 * to be able to test.  We can't use <pthread.h>'s initializer macros, but we
 * can prove that ours produce the same binary image.
 */
#define PG_CND_INIT_IMPL			{0}
#define PG_MTX_PLAIN_INIT_IMPL		{0}
#define PG_THREADS_EXT_CHECK_INITIALIZERS_MATCH_PTHREAD
#else
/*
 * Undocumented developer-only support for testing PG_THREADS_USE_THREADS_H on
 * systems where <threads.h> uses <pthread.h> types and that's verifiable from
 * the type system.  We take the much smaller liberty of inferring that the
 * latter's initializers therefore work for the former's types.
 *
 * This is expected to work on FreeBSD and NetBSD, and the type assertions are
 * expected to fail on Solaris and AIX.
 */
#define PG_CND_INIT_IMPL			PTHREAD_COND_INITIALIZER
#define PG_MTX_PLAIN_INIT_IMPL		PTHREAD_MUTEX_INITIALIZER
#define PG_THREADS_EXT_CHECK_INITIALIZERS_MATCH_PTHREAD
#define PG_THREADS_EXT_CHECK_TYPES_MATCH_PTHREAD
#endif

#ifndef WIN32
#include "port/pg_threads/map_pthread_ext.h"
#else
#include "port/pg_threads/map_windows_ext.h"
#endif

static inline void
pg_threads_ext_assertions(void)
{
#ifdef USE_ASSERT_CHECKING
#ifdef PG_THREADS_EXT_CHECK_INITIALIZERS_MATCH_PTHREAD
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	cnd_t		cnd = PG_CND_INIT_IMPL;
	mtx_t		mtx = PG_MTX_PLAIN_INIT_IMPL;

#if defined(PG_THREADS_EXT_CHECK_TYPES_MATCH_PTHREAD) && !defined(__cplusplus)
	/* Except on glibc/musl, the types must match or all bets are off. */
	StaticAssertVariableIsOfType(cnd, pthread_cond_t);
	StaticAssertVariableIsOfType(mtx, pthread_mutex_t);
#endif

	/* Even glibc and musl's obfuscated types should have these properties. */
	static_assert(sizeof(cnd) == sizeof(pthread_cond_t), "bad size");
	static_assert(sizeof(mtx) == sizeof(pthread_mutex_t), "bad size");
	static_assert(alignof(cnd_t) >= alignof(pthread_cond_t), "bad alignment");
	static_assert(alignof(mtx_t) >= alignof(pthread_mutex_t), "bad alignment");

	/* Our initializers should produce the same bits as <pthread.h>'s. */
	Assert(memcmp(&cnd, &cond, sizeof(cnd)) == 0);
	Assert(memcmp(&mtx, &mutex, sizeof(mtx)) == 0);
#endif
#endif
}

#endif
