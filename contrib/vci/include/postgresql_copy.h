/*-------------------------------------------------------------------------
 *
 * postgresql_copy.h
 *	  Definitions copied from PostgreSQL core
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/postgresql_copy.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGRESQL_COPY_H
#define POSTGRESQL_COPY_H

/*
 *  src/backend/utils/adt/float.c
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/elog.h"
#include "utils/errcodes.h"

/*
 * check to see if a float4/8 val has underflowed or overflowed
 */
#define CHECKFLOATVAL(val, inf_is_valid, zero_is_valid)			\
do {															\
	if (isinf(val) && !(inf_is_valid))							\
		ereport(ERROR,											\
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),	\
		  errmsg("value out of range: overflow")));				\
																\
	if ((val) == 0.0 && !(zero_is_valid))						\
		ereport(ERROR,											\
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),	\
		 errmsg("value out of range: underflow")));				\
} while(0)

/*
 * src/backend/utils/adt/float.c
 */
static inline float8 *
check_float8_array(ArrayType *transarray, const char *caller, int n)
{
	/*
	 * We expect the input to be an N-element float array; verify that. We
	 * don't need to use deconstruct_array() since the array data is just
	 * going to look like a C array of N float8 values.
	 */
	if (ARR_NDIM(transarray) != 1 ||
		ARR_DIMS(transarray)[0] != n ||
		ARR_HASNULL(transarray) ||
		ARR_ELEMTYPE(transarray) != FLOAT8OID)
		elog(ERROR, "%s: expected %d-element float8 array", caller, n);
	return (float8 *) ARR_DATA_PTR(transarray);
}

typedef struct Int8TransTypeData
{
	int64		count;
	int64		sum;
} Int8TransTypeData;

#ifdef VCI_USE_CMP_FUNC
/*
 *		interval_relop	- is interval1 relop interval2
 *
 *		collate invalid interval at the end
 */
static inline TimeOffset
interval_cmp_value(const Interval *interval)
{
	TimeOffset	span;

	span = interval->time;

#ifdef HAVE_INT64_TIMESTAMP
	span += interval->month * INT64CONST(30) * USECS_PER_DAY;
	span += interval->day * INT64CONST(24) * USECS_PER_HOUR;
#else
	span += interval->month * ((double) DAYS_PER_MONTH * SECS_PER_DAY);
	span += interval->day * ((double) HOURS_PER_DAY * SECS_PER_HOUR);
#endif

	return span;
}

static int
interval_cmp_internal(Interval *interval1, Interval *interval2)
{
	TimeOffset	span1 = interval_cmp_value(interval1);
	TimeOffset	span2 = interval_cmp_value(interval2);

	return ((span1 < span2) ? -1 : (span1 > span2) ? 1 : 0);
}

static int
timetz_cmp_internal(TimeTzADT *time1, TimeTzADT *time2)
{
	TimeOffset	t1,
				t2;

	/* Primary sort is by true (GMT-equivalent) time */
#ifdef HAVE_INT64_TIMESTAMP
	t1 = time1->time + (time1->zone * USECS_PER_SEC);
	t2 = time2->time + (time2->zone * USECS_PER_SEC);
#else
	t1 = time1->time + time1->zone;
	t2 = time2->time + time2->zone;
#endif

	if (t1 > t2)
		return 1;
	if (t1 < t2)
		return -1;

	/*
	 * If same GMT time, sort by timezone; we only want to say that two
	 * timetz's are equal if both the time and zone parts are equal.
	 */
	if (time1->zone > time2->zone)
		return 1;
	if (time1->zone < time2->zone)
		return -1;

	return 0;
}
#endif

/* taken from numeric.c */

typedef int16 NumericDigit;
struct NumericShort
{
	uint16		n_header;		/* Sign + display scale + weight */
	NumericDigit n_data[1];		/* Digits */
};

struct NumericLong
{
	uint16		n_sign_dscale;	/* Sign + display scale */
	int16		n_weight;		/* Weight of 1st digit  */
	NumericDigit n_data[1];		/* Digits */
};

union NumericChoice
{
	uint16		n_header;		/* Header word */
	struct NumericLong n_long;	/* Long form (4-byte header) */
	struct NumericShort n_short;	/* Short form (2-byte header) */
};

struct NumericData
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	union NumericChoice choice; /* choice of format */
};

typedef struct NumericVar
{
	int			ndigits;		/* # of digits in digits[] - can be 0! */
	int			weight;			/* weight of first digit */
	int			sign;			/* NUMERIC_POS, NUMERIC_NEG, or NUMERIC_NAN */
	int			dscale;			/* display scale */
	NumericDigit *buf;			/* start of palloc'd space for digits[] */
	NumericDigit *digits;		/* base-NBASE digits */
} NumericVar;

#endif							/* POSTGRESQL_COPY_H */
