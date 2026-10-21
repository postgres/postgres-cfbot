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

#include <ctype.h>
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

/*
 * TPC-C Non-Uniform Random (NURand) function:
 *   NURand(A, x, y) = (((random(0, A) | random(x, y)) + C) % (y - x + 1)) + x
 */
int64
pgbench_nurand(pg_prng_state *state, int64 A, int64 min, int64 max, int64 C)
{
	int64		r1 = pgbench_random(state, 0, A);
	int64		r2 = pgbench_random(state, min, max);
	uint64		span = (uint64) max - (uint64) min + UINT64CONST(1);
	uint64		comb = ((uint64) (r1 | r2)) + (uint64) C;
	uint64		rem = (span != 0) ? (comb % span) : comb;

	return (int64) ((uint64) min + rem);
}

/*
 * TPC-C Customer Last Name (c_last) syllable generator.
 * Maps an integer in [0, 999] to a 3-syllable string formed from the 10
 * standard TPC-C syllables.
 */
bool
pgbench_c_last(int64 num, char *buf, size_t buflen)
{
	static const char *const syllables[10] = {
		"BAR", "OUGHT", "ABLE", "PRI", "PRES",
		"ESE", "ANTI", "CALLY", "ATION", "EING"
	};

	if (num < 0 || num > 999)
		return false;

	snprintf(buf, buflen, "%s%s%s",
			 syllables[(num / 100) % 10],
			 syllables[(num / 10) % 10],
			 syllables[num % 10]);
	return true;
}

/*
 * Generate a random alphanumeric string [a-zA-Z0-9] of length in
 * [min_len, max_len]. Caller must provide a buffer of at least max_len + 1
 * bytes.
 */
void
pgbench_random_astring(pg_prng_state *state, int64 min_len, int64 max_len, char *buf)
{
	static const char alnum_chars[] =
		"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	int64		len = pgbench_random(state, min_len, max_len);

	for (int64 i = 0; i < len; i++)
	{
		uint64		idx = pg_prng_uint64_range(state, 0, sizeof(alnum_chars) - 2);

		buf[i] = alnum_chars[idx];
	}
	buf[len] = '\0';
}

/*
 * Generate a random numeric string [0-9] of length in [min_len, max_len].
 * Caller must provide a buffer of at least max_len + 1 bytes.
 */
void
pgbench_random_nstring(pg_prng_state *state, int64 min_len, int64 max_len, char *buf)
{
	int64		len = pgbench_random(state, min_len, max_len);

	for (int64 i = 0; i < len; i++)
	{
		uint64		idx = pg_prng_uint64_range(state, 0, 9);

		buf[i] = (char) ('0' + idx);
	}
	buf[len] = '\0';
}

/*
 * Generate a random binary payload of length in [min_len, max_len].
 * Caller must provide a buffer of at least max_len bytes (when max_len > 0).
 * Returns the generated length in bytes.
 */
int64
pgbench_random_bytea(pg_prng_state *state, int64 min_len, int64 max_len, uint8 *buf)
{
	int64		len = pgbench_random(state, min_len, max_len);
	int64		i = 0;

	while (i + 8 <= len)
	{
		uint64		r = pg_prng_uint64(state);

		memcpy(buf + i, &r, 8);
		i += 8;
	}
	if (i < len)
	{
		uint64		r = pg_prng_uint64(state);

		memcpy(buf + i, &r, (size_t) (len - i));
	}
	return len;
}

/*-------------------------------------------------------------------------
 * Date / Timestamp Helpers (microseconds since 2000-01-01 00:00:00)
 *-------------------------------------------------------------------------
 */
#define PGBENCH_EPOCH_JDATE		2451545 /* date2j(2000, 1, 1) */
#define PGBENCH_USECS_PER_SEC	INT64CONST(1000000)
#define PGBENCH_USECS_PER_MIN	INT64CONST(60000000)
#define PGBENCH_USECS_PER_HOUR	INT64CONST(3600000000)
#define PGBENCH_USECS_PER_DAY	INT64CONST(86400000000)

static int
pgbench_date2j(int y, int m, int d)
{
	int			julian;
	int			century;

	if (m > 2)
	{
		m += 1;
		y += 4800;
	}
	else
	{
		m += 13;
		y += 4799;
	}

	century = y / 100;
	julian = y * 365 - 32167;
	julian += y / 4 - century + century / 4;
	julian += 7834 * m / 256 + d;

	return julian;
}

