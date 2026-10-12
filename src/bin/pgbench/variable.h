/*-------------------------------------------------------------------------
 *
 * variable.h
 *		Scoped variable store and value evaluation support for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/variable.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_VARIABLE_H
#define PGBENCH_VARIABLE_H

/*
 * Variable types used in parser and expressions.
 */
typedef enum
{
	PGBT_NO_VALUE = 0,
	PGBT_NULL,
	PGBT_INT,
	PGBT_DOUBLE,
	PGBT_BOOLEAN,
	PGBT_TEXT,
	PGBT_TIMESTAMP,
	PGBT_ARRAY
} PgBenchValueType;

struct PgBenchValue;

typedef struct PgBenchArray
{
	int			nelems;
	struct PgBenchValue *elems;
} PgBenchArray;

typedef struct PgBenchValue
{
	PgBenchValueType type;
	union
	{
		int64		ival;
		double		dval;
		bool		bval;
		char	   *sval;		/* PGBT_TEXT: heap-allocated string */
		int64		tval;		/* PGBT_TIMESTAMP: usec since 2000-01-01 */
		PgBenchArray aval;		/* PGBT_ARRAY: dynamic array of PgBenchValue */
	}			u;
} PgBenchValue;

/*
 * Variable definitions.
 *
 * If a variable only has a string value, "svalue" is that value, and value is
 * "not set".  If the value is known, "value" contains the value (in any
 * variant).
 *
 * In this case "svalue" contains the string equivalent of the value, if we've
 * had occasion to compute that, or NULL if we haven't.
 */
typedef struct Variable
{
	char	   *name;			/* variable's name */
	char	   *svalue;			/* its value in string form, if known */
	PgBenchValue value;			/* actual variable's value */
} Variable;

/*
 * Scoped variable stack: each scope frame can hold local variables
 * that shadow outer variables, enabling loop counters, local script
 * variables, and function frame isolation.
 */
typedef struct VariableScope
{
	struct VariableScope *parent;	/* Enclosing scope (NULL for root scope) */
	Variable   *vars;				/* array of variable definitions */
	int			nvars;				/* number of variables in this scope */
	int			max_vars;			/* allocated capacity */
	bool		vars_sorted;		/* are variables sorted by name? */
} VariableScope;

/*
 * Container for client variables with root scope and active scope stack.
 */
typedef struct Variables
{
	VariableScope *root;			/* root / global scope */
	VariableScope *current;			/* top of scope stack (innermost scope) */
} Variables;

typedef Variables VariableScopeStack;

/* Value manipulation routines */
extern const char *valueTypeName(const PgBenchValue *pval);
extern bool coerceToBool(const PgBenchValue *pval, bool *bval);
extern bool valueTruth(const PgBenchValue *pval);
extern bool coerceToInt(const PgBenchValue *pval, int64 *ival);
extern bool coerceToDouble(const PgBenchValue *pval, double *dval);
extern bool coerceToText(const PgBenchValue *pval, char **sval);
extern bool coerceToTimestamp(const PgBenchValue *pval, int64 *tval);
extern void setNullValue(PgBenchValue *pv);
extern void setBoolValue(PgBenchValue *pv, bool bval);
extern void setIntValue(PgBenchValue *pv, int64 ival);
extern void setDoubleValue(PgBenchValue *pv, double dval);
extern void setTextValue(PgBenchValue *pv, const char *sval);
extern void setTimestampValue(PgBenchValue *pv, int64 tval);
extern void setArrayValue(PgBenchValue *pv, const PgBenchValue *elems, int nelems);
extern void freeValue(PgBenchValue *pv);
extern void copyValue(PgBenchValue *dest, const PgBenchValue *src);
extern char *formatValue(const PgBenchValue *pv);
extern bool parseTimestampString(const char *str, bool errorOK, int64 *tval);
extern void formatTimestampValue(int64 tval, char *buf, size_t buflen);
extern bool parseArrayLiteral(const char *str, PgBenchValue *aval);

/* Parsing numbers */
extern bool is_an_int(const char *str);
extern bool strtoint64(const char *str, bool errorOK, int64 *result);
extern bool strtodouble(const char *str, bool errorOK, double *dv);

/* Variable lifecycle & scope management */
extern void initVariables(Variables *variables);
extern void destroyVariables(Variables *variables);
extern bool copyVariables(Variables *dest, const Variables *src);
extern bool var_scope_push(Variables *variables);
extern bool var_scope_pop(Variables *variables);

/* Variable lookup and modification */
extern Variable *lookupVariable(Variables *variables, const char *name);
extern Variable *lookupCreateVariable(Variables *variables, const char *context, const char *name);
extern char *getVariable(Variables *variables, const char *name);
extern bool makeVariableValue(Variable *var);
extern bool putVariable(Variables *variables, const char *context, const char *name,
						const char *value);
extern bool putVariableValue(Variables *variables, const char *context, const char *name,
							 const PgBenchValue *value);
extern bool putVariableInt(Variables *variables, const char *context, const char *name,
						   int64 value);
extern bool putVariableArrayElement(Variables *variables, const char *context,
									const char *name, int64 index,
									const PgBenchValue *value);

/* High-level scoped get/put */
extern bool var_put_value(Variables *variables, const char *name, const PgBenchValue *val, bool local_only);
extern bool var_get_value(Variables *variables, const char *name, PgBenchValue *val);

/* String substitution helpers */
extern char *parseVariable(const char *sql, int *eaten);
extern char *replaceVariable(char **sql, char *param, int len, const char *value);
extern char *assignVariables(Variables *variables, char *sql);

#endif							/* PGBENCH_VARIABLE_H */
