/*-------------------------------------------------------------------------
 *
 * script.c
 *	  Script parsing, built-in scripts, and expression evaluation for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/script.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>

#include "common/int.h"
#include "common/logging.h"
#include "common/pg_prng.h"
#include "common/pgbench_funcs.h"
#include "fe_utils/conditional.h"
#include "pgbench.h"
#include "script.h"

QueryMode querymode = QUERY_SIMPLE;
const char *const QUERYMODE[] = {"simple", "extended", "prepared"};

ParsedScript sql_script[MAX_SCRIPTS];	/* SQL script files */
int	num_scripts;		/* number of scripts in sql_script[] */
int64 total_weight = 0;

static const BuiltinScript builtin_script[] =
{
	{
		"tpcb-like",
		"<builtin: TPC-B (sort of)>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"\\set bid random(1, " CppAsString2(nbranches) " * :scale)\n"
		"\\set tid random(1, " CppAsString2(ntellers) " * :scale)\n"
		"\\set delta random(-5000, 5000)\n"
		"BEGIN;\n"
		"UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
		"UPDATE pgbench_tellers SET tbalance = tbalance + :delta WHERE tid = :tid;\n"
		"UPDATE pgbench_branches SET bbalance = bbalance + :delta WHERE bid = :bid;\n"
		"INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
		"END;\n"
	},
	{
		"simple-update",
		"<builtin: simple update>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"\\set bid random(1, " CppAsString2(nbranches) " * :scale)\n"
		"\\set tid random(1, " CppAsString2(ntellers) " * :scale)\n"
		"\\set delta random(-5000, 5000)\n"
		"BEGIN;\n"
		"UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
		"INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
		"END;\n"
	},
	{
		"select-only",
		"<builtin: select only>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
	}
};

static void addScript(const ParsedScript *script);

/* callback functions for our flex lexer */
static const PsqlScanCallbacks pgbench_callbacks = {
	NULL,						/* don't need get_variable functionality */
};

static bool
isLazyFunc(PgBenchFunction func)
{
	return func == PGBENCH_AND || func == PGBENCH_OR ||
		func == PGBENCH_CASE || func == PGBENCH_ARRAY_GENERATE;
}

