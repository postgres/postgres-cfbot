/*-------------------------------------------------------------------------
 *
 * random.h
 *	  PRNG distributions, hashing, and permutation functions for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/random.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RANDOM_H
#define RANDOM_H

#include "common/pg_prng.h"

/*
 * Hashing constants
 */
#define FNV_PRIME			UINT64CONST(0x100000001b3)
#define FNV_OFFSET_BASIS	UINT64CONST(0xcbf29ce484222325)
#define MM2_MUL				UINT64CONST(0xc6a4a7935bd1e995)
#define MM2_MUL_TIMES_8		UINT64CONST(0x35253c9ade8f4ca8)
#define MM2_ROT				47

#define MIN_GAUSSIAN_PARAM		2.0 /* minimum parameter for gauss */

#define MIN_ZIPFIAN_PARAM		1.001	/* minimum parameter for zipfian */
#define MAX_ZIPFIAN_PARAM		1000.0	/* maximum parameter for zipfian */

extern int64 random_seed;
extern pg_prng_state base_random_sequence;

extern void initRandomState(pg_prng_state *state);
extern int64 getrand(pg_prng_state *state, int64 min, int64 max);
extern int64 getExponentialRand(pg_prng_state *state, int64 min, int64 max,
								double parameter);
extern int64 getGaussianRand(pg_prng_state *state, int64 min, int64 max,
							 double parameter);
extern int64 getPoissonRand(pg_prng_state *state, double center);
extern int64 getZipfianRand(pg_prng_state *state, int64 min, int64 max, double s);
extern int64 getHashFnv1a(int64 val, uint64 seed);
extern int64 getHashMurmur2(int64 val, uint64 seed);
extern int64 permute(const int64 val, const int64 isize, const int64 seed);
extern bool set_random_seed(const char *seed);

#endif							/* RANDOM_H */
