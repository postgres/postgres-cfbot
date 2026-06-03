/*
 * contrib/btree_gist/btree_int2.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "utils/rel.h"
#include "utils/sortsupport.h"

typedef struct int16key
{
	int16		lower;
	int16		upper;
} int16KEY;

/* GiST support functions */
PG_FUNCTION_INFO_V1(gbt_int2_compress);
PG_FUNCTION_INFO_V1(gbt_int2_fetch);
PG_FUNCTION_INFO_V1(gbt_int2_union);
PG_FUNCTION_INFO_V1(gbt_int2_picksplit);
PG_FUNCTION_INFO_V1(gbt_int2_consistent);
PG_FUNCTION_INFO_V1(gbt_int2_distance);
PG_FUNCTION_INFO_V1(gbt_int2_penalty);
PG_FUNCTION_INFO_V1(gbt_int2_same);
PG_FUNCTION_INFO_V1(gbt_int2_sortsupport);


static bool
gbt_int2gt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) > *((const int16 *) b));
}
static bool
gbt_int2ge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) >= *((const int16 *) b));
}
static bool
gbt_int2eq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) == *((const int16 *) b));
}
static bool
gbt_int2le(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) <= *((const int16 *) b));
}
static bool
gbt_int2lt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return (*((const int16 *) a) < *((const int16 *) b));
}

static int
gbt_int2key_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	int16KEY   *ia = (int16KEY *) (((const Nsrt *) a)->t);
	int16KEY   *ib = (int16KEY *) (((const Nsrt *) b)->t);

	if (ia->lower == ib->lower)
	{
		if (ia->upper == ib->upper)
			return 0;

		return (ia->upper > ib->upper) ? 1 : -1;
	}

	return (ia->lower > ib->lower) ? 1 : -1;
}

static float8
gbt_int2_dist(const void *a, const void *b, FmgrInfo *flinfo)
{
	return GET_FLOAT_DISTANCE(int16, a, b);
}

static const gbtree_ninfo tinfo =
{
	gbt_t_int2,
	sizeof(int16),
	4,							/* sizeof(gbtreekey4) */
	gbt_int2gt,
	gbt_int2ge,
	gbt_int2eq,
	gbt_int2le,
	gbt_int2lt,
	gbt_int2key_cmp,
	gbt_int2_dist
};

/*
 * Cross-type GiST callbacks: the indexed key is int2, the query is int4 or
 * int8.  Both reuse gbt_num_consistent()/gbt_num_distance() via a tinfo whose
 * comparison/distance callbacks read the query (left) and key (right) sides at
 * their own widths.  f_cmp is unused on these paths and left NULL.
 */
GBT_INT_CMP_FNS(gbt_int2_q4_, int32, int16)
GBT_INT_CMP_FNS(gbt_int2_q8_, int64, int16)

static const gbtree_ninfo tinfo_q4 =
{
	gbt_t_int2,
	sizeof(int16),
	4,
	gbt_int2_q4_gt,
	gbt_int2_q4_ge,
	gbt_int2_q4_eq,
	gbt_int2_q4_le,
	gbt_int2_q4_lt,
	NULL,
	gbt_int2_q4_dist
};

static const gbtree_ninfo tinfo_q8 =
{
	gbt_t_int2,
	sizeof(int16),
	4,
	gbt_int2_q8_gt,
	gbt_int2_q8_ge,
	gbt_int2_q8_eq,
	gbt_int2_q8_le,
	gbt_int2_q8_lt,
	NULL,
	gbt_int2_q8_dist
};

/*
 * Cross-type dispatch shared by gbt_int2_consistent and gbt_int2_distance:
 * select the tinfo for the query subtype and read the query value at its own
 * width into caller-owned storage.
 */
static const gbtree_ninfo *
gbt_int2_crosstype(Oid subtype, Datum d, gbt_intkey *q, const void **qp)
{
	switch (subtype)
	{
		case InvalidOid:		/* same-type: exclusion/temporal constraint
								 * checks pass the native type with subtype 0 */
		case INT2OID:
			q->i2 = DatumGetInt16(d);
			*qp = &q->i2;
			return &tinfo;
		case INT4OID:
			q->i4 = DatumGetInt32(d);
			*qp = &q->i4;
			return &tinfo_q4;
		case INT8OID:
			q->i8 = DatumGetInt64(d);
			*qp = &q->i8;
			return &tinfo_q8;
		default:
			elog(ERROR, "unrecognized subtype %u for btree_gist int2 cross-type comparison",
				 subtype);
			return NULL;		/* keep compiler quiet */
	}
}


