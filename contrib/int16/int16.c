/*-------------------------------------------------------------------------
 *
 * int16.c
 *	  128-bit signed integer operations for the "int16" data type.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * contrib/int16/int16.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "int16.h"

PG_MODULE_MAGIC;

/* I/O */
PG_FUNCTION_INFO_V1(int16in);
PG_FUNCTION_INFO_V1(int16out);
PG_FUNCTION_INFO_V1(int16recv);
PG_FUNCTION_INFO_V1(int16send);

/* Arithmetic */
PG_FUNCTION_INFO_V1(int16pl);
PG_FUNCTION_INFO_V1(int16mi);
PG_FUNCTION_INFO_V1(int16mul);
PG_FUNCTION_INFO_V1(int16div);
PG_FUNCTION_INFO_V1(int16mod);
PG_FUNCTION_INFO_V1(int16um);
PG_FUNCTION_INFO_V1(int16up);
PG_FUNCTION_INFO_V1(int16abs);

/* Comparison */
PG_FUNCTION_INFO_V1(int16eq);
PG_FUNCTION_INFO_V1(int16ne);
PG_FUNCTION_INFO_V1(int16lt);
PG_FUNCTION_INFO_V1(int16gt);
PG_FUNCTION_INFO_V1(int16le);
PG_FUNCTION_INFO_V1(int16ge);
PG_FUNCTION_INFO_V1(int16_cmp);

/* Bitwise */
PG_FUNCTION_INFO_V1(int16and);
PG_FUNCTION_INFO_V1(int16or);
PG_FUNCTION_INFO_V1(int16xor);
PG_FUNCTION_INFO_V1(int16not);
PG_FUNCTION_INFO_V1(int16shl);
PG_FUNCTION_INFO_V1(int16shr);

/* Conversions */
PG_FUNCTION_INFO_V1(int16_from_int8);
PG_FUNCTION_INFO_V1(int16_to_int8);
PG_FUNCTION_INFO_V1(int16_from_int4);
PG_FUNCTION_INFO_V1(int16_to_int4);
PG_FUNCTION_INFO_V1(int16_from_int2);
PG_FUNCTION_INFO_V1(int16_to_int2);

/* Cross-type arithmetic: int16 op int8 */
PG_FUNCTION_INFO_V1(int168pl);
PG_FUNCTION_INFO_V1(int168mi);
PG_FUNCTION_INFO_V1(int168mul);
PG_FUNCTION_INFO_V1(int168div);
PG_FUNCTION_INFO_V1(int816pl);
PG_FUNCTION_INFO_V1(int816mi);
PG_FUNCTION_INFO_V1(int816mul);
PG_FUNCTION_INFO_V1(int816div);

/* Cross-type arithmetic: int16 op int4 */
PG_FUNCTION_INFO_V1(int164pl);
PG_FUNCTION_INFO_V1(int164mi);
PG_FUNCTION_INFO_V1(int164mul);
PG_FUNCTION_INFO_V1(int164div);
PG_FUNCTION_INFO_V1(int416pl);
PG_FUNCTION_INFO_V1(int416mi);
PG_FUNCTION_INFO_V1(int416mul);
PG_FUNCTION_INFO_V1(int416div);

/* Cross-type arithmetic: int16 op int2 */
PG_FUNCTION_INFO_V1(int162pl);
PG_FUNCTION_INFO_V1(int162mi);
PG_FUNCTION_INFO_V1(int162mul);
PG_FUNCTION_INFO_V1(int162div);
PG_FUNCTION_INFO_V1(int216pl);
PG_FUNCTION_INFO_V1(int216mi);
PG_FUNCTION_INFO_V1(int216mul);
PG_FUNCTION_INFO_V1(int216div);

/* Cross-type comparison: int16 vs int8 */
PG_FUNCTION_INFO_V1(int168eq);
PG_FUNCTION_INFO_V1(int168ne);
PG_FUNCTION_INFO_V1(int168lt);
PG_FUNCTION_INFO_V1(int168gt);
PG_FUNCTION_INFO_V1(int168le);
PG_FUNCTION_INFO_V1(int168ge);
PG_FUNCTION_INFO_V1(int168cmp);
PG_FUNCTION_INFO_V1(int816eq);
PG_FUNCTION_INFO_V1(int816ne);
PG_FUNCTION_INFO_V1(int816lt);
PG_FUNCTION_INFO_V1(int816gt);
PG_FUNCTION_INFO_V1(int816le);
PG_FUNCTION_INFO_V1(int816ge);
PG_FUNCTION_INFO_V1(int816cmp);

