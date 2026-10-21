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
#include "common/pgbench_funcs.h"
#include "pqexpbuffer.h"
#include "variable.h"

#define VARIABLES_ALLOC_MARGIN	8

/*
 * Return whether str matches "^\s*[-+]?[0-9]+$"
 *
 * This should agree with strtoint64() on what's accepted, ignoring overflows.
 */
bool
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
			freeValue(&scope->vars[i].value);
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
		freeValue(&current->vars[i].value);
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

	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (scope->nvars <= 0)
		return NULL;

	/* Sort if we have to */
	if (!scope->vars_sorted)
	{
		qsort(scope->vars, scope->nvars, sizeof(Variable),
			  compareVariableNames);
		scope->vars_sorted = true;
	}

	/* Now we can search */
	key.name = unconstify(char *, name);
	return (Variable *) bsearch(&key,
								scope->vars,
								scope->nvars,
								sizeof(Variable),
								compareVariableNames);
}

/* Locate a variable by name; returns NULL if unknown */
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

/* Get the value of a variable, in string form; returns NULL if unknown */
char *
getVariable(Variables *variables, const char *name)
{
	Variable   *var;

	var = lookupVariable(variables, name);
	if (var == NULL)
		return NULL;			/* not found */

	if (var->svalue)
		return var->svalue;		/* we have it in string form */

	/* We need to produce a string equivalent of the value */
	Assert(var->value.type != PGBT_NO_VALUE);
	var->svalue = formatValue(&var->value);
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
	else if (slen >= 2 && var->svalue[0] == '{' && var->svalue[slen - 1] == '}')
	{
		if (!parseArrayLiteral(var->svalue, &var->value))
		{
			pg_log_error("malformed variable \"%s\" value: \"%s\"",
						 var->name, var->svalue);
			return false;
		}
	}
	else if (slen >= 3 && var->svalue[0] == '[' && var->svalue[slen - 1] == ']')
	{
		if (!parseVectorLiteral(var->svalue, &var->value))
		{
			pg_log_error("malformed variable \"%s\" value: \"%s\"",
						 var->name, var->svalue);
			return false;
		}
	}
	else if (slen >= 2 && var->svalue[0] == '\\' &&
			 (var->svalue[1] == 'x' || var->svalue[1] == 'X'))
	{
		if (!parseByteaLiteral(var->svalue, &var->value))
		{
			pg_log_error("malformed variable \"%s\" value: \"%s\"",
						 var->name, var->svalue);
			return false;
		}
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
bool
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
enlargeVariables(VariableScope *scope, int needed)
{
	/* total number of variables required now */
	needed += scope->nvars;

	if (scope->max_vars < needed)
	{
		scope->max_vars = needed + VARIABLES_ALLOC_MARGIN;
		scope->vars = (Variable *)
			pg_realloc_array(scope->vars, Variable, scope->max_vars);
	}
}

/*
 * Lookup a variable by name, creating it if need be.
 * Caller is expected to assign a value to the variable.
 * Returns NULL on failure (bad name).
 */
Variable *
lookupCreateVariable(Variables *variables, const char *context, const char *name)
{
	Variable   *var;
	VariableScope *current = getCurrentScope(variables);

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
		enlargeVariables(current, 1);

		var = &(current->vars[current->nvars]);

		var->name = pg_strdup(name);
		var->svalue = NULL;
		var->value.type = PGBT_NO_VALUE;
		/* caller is expected to initialize remaining fields */

		current->nvars++;
		/* we don't re-sort the array till we have to */
		current->vars_sorted = false;
	}

	return var;
}

/* Assign a string value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
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
	freeValue(&var->value);

	return true;
}

/* Assign a value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
bool
putVariableValue(Variables *variables, const char *context, const char *name,
				 const PgBenchValue *value)
{
	Variable   *var;
	PgBenchValue copy;

	var = lookupCreateVariable(variables, context, name);
	if (!var)
		return false;

	copyValue(&copy, value);
	free(var->svalue);
	var->svalue = NULL;
	freeValue(&var->value);
	var->value = copy;

	return true;
}

/* Assign an integer value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
bool
putVariableInt(Variables *variables, const char *context, const char *name,
			   int64 value)
{
	PgBenchValue val;

	setIntValue(&val, value);
	return putVariableValue(variables, context, name, &val);
}

/*
 * Assign a value to an element of an array, vector, or bytea variable at
 * 0-based index.
 * Returns false on failure (undefined variable, non-subscriptable type, or
 * out of bounds).
 */
bool
putVariableArrayElement(Variables *variables, const char *context,
						const char *name, int64 index,
						const PgBenchValue *value)
{
	Variable   *var;

	var = lookupVariable(variables, name);
	if (var == NULL)
	{
		pg_log_error("%s: undefined variable \"%s\"", context, name);
		return false;
	}

	if (!makeVariableValue(var))
		return false;

	if (var->value.type == PGBT_ARRAY)
	{
		PgBenchValue copy;

		if (index < 0 || index >= var->value.u.aval.nelems)
		{
			pg_log_error("%s: array index " INT64_FORMAT " out of bounds for variable \"%s\" (length %d)",
						 context, index, name, var->value.u.aval.nelems);
			return false;
		}

		copyValue(&copy, value);
		freeValue(&var->value.u.aval.elems[index]);
		var->value.u.aval.elems[index] = copy;
	}
	else if (var->value.type == PGBT_VECTOR)
	{
		double		dval;
		float	   *mutable_elems;

		if (index < 0 || index >= var->value.u.vval.dim)
		{
			pg_log_error("%s: vector index " INT64_FORMAT " out of bounds for variable \"%s\" (dim %d)",
						 context, index, name, var->value.u.vval.dim);
			return false;
		}

		if (!coerceToDouble(value, &dval))
			return false;

		/* Copy-on-write if vector currently points into read-only mmap */
		if (var->value.u.vval.is_borrowed)
		{
			int			dim = var->value.u.vval.dim;
			float	   *new_elems = pg_malloc_array(float, Max(dim, 1));

			if (dim > 0)
				memcpy(new_elems, var->value.u.vval.elems, sizeof(float) * (size_t) dim);
			var->value.u.vval.elems = new_elems;
			var->value.u.vval.is_borrowed = false;
		}

		mutable_elems = unconstify(float *, var->value.u.vval.elems);
		mutable_elems[index] = (float) dval;
	}
	else if (var->value.type == PGBT_BYTEA)
	{
		int64		ival;
		uint8	   *mutable_data;

		if (index < 0 || index >= var->value.u.byval.len)
		{
			pg_log_error("%s: bytea index " INT64_FORMAT " out of bounds for variable \"%s\" (length %d)",
						 context, index, name, var->value.u.byval.len);
			return false;
		}

		if (!coerceToInt(value, &ival) || ival < 0 || ival > 255)
		{
			pg_log_error("%s: bytea element value must be in range [0, 255]", context);
			return false;
		}

		/* Copy-on-write if bytea currently points into read-only mmap */
		if (var->value.u.byval.is_borrowed)
		{
			int			len = var->value.u.byval.len;
			uint8	   *new_data = pg_malloc_array(uint8, Max(len, 1));

			if (len > 0)
				memcpy(new_data, var->value.u.byval.data, (size_t) len);
			var->value.u.byval.data = new_data;
			var->value.u.byval.is_borrowed = false;
		}

		mutable_data = unconstify(uint8 *, var->value.u.byval.data);
		mutable_data[index] = (uint8) ival;
	}
	else
	{
		pg_log_error("%s: variable \"%s\" is not an array", context, name);
		return false;
	}

	/* Invalidate cached string representation */
	free(var->svalue);
	var->svalue = NULL;

	return true;
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
		PgBenchValue copy;

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
		copyValue(&copy, val);
		free(var->svalue);
		var->svalue = NULL;
		freeValue(&var->value);
		var->value = copy;
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
		copyValue(val, &var->value);
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

char *
assignVariables(Variables *variables, char *sql)
{
	char	   *p;
	bool		in_quote = false;

	p = sql;
	while (*p != '\0')
	{
		int			eaten;
		char	   *name;
		Variable   *var;
		char	   *val;

		if (*p == '\'')
		{
			in_quote = !in_quote;
			p++;
			continue;
		}
		if (*p != ':')
		{
			p++;
			continue;
		}

		name = parseVariable(p, &eaten);
		if (name == NULL)
		{
			while (*p == ':')
			{
				p++;
			}
			continue;
		}

		var = lookupVariable(variables, name);
		if (var != NULL && var->value.type == PGBT_NO_VALUE && var->svalue != NULL)
		{
			size_t		slen = strlen(var->svalue);

			if ((slen >= 2 && var->svalue[0] == '{' && var->svalue[slen - 1] == '}') ||
				(slen >= 3 && var->svalue[0] == '[' && var->svalue[slen - 1] == ']') ||
				(slen >= 2 && var->svalue[0] == '\\' &&
				 (var->svalue[1] == 'x' || var->svalue[1] == 'X')))
				(void) makeVariableValue(var);
		}
		val = getVariable(variables, name);
		free(name);
		if (val == NULL)
		{
			p++;
			continue;
		}

		if (var != NULL &&
			(var->value.type == PGBT_TEXT ||
			 var->value.type == PGBT_TIMESTAMP ||
			 var->value.type == PGBT_ARRAY ||
			 var->value.type == PGBT_VECTOR ||
			 var->value.type == PGBT_BYTEA))
		{
			PQExpBufferData qbuf;

			initPQExpBuffer(&qbuf);
			if (!in_quote)
				appendPQExpBufferChar(&qbuf, '\'');
			for (const char *c = val; *c != '\0'; c++)
			{
				if (*c == '\'')
					appendPQExpBufferChar(&qbuf, '\'');
				appendPQExpBufferChar(&qbuf, *c);
			}
			if (!in_quote)
				appendPQExpBufferChar(&qbuf, '\'');

			p = replaceVariable(&sql, p, eaten, qbuf.data);
			termPQExpBuffer(&qbuf);
		}
		else
		{
			p = replaceVariable(&sql, p, eaten, val);
		}
	}

	return sql;
}

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
	else if (pval->type == PGBT_TEXT)
		return "text";
	else if (pval->type == PGBT_TIMESTAMP)
		return "timestamp";
	else if (pval->type == PGBT_ARRAY)
		return "array";
	else if (pval->type == PGBT_VECTOR)
		return "vector";
	else if (pval->type == PGBT_BYTEA)
		return "bytea";
	else
	{
		/* internal error, should never get there */
		Assert(false);
		return NULL;
	}
}

/* get a value as a boolean, or tell if there is a problem */
bool
coerceToBool(const PgBenchValue *pval, bool *bval)
{
	if (pval->type == PGBT_BOOLEAN)
	{
		*bval = pval->u.bval;
		return true;
	}
	else						/* NULL, INT, DOUBLE, TEXT, etc. */
	{
		pg_log_error("cannot coerce %s to boolean", valueTypeName(pval));
		*bval = false;			/* suppress uninitialized-variable warnings */
		return false;
	}
}

/*
 * Return true or false from an expression for conditional purposes.
 * Non zero numerical values are true, zero and NULL are false.
 * Non-empty text and non-empty arrays are true.
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
		case PGBT_TEXT:
			return pval->u.sval != NULL && pval->u.sval[0] != '\0';
		case PGBT_TIMESTAMP:
			return pval->u.tval != 0;
		case PGBT_ARRAY:
			return pval->u.aval.nelems > 0;
		case PGBT_VECTOR:
			return pval->u.vval.dim > 0;
		case PGBT_BYTEA:
			return pval->u.byval.len > 0;
		default:
			/* internal error, unexpected type */
			Assert(0);
			return false;
	}
}