static void
pgbench_j2date(int jd, int *year, int *month, int *day)
{
	unsigned int julian;
	unsigned int quad;
	unsigned int extra;
	int			y;

	julian = jd;
	julian += 32044;
	quad = julian / 146097;
	extra = (julian - quad * 146097) * 4 + 3;
	julian += 60 + quad * 3 + extra / 146097;
	quad = julian / 1461;
	julian -= quad * 1461;
	y = julian * 4 / 1461;
	julian = ((y != 0) ? ((julian + 305) % 365) : ((julian + 306) % 366))
		+ 123;
	y += quad * 4;
	*year = y - 4800;
	quad = julian * 2141 / 65536;
	*day = julian - 7834 * quad / 256;
	*month = (quad + 10) % 12 + 1;
}

static int
pgbench_days_in_month(int year, int month)
{
	static const int days_tab[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};

	if (month == 2)
	{
		bool		is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));

		return is_leap ? 29 : 28;
	}
	if (month < 1 || month > 12)
		return 0;
	return days_tab[month - 1];
}

/*
 * Parse an ISO-8601 date/timestamp string ("YYYY-MM-DD" or
 * "YYYY-MM-DD HH:MM[:SS[.ffffff]]") into microseconds since 2000-01-01.
 */
bool
pgbench_parse_timestamp(const char *str, int64 *usec_out)
{
	long		year,
				month,
				day,
				hour = 0,
				min = 0,
				sec = 0;
	int64		fsec = 0;
	const char *p = str;
	char	   *endp;
	int			jday;

	if (str == NULL)
		return false;

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;

	year = strtol(p, &endp, 10);
	if (endp == p || *endp != '-' || year < 1 || year > 9999)
		return false;
	p = endp + 1;

	month = strtol(p, &endp, 10);
	if (endp == p || *endp != '-' || month < 1 || month > 12)
		return false;
	p = endp + 1;

	day = strtol(p, &endp, 10);
	if (endp == p || day < 1 || day > pgbench_days_in_month((int) year, (int) month))
		return false;
	p = endp;

	if (*p == ' ' || *p == 'T' || *p == 't')
	{
		p++;
		while (*p == ' ')
			p++;
		if (*p != '\0')
		{
			hour = strtol(p, &endp, 10);
			if (endp == p || *endp != ':' || hour < 0 || hour > 23)
				return false;
			p = endp + 1;

			min = strtol(p, &endp, 10);
			if (endp == p || min < 0 || min > 59)
				return false;
			p = endp;

			if (*p == ':')
			{
				p++;
				sec = strtol(p, &endp, 10);
				if (endp == p || sec < 0 || sec > 60)
					return false;
				p = endp;

				if (*p == '.')
				{
					int			digits = 0;

					p++;
					if (!isdigit((unsigned char) *p))
						return false;
					while (*p >= '0' && *p <= '9')
					{
						if (digits < 6)
						{
							fsec = fsec * 10 + (*p - '0');
							digits++;
						}
						p++;
					}
					while (digits < 6)
					{
						fsec *= 10;
						digits++;
					}
				}
			}
		}
	}

	/* Allow optional UTC suffix 'Z' or '+00' / '+00:00' and trailing spaces */
	if (*p == 'Z' || *p == 'z')
		p++;
	else if ((*p == '+' || *p == '-') && p[1] == '0' && p[2] == '0')
	{
		p += 3;
		if (*p == ':' && p[1] == '0' && p[2] == '0')
			p += 3;
	}

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;
	if (*p != '\0')
		return false;

	jday = pgbench_date2j((int) year, (int) month, (int) day) - PGBENCH_EPOCH_JDATE;
	*usec_out = (int64) jday * PGBENCH_USECS_PER_DAY
		+ (int64) hour * PGBENCH_USECS_PER_HOUR
		+ (int64) min * PGBENCH_USECS_PER_MIN
		+ (int64) sec * PGBENCH_USECS_PER_SEC
		+ fsec;
	return true;
}

/*
 * Format microseconds since 2000-01-01 as "YYYY-MM-DD" (if time_usec == 0)
 * or "YYYY-MM-DD HH:MM:SS[.ffffff]".
 */
