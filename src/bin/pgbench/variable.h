/*-------------------------------------------------------------------------
 *
 * variable.h
 *	  Variable management and value types for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/variable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VARIABLE_H
#define VARIABLE_H

/*
 * Variable types used in parser.
 */
typedef enum
{
	PGBT_NO_VALUE = 0,
	PGBT_NULL,
	PGBT_INT,
	PGBT_DOUBLE,
	PGBT_BOOLEAN,
	/* add other types here */
} PgBenchValueType;

typedef struct
{
	PgBenchValueType type;
	union
	{
		int64		ival;
		double		dval;
		bool		bval;
		/* add other types here */
	}			u;
} PgBenchValue;

/*
 * We don't want to allocate variables one by one; for efficiency, add a
 * constant margin each time it overflows.
 */
#define VARIABLES_ALLOC_MARGIN	8

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
typedef struct
{
	char	   *name;			/* variable's name */
	char	   *svalue;			/* its value in string form, if known */
	PgBenchValue value;			/* actual variable's value */
} Variable;

/*
 * Data structure for client variables.
 */
typedef struct
{
	Variable   *vars;			/* array of variable definitions */
	int			nvars;			/* number of variables */

	/*
	 * The maximum number of variables that we can currently store in 'vars'
	 * without having to reallocate more space. We must always have max_vars
	 * >= nvars.
	 */
	int			max_vars;

	bool		vars_sorted;	/* are variables sorted by name? */
} Variables;

extern bool strtoint64(const char *str, bool errorOK, int64 *result);
extern bool strtodouble(const char *str, bool errorOK, double *dv);

extern Variable *lookupVariable(Variables *variables, char *name);
extern char *getVariable(Variables *variables, char *name);
extern bool makeVariableValue(Variable *var);
extern bool putVariable(Variables *variables, const char *context, char *name,
						const char *value);
extern bool putVariableValue(Variables *variables, const char *context,
							 char *name, const PgBenchValue *value);
extern bool putVariableInt(Variables *variables, const char *context, char *name,
						   int64 value);
extern char *parseVariable(const char *sql, int *eaten);
extern char *replaceVariable(char **sql, char *param, int len, char *value);
extern char *assignVariables(Variables *variables, char *sql);
extern bool coerceToBool(PgBenchValue *pval, bool *bval);
extern bool valueTruth(PgBenchValue *pval);
extern bool coerceToInt(PgBenchValue *pval, int64 *ival);
extern bool coerceToDouble(PgBenchValue *pval, double *dval);
extern void setNullValue(PgBenchValue *pv);
extern void setBoolValue(PgBenchValue *pv, bool bval);
extern void setIntValue(PgBenchValue *pv, int64 ival);
extern void setDoubleValue(PgBenchValue *pv, double dval);

#endif							/* VARIABLE_H */
