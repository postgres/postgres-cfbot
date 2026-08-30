#include "postgres.h"

#include <math.h>

#include "c.h"
#include "catalog/pg_type.h"
#include "nodes/miscnodes.h"
#include "nodes/primnodes.h"
#include "nodes/supportnodes.h"
#include "parser/scansup.h"
#include "pgtime.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/datetime.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "varatt.h"

/**
 * Extended numeric sign, the usual -1, 0, 1,
 * for real numbers, and additional values for non-real
 * numeric values and null.
*/

typedef enum SLOPE_SIGN
{
	SLOPE_SIGN_NINF = -2,
	SLOPE_SIGN_NEG = -1,
	SLOPE_SIGN_ZERO = 0,
	SLOPE_SIGN_POS = 1,
	SLOPE_SIGN_PINF = 2,
	SLOPE_SIGN_NAN = 3,
	SLOPE_SIGN_NULL = 4,
}			SLOPE_SIGN;

#define SLOPE_REQUEST(req) \
	SupportRequestMonotonic *req; \
	req = (SupportRequestMonotonic*) PG_GETARG_POINTER(0); \
	if (!IsA(req, SupportRequestMonotonic)) \
		PG_RETURN_POINTER(NULL)

#define SLOPE_REQUEST_ARGS(req, args, n) \
	List *args; \
	SLOPE_REQUEST(req); \
	args = get_arg_list(req); \
	if(list_length(args) < n) \
		PG_RETURN_POINTER(NULL)

#define SLOPE_REQUEST_2ARGS(req, arg0, arg1) \
	Node *arg0; \
	Node *arg1; \
	SLOPE_REQUEST_ARGS(req, args, 2); \
	arg0 = linitial(args); \
	arg1 = lsecond(args); \
	if(!arg0 || !arg1) \
		PG_RETURN_POINTER(NULL)

/* Static variables returned by prosupport below */
static const MonotonicFunction asc_slope[2] = {MONOTONICFUNC_INCREASING, MONOTONICFUNC_INCREASING};
static const MonotonicFunction desc_slope[2] = {MONOTONICFUNC_DECREASING, MONOTONICFUNC_DECREASING};
static const MonotonicFunction flat_slope[2] = {MONOTONICFUNC_BOTH, MONOTONICFUNC_BOTH};
static const MonotonicFunction diff_slope[2] = {MONOTONICFUNC_INCREASING, MONOTONICFUNC_DECREASING};
static const MonotonicFunction asc1_slope[2] = {MONOTONICFUNC_NONE, MONOTONICFUNC_INCREASING};
static const MonotonicFunction asc0_slope[1] = {MONOTONICFUNC_INCREASING};
static const MonotonicFunction desc0_slope[1] = {MONOTONICFUNC_DECREASING};


/*
 * monotonic_slope_support
 *		Generic helper for prosupport functions that declare monotonic slopes.
 *
 * If the request is a SupportRequestMonotonic, fills in nslopes and slopes
 * and returns the request pointer.  Otherwise returns NULL.
 */
static Datum
monotonic_slope_support(SupportRequestMonotonic *req, int nslopes,
						const MonotonicFunction *slopes)
{
	req->nslopes = nslopes;
	req->slopes = slopes;
	return PointerGetDatum(req);
}

static List *
get_arg_list(SupportRequestMonotonic *req)
{
	if (IsA(req->expr, FuncExpr))
		return ((FuncExpr *) req->expr)->args;
	else if (IsA(req->expr, OpExpr))
		return ((OpExpr *) req->expr)->args;
	else
		return NULL;
}

/*
 * get_const_sign
 *  Helper to determine the sign of a numeric constant.
 *  It will classify a number according
 */