PG_FUNCTION_INFO_V1(int2_dist);
Datum
int2_dist(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int16		b = PG_GETARG_INT16(1);
	int16		r;
	int16		ra;

	if (pg_sub_s16_overflow(a, b, &r) ||
		r == PG_INT16_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	ra = abs(r);

	PG_RETURN_INT16(ra);
}

PG_FUNCTION_INFO_V1(int2_int4_dist);
Datum
int2_int4_dist(PG_FUNCTION_ARGS)
{
	int32		a = (int32) PG_GETARG_INT16(0);
	int32		b = PG_GETARG_INT32(1);
	int32		r;

	if (pg_sub_s32_overflow(a, b, &r) ||
		r == PG_INT32_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32(abs(r));
}

PG_FUNCTION_INFO_V1(int2_int8_dist);
Datum
int2_int8_dist(PG_FUNCTION_ARGS)
{
	int64		a = (int64) PG_GETARG_INT16(0);
	int64		b = PG_GETARG_INT64(1);
	int64		r;

	if (pg_sub_s64_overflow(a, b, &r) ||
		r == PG_INT64_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64(i64abs(r));
}


/**************************************************
 * GiST support functions
 **************************************************/

Datum
gbt_int2_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_compress(entry, &tinfo));
}

Datum
gbt_int2_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_int2_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		queryDatum = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	Oid			subtype = PG_GETARG_OID(3);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int16KEY   *kkk = (int16KEY *) DatumGetPointer(entry->key);
	const gbtree_ninfo *ti;
	gbt_intkey	query;
	const void *qp;
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	ti = gbt_int2_crosstype(subtype, queryDatum, &query, &qp);

	PG_RETURN_BOOL(gbt_num_consistent(&key, qp, strategy, GIST_LEAF(entry),
									  ti, fcinfo->flinfo));
}

Datum
gbt_int2_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		queryDatum = PG_GETARG_DATUM(1);
	Oid			subtype = PG_GETARG_OID(3);
	int16KEY   *kkk = (int16KEY *) DatumGetPointer(entry->key);
	const gbtree_ninfo *ti;
	gbt_intkey	query;
	const void *qp;
	GBT_NUMKEY_R key;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	ti = gbt_int2_crosstype(subtype, queryDatum, &query, &qp);

	PG_RETURN_FLOAT8(gbt_num_distance(&key, qp, GIST_LEAF(entry),
									  ti, fcinfo->flinfo));
}

Datum
gbt_int2_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc_object(int16KEY);

	*(int *) PG_GETARG_POINTER(1) = sizeof(int16KEY);
	PG_RETURN_POINTER(gbt_num_union(out, entryvec, &tinfo, fcinfo->flinfo));
}

Datum
gbt_int2_penalty(PG_FUNCTION_ARGS)
{
	int16KEY   *origentry = (int16KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	int16KEY   *newentry = (int16KEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	penalty_num(result, origentry->lower, origentry->upper, newentry->lower, newentry->upper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_int2_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_int2_same(PG_FUNCTION_ARGS)
{
	int16KEY   *b1 = (int16KEY *) PG_GETARG_POINTER(0);
	int16KEY   *b2 = (int16KEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_int2_ssup_cmp(Datum x, Datum y, SortSupport ssup)
{
	int16KEY   *arg1 = (int16KEY *) DatumGetPointer(x);
	int16KEY   *arg2 = (int16KEY *) DatumGetPointer(y);

	/* for leaf items we expect lower == upper, so only compare lower */
	if (arg1->lower < arg2->lower)
		return -1;
	else if (arg1->lower > arg2->lower)
		return 1;
	else
		return 0;
}

Datum
gbt_int2_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_int2_ssup_cmp;
	PG_RETURN_VOID();
}
