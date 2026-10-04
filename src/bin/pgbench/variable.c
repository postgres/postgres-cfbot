/*-------------------------------------------------------------------------
 *
 * variable.c
 *	  Variable management and value coercion for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/variable.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <float.h>
#include <math.h>

#include "common/int.h"
#include "common/logging.h"
#include "variable.h"

/*
 * Return whether str matches "^\s*[-+]?[0-9]+$"
 *
 * This should agree with strtoint64() on what's accepted, ignoring overflows.
 */
static bool
is_an_int(const char *str)
{
	const char *ptr = str;

	/* skip leading spaces */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;

	/* skip sign */
	if (*ptr == '+' || *ptr == '-')
		ptr++;

	/* at least one digit */
	if (*ptr && !isdigit((unsigned char) *ptr))
		return false;

	/* eat all digits */
	while (*ptr && isdigit((unsigned char) *ptr))
		ptr++;

	/* must have reached end of string */
	return *ptr == '\0';
}


/*
 * strtoint64 -- convert a string to 64-bit integer
 *
 * The function returns whether the conversion worked, and if so
 * "*result" is set to the result.
 *
 * If not errorOK, an error message is also printed out on errors.
 */
bool
strtoint64(const char *str, bool errorOK, int64 *result)
{
	char	   *end;

	errno = 0;
	*result = strtoi64(str, &end, 10);

	if (unlikely(errno == ERANGE))
	{
		if (!errorOK)
			pg_log_error("value \"%s\" is out of range for type bigint", str);
		return false;
	}

	if (unlikely(errno != 0 || end == str || *end != '\0'))
	{
		if (!errorOK)
			pg_log_error("invalid input syntax for type bigint: \"%s\"", str);
		return false;
	}
	return true;
}

/* convert string to double, detecting overflows/underflows */
bool
strtodouble(const char *str, bool errorOK, double *dv)
{
	char	   *end;

	errno = 0;
	*dv = strtod(str, &end);

	if (unlikely(errno == ERANGE))
	{
		if (!errorOK)
			pg_log_error("value \"%s\" is out of range for type double", str);
		return false;
	}

	if (unlikely(errno != 0 || end == str || *end != '\0'))
	{
		if (!errorOK)
			pg_log_error("invalid input syntax for type double: \"%s\"", str);
		return false;
	}
	return true;
}

/* qsort comparator for Variable array */
static int
compareVariableNames(const void *v1, const void *v2)
{
	return strcmp(((const Variable *) v1)->name,
				  ((const Variable *) v2)->name);
}

/* Locate a variable by name; returns NULL if unknown */
Variable *
lookupVariable(Variables *variables, char *name)
{
	Variable	key;

	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (variables->nvars <= 0)
		return NULL;

	/* Sort if we have to */
	if (!variables->vars_sorted)
	{
		qsort(variables->vars, variables->nvars, sizeof(Variable),
			  compareVariableNames);
		variables->vars_sorted = true;
	}

	/* Now we can search */
	key.name = name;
	return (Variable *) bsearch(&key,
								variables->vars,
								variables->nvars,
								sizeof(Variable),
								compareVariableNames);
}

/* Get the value of a variable, in string form; returns NULL if unknown */
char *
getVariable(Variables *variables, char *name)
{
	Variable   *var;
	char		stringform[64];

	var = lookupVariable(variables, name);
	if (var == NULL)
		return NULL;			/* not found */

	if (var->svalue)
		return var->svalue;		/* we have it in string form */

	/* We need to produce a string equivalent of the value */
	Assert(var->value.type != PGBT_NO_VALUE);
	if (var->value.type == PGBT_NULL)
		snprintf(stringform, sizeof(stringform), "NULL");
	else if (var->value.type == PGBT_BOOLEAN)
		snprintf(stringform, sizeof(stringform),
				 "%s", var->value.u.bval ? "true" : "false");
	else if (var->value.type == PGBT_INT)
		snprintf(stringform, sizeof(stringform),
				 INT64_FORMAT, var->value.u.ival);
	else if (var->value.type == PGBT_DOUBLE)
		snprintf(stringform, sizeof(stringform),
				 "%.*g", DBL_DIG, var->value.u.dval);
	else						/* internal error, unexpected type */
		Assert(0);
	var->svalue = pg_strdup(stringform);
	return var->svalue;
}

