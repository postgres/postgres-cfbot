/*-------------------------------------------------------------------------
 *
 * script.c
 *		Script parsing, AST management, and expression evaluation for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/script.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>

#include "common/fe_memutils.h"
#include "common/int.h"
#include "common/logging.h"
#include "common/pg_prng.h"
#include "common/string.h"
#include "pgbench.h"
#include "port/pg_bitutils.h"

/* X/Open (XSI) requires <math.h> to provide M_PI, but core POSIX does not */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* Global script repository and configuration */
ParsedScript sql_script[MAX_SCRIPTS];
int			num_scripts = 0;
int64		total_weight = 0;
QueryMode	querymode = QUERY_SIMPLE;
const char *const QUERYMODE[] = {"simple", "extended", "prepared"};

/* Builtin test scripts */
const BuiltinScript builtin_script[] =
{
	{
		"tpcb-like",
		"<builtin: TPC-B (sort of)>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"\\set bid random(1, " CppAsString2(nbranches) " * :scale)\n"
		"\\set tid random(1, " CppAsString2(ntellers) " * :scale)\n"
		"\\set delta random(-5000, 5000)\n"
		"BEGIN;\n"
		"UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
		"UPDATE pgbench_tellers SET tbalance = tbalance + :delta WHERE tid = :tid;\n"
		"UPDATE pgbench_branches SET bbalance = bbalance + :delta WHERE bid = :bid;\n"
		"INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
		"END;\n"
	},
	{
		"simple-update",
		"<builtin: simple update>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"\\set bid random(1, " CppAsString2(nbranches) " * :scale)\n"
		"\\set tid random(1, " CppAsString2(ntellers) " * :scale)\n"
		"\\set delta random(-5000, 5000)\n"
		"BEGIN;\n"
		"UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
		"INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
		"END;\n"
	},
	{
		"select-only",
		"<builtin: select only>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
	}
};

const size_t num_builtin_scripts = lengthof(builtin_script);

/* callback functions for our flex lexer */
static const PsqlScanCallbacks pgbench_callbacks = {
	NULL,						/* don't need get_variable functionality */
};

/*
 * random number generator: uniform distribution from min to max inclusive.
 *
 * Although the limits are expressed as int64, you can't generate the full
 * int64 range in one call, because the difference of the limits mustn't
 * overflow int64.  This is not checked.
 */
static int64
getrand(pg_prng_state *state, int64 min, int64 max)
{
	return min + (int64) pg_prng_uint64_range(state, 0, max - min);
}

/*
 * random number generator: exponential distribution from min to max inclusive.
 * the parameter is so that the density of probability for the last cut-off max
 * value is exp(-parameter).
 */
