/*-------------------------------------------------------------------------
 *
 * multirangetypes_selfuncs.c
 *	  Functions for selectivity estimation of multirange operators
 *
 * Estimates are based on histograms of lower and upper bounds, and the
 * fraction of empty multiranges.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/multirangetypes_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/multirangetypes.h"
#include "utils/rangetypes.h"
#include "utils/rangetypes_selfuncs.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"

static double calc_multirangesel(TypeCacheEntry *typcache,
								 VariableStatData *vardata,
								 const MultirangeType *constval, Oid operator);
static double default_multirange_selectivity(Oid operator);
static double calc_hist_selectivity(TypeCacheEntry *typcache,
									VariableStatData *vardata,
									const MultirangeType *constval,
									Oid operator);

/*
 * Returns a default selectivity estimate for given operator, when we don't
 * have statistics or cannot use them for some reason.
 */
static double
default_multirange_selectivity(Oid operator)
{
	switch (operator)
	{
		case OID_MULTIRANGE_OVERLAPS_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RANGE_OP:
		case OID_RANGE_OVERLAPS_MULTIRANGE_OP:
			return 0.01;

		case OID_RANGE_CONTAINS_MULTIRANGE_OP:
		case OID_RANGE_MULTIRANGE_CONTAINED_OP:
		case OID_MULTIRANGE_CONTAINS_RANGE_OP:
		case OID_MULTIRANGE_CONTAINS_MULTIRANGE_OP:
		case OID_MULTIRANGE_RANGE_CONTAINED_OP:
		case OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP:
			return 0.005;

		case OID_MULTIRANGE_CONTAINS_ELEM_OP:
		case OID_MULTIRANGE_ELEM_CONTAINED_OP:

			/*
			 * "multirange @> elem" is more or less identical to a scalar
			 * inequality "A >= b AND A <= c".
			 */
			return DEFAULT_MULTIRANGE_INEQ_SEL;

		case OID_MULTIRANGE_LESS_OP:
		case OID_MULTIRANGE_LESS_EQUAL_OP:
		case OID_MULTIRANGE_GREATER_OP:
		case OID_MULTIRANGE_GREATER_EQUAL_OP:
		case OID_MULTIRANGE_LEFT_RANGE_OP:
		case OID_MULTIRANGE_LEFT_MULTIRANGE_OP:
		case OID_RANGE_LEFT_MULTIRANGE_OP:
		case OID_MULTIRANGE_RIGHT_RANGE_OP:
		case OID_MULTIRANGE_RIGHT_MULTIRANGE_OP:
		case OID_RANGE_RIGHT_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_LEFT_RANGE_OP:
		case OID_RANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RIGHT_RANGE_OP:
		case OID_RANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
			/* these are similar to regular scalar inequalities */
			return DEFAULT_INEQ_SEL;

		default:

			/*
			 * all multirange operators should be handled above, but just in
			 * case
			 */
			return 0.01;
	}
}

/*
 * multirangesel -- restriction selectivity for multirange operators
 */