static void
freeValues(PgBenchValue *vargs, int nargs)
{
	for (int i = 0; i < nargs; i++)
		freeValue(&vargs[i]);
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
				freeValue(&a1);
				setNullValue(retval);
				return true;
			}

			if (!coerceToBool(&a1, &ba1))
			{
				freeValue(&a1);
				return false;
			}
			freeValue(&a1);

			if (!ba1)
			{
				setBoolValue(retval, false);
				return true;
			}

			if (!evaluateExpr(st, args->expr, &a2))
				return false;

			if (a2.type == PGBT_NULL)
			{
				freeValue(&a2);
				setNullValue(retval);
				return true;
			}
			else if (!coerceToBool(&a2, &ba2))
			{
				freeValue(&a2);
				return false;
			}
			else
			{
				freeValue(&a2);
				setBoolValue(retval, ba2);
				return true;
			}

		case PGBENCH_OR:

			if (a1.type == PGBT_NULL)
			{
				freeValue(&a1);
				setNullValue(retval);
				return true;
			}

			if (!coerceToBool(&a1, &ba1))
			{
				freeValue(&a1);
				return false;
			}
			freeValue(&a1);

			if (ba1)
			{
				setBoolValue(retval, true);
				return true;
			}

			if (!evaluateExpr(st, args->expr, &a2))
				return false;

			if (a2.type == PGBT_NULL)
			{
				freeValue(&a2);
				setNullValue(retval);
				return true;
			}
			else if (!coerceToBool(&a2, &ba2))
			{
				freeValue(&a2);
				return false;
			}
			else
			{
				freeValue(&a2);
				setBoolValue(retval, ba2);
				return true;
			}

		case PGBENCH_CASE:
			/* when true, execute branch */
			if (valueTruth(&a1))
			{
				freeValue(&a1);
				return evaluateExpr(st, args->expr, retval);
			}
			freeValue(&a1);

			/* now args contains next condition or final else expression */
			args = args->next;

			/* final else case? */
			if (args->next == NULL)
				return evaluateExpr(st, args->expr, retval);

			/* no, another when, proceed */
			return evalLazyFunc(st, PGBENCH_CASE, args, retval);

		case PGBENCH_ARRAY_GENERATE:
			{
				int64		len;
				PgBenchValue *elems = NULL;

				if (a1.type == PGBT_NULL)
				{
					freeValue(&a1);
					setNullValue(retval);
					return true;
				}
				if (!coerceToInt(&a1, &len))
				{
					freeValue(&a1);
					return false;
				}
				freeValue(&a1);

				if (len < 0 || len > 100000)
				{
					pg_log_error("array_generate length " INT64_FORMAT " out of range [0, 100000]",
								 len);
					return false;
				}

				if (len > 0)
				{
					elems = pg_malloc0_array(PgBenchValue, (int) len);
					for (int64 i = 0; i < len; i++)
					{
						if (!evaluateExpr(st, args->expr, &elems[i]))
						{
							for (int64 j = 0; j < i; j++)
								freeValue(&elems[j]);
							pg_free(elems);
							return false;
						}
					}
				}

				retval->type = PGBT_ARRAY;
				retval->u.aval.nelems = (int) len;
				retval->u.aval.elems = elems;
				return true;
			}

		default:
			/* internal error, cannot get here */
			freeValue(&a1);
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
	bool		ok = false;

	for (nargs = 0; nargs < MAX_FARGS && l != NULL; nargs++, l = l->next)
	{
		if (!evaluateExpr(st, l->expr, &vargs[nargs]))
		{
			freeValues(vargs, nargs);
			return false;
		}
		has_null |= vargs[nargs].type == PGBT_NULL;
	}

	if (l != NULL)
	{
		pg_log_error("too many function arguments, maximum is %d", MAX_FARGS);
		freeValues(vargs, nargs);
		return false;
	}

	/* NULL arguments */
	if (has_null && func != PGBENCH_IS && func != PGBENCH_DEBUG &&
		func != PGBENCH_ARRAY)
	{
		setNullValue(retval);
		freeValues(vargs, nargs);
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

				/* timestamp comparisons */
				if ((lval->type == PGBT_TIMESTAMP ||
					 rval->type == PGBT_TIMESTAMP) &&
					(func == PGBENCH_EQ || func == PGBENCH_NE ||
					 func == PGBENCH_LE || func == PGBENCH_LT))
				{
					int64		lt,
								rt;

					if (!coerceToTimestamp(lval, &lt) ||
						!coerceToTimestamp(rval, &rt))
						goto cleanup;

					if (func == PGBENCH_EQ)
						setBoolValue(retval, lt == rt);
					else if (func == PGBENCH_NE)
						setBoolValue(retval, lt != rt);
					else if (func == PGBENCH_LE)
						setBoolValue(retval, lt <= rt);
					else
						setBoolValue(retval, lt < rt);

					ok = true;
					goto cleanup;
				}

				/* text comparisons */
				if ((lval->type == PGBT_TEXT ||
					 rval->type == PGBT_TEXT) &&
					(func == PGBENCH_EQ || func == PGBENCH_NE ||
					 func == PGBENCH_LE || func == PGBENCH_LT))
				{
					char	   *ls = NULL,
							   *rs = NULL;
					int			cmp;

					if (!coerceToText(lval, &ls) ||
						!coerceToText(rval, &rs))
					{
						pg_free(ls);
						pg_free(rs);
						goto cleanup;
					}

					cmp = strcmp(ls, rs);
					pg_free(ls);
					pg_free(rs);

					if (func == PGBENCH_EQ)
						setBoolValue(retval, cmp == 0);
					else if (func == PGBENCH_NE)
						setBoolValue(retval, cmp != 0);
					else if (func == PGBENCH_LE)
						setBoolValue(retval, cmp <= 0);
					else
						setBoolValue(retval, cmp < 0);

					ok = true;
					goto cleanup;
				}

				/* overloaded type management, double if some double */
				if ((lval->type == PGBT_DOUBLE ||
					 rval->type == PGBT_DOUBLE) && func != PGBENCH_MOD)
				{
					double		ld,
								rd;

					if (!coerceToDouble(lval, &ld) ||
						!coerceToDouble(rval, &rd))
						goto cleanup;

					switch (func)
					{
						case PGBENCH_ADD:
							setDoubleValue(retval, ld + rd);
							ok = true;
							goto cleanup;

						case PGBENCH_SUB:
							setDoubleValue(retval, ld - rd);
							ok = true;
							goto cleanup;

						case PGBENCH_MUL:
							setDoubleValue(retval, ld * rd);
							ok = true;
							goto cleanup;

						case PGBENCH_DIV:
							setDoubleValue(retval, ld / rd);
							ok = true;
							goto cleanup;

						case PGBENCH_EQ:
							setBoolValue(retval, ld == rd);
							ok = true;
							goto cleanup;

						case PGBENCH_NE:
							setBoolValue(retval, ld != rd);
							ok = true;
							goto cleanup;

						case PGBENCH_LE:
							setBoolValue(retval, ld <= rd);
							ok = true;
							goto cleanup;

						case PGBENCH_LT:
							setBoolValue(retval, ld < rd);
							ok = true;
							goto cleanup;

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
						goto cleanup;

					switch (func)
					{
						case PGBENCH_ADD:
							if (pg_add_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint add out of range");
								goto cleanup;
							}
							setIntValue(retval, res);
							ok = true;
							goto cleanup;

						case PGBENCH_SUB:
							if (pg_sub_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint sub out of range");
								goto cleanup;
							}
							setIntValue(retval, res);
							ok = true;
							goto cleanup;

						case PGBENCH_MUL:
							if (pg_mul_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint mul out of range");
								goto cleanup;
							}
							setIntValue(retval, res);
							ok = true;
							goto cleanup;

						case PGBENCH_EQ:
							setBoolValue(retval, li == ri);
							ok = true;
							goto cleanup;

						case PGBENCH_NE:
							setBoolValue(retval, li != ri);
							ok = true;
							goto cleanup;

						case PGBENCH_LE:
							setBoolValue(retval, li <= ri);
							ok = true;
							goto cleanup;

						case PGBENCH_LT:
							setBoolValue(retval, li < ri);
							ok = true;
							goto cleanup;

						case PGBENCH_DIV:
						case PGBENCH_MOD:
							if (ri == 0)
							{
								pg_log_error("division by zero");
								goto cleanup;
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
										goto cleanup;
									}
									else
										setIntValue(retval, -li);
								}
								else
									setIntValue(retval, 0);
								ok = true;
								goto cleanup;
							}
							/* else divisor is not -1 */
							if (func == PGBENCH_DIV)
								setIntValue(retval, li / ri);
							else	/* func == PGBENCH_MOD */
								setIntValue(retval, li % ri);

							ok = true;
							goto cleanup;

						default:
							/* cannot get here */
							Assert(0);
					}
				}

				Assert(0);
				goto cleanup;	/* NOTREACHED */
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
					goto cleanup;

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

				ok = true;
				goto cleanup;
			}

			/* logical operators */
		case PGBENCH_NOT:
			{
				bool		b;

				if (!coerceToBool(&vargs[0], &b))
					goto cleanup;

				setBoolValue(retval, !b);
				ok = true;
				goto cleanup;
			}

			/* no arguments */
		case PGBENCH_PI:
			setDoubleValue(retval, M_PI);
			ok = true;
			goto cleanup;

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

				ok = true;
				goto cleanup;
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
				else if (varg->type == PGBT_TEXT)
					fprintf(stderr, "text '%s'\n", varg->u.sval ? varg->u.sval : "");
				else if (varg->type == PGBT_TIMESTAMP)
				{
					char	   *s = formatValue(varg);

					fprintf(stderr, "timestamp %s\n", s);
					pg_free(s);
				}
				else if (varg->type == PGBT_ARRAY)
				{
					char	   *s = formatValue(varg);

					fprintf(stderr, "array %s\n", s);
					pg_free(s);
				}
				else			/* internal error, unexpected type */
					Assert(0);

				copyValue(retval, varg);

				ok = true;
				goto cleanup;
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
					goto cleanup;

				if (func == PGBENCH_SQRT)
					dval = sqrt(dval);
				else if (func == PGBENCH_LN)
					dval = log(dval);
				else if (func == PGBENCH_EXP)
					dval = exp(dval);
				/* else is cast: do nothing */

				setDoubleValue(retval, dval);
				ok = true;
				goto cleanup;
			}

			/* 1 int argument */
		case PGBENCH_INT:
			{
				int64		ival;

				Assert(nargs == 1);

				if (!coerceToInt(&vargs[0], &ival))
					goto cleanup;

				setIntValue(retval, ival);
				ok = true;
				goto cleanup;
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
						goto cleanup;
					for (i = 1; i < nargs; i++)
					{
						double		dval;

						if (!coerceToDouble(&vargs[i], &dval))
							goto cleanup;
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
						goto cleanup;
					for (i = 1; i < nargs; i++)
					{
						int64		ival;

						if (!coerceToInt(&vargs[i], &ival))
							goto cleanup;
						if (func == PGBENCH_LEAST)
							extremum = Min(extremum, ival);
						else
							extremum = Max(extremum, ival);
					}
					setIntValue(retval, extremum);
				}
				ok = true;
				goto cleanup;
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
					goto cleanup;

				/* check random range */
				if (unlikely(imin > imax))
				{
					pg_log_error("empty range given to random");
					goto cleanup;
				}
				else if (unlikely(pg_sub_s64_overflow(imax, imin, &delta) ||
								  pg_add_s64_overflow(delta, 1, &delta)))
				{
					/* prevent int overflows in random functions */
					pg_log_error("random range is too large");
					goto cleanup;
				}

				if (func == PGBENCH_RANDOM)
				{
					Assert(nargs == 2);
					setIntValue(retval, pgbench_random(&st->cs_func_rs, imin, imax));
				}
				else			/* gaussian & exponential */
				{
					double		param;

					Assert(nargs == 3);

					if (!coerceToDouble(&vargs[2], &param))
						goto cleanup;

					if (func == PGBENCH_RANDOM_GAUSSIAN)
					{
						if (param < PGBENCH_MIN_GAUSSIAN_PARAM)
						{
							pg_log_error("gaussian parameter must be at least %f (not %f)",
										 PGBENCH_MIN_GAUSSIAN_PARAM, param);
							goto cleanup;
						}

						setIntValue(retval,
									pgbench_random_gaussian(&st->cs_func_rs,
															imin, imax, param));
					}
					else if (func == PGBENCH_RANDOM_ZIPFIAN)
					{
						if (param < PGBENCH_MIN_ZIPFIAN_PARAM || param > PGBENCH_MAX_ZIPFIAN_PARAM)
						{
							pg_log_error("zipfian parameter must be in range [%.3f, %.0f] (not %f)",
										 PGBENCH_MIN_ZIPFIAN_PARAM, PGBENCH_MAX_ZIPFIAN_PARAM, param);
							goto cleanup;
						}

						setIntValue(retval,
									pgbench_random_zipfian(&st->cs_func_rs, imin, imax, param));
					}
					else		/* exponential */
					{
						if (param <= 0.0)
						{
							pg_log_error("exponential parameter must be greater than zero (not %f)",
										 param);
							goto cleanup;
						}

						setIntValue(retval,
									pgbench_random_exponential(&st->cs_func_rs,
															   imin, imax, param));
					}
				}

				ok = true;
				goto cleanup;
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
					goto cleanup;

				setDoubleValue(retval, pow(ld, rd));

				ok = true;
				goto cleanup;
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
				ok = true;
				goto cleanup;
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
					goto cleanup;

				if (func == PGBENCH_HASH_MURMUR2)
					setIntValue(retval, pgbench_hash_murmur2(val, seed));
				else if (func == PGBENCH_HASH_FNV1A)
					setIntValue(retval, pgbench_hash_fnv1a(val, seed));
				else
					/* cannot get here */
					Assert(0);

				ok = true;
				goto cleanup;
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
					goto cleanup;

				if (size <= 0)
				{
					pg_log_error("permute size parameter must be greater than zero");
					goto cleanup;
				}

				setIntValue(retval, pgbench_permute(val, size, seed));
				ok = true;
				goto cleanup;
			}

		case PGBENCH_SUBSCRIPT:
			{
				int64		idx;

				Assert(nargs == 2);

				if (vargs[0].type == PGBT_NULL || vargs[1].type == PGBT_NULL)
				{
					setNullValue(retval);
					ok = true;
					goto cleanup;
				}

				if (vargs[0].type != PGBT_ARRAY)
				{
					pg_log_error("cannot subscript non-array value (type %s)",
								 valueTypeName(&vargs[0]));
					goto cleanup;
				}

				if (!coerceToInt(&vargs[1], &idx))
					goto cleanup;

				if (idx < 0 || idx >= vargs[0].u.aval.nelems)
				{
					pg_log_error("array index " INT64_FORMAT " out of bounds (0 .. %d)",
								 idx, vargs[0].u.aval.nelems - 1);
					goto cleanup;
				}

				copyValue(retval, &vargs[0].u.aval.elems[idx]);
				ok = true;
				goto cleanup;
			}

		case PGBENCH_NURAND:
			{
				int64		A,
							imin,
							imax,
							C = 0,
							delta;

				Assert(nargs == 3 || nargs == 4);

				if (!coerceToInt(&vargs[0], &A) ||
					!coerceToInt(&vargs[1], &imin) ||
					!coerceToInt(&vargs[2], &imax))
					goto cleanup;

				if (A < 0)
				{
					pg_log_error("nurand A parameter must be non-negative (not " INT64_FORMAT ")", A);
					goto cleanup;
				}

				if (unlikely(imin > imax))
				{
					pg_log_error("empty range given to nurand");
					goto cleanup;
				}
				else if (unlikely(pg_sub_s64_overflow(imax, imin, &delta) ||
								  pg_add_s64_overflow(delta, 1, &delta)))
				{
					pg_log_error("nurand range is too large");
					goto cleanup;
				}

				if (nargs == 4)
				{
					if (!coerceToInt(&vargs[3], &C))
						goto cleanup;
				}
				else
				{
					if (!st->nurand_c_initialized)
					{
						st->nurand_c_last = pgbench_random(&st->cs_func_rs, 0, 255);
						st->nurand_c_id = pgbench_random(&st->cs_func_rs, 0, 1023);
						st->nurand_c_ol = pgbench_random(&st->cs_func_rs, 0, 8191);
						st->nurand_c_other = pgbench_random(&st->cs_func_rs, 0, Max(A, 1));
						st->nurand_c_initialized = true;
					}

					if (A == 255)
						C = st->nurand_c_last;
					else if (A == 1023)
						C = st->nurand_c_id;
					else if (A == 8191)
						C = st->nurand_c_ol;
					else
						C = st->nurand_c_other % (A + 1);
				}

				setIntValue(retval, pgbench_nurand(&st->cs_func_rs, A, imin, imax, C));
				ok = true;
				goto cleanup;
			}

		case PGBENCH_C_LAST:
			{
				int64		num;
				char		buf[32];

				Assert(nargs == 1);

				if (!coerceToInt(&vargs[0], &num))
					goto cleanup;

				if (!pgbench_c_last(num, buf, sizeof(buf)))
				{
					pg_log_error("c_last parameter must be in range [0, 999] (not " INT64_FORMAT ")",
								 num);
					goto cleanup;
				}

				setTextValue(retval, buf);
				ok = true;
				goto cleanup;
			}

		case PGBENCH_RANDOM_ASTRING:
		case PGBENCH_RANDOM_NSTRING:
			{
				int64		min_len,
							max_len;
				char	   *buf;

				Assert(nargs == 2);

				if (!coerceToInt(&vargs[0], &min_len) ||
					!coerceToInt(&vargs[1], &max_len))
					goto cleanup;

				if (min_len < 0 || min_len > max_len || max_len > 1048576)
				{
					pg_log_error("invalid string length range [" INT64_FORMAT ", " INT64_FORMAT "]",
								 min_len, max_len);
					goto cleanup;
				}

				buf = pg_malloc((size_t) max_len + 1);
				if (func == PGBENCH_RANDOM_ASTRING)
					pgbench_random_astring(&st->cs_func_rs, min_len, max_len, buf);
				else
					pgbench_random_nstring(&st->cs_func_rs, min_len, max_len, buf);

				retval->type = PGBT_TEXT;
				retval->u.sval = buf;
				ok = true;
				goto cleanup;
			}

		case PGBENCH_FORMAT_STRING:
			{
				char	   *fmt = NULL;
				const char *p;
				PQExpBufferData buf;
				int			arg_idx = 1;

				Assert(nargs >= 1);

				if (!coerceToText(&vargs[0], &fmt))
					goto cleanup;

				initPQExpBuffer(&buf);
				p = fmt;
				while (*p != '\0')
				{
					if (*p != '%')
					{
						appendPQExpBufferChar(&buf, *p++);
						continue;
					}
					if (p[1] == '%')
					{
						appendPQExpBufferChar(&buf, '%');
						p += 2;
						continue;
					}

					/* Parse format specifier: %[flags][width][.prec][len]conv */
					{
						char		spec[64];
						size_t		slen = 0;
						char		conv;

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
						{
							pg_log_error("unterminated format specifier in format_string");
							termPQExpBuffer(&buf);
							pg_free(fmt);
							goto cleanup;
						}
						p++;

						if (arg_idx >= nargs)
						{
							pg_log_error("not enough arguments for format_string");
							termPQExpBuffer(&buf);
							pg_free(fmt);
							goto cleanup;
						}

						if (conv == 'd' || conv == 'i' || conv == 'u' ||
							conv == 'x' || conv == 'X' || conv == 'o')
						{
							int64		ival;

							if (!coerceToInt(&vargs[arg_idx++], &ival))
							{
								termPQExpBuffer(&buf);
								pg_free(fmt);
								goto cleanup;
							}
							spec[slen++] = 'l';
							spec[slen++] = 'l';
							spec[slen++] = (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') ? conv : 'd';
							spec[slen] = '\0';
							appendPQExpBuffer(&buf, spec, (long long) ival);
						}
						else if (conv == 'f' || conv == 'F' || conv == 'e' ||
								 conv == 'E' || conv == 'g' || conv == 'G')
						{
							double		dval;

							if (!coerceToDouble(&vargs[arg_idx++], &dval))
							{
								termPQExpBuffer(&buf);
								pg_free(fmt);
								goto cleanup;
							}
							spec[slen++] = conv;
							spec[slen] = '\0';
							appendPQExpBuffer(&buf, spec, dval);
						}
						else if (conv == 's')
						{
							char	   *sval = NULL;

							if (!coerceToText(&vargs[arg_idx++], &sval))
							{
								termPQExpBuffer(&buf);
								pg_free(fmt);
								goto cleanup;
							}
							spec[slen++] = 's';
							spec[slen] = '\0';
							appendPQExpBuffer(&buf, spec, sval);
							pg_free(sval);
						}
						else
						{
							pg_log_error("unsupported format specifier \"%%%c\" in format_string",
										 conv);
							termPQExpBuffer(&buf);
							pg_free(fmt);
							goto cleanup;
						}
					}
				}

				pg_free(fmt);
				setTextValue(retval, buf.data);
				termPQExpBuffer(&buf);
				ok = true;
				goto cleanup;
			}

		case PGBENCH_TO_TIMESTAMP:
			{
				int64		usec;

				Assert(nargs == 1);

				if (!coerceToTimestamp(&vargs[0], &usec))
					goto cleanup;

				setTimestampValue(retval, usec);
				ok = true;
				goto cleanup;
			}

		case PGBENCH_DATE_ADD:
			{
				int64		base_usec,
							offset,
							res_usec;
				char	   *unit = NULL;

				Assert(nargs == 3);

				if (!coerceToTimestamp(&vargs[0], &base_usec) ||
					!coerceToInt(&vargs[1], &offset) ||
					!coerceToText(&vargs[2], &unit))
					goto cleanup;

				if (!pgbench_date_add(base_usec, offset, unit, &res_usec))
				{
					pg_log_error("invalid unit \"%s\" for date_add", unit);
					pg_free(unit);
					goto cleanup;
				}
				pg_free(unit);

				setTimestampValue(retval, res_usec);
				ok = true;
				goto cleanup;
			}

		case PGBENCH_INT_TO_DATE:
			{
				int64		base_usec,
							val,
							res_usec;

				Assert(nargs == 3 || nargs == 4);

				if (!coerceToTimestamp(&vargs[0], &base_usec) ||
					!coerceToInt(&vargs[1], &val))
					goto cleanup;

				if (nargs == 3)
				{
					char	   *unit = NULL;

					if (!coerceToText(&vargs[2], &unit))
						goto cleanup;

					if (!pgbench_date_add(base_usec, val, unit, &res_usec))
					{
						pg_log_error("invalid unit \"%s\" for int_to_date", unit);
						pg_free(unit);
						goto cleanup;
					}
					pg_free(unit);
				}
				else
				{
					int64		max_val;
					char	   *interval_str = NULL;

					if (!coerceToInt(&vargs[2], &max_val) ||
						!coerceToText(&vargs[3], &interval_str))
						goto cleanup;

					if (!pgbench_int_to_date_window(base_usec, val, max_val,
													interval_str, &res_usec))
					{
						pg_log_error("invalid parameters (max_val=" INT64_FORMAT ", interval=\"%s\") for int_to_date",
									 max_val, interval_str);
						pg_free(interval_str);
						goto cleanup;
					}
					pg_free(interval_str);
				}

				setTimestampValue(retval, res_usec);
				ok = true;
				goto cleanup;
			}

		case PGBENCH_ARRAY:
			{
				setArrayValue(retval, vargs, nargs);
				ok = true;
				goto cleanup;
			}

		default:
			/* cannot get here */
			Assert(0);
			/* dead code to avoid a compiler warning */
			goto cleanup;
	}