/* get a value as an int, tell if there is a problem */
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
	else if (pval->type == PGBT_TEXT)
	{
		if (pval->u.sval != NULL && is_an_int(pval->u.sval) &&
			strtoint64(pval->u.sval, true, ival))
			return true;
		pg_log_error("cannot coerce %s to int", valueTypeName(pval));
		return false;
	}
	else						/* BOOLEAN, NULL, TIMESTAMP, ARRAY, VECTOR, BYTEA */
	{
		pg_log_error("cannot coerce %s to int", valueTypeName(pval));
		return false;
	}
}

/* get a value as a double, or tell if there is a problem */
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
	else if (pval->type == PGBT_TEXT)
	{
		if (pval->u.sval != NULL && strtodouble(pval->u.sval, true, dval))
			return true;
		pg_log_error("cannot coerce %s to double", valueTypeName(pval));
		return false;
	}
	else						/* BOOLEAN, NULL, TIMESTAMP, ARRAY, VECTOR, BYTEA */
	{
		pg_log_error("cannot coerce %s to double", valueTypeName(pval));
		return false;
	}
}

/* get a value as a newly-allocated string, or tell if there is a problem */
bool
coerceToText(const PgBenchValue *pval, char **sval)
{
	if (pval->type == PGBT_NO_VALUE)
	{
		pg_log_error("cannot coerce %s to text", valueTypeName(pval));
		*sval = NULL;
		return false;
	}
	*sval = formatValue(pval);
	return true;
}