void
pgbench_format_timestamp(int64 usec, char *buf, size_t buflen)
{
	int64		days;
	int64		time_usec;
	int			year,
				month,
				day,
				hour,
				min,
				sec,
				fsec;

	days = usec / PGBENCH_USECS_PER_DAY;
	time_usec = usec % PGBENCH_USECS_PER_DAY;
	if (time_usec < 0)
	{
		days--;
		time_usec += PGBENCH_USECS_PER_DAY;
	}

	pgbench_j2date((int) days + PGBENCH_EPOCH_JDATE, &year, &month, &day);

	if (time_usec == 0)
	{
		snprintf(buf, buflen, "%04d-%02d-%02d", year, month, day);
		return;
	}

	hour = (int) (time_usec / PGBENCH_USECS_PER_HOUR);
	time_usec %= PGBENCH_USECS_PER_HOUR;
	min = (int) (time_usec / PGBENCH_USECS_PER_MIN);
	time_usec %= PGBENCH_USECS_PER_MIN;
	sec = (int) (time_usec / PGBENCH_USECS_PER_SEC);
	fsec = (int) (time_usec % PGBENCH_USECS_PER_SEC);

	if (fsec == 0)
	{
		snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d",
				 year, month, day, hour, min, sec);
	}
	else
	{
		snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%06d",
				 year, month, day, hour, min, sec, fsec);
		for (size_t len = strlen(buf); len > 0 && buf[len - 1] == '0'; len--)
			buf[len - 1] = '\0';
	}
}

static bool
is_date_unit_only(const char *unit)
{
	return (pg_strcasecmp(unit, "day") == 0 ||
			pg_strcasecmp(unit, "days") == 0 ||
			pg_strcasecmp(unit, "d") == 0 ||
			pg_strcasecmp(unit, "week") == 0 ||
			pg_strcasecmp(unit, "weeks") == 0 ||
			pg_strcasecmp(unit, "w") == 0 ||
			pg_strcasecmp(unit, "month") == 0 ||
			pg_strcasecmp(unit, "months") == 0 ||
			pg_strcasecmp(unit, "mon") == 0 ||
			pg_strcasecmp(unit, "mons") == 0 ||
			pg_strcasecmp(unit, "year") == 0 ||
			pg_strcasecmp(unit, "years") == 0 ||
			pg_strcasecmp(unit, "y") == 0 ||
			pg_strcasecmp(unit, "yr") == 0 ||
			pg_strcasecmp(unit, "yrs") == 0);
}

/*
 * Add offset units to base_usec.
 */
bool
pgbench_date_add(int64 base_usec, int64 offset, const char *unit, int64 *result_usec)
{
	char		clean_unit[32];
	const char *p = unit;
	size_t		len;

	if (unit == NULL)
		return false;

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;
	len = strlen(p);
	while (len > 0 && isspace((unsigned char) p[len - 1]))
		len--;
	if (len == 0 || len >= sizeof(clean_unit))
		return false;
	memcpy(clean_unit, p, len);
	clean_unit[len] = '\0';

	if (pg_strcasecmp(clean_unit, "microsecond") == 0 ||
		pg_strcasecmp(clean_unit, "microseconds") == 0 ||
		pg_strcasecmp(clean_unit, "us") == 0 ||
		pg_strcasecmp(clean_unit, "usec") == 0 ||
		pg_strcasecmp(clean_unit, "usecs") == 0)
	{
		*result_usec = base_usec + offset;
		return true;
	}
	else if (pg_strcasecmp(clean_unit, "millisecond") == 0 ||
			 pg_strcasecmp(clean_unit, "milliseconds") == 0 ||
			 pg_strcasecmp(clean_unit, "ms") == 0 ||
			 pg_strcasecmp(clean_unit, "msec") == 0 ||
			 pg_strcasecmp(clean_unit, "msecs") == 0)
	{
		*result_usec = base_usec + offset * INT64CONST(1000);
		return true;
	}
	else if (pg_strcasecmp(clean_unit, "second") == 0 ||
			 pg_strcasecmp(clean_unit, "seconds") == 0 ||
			 pg_strcasecmp(clean_unit, "s") == 0 ||
			 pg_strcasecmp(clean_unit, "sec") == 0 ||
			 pg_strcasecmp(clean_unit, "secs") == 0)
	{
		*result_usec = base_usec + offset * PGBENCH_USECS_PER_SEC;
		return true;
	}
	else if (pg_strcasecmp(clean_unit, "minute") == 0 ||
			 pg_strcasecmp(clean_unit, "minutes") == 0 ||
			 pg_strcasecmp(clean_unit, "m") == 0 ||
			 pg_strcasecmp(clean_unit, "min") == 0 ||
			 pg_strcasecmp(clean_unit, "mins") == 0)
	{
		*result_usec = base_usec + offset * PGBENCH_USECS_PER_MIN;
		return true;
	}
	else if (pg_strcasecmp(clean_unit, "hour") == 0 ||
			 pg_strcasecmp(clean_unit, "hours") == 0 ||
			 pg_strcasecmp(clean_unit, "h") == 0 ||
			 pg_strcasecmp(clean_unit, "hr") == 0 ||
			 pg_strcasecmp(clean_unit, "hrs") == 0)
	{
		*result_usec = base_usec + offset * PGBENCH_USECS_PER_HOUR;
		return true;
	}
	else if (pg_strcasecmp(clean_unit, "day") == 0 ||
			 pg_strcasecmp(clean_unit, "days") == 0 ||
			 pg_strcasecmp(clean_unit, "d") == 0)
	{
		*result_usec = base_usec + offset * PGBENCH_USECS_PER_DAY;
		return true;
	}
	else if (pg_strcasecmp(clean_unit, "week") == 0 ||
			 pg_strcasecmp(clean_unit, "weeks") == 0 ||
			 pg_strcasecmp(clean_unit, "w") == 0)
	{
		*result_usec = base_usec + offset * INT64CONST(7) * PGBENCH_USECS_PER_DAY;
		return true;
	}
	else if (pg_strcasecmp(clean_unit, "month") == 0 ||
			 pg_strcasecmp(clean_unit, "months") == 0 ||
			 pg_strcasecmp(clean_unit, "mon") == 0 ||
			 pg_strcasecmp(clean_unit, "mons") == 0 ||
			 pg_strcasecmp(clean_unit, "year") == 0 ||
			 pg_strcasecmp(clean_unit, "years") == 0 ||
			 pg_strcasecmp(clean_unit, "y") == 0 ||
			 pg_strcasecmp(clean_unit, "yr") == 0 ||
			 pg_strcasecmp(clean_unit, "yrs") == 0)
	{
		int64		month_delta = offset;
		int64		days;
		int64		time_usec;
		int			year,
					month,
					day;
		int64		total_months;
		int			new_year,
					new_month,
					max_day,
					jday;

		if (pg_strncasecmp(clean_unit, "y", 1) == 0)
			month_delta *= 12;

		days = base_usec / PGBENCH_USECS_PER_DAY;
		time_usec = base_usec % PGBENCH_USECS_PER_DAY;
		if (time_usec < 0)
		{
			days--;
			time_usec += PGBENCH_USECS_PER_DAY;
		}

		pgbench_j2date((int) days + PGBENCH_EPOCH_JDATE, &year, &month, &day);

		total_months = (int64) year * 12 + (month - 1) + month_delta;
		new_year = (int) (total_months / 12);
		new_month = (int) (total_months % 12) + 1;
		if (new_month <= 0)
		{
			new_year--;
			new_month += 12;
		}

		max_day = pgbench_days_in_month(new_year, new_month);
		if (day > max_day)
			day = max_day;

		jday = pgbench_date2j(new_year, new_month, day) - PGBENCH_EPOCH_JDATE;
		*result_usec = (int64) jday * PGBENCH_USECS_PER_DAY + time_usec;
		return true;
	}

	return false;
}