cleanup:
	freeValues(vargs, nargs);
	return ok;
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
				copyValue(retval, &expr->u.constant);
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

				copyValue(retval, &var->value);
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

/*
 * Convert command name to meta-command enum identifier
 */
static MetaCommand
getMetaCommand(const char *cmd)
{
	MetaCommand mc;

	if (cmd == NULL)
		mc = META_NONE;
	else if (pg_strcasecmp(cmd, "set") == 0)
		mc = META_SET;
	else if (pg_strcasecmp(cmd, "setshell") == 0)
		mc = META_SETSHELL;
	else if (pg_strcasecmp(cmd, "shell") == 0)
		mc = META_SHELL;
	else if (pg_strcasecmp(cmd, "sleep") == 0)
		mc = META_SLEEP;
	else if (pg_strcasecmp(cmd, "if") == 0)
		mc = META_IF;
	else if (pg_strcasecmp(cmd, "elif") == 0)
		mc = META_ELIF;
	else if (pg_strcasecmp(cmd, "else") == 0)
		mc = META_ELSE;
	else if (pg_strcasecmp(cmd, "endif") == 0)
		mc = META_ENDIF;
	else if (pg_strcasecmp(cmd, "gset") == 0)
		mc = META_GSET;
	else if (pg_strcasecmp(cmd, "aset") == 0)
		mc = META_ASET;
	else if (pg_strcasecmp(cmd, "startpipeline") == 0)
		mc = META_STARTPIPELINE;
	else if (pg_strcasecmp(cmd, "syncpipeline") == 0)
		mc = META_SYNCPIPELINE;
	else if (pg_strcasecmp(cmd, "endpipeline") == 0)
		mc = META_ENDPIPELINE;
	else
		mc = META_NONE;
	return mc;
}

