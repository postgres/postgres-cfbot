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

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/pgbench_funcs.h"
#include "common/pg_prng.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* Shared PRNG state used by pgbench extension random functions */
static pg_prng_state pgbench_prng_state;
static bool pgbench_prng_seed_set = false;
static bool pgbench_nurand_c_initialized = false;
static int64 pgbench_nurand_c_last = 0;
static int64 pgbench_nurand_c_id = 0;
static int64 pgbench_nurand_c_ol = 0;
static int64 pgbench_nurand_c_other = 0;

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
	pgbench_nurand_c_initialized = false;

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgbench_setseed_int64);
Datum
pgbench_setseed_int64(PG_FUNCTION_ARGS)
{
	int64		seed = PG_GETARG_INT64(0);

	pg_prng_seed(&pgbench_prng_state, (uint64) seed);
	pgbench_prng_seed_set = true;
	pgbench_nurand_c_initialized = false;

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgbench_setseed_int32);
Datum
pgbench_setseed_int32(PG_FUNCTION_ARGS)
{
	int32		seed = PG_GETARG_INT32(0);

	pg_prng_seed(&pgbench_prng_state, (uint64) seed);
	pgbench_prng_seed_set = true;
	pgbench_nurand_c_initialized = false;

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

PG_FUNCTION_INFO_V1(pgbench_nurand_int64);
Datum
pgbench_nurand_int64(PG_FUNCTION_ARGS)
{
	int64		A = PG_GETARG_INT64(0);
	int64		min = PG_GETARG_INT64(1);
	int64		max = PG_GETARG_INT64(2);
	int64		C;

	if (A < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("nurand A parameter must be non-negative (not " INT64_FORMAT ")", A)));

	check_random_range(min, max);
	initialize_prng();

	if (PG_NARGS() >= 4 && !PG_ARGISNULL(3))
		C = PG_GETARG_INT64(3);
	else
	{
		if (!pgbench_nurand_c_initialized)
		{
			pgbench_nurand_c_last = pgbench_random(&pgbench_prng_state, 0, 255);
			pgbench_nurand_c_id = pgbench_random(&pgbench_prng_state, 0, 1023);
			pgbench_nurand_c_ol = pgbench_random(&pgbench_prng_state, 0, 8191);
			pgbench_nurand_c_other = pgbench_random(&pgbench_prng_state, 0, Max(A, 1));
			pgbench_nurand_c_initialized = true;
		}

		if (A == 255)
			C = pgbench_nurand_c_last;
		else if (A == 1023)
			C = pgbench_nurand_c_id;
		else if (A == 8191)
			C = pgbench_nurand_c_ol;
		else
			C = pgbench_nurand_c_other % (A + 1);
	}

	PG_RETURN_INT64(pgbench_nurand(&pgbench_prng_state, A, min, max, C));
}

PG_FUNCTION_INFO_V1(pgbench_c_last_text);
Datum
pgbench_c_last_text(PG_FUNCTION_ARGS)
{
	int64		num = PG_GETARG_INT64(0);
	char		buf[32];

	if (!pgbench_c_last(num, buf, sizeof(buf)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("c_last parameter must be in range [0, 999] (not " INT64_FORMAT ")", num)));

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

PG_FUNCTION_INFO_V1(pgbench_random_astring_text);
Datum
pgbench_random_astring_text(PG_FUNCTION_ARGS)
{
	int64		min_len = PG_GETARG_INT64(0);
	int64		max_len = PG_GETARG_INT64(1);
	char	   *buf;
	text	   *result;

	if (min_len < 0 || min_len > max_len || max_len > 1048576)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid string length range [" INT64_FORMAT ", " INT64_FORMAT "]",
						min_len, max_len)));

	initialize_prng();
	buf = palloc((Size) max_len + 1);
	pgbench_random_astring(&pgbench_prng_state, min_len, max_len, buf);
	result = cstring_to_text(buf);
	pfree(buf);

	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_random_nstring_text);