static inline SLOPE_SIGN
get_const_sign(Const *constval)
{
	if (constval->constisnull)
		return SLOPE_SIGN_NULL;

	switch (constval->consttype)
	{
		case INT2OID:
			{
				int16		val = DatumGetInt16(constval->constvalue);

				return (val > 0) ? 1 : (val < 0) ? -1 : 0;
			}
		case INT4OID:
			{
				int32		val = DatumGetInt32(constval->constvalue);

				return (val > 0) ? 1 : (val < 0) ? -1 : 0;
			}
		case INT8OID:
			{
				int64		val = DatumGetInt64(constval->constvalue);

				return (val > 0) ? 1 : (val < 0) ? -1 : 0;
			}
		case FLOAT4OID:
			{
				float4		val = DatumGetFloat4(constval->constvalue);

				if (isnan(val))
					return SLOPE_SIGN_NAN;
				if (isinf(val))
					return val > 0 ? SLOPE_SIGN_PINF : SLOPE_SIGN_NINF;
				else if (val == 0)
					return SLOPE_SIGN_ZERO;
				else
					return val > 0 ? SLOPE_SIGN_POS : SLOPE_SIGN_NEG;
			}
		case FLOAT8OID:
			{
				float8		val = DatumGetFloat8(constval->constvalue);

				if (isnan(val))
					return SLOPE_SIGN_NAN;
				if (isinf(val))
					return val > 0 ? SLOPE_SIGN_PINF : SLOPE_SIGN_NINF;
				else if (val == 0)
					return SLOPE_SIGN_ZERO;
				else
					return val > 0 ? SLOPE_SIGN_POS : SLOPE_SIGN_NEG;
			}
		case NUMERICOID:
			{
				Numeric		num = DatumGetNumeric(constval->constvalue);
				Datum		result;
				Numeric		sign_num;
				int			val_sign;

				if (numeric_is_nan(num))
					return SLOPE_SIGN_NAN;

				result = DirectFunctionCall1(numeric_sign, NumericGetDatum(num));
				sign_num = DatumGetNumeric(result);
				val_sign = numeric_int4_safe(sign_num, NULL);
				if (numeric_is_inf(num))
					return val_sign == 1 ? SLOPE_SIGN_PINF : SLOPE_SIGN_NINF;
				else
					return (enum SLOPE_SIGN) val_sign;
			}
		default:
			return 0;
	}
}


/*
 * Prosupport: f(x, ...) is monotonically increasing in x.
 */
Datum
arg0_asc_slope_support(PG_FUNCTION_ARGS)
{
	SLOPE_REQUEST(req);
	return monotonic_slope_support(req, 1, asc0_slope);
}

 /*
  * Prosupport: f(x, ...) is monotonically decreasing in x.
  */
Datum
arg0_desc_slope_support(PG_FUNCTION_ARGS)
{
	SLOPE_REQUEST(req);
	return monotonic_slope_support(req, 1, desc0_slope);
}

 /*
  * Prosupport: f(a, x, ...) is monotonically increasing in x.
  */
Datum
arg1_asc_slope_support(PG_FUNCTION_ARGS)
{
	SLOPE_REQUEST(req);
	return monotonic_slope_support(req, 2, asc1_slope);
}

/*
 * diff_slope_support
 *		Prosupport: f(x, y) = x - y is increasing in x, decreasing in y.
 */
Datum
diff_slope_support(PG_FUNCTION_ARGS)
{
	SLOPE_REQUEST_2ARGS(req, arg0, arg1);

	if (IsA(arg0, Const))
	{
		switch (get_const_sign((Const *) arg0))
		{
			case SLOPE_SIGN_NINF:
			case SLOPE_SIGN_PINF:
			case SLOPE_SIGN_NAN:
				return monotonic_slope_support(req, 2, flat_slope);
			default:
				break;
		}
	}
	else if (IsA(arg1, Const))
	{
		switch (get_const_sign((Const *) arg1))
		{
			case SLOPE_SIGN_NINF:
			case SLOPE_SIGN_PINF:
			case SLOPE_SIGN_NAN:
				return monotonic_slope_support(req, 2, flat_slope);
			default:
				break;
		}
	}

	return monotonic_slope_support(req, 2, diff_slope);
}

/*
 * addition_slope_support
 *		Prosupport: f(x, y) = x + y is increasing in both x and y.
 */
Datum
addition_slope_support(PG_FUNCTION_ARGS)
{
	SLOPE_REQUEST_2ARGS(req, arg0, arg1);

	if (IsA(arg0, Const))
	{
		switch (get_const_sign((Const *) arg0))
		{
			case SLOPE_SIGN_PINF:
			case SLOPE_SIGN_NINF:
			case SLOPE_SIGN_NAN:
				return monotonic_slope_support(req, 2, flat_slope);
			default:
				break;
		}
	}
	else if (IsA(arg1, Const))
	{
		switch (get_const_sign((Const *) arg1))
		{
			case SLOPE_SIGN_PINF:
			case SLOPE_SIGN_NINF:
			case SLOPE_SIGN_NAN:
				return monotonic_slope_support(req, 2, flat_slope);
			default:
				break;
		}
	}

	return monotonic_slope_support(req, 2, asc_slope);
}