/* return a script number with a weighted choice. */
int
chooseScript(pg_prng_state *random_state)
{
	int			i = 0;
	int64		w;

	if (num_scripts == 1)
		return 0;

	w = pgbench_random(random_state, 0, total_weight - 1);
	do
	{
		w -= sql_script[i++].weight;
	} while (w >= 0);

	return i - 1;
}

/*
 * Replace :param with $n throughout the command's SQL text, which
 * is a modifiable string in cmd->lines.
 */
static bool
parseQuery(Command *cmd)
{
	char	   *sql,
			   *p;

	cmd->argc = 1;

	p = sql = pg_strdup(cmd->lines.data);
	while ((p = strchr(p, ':')) != NULL)
	{
		char		var[13];
		char	   *name;
		int			eaten;

		name = parseVariable(p, &eaten);
		if (name == NULL)
		{
			while (*p == ':')
			{
				p++;
			}
			continue;
		}

		/*
		 * cmd->argv[0] is the SQL statement itself, so the max number of
		 * arguments is one less than MAX_ARGS
		 */
		if (cmd->argc >= MAX_ARGS)
		{
			pg_log_error("statement has too many arguments (maximum is %d): %s",
						 MAX_ARGS - 1, cmd->lines.data);
			pg_free(name);
			return false;
		}

		sprintf(var, "$%d", cmd->argc);
		p = replaceVariable(&sql, p, eaten, var);

		cmd->argv[cmd->argc] = name;
		cmd->argc++;
	}

	Assert(cmd->argv[0] == NULL);
	cmd->argv[0] = sql;
	return true;
}