static int64
getExponentialRand(pg_prng_state *state, int64 min, int64 max,
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
static int64
getGaussianRand(pg_prng_state *state, int64 min, int64 max,
				double parameter)
{
	double		stdev;
	double		rand;

	/* abort if parameter is too low, but must really be checked beforehand */
	Assert(parameter >= MIN_GAUSSIAN_PARAM);

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
static int64
getZipfianRand(pg_prng_state *state, int64 min, int64 max, double s)
{
	int64		n = max - min + 1;

	/* abort if parameter is invalid */
	Assert(MIN_ZIPFIAN_PARAM <= s && s <= MAX_ZIPFIAN_PARAM);

	return min - 1 + computeIterativeZipfian(state, n, s);
}

/*
 * FNV-1a hash function
 */
static int64
getHashFnv1a(int64 val, uint64 seed)
{
	int64		result;
	int			i;

	result = FNV_OFFSET_BASIS ^ seed;
	for (i = 0; i < 8; ++i)
	{
		int32		octet = val & 0xff;

		val = val >> 8;
		result = result ^ octet;
		result = result * FNV_PRIME;
	}

	return result;
}

/*
 * Murmur2 hash function
 *
 * Based on original work of Austin Appleby
 * https://github.com/aappleby/smhasher/blob/master/src/MurmurHash2.cpp
 */
static int64
getHashMurmur2(int64 val, uint64 seed)
{
	uint64		result = seed ^ MM2_MUL_TIMES_8;	/* sizeof(int64) */
	uint64		k = (uint64) val;

	k *= MM2_MUL;
	k ^= k >> MM2_ROT;
	k *= MM2_MUL;

	result ^= k;
	result *= MM2_MUL;

	result ^= result >> MM2_ROT;
	result *= MM2_MUL;
	result ^= result >> MM2_ROT;

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
static int64
permute(const int64 val, const int64 isize, const int64 seed)
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
 * Check if a function requires lazy evaluation (short-circuiting).
 */
bool
isLazyFunc(PgBenchFunction func)
{
	return func == PGBENCH_AND || func == PGBENCH_OR || func == PGBENCH_CASE;
}

/*
 * Lazy evaluation of short-circuiting functions (AND, OR, CASE).
 */
bool
evalLazyFunc(CState *st,
			 PgBenchFunction func, PgBenchExprLink *args, PgBenchValue *retval)
{
	PgBenchValue a1,
				a2;
	bool		ba1,
				ba2;

	Assert(isLazyFunc(func) && args != NULL && args->next != NULL);

	/* args points to first condition */
	if (!evaluateExpr(st, args->expr, &a1))
		return false;

	/* second condition for AND/OR and corresponding branch for CASE */
	args = args->next;

	switch (func)
	{
		case PGBENCH_AND:
			if (a1.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}

			if (!coerceToBool(&a1, &ba1))
				return false;

			if (!ba1)
			{
				setBoolValue(retval, false);
				return true;
			}

			if (!evaluateExpr(st, args->expr, &a2))
				return false;

			if (a2.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}
			else if (!coerceToBool(&a2, &ba2))
				return false;
			else
			{
				setBoolValue(retval, ba2);
				return true;
			}

			return true;

		case PGBENCH_OR:

			if (a1.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}

			if (!coerceToBool(&a1, &ba1))
				return false;

			if (ba1)
			{
				setBoolValue(retval, true);
				return true;
			}

			if (!evaluateExpr(st, args->expr, &a2))
				return false;

			if (a2.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}
			else if (!coerceToBool(&a2, &ba2))
				return false;
			else
			{
				setBoolValue(retval, ba2);
				return true;
			}

		case PGBENCH_CASE:
			/* when true, execute branch */
			if (valueTruth(&a1))
				return evaluateExpr(st, args->expr, retval);

			/* now args contains next condition or final else expression */
			args = args->next;

			/* final else case? */
			if (args->next == NULL)
				return evaluateExpr(st, args->expr, retval);

			/* no, another when, proceed */
			return evalLazyFunc(st, PGBENCH_CASE, args, retval);

		default:
			/* internal error, cannot get here */
			Assert(0);
			break;
	}
	return false;
}

/* maximum number of function arguments */
#define MAX_FARGS 16

/*
 * Recursive evaluation of standard functions,
 * which do not require lazy evaluation.
 */
bool
evalStandardFunc(CState *st,
				 PgBenchFunction func, PgBenchExprLink *args,
				 PgBenchValue *retval)
{
	/* evaluate all function arguments */
	int			nargs = 0;
	PgBenchValue vargs[MAX_FARGS] = {0};
	PgBenchExprLink *l = args;
	bool		has_null = false;

	for (nargs = 0; nargs < MAX_FARGS && l != NULL; nargs++, l = l->next)
	{
		if (!evaluateExpr(st, l->expr, &vargs[nargs]))
			return false;
		has_null |= vargs[nargs].type == PGBT_NULL;
	}

	if (l != NULL)
	{
		pg_log_error("too many function arguments, maximum is %d", MAX_FARGS);
		return false;
	}

	/* NULL arguments */
	if (has_null && func != PGBENCH_IS && func != PGBENCH_DEBUG)
	{
		setNullValue(retval);
		return true;
	}

	/* then evaluate function */
	switch (func)
	{
			/* overloaded operators */
		case PGBENCH_ADD:
		case PGBENCH_SUB:
		case PGBENCH_MUL:
		case PGBENCH_DIV:
		case PGBENCH_MOD:
		case PGBENCH_EQ:
		case PGBENCH_NE:
		case PGBENCH_LE:
		case PGBENCH_LT:
			{
				PgBenchValue *lval = &vargs[0],
						   *rval = &vargs[1];

				Assert(nargs == 2);

				/* overloaded type management, double if some double */
				if ((lval->type == PGBT_DOUBLE ||
					 rval->type == PGBT_DOUBLE) && func != PGBENCH_MOD)
				{
					double		ld,
								rd;

					if (!coerceToDouble(lval, &ld) ||
						!coerceToDouble(rval, &rd))
						return false;

					switch (func)
					{
						case PGBENCH_ADD:
							setDoubleValue(retval, ld + rd);
							return true;

						case PGBENCH_SUB:
							setDoubleValue(retval, ld - rd);
							return true;

						case PGBENCH_MUL:
							setDoubleValue(retval, ld * rd);
							return true;

						case PGBENCH_DIV:
							setDoubleValue(retval, ld / rd);
							return true;

						case PGBENCH_EQ:
							setBoolValue(retval, ld == rd);
							return true;

						case PGBENCH_NE:
							setBoolValue(retval, ld != rd);
							return true;

						case PGBENCH_LE:
							setBoolValue(retval, ld <= rd);
							return true;

						case PGBENCH_LT:
							setBoolValue(retval, ld < rd);
							return true;

						default:
							/* cannot get here */
							Assert(0);
					}
				}
				else			/* we have integer operands, or % */
				{
					int64		li,
								ri,
								res;

					if (!coerceToInt(lval, &li) ||
						!coerceToInt(rval, &ri))
						return false;

					switch (func)
					{
						case PGBENCH_ADD:
							if (pg_add_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint add out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_SUB:
							if (pg_sub_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint sub out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_MUL:
							if (pg_mul_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint mul out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_EQ:
							setBoolValue(retval, li == ri);
							return true;

						case PGBENCH_NE:
							setBoolValue(retval, li != ri);
							return true;

						case PGBENCH_LE:
							setBoolValue(retval, li <= ri);
							return true;

						case PGBENCH_LT:
							setBoolValue(retval, li < ri);
							return true;

						case PGBENCH_DIV:
						case PGBENCH_MOD:
							if (ri == 0)
							{
								pg_log_error("division by zero");
								return false;
							}
							/* special handling of -1 divisor */
							if (ri == -1)
							{
								if (func == PGBENCH_DIV)
								{
									/* overflow check (needed for INT64_MIN) */
									if (li == PG_INT64_MIN)
									{
										pg_log_error("bigint div out of range");
										return false;
									}
									else
										setIntValue(retval, -li);
								}
								else
									setIntValue(retval, 0);
								return true;
							}
							/* else divisor is not -1 */
							if (func == PGBENCH_DIV)
								setIntValue(retval, li / ri);
							else	/* func == PGBENCH_MOD */
								setIntValue(retval, li % ri);

							return true;

						default:
							/* cannot get here */
							Assert(0);
					}
				}

				Assert(0);
				return false;	/* NOTREACHED */
			}

			/* integer bitwise operators */
		case PGBENCH_BITAND:
		case PGBENCH_BITOR:
		case PGBENCH_BITXOR:
		case PGBENCH_LSHIFT:
		case PGBENCH_RSHIFT:
			{
				int64		li,
							ri;

				if (!coerceToInt(&vargs[0], &li) || !coerceToInt(&vargs[1], &ri))
					return false;

				if (func == PGBENCH_BITAND)
					setIntValue(retval, li & ri);
				else if (func == PGBENCH_BITOR)
					setIntValue(retval, li | ri);
				else if (func == PGBENCH_BITXOR)
					setIntValue(retval, li ^ ri);
				else if (func == PGBENCH_LSHIFT)
					setIntValue(retval, li << ri);
				else if (func == PGBENCH_RSHIFT)
					setIntValue(retval, li >> ri);
				else			/* cannot get here */
					Assert(0);

				return true;
			}

			/* logical operators */
		case PGBENCH_NOT:
			{
				bool		b;

				if (!coerceToBool(&vargs[0], &b))
					return false;

				setBoolValue(retval, !b);
				return true;
			}

			/* no arguments */
		case PGBENCH_PI:
			setDoubleValue(retval, M_PI);
			return true;

			/* 1 overloaded argument */
		case PGBENCH_ABS:
			{
				PgBenchValue *varg = &vargs[0];

				Assert(nargs == 1);

				if (varg->type == PGBT_INT)
				{
					int64		i = varg->u.ival;

					setIntValue(retval, i < 0 ? -i : i);
				}
				else
				{
					double		d = varg->u.dval;

					Assert(varg->type == PGBT_DOUBLE);
					setDoubleValue(retval, d < 0.0 ? -d : d);
				}

				return true;
			}

		case PGBENCH_DEBUG:
			{
				PgBenchValue *varg = &vargs[0];

				Assert(nargs == 1);

				fprintf(stderr, "debug(script=%d,command=%d): ",
						st->use_file, st->command + 1);

				if (varg->type == PGBT_NULL)
					fprintf(stderr, "null\n");
				else if (varg->type == PGBT_BOOLEAN)
					fprintf(stderr, "boolean %s\n", varg->u.bval ? "true" : "false");
				else if (varg->type == PGBT_INT)
					fprintf(stderr, "int " INT64_FORMAT "\n", varg->u.ival);
				else if (varg->type == PGBT_DOUBLE)
					fprintf(stderr, "double %.*g\n", DBL_DIG, varg->u.dval);
				else			/* internal error, unexpected type */
					Assert(0);

				*retval = *varg;

				return true;
			}

			/* 1 double argument */
		case PGBENCH_DOUBLE:
		case PGBENCH_SQRT:
		case PGBENCH_LN:
		case PGBENCH_EXP:
			{
				double		dval;

				Assert(nargs == 1);

				if (!coerceToDouble(&vargs[0], &dval))
					return false;

				if (func == PGBENCH_SQRT)
					dval = sqrt(dval);
				else if (func == PGBENCH_LN)
					dval = log(dval);
				else if (func == PGBENCH_EXP)
					dval = exp(dval);
				/* else is cast: do nothing */

				setDoubleValue(retval, dval);
				return true;
			}

			/* 1 int argument */
		case PGBENCH_INT:
			{
				int64		ival;

				Assert(nargs == 1);

				if (!coerceToInt(&vargs[0], &ival))
					return false;

				setIntValue(retval, ival);
				return true;
			}

			/* variable number of arguments */
		case PGBENCH_LEAST:
		case PGBENCH_GREATEST:
			{
				bool		havedouble;
				int			i;

				Assert(nargs >= 1);

				/* need double result if any input is double */
				havedouble = false;
				for (i = 0; i < nargs; i++)
				{
					if (vargs[i].type == PGBT_DOUBLE)
					{
						havedouble = true;
						break;
					}
				}
				if (havedouble)
				{
					double		extremum;

					if (!coerceToDouble(&vargs[0], &extremum))
						return false;
					for (i = 1; i < nargs; i++)
					{
						double		dval;

						if (!coerceToDouble(&vargs[i], &dval))
							return false;
						if (func == PGBENCH_LEAST)
							extremum = Min(extremum, dval);
						else
							extremum = Max(extremum, dval);
					}
					setDoubleValue(retval, extremum);
				}
				else
				{
					int64		extremum;

					if (!coerceToInt(&vargs[0], &extremum))
						return false;
					for (i = 1; i < nargs; i++)
					{
						int64		ival;

						if (!coerceToInt(&vargs[i], &ival))
							return false;
						if (func == PGBENCH_LEAST)
							extremum = Min(extremum, ival);
						else
							extremum = Max(extremum, ival);
					}
					setIntValue(retval, extremum);
				}
				return true;
			}

			/* random functions */
		case PGBENCH_RANDOM:
		case PGBENCH_RANDOM_EXPONENTIAL:
		case PGBENCH_RANDOM_GAUSSIAN:
		case PGBENCH_RANDOM_ZIPFIAN:
			{
				int64		imin,
							imax,
							delta;

				Assert(nargs >= 2);

				if (!coerceToInt(&vargs[0], &imin) ||
					!coerceToInt(&vargs[1], &imax))
					return false;

				/* check random range */
				if (unlikely(imin > imax))
				{
					pg_log_error("empty range given to random");
					return false;
				}
				else if (unlikely(pg_sub_s64_overflow(imax, imin, &delta) ||
								  pg_add_s64_overflow(delta, 1, &delta)))
				{
					/* prevent int overflows in random functions */
					pg_log_error("random range is too large");
					return false;
				}

				if (func == PGBENCH_RANDOM)
				{
					Assert(nargs == 2);
					setIntValue(retval, getrand(&st->cs_func_rs, imin, imax));
				}
				else			/* gaussian & exponential */
				{
					double		param;

					Assert(nargs == 3);

					if (!coerceToDouble(&vargs[2], &param))
						return false;

					if (func == PGBENCH_RANDOM_GAUSSIAN)
					{
						if (param < MIN_GAUSSIAN_PARAM)
						{
							pg_log_error("gaussian parameter must be at least %f (not %f)",
										 MIN_GAUSSIAN_PARAM, param);
							return false;
						}

						setIntValue(retval,
									getGaussianRand(&st->cs_func_rs,
													imin, imax, param));
					}
					else if (func == PGBENCH_RANDOM_ZIPFIAN)
					{
						if (param < MIN_ZIPFIAN_PARAM || param > MAX_ZIPFIAN_PARAM)
						{
							pg_log_error("zipfian parameter must be in range [%.3f, %.0f] (not %f)",
										 MIN_ZIPFIAN_PARAM, MAX_ZIPFIAN_PARAM, param);
							return false;
						}

						setIntValue(retval,
									getZipfianRand(&st->cs_func_rs, imin, imax, param));
					}
					else		/* exponential */
					{
						if (param <= 0.0)
						{
							pg_log_error("exponential parameter must be greater than zero (not %f)",
										 param);
							return false;
						}

						setIntValue(retval,
									getExponentialRand(&st->cs_func_rs,
													   imin, imax, param));
					}
				}

				return true;
			}

		case PGBENCH_POW:
			{
				PgBenchValue *lval = &vargs[0];
				PgBenchValue *rval = &vargs[1];
				double		ld,
							rd;

				Assert(nargs == 2);

				if (!coerceToDouble(lval, &ld) ||
					!coerceToDouble(rval, &rd))
					return false;

				setDoubleValue(retval, pow(ld, rd));

				return true;
			}

		case PGBENCH_IS:
			{
				Assert(nargs == 2);

				/*
				 * note: this simple implementation is more permissive than
				 * SQL
				 */
				setBoolValue(retval,
							 vargs[0].type == vargs[1].type &&
							 vargs[0].u.bval == vargs[1].u.bval);
				return true;
			}

			/* hashing */
		case PGBENCH_HASH_FNV1A:
		case PGBENCH_HASH_MURMUR2:
			{
				int64		val,
							seed;

				Assert(nargs == 2);

				if (!coerceToInt(&vargs[0], &val) ||
					!coerceToInt(&vargs[1], &seed))
					return false;

				if (func == PGBENCH_HASH_MURMUR2)
					setIntValue(retval, getHashMurmur2(val, seed));
				else if (func == PGBENCH_HASH_FNV1A)
					setIntValue(retval, getHashFnv1a(val, seed));
				else
					/* cannot get here */
					Assert(0);

				return true;
			}

		case PGBENCH_PERMUTE:
			{
				int64		val,
							size,
							seed;

				Assert(nargs == 3);

				if (!coerceToInt(&vargs[0], &val) ||
					!coerceToInt(&vargs[1], &size) ||
					!coerceToInt(&vargs[2], &seed))
					return false;

				if (size <= 0)
				{
					pg_log_error("permute size parameter must be greater than zero");
					return false;
				}

				setIntValue(retval, permute(val, size, seed));
				return true;
			}

		default:
			/* cannot get here */
			Assert(0);
			/* dead code to avoid a compiler warning */
			return false;
	}
}

/*
 * Evaluate a function (routes to lazy or standard evaluation).
 */
bool
evalFunc(CState *st,
		 PgBenchFunction func, PgBenchExprLink *args, PgBenchValue *retval)
{
	if (isLazyFunc(func))
		return evalLazyFunc(st, func, args, retval);
	else
		return evalStandardFunc(st, func, args, retval);
}

/*
 * Recursive evaluation of an expression in a pgbench script
 * using the current state of variables.
 * Returns whether the evaluation was ok,
 * the value itself is returned through the retval pointer.
 */
bool
evaluateExpr(CState *st, PgBenchExpr *expr, PgBenchValue *retval)
{
	switch (expr->etype)
	{
		case ENODE_CONSTANT:
			{
				*retval = expr->u.constant;
				return true;
			}

		case ENODE_VARIABLE:
			{
				Variable   *var;

				if ((var = lookupVariable(&st->variables, expr->u.variable.varname)) == NULL)
				{
					pg_log_error("undefined variable \"%s\"", expr->u.variable.varname);
					return false;
				}

				if (!makeVariableValue(var))
					return false;

				*retval = var->value;
				return true;
			}

		case ENODE_FUNCTION:
			return evalFunc(st,
							expr->u.function.function,
							expr->u.function.args,
							retval);

		default:
			/* internal error which should never occur */
			pg_fatal("unexpected enode type in evaluation: %d", expr->etype);
	}
}

/*
 * Convert command name to meta-command enum identifier
 */
MetaCommand
getMetaCommand(const char *cmd)
{
	MetaCommand mc;

	if (cmd == NULL)
		mc = META_NONE;
	else if (pg_strcasecmp(cmd, "set") == 0)
		mc = META_SET;
	else if (pg_strcasecmp(cmd, "setshell") == 0)
		mc = META_SETSHELL;
	else if (pg_strcasecmp(cmd, "shell") == 0)
		mc = META_SHELL;
	else if (pg_strcasecmp(cmd, "sleep") == 0)
		mc = META_SLEEP;
	else if (pg_strcasecmp(cmd, "if") == 0)
		mc = META_IF;
	else if (pg_strcasecmp(cmd, "elif") == 0)
		mc = META_ELIF;
	else if (pg_strcasecmp(cmd, "else") == 0)
		mc = META_ELSE;
	else if (pg_strcasecmp(cmd, "endif") == 0)
		mc = META_ENDIF;
	else if (pg_strcasecmp(cmd, "gset") == 0)
		mc = META_GSET;
	else if (pg_strcasecmp(cmd, "aset") == 0)
		mc = META_ASET;
	else if (pg_strcasecmp(cmd, "startpipeline") == 0)
		mc = META_STARTPIPELINE;
	else if (pg_strcasecmp(cmd, "syncpipeline") == 0)
		mc = META_SYNCPIPELINE;
	else if (pg_strcasecmp(cmd, "endpipeline") == 0)
		mc = META_ENDPIPELINE;
	else
		mc = META_NONE;
	return mc;
}

/*
 * Replace :param with $n throughout the command's SQL text, which
 * is a modifiable string in cmd->lines.
 */
bool
parseQuery(Command *cmd)
{
	char	   *sql,
			   *p;

	cmd->argc = 1;

	p = sql = pg_strdup(cmd->lines.data);
	while ((p = strchr(p, ':')) != NULL)
	{
		char		var[13];
		char	   *name;
		int			eaten;

		name = parseVariable(p, &eaten);
		if (name == NULL)
		{
			while (*p == ':')
			{
				p++;
			}
			continue;
		}

		/*
		 * cmd->argv[0] is the SQL statement itself, so the max number of
		 * arguments is one less than MAX_ARGS
		 */
		if (cmd->argc >= MAX_ARGS)
		{
			pg_log_error("statement has too many arguments (maximum is %d): %s",
						 MAX_ARGS - 1, cmd->lines.data);
			pg_free(name);
			return false;
		}

		sprintf(var, "$%d", cmd->argc);
		p = replaceVariable(&sql, p, eaten, var);

		cmd->argv[cmd->argc] = name;
		cmd->argc++;
	}

	Assert(cmd->argv[0] == NULL);
	cmd->argv[0] = sql;
	return true;
}

/*
 * syntax error while parsing a script (in practice, while parsing a
 * backslash command, because we don't detect syntax errors in SQL)
 *
 * source: source of script (filename or builtin-script ID)
 * lineno: line number within script (count from 1)
 * line: whole line of backslash command, if available
 * command: backslash command name, if available
 * msg: the actual error message
 * more: optional extra message
 * column: zero-based column number, or -1 if unknown
 */
void
syntax_error(const char *source, int lineno,
			 const char *line, const char *command,
			 const char *msg, const char *more, int column)
{
	PQExpBufferData buf;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf, "%s:%d: %s", source, lineno, msg);
	if (more != NULL)
		appendPQExpBuffer(&buf, " (%s)", more);
	if (command != NULL)
		appendPQExpBuffer(&buf, " in command \"%s\"", command);

	pg_log_error("%s", buf.data);

	termPQExpBuffer(&buf);

	if (line != NULL)
	{
		fprintf(stderr, "%s\n", line);
		if (column >= 0)
			fprintf(stderr, "%*c error found here\n", column + 1, '^');
	}

	exit(1);
}

/*
 * Return a pointer to the start of the SQL command, after skipping over
 * whitespace and "--" comments.
 * If the end of the string is reached, return NULL.
 */
char *
skip_sql_comments(char *sql_command)
{
	char	   *p = sql_command;

	/* Skip any leading whitespace, as well as "--" style comments */
	for (;;)
	{
		if (isspace((unsigned char) *p))
			p++;
		else if (strncmp(p, "--", 2) == 0)
		{
			p = strchr(p, '\n');
			if (p == NULL)
				return NULL;
			p++;
		}
		else
			break;
	}

	/* NULL if there's nothing but whitespace and comments */
	if (*p == '\0')
		return NULL;

	return p;
}

/*
 * Parse a SQL command; return a Command struct, or NULL if it's a comment
 *
 * On entry, psqlscan.l has collected the command into "buf", so we don't
 * really need to do much here except check for comments and set up a Command
 * struct.
 */
Command *
create_sql_command(PQExpBuffer buf)
{
	Command    *my_command;
	char	   *p = skip_sql_comments(buf->data);

	if (p == NULL)
		return NULL;

	/* Allocate and initialize Command structure */
	my_command = pg_malloc0_object(Command);
	initPQExpBuffer(&my_command->lines);
	appendPQExpBufferStr(&my_command->lines, p);
	my_command->first_line = NULL;	/* this is set later */
	my_command->type = SQL_COMMAND;
	my_command->meta = META_NONE;
	my_command->argc = 0;
	my_command->retries = 0;
	my_command->failures = 0;
	memset(my_command->argv, 0, sizeof(my_command->argv));
	my_command->varprefix = NULL;	/* allocated later, if needed */
	my_command->expr = NULL;
	initSimpleStats(&my_command->stats);
	my_command->prepname = NULL;	/* set later, if needed */

	return my_command;
}

/* Free a Command structure and associated data */
void
free_command(Command *command)
{
	termPQExpBuffer(&command->lines);
	pg_free(command->first_line);
	for (int i = 0; i < command->argc; i++)
		pg_free(command->argv[i]);
	pg_free(command->varprefix);

	/*
	 * It should also free expr recursively, but this is currently not needed
	 * as only gset commands (which do not have an expression) are freed.
	 */
	pg_free(command);
}

/*
 * Once an SQL command is fully parsed, possibly by accumulating several
 * parts, complete other fields of the Command structure.
 */
void
postprocess_sql_command(Command *my_command)
{
	char		buffer[128];
	static int	prepnum = 0;

	Assert(my_command->type == SQL_COMMAND);

	/* Save the first line for error display. */
	strlcpy(buffer, my_command->lines.data, sizeof(buffer));
	buffer[strcspn(buffer, "\n\r")] = '\0';
	my_command->first_line = pg_strdup(buffer);

	/* Parse query and generate prepared statement name, if necessary */
	switch (querymode)
	{
		case QUERY_SIMPLE:
			my_command->argv[0] = my_command->lines.data;
			my_command->argc++;
			break;
		case QUERY_PREPARED:
			my_command->prepname = psprintf("P_%d", prepnum++);
			pg_fallthrough;
		case QUERY_EXTENDED:
			if (!parseQuery(my_command))
				exit(1);
			break;
		default:
			exit(1);
	}
}

/*
 * Parse a backslash command; return a Command struct, or NULL if comment
 *
 * At call, we have scanned only the initial backslash.
 */
Command *
process_backslash_command(PsqlScanState sstate, const char *source,
						  int lineno, int start_offset)
{
	Command    *my_command;
	PQExpBufferData word_buf;
	int			word_offset;
	int			offsets[MAX_ARGS];	/* offsets of argument words */
	int			j;

	initPQExpBuffer(&word_buf);

	/* Collect first word of command */
	if (!expr_lex_one_word(sstate, &word_buf, &word_offset))
	{
		termPQExpBuffer(&word_buf);
		return NULL;
	}

	/* Allocate and initialize Command structure */
	my_command = pg_malloc0_object(Command);
	my_command->type = META_COMMAND;
	my_command->argc = 0;
	initSimpleStats(&my_command->stats);

	/* Save first word (command name) */
	j = 0;
	offsets[j] = word_offset;
	my_command->argv[j++] = pg_strdup(word_buf.data);
	my_command->argc++;

	/* ... and convert it to enum form */
	my_command->meta = getMetaCommand(my_command->argv[0]);

	if (my_command->meta == META_SET ||
		my_command->meta == META_IF ||
		my_command->meta == META_ELIF)
	{
		yyscan_t	yyscanner;

		/* For \set, collect var name */
		if (my_command->meta == META_SET)
		{
			if (!expr_lex_one_word(sstate, &word_buf, &word_offset))
				syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
							 "missing argument", NULL, -1);

			offsets[j] = word_offset;
			my_command->argv[j++] = pg_strdup(word_buf.data);
			my_command->argc++;
		}

		/* then for all parse the expression */
		yyscanner = expr_scanner_init(sstate, source, lineno, start_offset,
									  my_command->argv[0]);

		if (expr_yyparse(&my_command->expr, yyscanner) != 0)
		{
			/* dead code: exit done from syntax_error called by yyerror */
			exit(1);
		}

		/* Save line, trimming any trailing newline */
		my_command->first_line =
			expr_scanner_get_substring(sstate,
									   start_offset,
									   true);

		expr_scanner_finish(yyscanner);

		termPQExpBuffer(&word_buf);

		return my_command;
	}

	/* For all other commands, collect remaining words. */
	while (expr_lex_one_word(sstate, &word_buf, &word_offset))
	{
		/*
		 * my_command->argv[0] is the command itself, so the max number of
		 * arguments is one less than MAX_ARGS
		 */
		if (j >= MAX_ARGS)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL, -1);

		offsets[j] = word_offset;
		my_command->argv[j++] = pg_strdup(word_buf.data);
		my_command->argc++;
	}

	/* Save line, trimming any trailing newline */
	my_command->first_line =
		expr_scanner_get_substring(sstate,
								   start_offset,
								   true);

	if (my_command->meta == META_SLEEP)
	{
		if (my_command->argc < 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing argument", NULL, -1);

		if (my_command->argc > 3)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL,
						 offsets[3] - start_offset);

		/*
		 * Split argument into number and unit to allow "sleep 1ms" etc. We
		 * don't have to terminate the number argument with null because it
		 * will be parsed with atoi, which ignores trailing non-digit
		 * characters.
		 */
		if (my_command->argv[1][0] != ':')
		{
			char	   *c = my_command->argv[1];
			bool		have_digit = false;

			/* Skip sign */
			if (*c == '+' || *c == '-')
				c++;

			/* Require at least one digit */
			if (*c && isdigit((unsigned char) *c))
				have_digit = true;

			/* Eat all digits */
			while (*c && isdigit((unsigned char) *c))
				c++;

			if (*c)
			{
				if (my_command->argc == 2 && have_digit)
				{
					my_command->argv[2] = c;
					offsets[2] = offsets[1] + (c - my_command->argv[1]);
					my_command->argc = 3;
				}
				else
				{
					/*
					 * Raise an error if argument starts with non-digit
					 * character (after sign).
					 */
					syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
								 "invalid sleep time, must be an integer",
								 my_command->argv[1], offsets[1] - start_offset);
				}
			}
		}

		if (my_command->argc == 3)
		{
			if (pg_strcasecmp(my_command->argv[2], "us") != 0 &&
				pg_strcasecmp(my_command->argv[2], "ms") != 0 &&
				pg_strcasecmp(my_command->argv[2], "s") != 0)
				syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
							 "unrecognized time unit, must be us, ms or s",
							 my_command->argv[2], offsets[2] - start_offset);
		}
	}
	else if (my_command->meta == META_SETSHELL)
	{
		if (my_command->argc < 3)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing argument", NULL, -1);
	}
	else if (my_command->meta == META_SHELL)
	{
		if (my_command->argc < 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing command", NULL, -1);
	}
	else if (my_command->meta == META_ELSE || my_command->meta == META_ENDIF ||
			 my_command->meta == META_STARTPIPELINE ||
			 my_command->meta == META_ENDPIPELINE ||
			 my_command->meta == META_SYNCPIPELINE)
	{
		if (my_command->argc != 1)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "unexpected argument", NULL, -1);
	}
	else if (my_command->meta == META_GSET || my_command->meta == META_ASET)
	{
		if (my_command->argc > 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL, -1);
	}
	else
	{
		/* my_command->meta == META_NONE */
		syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
					 "invalid command", NULL, -1);
	}

	termPQExpBuffer(&word_buf);

	return my_command;
}