/* get a value as a timestamp (microseconds since 2000-01-01), or report error */
bool
coerceToTimestamp(const PgBenchValue *pval, int64 *tval)
{
	if (pval->type == PGBT_TIMESTAMP)
	{
		*tval = pval->u.tval;
		return true;
	}
	else if (pval->type == PGBT_TEXT)
	{
		return parseTimestampString(pval->u.sval, false, tval);
	}
	else if (pval->type == PGBT_INT)
	{
		*tval = pval->u.ival;
		return true;
	}
	else
	{
		pg_log_error("cannot coerce %s to timestamp", valueTypeName(pval));
		*tval = 0;
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

/* assign a text value (makes a heap copy of sval) */
void
setTextValue(PgBenchValue *pv, const char *sval)
{
	pv->type = PGBT_TEXT;
	pv->u.sval = pg_strdup(sval != NULL ? sval : "");
}

/* assign a timestamp value (microseconds since 2000-01-01 00:00:00) */
void
setTimestampValue(PgBenchValue *pv, int64 tval)
{
	pv->type = PGBT_TIMESTAMP;
	pv->u.tval = tval;
}

/* assign an array value (deep-copies elements) */
void
setArrayValue(PgBenchValue *pv, const PgBenchValue *elems, int nelems)
{
	pv->type = PGBT_ARRAY;
	pv->u.aval.nelems = nelems;
	if (nelems > 0)
	{
		pv->u.aval.elems = pg_malloc_array(PgBenchValue, nelems);
		for (int i = 0; i < nelems; i++)
			copyValue(&pv->u.aval.elems[i], &elems[i]);
	}
	else
	{
		pv->u.aval.elems = NULL;
	}
}

/*
 * assign a float32 vector value.
 * When is_borrowed is true, stores elems pointer directly (zero-copy into
 * persistent read-only mmap).  When is_borrowed is false, allocates and
 * copies dim floats onto the heap.
 */
void
setVectorValue(PgBenchValue *pv, const float *elems, int dim, bool is_borrowed)
{
	pv->type = PGBT_VECTOR;
	pv->u.vval.dim = dim;
	pv->u.vval.is_borrowed = is_borrowed;
	if (is_borrowed)
	{
		pv->u.vval.elems = elems;
	}
	else if (dim > 0)
	{
		float	   *copy = pg_malloc_array(float, dim);

		if (elems != NULL)
			memcpy(copy, elems, sizeof(float) * (size_t) dim);
		else
			memset(copy, 0, sizeof(float) * (size_t) dim);
		pv->u.vval.elems = copy;
	}
	else
	{
		pv->u.vval.elems = NULL;
	}
}

/*
 * assign a bytea value.
 * When is_borrowed is true, stores data pointer directly (zero-copy into
 * persistent read-only mmap).  When is_borrowed is false, allocates and
 * copies len bytes onto the heap.
 */
void
setByteaValue(PgBenchValue *pv, const uint8 *data, int len, bool is_borrowed)
{
	pv->type = PGBT_BYTEA;
	pv->u.byval.len = len;
	pv->u.byval.is_borrowed = is_borrowed;
	if (is_borrowed)
	{
		pv->u.byval.data = data;
	}
	else if (len > 0)
	{
		uint8	   *copy = pg_malloc_array(uint8, len);

		if (data != NULL)
			memcpy(copy, data, (size_t) len);
		else
			memset(copy, 0, (size_t) len);
		pv->u.byval.data = copy;
	}
	else
	{
		pv->u.byval.data = NULL;
	}
}

/* free any heap storage owned by a PgBenchValue and reset to PGBT_NO_VALUE */
void
freeValue(PgBenchValue *pv)
{
	if (pv == NULL)
		return;

	if (pv->type == PGBT_TEXT)
	{
		pg_free(pv->u.sval);
		pv->u.sval = NULL;
	}
	else if (pv->type == PGBT_ARRAY)
	{
		for (int i = 0; i < pv->u.aval.nelems; i++)
			freeValue(&pv->u.aval.elems[i]);
		pg_free(pv->u.aval.elems);
		pv->u.aval.elems = NULL;
		pv->u.aval.nelems = 0;
	}
	else if (pv->type == PGBT_VECTOR)
	{
		if (!pv->u.vval.is_borrowed && pv->u.vval.elems != NULL)
			pg_free(unconstify(float *, pv->u.vval.elems));
		pv->u.vval.elems = NULL;
		pv->u.vval.dim = 0;
		pv->u.vval.is_borrowed = false;
	}
	else if (pv->type == PGBT_BYTEA)
	{
		if (!pv->u.byval.is_borrowed && pv->u.byval.data != NULL)
			pg_free(unconstify(uint8 *, pv->u.byval.data));
		pv->u.byval.data = NULL;
		pv->u.byval.len = 0;
		pv->u.byval.is_borrowed = false;
	}
	pv->type = PGBT_NO_VALUE;
}

/* deep-copy src into dest (dest is assumed uninitialized or already freed) */
void
copyValue(PgBenchValue *dest, const PgBenchValue *src)
{
	if (src->type == PGBT_TEXT)
		setTextValue(dest, src->u.sval);
	else if (src->type == PGBT_ARRAY)
		setArrayValue(dest, src->u.aval.elems, src->u.aval.nelems);
	else if (src->type == PGBT_VECTOR)
		setVectorValue(dest, src->u.vval.elems, src->u.vval.dim,
					   src->u.vval.is_borrowed);
	else if (src->type == PGBT_BYTEA)
		setByteaValue(dest, src->u.byval.data, src->u.byval.len,
					  src->u.byval.is_borrowed);
	else
		*dest = *src;
}

/*
 * Parse an ISO-8601 date/timestamp string ("YYYY-MM-DD[ HH:MI:SS[.ffffff]]")
 * into microseconds since 2000-01-01 00:00:00.
 */
bool
parseTimestampString(const char *str, bool errorOK, int64 *tval)
{
	if (!pgbench_parse_timestamp(str, tval))
	{
		if (!errorOK)
			pg_log_error("invalid input syntax for type timestamp: \"%s\"",
						 str != NULL ? str : "");
		return false;
	}
	return true;
}

/*
 * Format microseconds since 2000-01-01 00:00:00 into ISO-8601 string
 * "YYYY-MM-DD" (if midnight) or "YYYY-MM-DD HH:MI:SS[.ffffff]".
 */
void
formatTimestampValue(int64 tval, char *buf, size_t buflen)
{
	pgbench_format_timestamp(tval, buf, buflen);
}

/*
 * Check whether an array element string needs double-quoting in a PostgreSQL
 * array literal "{...}".
 */
static bool
arrayElemNeedsQuotes(const char *s)
{
	if (s == NULL || *s == '\0')
		return true;
	if (pg_strcasecmp(s, "null") == 0)
		return true;
	for (const char *p = s; *p != '\0'; p++)
	{
		if (isspace((unsigned char) *p) || *p == ',' || *p == '{' ||
			*p == '}' || *p == '"' || *p == '\\')
			return true;
	}
	return false;
}

/*
 * Format any PgBenchValue into a newly pg_malloc'd string.
 */
char *
formatValue(const PgBenchValue *pv)
{
	char		stringform[64];

	switch (pv->type)
	{
		case PGBT_NULL:
			return pg_strdup("NULL");

		case PGBT_BOOLEAN:
			return pg_strdup(pv->u.bval ? "true" : "false");

		case PGBT_INT:
			snprintf(stringform, sizeof(stringform), INT64_FORMAT, pv->u.ival);
			return pg_strdup(stringform);

		case PGBT_DOUBLE:
			snprintf(stringform, sizeof(stringform), "%.*g", DBL_DIG, pv->u.dval);
			return pg_strdup(stringform);

		case PGBT_TEXT:
			return pg_strdup(pv->u.sval != NULL ? pv->u.sval : "");

		case PGBT_TIMESTAMP:
			formatTimestampValue(pv->u.tval, stringform, sizeof(stringform));
			return pg_strdup(stringform);

		case PGBT_VECTOR:
			{
				int			dim = pv->u.vval.dim;
				size_t		buflen = (size_t) Max(dim, 1) * 16 + 8;
				char	   *buf = (char *) pg_malloc(buflen);

				if (pgbench_format_vector(pv->u.vval.elems, dim, buf, buflen) == 0 &&
					dim > 0)
				{
					/* Fallback with larger allocation if needed */
					buflen = (size_t) dim * 32 + 8;
					buf = (char *) pg_realloc(buf, buflen);
					(void) pgbench_format_vector(pv->u.vval.elems, dim, buf, buflen);
				}
				return buf;
			}

		case PGBT_BYTEA:
			{
				static const char hex_chars[] = "0123456789abcdef";
				int			len = pv->u.byval.len;
				const uint8 *data = pv->u.byval.data;
				char	   *buf = (char *) pg_malloc((size_t) len * 2 + 3);
				char	   *dst = buf;

				*dst++ = '\\';
				*dst++ = 'x';
				for (int i = 0; i < len; i++)
				{
					uint8		b = data[i];

					*dst++ = hex_chars[(b >> 4) & 0x0f];
					*dst++ = hex_chars[b & 0x0f];
				}
				*dst = '\0';
				return buf;
			}

		case PGBT_ARRAY:
			{
				PQExpBufferData buf;

				initPQExpBuffer(&buf);
				appendPQExpBufferChar(&buf, '{');
				for (int i = 0; i < pv->u.aval.nelems; i++)
				{
					const PgBenchValue *elem = &pv->u.aval.elems[i];

					if (i > 0)
						appendPQExpBufferChar(&buf, ',');

					if (elem->type == PGBT_NULL)
					{
						appendPQExpBufferStr(&buf, "NULL");
					}
					else if (elem->type == PGBT_ARRAY)
					{
						char	   *sub = formatValue(elem);

						appendPQExpBufferStr(&buf, sub);
						pg_free(sub);
					}
					else
					{
						char	   *s = formatValue(elem);

						if ((elem->type == PGBT_TEXT ||
							 elem->type == PGBT_TIMESTAMP ||
							 elem->type == PGBT_VECTOR ||
							 elem->type == PGBT_BYTEA) &&
							arrayElemNeedsQuotes(s))
						{
							appendPQExpBufferChar(&buf, '"');
							for (const char *p = s; *p != '\0'; p++)
							{
								if (*p == '"' || *p == '\\')
									appendPQExpBufferChar(&buf, '\\');
								appendPQExpBufferChar(&buf, *p);
							}
							appendPQExpBufferChar(&buf, '"');
						}
						else
						{
							appendPQExpBufferStr(&buf, s);
						}
						pg_free(s);
					}
				}
				appendPQExpBufferChar(&buf, '}');
				return buf.data;
			}

		default:
			Assert(false);
			return pg_strdup("");
	}
}

/*
 * Parse a single scalar element from a PostgreSQL array literal into a PgBenchValue.
 */
static void
parseArrayScalarElement(const char *token, bool was_quoted, PgBenchValue *elem)
{
	int64		ival;
	double		dval;

	if (!was_quoted && pg_strcasecmp(token, "NULL") == 0)
	{
		setNullValue(elem);
	}
	else if (!was_quoted &&
			 (pg_strcasecmp(token, "true") == 0 || pg_strcasecmp(token, "t") == 0))
	{
		setBoolValue(elem, true);
	}
	else if (!was_quoted &&
			 (pg_strcasecmp(token, "false") == 0 || pg_strcasecmp(token, "f") == 0))
	{
		setBoolValue(elem, false);
	}
	else if (!was_quoted && is_an_int(token) && strtoint64(token, true, &ival))
	{
		setIntValue(elem, ival);
	}
	else if (!was_quoted && strtodouble(token, true, &dval))
	{
		setDoubleValue(elem, dval);
	}
	else
	{
		setTextValue(elem, token);
	}
}

/*
 * Parse a PostgreSQL array literal "{elem1,elem2,...}" into a PGBT_ARRAY value.
 */
bool
parseArrayLiteral(const char *str, PgBenchValue *aval)
{
	const char *p = str;
	size_t		len;
	PgBenchValue *elems = NULL;
	int			nelems = 0;
	int			capacity = 0;

	if (str == NULL)
		return false;

	while (*p && isspace((unsigned char) *p))
		p++;

	len = strlen(p);
	while (len > 0 && isspace((unsigned char) p[len - 1]))
		len--;

	if (len < 2 || p[0] != '{' || p[len - 1] != '}')
		return false;

	/* Advance past '{' */
	p++;
	len -= 2;

	/* Empty array "{}" */
	{
		size_t		i = 0;

		while (i < len && isspace((unsigned char) p[i]))
			i++;
		if (i == len)
		{
			setArrayValue(aval, NULL, 0);
			return true;
		}
	}

	{
		const char *end = p + len;

		while (p < end)
		{
			PgBenchValue elem;

			while (p < end && isspace((unsigned char) *p))
				p++;

			if (p < end && *p == '{')
			{
				/* Nested array */
				int			depth = 0;
				const char *sub_start = p;
				bool		in_quotes = false;
				char	   *sub_str;

				while (p < end)
				{
					if (in_quotes)
					{
						if (*p == '\\' && (p + 1) < end)
							p++;
						else if (*p == '"')
							in_quotes = false;
					}
					else
					{
						if (*p == '"')
							in_quotes = true;
						else if (*p == '{')
							depth++;
						else if (*p == '}')
						{
							depth--;
							if (depth == 0)
							{
								p++;
								break;
							}
						}
					}
					p++;
				}
				if (depth != 0)
					goto fail;

				sub_str = pg_malloc((p - sub_start) + 1);
				memcpy(sub_str, sub_start, p - sub_start);
				sub_str[p - sub_start] = '\0';
				if (!parseArrayLiteral(sub_str, &elem))
				{
					pg_free(sub_str);
					goto fail;
				}
				pg_free(sub_str);

				while (p < end && isspace((unsigned char) *p))
					p++;
				if (p < end)
				{
					if (*p != ',')
					{
						freeValue(&elem);
						goto fail;
					}
					p++;
				}
			}
			else if (p < end && *p == '"')
			{
				/* Quoted element */
				PQExpBufferData tok;

				initPQExpBuffer(&tok);
				p++;			/* skip opening '"' */
				while (p < end && *p != '"')
				{
					if (*p == '\\' && (p + 1) < end)
						p++;
					appendPQExpBufferChar(&tok, *p);
					p++;
				}
				if (p >= end || *p != '"')
				{
					termPQExpBuffer(&tok);
					goto fail;
				}
				p++;			/* skip closing '"' */

				parseArrayScalarElement(tok.data, true, &elem);
				termPQExpBuffer(&tok);

				while (p < end && isspace((unsigned char) *p))
					p++;
				if (p < end)
				{
					if (*p != ',')
					{
						freeValue(&elem);
						goto fail;
					}
					p++;
				}
			}
			else
			{
				/* Unquoted element up to ',' or end */
				const char *tok_start = p;
				const char *tok_end;
				char	   *tok;

				while (p < end && *p != ',')
				{
					if (*p == '{' || *p == '}' || *p == '"')
						goto fail;
					p++;
				}
				tok_end = p;
				while (tok_end > tok_start && isspace((unsigned char) tok_end[-1]))
					tok_end--;

				tok = pg_malloc((tok_end - tok_start) + 1);
				memcpy(tok, tok_start, tok_end - tok_start);
				tok[tok_end - tok_start] = '\0';

				parseArrayScalarElement(tok, false, &elem);
				pg_free(tok);

				if (p < end && *p == ',')
					p++;
			}

			if (nelems >= capacity)
			{
				capacity = (capacity == 0) ? 8 : capacity * 2;
				elems = pg_realloc_array(elems, PgBenchValue, capacity);
			}
			elems[nelems++] = elem;
		}
	}

	aval->type = PGBT_ARRAY;
	aval->u.aval.nelems = nelems;
	aval->u.aval.elems = elems;
	return true;

fail:
	for (int i = 0; i < nelems; i++)
		freeValue(&elems[i]);
	pg_free(elems);
	return false;
}

/*
 * Parse a pgvector literal "[f1,f2,...]" into a PGBT_VECTOR value.
 */
bool
parseVectorLiteral(const char *str, PgBenchValue *vval)
{
	int			dim;
	float	   *elems;

	if (!pgbench_parse_vector_count(str, &dim) || dim <= 0)
		return false;

	elems = pg_malloc_array(float, dim);
	if (!pgbench_parse_vector_into(str, elems, dim))
	{
		pg_free(elems);
		return false;
	}

	vval->type = PGBT_VECTOR;
	vval->u.vval.dim = dim;
	vval->u.vval.is_borrowed = false;
	vval->u.vval.elems = elems;
	return true;
}

/*
 * Helper to decode a single hex character into 0..15, or -1 if invalid.
 */
static inline int
hexCharVal(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Parse a PostgreSQL hex bytea literal "\x..." into a PGBT_BYTEA value.
 */
bool
parseByteaLiteral(const char *str, PgBenchValue *byval)
{
	const char *p = str;
	size_t		len;
	int			nbytes;
	uint8	   *data = NULL;

	if (str == NULL)
		return false;

	while (*p && isspace((unsigned char) *p))
		p++;

	len = strlen(p);
	while (len > 0 && isspace((unsigned char) p[len - 1]))
		len--;

	if (len < 2 || p[0] != '\\' || (p[1] != 'x' && p[1] != 'X'))
		return false;

	p += 2;
	len -= 2;

	/* Hex payload must have an even number of hex digits */
	if ((len % 2) != 0)
		return false;

	nbytes = (int) (len / 2);
	if (nbytes > 0)
	{
		data = pg_malloc_array(uint8, nbytes);
		for (int i = 0; i < nbytes; i++)
		{
			int			hi = hexCharVal(p[2 * i]);
			int			lo = hexCharVal(p[2 * i + 1]);

			if (hi < 0 || lo < 0)
			{
				pg_free(data);
				return false;
			}
			data[i] = (uint8) ((hi << 4) | lo);
		}
	}

	byval->type = PGBT_BYTEA;
	byval->u.byval.len = nbytes;
	byval->u.byval.is_borrowed = false;
	byval->u.byval.data = data;
	return true;
}

