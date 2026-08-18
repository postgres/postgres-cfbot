/*-------------------------------------------------------------------------
 *
 * int16_numeric.c
 *	  Cast functions between int16 (128-bit signed integer) and numeric.
 *
 * The conversion approach mirrors core PostgreSQL's int8↔numeric casts:
 *
 *  - int16 → numeric: uses the shared int128_to_numeric() helper (string-based).
 *  - numeric → int16: rounds to 0 decimal places (round-half-away-from-zero,
 *    matching numeric_int8's round_var behavior), then converts via string
 *    representation using numeric_out / int16in.
 *
 * NaN and Infinity numerics are rejected with ERRCODE_FEATURE_NOT_SUPPORTED,
 * matching the core numeric_int8 behavior.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * contrib/int16/int16_numeric.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "int16.h"

PG_FUNCTION_INFO_V1(int16_numeric);
PG_FUNCTION_INFO_V1(numeric_int16);

/*
 * int16_numeric(int16) → numeric
 *
 * Convert a 128-bit signed integer to numeric.
 * This is the widening direction → IMPLICIT cast (matches int8→numeric).
 */
Datum
int16_numeric(PG_FUNCTION_ARGS)
{
	int16StructP val = PG_GETARG_INT16_P(0);
	INT128		v;

	memcpy(&v, val, INT16_LEN);

	return int128_to_numeric(v);
}

/*
 * numeric_int16(numeric) → int16
 *
 * Convert a numeric to a 128-bit signed integer.
 * This is the narrowing direction → ASSIGNMENT cast (matches numeric→int8).
 *
 * Behavior matches core numeric_int8():
 *  - NaN and Infinity raise ERRCODE_FEATURE_NOT_SUPPORTED
 *  - Fractional parts are rounded to nearest integer (round-half-away-from-zero)
 *  - Values outside the int16 range raise ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE
 */
Datum
numeric_int16(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);
	Datum		rounded;
	char	   *str;
	int16StructP result;

	/* Reject NaN and Infinity, matching numeric_int8 behavior */
	if (numeric_is_nan(num))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot convert NaN to %s", "int16")));
	else if (numeric_is_inf(num))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot convert infinity to %s", "int16")));

	/*
	 * Round to 0 decimal places (round-half-away-from-zero), matching
	 * numeric_int8's use of round_var(&rounded, 0).
	 */
	rounded = DirectFunctionCall2(numeric_round,
								  NumericGetDatum(num),
								  Int32GetDatum(0));

	/* Convert the rounded numeric to a string (no exponent, plain decimal) */
	str = DatumGetCString(DirectFunctionCall1(numeric_out, rounded));

	/*
	 * Parse the string as int16.  int16in will raise
	 * ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE for values outside the int16 range,
	 * and ERRCODE_INVALID_TEXT_REPRESENTATION for malformed input.
	 */
	result = DatumGetInt16P(DirectFunctionCall3(int16in,
												CStringGetDatum(str),
												ObjectIdGetDatum(InvalidOid),
												Int32GetDatum(-1)));

	PG_RETURN_INT16_P(result);
}