/*
 * Scale integer val in [0, max_val] proportionally across interval_str
 * (e.g., '7 years', '180 days', '24 hours') added to base_usec:
 *   date = base_date + (val / max_val) * interval
 */
bool
pgbench_int_to_date_window(int64 base_usec, int64 val, int64 max_val,
						   const char *interval_str, int64 *result_usec)
{
	const char *p = interval_str;
	char	   *endp;
	int64		interval_num = 1;
	char		unit_buf[32];
	size_t		ulen;
	int64		end_usec;
	int64		span_usec;

	if (interval_str == NULL || max_val <= 0)
		return false;

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;

	if (*p == '+' || *p == '-' || (*p >= '0' && *p <= '9'))
	{
		interval_num = strtoll(p, &endp, 10);
		if (endp == p)
			return false;
		p = endp;
		while (*p != '\0' && isspace((unsigned char) *p))
			p++;
	}

	ulen = strlen(p);
	while (ulen > 0 && isspace((unsigned char) p[ulen - 1]))
		ulen--;
	if (ulen == 0 || ulen >= sizeof(unit_buf))
		return false;
	memcpy(unit_buf, p, ulen);
	unit_buf[ulen] = '\0';

	if (!pgbench_date_add(base_usec, interval_num, unit_buf, &end_usec))
		return false;

	span_usec = end_usec - base_usec;

	if (base_usec % PGBENCH_USECS_PER_DAY == 0 && is_date_unit_only(unit_buf))
	{
		int64		span_days = span_usec / PGBENCH_USECS_PER_DAY;
		int64		scaled_days = (int64) llround(((double) val / (double) max_val) * (double) span_days);

		*result_usec = base_usec + scaled_days * PGBENCH_USECS_PER_DAY;
	}
	else
	{
		int64		scaled_usec = (int64) llround(((double) val / (double) max_val) * (double) span_usec);

		*result_usec = base_usec + scaled_usec;
	}

	return true;
}

/*-------------------------------------------------------------------------
 * Multi-Modal Vector, Sparse Vector, and Graph Edge Generators
 *-------------------------------------------------------------------------
 */

