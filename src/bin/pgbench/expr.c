/*-------------------------------------------------------------------------
 *
 * expr.c
 *	  Expression and built-in function evaluation for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/expr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <float.h>
#include <limits.h>
#include <math.h>

#include "common/int.h"
#include "common/logging.h"
#include "expr.h"
#include "pgbench.h"
#include "random.h"
#include "variable.h"

/* X/Open (XSI) requires <math.h> to provide M_PI, but core POSIX does not */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool
isLazyFunc(PgBenchFunction func)
{
	return func == PGBENCH_AND || func == PGBENCH_OR || func == PGBENCH_CASE;
}

/* lazy evaluation of some functions */
static bool
evalLazyFunc(CState *st,
			 PgBenchFunction func, PgBenchExprLink *args, PgBenchValue *retval)
{
	PgBenchValue a1,
				a2;
	bool		ba1,
				ba2;

	Assert(isLazyFunc(func) && args != NULL && args->next != NULL);

	/* args points to first condition */
	if (!evaluateExpr(st, args->expr, &a1))
		return false;

	/* second condition for AND/OR and corresponding branch for CASE */
	args = args->next;

	switch (func)
	{
		case PGBENCH_AND:
			if (a1.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}

			if (!coerceToBool(&a1, &ba1))
				return false;

			if (!ba1)
			{
				setBoolValue(retval, false);
				return true;
			}

			if (!evaluateExpr(st, args->expr, &a2))
				return false;

			if (a2.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}
			else if (!coerceToBool(&a2, &ba2))
				return false;
			else
			{
				setBoolValue(retval, ba2);
				return true;
			}

			return true;

		case PGBENCH_OR:

			if (a1.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}

			if (!coerceToBool(&a1, &ba1))
				return false;

			if (ba1)
			{
				setBoolValue(retval, true);
				return true;
			}

			if (!evaluateExpr(st, args->expr, &a2))
				return false;

			if (a2.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}
			else if (!coerceToBool(&a2, &ba2))
				return false;
			else
			{
				setBoolValue(retval, ba2);
				return true;
			}

		case PGBENCH_CASE:
			/* when true, execute branch */
			if (valueTruth(&a1))
				return evaluateExpr(st, args->expr, retval);

			/* now args contains next condition or final else expression */
			args = args->next;

			/* final else case? */
			if (args->next == NULL)
				return evaluateExpr(st, args->expr, retval);

			/* no, another when, proceed */
			return evalLazyFunc(st, PGBENCH_CASE, args, retval);

		default:
			/* internal error, cannot get here */
			Assert(0);
			break;
	}
	return false;
}

/* maximum number of function arguments */
#define MAX_FARGS 16

/*
 * Recursive evaluation of standard functions,
 * which do not require lazy evaluation.
 */
