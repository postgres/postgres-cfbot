/*-------------------------------------------------------------------------
 *
 * int16.h
 *	  Header file for the "int16" 128-bit signed integer ADT.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * contrib/int16/int16.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INT16_EXT_H
#define INT16_EXT_H

#include "common/int128.h"

/* int16 size in bytes */
#define INT16_LEN	16

typedef INT128 int16Struct;
typedef int16Struct * int16StructP;

/* fmgr interface macros */
static inline Datum
Int16PGetDatum(const int16StructP X)
{
	return PointerGetDatum(X);
}

#define PG_RETURN_INT16_P(X)	return Int16PGetDatum(X)

static inline int16StructP
DatumGetInt16P(Datum X)
{
	return (int16StructP) DatumGetPointer(X);
}

#define PG_GETARG_INT16_P(n)	DatumGetInt16P(PG_GETARG_DATUM(n))

/*
 * INT128 limits: 2^127 - 1 and -2^127.
 * 170141183460469231731687303715884105727
 * -170141183460469231731687303715884105728
 */
#define INT16_MAX_VAL	make_int128(INT64CONST(0x7fffffffffffffff), UINT64CONST(0xffffffffffffffff))
#define INT16_MIN_VAL	make_int128(INT64CONST(0x8000000000000000), UINT64CONST(0))

/* Convert INT128 to numeric Datum (shared by int16_avg and int16_numeric). */
extern Datum int128_to_numeric(INT128 val);

/* int16 input function (shared by int16in and numeric_int16). */
extern PGDLLEXPORT Datum int16in(PG_FUNCTION_ARGS);

/*
 * Internal helpers shared across int16 source files.
 * Defined in int16.c, used by int16_funcs.c.
 */
extern bool int128_add_overflow(INT128 a, INT128 b, INT128 *result);
extern bool int128_sub_overflow(INT128 a, INT128 b, INT128 *result);
extern bool int128_negate_overflow(INT128 a, INT128 *result);
extern bool int128_mul_overflow(INT128 a, INT128 b, INT128 *result);
extern bool int128_div_mod_int128(INT128 a, INT128 b,
								  INT128 *quotient, INT128 *remainder);

#endif							/* INT16_EXT_H */