pg_noreturn void
ConditionError(const char *desc, int cmdn, const char *msg)
{
	pg_fatal("condition error in script \"%s\" command %d: %s",
			 desc, cmdn, msg);
}

/*
 * Partial evaluation of conditionals before recording and running the script.
 */
void
CheckConditional(const ParsedScript *ps)
{
	/* statically check conditional structure */
	ConditionalStack cs = conditional_stack_create();
	int			i;

	for (i = 0; ps->commands[i] != NULL; i++)
	{
		Command    *cmd = ps->commands[i];

		if (cmd->type == META_COMMAND)
		{
			switch (cmd->meta)
			{
				case META_IF:
					conditional_stack_push(cs, IFSTATE_FALSE);
					break;
				case META_ELIF:
					if (conditional_stack_empty(cs))
						ConditionError(ps->desc, i + 1, "\\elif without matching \\if");
					if (conditional_stack_peek(cs) == IFSTATE_ELSE_FALSE)
						ConditionError(ps->desc, i + 1, "\\elif after \\else");
					break;
				case META_ELSE:
					if (conditional_stack_empty(cs))
						ConditionError(ps->desc, i + 1, "\\else without matching \\if");
					if (conditional_stack_peek(cs) == IFSTATE_ELSE_FALSE)
						ConditionError(ps->desc, i + 1, "\\else after \\else");
					conditional_stack_poke(cs, IFSTATE_ELSE_FALSE);
					break;
				case META_ENDIF:
					if (!conditional_stack_pop(cs))
						ConditionError(ps->desc, i + 1, "\\endif without matching \\if");
					break;
				default:
					/* ignore anything else... */
					break;
			}
		}
	}
	if (!conditional_stack_empty(cs))
		ConditionError(ps->desc, i + 1, "\\if without matching \\endif");
	conditional_stack_destroy(cs);
}