static bool
evalStandardFunc(CState *st,
				 PgBenchFunction func, PgBenchExprLink *args,
				 PgBenchValue *retval)
{
	/* evaluate all function arguments */
	int			nargs = 0;
	PgBenchValue vargs[MAX_FARGS] = {0};
	PgBenchExprLink *l = args;
	bool		has_null = false;

	for (nargs = 0; nargs < MAX_FARGS && l != NULL; nargs++, l = l->next)
	{
		if (!evaluateExpr(st, l->expr, &vargs[nargs]))
			return false;
		has_null |= vargs[nargs].type == PGBT_NULL;
	}

	if (l != NULL)
	{
		pg_log_error("too many function arguments, maximum is %d", MAX_FARGS);
		return false;
	}

	/* NULL arguments */
	if (has_null && func != PGBENCH_IS && func != PGBENCH_DEBUG)
	{
		setNullValue(retval);
		return true;
	}

	/* then evaluate function */
	switch (func)
	{
			/* overloaded operators */
		case PGBENCH_ADD:
		case PGBENCH_SUB:
		case PGBENCH_MUL:
		case PGBENCH_DIV:
		case PGBENCH_MOD:
		case PGBENCH_EQ:
		case PGBENCH_NE:
		case PGBENCH_LE:
		case PGBENCH_LT:
			{
				PgBenchValue *lval = &vargs[0],
						   *rval = &vargs[1];

				Assert(nargs == 2);

				/* overloaded type management, double if some double */
				if ((lval->type == PGBT_DOUBLE ||
					 rval->type == PGBT_DOUBLE) && func != PGBENCH_MOD)
				{
					double		ld,
								rd;

					if (!coerceToDouble(lval, &ld) ||
						!coerceToDouble(rval, &rd))
						return false;

					switch (func)
					{
						case PGBENCH_ADD:
							setDoubleValue(retval, ld + rd);
							return true;

						case PGBENCH_SUB:
							setDoubleValue(retval, ld - rd);
							return true;

						case PGBENCH_MUL:
							setDoubleValue(retval, ld * rd);
							return true;

						case PGBENCH_DIV:
							setDoubleValue(retval, ld / rd);
							return true;

						case PGBENCH_EQ:
							setBoolValue(retval, ld == rd);
							return true;

						case PGBENCH_NE:
							setBoolValue(retval, ld != rd);
							return true;

						case PGBENCH_LE:
							setBoolValue(retval, ld <= rd);
							return true;

						case PGBENCH_LT:
							setBoolValue(retval, ld < rd);
							return true;

						default:
							/* cannot get here */
							Assert(0);
					}
				}
				else			/* we have integer operands, or % */
				{
					int64		li,
								ri,
								res;

					if (!coerceToInt(lval, &li) ||
						!coerceToInt(rval, &ri))
						return false;

					switch (func)
					{
						case PGBENCH_ADD:
							if (pg_add_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint add out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_SUB:
							if (pg_sub_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint sub out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_MUL:
							if (pg_mul_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint mul out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_EQ:
							setBoolValue(retval, li == ri);
							return true;

						case PGBENCH_NE:
							setBoolValue(retval, li != ri);
							return true;

						case PGBENCH_LE:
							setBoolValue(retval, li <= ri);
							return true;

						case PGBENCH_LT:
							setBoolValue(retval, li < ri);
							return true;

						case PGBENCH_DIV:
						case PGBENCH_MOD:
							if (ri == 0)
							{
								pg_log_error("division by zero");
								return false;
							}
							/* special handling of -1 divisor */
							if (ri == -1)
							{
								if (func == PGBENCH_DIV)
								{
									/* overflow check (needed for INT64_MIN) */
									if (li == PG_INT64_MIN)
									{
										pg_log_error("bigint div out of range");
										return false;
									}
									else
										setIntValue(retval, -li);
								}
								else
									setIntValue(retval, 0);
								return true;
							}
							/* else divisor is not -1 */
							if (func == PGBENCH_DIV)
								setIntValue(retval, li / ri);
							else	/* func == PGBENCH_MOD */
								setIntValue(retval, li % ri);

							return true;

						default:
							/* cannot get here */
							Assert(0);
					}
				}

				Assert(0);
				return false;	/* NOTREACHED */
			}

			/* integer bitwise operators */
		case PGBENCH_BITAND:
		case PGBENCH_BITOR:
		case PGBENCH_BITXOR:
		case PGBENCH_LSHIFT:
		case PGBENCH_RSHIFT:
			{
				int64		li,
							ri;

				if (!coerceToInt(&vargs[0], &li) || !coerceToInt(&vargs[1], &ri))
					return false;

				if (func == PGBENCH_BITAND)
					setIntValue(retval, li & ri);
				else if (func == PGBENCH_BITOR)
					setIntValue(retval, li | ri);
				else if (func == PGBENCH_BITXOR)
					setIntValue(retval, li ^ ri);
				else if (func == PGBENCH_LSHIFT)
					setIntValue(retval, li << ri);
				else if (func == PGBENCH_RSHIFT)
					setIntValue(retval, li >> ri);
				else			/* cannot get here */
					Assert(0);

				return true;
			}

			/* logical operators */
		case PGBENCH_NOT:
			{
				bool		b;

				if (!coerceToBool(&vargs[0], &b))
					return false;

				setBoolValue(retval, !b);
				return true;
			}

			/* no arguments */
		case PGBENCH_PI:
			setDoubleValue(retval, M_PI);
			return true;

			/* 1 overloaded argument */
		case PGBENCH_ABS:
			{
				PgBenchValue *varg = &vargs[0];

				Assert(nargs == 1);

				if (varg->type == PGBT_INT)
				{
					int64		i = varg->u.ival;

					setIntValue(retval, i < 0 ? -i : i);
				}
				else
				{
					double		d = varg->u.dval;

					Assert(varg->type == PGBT_DOUBLE);
					setDoubleValue(retval, d < 0.0 ? -d : d);
				}

				return true;
			}

		case PGBENCH_DEBUG:
			{
				PgBenchValue *varg = &vargs[0];

				Assert(nargs == 1);

				fprintf(stderr, "debug(script=%d,command=%d): ",
						st->use_file, st->command + 1);

				if (varg->type == PGBT_NULL)
					fprintf(stderr, "null\n");
				else if (varg->type == PGBT_BOOLEAN)
					fprintf(stderr, "boolean %s\n", varg->u.bval ? "true" : "false");
				else if (varg->type == PGBT_INT)
					fprintf(stderr, "int " INT64_FORMAT "\n", varg->u.ival);
				else if (varg->type == PGBT_DOUBLE)
					fprintf(stderr, "double %.*g\n", DBL_DIG, varg->u.dval);
				else			/* internal error, unexpected type */
					Assert(0);

				*retval = *varg;

				return true;
			}

			/* 1 double argument */
		case PGBENCH_DOUBLE:
		case PGBENCH_SQRT:
		case PGBENCH_LN:
		case PGBENCH_EXP:
			{
				double		dval;

				Assert(nargs == 1);

				if (!coerceToDouble(&vargs[0], &dval))
					return false;

				if (func == PGBENCH_SQRT)
					dval = sqrt(dval);
				else if (func == PGBENCH_LN)
					dval = log(dval);
				else if (func == PGBENCH_EXP)
					dval = exp(dval);
				/* else is cast: do nothing */

				setDoubleValue(retval, dval);
				return true;
			}

			/* 1 int argument */
		case PGBENCH_INT:
			{
				int64		ival;

				Assert(nargs == 1);

				if (!coerceToInt(&vargs[0], &ival))
					return false;

				setIntValue(retval, ival);
				return true;
			}

			/* variable number of arguments */
		case PGBENCH_LEAST:
		case PGBENCH_GREATEST:
			{
				bool		havedouble;
				int			i;

				Assert(nargs >= 1);

				/* need double result if any input is double */
				havedouble = false;
				for (i = 0; i < nargs; i++)
				{
					if (vargs[i].type == PGBT_DOUBLE)
					{
						havedouble = true;
						break;
					}
				}
				if (havedouble)
				{
					double		extremum;

					if (!coerceToDouble(&vargs[0], &extremum))
						return false;
					for (i = 1; i < nargs; i++)
					{
						double		dval;

						if (!coerceToDouble(&vargs[i], &dval))
							return false;
						if (func == PGBENCH_LEAST)
							extremum = Min(extremum, dval);
						else
							extremum = Max(extremum, dval);
					}
					setDoubleValue(retval, extremum);
				}
				else
				{
					int64		extremum;

					if (!coerceToInt(&vargs[0], &extremum))
						return false;
					for (i = 1; i < nargs; i++)
					{
						int64		ival;

						if (!coerceToInt(&vargs[i], &ival))
							return false;
						if (func == PGBENCH_LEAST)
							extremum = Min(extremum, ival);
						else
							extremum = Max(extremum, ival);
					}
					setIntValue(retval, extremum);
				}
				return true;
			}

			/* random functions */
		case PGBENCH_RANDOM:
		case PGBENCH_RANDOM_EXPONENTIAL:
		case PGBENCH_RANDOM_GAUSSIAN:
		case PGBENCH_RANDOM_ZIPFIAN:
			{
				int64		imin,
							imax,
							delta;

				Assert(nargs >= 2);

				if (!coerceToInt(&vargs[0], &imin) ||
					!coerceToInt(&vargs[1], &imax))
					return false;

				/* check random range */
				if (unlikely(imin > imax))
				{
					pg_log_error("empty range given to random");
					return false;
				}
				else if (unlikely(pg_sub_s64_overflow(imax, imin, &delta) ||
								  pg_add_s64_overflow(delta, 1, &delta)))
				{
					/* prevent int overflows in random functions */
					pg_log_error("random range is too large");
					return false;
				}

				if (func == PGBENCH_RANDOM)
				{
					Assert(nargs == 2);
					setIntValue(retval, getrand(&st->cs_func_rs, imin, imax));
				}
				else			/* gaussian & exponential */
				{
					double		param;

					Assert(nargs == 3);

					if (!coerceToDouble(&vargs[2], &param))
						return false;

					if (func == PGBENCH_RANDOM_GAUSSIAN)
					{
						if (param < MIN_GAUSSIAN_PARAM)
						{
							pg_log_error("gaussian parameter must be at least %f (not %f)",
										 MIN_GAUSSIAN_PARAM, param);
							return false;
						}

						setIntValue(retval,
									getGaussianRand(&st->cs_func_rs,
													imin, imax, param));
					}
					else if (func == PGBENCH_RANDOM_ZIPFIAN)
					{
						if (param < MIN_ZIPFIAN_PARAM || param > MAX_ZIPFIAN_PARAM)
						{
							pg_log_error("zipfian parameter must be in range [%.3f, %.0f] (not %f)",
										 MIN_ZIPFIAN_PARAM, MAX_ZIPFIAN_PARAM, param);
							return false;
						}

						setIntValue(retval,
									getZipfianRand(&st->cs_func_rs, imin, imax, param));
					}
					else		/* exponential */
					{
						if (param <= 0.0)
						{
							pg_log_error("exponential parameter must be greater than zero (not %f)",
										 param);
							return false;
						}

						setIntValue(retval,
									getExponentialRand(&st->cs_func_rs,
													   imin, imax, param));
					}
				}

				return true;
			}

		case PGBENCH_POW:
			{
				PgBenchValue *lval = &vargs[0];
				PgBenchValue *rval = &vargs[1];
				double		ld,
							rd;

				Assert(nargs == 2);

				if (!coerceToDouble(lval, &ld) ||
					!coerceToDouble(rval, &rd))
					return false;

				setDoubleValue(retval, pow(ld, rd));

				return true;
			}

		case PGBENCH_IS:
			{
				Assert(nargs == 2);

				/*
				 * note: this simple implementation is more permissive than
				 * SQL
				 */
				setBoolValue(retval,
							 vargs[0].type == vargs[1].type &&
							 vargs[0].u.bval == vargs[1].u.bval);
				return true;
			}

			/* hashing */
		case PGBENCH_HASH_FNV1A:
		case PGBENCH_HASH_MURMUR2:
			{
				int64		val,
							seed;

				Assert(nargs == 2);

				if (!coerceToInt(&vargs[0], &val) ||
					!coerceToInt(&vargs[1], &seed))
					return false;

				if (func == PGBENCH_HASH_MURMUR2)
					setIntValue(retval, getHashMurmur2(val, seed));
				else if (func == PGBENCH_HASH_FNV1A)
					setIntValue(retval, getHashFnv1a(val, seed));
				else
					/* cannot get here */
					Assert(0);

				return true;
			}

		case PGBENCH_PERMUTE:
			{
				int64		val,
							size,
							seed;

				Assert(nargs == 3);

				if (!coerceToInt(&vargs[0], &val) ||
					!coerceToInt(&vargs[1], &size) ||
					!coerceToInt(&vargs[2], &seed))
					return false;

				if (size <= 0)
				{
					pg_log_error("permute size parameter must be greater than zero");
					return false;
				}

				setIntValue(retval, permute(val, size, seed));
				return true;
			}

		default:
			/* cannot get here */
			Assert(0);
			/* dead code to avoid a compiler warning */
			return false;
	}
}

/* evaluate some function */
static bool
evalFunc(CState *st,
		 PgBenchFunction func, PgBenchExprLink *args, PgBenchValue *retval)
{
	if (isLazyFunc(func))
		return evalLazyFunc(st, func, args, retval);
	else
		return evalStandardFunc(st, func, args, retval);
}

/*
 * Recursive evaluation of an expression in a pgbench script
 * using the current state of variables.
 * Returns whether the evaluation was ok,
 * the value itself is returned through the retval pointer.
 */
bool
evaluateExpr(CState *st, PgBenchExpr *expr, PgBenchValue *retval)
{
	switch (expr->etype)
	{
		case ENODE_CONSTANT:
			{
				*retval = expr->u.constant;
				return true;
			}

		case ENODE_VARIABLE:
			{
				Variable   *var;

				if ((var = lookupVariable(&st->variables, expr->u.variable.varname)) == NULL)
				{
					pg_log_error("undefined variable \"%s\"", expr->u.variable.varname);
					return false;
				}

				if (!makeVariableValue(var))
					return false;

				*retval = var->value;
				return true;
			}

		case ENODE_FUNCTION:
			return evalFunc(st,
							expr->u.function.function,
							expr->u.function.args,
							retval);

		default:
			/* internal error which should never occur */
			pg_fatal("unexpected enode type in evaluation: %d", expr->etype);
	}
}