/* Cross-type comparison: int16 vs int4 */
PG_FUNCTION_INFO_V1(int164eq);
PG_FUNCTION_INFO_V1(int164ne);
PG_FUNCTION_INFO_V1(int164lt);
PG_FUNCTION_INFO_V1(int164gt);
PG_FUNCTION_INFO_V1(int164le);
PG_FUNCTION_INFO_V1(int164ge);
PG_FUNCTION_INFO_V1(int164cmp);
PG_FUNCTION_INFO_V1(int416eq);
PG_FUNCTION_INFO_V1(int416ne);
PG_FUNCTION_INFO_V1(int416lt);
PG_FUNCTION_INFO_V1(int416gt);
PG_FUNCTION_INFO_V1(int416le);
PG_FUNCTION_INFO_V1(int416ge);
PG_FUNCTION_INFO_V1(int416cmp);

/* Cross-type comparison: int16 vs int2 */
PG_FUNCTION_INFO_V1(int162eq);
PG_FUNCTION_INFO_V1(int162ne);
PG_FUNCTION_INFO_V1(int162lt);
PG_FUNCTION_INFO_V1(int162gt);
PG_FUNCTION_INFO_V1(int162le);
PG_FUNCTION_INFO_V1(int162ge);
PG_FUNCTION_INFO_V1(int162cmp);
PG_FUNCTION_INFO_V1(int216eq);
PG_FUNCTION_INFO_V1(int216ne);
PG_FUNCTION_INFO_V1(int216lt);
PG_FUNCTION_INFO_V1(int216gt);
PG_FUNCTION_INFO_V1(int216le);
PG_FUNCTION_INFO_V1(int216ge);
PG_FUNCTION_INFO_V1(int216cmp);

/* Hash */
PG_FUNCTION_INFO_V1(int16_hash);

/* Aggregates */
PG_FUNCTION_INFO_V1(int16larger);
PG_FUNCTION_INFO_V1(int16smaller);
PG_FUNCTION_INFO_V1(int16_avg_state_in);
PG_FUNCTION_INFO_V1(int16_avg_state_out);
PG_FUNCTION_INFO_V1(int16_avg_accum);
PG_FUNCTION_INFO_V1(int16_avg_accum_combine);
PG_FUNCTION_INFO_V1(int16_avg);

/*
 * Internal helpers
 */

/* Add two INT128 values with overflow check; returns true on success */
bool
int128_add_overflow(INT128 a, INT128 b, INT128 *result)
{
#if USE_NATIVE_INT128
	return __builtin_add_overflow(a, b, result);
#else
	INT128		r = make_int128(0, 0);

	int128_add_int128(&r, a);
	int128_add_int128(&r, b);

	/*
	 * Overflow check: if a and b have the same sign and the result has a
	 * different sign, overflow occurred.
	 */
	if ((a.hi < 0) == (b.hi < 0))
	{
		if ((a.hi < 0) != (r.hi < 0))
			return true;
	}
	*result = r;
	return false;
#endif
}

/* Subtract INT128 v from INT128 variable *i128 */
static inline void
int128_sub_int128(INT128 *i128, INT128 v)
{
#if USE_NATIVE_INT128
	*i128 -= v;
#else
	int128_sub_uint64(i128, v.lo);
	i128->hi -= v.hi;
#endif
}

/* Negate an INT128 value with overflow check (INT128_MIN overflows) */
bool
int128_negate_overflow(INT128 a, INT128 *result)
{
#if USE_NATIVE_INT128
	return __builtin_sub_overflow(0, a, result);
#else
	/* INT128_MIN is the only value that overflows on negation */
	if (a.hi == INT64CONST(0x8000000000000000) && a.lo == 0)
		return true;
	*result = make_int128(0, 0);
	int128_sub_int128(result, a);
	return false;
#endif
}

/* Subtract b from a with overflow check */
bool
int128_sub_overflow(INT128 a, INT128 b, INT128 *result)
{
#if USE_NATIVE_INT128
	return __builtin_sub_overflow(a, b, result);
#else
	INT128		r = a;

	int128_sub_int128(&r, b);

	/*
	 * Overflow: if a and b have different signs, and the result's sign
	 * differs from a's sign, then overflow occurred.
	 */
	if ((a.hi < 0) != (b.hi < 0))
	{
		if ((a.hi < 0) != (r.hi < 0))
			return true;
	}
	*result = r;
	return false;
#endif
}