/*
 * Parse a script (either the contents of a file, or a built-in script)
 * and add it to the list of scripts.
 */
void
ParseScript(const char *script, const char *desc, int weight)
{
	ParsedScript ps;
	PsqlScanState sstate;
	PQExpBufferData line_buf;
	int			alloc_num;
	int			index;

	alloc_num = COMMANDS_ALLOC_NUM;

	/* Initialize all fields of ps */
	ps.desc = desc;
	ps.weight = weight;
	ps.commands = pg_malloc_array(Command *, alloc_num);
	initStats(&ps.stats, 0);

	/* Prepare to parse script */
	sstate = psql_scan_create(&pgbench_callbacks);

	/*
	 * Ideally, we'd scan scripts using the encoding and stdstrings settings
	 * we get from a DB connection.  However, without major rearrangement of
	 * pgbench's argument parsing, we can't have a DB connection at the time
	 * we parse scripts.  Using SQL_ASCII (encoding 0) should work well enough
	 * with any backend-safe encoding, though conceivably we could be fooled
	 * if a script file uses a client-only encoding.  We also assume that
	 * stdstrings should be true, which is a bit riskier.
	 */
	psql_scan_setup(sstate, script, strlen(script), 0, true);

	initPQExpBuffer(&line_buf);

	index = 0;

	for (;;)
	{
		PsqlScanResult sr;
		promptStatus_t prompt;
		Command    *command = NULL;

		resetPQExpBuffer(&line_buf);

		sr = psql_scan(sstate, &line_buf, &prompt);

		/* If we collected a new SQL command, process that */
		command = create_sql_command(&line_buf);

		/* store new command */
		if (command)
			ps.commands[index++] = command;

		/* If we reached a backslash, process that */
		if (sr == PSCAN_BACKSLASH)
		{
			int			lineno;
			int			start_offset;

			/* Capture location of the backslash */
			psql_scan_get_location(sstate, &lineno, &start_offset);
			start_offset--;

			command = process_backslash_command(sstate, desc,
												lineno, start_offset);

			if (command)
			{
				/*
				 * If this is gset or aset, merge into the preceding command.
				 * (We don't use a command slot in this case).
				 */
				if (command->meta == META_GSET || command->meta == META_ASET)
				{
					Command    *cmd;

					if (index == 0)
						syntax_error(desc, lineno, NULL, NULL,
									 "\\gset must follow an SQL command",
									 NULL, -1);

					cmd = ps.commands[index - 1];

					if (cmd->type != SQL_COMMAND ||
						cmd->varprefix != NULL)
						syntax_error(desc, lineno, NULL, NULL,
									 "\\gset must follow an SQL command",
									 cmd->first_line, -1);

					/* get variable prefix */
					if (command->argc <= 1 || command->argv[1][0] == '\0')
						cmd->varprefix = pg_strdup("");
					else
						cmd->varprefix = pg_strdup(command->argv[1]);

					/* update the sql command meta */
					cmd->meta = command->meta;

					/* cleanup unused command */
					free_command(command);

					continue;
				}

				/* Attach any other backslash command as a new command */
				ps.commands[index++] = command;
			}
		}

		/*
		 * Since we used a command slot, allocate more if needed.  Note we
		 * always allocate one more in order to accommodate the NULL
		 * terminator below.
		 */
		if (index >= alloc_num)
		{
			alloc_num += COMMANDS_ALLOC_NUM;
			ps.commands = (Command **)
				pg_realloc_array(ps.commands, Command *, alloc_num);
		}

		/* Done if we reached EOF */
		if (sr == PSCAN_INCOMPLETE || sr == PSCAN_EOL)
			break;
	}

	ps.commands[index] = NULL;

	addScript(&ps);

	termPQExpBuffer(&line_buf);
	psql_scan_finish(sstate);
	psql_scan_destroy(sstate);
}

