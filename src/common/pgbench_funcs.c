/*-------------------------------------------------------------------------
 *
 * pgbench_funcs.c
 *	  Shared random distribution, permutation, and hashing functions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/pgbench_funcs.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <math.h>

#include "common/pgbench_funcs.h"
#include "port/pg_bitutils.h"

/*
 * random number generator: uniform distribution from min to max inclusive.
 *
 * Although the limits are expressed as int64, you can't generate the full
 * int64 range in one call, because the difference of the limits mustn't
 * overflow int64.  This is not checked here; callers should check.
 */
int64
pgbench_random(pg_prng_state *state, int64 min, int64 max)
{
	return min + (int64) pg_prng_uint64_range(state, 0, max - min);
}

/*
 * random number generator: exponential distribution from min to max inclusive.
 * the parameter is so that the density of probability for the last cut-off max
 * value is exp(-parameter).
 */
int64
pgbench_random_exponential(pg_prng_state *state, int64 min, int64 max,
						   double parameter)
{
	double		cut,
				uniform,
				rand;

	/* abort if wrong parameter, but must really be checked beforehand */
	Assert(parameter > 0.0);
	cut = exp(-parameter);
	/* pg_prng_double value in [0, 1), uniform in (0, 1] */
	uniform = 1.0 - pg_prng_double(state);

	/*
	 * inner expression in (cut, 1] (if parameter > 0), rand in [0, 1)
	 */
	Assert((1.0 - cut) != 0.0);
	rand = -log(cut + (1.0 - cut) * uniform) / parameter;
	/* return int64 random number within between min and max */
	return min + (int64) ((max - min + 1) * rand);
}

/* random number generator: gaussian distribution from min to max inclusive */
int64
pgbench_random_gaussian(pg_prng_state *state, int64 min, int64 max,
						double parameter)
{
	double		stdev;
	double		rand;

	/* abort if parameter is too low, but must really be checked beforehand */
	Assert(parameter >= PGBENCH_MIN_GAUSSIAN_PARAM);

	/*
	 * Get normally-distributed random number in the range -parameter <= stdev
	 * < parameter.
	 *
	 * This loop is executed until the number is in the expected range.
	 *
	 * As the minimum parameter is 2.0, the probability of looping is low:
	 * sqrt(-2 ln(r)) <= 2 => r >= e^{-2} ~ 0.135, then when taking the
	 * average sinus multiplier as 2/pi, we have a 8.6% looping probability in
	 * the worst case. For a parameter value of 5.0, the looping probability
	 * is about e^{-5} * 2 / pi ~ 0.43%.
	 */
	do
	{
		stdev = pg_prng_double_normal(state);
	}
	while (stdev < -parameter || stdev >= parameter);

	/* stdev is in [-parameter, parameter), normalization to [0,1) */
	rand = (stdev + parameter) / (parameter * 2.0);

	/* return int64 random number within between min and max */
	return min + (int64) ((max - min + 1) * rand);
}

/*
 * random number generator: generate a value, such that the series of values
 * will approximate a Poisson distribution centered on the given value.
 *
 * Individual results are rounded to integers, though the center value need
 * not be one.
 */
int64
pgbench_random_poisson(pg_prng_state *state, double center)
{
	/*
	 * Use inverse transform sampling to generate a value > 0, such that the
	 * expected (i.e. average) value is the given argument.
	 */
	double		uniform;

	/* pg_prng_double value in [0, 1), uniform in (0, 1] */
	uniform = 1.0 - pg_prng_double(state);

	return (int64) (-log(uniform) * center + 0.5);
}

/*
 * Computing zipfian using rejection method, based on
 * "Non-Uniform Random Variate Generation",
 * Luc Devroye, p. 550-551, Springer 1986.
 *
 * This works for s > 1.0, but may perform badly for s very close to 1.0.
 */
static int64
computeIterativeZipfian(pg_prng_state *state, int64 n, double s)
{
	double		b = pow(2.0, s - 1.0);
	double		x,
				t,
				u,
				v;

	/* Ensure n is sane */
	if (n <= 1)
		return 1;

	while (true)
	{
		/* random variates */
		u = pg_prng_double(state);
		v = pg_prng_double(state);

		x = floor(pow(u, -1.0 / (s - 1.0)));

		t = pow(1.0 + 1.0 / x, s - 1.0);
		/* reject if too large or out of bound */
		if (v * x * (t - 1.0) / (b - 1.0) <= t / b && x <= n)
			break;
	}
	return (int64) x;
}

/* random number generator: zipfian distribution from min to max inclusive */
int64
pgbench_random_zipfian(pg_prng_state *state, int64 min, int64 max, double s)
{
	int64		n = max - min + 1;

	/* abort if parameter is invalid */
	Assert(PGBENCH_MIN_ZIPFIAN_PARAM <= s && s <= PGBENCH_MAX_ZIPFIAN_PARAM);

	return min - 1 + computeIterativeZipfian(state, n, s);
}

/*
 * FNV-1a hash function
 */