/*
 * syntax error while parsing a script (in practice, while parsing a
 * backslash command, because we don't detect syntax errors in SQL)
 *
 * source: source of script (filename or builtin-script ID)
 * lineno: line number within script (count from 1)
 * line: whole line of backslash command, if available
 * command: backslash command name, if available
 * msg: the actual error message
 * more: optional extra message
 * column: zero-based column number, or -1 if unknown
 */
void
syntax_error(const char *source, int lineno,
			 const char *line, const char *command,
			 const char *msg, const char *more, int column)
{
	PQExpBufferData buf;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf, "%s:%d: %s", source, lineno, msg);
	if (more != NULL)
		appendPQExpBuffer(&buf, " (%s)", more);
	if (column >= 0 && line == NULL)
		appendPQExpBuffer(&buf, " at column %d", column + 1);
	if (command != NULL)
		appendPQExpBuffer(&buf, " in command \"%s\"", command);

	pg_log_error("%s", buf.data);

	termPQExpBuffer(&buf);

	if (line != NULL)
	{
		fprintf(stderr, "%s\n", line);
		if (column >= 0)
			fprintf(stderr, "%*c error found here\n", column + 1, '^');
	}

	exit(1);
}

/*
 * Return a pointer to the start of the SQL command, after skipping over
 * whitespace and "--" comments.
 * If the end of the string is reached, return NULL.
 */
static char *
skip_sql_comments(char *sql_command)
{
	char	   *p = sql_command;

	/* Skip any leading whitespace, as well as "--" style comments */
	for (;;)
	{
		if (isspace((unsigned char) *p))
			p++;
		else if (strncmp(p, "--", 2) == 0)
		{
			p = strchr(p, '\n');
			if (p == NULL)
				return NULL;
			p++;
		}
		else
			break;
	}

	/* NULL if there's nothing but whitespace and comments */
	if (*p == '\0')
		return NULL;

	return p;
}

/*
 * Parse a SQL command; return a Command struct, or NULL if it's a comment
 *
 * On entry, psqlscan.l has collected the command into "buf", so we don't
 * really need to do much here except check for comments and set up a Command
 * struct.
 */
static Command *
create_sql_command(PQExpBuffer buf)
{
	Command    *my_command;
	char	   *p = skip_sql_comments(buf->data);

	if (p == NULL)
		return NULL;

	/* Allocate and initialize Command structure */
	my_command = pg_malloc0_object(Command);
	initPQExpBuffer(&my_command->lines);
	appendPQExpBufferStr(&my_command->lines, p);
	my_command->first_line = NULL;	/* this is set later */
	my_command->type = SQL_COMMAND;
	my_command->meta = META_NONE;
	my_command->argc = 0;
	my_command->retries = 0;
	my_command->failures = 0;
	memset(my_command->argv, 0, sizeof(my_command->argv));
	my_command->varprefix = NULL;	/* allocated later, if needed */
	my_command->expr = NULL;
	initSimpleStats(&my_command->stats);
	my_command->prepname = NULL;	/* set later, if needed */

	return my_command;
}