/* Multiply two INT128 values with overflow check */
bool
int128_mul_overflow(INT128 a, INT128 b, INT128 *result)
{
#if USE_NATIVE_INT128
	return __builtin_mul_overflow(a, b, result);
#else
	/*
	 * Schoolbook multiply using absolute values, with overflow detection. a =
	 * a_hi*2^64 + a_lo, b = b_hi*2^64 + b_lo (unsigned). a*b =
	 * (a_hi*b_hi)*2^128 + (a_hi*b_lo + a_lo*b_hi)*2^64 + a_lo*b_lo For the
	 * result to fit in 128 bits, a_hi*b_hi must be zero, and the cross terms
	 * must fit when added to the upper 64 bits.
	 */
	bool		a_neg = (a.hi < 0);
	bool		b_neg = (b.hi < 0);
	bool		result_neg = (a_neg != b_neg);
	uint64		a_hi,
				a_lo,
				b_hi,
				b_lo;
	uint64		prod_hi,
				prod_lo;
	uint64		cross,
				old;

	/* Get absolute values */
	a_lo = a.lo;
	a_hi = (uint64) a.hi;
	if (a_neg)
	{
		a_hi = ~a_hi;
		a_lo = ~a_lo + 1;
		if (a_lo == 0)
			a_hi++;
	}
	b_lo = b.lo;
	b_hi = (uint64) b.hi;
	if (b_neg)
	{
		b_hi = ~b_hi;
		b_lo = ~b_lo + 1;
		if (b_lo == 0)
			b_hi++;
	}

	/* If both high parts are nonzero, product exceeds 128 bits */
	if (a_hi != 0 && b_hi != 0)
		return true;

	/* Compute a_lo * b_lo as 128-bit product (using 32-bit limbs) */
	{
		uint64		a0 = (uint32) a_lo;
		uint64		a1 = a_lo >> 32;
		uint64		b0 = (uint32) b_lo;
		uint64		b1 = b_lo >> 32;
		uint64		p0 = a0 * b0;
		uint64		p1 = a0 * b1;
		uint64		p2 = a1 * b0;
		uint64		p3 = a1 * b1;
		uint64		carry = (p0 >> 32) + (uint32) p1 + (uint32) p2;

		prod_lo = (p0 & 0xffffffff) | (carry << 32);
		prod_hi = p3 + (carry >> 32) + (p1 >> 32) + (p2 >> 32);
	}

	/* Add cross term a_hi * b_lo (shifted left 64, so adds to prod_hi) */
	if (a_hi != 0)
	{
		/* a_hi * b_lo must fit in 64 bits (it's added to prod_hi) */
		if (b_lo != 0 && a_hi > UINT64_MAX / b_lo)
			return true;
		cross = a_hi * b_lo;
		old = prod_hi;
		prod_hi += cross;
		if (prod_hi < old)
			return true;
	}

	/* Add cross term a_lo * b_hi (shifted left 64, so adds to prod_hi) */
	if (b_hi != 0)
	{
		if (a_lo != 0 && b_hi > UINT64_MAX / a_lo)
			return true;
		cross = b_hi * a_lo;
		old = prod_hi;
		prod_hi += cross;
		if (prod_hi < old)
			return true;
	}

	/*
	 * Check that the unsigned 128-bit product fits in a signed 128-bit value.
	 * For a positive result, prod_hi must not have bit 63 set (max is 2^127 -
	 * 1).  For a negative result, the absolute value must be at most 2^127
	 * (INT128_MIN), so prod_hi must be <= 0x8000000000000000, and if it
	 * equals that, prod_lo must be 0.
	 */
	if (result_neg)
	{
		if (prod_hi > UINT64CONST(0x8000000000000000) ||
			(prod_hi == UINT64CONST(0x8000000000000000) && prod_lo != 0))
			return true;
	}
	else
	{
		if (prod_hi > UINT64CONST(0x7fffffffffffffff))
			return true;
	}

	/* Assemble result */
	{
		INT128		rval = make_int128((int64) prod_hi, prod_lo);

		/* Negate if needed */
		if (result_neg)
		{
			if (int128_negate_overflow(rval, &rval))
				return true;
		}
		*result = rval;
		return false;
	}
#endif
}

/*
 * Divide INT128 a by INT128 b, producing quotient and remainder.
 * Returns true on overflow (division by zero, or INT128_MIN / -1).
 */