/*
 * Normalize a float32 vector in-place onto the unit hypersphere S^{dim-1}.
 * If the vector has zero L2 norm, leaves it unchanged.
 */
void
pgbench_normalize_vector(float *vec, int dim)
{
	double		norm2 = 0.0;

	for (int i = 0; i < dim; i++)
		norm2 += (double) vec[i] * (double) vec[i];

	if (norm2 > 0.0)
	{
		double		inv_norm = 1.0 / sqrt(norm2);

		for (int i = 0; i < dim; i++)
			vec[i] = (float) ((double) vec[i] * inv_norm);
	}
}

/*
 * Generate a uniform random float32 vector in [min_val, max_val]^dim,
 * optionally L2-normalized to unit length.
 */
void
pgbench_random_vector(pg_prng_state *state, int dim,
					  double min_val, double max_val,
					  bool normalize, float *out)
{
	double		span = max_val - min_val;

	Assert(dim > 0 && dim <= PGBENCH_MAX_VECTOR_DIM);

	for (int i = 0; i < dim; i++)
		out[i] = (float) (min_val + span * pg_prng_double(state));

	if (normalize)
		pgbench_normalize_vector(out, dim);
}

/*
 * Generate an isotropic Gaussian float32 vector N(0, sigma^2)^dim,
 * optionally L2-normalized onto the unit hypersphere S^{dim-1}.
 */
void
pgbench_random_vector_gaussian(pg_prng_state *state, int dim,
							   double sigma, bool normalize,
							   float *out)
{
	Assert(dim > 0 && dim <= PGBENCH_MAX_VECTOR_DIM);

	for (int i = 0; i < dim; i++)
		out[i] = (float) (pg_prng_double_normal(state) * sigma);

	if (normalize)
		pgbench_normalize_vector(out, dim);
}

/*
 * Generate a clustered float32 vector around a deterministic centroid on
 * the unit hypersphere S^{dim-1} with Gaussian perturbation sigma.
 *
 * When cluster_id < 0, a cluster in [0, num_clusters - 1] is chosen uniformly
 * using state.  When cluster_id >= 0, cluster (cluster_id % num_clusters) is
 * used directly.  Because centroids are derived deterministically via
 * pgbench_hash_murmur2(cluster, seed), server-side initialization and
 * client-side query generation share identical cluster geometry without
 * storing a centroid table in memory.
 */
void
pgbench_clustered_vector(pg_prng_state *state, int dim,
						 int num_clusters, double sigma,
						 int64 seed, int cluster_id, float *out)
{
	int			nclusters = (num_clusters > 0) ? num_clusters : 1;
	int			cid;
	uint64		cseed;

	Assert(dim > 0 && dim <= PGBENCH_MAX_VECTOR_DIM);

	if (cluster_id >= 0)
		cid = cluster_id % nclusters;
	else
		cid = (int) pgbench_random(state, 0, nclusters - 1);

	cseed = (uint64) pgbench_hash_murmur2((int64) cid,
										  (uint64) seed ^ UINT64CONST(0x9e3779b97f4a7c15));

	/* Reconstruct deterministic unit centroid c_cid on S^{dim-1} */
	for (int i = 0; i < dim; i++)
	{
		uint64		h = (uint64) pgbench_hash_murmur2((int64) i, cseed);
		double		u = ((double) (h & UINT64CONST(0x1fffffffffffff)) /
						 (double) UINT64CONST(0x1fffffffffffff)) * 2.0 - 1.0;

		out[i] = (float) u;
	}
	pgbench_normalize_vector(out, dim);

	/* Apply Gaussian perturbation around the unit centroid and renormalize */
	if (sigma > 0.0)
	{
		for (int i = 0; i < dim; i++)
			out[i] += (float) (pg_prng_double_normal(state) * sigma);
		pgbench_normalize_vector(out, dim);
	}
}

/*
 * Generate a planted vector at controlled Euclidean perturbation delta around
 * a deterministic unit base vector for target_id and seed.
 *
 * When delta == 0.0, returns the exact unit base vector for (target_id, seed).
 * When delta > 0.0, adds a random unit-norm perturbation vector scaled by
 * delta so that the Euclidean (L2) distance from the base vector is delta.
 */