/*
 * multiply_slope_support
 *		Prosupport: x * c is increasing if c > 0, decreasing if c < 0.
 *		Similarly for c * x.
 *
 * For multiplication, the monotonicity depends on the sign of the constant:
 * - x * positive_const: increasing in x
 * - x * negative_const: decreasing in x
 * - x * 0: not monotonic (constant result)
 */
Datum
multiply_slope_support(PG_FUNCTION_ARGS)
{
	Const	   *constval;
	SLOPE_REQUEST_2ARGS(req, arg0, arg1);

	if (IsA(arg0, Const))
		constval = (Const *) arg0;
	else if (IsA(arg1, Const))
		constval = (Const *) arg1;
	else
		PG_RETURN_POINTER(NULL);

	switch (get_const_sign(constval))
	{
		case SLOPE_SIGN_POS:
			return monotonic_slope_support(req, 2, asc_slope);
		case SLOPE_SIGN_NEG:
			return monotonic_slope_support(req, 2, desc_slope);
		case SLOPE_SIGN_NAN:
			return monotonic_slope_support(req, 2, flat_slope);
		case SLOPE_SIGN_ZERO:
			return monotonic_slope_support(req, 2, flat_slope);
		default:
			PG_RETURN_POINTER(NULL);
	}

}

/*
 * divide_slope_support
 *		Prosupport: x / c is increasing if c > 0, decreasing if c < 0.
 *
 * Division by a constant has the same monotonicity as multiplication:
 * - x / positive_const: increasing in x
 * - x / negative_const: decreasing in x
 * - x / 0: undefined (not monotonic)
 */
Datum
divide_slope_support(PG_FUNCTION_ARGS)
{
	Const	   *constval;
	SLOPE_REQUEST_2ARGS(req, arg0, arg1);

	if (!IsA(arg1, Const))
		PG_RETURN_POINTER(NULL);

	constval = (Const *) arg1;
	switch (get_const_sign(constval))
	{
		case SLOPE_SIGN_POS:
			return monotonic_slope_support(req, 2, asc_slope);
		case SLOPE_SIGN_NEG:
			return monotonic_slope_support(req, 2, desc_slope);
		case SLOPE_SIGN_NAN:
			return monotonic_slope_support(req, 2, flat_slope);
		case SLOPE_SIGN_PINF:
		case SLOPE_SIGN_NINF:
			return monotonic_slope_support(req, 2, flat_slope);
		default:
			PG_RETURN_POINTER(NULL);
	}
	PG_RETURN_POINTER(NULL);
}

/*
 * Look up a timezone from a constant text argument.
 */
static pg_tz *
get_const_timezone_arg(List *args, int argno)
{
	Node	   *tz_arg_node;
	Const	   *tz_const;
	text	   *zone;
	char		tzname[TZ_STRLEN_MAX + 1];
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	int			offset;
	pg_tz	   *tz;

	if (args == NULL || list_length(args) <= argno)
		return NULL;

	tz_arg_node = list_nth(args, argno);
	if (!IsA(tz_arg_node, Const))
		return NULL;

	tz_const = (Const *) tz_arg_node;

	if (tz_const->constisnull || tz_const->consttype != TEXTOID)
		return NULL;

	zone = DatumGetTextPP(tz_const->constvalue);
	text_to_cstring_buffer(zone, tzname, sizeof(tzname));

	DecodeTimezoneName(tzname, &offset, &tz, (Node *) &escontext);
	if (escontext.error_occurred)
		return NULL;
	return tz;
}

/*
 * Slope support for date(timestamptz)
 */
Datum
timestamptz_date_slope_support(PG_FUNCTION_ARGS)
{
	SLOPE_REQUEST(req);

	if (pg_timezone_is_monotonic(session_timezone, TZ_GAP_DAY, false))
		return monotonic_slope_support(req, 1, asc0_slope);
	else
		PG_RETURN_POINTER(NULL);
}