bool
int128_div_mod_int128(INT128 a, INT128 b, INT128 *quotient, INT128 *remainder)
{
#if USE_NATIVE_INT128
	if (b == 0)
		return true;
	/* INT128_MIN / -1 overflows */
	if (a == INT16_MIN_VAL && b == -1)
		return true;
	*quotient = a / b;
	*remainder = a % b;
	return false;
#else
	/*
	 * Manual shift-and-subtract division for the non-native case. We work
	 * with absolute values and fix the sign at the end.
	 */
	bool		a_neg = (a.hi < 0);
	bool		b_neg = (b.hi < 0);
	bool		result_neg = (a_neg != b_neg);
	uint64		ua_hi,
				ua_lo,
				ub_hi,
				ub_lo;
	uint64		q_hi = 0,
				q_lo = 0;
	uint64		r_hi = 0,
				r_lo = 0;
	int			bit;

	if (b.hi == 0 && b.lo == 0)
		return true;

	/* Check for INT128_MIN / -1: a == INT128_MIN, b == -1 */
	if (a.hi == INT64CONST(0x8000000000000000) && a.lo == 0 &&
		b.hi == -1 && b.lo == UINT64CONST(0xffffffffffffffff))
		return true;

	/* Compute absolute values of a */
	ua_hi = (uint64) a.hi;
	ua_lo = a.lo;
	if (a_neg)
	{
		ua_hi = ~ua_hi;
		ua_lo = ~ua_lo + 1;
		if (ua_lo == 0)
			ua_hi++;
	}

	/* Compute absolute values of b */
	ub_hi = (uint64) b.hi;
	ub_lo = b.lo;
	if (b_neg)
	{
		ub_hi = ~ub_hi;
		ub_lo = ~ub_lo + 1;
		if (ub_lo == 0)
			ub_hi++;
	}

	/* Shift-and-subtract: process 128 bits from most significant to least */
	for (bit = 127; bit >= 0; bit--)
	{
		uint64		new_r_hi = (r_hi << 1) | (r_lo >> 63);
		uint64		new_r_lo = (r_lo << 1);

		/* Extract bit 'bit' from the dividend */
		if (bit >= 64)
		{
			if (ua_hi & (UINT64CONST(1) << (bit - 64)))
				new_r_lo |= 1;
		}
		else
		{
			if (ua_lo & (UINT64CONST(1) << bit))
				new_r_lo |= 1;
		}

		r_hi = new_r_hi;
		r_lo = new_r_lo;

		/* If r >= ub, subtract ub and set quotient bit */
		if (r_hi > ub_hi ||
			(r_hi == ub_hi && r_lo >= ub_lo))
		{
			uint64		old_r_lo = r_lo;

			r_lo -= ub_lo;
			r_hi -= ub_hi;
			if (r_lo > old_r_lo)
				r_hi--;

			if (bit >= 64)
				q_hi |= (UINT64CONST(1) << (bit - 64));
			else
				q_lo |= (UINT64CONST(1) << bit);
		}
	}

	/* Apply sign to quotient */
	{
		INT128		q_val = make_int128((int64) q_hi, q_lo);
		INT128		r_val = make_int128((int64) r_hi, r_lo);

		if (result_neg)
			int128_negate_overflow(q_val, &q_val);

		/* Remainder has the same sign as the dividend */
		if (a_neg)
			int128_negate_overflow(r_val, &r_val);

		*quotient = q_val;
		*remainder = r_val;
	}
	return false;
#endif
}

/* Widen int32 to INT128 */
static inline INT128
int32_to_int128(int32 v)
{
	return int64_to_int128((int64) v);
}

/* Widen int16 to INT128 */
static inline INT128
int16_to_int128(int16 v)
{
	return int64_to_int128((int64) v);
}

/* Convert INT128 to int64 with overflow check */
static bool
int128_to_int64_overflow(INT128 val, int64 *result)
{
#if USE_NATIVE_INT128
	if (val < INT64_MIN || val > INT64_MAX)
		return true;
	*result = (int64) val;
	return false;
#else
	if (val.hi < 0)
	{
		/* Negative: check that high bits are all 1s, or hi == -1 and lo >= 0 */
		if (val.hi < -1)
			return true;
	}
	else
	{
		/* Non-negative: hi must be 0 */
		if (val.hi > 0)
			return true;
	}
	*result = (int64) val.lo;
	return false;
#endif
}

/* Convert INT128 to int32 with overflow check */
static bool
int128_to_int32_overflow(INT128 val, int32 *result)
{
	int64		v64;

	if (int128_to_int64_overflow(val, &v64))
		return true;
	if (v64 < INT32_MIN || v64 > INT32_MAX)
		return true;
	*result = (int32) v64;
	return false;
}

/* Convert INT128 to int16 with overflow check */
static bool
int128_to_int16_overflow(INT128 val, int16 *result)
{
	int64		v64;

	if (int128_to_int64_overflow(val, &v64))
		return true;
	if (v64 < INT16_MIN || v64 > INT16_MAX)
		return true;
	*result = (int16) v64;
	return false;
}


/*----------------------------------------------------------
 * I/O routines
 *---------------------------------------------------------
 */

/* int16in()
 * Parse a string to INT128.
 */
