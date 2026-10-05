/*-------------------------------------------------------------------------
 *
 * int16_funcs.c
 *	  Additional functions for the "int16" 128-bit signed integer data type.
 *
 * This file contains:
 *   - gcd, lcm (Euclidean algorithm, mirroring int8)
 *   - factorial (returns numeric, mirroring numeric_fac)
 *   - generate_series (set-returning function, mirroring int8)
 *   - in_range (window frame support, mirroring int8)
 *   - btree sort support and skip support functions
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * contrib/int16/int16_funcs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>

#include "fmgr.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/sortsupport.h"
#include "utils/skipsupport.h"
#include "int16.h"

PG_FUNCTION_INFO_V1(int16gcd);
PG_FUNCTION_INFO_V1(int16lcm);
PG_FUNCTION_INFO_V1(int16factorial);
PG_FUNCTION_INFO_V1(generate_series_int16);
PG_FUNCTION_INFO_V1(generate_series_step_int16);
PG_FUNCTION_INFO_V1(in_range_int16_int16);
PG_FUNCTION_INFO_V1(int16sortsupport);
PG_FUNCTION_INFO_V1(int16skipsupport);

/*----------------------------------------------------------
 * Local helpers
 *---------------------------------------------------------
 */

/*
 * Greatest Common Divisor (internal).
 *
 * Mirrors int8gcd_internal from int8.c, working in negative space to
 * handle INT128_MIN (whose absolute value cannot be represented).
 *
 * Special cases:
 *   gcd(x, 0) = gcd(0, x) = abs(x)
 *   gcd(0, 0) = 0
 *   gcd(INT128_MIN, 0), gcd(0, INT128_MIN), gcd(INT128_MIN, INT128_MIN)
 *       all error (abs(INT128_MIN) overflows).
 *   gcd(INT128_MIN, -1) = 1 (special-cased to avoid modulo exception).
 */
static INT128
int16gcd_internal(INT128 arg1, INT128 arg2)
{
	INT128		swap;
	INT128		a1,
				a2;
	INT128		zero = make_int128(0, 0);
	INT128		minus_one;
	INT128		q,
				r;

	/* Build -1 as INT128 */
	if (int128_negate_overflow(make_int128(0, 1), &minus_one))
	{
		/* Should not happen */
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("cannot negate 1")));
	}

	/*
	 * Put the greater absolute value in arg1.
	 *
	 * We do this in negative space in order to handle INT128_MIN. A value
	 * that is negative has a larger absolute value, so we compare: if a1 > a2
	 * (in negative space, "less negative" means "smaller absolute value"),
	 * then swap.
	 */
	if (int128_sign(arg1) < 0)
		a1 = arg1;
	else
	{
		if (int128_negate_overflow(arg1, &a1))
		{
			/* arg1 == INT128_MIN, abs is max, no swap needed */
			a1 = arg1;
		}
	}
	if (int128_sign(arg2) < 0)
		a2 = arg2;
	else
	{
		if (int128_negate_overflow(arg2, &a2))
		{
			/* arg2 == INT128_MIN */
			a2 = arg2;
		}
	}

	/*
	 * In negative space, a "greater" value means a smaller absolute value. So
	 * if a1 > a2, arg1 has a smaller absolute value than arg2, and we swap to
	 * put the larger absolute value in arg1.
	 */
	if (int128_compare(a1, a2) > 0)
	{
		swap = arg1;
		arg1 = arg2;
		arg2 = swap;
	}

	/* Special care for INT128_MIN */
	if (int128_compare(arg1, INT16_MIN_VAL) == 0)
	{
		if (int128_compare(arg2, zero) == 0 ||
			int128_compare(arg2, INT16_MIN_VAL) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));

		/*
		 * Some machines throw a floating-point exception for INT128_MIN % -1,
		 * which is a bit silly since the correct answer is perfectly
		 * well-defined, namely zero.  Guard against this and just return the
		 * result, gcd(INT128_MIN, -1) = 1.
		 */
		if (int128_compare(arg2, minus_one) == 0)
			return make_int128(0, 1);
	}

	/* Use the Euclidean algorithm to find the GCD */
	while (!int128_is_zero(arg2))
	{
		swap = arg2;
		/* arg2 = arg1 % arg2 */
		if (int128_div_mod_int128(arg1, arg2, &q, &r))
		{
			/* Only possible for INT128_MIN / -1, handled above */
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("unexpected overflow in gcd")));
		}
		arg2 = r;
		arg1 = swap;
	}

	/* Make sure the result is positive (we know it's not INT128_MIN) */
	if (int128_sign(arg1) < 0)
	{
		if (int128_negate_overflow(arg1, &arg1))
		{
			/* Should not happen given the checks above */
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
		}
	}

	return arg1;
}

/*----------------------------------------------------------
 * gcd
 *---------------------------------------------------------
 */
Datum
int16gcd(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				result;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	result = int16gcd_internal(a, b);

	{
		int16StructP r = palloc(INT16_LEN);

		memcpy(r, &result, INT16_LEN);
		PG_RETURN_INT16_P(r);
	}
}

