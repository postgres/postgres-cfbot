/*-------------------------------------------------------------------------
 *
 * pgbench.c
 *	  Server-side extension functions exposing pgbench random distributions,
 *	  permutation, and hashing functions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/pgbench/pgbench.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "common/int.h"
#include "common/pgbench_funcs.h"
#include "common/pg_prng.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* Shared PRNG state used by pgbench extension random functions */
static pg_prng_state pgbench_prng_state;
static bool pgbench_prng_seed_set = false;

/*
 * Initialize (seed) the PRNG, if not done yet in this backend process.
 */
static void
initialize_prng(void)
{
	if (unlikely(!pgbench_prng_seed_set))
	{
		if (unlikely(!pg_prng_strong_seed(&pgbench_prng_state)))
		{
			TimestampTz now = GetCurrentTimestamp();
			uint64		iseed;

			/* Mix the PID with the most predictable bits of the timestamp */
			iseed = (uint64) now ^ ((uint64) MyProcPid << 32);
			pg_prng_seed(&pgbench_prng_state, iseed);
		}
		pgbench_prng_seed_set = true;
	}
}

static inline void
check_random_range(int64 min, int64 max)
{
	int64		delta;

	if (unlikely(min > max))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty range given to random: lower bound " INT64_FORMAT " is greater than upper bound " INT64_FORMAT,
						min, max)));

	if (unlikely(pg_sub_s64_overflow(max, min, &delta) ||
				 pg_add_s64_overflow(delta, 1, &delta)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("random range is too large")));
}

PG_FUNCTION_INFO_V1(pgbench_setseed_double);
Datum
pgbench_setseed_double(PG_FUNCTION_ARGS)
{
	float8		seed = PG_GETARG_FLOAT8(0);

	if (seed < -1.0 || seed > 1.0 || isnan(seed))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("setseed parameter %g is out of allowed range [-1,1]",
						seed)));

	pg_prng_fseed(&pgbench_prng_state, seed);
	pgbench_prng_seed_set = true;

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgbench_setseed_int64);
Datum
pgbench_setseed_int64(PG_FUNCTION_ARGS)
{
	int64		seed = PG_GETARG_INT64(0);

	pg_prng_seed(&pgbench_prng_state, (uint64) seed);
	pgbench_prng_seed_set = true;

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgbench_random_int64);
Datum
pgbench_random_int64(PG_FUNCTION_ARGS)
{
	int64		min = PG_GETARG_INT64(0);
	int64		max = PG_GETARG_INT64(1);

	check_random_range(min, max);
	initialize_prng();

	PG_RETURN_INT64(pgbench_random(&pgbench_prng_state, min, max));
}

PG_FUNCTION_INFO_V1(pgbench_random_gaussian_int64);
Datum
pgbench_random_gaussian_int64(PG_FUNCTION_ARGS)
{
	int64		min = PG_GETARG_INT64(0);
	int64		max = PG_GETARG_INT64(1);
	float8		param = PG_GETARG_FLOAT8(2);

	check_random_range(min, max);

	if (isnan(param) || param < PGBENCH_MIN_GAUSSIAN_PARAM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("gaussian parameter must be at least %f (not %f)",
						PGBENCH_MIN_GAUSSIAN_PARAM, param)));

	initialize_prng();

	PG_RETURN_INT64(pgbench_random_gaussian(&pgbench_prng_state, min, max, param));
}

PG_FUNCTION_INFO_V1(pgbench_random_exponential_int64);
Datum
pgbench_random_exponential_int64(PG_FUNCTION_ARGS)
{
	int64		min = PG_GETARG_INT64(0);
	int64		max = PG_GETARG_INT64(1);
	float8		param = PG_GETARG_FLOAT8(2);

	check_random_range(min, max);

	if (isnan(param) || param <= 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("exponential parameter must be greater than zero (not %f)",
						param)));

	initialize_prng();

	PG_RETURN_INT64(pgbench_random_exponential(&pgbench_prng_state, min, max, param));
}

PG_FUNCTION_INFO_V1(pgbench_random_zipfian_int64);
Datum
pgbench_random_zipfian_int64(PG_FUNCTION_ARGS)
{
	int64		min = PG_GETARG_INT64(0);
	int64		max = PG_GETARG_INT64(1);
	float8		param = PG_GETARG_FLOAT8(2);

	check_random_range(min, max);

	if (isnan(param) || param < PGBENCH_MIN_ZIPFIAN_PARAM || param > PGBENCH_MAX_ZIPFIAN_PARAM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("zipfian parameter must be in range [%.3f, %.0f] (not %f)",
						PGBENCH_MIN_ZIPFIAN_PARAM, PGBENCH_MAX_ZIPFIAN_PARAM, param)));

	initialize_prng();

	PG_RETURN_INT64(pgbench_random_zipfian(&pgbench_prng_state, min, max, param));
}

PG_FUNCTION_INFO_V1(pgbench_random_poisson_int64);
Datum
pgbench_random_poisson_int64(PG_FUNCTION_ARGS)
{
	float8		center = PG_GETARG_FLOAT8(0);

	if (isnan(center) || center <= 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("poisson center parameter must be greater than zero (not %f)",
						center)));

	initialize_prng();

	PG_RETURN_INT64(pgbench_random_poisson(&pgbench_prng_state, center));
}

PG_FUNCTION_INFO_V1(pgbench_hash_murmur2_int64);
Datum
pgbench_hash_murmur2_int64(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	int64		seed = PG_GETARG_INT64(1);

	PG_RETURN_INT64(pgbench_hash_murmur2(val, (uint64) seed));
}

PG_FUNCTION_INFO_V1(pgbench_hash_fnv1a_int64);
Datum
pgbench_hash_fnv1a_int64(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	int64		seed = PG_GETARG_INT64(1);

	PG_RETURN_INT64(pgbench_hash_fnv1a(val, (uint64) seed));
}

PG_FUNCTION_INFO_V1(pgbench_permute_int64);
Datum
pgbench_permute_int64(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	int64		size = PG_GETARG_INT64(1);
	int64		seed = PG_GETARG_INT64(2);

	if (size <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("permute size parameter must be greater than zero")));

	PG_RETURN_INT64(pgbench_permute(val, size, seed));
}