Datum
int16in(PG_FUNCTION_ARGS)
{
	char	   *num = PG_GETARG_CSTRING(0);
	INT128		result = make_int128(0, 0);
	bool		negative = false;
	const char *str;

	/* Skip leading whitespace */
	str = num;
	while (*str && isspace((unsigned char) *str))
		str++;

	if (*str == '-')
	{
		negative = true;
		str++;
	}
	else if (*str == '+')
		str++;

	if (*str == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"int16", num)));

	/* Parse digits */
	while (*str)
	{
		int			digit;

		if (!isdigit((unsigned char) *str))
			break;

		digit = (*str - '0');

		/* result = result * 10 + digit, with overflow check */
		{
			INT128		tmp = result;

			/* tmp = result * 10 = result * 8 + result * 2 */
			int128_add_int128(&tmp, result);	/* tmp = result * 2 */
			{
				INT128		eight = tmp;

				int128_add_int128(&eight, eight);	/* eight = result * 4 */
				int128_add_int128(&eight, eight);	/* eight = result * 8 */
				int128_add_int128(&tmp, eight); /* tmp = result * 10 */
			}
			int128_add_int64(&tmp, digit);

			/*
			 * Check for overflow: if tmp became negative, the value exceeded
			 * 2^127-1 (for positive) or 2^127 (for negative). For negative,
			 * 2^127 is the valid |INT128_MIN|, but anything larger overflows.
			 */
			if (int128_sign(tmp) < 0)
			{
				bool		is_min_abs;

#if USE_NATIVE_INT128
				is_min_abs = (tmp == ((INT128) 1 << 127));
#else
				is_min_abs = (PG_INT128_HI_INT64(tmp) == INT64CONST(0x8000000000000000) &&
							  PG_INT128_LO_UINT64(tmp) == 0);
#endif
				if (!negative || !is_min_abs)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value \"%s\" is out of range for type %s",
									num, "int16")));
			}
			else if (!negative && int128_compare(tmp, INT16_MAX_VAL) > 0)
			{
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("value \"%s\" is out of range for type %s",
								num, "int16")));
			}

			result = tmp;
		}

		str++;
	}

	/* Skip trailing whitespace */
	while (*str && isspace((unsigned char) *str))
		str++;

	if (*str != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"int16", num)));

	if (negative)
	{
		INT128		zero = make_int128(0, 0);

		int128_sub_int128(&zero, result);
		result = zero;
	}

	{
		int16StructP ret = palloc(INT16_LEN);

		memcpy(ret, &result, INT16_LEN);
		PG_RETURN_INT16_P(ret);
	}
}

/* int16out()
 * Convert INT128 to string.
 */
Datum
int16out(PG_FUNCTION_ARGS)
{
	int16StructP val = PG_GETARG_INT16_P(0);
	INT128		v;
	char		buf[42];		/* 39 digits + sign + null + safety */
	char	   *p;
	char	   *result;
	bool		negative = false;

	memcpy(&v, val, INT16_LEN);

	/* Handle zero specially */
	if (int128_is_zero(v))
	{
		result = palloc(2);
		result[0] = '0';
		result[1] = '\0';
		PG_RETURN_CSTRING(result);
	}

	/* Handle negative */
	if (int128_sign(v) < 0)
	{
		negative = true;
		if (int128_negate_overflow(v, &v))
		{
			/* INT128_MIN: use a constant string */
			result = pstrdup("-170141183460469231731687303715884105728");
			PG_RETURN_CSTRING(result);
		}
	}

	/* Convert to decimal digits (reverse order) */
	p = buf + sizeof(buf) - 1;
	*p = '\0';
	do
	{
		int32		rem;

		int128_div_mod_int32(&v, 10, &rem);
		*--p = '0' + rem;
	} while (!int128_is_zero(v));

	if (negative)
		*--p = '-';

	result = pstrdup(p);
	PG_RETURN_CSTRING(result);
}

/* int16recv() — binary input */
Datum
int16recv(PG_FUNCTION_ARGS)
{
	StringInfo	buffer = (StringInfo) PG_GETARG_POINTER(0);
	int16StructP result;

	result = (int16StructP) palloc(INT16_LEN);
	memcpy(result, pq_getmsgbytes(buffer, INT16_LEN), INT16_LEN);
	PG_RETURN_INT16_P(result);
}

/* int16send() — binary output */
Datum
int16send(PG_FUNCTION_ARGS)
{
	int16StructP val = PG_GETARG_INT16_P(0);
	StringInfoData buffer;

	pq_begintypsend(&buffer);
	pq_sendbytes(&buffer, (char *) val, INT16_LEN);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}


/*----------------------------------------------------------
 * Arithmetic operators (int16 vs int16)
 *---------------------------------------------------------
 */

Datum
int16pl(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_add_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16mi(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_sub_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16mul(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_mul_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16div(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				q,
				r;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_div_mod_int128(a, b, &q, &r))
	{
		if (int128_is_zero(b))
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &q, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16mod(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				q,
				r;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_div_mod_int128(a, b, &q, &r))
	{
		if (int128_is_zero(b))
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16um(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);
	INT128		a,
				r;

	memcpy(&a, arg, INT16_LEN);

	if (int128_negate_overflow(a, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16up(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);

	/* Just return a copy */
	int16StructP result = palloc(INT16_LEN);

	memcpy(result, arg, INT16_LEN);
	PG_RETURN_INT16_P(result);
}

Datum
int16abs(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);
	INT128		a,
				r;

	memcpy(&a, arg, INT16_LEN);

	if (int128_sign(a) < 0)
	{
		if (int128_negate_overflow(a, &r))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}
	else
		r = a;

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}


/*----------------------------------------------------------
 * Comparison operators (int16 vs int16)
 *---------------------------------------------------------
 */

Datum
int16eq(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) == 0);
}

Datum
int16ne(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) != 0);
}

Datum
int16lt(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) < 0);
}

Datum
int16gt(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) > 0);
}

Datum
int16le(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) <= 0);
}

Datum
int16ge(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) >= 0);
}

Datum
int16_cmp(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_INT32(int128_compare(a, b));
}


