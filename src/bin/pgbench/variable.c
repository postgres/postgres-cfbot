/*-------------------------------------------------------------------------
 *
 * variable.c
 *		Scoped variable store and value evaluation support for pgbench
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
#include <errno.h>
#include <float.h>
#include <math.h>

#include "common/fe_memutils.h"
#include "common/logging.h"
#include "common/string.h"
#include "port.h"
#include "variable.h"

#define VARIABLES_ALLOC_MARGIN	8

/*
 * Helper to ensure variables has a valid root and current scope.
 */
static inline VariableScope *
getCurrentScope(Variables *variables)
{
	if (unlikely(variables->root == NULL))
	{
		variables->root = (VariableScope *) pg_malloc0(sizeof(VariableScope));
		variables->root->vars_sorted = true;
		variables->current = variables->root;
	}
	return variables->current;
}

/*
 * Initialize a Variables container to empty with an active root scope.
 */
void
initVariables(Variables *variables)
{
	variables->root = (VariableScope *) pg_malloc0(sizeof(VariableScope));
	variables->root->vars_sorted = true;
	variables->current = variables->root;
}

/*
 * Destroy all variables across all scopes, freeing memory.
 */
void
destroyVariables(Variables *variables)
{
	VariableScope *scope;
	int			i;

	while (var_scope_pop(variables))
		;

	if (variables->root)
	{
		scope = variables->root;
		for (i = 0; i < scope->nvars; i++)
		{
			free(scope->vars[i].name);
			free(scope->vars[i].svalue);
		}
		if (scope->vars)
			free(scope->vars);
		free(scope);
		variables->root = NULL;
		variables->current = NULL;
	}
}

/*
 * Push a new child scope onto the stack.
 */
bool
var_scope_push(Variables *variables)
{
	VariableScope *current = getCurrentScope(variables);
	VariableScope *new_scope = (VariableScope *) pg_malloc0(sizeof(VariableScope));

	new_scope->parent = current;
	new_scope->vars_sorted = true;
	variables->current = new_scope;
	return true;
}

/*
 * Pop the current scope from the stack, freeing all variables declared in it.
 * Root scope cannot be popped; returns false if at root.
 */
bool
var_scope_pop(Variables *variables)
{
	VariableScope *current;
	int			i;

	if (variables->root == NULL || variables->current == NULL)
		return false;

	current = variables->current;
	if (current == variables->root || current->parent == NULL)
		return false;

	variables->current = current->parent;

	for (i = 0; i < current->nvars; i++)
	{
		free(current->vars[i].name);
		free(current->vars[i].svalue);
	}
	if (current->vars)
		free(current->vars);
	free(current);

	return true;
}

/*
 * Deep copy all variables from the current scope of src into dest.
 */
