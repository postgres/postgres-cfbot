/*-------------------------------------------------------------------------
 *
 * script.h
 *		Script parsing, AST structures, and expression evaluation for pgbench
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/script.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_SCRIPT_H
#define PGBENCH_SCRIPT_H

#include "fe_utils/conditional.h"
#include "fe_utils/psqlscan.h"
#include "common/pg_prng.h"
#include "stats.h"
#include "variable.h"

/* Forward declarations */
typedef void *yyscan_t;
union YYSTYPE;
typedef struct CState CState;
typedef struct TState TState;

/* Scale factor defaults */
#define nbranches	1
#define ntellers	10
#define naccounts	100000
#define SCALE_32BIT_THRESHOLD 20000

/* Constants */
#define SQL_COMMAND			1
#define META_COMMAND		2
#define MAX_ARGS			256
#define MAX_SCRIPTS			128
#define COMMANDS_ALLOC_NUM	128
#define WSEP				'@'

/* Types of expression nodes */
typedef enum PgBenchExprType
{
	ENODE_CONSTANT,
	ENODE_VARIABLE,
	ENODE_FUNCTION,
} PgBenchExprType;

/* List of operators and callable functions */
typedef enum PgBenchFunction
{
	PGBENCH_ADD,
	PGBENCH_SUB,
	PGBENCH_MUL,
	PGBENCH_DIV,
	PGBENCH_MOD,
	PGBENCH_DEBUG,
	PGBENCH_ABS,
	PGBENCH_LEAST,
	PGBENCH_GREATEST,
	PGBENCH_INT,
	PGBENCH_DOUBLE,
	PGBENCH_PI,
	PGBENCH_SQRT,
	PGBENCH_LN,
	PGBENCH_EXP,
	PGBENCH_RANDOM,
	PGBENCH_RANDOM_GAUSSIAN,
	PGBENCH_RANDOM_EXPONENTIAL,
	PGBENCH_RANDOM_ZIPFIAN,
	PGBENCH_POW,
	PGBENCH_AND,
	PGBENCH_OR,
	PGBENCH_NOT,
	PGBENCH_BITAND,
	PGBENCH_BITOR,
	PGBENCH_BITXOR,
	PGBENCH_LSHIFT,
	PGBENCH_RSHIFT,
	PGBENCH_EQ,
	PGBENCH_NE,
	PGBENCH_LE,
	PGBENCH_LT,
	PGBENCH_IS,
	PGBENCH_CASE,
	PGBENCH_HASH_FNV1A,
	PGBENCH_HASH_MURMUR2,
	PGBENCH_PERMUTE,
} PgBenchFunction;

typedef struct PgBenchExpr PgBenchExpr;
typedef struct PgBenchExprLink PgBenchExprLink;
typedef struct PgBenchExprList PgBenchExprList;

struct PgBenchExpr
{
	PgBenchExprType etype;
	union
	{
		PgBenchValue constant;
		struct
		{
			char	   *varname;
		}			variable;
		struct
		{
			PgBenchFunction function;
			PgBenchExprLink *args;
		}			function;
	}			u;
};

/* List of expression nodes */
struct PgBenchExprLink
{
	PgBenchExpr *expr;
	PgBenchExprLink *next;
};

struct PgBenchExprList
{
	PgBenchExprLink *head;
	PgBenchExprLink *tail;
};

/* Meta-command enumeration */
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

/* Query mode */
typedef enum QueryMode
{
	QUERY_SIMPLE,				/* simple query */
	QUERY_EXTENDED,				/* extended query */
	QUERY_PREPARED,				/* extended query with prepared statements */
	NUM_QUERYMODE
} QueryMode;

/*
 * struct Command represents one command in a script.
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

/*
 * ParsedScript represents one loaded script.
 */
typedef struct ParsedScript
{
	const char *desc;			/* script descriptor (eg, file name) */
	int			weight;			/* selection weight */
	Command   **commands;		/* NULL-terminated array of Commands */
	StatsData	stats;			/* total time spent in script */
} ParsedScript;

/* Builtin test scripts */
typedef struct BuiltinScript
{
	const char *name;			/* very short name for -b ... */
	const char *desc;			/* short description */
	const char *script;			/* actual pgbench script */
} BuiltinScript;

/* Global script repository and configuration */
extern ParsedScript sql_script[MAX_SCRIPTS];
extern int	num_scripts;
extern int64 total_weight;
extern QueryMode querymode;
extern const char *const QUERYMODE[];
extern const BuiltinScript builtin_script[];
extern const size_t num_builtin_scripts;

/* Parser & Scanner external prototypes */
extern int	expr_yyparse(PgBenchExpr **expr_parse_result_p, yyscan_t yyscanner);
extern int	expr_yylex(union YYSTYPE *yylval_param, yyscan_t yyscanner);
pg_noreturn extern void expr_yyerror(PgBenchExpr **expr_parse_result_p, yyscan_t yyscanner, const char *message);
pg_noreturn extern void expr_yyerror_more(yyscan_t yyscanner, const char *message,
										  const char *more);
extern bool expr_lex_one_word(PsqlScanState state, PQExpBuffer word_buf,
							  int *offset);
extern yyscan_t expr_scanner_init(PsqlScanState state,
								  const char *source, int lineno, int start_offset,
								  const char *command);
extern void expr_scanner_finish(yyscan_t yyscanner);
extern char *expr_scanner_get_substring(PsqlScanState state,
										int start_offset,
										bool chomp);
pg_noreturn extern void syntax_error(const char *source, int lineno, const char *line,
									 const char *command, const char *msg,
									 const char *more, int column);

/* Script parsing and loading APIs */
extern char *skip_sql_comments(char *sql_command);
extern Command *create_sql_command(PQExpBuffer buf);
extern void free_command(Command *command);
extern void postprocess_sql_command(Command *my_command);
extern bool parseQuery(Command *cmd);
extern MetaCommand getMetaCommand(const char *cmd);
extern Command *process_backslash_command(PsqlScanState sstate, const char *source,
										  int lineno, int start_offset);
pg_noreturn extern void ConditionError(const char *desc, int cmdn, const char *msg);
extern void CheckConditional(const ParsedScript *ps);
extern void ParseScript(const char *script, const char *desc, int weight);
extern void addScript(const ParsedScript *script);
extern char *read_file_contents(FILE *fd);
extern void process_file(const char *filename, int weight);
extern void process_builtin(const BuiltinScript *bi, int weight);
extern void listAvailableScripts(void);
extern const BuiltinScript *findBuiltin(const char *name);
extern int	parseScriptWeight(const char *option, char **script);
extern int	chooseScript(pg_prng_state *random_state);

/* Expression evaluation APIs */
extern bool isLazyFunc(PgBenchFunction func);
extern bool evalLazyFunc(CState *st, PgBenchFunction func,
						 PgBenchExprLink *args, PgBenchValue *retval);
extern bool evalStandardFunc(CState *st, PgBenchFunction func,
							 PgBenchExprLink *args, PgBenchValue *retval);
extern bool evalFunc(CState *st, PgBenchFunction func,
					 PgBenchExprLink *args, PgBenchValue *retval);
extern bool evaluateExpr(CState *st, PgBenchExpr *expr, PgBenchValue *retval);

#endif							/* PGBENCH_SCRIPT_H */