Datum
pgbench_random_nstring_text(PG_FUNCTION_ARGS)
{
	int64		min_len = PG_GETARG_INT64(0);
	int64		max_len = PG_GETARG_INT64(1);
	char	   *buf;
	text	   *result;

	if (min_len < 0 || min_len > max_len || max_len > 1048576)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid string length range [" INT64_FORMAT ", " INT64_FORMAT "]",
						min_len, max_len)));

	initialize_prng();
	buf = palloc((Size) max_len + 1);
	pgbench_random_nstring(&pgbench_prng_state, min_len, max_len, buf);
	result = cstring_to_text(buf);
	pfree(buf);

	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_date_add_text);
Datum
pgbench_date_add_text(PG_FUNCTION_ARGS)
{
	char	   *base_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		offset = PG_GETARG_INT64(1);
	char	   *unit_str = text_to_cstring(PG_GETARG_TEXT_PP(2));
	int64		base_usec,
				res_usec;
	char		buf[64];

	if (!pgbench_parse_timestamp(base_str, &base_usec))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid timestamp string \"%s\"", base_str)));

	if (!pgbench_date_add(base_usec, offset, unit_str, &res_usec))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid unit \"%s\" for date_add", unit_str)));

	pgbench_format_timestamp(res_usec, buf, sizeof(buf));
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

PG_FUNCTION_INFO_V1(pgbench_int_to_date_window_text);
Datum
pgbench_int_to_date_window_text(PG_FUNCTION_ARGS)
{
	char	   *base_str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		val = PG_GETARG_INT64(1);
	int64		max_val = PG_GETARG_INT64(2);
	char	   *interval_str = text_to_cstring(PG_GETARG_TEXT_PP(3));
	int64		base_usec,
				res_usec;
	char		buf[64];

	if (!pgbench_parse_timestamp(base_str, &base_usec))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid timestamp string \"%s\"", base_str)));

	if (!pgbench_int_to_date_window(base_usec, val, max_val, interval_str, &res_usec))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid parameters (max_val=" INT64_FORMAT ", interval=\"%s\") for int_to_date",
						max_val, interval_str)));

	pgbench_format_timestamp(res_usec, buf, sizeof(buf));
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

PG_FUNCTION_INFO_V1(pgbench_to_timestamp_text);
Datum
pgbench_to_timestamp_text(PG_FUNCTION_ARGS)
{
	char	   *str = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64		usec;
	char		buf[64];

	if (!pgbench_parse_timestamp(str, &usec))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("invalid timestamp string \"%s\"", str)));

	pgbench_format_timestamp(usec, buf, sizeof(buf));
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