/*----------------------------------------------------------
 * Bitwise operators
 *---------------------------------------------------------
 */

Datum
int16and(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

#if USE_NATIVE_INT128
	r = a & b;
#else
	r.hi = a.hi & b.hi;
	r.lo = a.lo & b.lo;
#endif

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16or(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

#if USE_NATIVE_INT128
	r = a | b;
#else
	r.hi = a.hi | b.hi;
	r.lo = a.lo | b.lo;
#endif

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16xor(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

#if USE_NATIVE_INT128
	r = a ^ b;
#else
	r.hi = a.hi ^ b.hi;
	r.lo = a.lo ^ b.lo;
#endif

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16not(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);
	INT128		a,
				r;

	memcpy(&a, arg, INT16_LEN);

#if USE_NATIVE_INT128
	r = ~a;
#else
	r.hi = ~a.hi;
	r.lo = ~a.lo;
#endif

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16shl(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		shift = PG_GETARG_INT32(1);
	INT128		a,
				r;

	memcpy(&a, arg1, INT16_LEN);

#if USE_NATIVE_INT128
	/* avoid undefined behavior if shift is more than 127 */
	shift &= 127;
	r = a << shift;
#else

	/*
	 * Replicate C shift behavior: mask shift to 7 bits (0-127). This matches
	 * what most compilers do for native int128 shifts.
	 */
	shift &= 127;
	if (shift == 0)
		r = a;
	else if (shift < 64)
	{
		r.hi = (a.hi << shift) | (a.lo >> (64 - shift));
		r.lo = a.lo << shift;
	}
	else
	{
		r.hi = (int64) (a.lo << (shift - 64));
		r.lo = 0;
	}
#endif

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16shr(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		shift = PG_GETARG_INT32(1);
	INT128		a,
				r;

	memcpy(&a, arg1, INT16_LEN);

#if USE_NATIVE_INT128
	/* avoid undefined behavior if shift is more than 127 */
	shift &= 127;
	r = a >> shift;				/* arithmetic shift for signed type */
#else

	/*
	 * Replicate C shift behavior: mask shift to 7 bits (0-127). This matches
	 * what most compilers do for native int128 shifts.
	 */
	shift &= 127;
	if (shift == 0)
		r = a;
	else if (shift < 64)
	{
		r.lo = (a.lo >> shift) | ((uint64) a.hi << (64 - shift));
		r.hi = a.hi >> shift;	/* arithmetic shift */
	}
	else
	{
		r.lo = (uint64) a.hi >> (shift - 64);
		r.hi = (a.hi < 0) ? -1 : 0;
	}
#endif

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}


/*----------------------------------------------------------
 * Conversion functions (for casts)
 *---------------------------------------------------------
 */

Datum
int16_from_int8(PG_FUNCTION_ARGS)
{
	int64		val = PG_GETARG_INT64(0);
	INT128		result = int64_to_int128(val);
	int16StructP ret = palloc(INT16_LEN);

	memcpy(ret, &result, INT16_LEN);
	PG_RETURN_INT16_P(ret);
}

Datum
int16_to_int8(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);
	INT128		val;
	int64		result;

	memcpy(&val, arg, INT16_LEN);

	if (int128_to_int64_overflow(val, &result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT64(result);
}

Datum
int16_from_int4(PG_FUNCTION_ARGS)
{
	int32		val = PG_GETARG_INT32(0);
	INT128		result = int32_to_int128(val);
	int16StructP ret = palloc(INT16_LEN);

	memcpy(ret, &result, INT16_LEN);
	PG_RETURN_INT16_P(ret);
}

Datum
int16_to_int4(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);
	INT128		val;
	int32		result;

	memcpy(&val, arg, INT16_LEN);

	if (int128_to_int32_overflow(val, &result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32(result);
}

Datum
int16_from_int2(PG_FUNCTION_ARGS)
{
	int16		val = PG_GETARG_INT16(0);
	INT128		result = int16_to_int128(val);
	int16StructP ret = palloc(INT16_LEN);

	memcpy(ret, &result, INT16_LEN);
	PG_RETURN_INT16_P(ret);
}

Datum
int16_to_int2(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);
	INT128		val;
	int16		result;

	memcpy(&val, arg, INT16_LEN);

	if (int128_to_int16_overflow(val, &result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT16(result);
}


/*----------------------------------------------------------
 * Cross-type arithmetic: int16 op int8
 *---------------------------------------------------------
 */

Datum
int168pl(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	if (int128_add_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int168mi(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	if (int128_sub_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int168mul(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	if (int128_mul_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int168div(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b,
				q,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	if (int128_div_mod_int128(a, b, &q, &r))
	{
		if (int128_is_zero(b))
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &q, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

/* int8 op int16 */

Datum
int816pl(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_add_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int816mi(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_sub_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int816mul(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_mul_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int816div(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				q,
				r;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_div_mod_int128(a, b, &q, &r))
	{
		if (int128_is_zero(b))
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &q, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}


/*----------------------------------------------------------
 * Cross-type arithmetic: int16 op int4
 *---------------------------------------------------------
 */

Datum
int164pl(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	if (int128_add_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int164mi(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	if (int128_sub_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int164mul(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	if (int128_mul_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int164div(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b,
				q,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	if (int128_div_mod_int128(a, b, &q, &r))
	{
		if (int128_is_zero(b))
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &q, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

/* int4 op int16 */

Datum
int416pl(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_add_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int416mi(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_sub_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int416mul(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_mul_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int416div(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				q,
				r;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_div_mod_int128(a, b, &q, &r))
	{
		if (int128_is_zero(b))
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &q, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}


/*----------------------------------------------------------
 * Cross-type arithmetic: int16 op int2
 *---------------------------------------------------------
 */

Datum
int162pl(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	if (int128_add_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int162mi(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	if (int128_sub_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int162mul(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	if (int128_mul_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int162div(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b,
				q,
				r;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	if (int128_div_mod_int128(a, b, &q, &r))
	{
		if (int128_is_zero(b))
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &q, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

/* int2 op int16 */

Datum
int216pl(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_add_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int216mi(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_sub_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int216mul(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				r;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_mul_overflow(a, b, &r))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &r, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int216div(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				q,
				r;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_div_mod_int128(a, b, &q, &r))
	{
		if (int128_is_zero(b))
			ereport(ERROR,
					(errcode(ERRCODE_DIVISION_BY_ZERO),
					 errmsg("division by zero")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, &q, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}


/*----------------------------------------------------------
 * Cross-type comparison operators
 *---------------------------------------------------------
 *
 * Each pair (int16 vs int8/int4/int2) gets 6 operators + a cmp function.
 * We use macros to reduce boilerplate.
 *---------------------------------------------------------
 */

/* int16 vs int8 */

Datum
int168eq(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) == 0);
}

Datum
int168ne(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) != 0);
}

Datum
int168lt(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) < 0);
}

Datum
int168gt(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) > 0);
}

Datum
int168le(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) <= 0);
}

Datum
int168ge(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) >= 0);
}

Datum
int168cmp(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int64		arg2 = PG_GETARG_INT64(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int64_to_int128(arg2);

	PG_RETURN_INT32(int128_compare(a, b));
}

/* int8 vs int16 */

Datum
int816eq(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) == 0);
}

Datum
int816ne(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) != 0);
}

Datum
int816lt(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) < 0);
}

Datum
int816gt(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) > 0);
}

Datum
int816le(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) <= 0);
}

Datum
int816ge(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) >= 0);
}

Datum
int816cmp(PG_FUNCTION_ARGS)
{
	int64		arg1 = PG_GETARG_INT64(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int64_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_INT32(int128_compare(a, b));
}

/* int16 vs int4 */

Datum
int164eq(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) == 0);
}

Datum
int164ne(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) != 0);
}

Datum
int164lt(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) < 0);
}

Datum
int164gt(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) > 0);
}

Datum
int164le(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) <= 0);
}

Datum
int164ge(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) >= 0);
}

Datum
int164cmp(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int32		arg2 = PG_GETARG_INT32(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int32_to_int128(arg2);

	PG_RETURN_INT32(int128_compare(a, b));
}

/* int4 vs int16 */

Datum
int416eq(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) == 0);
}

Datum
int416ne(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) != 0);
}

Datum
int416lt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) < 0);
}

Datum
int416gt(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) > 0);
}