Datum
multirangesel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity selec;
	TypeCacheEntry *typcache = NULL;
	MultirangeType *constmultirange = NULL;
	RangeType  *constrange = NULL;

	/*
	 * If expression is not (variable op something) or (something op
	 * variable), then punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(default_multirange_selectivity(operator));

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(default_multirange_selectivity(operator));
	}

	/*
	 * All the multirange operators are strict, so we can cope with a NULL
	 * constant right away.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/*
	 * If var is on the right, commute the operator, so that we can assume the
	 * var is on the left in what follows.
	 */
	if (!varonleft)
	{
		/* we have other Op var, commute to make var Op other */
		operator = get_commutator(operator);
		if (!operator)
		{
			/* Use default selectivity (should we raise an error instead?) */
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(default_multirange_selectivity(operator));
		}
	}

	/*
	 * OK, there's a Var and a Const we're dealing with here.  We need the
	 * Const to be of same multirange type as the column, else we can't do
	 * anything useful. (Such cases will likely fail at runtime, but here we'd
	 * rather just return a default estimate.)
	 *
	 * If the operator is "multirange @> element", the constant should be of
	 * the element type of the multirange column. Convert it to a multirange
	 * that includes only that single point, so that we don't need special
	 * handling for that in what follows.
	 */
	if (operator == OID_MULTIRANGE_CONTAINS_ELEM_OP)
	{
		typcache = multirange_get_typcache(fcinfo, vardata.vartype);

		if (((Const *) other)->consttype == typcache->rngtype->rngelemtype->type_id)
		{
			RangeBound	lower,
						upper;

			lower.inclusive = true;
			lower.val = ((Const *) other)->constvalue;
			lower.infinite = false;
			lower.lower = true;
			upper.inclusive = true;
			upper.val = ((Const *) other)->constvalue;
			upper.infinite = false;
			upper.lower = false;
			constrange = range_serialize(typcache->rngtype, &lower, &upper,
										 false, NULL);
			constmultirange = make_multirange(typcache->type_id, typcache->rngtype,
											  1, &constrange);
		}
	}
	else if (operator == OID_RANGE_MULTIRANGE_CONTAINED_OP ||
			 operator == OID_MULTIRANGE_CONTAINS_RANGE_OP ||
			 operator == OID_MULTIRANGE_OVERLAPS_RANGE_OP ||
			 operator == OID_MULTIRANGE_OVERLAPS_LEFT_RANGE_OP ||
			 operator == OID_MULTIRANGE_OVERLAPS_RIGHT_RANGE_OP ||
			 operator == OID_MULTIRANGE_LEFT_RANGE_OP ||
			 operator == OID_MULTIRANGE_RIGHT_RANGE_OP)
	{
		/*
		 * Promote a range in "multirange OP range" just like we do an element
		 * in "multirange OP element".
		 */
		typcache = multirange_get_typcache(fcinfo, vardata.vartype);
		if (((Const *) other)->consttype == typcache->rngtype->type_id)
		{
			constrange = DatumGetRangeTypeP(((Const *) other)->constvalue);
			constmultirange = make_multirange(typcache->type_id, typcache->rngtype,
											  1, &constrange);
		}
	}
	else if (operator == OID_RANGE_OVERLAPS_MULTIRANGE_OP ||
			 operator == OID_RANGE_OVERLAPS_LEFT_MULTIRANGE_OP ||
			 operator == OID_RANGE_OVERLAPS_RIGHT_MULTIRANGE_OP ||
			 operator == OID_RANGE_LEFT_MULTIRANGE_OP ||
			 operator == OID_RANGE_RIGHT_MULTIRANGE_OP ||
			 operator == OID_RANGE_CONTAINS_MULTIRANGE_OP ||
			 operator == OID_MULTIRANGE_ELEM_CONTAINED_OP ||
			 operator == OID_MULTIRANGE_RANGE_CONTAINED_OP)
	{
		/*
		 * Here, the Var is the elem/range, not the multirange.  For now we
		 * just punt and return the default estimate.  In future we could
		 * disassemble the multirange constant to do something more
		 * intelligent.
		 */
	}
	else if (((Const *) other)->consttype == vardata.vartype)
	{
		/* Both sides are the same multirange type */
		typcache = multirange_get_typcache(fcinfo, vardata.vartype);

		constmultirange = DatumGetMultirangeTypeP(((Const *) other)->constvalue);
	}

	/*
	 * If we got a valid constant on one side of the operator, proceed to
	 * estimate using statistics. Otherwise punt and return a default constant
	 * estimate.  Note that calc_multirangesel need not handle
	 * OID_MULTIRANGE_*_CONTAINED_OP.
	 */
	if (constmultirange)
		selec = calc_multirangesel(typcache, &vardata, constmultirange, operator);
	else
		selec = default_multirange_selectivity(operator);

	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