PG_FUNCTION_INFO_V1(pgbench_format_string_text);
Datum
pgbench_format_string_text(PG_FUNCTION_ARGS)
{
	char	   *fmt;
	const char *p;
	StringInfoData buf;
	int			nargs = PG_NARGS();
	int			arg_idx = 1;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	fmt = text_to_cstring(PG_GETARG_TEXT_PP(0));
	initStringInfo(&buf);
	p = fmt;

	while (*p != '\0')
	{
		char		spec[64];
		size_t		slen = 0;
		char		conv;
		Oid			argtype;
		Oid			typoutput;
		bool		typIsVarlena;
		char	   *argstr;

		if (*p != '%')
		{
			appendStringInfoChar(&buf, *p++);
			continue;
		}
		if (p[1] == '%')
		{
			appendStringInfoChar(&buf, '%');
			p += 2;
			continue;
		}

		spec[slen++] = *p++;	/* '%' */

		/* flags */
		while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')
		{
			if (slen < sizeof(spec) - 8)
				spec[slen++] = *p;
			p++;
		}
		/* width */
		while (*p >= '0' && *p <= '9')
		{
			if (slen < sizeof(spec) - 8)
				spec[slen++] = *p;
			p++;
		}
		/* precision */
		if (*p == '.')
		{
			if (slen < sizeof(spec) - 8)
				spec[slen++] = *p;
			p++;
			while (*p >= '0' && *p <= '9')
			{
				if (slen < sizeof(spec) - 8)
					spec[slen++] = *p;
				p++;
			}
		}
		/* skip any user length modifiers (we supply our own) */
		while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't')
			p++;

		conv = *p;
		if (conv == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unterminated format specifier in format_string")));
		p++;

		if (arg_idx >= nargs)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("not enough arguments for format_string")));

		if (PG_ARGISNULL(arg_idx))
			PG_RETURN_NULL();

		argtype = get_fn_expr_argtype(fcinfo->flinfo, arg_idx);
		getTypeOutputInfo(argtype, &typoutput, &typIsVarlena);
		argstr = OidOutputFunctionCall(typoutput, PG_GETARG_DATUM(arg_idx));
		arg_idx++;

		if (conv == 'd' || conv == 'i' || conv == 'u' ||
			conv == 'x' || conv == 'X' || conv == 'o')
		{
			char	   *endptr;
			long long	ival;

			errno = 0;
			ival = strtoll(argstr, &endptr, 10);
			if (errno != 0 || endptr == argstr || *endptr != '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid integer argument \"%s\" for format_string", argstr)));

			spec[slen++] = 'l';
			spec[slen++] = 'l';
			spec[slen++] = (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') ? conv : 'd';
			spec[slen] = '\0';
			appendStringInfo(&buf, spec, ival);
		}
		else if (conv == 'f' || conv == 'F' || conv == 'e' ||
				 conv == 'E' || conv == 'g' || conv == 'G')
		{
			char	   *endptr;
			double		dval;

			errno = 0;
			dval = strtod(argstr, &endptr);
			if (errno != 0 || endptr == argstr || *endptr != '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid float argument \"%s\" for format_string", argstr)));

			spec[slen++] = conv;
			spec[slen] = '\0';
			appendStringInfo(&buf, spec, dval);
		}
		else if (conv == 's')
		{
			spec[slen++] = 's';
			spec[slen] = '\0';
			appendStringInfo(&buf, spec, argstr);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format specifier \"%%%c\" in format_string", conv)));
		}

		pfree(argstr);
	}

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

PG_FUNCTION_INFO_V1(pgbench_array_generate_const);
Datum
pgbench_array_generate_const(PG_FUNCTION_ARGS)
{
	int64		len = PG_GETARG_INT64(0);
	int64		val = PG_GETARG_INT64(1);
	Datum	   *elems;
	ArrayType  *result;

	if (len < 0 || len > 100000)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("array_generate length " INT64_FORMAT " out of range [0, 100000]", len)));

	if (len == 0)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(INT8OID));

	elems = (Datum *) palloc(sizeof(Datum) * (Size) len);
	for (int64 i = 0; i < len; i++)
	{
		if ((i & 0xfff) == 0)
			CHECK_FOR_INTERRUPTS();
		elems[i] = Int64GetDatum(val);
	}

	result = construct_array_builtin(elems, (int) len, INT8OID);
	pfree(elems);

	PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_array_generate_random);
Datum
pgbench_array_generate_random(PG_FUNCTION_ARGS)
{
	int64		len = PG_GETARG_INT64(0);
	int64		min_val = PG_GETARG_INT64(1);
	int64		max_val = PG_GETARG_INT64(2);
	Datum	   *elems;
	ArrayType  *result;

	if (len < 0 || len > 100000)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("array_generate length " INT64_FORMAT " out of range [0, 100000]", len)));

	check_random_range(min_val, max_val);

	if (len == 0)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(INT8OID));

	initialize_prng();
	elems = (Datum *) palloc(sizeof(Datum) * (Size) len);
	for (int64 i = 0; i < len; i++)
	{
		if ((i & 0xfff) == 0)
			CHECK_FOR_INTERRUPTS();
		elems[i] = Int64GetDatum(pgbench_random(&pgbench_prng_state, min_val, max_val));
	}

	result = construct_array_builtin(elems, (int) len, INT8OID);
	pfree(elems);

	PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_random_bytea_bytea);
