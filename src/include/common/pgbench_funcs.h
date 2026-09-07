/*-------------------------------------------------------------------------
 *
 * pgbench_funcs.h
 *	  Shared random distribution, permutation, and hashing functions for
 *	  pgbench and backend extensions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/pgbench_funcs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGBENCH_FUNCS_H
#define PGBENCH_FUNCS_H

#include "common/pg_prng.h"

/* Parameter boundaries for statistical distributions */
#define PGBENCH_MIN_GAUSSIAN_PARAM		2.0
#define PGBENCH_MIN_ZIPFIAN_PARAM		1.001
#define PGBENCH_MAX_ZIPFIAN_PARAM		1000.0

/* Hashing Constants */
#define PGBENCH_FNV_PRIME				UINT64CONST(0x100000001b3)
#define PGBENCH_FNV_OFFSET_BASIS		UINT64CONST(0xcbf29ce484222325)
#define PGBENCH_MM2_MUL					UINT64CONST(0xc6a4a7935bd1e995)
#define PGBENCH_MM2_MUL_TIMES_8			UINT64CONST(0x35253c9ade8f4ca8)
#define PGBENCH_MM2_ROT					47

/* Random Distribution Functions */
extern int64 pgbench_random(pg_prng_state *state, int64 min, int64 max);
extern int64 pgbench_random_gaussian(pg_prng_state *state, int64 min, int64 max,
									 double parameter);
extern int64 pgbench_random_exponential(pg_prng_state *state, int64 min, int64 max,
										double parameter);
extern int64 pgbench_random_zipfian(pg_prng_state *state, int64 min, int64 max,
									double s);
extern int64 pgbench_random_poisson(pg_prng_state *state, double center);

/* Hashing Functions */
extern int64 pgbench_hash_fnv1a(int64 val, uint64 seed);
extern int64 pgbench_hash_murmur2(int64 val, uint64 seed);

/* Permutation Function */
extern int64 pgbench_permute(int64 val, int64 isize, int64 seed);

#endif							/* PGBENCH_FUNCS_H */