/* Free a Command structure and associated data */
static void
free_command(Command *command)
{
	termPQExpBuffer(&command->lines);
	pg_free(command->first_line);
	for (int i = 0; i < command->argc; i++)
		pg_free(command->argv[i]);
	pg_free(command->varprefix);

	/*
	 * It should also free expr recursively, but this is currently not needed
	 * as only gset commands (which do not have an expression) are freed.
	 */
	pg_free(command);
}

/*
 * Once an SQL command is fully parsed, possibly by accumulating several
 * parts, complete other fields of the Command structure.
 */
void
postprocess_sql_command(Command *my_command)
{
	char		buffer[128];
	static int	prepnum = 0;

	Assert(my_command->type == SQL_COMMAND);

	/* Save the first line for error display. */
	strlcpy(buffer, my_command->lines.data, sizeof(buffer));
	buffer[strcspn(buffer, "\n\r")] = '\0';
	my_command->first_line = pg_strdup(buffer);

	/* Parse query and generate prepared statement name, if necessary */
	switch (querymode)
	{
		case QUERY_SIMPLE:
			my_command->argv[0] = my_command->lines.data;
			my_command->argc++;
			break;
		case QUERY_PREPARED:
			my_command->prepname = psprintf("P_%d", prepnum++);
			pg_fallthrough;
		case QUERY_EXTENDED:
			if (!parseQuery(my_command))
				exit(1);
			break;
		default:
			exit(1);
	}
}

/*
 * Parse a backslash command; return a Command struct, or NULL if comment
 *
 * At call, we have scanned only the initial backslash.
 */
static Command *
process_backslash_command(PsqlScanState sstate, const char *source,
						  int lineno, int start_offset)
{
	Command    *my_command;
	PQExpBufferData word_buf;
	int			word_offset;
	int			offsets[MAX_ARGS];	/* offsets of argument words */
	int			j;

	initPQExpBuffer(&word_buf);

	/* Collect first word of command */
	if (!expr_lex_one_word(sstate, &word_buf, &word_offset))
	{
		termPQExpBuffer(&word_buf);
		return NULL;
	}

	/* Allocate and initialize Command structure */
	my_command = pg_malloc0_object(Command);
	my_command->type = META_COMMAND;
	my_command->argc = 0;
	initSimpleStats(&my_command->stats);

	/* Save first word (command name) */
	j = 0;
	offsets[j] = word_offset;
	my_command->argv[j++] = pg_strdup(word_buf.data);
	my_command->argc++;

	/* ... and convert it to enum form */
	my_command->meta = getMetaCommand(my_command->argv[0]);

	if (my_command->meta == META_SET ||
		my_command->meta == META_IF ||
		my_command->meta == META_ELIF)
	{
		yyscan_t	yyscanner;

		/* For \set, collect var name (or var[index_expr]) */
		if (my_command->meta == META_SET)
		{
			char	   *lb;

			if (!expr_lex_one_word(sstate, &word_buf, &word_offset))
				syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
							 "missing argument", NULL, -1);

			offsets[j] = word_offset;

			if (strchr(word_buf.data, '[') != NULL)
			{
				PQExpBufferData target_buf;
				int			depth = 0;

				initPQExpBuffer(&target_buf);
				appendPQExpBufferStr(&target_buf, word_buf.data);

				for (const char *c = target_buf.data; *c != '\0'; c++)
				{
					if (*c == '[')
						depth++;
					else if (*c == ']')
						depth--;
				}

				while (depth > 0)
				{
					int			next_offset;

					if (!expr_lex_one_word(sstate, &word_buf, &next_offset))
						syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
									 "unterminated array subscript in \\set target", NULL, -1);
					appendPQExpBuffer(&target_buf, " %s", word_buf.data);
					for (const char *c = word_buf.data; *c != '\0'; c++)
					{
						if (*c == '[')
							depth++;
						else if (*c == ']')
							depth--;
					}
				}

				lb = strchr(target_buf.data, '[');
				if (lb == NULL || target_buf.data[target_buf.len - 1] != ']')
					syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
								 "invalid array subscript in \\set target",
								 target_buf.data, -1);

				*lb = '\0';
				target_buf.data[target_buf.len - 1] = '\0';

				my_command->argv[j++] = pg_strdup(target_buf.data);
				my_command->argc++;

				/* Parse the index expression inside [...] */
				{
					PsqlScanState idx_sstate;
					yyscan_t	idx_yyscanner;
					char	   *idx_str = psprintf("%s\n", lb + 1);

					idx_sstate = psql_scan_create(&pgbench_callbacks);
					psql_scan_setup(idx_sstate, idx_str, strlen(idx_str), 0, true);
					idx_yyscanner = expr_scanner_init(idx_sstate, source, lineno, 0,
													  my_command->argv[0]);
					if (expr_yyparse(&my_command->index_expr, idx_yyscanner) != 0)
						exit(1);
					expr_scanner_finish(idx_yyscanner);
					psql_scan_finish(idx_sstate);
					psql_scan_destroy(idx_sstate);
					pg_free(idx_str);
				}

				termPQExpBuffer(&target_buf);
			}
			else
			{
				my_command->argv[j++] = pg_strdup(word_buf.data);
				my_command->argc++;
			}
		}

		/* then for all parse the expression */
		yyscanner = expr_scanner_init(sstate, source, lineno, start_offset,
									  my_command->argv[0]);

		if (expr_yyparse(&my_command->expr, yyscanner) != 0)
		{
			/* dead code: exit done from syntax_error called by yyerror */
			exit(1);
		}

		/* Save line, trimming any trailing newline */
		my_command->first_line =
			expr_scanner_get_substring(sstate,
									   start_offset,
									   true);

		expr_scanner_finish(yyscanner);

		termPQExpBuffer(&word_buf);

		return my_command;
	}

	/* For all other commands, collect remaining words. */
	while (expr_lex_one_word(sstate, &word_buf, &word_offset))
	{
		/*
		 * my_command->argv[0] is the command itself, so the max number of
		 * arguments is one less than MAX_ARGS
		 */
		if (j >= MAX_ARGS)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL, -1);

		offsets[j] = word_offset;
		my_command->argv[j++] = pg_strdup(word_buf.data);
		my_command->argc++;
	}

	/* Save line, trimming any trailing newline */
	my_command->first_line =
		expr_scanner_get_substring(sstate,
								   start_offset,
								   true);

	if (my_command->meta == META_SLEEP)
	{
		if (my_command->argc < 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing argument", NULL, -1);

		if (my_command->argc > 3)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL,
						 offsets[3] - start_offset);

		/*
		 * Split argument into number and unit to allow "sleep 1ms" etc. We
		 * don't have to terminate the number argument with null because it
		 * will be parsed with atoi, which ignores trailing non-digit
		 * characters.
		 */
		if (my_command->argv[1][0] != ':')
		{
			char	   *c = my_command->argv[1];
			bool		have_digit = false;

			/* Skip sign */
			if (*c == '+' || *c == '-')
				c++;

			/* Require at least one digit */
			if (*c && isdigit((unsigned char) *c))
				have_digit = true;

			/* Eat all digits */
			while (*c && isdigit((unsigned char) *c))
				c++;

			if (*c)
			{
				if (my_command->argc == 2 && have_digit)
				{
					my_command->argv[2] = c;
					offsets[2] = offsets[1] + (c - my_command->argv[1]);
					my_command->argc = 3;
				}
				else
				{
					/*
					 * Raise an error if argument starts with non-digit
					 * character (after sign).
					 */
					syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
								 "invalid sleep time, must be an integer",
								 my_command->argv[1], offsets[1] - start_offset);
				}
			}
		}

		if (my_command->argc == 3)
		{
			if (pg_strcasecmp(my_command->argv[2], "us") != 0 &&
				pg_strcasecmp(my_command->argv[2], "ms") != 0 &&
				pg_strcasecmp(my_command->argv[2], "s") != 0)
				syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
							 "unrecognized time unit, must be us, ms or s",
							 my_command->argv[2], offsets[2] - start_offset);
		}
	}
	else if (my_command->meta == META_SETSHELL)
	{
		if (my_command->argc < 3)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing argument", NULL, -1);
	}
	else if (my_command->meta == META_SHELL)
	{
		if (my_command->argc < 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing command", NULL, -1);
	}
	else if (my_command->meta == META_ELSE || my_command->meta == META_ENDIF ||
			 my_command->meta == META_STARTPIPELINE ||
			 my_command->meta == META_ENDPIPELINE ||
			 my_command->meta == META_SYNCPIPELINE)
	{
		if (my_command->argc != 1)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "unexpected argument", NULL, -1);
	}
	else if (my_command->meta == META_GSET || my_command->meta == META_ASET)
	{
		if (my_command->argc > 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL, -1);
	}
	else
	{
		/* my_command->meta == META_NONE */
		syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
					 "invalid command", NULL, -1);
	}

	termPQExpBuffer(&word_buf);

	return my_command;
}