Datum
pgbench_random_bytea_bytea(PG_FUNCTION_ARGS)
{
	int64		min_len = PG_GETARG_INT64(0);
	int64		max_len = PG_GETARG_INT64(1);
	int64		actual_len;
	bytea	   *result;

	if (min_len < 0 || min_len > max_len || max_len > 16777216)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid bytea length range [" INT64_FORMAT ", " INT64_FORMAT "]",
						min_len, max_len)));

	initialize_prng();
	result = (bytea *) palloc(VARHDRSZ + (Size) max_len);
	actual_len = pgbench_random_bytea(&pgbench_prng_state, min_len, max_len,
									  (uint8 *) VARDATA(result));
	SET_VARSIZE(result, VARHDRSZ + (Size) actual_len);

	PG_RETURN_BYTEA_P(result);
}

static inline void
check_vector_dim(int32 dim)
{
	if (dim < 1 || dim > PGBENCH_MAX_VECTOR_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("vector dimension %d out of range [1, %d]",
						dim, PGBENCH_MAX_VECTOR_DIM)));
}

static text *
format_float_vector_to_text(const float *vec, int32 dim)
{
	Size		buflen = (Size) dim * 16 + 8;
	char	   *buf = (char *) palloc(buflen);
	size_t		len = pgbench_format_vector(vec, dim, buf, buflen);
	text	   *res = cstring_to_text_with_len(buf, (int) len);

	pfree(buf);
	return res;
}

PG_FUNCTION_INFO_V1(pgbench_random_vector_text);
Datum
pgbench_random_vector_text(PG_FUNCTION_ARGS)
{
	int32		dim = PG_GETARG_INT32(0);
	float8		min_val = PG_GETARG_FLOAT8(1);
	float8		max_val = PG_GETARG_FLOAT8(2);
	bool		normalize = PG_GETARG_BOOL(3);
	float	   *vec;
	text	   *result;

	check_vector_dim(dim);
	if (isnan(min_val) || isnan(max_val) || min_val > max_val)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid range [%g, %g] for random_vector",
						min_val, max_val)));

	initialize_prng();
	vec = (float *) palloc(sizeof(float) * (Size) dim);
	pgbench_random_vector(&pgbench_prng_state, dim, min_val, max_val, normalize, vec);
	result = format_float_vector_to_text(vec, dim);
	pfree(vec);

	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_random_vector_dist_text);
Datum
pgbench_random_vector_dist_text(PG_FUNCTION_ARGS)
{
	int32		dim = PG_GETARG_INT32(0);
	char	   *dist_str = text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		normalize = PG_GETARG_BOOL(2);
	float	   *vec;
	text	   *result;

	check_vector_dim(dim);
	initialize_prng();
	vec = (float *) palloc(sizeof(float) * (Size) dim);

	if (pg_strcasecmp(dist_str, "gaussian") == 0 ||
		pg_strcasecmp(dist_str, "normal") == 0)
	{
		pgbench_random_vector_gaussian(&pgbench_prng_state, dim, 1.0, normalize, vec);
	}
	else if (pg_strcasecmp(dist_str, "sphere") == 0)
	{
		pgbench_random_vector_gaussian(&pgbench_prng_state, dim, 1.0, true, vec);
	}
	else if (pg_strcasecmp(dist_str, "uniform") == 0)
	{
		pgbench_random_vector(&pgbench_prng_state, dim, -1.0, 1.0, normalize, vec);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid vector distribution \"%s\" (expected \"gaussian\", \"uniform\", or \"sphere\")",
						dist_str)));
	}

	result = format_float_vector_to_text(vec, dim);
	pfree(vec);
	pfree(dist_str);

	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_random_vector_gaussian_text);