void
pgbench_planted_vector(pg_prng_state *state, int dim,
					   int64 target_id, double delta,
					   int64 seed, float *out)
{
	uint64		tseed;

	Assert(dim > 0 && dim <= PGBENCH_MAX_VECTOR_DIM);

	tseed = (uint64) pgbench_hash_murmur2(target_id,
										  (uint64) seed ^ UINT64CONST(0x517cc1b727220a95));

	for (int i = 0; i < dim; i++)
	{
		uint64		h = (uint64) pgbench_hash_murmur2((int64) i, tseed);
		double		u = ((double) (h & UINT64CONST(0x1fffffffffffff)) /
						 (double) UINT64CONST(0x1fffffffffffff)) * 2.0 - 1.0;

		out[i] = (float) u;
	}
	pgbench_normalize_vector(out, dim);

	if (delta > 0.0)
	{
		pg_prng_state noise_rs;
		uint64		nseed = pg_prng_uint64(state);
		double		noise_norm2 = 0.0;

		pg_prng_seed(&noise_rs, nseed);
		for (int i = 0; i < dim; i++)
		{
			double		z = pg_prng_double_normal(&noise_rs);

			noise_norm2 += z * z;
		}
		if (noise_norm2 > 0.0)
		{
			double		scale_factor = delta / sqrt(noise_norm2);

			pg_prng_seed(&noise_rs, nseed);
			for (int i = 0; i < dim; i++)
			{
				double		z = pg_prng_double_normal(&noise_rs);

				out[i] += (float) (z * scale_factor);
			}
		}
	}
}

static int
pgbench_sparse_idx_cmp(const void *a, const void *b)
{
	int			ia = *(const int *) a;
	int			ib = *(const int *) b;

	return (ia > ib) - (ia < ib);
}

/*
 * Generate a sparse vector string in pgvector sparsevec format:
 *   "{i1:v1,i2:v2,...}/dim"
 * where 1 <= i1 < i2 < ... <= dim are nnz unique 1-based dimension indices
 * and v_k are uniform non-zero floats in [min_val, max_val].
 */
bool
pgbench_random_sparse_vector(pg_prng_state *state, int dim,
							 int nnz, double min_val, double max_val,
							 char *buf, size_t buflen)
{
	size_t		pos = 0;
	int			written;

	if (dim <= 0 || dim > 100000000 || nnz < 0 || nnz > dim ||
		min_val > max_val || buf == NULL || buflen < 8)
		return false;

	buf[pos++] = '{';

	if (nnz > 0)
	{
		/*
		 * Use Knuth's Algorithm S when dim <= 4 * nnz, Floyd's sampling +
		 * stack insertion sort when nnz <= 256, or Floyd's sampling with an
		 * open-addressing hash table + qsort in O(nnz log nnz) when nnz > 256.
		 */
		int			stack_indices[256];
		int		   *indices = stack_indices;
		bool		heap_alloc = false;

		if (nnz > 256)
		{
			indices = (int *) malloc(sizeof(int) * (size_t) nnz);
			if (indices == NULL)
				return false;
			heap_alloc = true;
		}

		if ((int64) nnz * 4 >= (int64) dim)
		{
			/* Knuth's Algorithm S: single linear scan yielding sorted indices */
			int			needed = nnz;
			int			out_idx = 0;

			for (int d = 1; d <= dim && needed > 0; d++)
			{
				int			remaining = dim - d + 1;

				if ((int) pg_prng_uint64_range(state, 0, remaining - 1) < needed)
				{
					indices[out_idx++] = d;
					needed--;
				}
			}
		}
		else if (nnz <= 256)
		{
			/* Floyd's algorithm for small k-subset in [1..dim], maintained sorted */
			int			count = 0;

			for (int j = dim - nnz + 1; j <= dim; j++)
			{
				int			t = (int) pgbench_random(state, 1, j);
				int			ins_val = t;
				int			k;

				for (k = 0; k < count; k++)
				{
					if (indices[k] == t)
					{
						ins_val = j;
						break;
					}
					if (indices[k] > t)
						break;
				}

				k = count;
				while (k > 0 && indices[k - 1] > ins_val)
				{
					indices[k] = indices[k - 1];
					k--;
				}
				indices[k] = ins_val;
				count++;
			}
		}
		else
		{
			/* Floyd's algorithm with open-addressing hash set + qsort: O(nnz log nnz) */
			size_t		ht_cap = 512;
			size_t		mask;
			int		   *ht;
			int			out_idx = 0;

			while (ht_cap < (size_t) nnz * 2)
				ht_cap <<= 1;

			ht = (int *) calloc(ht_cap, sizeof(int));
			if (ht == NULL)
			{
				free(indices);
				return false;
			}
			mask = ht_cap - 1;

			for (int j = dim - nnz + 1; j <= dim; j++)
			{
				int			t = (int) pgbench_random(state, 1, j);
				int			ins_val = t;
				size_t		slot = ((uint64) t * UINT64CONST(0x9e3779b97f4a7c15)) & mask;

				while (ht[slot] != 0 && ht[slot] != t)
					slot = (slot + 1) & mask;

				if (ht[slot] == t)
				{
					ins_val = j;
					slot = ((uint64) j * UINT64CONST(0x9e3779b97f4a7c15)) & mask;
					while (ht[slot] != 0)
						slot = (slot + 1) & mask;
				}

				ht[slot] = ins_val;
				indices[out_idx++] = ins_val;
			}

			free(ht);
			qsort(indices, (size_t) nnz, sizeof(int), pgbench_sparse_idx_cmp);
		}

		for (int i = 0; i < nnz; i++)
		{
			double		val = min_val + (max_val - min_val) * pg_prng_double(state);

			if (fabs(val) < 1e-7)
				val = (max_val != 0.0) ? (float) (max_val * 0.5) : 1.0;

			written = snprintf(buf + pos, buflen - pos,
							   "%s%d:%.6g",
							   (i > 0) ? "," : "",
							   indices[i], (double) (float) val);
			if (written < 0 || (size_t) written >= buflen - pos)
			{
				if (heap_alloc)
					free(indices);
				return false;
			}
			pos += (size_t) written;
		}

		if (heap_alloc)
			free(indices);
	}

	written = snprintf(buf + pos, buflen - pos, "}/%d", dim);
	if (written < 0 || (size_t) written >= buflen - pos)
		return false;

	return true;
}