Datum
int416le(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) <= 0);
}

Datum
int416ge(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) >= 0);
}

Datum
int416cmp(PG_FUNCTION_ARGS)
{
	int32		arg1 = PG_GETARG_INT32(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int32_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_INT32(int128_compare(a, b));
}

/* int16 vs int2 */

Datum
int162eq(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) == 0);
}

Datum
int162ne(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) != 0);
}

Datum
int162lt(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) < 0);
}

Datum
int162gt(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) > 0);
}

Datum
int162le(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) <= 0);
}

Datum
int162ge(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	PG_RETURN_BOOL(int128_compare(a, b) >= 0);
}

Datum
int162cmp(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16		arg2 = PG_GETARG_INT16(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	b = int16_to_int128(arg2);

	PG_RETURN_INT32(int128_compare(a, b));
}

/* int2 vs int16 */

Datum
int216eq(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) == 0);
}

Datum
int216ne(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) != 0);
}

Datum
int216lt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) < 0);
}

Datum
int216gt(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) > 0);
}

Datum
int216le(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) <= 0);
}

Datum
int216ge(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_BOOL(int128_compare(a, b) >= 0);
}

Datum
int216cmp(PG_FUNCTION_ARGS)
{
	int16		arg1 = PG_GETARG_INT16(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	a = int16_to_int128(arg1);
	memcpy(&b, arg2, INT16_LEN);

	PG_RETURN_INT32(int128_compare(a, b));
}


/*----------------------------------------------------------
 * Hash function
 *---------------------------------------------------------
 */

Datum
int16_hash(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);

	return hash_any((unsigned char *) arg, INT16_LEN);
}