Datum
pgbench_random_vector_gaussian_text(PG_FUNCTION_ARGS)
{
	int32		dim = PG_GETARG_INT32(0);
	float8		sigma = PG_GETARG_FLOAT8(1);
	bool		normalize = PG_GETARG_BOOL(2);
	float	   *vec;
	text	   *result;

	check_vector_dim(dim);
	if (isnan(sigma) || sigma <= 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("sigma must be greater than zero (not %g)", sigma)));

	initialize_prng();
	vec = (float *) palloc(sizeof(float) * (Size) dim);
	pgbench_random_vector_gaussian(&pgbench_prng_state, dim, sigma, normalize, vec);
	result = format_float_vector_to_text(vec, dim);
	pfree(vec);

	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_clustered_vector_text);
Datum
pgbench_clustered_vector_text(PG_FUNCTION_ARGS)
{
	int32		dim = PG_GETARG_INT32(0);
	int32		num_clusters = PG_GETARG_INT32(1);
	float8		sigma = PG_GETARG_FLOAT8(2);
	int64		seed = PG_GETARG_INT64(3);
	int32		cluster_id = PG_GETARG_INT32(4);
	float	   *vec;
	text	   *result;

	check_vector_dim(dim);
	if (num_clusters < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("num_clusters must be at least 1 (not %d)", num_clusters)));
	if (isnan(sigma) || sigma < 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("sigma must be non-negative (not %g)", sigma)));

	initialize_prng();
	vec = (float *) palloc(sizeof(float) * (Size) dim);
	pgbench_clustered_vector(&pgbench_prng_state, dim, num_clusters,
							 sigma, seed, cluster_id, vec);
	result = format_float_vector_to_text(vec, dim);
	pfree(vec);

	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_planted_vector_text);
Datum
pgbench_planted_vector_text(PG_FUNCTION_ARGS)
{
	int32		dim = PG_GETARG_INT32(0);
	int64		target_id = PG_GETARG_INT64(1);
	float8		delta = PG_GETARG_FLOAT8(2);
	int64		seed = PG_GETARG_INT64(3);
	float	   *vec;
	text	   *result;

	check_vector_dim(dim);
	if (isnan(delta) || delta < 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("delta must be non-negative (not %g)", delta)));

	initialize_prng();
	vec = (float *) palloc(sizeof(float) * (Size) dim);
	pgbench_planted_vector(&pgbench_prng_state, dim, target_id, delta, seed, vec);
	result = format_float_vector_to_text(vec, dim);
	pfree(vec);

	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_random_sparse_vector_text);
Datum
pgbench_random_sparse_vector_text(PG_FUNCTION_ARGS)
{
	int32		dim = PG_GETARG_INT32(0);
	int32		nnz = PG_GETARG_INT32(1);
	float8		min_val = PG_GETARG_FLOAT8(2);
	float8		max_val = PG_GETARG_FLOAT8(3);
	Size		buflen;
	char	   *buf;
	text	   *result;

	if (dim < 1 || dim > 100000000 || nnz < 0 || nnz > dim || nnz > 100000)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid sparse vector parameters (dim=%d, nnz=%d)",
						dim, nnz)));
	if (isnan(min_val) || isnan(max_val) || min_val > max_val)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid range [%g, %g] for random_sparse_vector",
						min_val, max_val)));

	initialize_prng();
	buflen = (Size) nnz * 28 + 32;
	buf = (char *) palloc(buflen);
	if (!pgbench_random_sparse_vector(&pgbench_prng_state, dim, nnz,
									  min_val, max_val, buf, buflen))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to format sparse vector")));

	result = cstring_to_text(buf);
	pfree(buf);

	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_rmat_edge_record);