static Oid
get_monotonic_expr_funcid(SupportRequestMonotonic * req)
{
	Node	   *expr = req->expr;

	if (IsA(expr, FuncExpr))
		return ((FuncExpr *) expr)->funcid;
	return InvalidOid;
}

static Datum
timestamptz_support_lookup(SupportRequestMonotonic *req, Datum fixed_unit, pg_tz *tzp)
{
	text	   *units;
	int			val;
	char	   *lowunits;
	int			type;
	TZMonotonicityBits tz_unit;

	units = DatumGetTextPP(fixed_unit);

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);

	if (type != UNITS)
		PG_RETURN_POINTER(NULL);

	switch (val)
	{
		case DTK_WEEK:
			PG_RETURN_POINTER(NULL);
		case DTK_MILLENNIUM:
		case DTK_CENTURY:
		case DTK_DECADE:
		case DTK_YEAR:
			tz_unit = TZ_GAP_YEAR;
			break;
		case DTK_QUARTER:
		case DTK_MONTH:
			tz_unit = TZ_GAP_MONTH;
			break;
		case DTK_DAY:
			tz_unit = TZ_GAP_DAY;
			break;
		case DTK_HOUR:
			tz_unit = TZ_GAP_HOUR;
			break;
		case DTK_MINUTE:
			tz_unit = TZ_GAP_MINUTE;
			break;
		case DTK_SECOND:
		case DTK_MILLISEC:
		case DTK_MICROSEC:
			tz_unit = TZ_GAP_SECOND;
			break;
		default:
			PG_RETURN_POINTER(NULL);
	}
	if (pg_timezone_is_monotonic(tzp, tz_unit, false))
		return monotonic_slope_support(req, 2, asc1_slope);
	else
		PG_RETURN_POINTER(NULL);
}

/*
 * Prosupport for timezone(...) overloads with timestamp types.
 */
Datum
timezone_prosupport(PG_FUNCTION_ARGS)
{
	pg_tz	   *tzp;
	bool		to_utc = false;
	SLOPE_REQUEST_ARGS(req, args, 1);

	switch (get_monotonic_expr_funcid(req))
	{

		case F_TIMEZONE_TEXT_TIMESTAMP:
			to_utc = true;
			pg_fallthrough;
		case F_TIMEZONE_TEXT_TIMESTAMPTZ:
			tzp = get_const_timezone_arg(args, 0);
			break;
		case F_TIMEZONE_TIMESTAMP:
			to_utc = true;
			pg_fallthrough;
		case F_TIMEZONE_TIMESTAMPTZ:
			tzp = session_timezone;
			break;

		default:
			PG_RETURN_POINTER(NULL);
	}

	if (tzp == NULL || !pg_timezone_is_monotonic(tzp, TZ_GAP_SECOND, to_utc))
		PG_RETURN_POINTER(NULL);

	/*
	 * We need MONOTONICFUNC_INCREASING for either the first or second
	 * argument, but the other argument is either a constant or missing, so we
	 * can simply return MONOTONICFUNC_INCREASING for both.
	 */
	return monotonic_slope_support(req, 2, asc_slope);
}

/*
 * Prosupport for date_trunc(...) overloads with timestamp types.
 */
Datum
date_trunc_slope_support(PG_FUNCTION_ARGS)
{
	Const	   *unit_arg;
	pg_tz	   *tzp = NULL;
	SLOPE_REQUEST_ARGS(req, args, 2);

	unit_arg = (Const *) linitial(args);
	if (!IsA(unit_arg, Const) || unit_arg->constisnull)
		PG_RETURN_POINTER(NULL);

	switch (get_monotonic_expr_funcid(req))
	{
		case F_DATE_TRUNC_TEXT_TIMESTAMP:
			tzp = DecodeTimezoneNameToTz("UTC");
			break;

		case F_DATE_TRUNC_TEXT_TIMESTAMPTZ_TEXT:
			tzp = get_const_timezone_arg(args, 2);
			break;

		case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
			tzp = session_timezone;
			break;

		default:
			PG_RETURN_POINTER(NULL);
	}

	if (tzp == NULL)
		PG_RETURN_POINTER(NULL);

	return timestamptz_support_lookup(req, unit_arg->constvalue, tzp);
}