bool
copyVariables(Variables *dest, const Variables *src)
{
	const VariableScope *src_scope;
	int			j;

	if (src == NULL)
		return true;

	src_scope = (src->current != NULL) ? src->current : src->root;
	if (src_scope == NULL)
		return true;

	for (j = 0; j < src_scope->nvars; j++)
	{
		const Variable *var = &src_scope->vars[j];

		if (var->value.type != PGBT_NO_VALUE)
		{
			if (!putVariableValue(dest, "startup", var->name, &var->value))
				return false;
		}
		else
		{
			if (!putVariable(dest, "startup", var->name, var->svalue))
				return false;
		}
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

/* Locate a variable by name within a single scope */
static Variable *
lookupVariableInScope(VariableScope *scope, const char *name)
{
	Variable	key;

	if (scope->nvars <= 0)
		return NULL;

	if (!scope->vars_sorted)
	{
		qsort(scope->vars, scope->nvars, sizeof(Variable),
			  compareVariableNames);
		scope->vars_sorted = true;
	}

	key.name = unconstify(char *, name);
	return (Variable *) bsearch(&key,
								scope->vars,
								scope->nvars,
								sizeof(Variable),
								compareVariableNames);
}

/*
 * Locate a variable by name searching upwards through the scope stack.
 * Returns NULL if unknown in all scopes.
 */
Variable *
lookupVariable(Variables *variables, const char *name)
{
	VariableScope *scope = getCurrentScope(variables);

	while (scope != NULL)
	{
		Variable   *var = lookupVariableInScope(scope, name);

		if (var != NULL)
			return var;
		scope = scope->parent;
	}
	return NULL;
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
 * Make sure there is enough space for 'needed' more variable in the scope.
 */
static void
enlargeVariables(VariableScope *scope, int needed)
{
	needed += scope->nvars;

	if (scope->max_vars < needed)
	{
		scope->max_vars = needed + VARIABLES_ALLOC_MARGIN;
		scope->vars = (Variable *)
			pg_realloc_array(scope->vars, Variable, scope->max_vars);
	}
}

/*
 * Lookup a variable by name, creating it in the current scope if need be.
 * Caller is expected to assign a value to the variable.
 * Returns NULL on failure (bad name).
 */
Variable *
lookupCreateVariable(Variables *variables, const char *context, const char *name)
{
	Variable   *var;
	VariableScope *current = getCurrentScope(variables);

	/* Check if it exists anywhere in the scope chain */
	var = lookupVariable(variables, name);
	if (var == NULL)
	{
		if (!valid_variable_name(name))
		{
			pg_log_error("%s: invalid variable name: \"%s\"", context, name);
			return NULL;
		}

		/* Create variable in current scope */
		enlargeVariables(current, 1);

		var = &(current->vars[current->nvars]);

		var->name = pg_strdup(name);
		var->svalue = NULL;
		var->value.type = PGBT_NO_VALUE;

		current->nvars++;
		current->vars_sorted = false;
	}

	return var;
}

/*
 * Get the value of a variable, in string form; returns NULL if unknown.
 */
char *
getVariable(Variables *variables, const char *name)
{
	Variable   *var;
	char		stringform[64];

	var = lookupVariable(variables, name);
	if (var == NULL)
		return NULL;

	if (var->svalue)
		return var->svalue;

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
	else
		Assert(0);
	var->svalue = pg_strdup(stringform);
	return var->svalue;
}

/*
 * Try to convert variable string form to a PgBenchValue; return false on failure.
 */
bool
makeVariableValue(Variable *var)
{
	size_t		slen;

	if (var->value.type != PGBT_NO_VALUE)
		return true;

	slen = strlen(var->svalue);

	if (slen == 0)
		return false;

	if (pg_strcasecmp(var->svalue, "null") == 0)
	{
		setNullValue(&var->value);
	}
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
		int64		iv;

		if (!strtoint64(var->svalue, false, &iv))
			return false;

		setIntValue(&var->value, iv);
	}
	else
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
 * Assign a string value to a variable, creating it if need be.
 */
bool
putVariable(Variables *variables, const char *context, const char *name,
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

/*
 * Assign a PgBenchValue to a variable, creating it if need be.
 */
bool
putVariableValue(Variables *variables, const char *context, const char *name,
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

/*
 * Assign an integer value to a variable, creating it if need be.
 */
bool
putVariableInt(Variables *variables, const char *context, const char *name,
			   int64 value)
{
	PgBenchValue val;

	setIntValue(&val, value);
	return putVariableValue(variables, context, name, &val);
}

/*
 * Scoped put: if local_only is true, variable is bound to the current
 * innermost scope frame, shadowing outer frames.
 */
bool
var_put_value(Variables *variables, const char *name, const PgBenchValue *val, bool local_only)
{
	VariableScope *current = getCurrentScope(variables);
	Variable   *var;

	if (local_only)
	{
		var = lookupVariableInScope(current, name);
		if (var == NULL)
		{
			if (!valid_variable_name(name))
			{
				pg_log_error("invalid variable name: \"%s\"", name);
				return false;
			}
			enlargeVariables(current, 1);
			var = &(current->vars[current->nvars]);
			var->name = pg_strdup(name);
			var->svalue = NULL;
			var->value.type = PGBT_NO_VALUE;
			current->nvars++;
			current->vars_sorted = false;
		}
		free(var->svalue);
		var->svalue = NULL;
		var->value = *val;
		return true;
	}
	else
	{
		return putVariableValue(variables, "variable", name, val);
	}
}

/*
 * Scoped get: lookup variable across scopes and ensure its value is converted.
 */
bool
var_get_value(Variables *variables, const char *name, PgBenchValue *val)
{
	Variable   *var = lookupVariable(variables, name);

	if (var == NULL)
		return false;

	if (!makeVariableValue(var))
		return false;

	if (val != NULL)
		*val = var->value;
	return true;
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

/*
 * Replace variable placeholder with its string value in SQL buffer.
 */
char *
replaceVariable(char **sql, char *param, int len, const char *value)
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

/*
 * Substitute all :varname occurrences in sql string.
 */
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

/*
 * Return string representation of value type name.
 */
const char *
valueTypeName(const PgBenchValue *pval)
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
		Assert(false);
		return NULL;
	}
}

/*
 * Get a value as a boolean, or report an error.
 */
bool
coerceToBool(const PgBenchValue *pval, bool *bval)
{
	if (pval->type == PGBT_BOOLEAN)
	{
		*bval = pval->u.bval;
		return true;
	}
	else
	{
		pg_log_error("cannot coerce %s to boolean", valueTypeName(pval));
		*bval = false;
		return false;
	}
}

/*
 * Return true or false from an expression for conditional purposes.
 * Non-zero numerical values are true, zero and NULL are false.
 */
bool
valueTruth(const PgBenchValue *pval)
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
			Assert(0);
			return false;
	}
}

/*
 * Get a value as an int, report if error.
 */
bool
coerceToInt(const PgBenchValue *pval, int64 *ival)
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
	else
	{
		pg_log_error("cannot coerce %s to int", valueTypeName(pval));
		return false;
	}
}

/*
 * Get a value as a double, report if error.
 */
bool
coerceToDouble(const PgBenchValue *pval, double *dval)
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
	else
	{
		pg_log_error("cannot coerce %s to double", valueTypeName(pval));
		return false;
	}
}

void
setNullValue(PgBenchValue *pv)
{
	pv->type = PGBT_NULL;
	pv->u.ival = 0;
}

void
setBoolValue(PgBenchValue *pv, bool bval)
{
	pv->type = PGBT_BOOLEAN;
	pv->u.bval = bval;
}

void
setIntValue(PgBenchValue *pv, int64 ival)
{
	pv->type = PGBT_INT;
	pv->u.ival = ival;
}

void
setDoubleValue(PgBenchValue *pv, double dval)
{
	pv->type = PGBT_DOUBLE;
	pv->u.dval = dval;
}

/*
 * Return whether str matches "^\s*[-+]?[0-9]+$"
 */
bool
is_an_int(const char *str)
{
	const char *ptr = str;

	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;

	if (*ptr == '+' || *ptr == '-')
		ptr++;

	if (*ptr && !isdigit((unsigned char) *ptr))
		return false;

	while (*ptr && isdigit((unsigned char) *ptr))
		ptr++;

	return *ptr == '\0';
}

/*
 * strtoint64 -- convert a string to 64-bit integer
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

/*
 * convert string to double, detecting overflows/underflows
 */
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