Datum
pgbench_rmat_edge_record(PG_FUNCTION_ARGS)
{
	int32		scale_log2 = PG_GETARG_INT32(0);
	float8		a = PG_GETARG_FLOAT8(1);
	float8		b = PG_GETARG_FLOAT8(2);
	float8		c = PG_GETARG_FLOAT8(3);
	int64		seed = PG_GETARG_INT64(4);
	int64		src,
				dst;
	Datum		elems[2];
	ArrayType  *result;

	initialize_prng();
	if (!pgbench_rmat_edge(&pgbench_prng_state, scale_log2, a, b, c, seed,
						   &src, &dst))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid rmat_edge parameters (scale_log2=%d, a=%g, b=%g, c=%g)",
						scale_log2, a, b, c)));

	elems[0] = Int64GetDatum(src);
	elems[1] = Int64GetDatum(dst);
	result = construct_array_builtin(elems, 2, INT8OID);

	PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(pgbench_vector_distance_text);
Datum
pgbench_vector_distance_text(PG_FUNCTION_ARGS)
{
	char	   *s1 = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *s2 = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *metric_str = text_to_cstring(PG_GETARG_TEXT_PP(2));
	PgBenchVecDistMetric metric;
	int			dim1 = 0,
				dim2 = 0;
	float	   *v1,
			   *v2;
	double		dist;

	if (!pgbench_parse_vec_dist_metric(metric_str, &metric))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid vector distance metric \"%s\" (expected \"l2\", \"cosine\", or \"ip\")",
						metric_str)));

	if (!pgbench_parse_vector_count(s1, &dim1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid vector literal \"%s\"", s1)));

	if (!pgbench_parse_vector_count(s2, &dim2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid vector literal \"%s\"", s2)));

	if (dim1 != dim2)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different vector dimensions %d and %d", dim1, dim2)));

	v1 = (float *) palloc(sizeof(float) * (Size) dim1);
	v2 = (float *) palloc(sizeof(float) * (Size) dim2);
	(void) pgbench_parse_vector_into(s1, v1, dim1);
	(void) pgbench_parse_vector_into(s2, v2, dim2);

	dist = pgbench_vector_distance(v1, v2, dim1, metric);
	pfree(v1);
	pfree(v2);

	PG_RETURN_FLOAT8(dist);
}

PG_FUNCTION_INFO_V1(pgbench_ldbc_correlated_edge_sql);
Datum
pgbench_ldbc_correlated_edge_sql(PG_FUNCTION_ARGS)
{
	int64		src_id = PG_GETARG_INT64(0);
	int64		num_vertices = PG_GETARG_INT64(1);
	int64		num_communities = PG_GETARG_INT64(2);
	double		homophily_prob = PG_GETARG_FLOAT8(3);
	int64		seed = PG_GETARG_INT64(4);
	int64		dst = 0;

	initialize_prng();

	if (!pgbench_ldbc_correlated_edge(&pgbench_prng_state,
									  src_id,
									  num_vertices,
									  num_communities,
									  homophily_prob,
									  seed,
									  &dst))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid ldbc_correlated_edge parameters (num_vertices=%lld, num_communities=%lld, homophily_prob=%g)",
						(long long) num_vertices,
						(long long) num_communities,
						homophily_prob)));

	PG_RETURN_INT64(dst);
}

PG_FUNCTION_INFO_V1(pgbench_ldbc_flashmob_ts_sql);
Datum
pgbench_ldbc_flashmob_ts_sql(PG_FUNCTION_ARGS)
{
	int64		base_ts_us = PG_GETARG_INT64(0);
	int64		span_us = PG_GETARG_INT64(1);
	int64		num_spikes = PG_GETARG_INT64(2);
	double		spike_prob = PG_GETARG_FLOAT8(3);
	int64		seed = PG_GETARG_INT64(4);
	int64		ts = 0;

	initialize_prng();

	if (!pgbench_ldbc_flashmob_ts(&pgbench_prng_state,
								  base_ts_us,
								  span_us,
								  num_spikes,
								  spike_prob,
								  seed,
								  &ts))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid ldbc_flashmob_ts parameters (span_us=%lld, num_spikes=%lld, spike_prob=%g)",
						(long long) span_us,
						(long long) num_spikes,
						spike_prob)));

	PG_RETURN_INT64(ts);
}

