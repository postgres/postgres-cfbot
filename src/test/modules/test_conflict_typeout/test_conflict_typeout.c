/*--------------------------------------------------------------------------
 * test_conflict_typeout.c
 *
 * Demonstration type for the conflict-log-table residual gap: a
 * C-language type whose output function is disproportionate to its
 * storage/send footprint.  The conflict log table's per-column size cap
 * (CONFLICT_MAX_VALUE_SIZE in conflict.c) only measures a value's stored
 * size before deciding whether to render it, never what the output
 * function actually produces, so a type built this way sails through the
 * cap and can still overflow the log table's JSON rendering.
 *
 * boomtype stores a single 4-byte integer, wrapped in a varlena header.
 * Its send/receive functions are equally small, so a subscription with
 * binary = true only ever moves 4 bytes of this type across the wire.
 * Its output function ignores the stored value entirely and always
 * renders BOOM_OUTPUT_SIZE bytes.
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_conflict_typeout/test_conflict_typeout.c
 *
 *--------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/*
 * Two columns of this size land at 1.2GB combined, comfortably over the
 * ~1GB StringInfo limit, while each individual value is well under it.
 */
#define BOOM_OUTPUT_SIZE 600000000

PG_FUNCTION_INFO_V1(boomtype_in);
PG_FUNCTION_INFO_V1(boomtype_out);
PG_FUNCTION_INFO_V1(boomtype_send);
PG_FUNCTION_INFO_V1(boomtype_recv);
PG_FUNCTION_INFO_V1(boomtype_cmp);
PG_FUNCTION_INFO_V1(boomtype_lt);
PG_FUNCTION_INFO_V1(boomtype_eq);
PG_FUNCTION_INFO_V1(boomtype_gt);

static int32
boomtype_cmp_internal(bytea *a, bytea *b)
{
	int32		av,
				bv;

	memcpy(&av, VARDATA_ANY(a), sizeof(int32));
	memcpy(&bv, VARDATA_ANY(b), sizeof(int32));
	return av < bv ? -1 : (av > bv ? 1 : 0);
}

Datum
boomtype_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	int32		val = pg_strtoint32(str);
	bytea	   *result = (bytea *) palloc(VARHDRSZ + sizeof(int32));

	SET_VARSIZE(result, VARHDRSZ + sizeof(int32));
	memcpy(VARDATA(result), &val, sizeof(int32));
	PG_RETURN_BYTEA_P(result);
}

/*
 * Ignores the (8-byte-stored) input entirely.  A well-behaved output
 * function's result size tracks its input; this one doesn't, which is
 * exactly what the conflict log's size cap cannot detect, since the cap
 * only measures the raw stored size of the value, not what this function
 * is about to return.
 */
Datum
boomtype_out(PG_FUNCTION_ARGS)
{
	char	   *huge = palloc(BOOM_OUTPUT_SIZE + 1);

	memset(huge, 'x', BOOM_OUTPUT_SIZE);
	huge[BOOM_OUTPUT_SIZE] = '\0';
	PG_RETURN_CSTRING(huge);
}

/* Compact binary form -- this is what actually crosses the wire. */
Datum
boomtype_send(PG_FUNCTION_ARGS)
{
	bytea	   *val = PG_GETARG_BYTEA_PP(0);
	int32		intval;
	StringInfoData buf;

	memcpy(&intval, VARDATA_ANY(val), sizeof(int32));
	pq_begintypsend(&buf);
	pq_sendint32(&buf, intval);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
boomtype_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int32		val = pq_getmsgint(buf, 4);
	bytea	   *result = (bytea *) palloc(VARHDRSZ + sizeof(int32));

	SET_VARSIZE(result, VARHDRSZ + sizeof(int32));
	memcpy(VARDATA(result), &val, sizeof(int32));
	PG_RETURN_BYTEA_P(result);
}

/*
 * Just enough comparison support for a btree opclass, so boomtype can be a
 * PK.  Written directly in C, rather than as SQL-language wrappers around
 * boomtype_cmp, because the apply worker runs with search_path = '' and an
 * unqualified name inside a SQL function body would fail to resolve there.
 */
Datum
boomtype_cmp(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_PP(0);
	bytea	   *b = PG_GETARG_BYTEA_PP(1);

	PG_RETURN_INT32(boomtype_cmp_internal(a, b));
}

Datum
boomtype_lt(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_PP(0);
	bytea	   *b = PG_GETARG_BYTEA_PP(1);

	PG_RETURN_BOOL(boomtype_cmp_internal(a, b) < 0);
}

Datum
boomtype_eq(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_PP(0);
	bytea	   *b = PG_GETARG_BYTEA_PP(1);

	PG_RETURN_BOOL(boomtype_cmp_internal(a, b) == 0);
}

Datum
boomtype_gt(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_PP(0);
	bytea	   *b = PG_GETARG_BYTEA_PP(1);

	PG_RETURN_BOOL(boomtype_cmp_internal(a, b) > 0);
}