/* Try to convert variable to a value; return false on failure */
bool
makeVariableValue(Variable *var)
{
	size_t		slen;

	if (var->value.type != PGBT_NO_VALUE)
		return true;			/* no work */

	slen = strlen(var->svalue);

	if (slen == 0)
		/* what should it do on ""? */
		return false;

	if (pg_strcasecmp(var->svalue, "null") == 0)
	{
		setNullValue(&var->value);
	}

	/*
	 * accept prefixes such as y, ye, n, no... but not for "o". 0/1 are
	 * recognized later as an int, which is converted to bool if needed.
	 */
	else if (pg_strncasecmp(var->svalue, "true", slen) == 0 ||
			 pg_strncasecmp(var->svalue, "yes", slen) == 0 ||
			 pg_strcasecmp(var->svalue, "on") == 0)
	{
		setBoolValue(&var->value, true);
	}
	else if (pg_strncasecmp(var->svalue, "false", slen) == 0 ||
			 pg_strncasecmp(var->svalue, "no", slen) == 0 ||
			 pg_strcasecmp(var->svalue, "off") == 0 ||
			 pg_strcasecmp(var->svalue, "of") == 0)
	{
		setBoolValue(&var->value, false);
	}
	else if (is_an_int(var->svalue))
	{
		/* if it looks like an int, it must be an int without overflow */
		int64		iv;

		if (!strtoint64(var->svalue, false, &iv))
			return false;

		setIntValue(&var->value, iv);
	}
	else						/* type should be double */
	{
		double		dv;

		if (!strtodouble(var->svalue, true, &dv))
		{
			pg_log_error("malformed variable \"%s\" value: \"%s\"",
						 var->name, var->svalue);
			return false;
		}
		setDoubleValue(&var->value, dv);
	}
	return true;
}

/*
 * Check whether a variable's name is allowed.
 *
 * We allow any non-ASCII character, as well as ASCII letters, digits, and
 * underscore.
 *
 * Keep this in sync with the definitions of variable name characters in
 * "src/fe_utils/psqlscan.l", "src/bin/psql/psqlscanslash.l" and
 * "src/bin/pgbench/exprscan.l".  Also see parseVariable(), below.
 *
 * Note: this static function is copied from "src/bin/psql/variables.c"
 * but changed to disallow variable names starting with a digit.
 */
static bool
valid_variable_name(const char *name)
{
	const unsigned char *ptr = (const unsigned char *) name;

	/* Mustn't be zero-length */
	if (*ptr == '\0')
		return false;

	/* must not start with [0-9] */
	if (IS_HIGHBIT_SET(*ptr) ||
		strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
			   "_", *ptr) != NULL)
		ptr++;
	else
		return false;

	/* remaining characters can include [0-9] */
	while (*ptr)
	{
		if (IS_HIGHBIT_SET(*ptr) ||
			strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
				   "_0123456789", *ptr) != NULL)
			ptr++;
		else
			return false;
	}

	return true;
}

/*
 * Make sure there is enough space for 'needed' more variable in the variables
 * array.
 */
static void
enlargeVariables(Variables *variables, int needed)
{
	/* total number of variables required now */
	needed += variables->nvars;

	if (variables->max_vars < needed)
	{
		variables->max_vars = needed + VARIABLES_ALLOC_MARGIN;
		variables->vars = (Variable *)
			pg_realloc_array(variables->vars, Variable, variables->max_vars);
	}
}

/*
 * Lookup a variable by name, creating it if need be.
 * Caller is expected to assign a value to the variable.
 * Returns NULL on failure (bad name).
 */
static Variable *
lookupCreateVariable(Variables *variables, const char *context, char *name)
{
	Variable   *var;

	var = lookupVariable(variables, name);
	if (var == NULL)
	{
		/*
		 * Check for the name only when declaring a new variable to avoid
		 * overhead.
		 */
		if (!valid_variable_name(name))
		{
			pg_log_error("%s: invalid variable name: \"%s\"", context, name);
			return NULL;
		}

		/* Create variable at the end of the array */
		enlargeVariables(variables, 1);

		var = &(variables->vars[variables->nvars]);

		var->name = pg_strdup(name);
		var->svalue = NULL;
		/* caller is expected to initialize remaining fields */

		variables->nvars++;
		/* we don't re-sort the array till we have to */
		variables->vars_sorted = false;
	}

	return var;
}

/* Assign a string value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
bool
putVariable(Variables *variables, const char *context, char *name,
			const char *value)
{
	Variable   *var;
	char	   *val;

	var = lookupCreateVariable(variables, context, name);
	if (!var)
		return false;

	/* dup then free, in case value is pointing at this variable */
	val = pg_strdup(value);

	free(var->svalue);
	var->svalue = val;
	var->value.type = PGBT_NO_VALUE;

	return true;
}

/* Assign a value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
bool
putVariableValue(Variables *variables, const char *context, char *name,
				 const PgBenchValue *value)
{
	Variable   *var;

	var = lookupCreateVariable(variables, context, name);
	if (!var)
		return false;

	free(var->svalue);
	var->svalue = NULL;
	var->value = *value;

	return true;
}

/* Assign an integer value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
bool
putVariableInt(Variables *variables, const char *context, char *name,
			   int64 value)
{
	PgBenchValue val;

	setIntValue(&val, value);
	return putVariableValue(variables, context, name, &val);
}

/*
 * Parse a possible variable reference (:varname).
 *
 * "sql" points at a colon.  If what follows it looks like a valid
 * variable name, return a malloc'd string containing the variable name,
 * and set *eaten to the number of characters consumed (including the colon).
 * Otherwise, return NULL.
 */
