/*-------------------------------------------------------------------------
 *
 * script.h
 *	  Script parsing, built-in scripts, and expression evaluation for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/script.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SCRIPT_H
#define SCRIPT_H

#include "common/pg_prng.h"
#include "pgbench.h"
#include "pqexpbuffer.h"
#include "stats.h"
#include "variable.h"

/* X/Open (XSI) requires <math.h> to provide M_PI, but core POSIX does not */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Hashing constants
 */
#define FNV_PRIME			UINT64CONST(0x100000001b3)
#define FNV_OFFSET_BASIS	UINT64CONST(0xcbf29ce484222325)
#define MM2_MUL				UINT64CONST(0xc6a4a7935bd1e995)
#define MM2_MUL_TIMES_8		UINT64CONST(0x35253c9ade8f4ca8)
#define MM2_ROT				47

#define MIN_GAUSSIAN_PARAM		2.0 /* minimum parameter for gauss */

#define MIN_ZIPFIAN_PARAM		1.001	/* minimum parameter for zipfian */
#define MAX_ZIPFIAN_PARAM		1000.0	/* maximum parameter for zipfian */

#define nbranches	1			/* Makes little sense to change this.  Change
								 * -s instead */
#define ntellers	10
#define naccounts	100000

/*
 * The scale factor at/beyond which 32bit integers are incapable of storing
 * 64bit values.
 *
 * Although the actual threshold is 21474, we use 20000 because it is easier to
 * document and remember, and isn't that far away from the real threshold.
 */
#define SCALE_32BIT_THRESHOLD 20000

#define WSEP '@'				/* weight separator */

#define MAX_SCRIPTS		128		/* max number of SQL scripts allowed */

/*
 * queries read from files
 */
#define SQL_COMMAND		1
#define META_COMMAND	2

/*
 * max number of backslash command arguments or SQL variables,
 * including the command or SQL statement itself
 */
#define MAX_ARGS		256

typedef enum MetaCommand
{
	META_NONE,					/* not a known meta-command */
	META_SET,					/* \set */
	META_SETSHELL,				/* \setshell */
	META_SHELL,					/* \shell */
	META_SLEEP,					/* \sleep */
	META_GSET,					/* \gset */
	META_ASET,					/* \aset */
	META_IF,					/* \if */
	META_ELIF,					/* \elif */
	META_ELSE,					/* \else */
	META_ENDIF,					/* \endif */
	META_STARTPIPELINE,			/* \startpipeline */
	META_SYNCPIPELINE,			/* \syncpipeline */
	META_ENDPIPELINE,			/* \endpipeline */
} MetaCommand;

typedef enum QueryMode
{
	QUERY_SIMPLE,				/* simple query */
	QUERY_EXTENDED,				/* extended query */
	QUERY_PREPARED,				/* extended query with prepared statements */
	NUM_QUERYMODE
} QueryMode;

extern QueryMode querymode;
extern const char *const QUERYMODE[];

/*
 * struct Command represents one command in a script.
 *
 * lines		The raw, possibly multi-line command text.  Variable substitution
 *				not applied.
 * first_line	A short, single-line extract of 'lines', for error reporting.
 * type			SQL_COMMAND or META_COMMAND
 * meta			The type of meta-command, with META_NONE/GSET/ASET if command
 *				is SQL.
 * argc			Number of arguments of the command, 0 if not yet processed.
 * argv			Command arguments, the first of which is the command or SQL
 *				string itself.  For SQL commands, after post-processing
 *				argv[0] is the same as 'lines' with variables substituted.
 * prepname		The name that this command is prepared under, in prepare mode
 * varprefix	SQL commands terminated with \gset or \aset have this set
 *				to a non NULL value.  If nonempty, it's used to prefix the
 *				variable name that receives the value.
 * aset			do gset on all possible queries of a combined query (\;).
 * expr			Parsed expression, if needed.
 * stats		Time spent in this command.
 * retries		Number of retries after a serialization or deadlock error in the
 *				current command.
 * failures		Number of errors in the current command that were not retried.
 */
typedef struct Command
{
	PQExpBufferData lines;
	char	   *first_line;
	int			type;
	MetaCommand meta;
	int			argc;
	char	   *argv[MAX_ARGS];
	char	   *prepname;
	char	   *varprefix;
	PgBenchExpr *expr;
	SimpleStats stats;
	int64		retries;
	int64		failures;
} Command;

typedef struct ParsedScript
{
	const char *desc;			/* script descriptor (eg, file name) */
	int			weight;			/* selection weight */
	Command   **commands;		/* NULL-terminated array of Commands */
	StatsData	stats;			/* total time spent in script */
} ParsedScript;

extern ParsedScript sql_script[MAX_SCRIPTS];
extern int	num_scripts;
extern int64 total_weight;

/* Builtin test scripts */
typedef struct BuiltinScript
{
	const char *name;			/* very short name for -b ... */
	const char *desc;			/* short description */
	const char *script;			/* actual pgbench script */
} BuiltinScript;

extern bool evaluateExpr(CState *st, PgBenchExpr *expr, PgBenchValue *retval);
extern int	chooseScript(pg_prng_state *random_state);

extern void postprocess_sql_command(Command *my_command);
extern void process_file(const char *filename, int weight);
extern void process_builtin(const BuiltinScript *bi, int weight);
extern void listAvailableScripts(void);
extern const BuiltinScript *findBuiltin(const char *tb);
extern int	parseScriptWeight(const char *option, char **script);

#endif							/* SCRIPT_H */
