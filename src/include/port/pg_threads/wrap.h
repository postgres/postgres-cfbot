/*-------------------------------------------------------------------------
 *
 * wrap.h
 *    Macros for reducing boilerplate code when wrapping native APIs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/include/port/pg_threads/wrap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_THREADS_WRAP_H
#define PG_THREADS_WRAP_H

/* Given typenames T1, ... make an argument list T1 v1, .... */
#define PG_THREADS_MAKE_ARG_LIST(...)				\
	PG_THREADS_ARG4(__VA_ARGS__,					\
					PG_THREADS_AL3,					\
					PG_THREADS_AL2,					\
					PG_THREADS_AL1)(__VA_ARGS__)
#define PG_THREADS_ARG4(_1, _2, _3, _4, ...) _4
#define PG_THREADS_AL1(T1) T1 v1
#define PG_THREADS_AL2(T1, T2) T1 v1, T2 v2
#define PG_THREADS_AL3(T1, T2, T3) T1 v1, T2 v2, T3 v3

/* Given typenames T1, ... make a value list v1, ... */
#define PG_THREADS_MAKE_VAL_LIST(...)			\
	PG_THREADS_ARG4(__VA_ARGS__,				\
					PG_THREADS_VL3,				\
					PG_THREADS_VL2,				\
					PG_THREADS_VL1)
#define PG_THREADS_VL1 v1
#define PG_THREADS_VL2 v1, v2
#define PG_THREADS_VL3 v1, v2, v3

/* Forward fun(...) -> R to target_fun(...) -> R. */
#define PG_THREADS_FORWARD(fun, target_fun, R, ...)				  \
	static inline R												  \
	fun(PG_THREADS_MAKE_ARG_LIST(__VA_ARGS__))					  \
	{															  \
		return target_fun(PG_THREADS_MAKE_VAL_LIST(__VA_ARGS__)); \
	}
#define PG_THREADS_FORWARD_0ARG(fun, target, R) \
	static inline R								\
	fun(void)									\
	{											\
		return target();						\
	}
#define PG_THREADS_FORWARD_VOID(fun, target_fun, ...)		\
	static inline void										\
	fun(PG_THREADS_MAKE_ARG_LIST(__VA_ARGS__))				\
	{														\
		target_fun(PG_THREADS_MAKE_VAL_LIST(__VA_ARGS__));	\
	}
#define PG_THREADS_FORWARD_VOID_0ARG(fun, target_fun, ...) \
	static inline void									   \
	fun(void)											   \
	{													   \
		target_fun();									   \
	}

/* fun(...) -> void is an empty function. */
#define PG_THREADS_EMPTY_VOID(fun, ...)			\
	static inline void							\
	fun(PG_THREADS_MAKE_ARG_LIST(__VA_ARGS__))	\
	{											\
	}
#define PG_THREADS_EMPTY_VOID_0ARG(fun)			\
	static inline void							\
	fun(void)									\
	{											\
	}

/* Forward fun(...) -> int to pg_thrd_map(target_fun(...)) -> int. */
#define PG_THREADS_MAP(fun, target_fun, ...)							\
	static inline int													\
	fun(PG_THREADS_MAKE_ARG_LIST(__VA_ARGS__))							\
	{																	\
		return pg_thrd_map(target_fun(PG_THREADS_MAKE_VAL_LIST(__VA_ARGS__))); \
	}

/* Forward fun(...) -> int to target_fun(...), always reporting success. */
#define PG_THREADS_MAP_SUCCESS(fun, target_fun, ...)					\
	static inline int													\
	fun(PG_THREADS_MAKE_ARG_LIST(__VA_ARGS__))							\
	{																	\
		target_fun(PG_THREADS_MAKE_VAL_LIST(__VA_ARGS__));				\
		return pg_thrd_success_impl;									\
	}

/* fun(...) -> int, always reporting an "unsupported" error. */
#define PG_THREADS_MAP_ERROR_UNSUPPORTED(fun, ...)						\
	static inline int													\
	fun(PG_THREADS_MAKE_ARG_LIST(__VA_ARGS__))							\
	{																	\
		return pg_thrd_internal_error(#fun "(): unsupported on this platform"); \
	}

/* Forward fun(...) -> R to out-of-line fun##_impl(...) -> R. */
#define PG_THREADS_OUTOFLINE(fun, R, ...)						\
	extern R fun##_impl(PG_THREADS_MAKE_ARG_LIST(__VA_ARGS__)); \
	PG_THREADS_FORWARD(fun, fun##_impl, R, __VA_ARGS__);
#define PG_THREADS_OUTOFLINE_0ARG(fun, R)			\
	extern R fun##_impl(void);						\
	PG_THREADS_FORWARD_0ARG(fun, fun##_impl, R);
#define PG_THREADS_OUTOFLINE_VOID(fun, ...)						   \
	extern void fun##_impl(PG_THREADS_MAKE_ARG_LIST(__VA_ARGS__)); \
	PG_THREADS_FORWARD_VOID(fun, fun##_impl, __VA_ARGS__);
#define PG_THREADS_OUTOFLINE_VOID_0ARG(fun)			\
	extern void fun##_impl(void);					\
	PG_THREADS_FORWARD_VOID_0ARG(fun, fun##_impl);

#endif