/*----------------------------------------------------------
 * lcm
 *---------------------------------------------------------
 */
Datum
int16lcm(PG_FUNCTION_ARGS)
{
	int16StructP arg1 = PG_GETARG_INT16_P(0);
	int16StructP arg2 = PG_GETARG_INT16_P(1);
	INT128		a,
				b,
				gcd,
				q,
				r,
				result;

	memcpy(&a, arg1, INT16_LEN);
	memcpy(&b, arg2, INT16_LEN);

	/*
	 * Handle lcm(x, 0) = lcm(0, x) = 0 as a special case.  This prevents a
	 * division-by-zero error below when x is zero, and an overflow error from
	 * the GCD computation when x = INT128_MIN.
	 */
	if (int128_is_zero(a) || int128_is_zero(b))
	{
		int16StructP res = palloc(INT16_LEN);
		INT128		zero = make_int128(0, 0);

		memcpy(res, &zero, INT16_LEN);
		PG_RETURN_INT16_P(res);
	}

	/* lcm(x, y) = abs(x / gcd(x, y) * y) */
	gcd = int16gcd_internal(a, b);

	/* arg1 = arg1 / gcd */
	if (int128_div_mod_int128(a, gcd, &q, &r))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("unexpected overflow in lcm")));

	/* result = q * b */
	if (int128_mul_overflow(q, b, &result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	/* If the result is INT128_MIN, it cannot be represented as positive */
	if (int128_compare(result, INT16_MIN_VAL) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	if (int128_sign(result) < 0)
	{
		if (int128_negate_overflow(result, &result))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("integer out of range")));
	}

	{
		int16StructP res = palloc(INT16_LEN);

		memcpy(res, &result, INT16_LEN);
		PG_RETURN_INT16_P(res);
	}
}

/*----------------------------------------------------------
 * factorial
 *---------------------------------------------------------
 *
 * Mirrors numeric_fac() from numeric.c.  Takes int16, returns numeric.
 * The input is int16 but the practical limit is the same as for int8
 * (~32177) because numeric format itself overflows.
 */
Datum
int16factorial(PG_FUNCTION_ARGS)
{
	int16StructP arg = PG_GETARG_INT16_P(0);
	INT128		num128;
	int64		num;
	Datum		result;
	Datum		fact;

	memcpy(&num128, arg, INT16_LEN);

	/* Check if value is negative */
	if (int128_sign(num128) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("factorial of a negative number is undefined")));

	/* Check if value exceeds INT64_MAX (would definitely overflow numeric) */
	if (int128_compare(num128, int64_to_int128(INT64_MAX)) > 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value overflows numeric format")));

	/* Extract int64 value */
	num = int128_to_int64(num128);

	if (num <= 1)
		PG_RETURN_DATUM(DirectFunctionCall1(int8_numeric,
											Int64GetDatum(1)));

	/* Fail immediately if the result would overflow */
	if (num > 32177)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value overflows numeric format")));

	/*
	 * Compute factorial iteratively using numeric multiplication. Start with
	 * result = num, then multiply by num-1, num-2, ..., 2.
	 */
	result = DirectFunctionCall1(int8_numeric, Int64GetDatum(num));

	for (num = num - 1; num > 1; num--)
	{
		CHECK_FOR_INTERRUPTS();

		fact = DirectFunctionCall1(int8_numeric, Int64GetDatum(num));
		result = DirectFunctionCall2(numeric_mul, result, fact);
	}

	PG_RETURN_DATUM(result);
}

/*----------------------------------------------------------
 * generate_series
 *---------------------------------------------------------
 */

typedef struct
{
	INT128		current;
	INT128		finish;
	INT128		step;
}			int16_series_fctx;

Datum
generate_series_step_int16(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int16_series_fctx *fctx;
	MemoryContext oldcontext;
	INT128		result;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		int16StructP start_val = PG_GETARG_INT16_P(0);
		int16StructP finish_val = PG_GETARG_INT16_P(1);
		INT128		start;
		INT128		finish;
		INT128		step = int64_to_int128(1);

		memcpy(&start, start_val, INT16_LEN);
		memcpy(&finish, finish_val, INT16_LEN);

		/* see if we were given an explicit step size */
		if (PG_NARGS() == 3)
		{
			int16StructP step_val = PG_GETARG_INT16_P(2);

			memcpy(&step, step_val, INT16_LEN);
		}

		if (int128_is_zero(step))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* allocate memory for user context */
		fctx = palloc(sizeof(int16_series_fctx));

		/*
		 * Use fctx to keep state from call to call. Seed current with the
		 * original start value
		 */
		fctx->current = start;
		fctx->finish = finish;
		fctx->step = step;

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	/*
	 * get the saved state and use current as the result for this iteration
	 */
	fctx = funcctx->user_fctx;
	result = fctx->current;

	if ((int128_sign(fctx->step) > 0 && int128_compare(fctx->current, fctx->finish) <= 0) ||
		(int128_sign(fctx->step) < 0 && int128_compare(fctx->current, fctx->finish) >= 0))
	{
		/*
		 * Increment current in preparation for next iteration. If next-value
		 * computation overflows, this is the final result.
		 */
		if (int128_add_overflow(fctx->current, fctx->step, &fctx->current))
			fctx->step = make_int128(0, 0);

		/* do when there is more left to send */
		{
			int16StructP r = palloc(INT16_LEN);

			memcpy(r, &result, INT16_LEN);
			SRF_RETURN_NEXT(funcctx, PointerGetDatum(r));
		}
	}
	else
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}