int64
pgbench_hash_fnv1a(int64 val, uint64 seed)
{
	int64		result;
	int			i;

	result = PGBENCH_FNV_OFFSET_BASIS ^ seed;
	for (i = 0; i < 8; ++i)
	{
		int32		octet = val & 0xff;

		val = val >> 8;
		result = result ^ octet;
		result = result * PGBENCH_FNV_PRIME;
	}

	return result;
}

/*
 * Murmur2 hash function
 *
 * Based on original work of Austin Appleby
 * https://github.com/aappleby/smhasher/blob/master/src/MurmurHash2.cpp
 */
int64
pgbench_hash_murmur2(int64 val, uint64 seed)
{
	uint64		result = seed ^ PGBENCH_MM2_MUL_TIMES_8;	/* sizeof(int64) */
	uint64		k = (uint64) val;

	k *= PGBENCH_MM2_MUL;
	k ^= k >> PGBENCH_MM2_ROT;
	k *= PGBENCH_MM2_MUL;

	result ^= k;
	result *= PGBENCH_MM2_MUL;

	result ^= result >> PGBENCH_MM2_ROT;
	result *= PGBENCH_MM2_MUL;
	result ^= result >> PGBENCH_MM2_ROT;

	return (int64) result;
}

/*
 * Pseudorandom permutation function
 *
 * For small sizes, this generates each of the (size!) possible permutations
 * of integers in the range [0, size) with roughly equal probability.  Once
 * the size is larger than 20, the number of possible permutations exceeds the
 * number of distinct states of the internal pseudorandom number generator,
 * and so not all possible permutations can be generated, but the permutations
 * chosen should continue to give the appearance of being random.
 *
 * THIS FUNCTION IS NOT CRYPTOGRAPHICALLY SECURE.
 * DO NOT USE FOR SUCH PURPOSE.
 */
int64
pgbench_permute(const int64 val, const int64 isize, const int64 seed)
{
	/* using a high-end PRNG is probably overkill */
	pg_prng_state state;
	uint64		size;
	uint64		v;
	int			masklen;
	uint64		mask;
	int			i;

	if (isize < 2)
		return 0;				/* nothing to permute */

	/* Initialize prng state using the seed */
	pg_prng_seed(&state, (uint64) seed);

	/* Computations are performed on unsigned values */
	size = (uint64) isize;
	v = (uint64) val % size;

	/* Mask to work modulo largest power of 2 less than or equal to size */
	masklen = pg_leftmost_one_pos64(size);
	mask = (((uint64) 1) << masklen) - 1;

	/*
	 * Permute the input value by applying several rounds of pseudorandom
	 * bijective transformations.  The intention here is to distribute each
	 * input uniformly randomly across the range, and separate adjacent inputs
	 * approximately uniformly randomly from each other, leading to a fairly
	 * random overall choice of permutation.
	 *
	 * To separate adjacent inputs, we multiply by a random number modulo
	 * (mask + 1), which is a power of 2.  For this to be a bijection, the
	 * multiplier must be odd.  Since this is known to lead to less randomness
	 * in the lower bits, we also apply a rotation that shifts the topmost bit
	 * into the least significant bit.  In the special cases where size <= 3,
	 * mask = 1 and each of these operations is actually a no-op, so we also
	 * XOR the value with a different random number to inject additional
	 * randomness.  Since the size is generally not a power of 2, we apply
	 * this bijection on overlapping upper and lower halves of the input.
	 *
	 * To distribute the inputs uniformly across the range, we then also apply
	 * a random offset modulo the full range.
	 *
	 * Taken together, these operations resemble a modified linear
	 * congruential generator, as is commonly used in pseudorandom number
	 * generators.  The number of rounds is fairly arbitrary, but six has been
	 * found empirically to give a fairly good tradeoff between performance
	 * and uniform randomness.  For small sizes it selects each of the (size!)
	 * possible permutations with roughly equal probability.  For larger
	 * sizes, not all permutations can be generated, but the intended random
	 * spread is still produced.
	 */
	for (i = 0; i < 6; i++)
	{
		uint64		m,
					r,
					t;

		/* Random multiply (by an odd number), XOR and rotate of lower half */
		m = (pg_prng_uint64(&state) & mask) | 1;
		r = pg_prng_uint64(&state) & mask;
		if (v <= mask)
		{
			v = ((v * m) ^ r) & mask;
			v = ((v << 1) & mask) | (v >> (masklen - 1));
		}

		/* Random multiply (by an odd number), XOR and rotate of upper half */
		m = (pg_prng_uint64(&state) & mask) | 1;
		r = pg_prng_uint64(&state) & mask;
		t = size - 1 - v;
		if (t <= mask)
		{
			t = ((t * m) ^ r) & mask;
			t = ((t << 1) & mask) | (t >> (masklen - 1));
			v = size - 1 - t;
		}

		/* Random offset */
		r = pg_prng_uint64_range(&state, 0, size - 1);
		v = (v + r) % size;
	}

	return (int64) v;
}