/*
 * Read the entire contents of file fd, and return it in a malloc'd buffer.
 *
 * The buffer will typically be larger than necessary, but we don't care
 * in this program, because we'll free it as soon as we've parsed the script.
 */
char *
read_file_contents(FILE *fd)
{
	char	   *buf;
	size_t		buflen = BUFSIZ;
	size_t		used = 0;

	buf = (char *) pg_malloc(buflen);

	for (;;)
	{
		size_t		nread;

		nread = fread(buf + used, 1, BUFSIZ, fd);
		used += nread;
		/* If fread() read less than requested, must be EOF or error */
		if (nread < BUFSIZ)
			break;
		/* Enlarge buf so we can read some more */
		buflen += BUFSIZ;
		buf = (char *) pg_realloc(buf, buflen);
	}
	/* There is surely room for a terminator */
	buf[used] = '\0';

	return buf;
}

/*
 * Given a file name, read it and add its script to the list.
 * "-" means to read stdin.
 * NB: filename must be storage that won't disappear.
 */
void
process_file(const char *filename, int weight)
{
	FILE	   *fd;
	char	   *buf;

	/* Slurp the file contents into "buf" */
	if (strcmp(filename, "-") == 0)
		fd = stdin;
	else if ((fd = fopen(filename, "r")) == NULL)
		pg_fatal("could not open file \"%s\": %m", filename);

	buf = read_file_contents(fd);

	if (ferror(fd))
		pg_fatal("could not read file \"%s\": %m", filename);

	if (fd != stdin)
		fclose(fd);

	ParseScript(buf, filename, weight);

	free(buf);
}