static void
ConditionError(const char *desc, int cmdn, const char *msg)
{
	pg_fatal("condition error in script \"%s\" command %d: %s",
			 desc, cmdn, msg);
}

/*
 * Partial evaluation of conditionals before recording and running the script.
 */
static void
CheckConditional(const ParsedScript *ps)
{
	/* statically check conditional structure */
	ConditionalStack cs = conditional_stack_create();
	int			i;

	for (i = 0; ps->commands[i] != NULL; i++)
	{
		Command    *cmd = ps->commands[i];

		if (cmd->type == META_COMMAND)
		{
			switch (cmd->meta)
			{
				case META_IF:
					conditional_stack_push(cs, IFSTATE_FALSE);
					break;
				case META_ELIF:
					if (conditional_stack_empty(cs))
						ConditionError(ps->desc, i + 1, "\\elif without matching \\if");
					if (conditional_stack_peek(cs) == IFSTATE_ELSE_FALSE)
						ConditionError(ps->desc, i + 1, "\\elif after \\else");
					break;
				case META_ELSE:
					if (conditional_stack_empty(cs))
						ConditionError(ps->desc, i + 1, "\\else without matching \\if");
					if (conditional_stack_peek(cs) == IFSTATE_ELSE_FALSE)
						ConditionError(ps->desc, i + 1, "\\else after \\else");
					conditional_stack_poke(cs, IFSTATE_ELSE_FALSE);
					break;
				case META_ENDIF:
					if (!conditional_stack_pop(cs))
						ConditionError(ps->desc, i + 1, "\\endif without matching \\if");
					break;
				default:
					/* ignore anything else... */
					break;
			}
		}
	}
	if (!conditional_stack_empty(cs))
		ConditionError(ps->desc, i + 1, "\\if without matching \\endif");
	conditional_stack_destroy(cs);
}

/*
 * Parse a script (either the contents of a file, or a built-in script)
 * and add it to the list of scripts.
 */