/*----------------------------------------------------------
 * Aggregate support functions
 *---------------------------------------------------------
 */

Datum
int16larger(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_compare(a, b) >= 0)
	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, arg1, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
	else
	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, arg2, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

Datum
int16smaller(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	if (int128_compare(a, b) <= 0)
	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, arg1, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
	else
	{
		int16StructP result = palloc(INT16_LEN);

		memcpy(result, arg2, INT16_LEN);
		PG_RETURN_INT16_P(result);
	}
}

/*
 * State type for int16 avg aggregate: INT128 sum + int64 count.
 * This is exposed as the SQL type "int16_avg_state" (24 bytes).
 */
typedef struct Int16AvgState
{
	INT128		sum;
	int64		count;
}			Int16AvgState;

/* Macros for the avg state type (pass-by-reference, like int16) */
#define PG_GETARG_AVGSTATE_P(n)	((Int16AvgState *) PG_GETARG_POINTER(n))
#define PG_RETURN_AVGSTATE_P(x)	PG_RETURN_POINTER(x)

Datum
int16_avg_state_in(PG_FUNCTION_ARGS)
{
	/* This type is only used internally; reject direct input */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type int16_avg_state")));

	PG_RETURN_NULL();			/* keep compiler quiet */
}

Datum
int16_avg_state_out(PG_FUNCTION_ARGS)
{
	/* This type is only used internally; output a placeholder */
	PG_RETURN_CSTRING(pstrdup("(internal)"));
}

Datum
int16_avg_accum(PG_FUNCTION_ARGS)
{
	Int16AvgState *state;

	if (PG_ARGISNULL(0))
		state = palloc0_object(Int16AvgState);
	else
		state = PG_GETARG_AVGSTATE_P(0);

	if (!PG_ARGISNULL(1))
	{
		int16StructP newval = PG_GETARG_INT16_P(1);
		INT128		v;

		memcpy(&v, newval, INT16_LEN);

		int128_add_int128(&state->sum, v);
		state->count++;
	}

	PG_RETURN_AVGSTATE_P(state);
}

Datum
int16_avg_accum_combine(PG_FUNCTION_ARGS)
{
	Int16AvgState *state1;
	Int16AvgState *state2;

	state1 = PG_ARGISNULL(0) ? NULL : PG_GETARG_AVGSTATE_P(0);
	state2 = PG_ARGISNULL(1) ? NULL : PG_GETARG_AVGSTATE_P(1);

	if (state2 == NULL)
	{
		if (state1 == NULL)
			state1 = palloc0_object(Int16AvgState);
		PG_RETURN_AVGSTATE_P(state1);
	}
	if (state1 == NULL)
	{
		/* Return a copy of state2 */
		Int16AvgState *result = palloc_object(Int16AvgState);

		memcpy(result, state2, sizeof(Int16AvgState));
		PG_RETURN_AVGSTATE_P(result);
	}

	/* Merge state2 into state1 */
	int128_add_int128(&state1->sum, state2->sum);
	state1->count += state2->count;

	PG_RETURN_AVGSTATE_P(state1);
}

/*
 * Convert INT128 to numeric Datum by going through string representation.
 */
Datum
int128_to_numeric(INT128 val)
{
	char		buf[42];
	char	   *p;
	bool		negative = false;

	if (int128_is_zero(val))
		return DirectFunctionCall3(numeric_in, CStringGetDatum("0"),
								   ObjectIdGetDatum(InvalidOid),
								   Int32GetDatum(-1));

	if (int128_sign(val) < 0)
	{
		negative = true;
		if (int128_negate_overflow(val, &val))
		{
			/* INT128_MIN */
			return DirectFunctionCall3(numeric_in,
									   CStringGetDatum("-170141183460469231731687303715884105728"),
									   ObjectIdGetDatum(InvalidOid),
									   Int32GetDatum(-1));
		}
	}

	p = buf + sizeof(buf) - 1;
	*p = '\0';
	do
	{
		int32		rem;

		int128_div_mod_int32(&val, 10, &rem);
		*--p = '0' + rem;
	} while (!int128_is_zero(val));

	if (negative)
		*--p = '-';

	return DirectFunctionCall3(numeric_in, CStringGetDatum(p),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

Datum
int16_avg(PG_FUNCTION_ARGS)
{
	Int16AvgState *state;

	state = PG_ARGISNULL(0) ? NULL : PG_GETARG_AVGSTATE_P(0);

	/* If there were no non-null inputs, return NULL */
	if (state == NULL || state->count == 0)
		PG_RETURN_NULL();

	{
		Datum		sumd = int128_to_numeric(state->sum);
		Datum		countd = DirectFunctionCall1(int8_numeric,
												 Int64GetDatum(state->count));

		return DirectFunctionCall2(numeric_div, sumd, countd);
	}
}