/* Parse the given builtin script and add it to the list. */
void
process_builtin(const BuiltinScript *bi, int weight)
{
	ParseScript(bi->script, bi->desc, weight);
}

/* show available builtin scripts */
void
listAvailableScripts(void)
{
	fprintf(stderr, "Available builtin scripts:\n");
	for (size_t i = 0; i < lengthof(builtin_script); i++)
		fprintf(stderr, "  %13s: %s\n", builtin_script[i].name, builtin_script[i].desc);
	fprintf(stderr, "\n");
}

/* return builtin script "name" if unambiguous, fails if not found */
const BuiltinScript *
findBuiltin(const char *name)
{
	int			found = 0,
				len = strlen(name);
	const BuiltinScript *result = NULL;

	for (size_t i = 0; i < lengthof(builtin_script); i++)
	{
		if (strncmp(builtin_script[i].name, name, len) == 0)
		{
			result = &builtin_script[i];
			found++;
		}
	}

	/* ok, unambiguous result */
	if (found == 1)
		return result;

	/* error cases */
	if (found == 0)
		pg_log_error("no builtin script found for name \"%s\"", name);
	else						/* found > 1 */
		pg_log_error("ambiguous builtin name: %d builtin scripts found for prefix \"%s\"", found, name);

	listAvailableScripts();
	exit(1);
}