static double
calc_multirangesel(TypeCacheEntry *typcache, VariableStatData *vardata,
				   const MultirangeType *constval, Oid operator)
{
	double		hist_selec;
	double		selec;
	float4		empty_frac,
				null_frac;

	/*
	 * First look up the fraction of NULLs and empty multiranges from
	 * pg_statistic.
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;
		AttStatsSlot sslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		null_frac = stats->stanullfrac;

		/* Try to get fraction of empty multiranges */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
							 InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers != 1)
				elog(ERROR, "invalid empty fraction statistic");	/* shouldn't happen */
			empty_frac = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
		else
		{
			/* No empty fraction statistic. Assume no empty ranges. */
			empty_frac = 0.0;
		}
	}
	else
	{
		/*
		 * No stats are available. Follow through the calculations below
		 * anyway, assuming no NULLs and no empty multiranges. This still
		 * allows us to give a better-than-nothing estimate based on whether
		 * the constant is an empty multirange or not.
		 */
		null_frac = 0.0;
		empty_frac = 0.0;
	}

	if (MultirangeIsEmpty(constval))
	{
		/*
		 * An empty multirange matches all multiranges, all empty multiranges,
		 * or nothing, depending on the operator
		 */
		switch (operator)
		{
				/* these return false if either argument is empty */
			case OID_MULTIRANGE_OVERLAPS_RANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_MULTIRANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_LEFT_RANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_RIGHT_RANGE_OP:
			case OID_MULTIRANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
			case OID_MULTIRANGE_LEFT_RANGE_OP:
			case OID_MULTIRANGE_LEFT_MULTIRANGE_OP:
			case OID_MULTIRANGE_RIGHT_RANGE_OP:
			case OID_MULTIRANGE_RIGHT_MULTIRANGE_OP:
				/* nothing is less than an empty multirange */
			case OID_MULTIRANGE_LESS_OP:
				selec = 0.0;
				break;

				/*
				 * only empty multiranges can be contained by an empty
				 * multirange
				 */
			case OID_RANGE_MULTIRANGE_CONTAINED_OP:
			case OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP:
				/* only empty ranges are <= an empty multirange */
			case OID_MULTIRANGE_LESS_EQUAL_OP:
				selec = empty_frac;
				break;

				/* everything contains an empty multirange */
			case OID_MULTIRANGE_CONTAINS_RANGE_OP:
			case OID_MULTIRANGE_CONTAINS_MULTIRANGE_OP:
				/* everything is >= an empty multirange */
			case OID_MULTIRANGE_GREATER_EQUAL_OP:
				selec = 1.0;
				break;

				/* all non-empty multiranges are > an empty multirange */
			case OID_MULTIRANGE_GREATER_OP:
				selec = 1.0 - empty_frac;
				break;

				/* an element cannot be empty */
			case OID_MULTIRANGE_CONTAINS_ELEM_OP:

				/* filtered out by multirangesel() */
			case OID_RANGE_OVERLAPS_MULTIRANGE_OP:
			case OID_RANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
			case OID_RANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
			case OID_RANGE_LEFT_MULTIRANGE_OP:
			case OID_RANGE_RIGHT_MULTIRANGE_OP:
			case OID_RANGE_CONTAINS_MULTIRANGE_OP:
			case OID_MULTIRANGE_ELEM_CONTAINED_OP:
			case OID_MULTIRANGE_RANGE_CONTAINED_OP:

			default:
				elog(ERROR, "unexpected operator %u", operator);
				selec = 0.0;	/* keep compiler quiet */
				break;
		}
	}
	else
	{
		/*
		 * Calculate selectivity using bound histograms. If that fails for
		 * some reason, e.g no histogram in pg_statistic, use the default
		 * constant estimate for the fraction of non-empty values. This is
		 * still somewhat better than just returning the default estimate,
		 * because this still takes into account the fraction of empty and
		 * NULL tuples, if we had statistics for them.
		 */
		hist_selec = calc_hist_selectivity(typcache, vardata, constval,
										   operator);
		if (hist_selec < 0.0)
			hist_selec = default_multirange_selectivity(operator);

		/*
		 * Now merge the results for the empty multiranges and histogram
		 * calculations, realizing that the histogram covers only the
		 * non-null, non-empty values.
		 */
		if (operator == OID_RANGE_MULTIRANGE_CONTAINED_OP ||
			operator == OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP)
		{
			/* empty is contained by anything non-empty */
			selec = (1.0 - empty_frac) * hist_selec + empty_frac;
		}
		else
		{
			/* with any other operator, empty Op non-empty matches nothing */
			selec = (1.0 - empty_frac) * hist_selec;
		}
	}

	/* all multirange operators are strict */
	selec *= (1.0 - null_frac);

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * Calculate multirange operator selectivity using histograms of multirange bounds.
 *
 * This estimate is for the portion of values that are not empty and not
 * NULL.
 */