Datum
generate_series_int16(PG_FUNCTION_ARGS)
{
	return generate_series_step_int16(fcinfo);
}

/*----------------------------------------------------------
 * in_range support for window functions
 *---------------------------------------------------------
 *
 * Mirrors in_range_int8_int8 from int8.c.
 * Signature: in_range(val int16, base int16, offset int16, sub bool, less bool)
 * Returns true if val is within [base, base+offset] (or [base-offset, base]).
 */
Datum
in_range_int16_int16(PG_FUNCTION_ARGS)
{
	int16StructP val_p = PG_GETARG_INT16_P(0);
	int16StructP base_p = PG_GETARG_INT16_P(1);
	int16StructP offset_p = PG_GETARG_INT16_P(2);
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	INT128		val,
				base,
				offset,
				sum;

	memcpy(&val, val_p, INT16_LEN);
	memcpy(&base, base_p, INT16_LEN);
	memcpy(&offset, offset_p, INT16_LEN);

	if (int128_sign(offset) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
	{
		/* Negate offset; cannot overflow since offset >= 0 */
		if (int128_negate_overflow(offset, &offset))
		{
			/*
			 * offset was 0... but negating 0 can't overflow. This shouldn't
			 * happen. But if offset == INT128_MIN somehow (it can't since we
			 * checked offset < 0 above and INT128_MIN < 0), handle it.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
					 errmsg("invalid preceding or following size in window function")));
		}
	}

	if (int128_add_overflow(base, offset, &sum))
	{
		/*
		 * If sub is false, the true sum is surely more than val, so correct
		 * answer is the same as "less".  If sub is true, the true sum is
		 * surely less than val, so the answer is "!less".
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(int128_compare(val, sum) <= 0);
	else
		PG_RETURN_BOOL(int128_compare(val, sum) >= 0);
}

/*----------------------------------------------------------
 * B-tree sort support
 *---------------------------------------------------------
 */

static int
int16_sortsupport_cmp(Datum x, Datum y, SortSupport ssup)
{
	INT128		a,
				b;

	memcpy(&a, DatumGetPointer(x), INT16_LEN);
	memcpy(&b, DatumGetPointer(y), INT16_LEN);

	return int128_compare(a, b);
}

Datum
int16sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = int16_sortsupport_cmp;
	PG_RETURN_VOID();
}

/*----------------------------------------------------------
 * B-tree skip support
 *---------------------------------------------------------
 */

static Datum
int16_decrement(Relation rel, Datum existing, bool *underflow)
{
	INT128		val;

	memcpy(&val, DatumGetPointer(existing), INT16_LEN);

	if (int128_compare(val, INT16_MIN_VAL) == 0)
	{
		/* return value is undefined */
		*underflow = true;
		return (Datum) 0;
	}

	*underflow = false;
	{
		INT128		result;
		int16StructP r;

		/* Decrement: subtract 1 */
		int128_sub_uint64(&val, 1);
		result = val;

		r = palloc(INT16_LEN);
		memcpy(r, &result, INT16_LEN);
		return PointerGetDatum(r);
	}
}

static Datum
int16_increment(Relation rel, Datum existing, bool *overflow)
{
	INT128		val;

	memcpy(&val, DatumGetPointer(existing), INT16_LEN);

	if (int128_compare(val, INT16_MAX_VAL) == 0)
	{
		/* return value is undefined */
		*overflow = true;
		return (Datum) 0;
	}

	*overflow = false;
	{
		INT128		result;
		int16StructP r;

		/* Increment: add 1 */
		int128_add_uint64(&val, 1);
		result = val;

		r = palloc(INT16_LEN);
		memcpy(r, &result, INT16_LEN);
		return PointerGetDatum(r);
	}
}

Datum
int16skipsupport(PG_FUNCTION_ARGS)
{
	SkipSupport sksup = (SkipSupport) PG_GETARG_POINTER(0);
	INT128		low_val = INT16_MIN_VAL;
	INT128		high_val = INT16_MAX_VAL;
	int16StructP low_p;
	int16StructP high_p;

	sksup->decrement = int16_decrement;
	sksup->increment = int16_increment;

	low_p = palloc(INT16_LEN);
	memcpy(low_p, &low_val, INT16_LEN);
	sksup->low_elem = PointerGetDatum(low_p);

	high_p = palloc(INT16_LEN);
	memcpy(high_p, &high_val, INT16_LEN);
	sksup->high_elem = PointerGetDatum(high_p);

	PG_RETURN_VOID();
}