/*
 * Determine the weight specification from a script option (-b, -f), if any,
 * and return it as an integer (1 is returned if there's no weight).  The
 * script name is returned in *script as a malloc'd string.
 */
int
parseScriptWeight(const char *option, char **script)
{
	const char *sep;
	int			weight;

	if ((sep = strrchr(option, WSEP)))
	{
		int			namelen = sep - option;
		long		wtmp;
		char	   *badp;

		/* generate the script name */
		*script = pg_malloc(namelen + 1);
		strncpy(*script, option, namelen);
		(*script)[namelen] = '\0';

		/* process digits of the weight spec */
		errno = 0;
		wtmp = strtol(sep + 1, &badp, 10);
		if (errno != 0 || badp == sep + 1 || *badp != '\0')
			pg_fatal("invalid weight specification: %s", sep);
		if (wtmp > INT_MAX || wtmp < 0)
			pg_fatal("weight specification out of range (0 .. %d): %ld",
					 INT_MAX, wtmp);
		weight = wtmp;
	}
	else
	{
		*script = pg_strdup(option);
		weight = 1;
	}

	return weight;
}

/* append a script to the list of scripts to process */
void
addScript(const ParsedScript *script)
{
	if (script->commands == NULL || script->commands[0] == NULL)
		pg_fatal("empty command list for script \"%s\"", script->desc);

	if (num_scripts >= MAX_SCRIPTS)
		pg_fatal("at most %d SQL scripts are allowed", MAX_SCRIPTS);

	CheckConditional(script);

	sql_script[num_scripts] = *script;
	num_scripts++;
}

/*
 * Choose a script index to execute based on script weights.
 */
int
chooseScript(pg_prng_state *random_state)
{
	int			i = 0;
	int64		w;

	if (num_scripts == 1)
		return 0;

	w = getrand(random_state, 0, total_weight - 1);
	do
	{
		w -= sql_script[i++].weight;
	} while (w >= 0);

	return i - 1;
}