/*
 * Generate a directed graph edge (src, dst) in [0, 2^scale_log2 - 1] using
 * the Recursive Matrix (R-MAT) Kronecker model with quadrant probabilities
 * (a, b, c, d = 1 - (a + b + c)).  When seed != 0, applies pgbench_permute
 * to scramble vertex IDs while preserving the power-law degree distribution.
 */
bool
pgbench_rmat_edge(pg_prng_state *state, int scale_log2,
				  double a, double b, double c, int64 seed,
				  int64 *src_out, int64 *dst_out)
{
	int64		src = 0;
	int64		dst = 0;
	double		ab = a + b;
	double		abc = a + b + c;

	if (scale_log2 < 1 || scale_log2 > 60)
		return false;
	if (a < 0.0 || b < 0.0 || c < 0.0 || abc > 1.000001)
		return false;

	for (int bit = scale_log2 - 1; bit >= 0; bit--)
	{
		double		u = pg_prng_double(state);

		if (u < a)
		{
			/* top-left quadrant (0, 0) */
		}
		else if (u < ab)
		{
			/* top-right quadrant (0, 1) */
			dst |= (INT64CONST(1) << bit);
		}
		else if (u < abc)
		{
			/* bottom-left quadrant (1, 0) */
			src |= (INT64CONST(1) << bit);
		}
		else
		{
			/* bottom-right quadrant (1, 1) */
			src |= (INT64CONST(1) << bit);
			dst |= (INT64CONST(1) << bit);
		}
	}

	if (seed != 0)
	{
		int64		num_nodes = INT64CONST(1) << scale_log2;

		src = pgbench_permute(src, num_nodes, seed);
		dst = pgbench_permute(dst, num_nodes, seed);
	}

	*src_out = src;
	*dst_out = dst;
	return true;
}

/*
 * Parse a vector distance metric name ("l2", "cosine", "ip").
 */
bool
pgbench_parse_vec_dist_metric(const char *metric_str,
							  PgBenchVecDistMetric *metric_out)
{
	if (metric_str == NULL)
		return false;

	if (pg_strcasecmp(metric_str, "l2") == 0 ||
		pg_strcasecmp(metric_str, "euclidean") == 0)
	{
		*metric_out = PGBENCH_VEC_DIST_L2;
		return true;
	}
	else if (pg_strcasecmp(metric_str, "cosine") == 0 ||
			 pg_strcasecmp(metric_str, "cos") == 0)
	{
		*metric_out = PGBENCH_VEC_DIST_COSINE;
		return true;
	}
	else if (pg_strcasecmp(metric_str, "ip") == 0 ||
			 pg_strcasecmp(metric_str, "inner_product") == 0 ||
			 pg_strcasecmp(metric_str, "dot") == 0)
	{
		*metric_out = PGBENCH_VEC_DIST_IP;
		return true;
	}

	return false;
}

/*
 * Compute exact distance between two float32 vectors of dimension dim:
 * - PGBENCH_VEC_DIST_L2: Euclidean distance sqrt(sum((v1[i] - v2[i])^2))
 * - PGBENCH_VEC_DIST_COSINE: Cosine distance 1.0 - (v1 . v2) / (||v1|| * ||v2||)
 * - PGBENCH_VEC_DIST_IP: Negative inner product -(v1 . v2) (matching pgvector <#>)
 */