static double
calc_hist_selectivity(TypeCacheEntry *typcache, VariableStatData *vardata,
					  const MultirangeType *constval, Oid operator)
{
	TypeCacheEntry *rng_typcache = typcache->rngtype;
	AttStatsSlot hslot;
	AttStatsSlot lslot;
	int			nhist;
	RangeBound *hist_lower;
	RangeBound *hist_upper;
	int			i;
	RangeBound	const_lower;
	RangeBound	const_upper;
	RangeBound	tmp;
	double		hist_selec;

	/* Can't use the histogram with insecure multirange support functions */
	if (!statistic_proc_security_check(vardata,
									   rng_typcache->rng_cmp_proc_finfo.fn_oid))
		return -1;
	if (OidIsValid(rng_typcache->rng_subdiff_finfo.fn_oid) &&
		!statistic_proc_security_check(vardata,
									   rng_typcache->rng_subdiff_finfo.fn_oid))
		return -1;

	/* Try to get histogram of ranges */
	if (!(HeapTupleIsValid(vardata->statsTuple) &&
		  get_attstatsslot(&hslot, vardata->statsTuple,
						   STATISTIC_KIND_BOUNDS_HISTOGRAM, InvalidOid,
						   ATTSTATSSLOT_VALUES)))
		return -1.0;

	/* check that it's a histogram, not just a dummy entry */
	if (hslot.nvalues < 2)
	{
		free_attstatsslot(&hslot);
		return -1.0;
	}

	/*
	 * Convert histogram of ranges into histograms of its lower and upper
	 * bounds.
	 */
	nhist = hslot.nvalues;
	hist_lower = palloc_array(RangeBound, nhist);
	hist_upper = palloc_array(RangeBound, nhist);
	for (i = 0; i < nhist; i++)
	{
		bool		empty;

		range_deserialize(rng_typcache, DatumGetRangeTypeP(hslot.values[i]),
						  &hist_lower[i], &hist_upper[i], &empty);
		/* The histogram should not contain any empty ranges */
		if (empty)
			elog(ERROR, "bounds histogram contains an empty range");
	}

	/* @> and @< also need a histogram of range lengths */
	if (operator == OID_MULTIRANGE_CONTAINS_RANGE_OP ||
		operator == OID_MULTIRANGE_CONTAINS_MULTIRANGE_OP ||
		operator == OID_MULTIRANGE_RANGE_CONTAINED_OP ||
		operator == OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP)
	{
		if (!(HeapTupleIsValid(vardata->statsTuple) &&
			  get_attstatsslot(&lslot, vardata->statsTuple,
							   STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
							   InvalidOid,
							   ATTSTATSSLOT_VALUES)))
		{
			free_attstatsslot(&hslot);
			return -1.0;
		}

		/* check that it's a histogram, not just a dummy entry */
		if (lslot.nvalues < 2)
		{
			free_attstatsslot(&lslot);
			free_attstatsslot(&hslot);
			return -1.0;
		}
	}
	else
		memset(&lslot, 0, sizeof(lslot));

	/* Extract the bounds of the constant value. */
	Assert(constval->rangeCount > 0);
	multirange_get_bounds(rng_typcache, constval, 0,
						  &const_lower, &tmp);
	multirange_get_bounds(rng_typcache, constval, constval->rangeCount - 1,
						  &tmp, &const_upper);

	/*
	 * Calculate selectivity comparing the lower or upper bound of the
	 * constant with the histogram of lower or upper bounds.
	 */
	switch (operator)
	{
		case OID_MULTIRANGE_LESS_OP:

			/*
			 * The regular b-tree comparison operators (<, <=, >, >=) compare
			 * the lower bounds first, and the upper bounds for values with
			 * equal lower bounds. Estimate that by comparing the lower bounds
			 * only. This gives a fairly accurate estimate assuming there
			 * aren't many rows with a lower bound equal to the constant's
			 * lower bound.
			 */
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache, &const_lower,
											 hist_lower, nhist, false);
			break;

		case OID_MULTIRANGE_LESS_EQUAL_OP:
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache, &const_lower,
											 hist_lower, nhist, true);
			break;

		case OID_MULTIRANGE_GREATER_OP:
			hist_selec =
				1 - calc_hist_selectivity_scalar(rng_typcache, &const_lower,
												 hist_lower, nhist, false);
			break;

		case OID_MULTIRANGE_GREATER_EQUAL_OP:
			hist_selec =
				1 - calc_hist_selectivity_scalar(rng_typcache, &const_lower,
												 hist_lower, nhist, true);
			break;

		case OID_MULTIRANGE_LEFT_RANGE_OP:
		case OID_MULTIRANGE_LEFT_MULTIRANGE_OP:
			/* var << const when upper(var) < lower(const) */
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache, &const_lower,
											 hist_upper, nhist, false);
			break;

		case OID_MULTIRANGE_RIGHT_RANGE_OP:
		case OID_MULTIRANGE_RIGHT_MULTIRANGE_OP:
			/* var >> const when lower(var) > upper(const) */
			hist_selec =
				1 - calc_hist_selectivity_scalar(rng_typcache, &const_upper,
												 hist_lower, nhist, true);
			break;

		case OID_MULTIRANGE_OVERLAPS_RIGHT_RANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
			/* compare lower bounds */
			hist_selec =
				1 - calc_hist_selectivity_scalar(rng_typcache, &const_lower,
												 hist_lower, nhist, false);
			break;

		case OID_MULTIRANGE_OVERLAPS_LEFT_RANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
			/* compare upper bounds */
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache, &const_upper,
											 hist_upper, nhist, true);
			break;

		case OID_MULTIRANGE_OVERLAPS_RANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_MULTIRANGE_OP:
		case OID_MULTIRANGE_CONTAINS_ELEM_OP:

			/*
			 * A && B <=> NOT (A << B OR A >> B).
			 *
			 * Since A << B and A >> B are mutually exclusive events we can
			 * sum their probabilities to find probability of (A << B OR A >>
			 * B).
			 *
			 * "multirange @> elem" is equivalent to "multirange &&
			 * {[elem,elem]}". The caller already constructed the singular
			 * range from the element constant, so just treat it the same as
			 * &&.
			 */
			hist_selec =
				calc_hist_selectivity_scalar(rng_typcache,
											 &const_lower, hist_upper,
											 nhist, false);
			hist_selec +=
				(1.0 - calc_hist_selectivity_scalar(rng_typcache,
													&const_upper, hist_lower,
													nhist, true));
			hist_selec = 1.0 - hist_selec;
			break;

		case OID_MULTIRANGE_CONTAINS_RANGE_OP:
		case OID_MULTIRANGE_CONTAINS_MULTIRANGE_OP:
			hist_selec =
				calc_hist_selectivity_contains(rng_typcache, &const_lower,
											   &const_upper, hist_lower, nhist,
											   lslot.values, lslot.nvalues);
			break;

		case OID_MULTIRANGE_MULTIRANGE_CONTAINED_OP:
		case OID_RANGE_MULTIRANGE_CONTAINED_OP:
			if (const_lower.infinite)
			{
				/*
				 * Lower bound no longer matters. Just estimate the fraction
				 * with an upper bound <= const upper bound
				 */
				hist_selec =
					calc_hist_selectivity_scalar(rng_typcache, &const_upper,
												 hist_upper, nhist, true);
			}
			else if (const_upper.infinite)
			{
				hist_selec =
					1.0 - calc_hist_selectivity_scalar(rng_typcache, &const_lower,
													   hist_lower, nhist, false);
			}
			else
			{
				hist_selec =
					calc_hist_selectivity_contained(rng_typcache, &const_lower,
													&const_upper, hist_lower, nhist,
													lslot.values, lslot.nvalues);
			}
			break;

			/* filtered out by multirangesel() */
		case OID_RANGE_OVERLAPS_MULTIRANGE_OP:
		case OID_RANGE_OVERLAPS_LEFT_MULTIRANGE_OP:
		case OID_RANGE_OVERLAPS_RIGHT_MULTIRANGE_OP:
		case OID_RANGE_LEFT_MULTIRANGE_OP:
		case OID_RANGE_RIGHT_MULTIRANGE_OP:
		case OID_RANGE_CONTAINS_MULTIRANGE_OP:
		case OID_MULTIRANGE_ELEM_CONTAINED_OP:
		case OID_MULTIRANGE_RANGE_CONTAINED_OP:

		default:
			elog(ERROR, "unknown multirange operator %u", operator);
			hist_selec = -1.0;	/* keep compiler quiet */
			break;
	}

	free_attstatsslot(&lslot);
	free_attstatsslot(&hslot);

	return hist_selec;
}