char *
parseVariable(const char *sql, int *eaten)
{
	int			i = 1;			/* starting at 1 skips the colon */
	char	   *name;

	/* keep this logic in sync with valid_variable_name() */
	if (IS_HIGHBIT_SET(sql[i]) ||
		strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
			   "_", sql[i]) != NULL)
		i++;
	else
		return NULL;

	while (IS_HIGHBIT_SET(sql[i]) ||
		   strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
				  "_0123456789", sql[i]) != NULL)
		i++;

	name = pg_malloc(i);
	memcpy(name, &sql[1], i - 1);
	name[i - 1] = '\0';

	*eaten = i;
	return name;
}

char *
replaceVariable(char **sql, char *param, int len, char *value)
{
	int			valueln = strlen(value);

	if (valueln > len)
	{
		size_t		offset = param - *sql;

		*sql = pg_realloc(*sql, strlen(*sql) - len + valueln + 1);
		param = *sql + offset;
	}

	if (valueln != len)
		memmove(param + valueln, param + len, strlen(param + len) + 1);
	memcpy(param, value, valueln);

	return param + valueln;
}

char *
assignVariables(Variables *variables, char *sql)
{
	char	   *p,
			   *name,
			   *val;

	p = sql;
	while ((p = strchr(p, ':')) != NULL)
	{
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

		val = getVariable(variables, name);
		free(name);
		if (val == NULL)
		{
			p++;
			continue;
		}

		p = replaceVariable(&sql, p, eaten, val);
	}

	return sql;
}

static char *
valueTypeName(PgBenchValue *pval)
{
	if (pval->type == PGBT_NO_VALUE)
		return "none";
	else if (pval->type == PGBT_NULL)
		return "null";
	else if (pval->type == PGBT_INT)
		return "int";
	else if (pval->type == PGBT_DOUBLE)
		return "double";
	else if (pval->type == PGBT_BOOLEAN)
		return "boolean";
	else
	{
		/* internal error, should never get there */
		Assert(false);
		return NULL;
	}
}

/* get a value as a boolean, or tell if there is a problem */
bool
coerceToBool(PgBenchValue *pval, bool *bval)
{
	if (pval->type == PGBT_BOOLEAN)
	{
		*bval = pval->u.bval;
		return true;
	}
	else						/* NULL, INT or DOUBLE */
	{
		pg_log_error("cannot coerce %s to boolean", valueTypeName(pval));
		*bval = false;			/* suppress uninitialized-variable warnings */
		return false;
	}
}

/*
 * Return true or false from an expression for conditional purposes.
 * Non zero numerical values are true, zero and NULL are false.
 */
bool
valueTruth(PgBenchValue *pval)
{
	switch (pval->type)
	{
		case PGBT_NULL:
			return false;
		case PGBT_BOOLEAN:
			return pval->u.bval;
		case PGBT_INT:
			return pval->u.ival != 0;
		case PGBT_DOUBLE:
			return pval->u.dval != 0.0;
		default:
			/* internal error, unexpected type */
			Assert(0);
			return false;
	}
}

/* get a value as an int, tell if there is a problem */
bool
coerceToInt(PgBenchValue *pval, int64 *ival)
{
	if (pval->type == PGBT_INT)
	{
		*ival = pval->u.ival;
		return true;
	}
	else if (pval->type == PGBT_DOUBLE)
	{
		double		dval = rint(pval->u.dval);

		if (isnan(dval) || !FLOAT8_FITS_IN_INT64(dval))
		{
			pg_log_error("double to int overflow for %f", dval);
			return false;
		}
		*ival = (int64) dval;
		return true;
	}
	else						/* BOOLEAN or NULL */
	{
		pg_log_error("cannot coerce %s to int", valueTypeName(pval));
		return false;
	}
}

/* get a value as a double, or tell if there is a problem */
bool
coerceToDouble(PgBenchValue *pval, double *dval)
{
	if (pval->type == PGBT_DOUBLE)
	{
		*dval = pval->u.dval;
		return true;
	}
	else if (pval->type == PGBT_INT)
	{
		*dval = (double) pval->u.ival;
		return true;
	}
	else						/* BOOLEAN or NULL */
	{
		pg_log_error("cannot coerce %s to double", valueTypeName(pval));
		return false;
	}
}

/* assign a null value */
void
setNullValue(PgBenchValue *pv)
{
	pv->type = PGBT_NULL;
	pv->u.ival = 0;
}

/* assign a boolean value */
void
setBoolValue(PgBenchValue *pv, bool bval)
{
	pv->type = PGBT_BOOLEAN;
	pv->u.bval = bval;
}

/* assign an integer value */
void
setIntValue(PgBenchValue *pv, int64 ival)
{
	pv->type = PGBT_INT;
	pv->u.ival = ival;
}

/* assign a double value */
void
setDoubleValue(PgBenchValue *pv, double dval)
{
	pv->type = PGBT_DOUBLE;
	pv->u.dval = dval;
}