double
pgbench_vector_distance(const float *v1, const float *v2,
						int dim, PgBenchVecDistMetric metric)
{
	if (dim <= 0 || v1 == NULL || v2 == NULL)
		return 0.0;

	switch (metric)
	{
		case PGBENCH_VEC_DIST_L2:
			{
				double		sum_sq = 0.0;

				for (int i = 0; i < dim; i++)
				{
					double		diff = (double) v1[i] - (double) v2[i];

					sum_sq += diff * diff;
				}
				return sqrt(sum_sq);
			}

		case PGBENCH_VEC_DIST_COSINE:
			{
				double		dot = 0.0;
				double		norm1 = 0.0;
				double		norm2 = 0.0;
				double		denom;

				for (int i = 0; i < dim; i++)
				{
					double		a = (double) v1[i];
					double		b = (double) v2[i];

					dot += a * b;
					norm1 += a * a;
					norm2 += b * b;
				}
				denom = sqrt(norm1) * sqrt(norm2);
				if (denom == 0.0)
					return 1.0;
				return 1.0 - (dot / denom);
			}

		case PGBENCH_VEC_DIST_IP:
			{
				double		dot = 0.0;

				for (int i = 0; i < dim; i++)
					dot += (double) v1[i] * (double) v2[i];
				return -dot;
			}
	}

	return 0.0;
}

/*
 * Format a float32 vector into "[v0,v1,...]" in a single pass.
 * Returns the number of characters written (excluding NUL), or 0 if buflen
 * is insufficient.
 */
size_t
pgbench_format_vector(const float *vec, int dim, char *buf, size_t buflen)
{
	size_t		pos = 0;

	if (buf == NULL || buflen < 4 || dim < 0)
		return 0;

	buf[pos++] = '[';
	for (int i = 0; i < dim; i++)
	{
		int			written;

		if (i > 0)
		{
			if (pos + 2 >= buflen)
				return 0;
			buf[pos++] = ',';
		}

		written = snprintf(buf + pos, buflen - pos, "%.6g", (double) vec[i]);
		if (written < 0 || (size_t) written >= buflen - pos)
			return 0;
		pos += (size_t) written;
	}

	if (pos + 2 > buflen)
		return 0;
	buf[pos++] = ']';
	buf[pos] = '\0';
	return pos;
}

/*
 * Validate a pgvector-style "[f1,f2,...]" literal and count its dimension.
 * Requires at least one finite float element inside '[' and ']'.
 */
bool
pgbench_parse_vector_count(const char *str, int *dim_out)
{
	const char *p = str;
	int			dim = 0;

	if (str == NULL)
		return false;

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;
	if (*p != '[')
		return false;
	p++;

	for (;;)
	{
		char	   *endp;
		double		val;

		while (*p != '\0' && isspace((unsigned char) *p))
			p++;
		if (*p == '\0' || *p == ']')
			return false;		/* empty vector or trailing comma */

		errno = 0;
		val = strtod(p, &endp);
		if (endp == p || errno == ERANGE || isnan(val) || isinf(val))
			return false;

		dim++;
		if (dim > PGBENCH_MAX_VECTOR_DIM)
			return false;

		p = endp;
		while (*p != '\0' && isspace((unsigned char) *p))
			p++;

		if (*p == ',')
		{
			p++;
			continue;
		}
		else if (*p == ']')
		{
			p++;
			break;
		}
		else
			return false;
	}

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;
	if (*p != '\0')
		return false;

	*dim_out = dim;
	return (dim >= 1);
}

/*
 * Parse a validated "[f1,f2,...]" literal into out[0 .. expected_dim - 1].
 */
bool
pgbench_parse_vector_into(const char *str, float *out, int expected_dim)
{
	const char *p = str;
	int			idx = 0;

	if (str == NULL || out == NULL || expected_dim <= 0)
		return false;

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;
	if (*p != '[')
		return false;
	p++;

	while (idx < expected_dim)
	{
		char	   *endp;
		double		val;

		while (*p != '\0' && isspace((unsigned char) *p))
			p++;

		errno = 0;
		val = strtod(p, &endp);
		if (endp == p || errno == ERANGE || isnan(val) || isinf(val))
			return false;

		out[idx++] = (float) val;
		p = endp;

		while (*p != '\0' && isspace((unsigned char) *p))
			p++;

		if (idx < expected_dim)
		{
			if (*p != ',')
				return false;
			p++;
		}
		else
		{
			if (*p != ']')
				return false;
			p++;
		}
	}

	while (*p != '\0' && isspace((unsigned char) *p))
		p++;
	return (*p == '\0');
}