static void
ParseScript(const char *script, const char *desc, int weight)
{
	ParsedScript ps;
	PsqlScanState sstate;
	PQExpBufferData line_buf;
	int			alloc_num;
	int			index;

#define COMMANDS_ALLOC_NUM 128
	alloc_num = COMMANDS_ALLOC_NUM;

	/* Initialize all fields of ps */
	ps.desc = desc;
	ps.weight = weight;
	ps.commands = pg_malloc_array(Command *, alloc_num);
	initStats(&ps.stats, 0);

	/* Prepare to parse script */
	sstate = psql_scan_create(&pgbench_callbacks);

	/*
	 * Ideally, we'd scan scripts using the encoding and stdstrings settings
	 * we get from a DB connection.  However, without major rearrangement of
	 * pgbench's argument parsing, we can't have a DB connection at the time
	 * we parse scripts.  Using SQL_ASCII (encoding 0) should work well enough
	 * with any backend-safe encoding, though conceivably we could be fooled
	 * if a script file uses a client-only encoding.  We also assume that
	 * stdstrings should be true, which is a bit riskier.
	 */
	psql_scan_setup(sstate, script, strlen(script), 0, true);

	initPQExpBuffer(&line_buf);

	index = 0;

	for (;;)
	{
		PsqlScanResult sr;
		promptStatus_t prompt;
		Command    *command = NULL;

		resetPQExpBuffer(&line_buf);

		sr = psql_scan(sstate, &line_buf, &prompt);

		/* If we collected a new SQL command, process that */
		command = create_sql_command(&line_buf);

		/* store new command */
		if (command)
			ps.commands[index++] = command;

		/* If we reached a backslash, process that */
		if (sr == PSCAN_BACKSLASH)
		{
			int			lineno;
			int			start_offset;

			/* Capture location of the backslash */
			psql_scan_get_location(sstate, &lineno, &start_offset);
			start_offset--;

			command = process_backslash_command(sstate, desc,
												lineno, start_offset);

			if (command)
			{
				/*
				 * If this is gset or aset, merge into the preceding command.
				 * (We don't use a command slot in this case).
				 */
				if (command->meta == META_GSET || command->meta == META_ASET)
				{
					Command    *cmd;

					if (index == 0)
						syntax_error(desc, lineno, NULL, NULL,
									 "\\gset must follow an SQL command",
									 NULL, -1);

					cmd = ps.commands[index - 1];

					if (cmd->type != SQL_COMMAND ||
						cmd->varprefix != NULL)
						syntax_error(desc, lineno, NULL, NULL,
									 "\\gset must follow an SQL command",
									 cmd->first_line, -1);

					/* get variable prefix */
					if (command->argc <= 1 || command->argv[1][0] == '\0')
						cmd->varprefix = pg_strdup("");
					else
						cmd->varprefix = pg_strdup(command->argv[1]);

					/* update the sql command meta */
					cmd->meta = command->meta;

					/* cleanup unused command */
					free_command(command);

					continue;
				}

				/* Attach any other backslash command as a new command */
				ps.commands[index++] = command;
			}
		}

		/*
		 * Since we used a command slot, allocate more if needed.  Note we
		 * always allocate one more in order to accommodate the NULL
		 * terminator below.
		 */
		if (index >= alloc_num)
		{
			alloc_num += COMMANDS_ALLOC_NUM;
			ps.commands = (Command **)
				pg_realloc_array(ps.commands, Command *, alloc_num);
		}

		/* Done if we reached EOF */
		if (sr == PSCAN_INCOMPLETE || sr == PSCAN_EOL)
			break;
	}

	ps.commands[index] = NULL;

	addScript(&ps);

	termPQExpBuffer(&line_buf);
	psql_scan_finish(sstate);
	psql_scan_destroy(sstate);
}

/*
 * Read the entire contents of file fd, and return it in a malloc'd buffer.
 *
 * The buffer will typically be larger than necessary, but we don't care
 * in this program, because we'll free it as soon as we've parsed the script.
 */
static char *
read_file_contents(FILE *fd)
{
	char	   *buf;
	size_t		buflen = BUFSIZ;
	size_t		used = 0;

	buf = (char *) pg_malloc(buflen);

	for (;;)
	{
		size_t		nread;

		nread = fread(buf + used, 1, BUFSIZ, fd);
		used += nread;
		/* If fread() read less than requested, must be EOF or error */
		if (nread < BUFSIZ)
			break;
		/* Enlarge buf so we can read some more */
		buflen += BUFSIZ;
		buf = (char *) pg_realloc(buf, buflen);
	}
	/* There is surely room for a terminator */
	buf[used] = '\0';

	return buf;
}

/*
 * Given a file name, read it and add its script to the list.
 * "-" means to read stdin.
 * NB: filename must be storage that won't disappear.
 */
void
process_file(const char *filename, int weight)
{
	FILE	   *fd;
	char	   *buf;

	/* Slurp the file contents into "buf" */
	if (strcmp(filename, "-") == 0)
		fd = stdin;
	else if ((fd = fopen(filename, "r")) == NULL)
		pg_fatal("could not open file \"%s\": %m", filename);

	buf = read_file_contents(fd);

	if (ferror(fd))
		pg_fatal("could not read file \"%s\": %m", filename);

	if (fd != stdin)
		fclose(fd);

	ParseScript(buf, filename, weight);

	free(buf);
}

/* Parse the given builtin script and add it to the list. */
void
process_builtin(const BuiltinScript *bi, int weight)
{
	ParseScript(bi->script, bi->desc, weight);
}

/* show available builtin scripts */
void
listAvailableScripts(void)
{
	fprintf(stderr, "Available builtin scripts:\n");
	for (size_t i = 0; i < lengthof(builtin_script); i++)
		fprintf(stderr, "  %13s: %s\n", builtin_script[i].name, builtin_script[i].desc);
	fprintf(stderr, "\n");
}

/* return builtin script "name" if unambiguous, fails if not found */
const BuiltinScript *
findBuiltin(const char *name)
{
	int			found = 0,
				len = strlen(name);
	const BuiltinScript *result = NULL;

	for (size_t i = 0; i < lengthof(builtin_script); i++)
	{
		if (strncmp(builtin_script[i].name, name, len) == 0)
		{
			result = &builtin_script[i];
			found++;
		}
	}

	/* ok, unambiguous result */
	if (found == 1)
		return result;

	/* error cases */
	if (found == 0)
		pg_log_error("no builtin script found for name \"%s\"", name);
	else						/* found > 1 */
		pg_log_error("ambiguous builtin name: %d builtin scripts found for prefix \"%s\"", found, name);

	listAvailableScripts();
	exit(1);
}

/*
 * Determine the weight specification from a script option (-b, -f), if any,
 * and return it as an integer (1 is returned if there's no weight).  The
 * script name is returned in *script as a malloc'd string.
 */
int
parseScriptWeight(const char *option, char **script)
{
	const char *sep;
	int			weight;

	if ((sep = strrchr(option, WSEP)))
	{
		int			namelen = sep - option;
		long		wtmp;
		char	   *badp;

		/* generate the script name */
		*script = pg_malloc(namelen + 1);
		strncpy(*script, option, namelen);
		(*script)[namelen] = '\0';

		/* process digits of the weight spec */
		errno = 0;
		wtmp = strtol(sep + 1, &badp, 10);
		if (errno != 0 || badp == sep + 1 || *badp != '\0')
			pg_fatal("invalid weight specification: %s", sep);
		if (wtmp > INT_MAX || wtmp < 0)
			pg_fatal("weight specification out of range (0 .. %d): %ld",
					 INT_MAX, wtmp);
		weight = wtmp;
	}
	else
	{
		*script = pg_strdup(option);
		weight = 1;
	}

	return weight;
}

/* append a script to the list of scripts to process */
static void
addScript(const ParsedScript *script)
{
	if (script->commands == NULL || script->commands[0] == NULL)
		pg_fatal("empty command list for script \"%s\"", script->desc);

	if (num_scripts >= MAX_SCRIPTS)
		pg_fatal("at most %d SQL scripts are allowed", MAX_SCRIPTS);

	CheckConditional(script);

	sql_script[num_scripts] = *script;
	num_scripts++;
}