/*
 * multirangejoinsel -- join selectivity for multirange operators
 *
 * Supports: <<, >>, && for all type combinations:
 *   multirange vs multirange, multirange vs range, range vs multirange
 *
 * These operators map directly to strict bound comparisons P(X < Y),
 * which calc_hist_join_selectivity() estimates from bound histograms.
 * Both range and multirange types store bound histograms in the same
 * format, so the estimation is identical regardless of type combination.
 */
Datum
multirangejoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	VariableStatData vardata1;
	VariableStatData vardata2;
	Selectivity selec;
	AttStatsSlot hist1;
	AttStatsSlot hist2;
	AttStatsSlot sslot;
	bool		have_hist1 = false;
	bool		have_hist2 = false;
	TypeCacheEntry *typcache;
	TypeCacheEntry *rng_typcache;
	Form_pg_statistic stats1;
	Form_pg_statistic stats2;
	double		empty_frac1;
	double		empty_frac2;
	double		null_frac1;
	double		null_frac2;
	int			nhist1;
	int			nhist2;
	RangeBound *hist1_lower;
	RangeBound *hist1_upper;
	RangeBound *hist2_lower;
	RangeBound *hist2_upper;
	bool		join_is_reversed;
	bool		empty;
	int			i;

	get_join_variables(root, args, sjinfo, &vardata1, &vardata2,
					   &join_is_reversed);

	selec = default_multirange_selectivity(operator);

	/*
	 * Acquire histogram stats for both sides.  Each slot is tracked
	 * independently so we can release exactly what was acquired on any
	 * failure path.
	 */
	if (!HeapTupleIsValid(vardata1.statsTuple) ||
		!HeapTupleIsValid(vardata2.statsTuple))
		goto cleanup;

	memset(&hist1, 0, sizeof(hist1));
	memset(&hist2, 0, sizeof(hist2));

	if (!get_attstatsslot(&hist1, vardata1.statsTuple,
						  STATISTIC_KIND_BOUNDS_HISTOGRAM, InvalidOid,
						  ATTSTATSSLOT_VALUES))
		goto cleanup;
	have_hist1 = true;

	if (!get_attstatsslot(&hist2, vardata2.statsTuple,
						  STATISTIC_KIND_BOUNDS_HISTOGRAM, InvalidOid,
						  ATTSTATSSLOT_VALUES))
		goto cleanup;
	have_hist2 = true;

	/*
	 * Determine the range type cache for bound comparisons.  At least one
	 * side is a multirange type; try vardata1 first, then vardata2.
	 */
	typcache = lookup_type_cache(vardata1.vartype, TYPECACHE_MULTIRANGE_INFO);
	if (typcache->rngtype != NULL)
		rng_typcache = typcache->rngtype;
	else
	{
		typcache = lookup_type_cache(vardata2.vartype,
									 TYPECACHE_MULTIRANGE_INFO);
		rng_typcache = typcache->rngtype;
	}

	/* Look up NULL and empty fractions */
	stats1 = (Form_pg_statistic) GETSTRUCT(vardata1.statsTuple);
	stats2 = (Form_pg_statistic) GETSTRUCT(vardata2.statsTuple);

	null_frac1 = stats1->stanullfrac;
	null_frac2 = stats2->stanullfrac;

	/* Try to get empty fraction for the first variable */
	if (get_attstatsslot(&sslot, vardata1.statsTuple,
						 STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
						 InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		if (sslot.nnumbers != 1)
			elog(ERROR, "invalid empty fraction statistic");
		empty_frac1 = sslot.numbers[0];
		free_attstatsslot(&sslot);
	}
	else
	{
		empty_frac1 = 0.0;
	}

	/* Try to get empty fraction for the second variable */
	if (get_attstatsslot(&sslot, vardata2.statsTuple,
						 STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM,
						 InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		if (sslot.nnumbers != 1)
			elog(ERROR, "invalid empty fraction statistic");
		empty_frac2 = sslot.numbers[0];
		free_attstatsslot(&sslot);
	}
	else
	{
		empty_frac2 = 0.0;
	}

	/* Convert bound histograms to separate lower/upper bound arrays */
	nhist1 = hist1.nvalues;
	hist1_lower = (RangeBound *) palloc(sizeof(RangeBound) * nhist1);
	hist1_upper = (RangeBound *) palloc(sizeof(RangeBound) * nhist1);
	for (i = 0; i < nhist1; i++)
	{
		range_deserialize(rng_typcache, DatumGetRangeTypeP(hist1.values[i]),
						  &hist1_lower[i], &hist1_upper[i], &empty);
		if (empty)
			elog(ERROR, "bounds histogram contains an empty range");
	}

	nhist2 = hist2.nvalues;
	hist2_lower = (RangeBound *) palloc(sizeof(RangeBound) * nhist2);
	hist2_upper = (RangeBound *) palloc(sizeof(RangeBound) * nhist2);
	for (i = 0; i < nhist2; i++)
	{
		range_deserialize(rng_typcache, DatumGetRangeTypeP(hist2.values[i]),
						  &hist2_lower[i], &hist2_upper[i], &empty);
		if (empty)
			elog(ERROR, "bounds histogram contains an empty range");
	}

	/* Estimate selectivity based on the operator */
	switch (operator)
	{
		case OID_RANGE_OVERLAPS_MULTIRANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_RANGE_OP:
		case OID_MULTIRANGE_OVERLAPS_MULTIRANGE_OP:

			/*
			 * A && B iff NOT(A << B) AND NOT(A >> B) = 1 - P(A.upper <
			 * B.lower) - P(B.upper < A.lower)
			 *
			 * This decomposition is exact for single ranges.  For
			 * multiranges, the bound histograms only represent the outermost
			 * lower and upper bounds (see multirange_typanalyze), so internal
			 * gaps are not captured. This can overestimate overlap for sparse
			 * multiranges, but is consistent with how existing restriction
			 * selectivity handles multirange &&.
			 */
			selec = 1;
			selec -= calc_hist_join_selectivity(rng_typcache,
												hist1_upper, nhist1,
												hist2_lower, nhist2);
			selec -= calc_hist_join_selectivity(rng_typcache,
												hist2_upper, nhist2,
												hist1_lower, nhist1);
			break;

		case OID_RANGE_LEFT_MULTIRANGE_OP:
		case OID_MULTIRANGE_LEFT_RANGE_OP:
		case OID_MULTIRANGE_LEFT_MULTIRANGE_OP:
			/* A << B iff upper(A) < lower(B) */
			selec = calc_hist_join_selectivity(rng_typcache,
											   hist1_upper, nhist1,
											   hist2_lower, nhist2);
			break;

		case OID_RANGE_RIGHT_MULTIRANGE_OP:
		case OID_MULTIRANGE_RIGHT_RANGE_OP:
		case OID_MULTIRANGE_RIGHT_MULTIRANGE_OP:
			/* A >> B iff upper(B) < lower(A) */
			selec = calc_hist_join_selectivity(rng_typcache,
											   hist2_upper, nhist2,
											   hist1_lower, nhist1);
			break;

		default:
			/* Unsupported operator; keep the default selectivity */
			goto cleanup;
	}

	/* The histogram-based selectivity applies to non-empty values only */
	selec *= (1 - empty_frac1) * (1 - empty_frac2);

	/*
	 * For the supported operators (<<, >>, &&), empty values always produce
	 * false, so no empty-fraction adjustment is needed.
	 */

	/* All multirange operators are strict */
	selec *= (1 - null_frac1) * (1 - null_frac2);

cleanup:
	if (have_hist2)
		free_attstatsslot(&hist2);
	if (have_hist1)
		free_attstatsslot(&hist1);

	ReleaseVariableStats(vardata1);
	ReleaseVariableStats(vardata2);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}
