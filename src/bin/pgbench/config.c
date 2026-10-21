/*-------------------------------------------------------------------------
 *
 * config.c
 *		JSON / JSONC configuration file parser, serializer (--config and
 *		--dump-config), and multi-stage initialization runner for pgbench.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/config.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commands.h"
#include "common/int.h"
#include "common/jsonapi.h"
#include "common/logging.h"
#include "common/string.h"
#include "config.h"
#include "dataset.h"
#include "engine.h"
#include "fe_utils/conditional.h"
#include "fe_utils/option_utils.h"
#include "init.h"
#include "mb/pg_wchar.h"
#include "script.h"
#include "stats.h"
#include "variable.h"

char	   *config_suite_name = NULL;
char	   *config_description = NULL;
bool		config_scripts_loaded = false;
int			num_config_init_steps = 0;
Variables	config_variables;

/*-------------------------------------------------------------------------
 * In-memory JSON DOM representation built via common/jsonapi.c
 *-------------------------------------------------------------------------
 */
typedef enum ConfigJsonType
{
	CJSON_NULL = 0,
	CJSON_BOOL,
	CJSON_NUMBER,
	CJSON_STRING,
	CJSON_ARRAY,
	CJSON_OBJECT
} ConfigJsonType;

typedef struct ConfigJsonNode ConfigJsonNode;

typedef struct ConfigJsonField
{
	char		   *key;
	int				line_number;
	ConfigJsonNode *val;
} ConfigJsonField;

struct ConfigJsonNode
{
	ConfigJsonType	type;
	int				line_number;
	char		   *scalar_str;		/* for CJSON_STRING, CJSON_NUMBER */
	bool			bool_val;		/* for CJSON_BOOL */
	/* for CJSON_ARRAY */
	ConfigJsonNode **elems;
	int				nelems;
	int				max_elems;
	/* for CJSON_OBJECT */
	ConfigJsonField *fields;
	int				nfields;
	int				max_fields;
};

#define MAX_JSON_NESTING 64

typedef struct ConfigJsonParseState
{
	JsonLexContext *lex;
	ConfigJsonNode *root;
	ConfigJsonNode *stack[MAX_JSON_NESTING];
	char		   *pending_key[MAX_JSON_NESTING];
	int				pending_key_line[MAX_JSON_NESTING];
	int				depth;
} ConfigJsonParseState;

#define MAX_CONFIG_INIT_STEPS 64

typedef struct ConfigInitStep
{
	char	   *name;
	char	   *file;
	char	   *builtin_init;
	int			clients;
	int			threads;
	int			transactions;
	int			scale;
	Variables	variables;
} ConfigInitStep;

static ConfigInitStep config_init_steps[MAX_CONFIG_INIT_STEPS];

static ConfigJsonNode *
allocJsonNode(ConfigJsonType type, int line_number)
{
	ConfigJsonNode *n = pg_malloc0_object(ConfigJsonNode);

	n->type = type;
	n->line_number = line_number;
	return n;
}

static void
freeJsonNode(ConfigJsonNode *node)
{
	if (node == NULL)
		return;

	pg_free(node->scalar_str);
	if (node->type == CJSON_ARRAY)
	{
		for (int i = 0; i < node->nelems; i++)
			freeJsonNode(node->elems[i]);
		pg_free(node->elems);
	}
	else if (node->type == CJSON_OBJECT)
	{
		for (int i = 0; i < node->nfields; i++)
		{
			pg_free(node->fields[i].key);
			freeJsonNode(node->fields[i].val);
		}
		pg_free(node->fields);
	}
	pg_free(node);
}

static void
attachJsonChild(ConfigJsonParseState *st, ConfigJsonNode *child)
{
	ConfigJsonNode *parent;

	if (st->depth == 0)
	{
		st->root = child;
		return;
	}

	parent = st->stack[st->depth - 1];

	if (parent->type == CJSON_ARRAY)
	{
		if (parent->nelems >= parent->max_elems)
		{
			parent->max_elems = (parent->max_elems == 0) ? 8 : parent->max_elems * 2;
			parent->elems = (parent->elems == NULL) ?
				pg_malloc_array(ConfigJsonNode *, parent->max_elems) :
				pg_realloc_array(parent->elems, ConfigJsonNode *, parent->max_elems);
		}
		parent->elems[parent->nelems++] = child;
	}
	else if (parent->type == CJSON_OBJECT)
	{
		char	   *k = st->pending_key[st->depth - 1];
		int			kline = st->pending_key_line[st->depth - 1];

		st->pending_key[st->depth - 1] = NULL;
		if (k == NULL)
			k = pg_strdup("");

		/* If key already exists in object, replace previous value */
		for (int i = 0; i < parent->nfields; i++)
		{
			if (strcmp(parent->fields[i].key, k) == 0)
			{
				pg_free(k);
				freeJsonNode(parent->fields[i].val);
				parent->fields[i].val = child;
				parent->fields[i].line_number = kline;
				return;
			}
		}

		if (parent->nfields >= parent->max_fields)
		{
			parent->max_fields = (parent->max_fields == 0) ? 8 : parent->max_fields * 2;
			parent->fields = (parent->fields == NULL) ?
				pg_malloc_array(ConfigJsonField, parent->max_fields) :
				pg_realloc_array(parent->fields, ConfigJsonField, parent->max_fields);
		}
		parent->fields[parent->nfields].key = k;
		parent->fields[parent->nfields].line_number = kline;
		parent->fields[parent->nfields].val = child;
		parent->nfields++;
	}
}

static JsonParseErrorType
cjson_object_start(void *state)
{
	ConfigJsonParseState *st = (ConfigJsonParseState *) state;
	ConfigJsonNode *node;

	if (st->depth >= MAX_JSON_NESTING)
		return JSON_NESTING_TOO_DEEP;

	node = allocJsonNode(CJSON_OBJECT, st->lex->line_number);
	attachJsonChild(st, node);
	st->stack[st->depth] = node;
	st->pending_key[st->depth] = NULL;
	st->depth++;
	return JSON_SUCCESS;
}

static JsonParseErrorType
cjson_object_end(void *state)
{
	ConfigJsonParseState *st = (ConfigJsonParseState *) state;

	if (st->depth > 0)
	{
		st->depth--;
		pg_free(st->pending_key[st->depth]);
		st->pending_key[st->depth] = NULL;
	}
	return JSON_SUCCESS;
}

static JsonParseErrorType
cjson_array_start(void *state)
{
	ConfigJsonParseState *st = (ConfigJsonParseState *) state;
	ConfigJsonNode *node;

	if (st->depth >= MAX_JSON_NESTING)
		return JSON_NESTING_TOO_DEEP;

	node = allocJsonNode(CJSON_ARRAY, st->lex->line_number);
	attachJsonChild(st, node);
	st->stack[st->depth] = node;
	st->pending_key[st->depth] = NULL;
	st->depth++;
	return JSON_SUCCESS;
}

static JsonParseErrorType
cjson_array_end(void *state)
{
	ConfigJsonParseState *st = (ConfigJsonParseState *) state;

	if (st->depth > 0)
		st->depth--;
	return JSON_SUCCESS;
}

static JsonParseErrorType
cjson_object_field_start(void *state, char *fname, bool isnull)
{
	ConfigJsonParseState *st = (ConfigJsonParseState *) state;

	if (st->depth > 0)
	{
		pg_free(st->pending_key[st->depth - 1]);
		st->pending_key[st->depth - 1] = pg_strdup(fname ? fname : "");
		st->pending_key_line[st->depth - 1] = st->lex->line_number;
	}
	return JSON_SUCCESS;
}

static JsonParseErrorType
cjson_scalar(void *state, char *token, JsonTokenType tokentype)
{
	ConfigJsonParseState *st = (ConfigJsonParseState *) state;
	ConfigJsonNode *node = NULL;
	int			line = st->lex->line_number;

	switch (tokentype)
	{
		case JSON_TOKEN_STRING:
			node = allocJsonNode(CJSON_STRING, line);
			node->scalar_str = pg_strdup(token ? token : "");
			break;
		case JSON_TOKEN_NUMBER:
			node = allocJsonNode(CJSON_NUMBER, line);
			node->scalar_str = pg_strdup(token ? token : "0");
			break;
		case JSON_TOKEN_TRUE:
			node = allocJsonNode(CJSON_BOOL, line);
			node->bool_val = true;
			break;
		case JSON_TOKEN_FALSE:
			node = allocJsonNode(CJSON_BOOL, line);
			node->bool_val = false;
			break;
		case JSON_TOKEN_NULL:
		default:
			node = allocJsonNode(CJSON_NULL, line);
			break;
	}

	attachJsonChild(st, node);
	return JSON_SUCCESS;
}

/*
 * Strip JSONC comments (//, #, and block comments) and trailing commas before
 * '}' or ']' while preserving string literals and exact line numbers.
 */
static char *
stripJsoncCommentsAndTrailingCommas(const char *raw, size_t len,
									const char *config_path)
{
	char	   *clean = pg_malloc(len + 1);
	bool		in_string = false;
	int			line_no = 1;
	char		prev_sig = '\0';

	memcpy(clean, raw, len);
	clean[len] = '\0';

	/* Pass 1: strip //, #, and block comments */
	for (size_t i = 0; i < len; i++)
	{
		char		c = clean[i];

		if (c == '\n')
		{
			line_no++;
			continue;
		}

		if (in_string)
		{
			if (c == '\\' && i + 1 < len)
			{
				i++;
				if (clean[i] == '\n')
					line_no++;
				continue;
			}
			if (c == '"')
				in_string = false;
			continue;
		}

		if (c == '"')
		{
			in_string = true;
			continue;
		}

		/* Line comment starting with // */
		if (c == '/' && i + 1 < len && clean[i + 1] == '/')
		{
			while (i < len && clean[i] != '\n' && clean[i] != '\r')
			{
				clean[i] = ' ';
				i++;
			}
			if (i < len && clean[i] == '\n')
				line_no++;
			continue;
		}

		/* Line comment starting with # */
		if (c == '#')
		{
			while (i < len && clean[i] != '\n' && clean[i] != '\r')
			{
				clean[i] = ' ';
				i++;
			}
			if (i < len && clean[i] == '\n')
				line_no++;
			continue;
		}

		/* Block comment starting with / * */
		if (c == '/' && i + 1 < len && clean[i + 1] == '*')
		{
			int			start_line = line_no;
			bool		closed = false;

			clean[i] = ' ';
			clean[i + 1] = ' ';
			i += 2;
			while (i < len)
			{
				if (clean[i] == '*' && i + 1 < len && clean[i + 1] == '/')
				{
					clean[i] = ' ';
					clean[i + 1] = ' ';
					i++;
					closed = true;
					break;
				}
				if (clean[i] == '\n')
					line_no++;
				else if (clean[i] != '\r')
					clean[i] = ' ';
				i++;
			}
			if (!closed)
				pg_fatal("%s:%d: unterminated /* ... */ block comment in configuration file",
						 config_path, start_line);
			continue;
		}
	}

	/* Pass 2: strip trailing commas before '}' or ']' */
	in_string = false;
	prev_sig = '\0';
	for (size_t i = 0; i < len; i++)
	{
		char		c = clean[i];

		if (in_string)
		{
			if (c == '\\' && i + 1 < len)
			{
				i++;
				continue;
			}
			if (c == '"')
			{
				in_string = false;
				prev_sig = '"';
			}
			continue;
		}

		if (c == '"')
		{
			in_string = true;
			prev_sig = '"';
			continue;
		}

		if (c == ',')
		{
			size_t		j = i + 1;

			while (j < len && (clean[j] == ' ' || clean[j] == '\t' ||
							   clean[j] == '\n' || clean[j] == '\r'))
				j++;

			if (j < len && (clean[j] == '}' || clean[j] == ']') &&
				prev_sig != '\0' && prev_sig != '{' && prev_sig != '[' &&
				prev_sig != ',' && prev_sig != ':')
			{
				clean[i] = ' ';
				continue;
			}
		}

		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			prev_sig = c;
	}

	return clean;
}

static ConfigJsonNode *
parseConfigJsonFile(const char *config_path)
{
	FILE	   *fp;
	struct stat st;
	char	   *raw_buf;
	char	   *clean_buf;
	size_t		nread;
	JsonLexContext *lex;
	JsonSemAction sem;
	ConfigJsonParseState pstate;
	JsonParseErrorType res;

	fp = fopen(config_path, PG_BINARY_R);
	if (fp == NULL)
		pg_fatal("could not open configuration file \"%s\": %m", config_path);

	if (fstat(fileno(fp), &st) < 0)
	{
		fclose(fp);
		pg_fatal("could not stat configuration file \"%s\": %m", config_path);
	}

	raw_buf = pg_malloc((size_t) st.st_size + 1);
	nread = fread(raw_buf, 1, (size_t) st.st_size, fp);
	if (ferror(fp))
	{
		fclose(fp);
		pg_free(raw_buf);
		pg_fatal("could not read configuration file \"%s\": %m", config_path);
	}
	fclose(fp);
	raw_buf[nread] = '\0';

	clean_buf = stripJsoncCommentsAndTrailingCommas(raw_buf, nread, config_path);
	pg_free(raw_buf);

	lex = makeJsonLexContextCstringLen(NULL, clean_buf, nread, PG_UTF8, true);
	setJsonLexContextOwnsTokens(lex, true);

	memset(&pstate, 0, sizeof(pstate));
	pstate.lex = lex;

	memset(&sem, 0, sizeof(sem));
	sem.semstate = &pstate;
	sem.object_start = cjson_object_start;
	sem.object_end = cjson_object_end;
	sem.array_start = cjson_array_start;
	sem.array_end = cjson_array_end;
	sem.object_field_start = cjson_object_field_start;
	sem.scalar = cjson_scalar;

	res = pg_parse_json(lex, &sem);
	if (res != JSON_SUCCESS)
	{
		char	   *detail = json_errdetail(res, lex);

		pg_fatal("%s:%d: invalid JSON in configuration file: %s",
				 config_path, lex->line_number,
				 detail ? detail : "syntax error");
	}

	freeJsonLexContext(lex);
	pg_free(clean_buf);

	if (pstate.root == NULL || pstate.root->type != CJSON_OBJECT)
		pg_fatal("%s: top-level JSON value in configuration file must be an object",
				 config_path);

	return pstate.root;
}

/*-------------------------------------------------------------------------
 * JSON DOM extraction helpers with line-numbered diagnostics
 *-------------------------------------------------------------------------
 */
static ConfigJsonField *
findObjectField(const ConfigJsonNode *obj, const char *key)
{
	if (obj == NULL || obj->type != CJSON_OBJECT)
		return NULL;
	for (int i = 0; i < obj->nfields; i++)
	{
		if (strcmp(obj->fields[i].key, key) == 0)
			return &obj->fields[i];
	}
	return NULL;
}

static const char *
expectJsonString(const char *config_path, const char *key_name,
				 const ConfigJsonNode *node)
{
	if (node->type != CJSON_STRING)
		pg_fatal("%s:%d: configuration key \"%s\" expects a string value",
				 config_path, node->line_number, key_name);
	return node->scalar_str;
}

static bool
expectJsonBool(const char *config_path, const char *key_name,
			   const ConfigJsonNode *node)
{
	if (node->type == CJSON_BOOL)
		return node->bool_val;
	if (node->type == CJSON_STRING)
	{
		if (pg_strcasecmp(node->scalar_str, "true") == 0 ||
			strcmp(node->scalar_str, "1") == 0 ||
			pg_strcasecmp(node->scalar_str, "on") == 0)
			return true;
		if (pg_strcasecmp(node->scalar_str, "false") == 0 ||
			strcmp(node->scalar_str, "0") == 0 ||
			pg_strcasecmp(node->scalar_str, "off") == 0)
			return false;
	}
	pg_fatal("%s:%d: configuration key \"%s\" expects a boolean value (true or false)",
			 config_path, node->line_number, key_name);
	return false;
}

static int64
expectJsonInt64(const char *config_path, const char *key_name,
				const ConfigJsonNode *node, int64 min_val, int64 max_val)
{
	int64		val = 0;

	if ((node->type != CJSON_NUMBER && node->type != CJSON_STRING) ||
		!strtoint64(node->scalar_str, true, &val))
		pg_fatal("%s:%d: configuration key \"%s\" expects an integer value (got \"%s\")",
				 config_path, node->line_number, key_name,
				 node->scalar_str ? node->scalar_str : "");

	if (val < min_val || val > max_val)
		pg_fatal("%s:%d: value " INT64_FORMAT " for configuration key \"%s\" is out of range (" INT64_FORMAT " .. " INT64_FORMAT ")",
				 config_path, node->line_number, val, key_name, min_val, max_val);

	return val;
}

static int
expectJsonInt(const char *config_path, const char *key_name,
			  const ConfigJsonNode *node, int min_val, int max_val)
{
	return (int) expectJsonInt64(config_path, key_name, node,
								 (int64) min_val, (int64) max_val);
}

static double
expectJsonDouble(const char *config_path, const char *key_name,
				 const ConfigJsonNode *node)
{
	double		val = 0.0;

	if ((node->type != CJSON_NUMBER && node->type != CJSON_STRING) ||
		!strtodouble(node->scalar_str, true, &val))
		pg_fatal("%s:%d: configuration key \"%s\" expects a numeric value (got \"%s\")",
				 config_path, node->line_number, key_name,
				 node->scalar_str ? node->scalar_str : "");
	return val;
}

/*
 * Resolve a file path from a configuration file. If the path is relative and
 * the configuration file resides in a directory, resolve relative to the
 * configuration file's directory first, falling back to the current working
 * directory if the file exists there.
 */
static char *
resolveConfigPath(const char *config_dir, const char *file_path)
{
	char	   *candidate;

	if (file_path == NULL || *file_path == '\0')
		return pg_strdup("");

	if (is_absolute_path(file_path) || config_dir == NULL || *config_dir == '\0' ||
		strcmp(config_dir, ".") == 0)
		return pg_strdup(file_path);

	candidate = psprintf("%s/%s", config_dir, file_path);
	if (access(candidate, F_OK) == 0)
		return candidate;
	if (access(file_path, F_OK) == 0)
	{
		pg_free(candidate);
		return pg_strdup(file_path);
	}
	return candidate;
}

static char *
getConfigDir(const char *config_path)
{
	const char *last_sep = NULL;

	if (config_path == NULL)
		return NULL;

	for (const char *p = config_path; *p != '\0'; p++)
	{
		if (*p == '/' || *p == '\\')
			last_sep = p;
	}
	if (last_sep == NULL)
		return NULL;
	if (last_sep == config_path)
		return pg_strdup("/");
	return pnstrdup(config_path, last_sep - config_path);
}

/*
 * Store a JSON value into a pgbench Variables store.
 */
static void
applyJsonVariable(const char *config_path, Variables *vars,
				  const char *var_name, const ConfigJsonNode *val_node)
{
	if (vars == NULL)
		return;

	switch (val_node->type)
	{
		case CJSON_NULL:
			{
				PgBenchValue v;

				setNullValue(&v);
				if (!putVariableValue(vars, "config", (char *) var_name, &v))
					exit(1);
			}
			break;
		case CJSON_BOOL:
			{
				PgBenchValue v;

				setBoolValue(&v, val_node->bool_val);
				if (!putVariableValue(vars, "config", (char *) var_name, &v))
					exit(1);
			}
			break;
		case CJSON_NUMBER:
			{
				int64		ival;
				double		dval;

				if (is_an_int(val_node->scalar_str) &&
					strtoint64(val_node->scalar_str, true, &ival))
				{
					if (!putVariableInt(vars, "config", (char *) var_name, ival))
						exit(1);
				}
				else if (strtodouble(val_node->scalar_str, true, &dval))
				{
					PgBenchValue v;

					setDoubleValue(&v, dval);
					if (!putVariableValue(vars, "config", (char *) var_name, &v))
						exit(1);
				}
				else
				{
					if (!putVariable(vars, "config", (char *) var_name, val_node->scalar_str))
						exit(1);
				}
			}
			break;
		case CJSON_STRING:
			if (!putVariable(vars, "config", (char *) var_name, val_node->scalar_str))
				exit(1);
			break;
		case CJSON_ARRAY:
			{
				PgBenchValue arr_v;

				arr_v.type = PGBT_ARRAY;
				arr_v.u.aval.nelems = val_node->nelems;
				arr_v.u.aval.elems = (val_node->nelems > 0) ?
					pg_malloc0_array(PgBenchValue, val_node->nelems) : NULL;

				for (int i = 0; i < val_node->nelems; i++)
				{
					const ConfigJsonNode *el = val_node->elems[i];
					int64		ival;
					double		dval;

					if (el->type == CJSON_NULL)
						setNullValue(&arr_v.u.aval.elems[i]);
					else if (el->type == CJSON_BOOL)
						setBoolValue(&arr_v.u.aval.elems[i], el->bool_val);
					else if (el->type == CJSON_NUMBER &&
							 is_an_int(el->scalar_str) &&
							 strtoint64(el->scalar_str, true, &ival))
						setIntValue(&arr_v.u.aval.elems[i], ival);
					else if (el->type == CJSON_NUMBER &&
							 strtodouble(el->scalar_str, true, &dval))
						setDoubleValue(&arr_v.u.aval.elems[i], dval);
					else if (el->type == CJSON_STRING || el->type == CJSON_NUMBER)
						setTextValue(&arr_v.u.aval.elems[i], el->scalar_str);
					else
						pg_fatal("%s:%d: nested arrays/objects inside variable \"%s\" are not supported",
								 config_path, el->line_number, var_name);
				}
				if (!putVariableValue(vars, "config", (char *) var_name, &arr_v))
					exit(1);
				freeValue(&arr_v);
			}
			break;
		default:
			pg_fatal("%s:%d: variable \"%s\" must be a scalar or array",
					 config_path, val_node->line_number, var_name);
			break;
	}
}

/*
 * Parse a single script entry (string "path@W" or object {"file": "...", "weight": W}
 * or {"builtin": "...", "weight": W} or {"name": "...", "weight": W}).
 * Returns the index in sql_script[] and sets *out_weight.
 */
static int
parseConfigScriptEntry(const char *config_path, const char *config_dir,
					   const ConfigJsonNode *entry, int *out_weight)
{
	int			idx = num_scripts;
	int			weight = 1;

	if (entry->type == CJSON_STRING)
	{
		char	   *raw_script = NULL;
		char	   *resolved;
		const BuiltinScript *bi;

		weight = parseScriptWeight(entry->scalar_str, &raw_script);
		bi = findBuiltinOrNull(raw_script);
		if (bi != NULL && access(raw_script, F_OK) != 0)
		{
			process_builtin(bi, weight);
			internal_script_used = true;
			pg_free(raw_script);
		}
		else
		{
			resolved = resolveConfigPath(config_dir, raw_script);
			pg_free(raw_script);
			process_file(resolved, weight);
		}
	}
	else if (entry->type == CJSON_OBJECT)
	{
		ConfigJsonField *f_file = findObjectField(entry, "file");
		ConfigJsonField *f_builtin = findObjectField(entry, "builtin");
		ConfigJsonField *f_name = findObjectField(entry, "name");
		ConfigJsonField *f_weight = findObjectField(entry, "weight");

		if (f_weight != NULL)
			weight = expectJsonInt(config_path, "scripts[].weight",
								   f_weight->val, 0, INT_MAX);

		if (f_builtin != NULL)
		{
			const char *bname = expectJsonString(config_path, "scripts[].builtin",
												 f_builtin->val);
			const BuiltinScript *bi = findBuiltinOrNull(bname);

			if (bi == NULL)
				bi = findBuiltin(bname);
			process_builtin(bi, weight);
			internal_script_used = true;
		}
		else if (f_file != NULL)
		{
			const char *fpath = expectJsonString(config_path, "scripts[].file",
												 f_file->val);
			char	   *resolved = resolveConfigPath(config_dir, fpath);

			process_file(resolved, weight);
		}
		else if (f_name != NULL)
		{
			const char *nstr = expectJsonString(config_path, "scripts[].name",
												f_name->val);
			const BuiltinScript *bi = findBuiltinOrNull(nstr);

			if (bi != NULL)
			{
				process_builtin(bi, weight);
				internal_script_used = true;
			}
			else if (strcmp(nstr, "<builtin: trace-replay>") == 0)
			{
				process_replay_builtin(weight);
			}
			else
			{
				char	   *resolved = resolveConfigPath(config_dir, nstr);

				process_file(resolved, weight);
			}
		}
		else
			pg_fatal("%s:%d: script object must specify \"file\" or \"builtin\"",
					 config_path, entry->line_number);
	}
	else
		pg_fatal("%s:%d: script entry must be an object or string",
				 config_path, entry->line_number);

	if (out_weight != NULL)
		*out_weight = weight;
	return idx;
}

static void
parseConfigGroups(const char *config_path, const char *config_dir,
				  const ConfigJsonNode *groups_node)
{
	if (groups_node->type != CJSON_ARRAY)
		pg_fatal("%s:%d: \"groups\" must be an array",
				 config_path, groups_node->line_number);

	/* Replace any previously loaded groups/scripts from earlier config */
	clearConfigScriptsAndGroups();

	for (int g = 0; g < groups_node->nelems; g++)
	{
		const ConfigJsonNode *gnode = groups_node->elems[g];
		ClientGroup *grp;

		if (gnode->type != CJSON_OBJECT)
			pg_fatal("%s:%d: each element of \"groups\" must be an object",
					 config_path, gnode->line_number);

		if (num_groups >= MAX_GROUPS)
			pg_fatal("%s:%d: at most %d client groups are allowed",
					 config_path, gnode->line_number, MAX_GROUPS);

		grp = &client_groups[num_groups];
		memset(grp, 0, sizeof(ClientGroup));
		grp->nclients = 1;
		grp->nthreads = 1;
		grp->sequential = false;
		grp->rate = 0.0;
		grp->throttle_delay = 0.0;
		initStats(&grp->stats, 0);

		for (int f = 0; f < gnode->nfields; f++)
		{
			const ConfigJsonField *fld = &gnode->fields[f];
			const char *k = fld->key;
			const ConfigJsonNode *v = fld->val;

			if (strcmp(k, "name") == 0)
			{
				pg_free(grp->name);
				grp->name = pg_strdup(expectJsonString(config_path, "groups[].name", v));
			}
			else if (strcmp(k, "clients") == 0 || strcmp(k, "client") == 0)
				grp->nclients = expectJsonInt(config_path, "groups[].clients", v, 1, INT_MAX);
			else if (strcmp(k, "threads") == 0 || strcmp(k, "jobs") == 0)
				grp->nthreads = expectJsonInt(config_path, "groups[].threads", v, 1, INT_MAX);
			else if (strcmp(k, "sequential") == 0)
				grp->sequential = expectJsonBool(config_path, "groups[].sequential", v);
			else if (strcmp(k, "rate") == 0)
			{
				double		r = expectJsonDouble(config_path, "groups[].rate", v);

				if (r < 0.0)
					pg_fatal("%s:%d: invalid rate limit in group: %g",
							 config_path, v->line_number, r);
				grp->rate = r;
			}
			else if (strcmp(k, "file") == 0)
			{
				const char *fpath = expectJsonString(config_path, "groups[].file", v);
				char	   *raw_s = NULL;
				int			w = parseScriptWeight(fpath, &raw_s);
				char	   *resolved = resolveConfigPath(config_dir, raw_s);
				int			sidx = num_scripts;

				pg_free(raw_s);
				if (grp->num_scripts >= 128)
					pg_fatal("%s:%d: at most 128 scripts are allowed per group",
							 config_path, v->line_number);
				process_file(resolved, w);
				grp->script_indices[grp->num_scripts++] = sidx;
				grp->total_weight += w;
			}
			else if (strcmp(k, "builtin") == 0)
			{
				const char *bspec = expectJsonString(config_path, "groups[].builtin", v);
				char	   *raw_s = NULL;
				int			w = parseScriptWeight(bspec, &raw_s);
				int			sidx = num_scripts;

				if (grp->num_scripts >= 128)
					pg_fatal("%s:%d: at most 128 scripts are allowed per group",
							 config_path, v->line_number);
				process_builtin(findBuiltin(raw_s), w);
				pg_free(raw_s);
				grp->script_indices[grp->num_scripts++] = sidx;
				grp->total_weight += w;
				internal_script_used = true;
			}
			else if (strcmp(k, "scripts") == 0)
			{
				if (v->type != CJSON_ARRAY)
					pg_fatal("%s:%d: \"groups[].scripts\" must be an array",
							 config_path, v->line_number);
				for (int s = 0; s < v->nelems; s++)
				{
					int			w = 1;
					int			sidx;

					if (grp->num_scripts >= 128)
						pg_fatal("%s:%d: at most 128 scripts are allowed per group",
								 config_path, v->elems[s]->line_number);
					sidx = parseConfigScriptEntry(config_path, config_dir,
												  v->elems[s], &w);
					grp->script_indices[grp->num_scripts++] = sidx;
					grp->total_weight += w;
				}
			}
			/* Ignore telemetry fields when loading directly from a --json-report file */
			else if (strcmp(k, "transactions_processed") == 0 ||
					 strcmp(k, "transactions_failed") == 0 ||
					 strcmp(k, "expected_rollbacks") == 0 ||
					 strcmp(k, "tps") == 0 ||
					 strcmp(k, "latency_ms") == 0 ||
					 strcmp(k, "recall") == 0)
			{
				continue;
			}
			else
				pg_fatal("%s:%d: unrecognized key \"%s\" in group object",
						 config_path, fld->line_number, k);
		}

		if (grp->name == NULL)
			grp->name = psprintf("group_%d", num_groups + 1);
		if (grp->num_scripts == 0)
			pg_fatal("%s:%d: group \"%s\" must specify at least one script",
					 config_path, gnode->line_number, grp->name);
		if (!grp->sequential && grp->total_weight == 0)
			pg_fatal("%s:%d: group \"%s\" total script weight must not be zero",
					 config_path, gnode->line_number, grp->name);
		if (grp->nthreads > grp->nclients)
			grp->nthreads = grp->nclients;
		if (grp->rate > 0.0)
			grp->throttle_delay = (1000000.0 / grp->rate) * grp->nthreads;

		num_groups++;
	}

	config_scripts_loaded = (num_scripts > 0 || num_groups > 0);
}

static void
parseConfigDatasetSpecWithDir(const char *config_dir, const char *ds_name,
							  const char *s)
{
	const char *colon = strchr(s, ':');
	char	   *spec;

	if (colon != NULL &&
		(pg_strncasecmp(s, "csv:", 4) == 0 ||
		 pg_strncasecmp(s, "tsv:", 4) == 0 ||
		 pg_strncasecmp(s, "psv:", 4) == 0 ||
		 pg_strncasecmp(s, "pipe:", 5) == 0 ||
		 pg_strncasecmp(s, "fvecs:", 6) == 0 ||
		 pg_strncasecmp(s, "bvecs:", 6) == 0 ||
		 pg_strncasecmp(s, "ivecs:", 6) == 0))
	{
		const char *after_fmt = colon + 1;
		bool		has_hdr = false;
		char	   *fmt_part;
		char	   *resolved;

		if (pg_strncasecmp(after_fmt, "header:", 7) == 0)
		{
			has_hdr = true;
			after_fmt += 7;
		}
		fmt_part = pnstrdup(s, colon - s);
		resolved = resolveConfigPath(config_dir, after_fmt);

		spec = psprintf("%s=%s:%s%s", ds_name, fmt_part,
						has_hdr ? "header:" : "", resolved);
		pg_free(fmt_part);
		pg_free(resolved);
	}
	else
	{
		char	   *resolved = resolveConfigPath(config_dir, s);

		spec = psprintf("%s=%s", ds_name, resolved);
		pg_free(resolved);
	}
	parseDatasetSpec(spec);
	pg_free(spec);
}

static void
parseConfigDatasets(const char *config_path, const char *config_dir,
					const ConfigJsonNode *ds_node)
{
	if (ds_node->type == CJSON_OBJECT)
	{
		for (int i = 0; i < ds_node->nfields; i++)
		{
			const char *ds_name = ds_node->fields[i].key;
			const ConfigJsonNode *v = ds_node->fields[i].val;

			if (v->type == CJSON_STRING)
			{
				parseConfigDatasetSpecWithDir(config_dir, ds_name, v->scalar_str);
			}
			else if (v->type == CJSON_OBJECT)
			{
				ConfigJsonField *f_file = findObjectField(v, "file");
				ConfigJsonField *f_path = findObjectField(v, "path");
				ConfigJsonField *f_fmt = findObjectField(v, "format");
				ConfigJsonField *f_hdr = findObjectField(v, "header");
				ConfigJsonField *f_off = findObjectField(v, "id_offset");
				const char *fpath;
				char	   *resolved;
				char	   *spec;

				if (f_file == NULL)
					f_file = f_path;
				if (f_off == NULL)
					f_off = findObjectField(v, "offset");
				if (f_file == NULL)
					pg_fatal("%s:%d: dataset \"%s\" object must specify \"file\"",
							 config_path, v->line_number, ds_name);
				fpath = expectJsonString(config_path, "datasets.file", f_file->val);
				resolved = resolveConfigPath(config_dir, fpath);

				if (f_fmt != NULL)
				{
					const char *fmt = expectJsonString(config_path, "datasets.format", f_fmt->val);
					bool		hdr = (f_hdr != NULL) ?
						expectJsonBool(config_path, "datasets.header", f_hdr->val) : false;

					spec = psprintf("%s=%s:%s%s", ds_name, fmt,
									hdr ? "header:" : "", resolved);
				}
				else
					spec = psprintf("%s=%s", ds_name, resolved);

				pg_free(resolved);
				parseDatasetSpec(spec);
				pg_free(spec);

				if (f_off != NULL)
				{
					PgBenchDataset *ds = findDataset(ds_name);

					if (ds != NULL)
						ds->id_offset = expectJsonInt64(config_path, "datasets.id_offset",
														f_off->val, INT64_MIN, INT64_MAX);
				}
			}
			else
				pg_fatal("%s:%d: dataset \"%s\" value must be a path string or object",
						 config_path, v->line_number, ds_name);
		}
	}
	else if (ds_node->type == CJSON_ARRAY)
	{
		for (int i = 0; i < ds_node->nelems; i++)
		{
			const ConfigJsonNode *el = ds_node->elems[i];

			if (el->type == CJSON_STRING)
			{
				const char *eq = strchr(el->scalar_str, '=');

				if (eq != NULL && eq > el->scalar_str)
				{
					char	   *ds_name = pnstrdup(el->scalar_str, eq - el->scalar_str);

					parseConfigDatasetSpecWithDir(config_dir, ds_name, eq + 1);
					pg_free(ds_name);
				}
				else
					parseDatasetSpec(el->scalar_str);
			}
			else if (el->type == CJSON_OBJECT)
			{
				ConfigJsonField *f_name = findObjectField(el, "name");
				ConfigJsonField *f_file = findObjectField(el, "file");
				ConfigJsonField *f_fmt = findObjectField(el, "format");
				ConfigJsonField *f_hdr = findObjectField(el, "header");
				ConfigJsonField *f_off = findObjectField(el, "id_offset");
				const char *ds_name;
				const char *fpath;
				char	   *resolved;
				char	   *spec;

				if (f_off == NULL)
					f_off = findObjectField(el, "offset");
				if (f_name == NULL || f_file == NULL)
					pg_fatal("%s:%d: dataset array object must specify \"name\" and \"file\"",
							 config_path, el->line_number);
				ds_name = expectJsonString(config_path, "datasets[].name", f_name->val);
				fpath = expectJsonString(config_path, "datasets[].file", f_file->val);
				resolved = resolveConfigPath(config_dir, fpath);
				if (f_fmt != NULL)
				{
					const char *fmt = expectJsonString(config_path, "datasets[].format", f_fmt->val);
					bool		hdr = (f_hdr != NULL) ?
						expectJsonBool(config_path, "datasets[].header", f_hdr->val) : false;

					spec = psprintf("%s=%s:%s%s", ds_name, fmt,
									hdr ? "header:" : "", resolved);
				}
				else
					spec = psprintf("%s=%s", ds_name, resolved);
				pg_free(resolved);
				parseDatasetSpec(spec);
				pg_free(spec);

				if (f_off != NULL)
				{
					PgBenchDataset *ds = findDataset(ds_name);

					if (ds != NULL)
						ds->id_offset = expectJsonInt64(config_path, "datasets[].id_offset",
														f_off->val, INT64_MIN, INT64_MAX);
				}
			}
			else
				pg_fatal("%s:%d: dataset array element must be an object or string",
						 config_path, el->line_number);
		}
	}
	else
		pg_fatal("%s:%d: \"datasets\" must be an object or array",
				 config_path, ds_node->line_number);
}

static void
parseConfigInitSteps(const char *config_path, const char *config_dir,
					 const ConfigJsonNode *init_node,
					 bool is_init_mode,
					 bool *initialization_option_set)
{
	if (init_node->type == CJSON_STRING)
	{
		pg_free(initialize_steps);
		initialize_steps = pg_strdup(init_node->scalar_str);
		checkInitSteps(initialize_steps);
		if (is_init_mode && initialization_option_set != NULL)
			*initialization_option_set = true;
		return;
	}

	if (init_node->type != CJSON_ARRAY)
		pg_fatal("%s:%d: \"init_steps\" must be a string (e.g. \"dtgvp\") or an array of step objects",
				 config_path, init_node->line_number);

	/* Clear any previous init steps */
	for (int i = 0; i < num_config_init_steps; i++)
	{
		pg_free(config_init_steps[i].name);
		pg_free(config_init_steps[i].file);
		pg_free(config_init_steps[i].builtin_init);
		destroyVariables(&config_init_steps[i].variables);
	}
	num_config_init_steps = 0;

	for (int i = 0; i < init_node->nelems; i++)
	{
		const ConfigJsonNode *el = init_node->elems[i];
		ConfigInitStep *step;

		if (num_config_init_steps >= MAX_CONFIG_INIT_STEPS)
			pg_fatal("%s:%d: at most %d init_steps are allowed",
					 config_path, el->line_number, MAX_CONFIG_INIT_STEPS);

		step = &config_init_steps[num_config_init_steps];
		memset(step, 0, sizeof(ConfigInitStep));
		step->clients = 1;
		step->threads = 1;
		step->transactions = 1;
		step->scale = 0;

		if (el->type == CJSON_STRING)
		{
			char	   *resolved = resolveConfigPath(config_dir, el->scalar_str);

			step->name = psprintf("step_%d", i + 1);
			step->file = resolved;
			num_config_init_steps++;
			continue;
		}

		if (el->type != CJSON_OBJECT)
			pg_fatal("%s:%d: each element of \"init_steps\" must be an object or script path string",
					 config_path, el->line_number);

		for (int f = 0; f < el->nfields; f++)
		{
			const ConfigJsonField *fld = &el->fields[f];
			const char *k = fld->key;
			const ConfigJsonNode *v = fld->val;

			if (strcmp(k, "name") == 0)
			{
				pg_free(step->name);
				step->name = pg_strdup(expectJsonString(config_path, "init_steps[].name", v));
			}
			else if (strcmp(k, "file") == 0)
			{
				const char *fpath = expectJsonString(config_path, "init_steps[].file", v);

				pg_free(step->file);
				step->file = resolveConfigPath(config_dir, fpath);
			}
			else if (strcmp(k, "builtin_init") == 0 || strcmp(k, "init_steps") == 0)
			{
				const char *bsteps = expectJsonString(config_path, "init_steps[].builtin_init", v);

				checkInitSteps(bsteps);
				pg_free(step->builtin_init);
				step->builtin_init = pg_strdup(bsteps);
			}
			else if (strcmp(k, "clients") == 0 || strcmp(k, "client") == 0)
				step->clients = expectJsonInt(config_path, "init_steps[].clients", v, 1, INT_MAX);
			else if (strcmp(k, "threads") == 0 || strcmp(k, "jobs") == 0)
				step->threads = expectJsonInt(config_path, "init_steps[].threads", v, 1, INT_MAX);
			else if (strcmp(k, "transactions") == 0)
				step->transactions = expectJsonInt(config_path, "init_steps[].transactions", v, 1, INT_MAX);
			else if (strcmp(k, "scale") == 0)
				step->scale = expectJsonInt(config_path, "init_steps[].scale", v, 1, INT_MAX);
			else if (strcmp(k, "variables") == 0 || strcmp(k, "define") == 0)
			{
				if (v->type != CJSON_OBJECT)
					pg_fatal("%s:%d: \"init_steps[].variables\" must be an object",
							 config_path, v->line_number);
				for (int vf = 0; vf < v->nfields; vf++)
					applyJsonVariable(config_path, &step->variables,
									  v->fields[vf].key, v->fields[vf].val);
			}
			else if (strcmp(k, "description") == 0 || strcmp(k, "comment") == 0)
				continue;
			else
				pg_fatal("%s:%d: unrecognized key \"%s\" in init_steps object",
						 config_path, fld->line_number, k);
		}

		if (step->file == NULL && step->builtin_init == NULL)
			pg_fatal("%s:%d: init_steps entry must specify \"file\" or \"builtin_init\"",
					 config_path, el->line_number);
		if (step->name == NULL)
			step->name = psprintf("step_%d", i + 1);
		if (step->threads > step->clients)
			step->threads = step->clients;

		num_config_init_steps++;
	}
}

/*
 * Reconstruct an SLA spec string from a --json-report sla_verifications object
 * when the object does not already include a "spec" field.
 */
static void
parseSlaVerificationObject(const char *config_path, const ConfigJsonNode *obj)
{
	ConfigJsonField *f_spec = findObjectField(obj, "spec");
	ConfigJsonField *f_target;
	ConfigJsonField *f_metric;
	ConfigJsonField *f_named;
	ConfigJsonField *f_pct;
	ConfigJsonField *f_thr;
	ConfigJsonField *f_thr_ms;
	const char *target = "total";
	const char *metric = NULL;
	const char *named = NULL;
	double		thr = 0.0;
	char	   *spec;

	if (f_spec != NULL && f_spec->val->type == CJSON_STRING)
	{
		parseSlaSpec(f_spec->val->scalar_str);
		return;
	}

	f_target = findObjectField(obj, "target");
	f_metric = findObjectField(obj, "metric");
	f_named = findObjectField(obj, "named_metric");
	f_pct = findObjectField(obj, "percentile");
	f_thr = findObjectField(obj, "threshold");
	f_thr_ms = findObjectField(obj, "threshold_ms");

	if (f_target != NULL && f_target->val->type == CJSON_STRING)
		target = f_target->val->scalar_str;
	if (f_metric != NULL && f_metric->val->type == CJSON_STRING)
		metric = f_metric->val->scalar_str;
	if (f_named != NULL && f_named->val->type == CJSON_STRING)
		named = f_named->val->scalar_str;
	if (f_thr != NULL && f_thr->val->type == CJSON_NUMBER)
		thr = expectJsonDouble(config_path, "sla_verifications[].threshold", f_thr->val);
	else if (f_thr_ms != NULL && f_thr_ms->val->type == CJSON_NUMBER)
		thr = expectJsonDouble(config_path, "sla_verifications[].threshold_ms", f_thr_ms->val);

	if (metric != NULL)
	{
		if (pg_strncasecmp(metric, "p", 1) == 0 ||
			pg_strcasecmp(metric, "avg") == 0 ||
			pg_strncasecmp(metric, "lag_p", 5) == 0)
			spec = psprintf("%s:%s<=%gms", target, metric, thr);
		else if (pg_strcasecmp(metric, "tps") == 0 ||
				 pg_strcasecmp(metric, "qps") == 0 ||
				 pg_strcasecmp(metric, "recall") == 0 ||
				 pg_strcasecmp(metric, "ontime_pct") == 0 ||
				 pg_strcasecmp(metric, "validation_pass_rate") == 0)
		{
			if (named != NULL)
				spec = psprintf("%s:%s:%s>=%g", target, named, metric, thr);
			else
				spec = psprintf("%s:%s>=%g", target, metric, thr);
		}
		else
			spec = psprintf("%s:%s<=%g", target, metric, thr);
		parseSlaSpec(spec);
		pg_free(spec);
	}
	else if (f_pct != NULL && f_pct->val->type == CJSON_NUMBER)
	{
		double		pct = expectJsonDouble(config_path, "sla_verifications[].percentile", f_pct->val);

		if (pct > 0.0)
		{
			spec = psprintf("%s:p%g<=%gms", target, pct, thr);
			parseSlaSpec(spec);
			pg_free(spec);
		}
	}
}

void
clearConfigScriptsAndGroups(void)
{
	num_scripts = 0;
	total_weight = 0;
	for (int g = 0; g < num_groups; g++)
	{
		pg_free(client_groups[g].name);
		client_groups[g].name = NULL;
	}
	num_groups = 0;
	internal_script_used = false;
	config_scripts_loaded = false;
}

static void
applyConfigObject(const char *config_path,
				  const char *config_dir,
				  const ConfigJsonNode *obj,
				  bool is_init_mode,
				  Variables *client0_vars)
{
	for (int i = 0; i < obj->nfields; i++)
	{
		const ConfigJsonField *fld = &obj->fields[i];
		const char *k = fld->key;
		const ConfigJsonNode *v = fld->val;

		if (strcmp(k, "$schema") == 0 || strcmp(k, "version") == 0)
			continue;
		else if (strcmp(k, "suite") == 0)
		{
			pg_free(config_suite_name);
			config_suite_name = pg_strdup(expectJsonString(config_path, k, v));
		}
		else if (strcmp(k, "description") == 0 || strcmp(k, "comment") == 0)
		{
			pg_free(config_description);
			config_description = pg_strdup(expectJsonString(config_path, k, v));
		}
		else if (strcmp(k, "host") == 0)
		{
			if (v->type != CJSON_NULL)
				pghost = pg_strdup(expectJsonString(config_path, k, v));
		}
		else if (strcmp(k, "port") == 0)
		{
			if (v->type == CJSON_NUMBER)
			{
				int			p = expectJsonInt(config_path, k, v, 1, 65535);

				pgport = psprintf("%d", p);
			}
			else if (v->type == CJSON_STRING)
				pgport = pg_strdup(v->scalar_str);
			else if (v->type != CJSON_NULL)
				pg_fatal("%s:%d: configuration key \"port\" expects an integer or string",
						 config_path, v->line_number);
		}
		else if (strcmp(k, "username") == 0 || strcmp(k, "user") == 0)
		{
			if (v->type != CJSON_NULL)
				username = pg_strdup(expectJsonString(config_path, k, v));
		}
		else if (strcmp(k, "dbname") == 0 || strcmp(k, "database") == 0)
		{
			if (v->type != CJSON_NULL)
				dbName = pg_strdup(expectJsonString(config_path, k, v));
		}
		else if (strcmp(k, "scale") == 0)
		{
			scale = expectJsonInt(config_path, k, v, 1, INT_MAX);
		}
		else if (strcmp(k, "fillfactor") == 0)
			fillfactor = expectJsonInt(config_path, k, v, 10, 100);
		else if (strcmp(k, "unlogged_tables") == 0)
			unlogged_tables = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "foreign_keys") == 0)
			foreign_keys = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "tablespace") == 0)
		{
			if (v->type != CJSON_NULL)
				tablespace = pg_strdup(expectJsonString(config_path, k, v));
		}
		else if (strcmp(k, "index_tablespace") == 0)
		{
			if (v->type != CJSON_NULL)
				index_tablespace = pg_strdup(expectJsonString(config_path, k, v));
		}
		else if (strcmp(k, "partitions") == 0)
			partitions = expectJsonInt(config_path, k, v, 0, INT_MAX);
		else if (strcmp(k, "partition_method") == 0)
		{
			if (v->type != CJSON_NULL)
			{
				const char *pm = expectJsonString(config_path, k, v);

				if (pg_strcasecmp(pm, "range") == 0)
					partition_method = PART_RANGE;
				else if (pg_strcasecmp(pm, "hash") == 0)
					partition_method = PART_HASH;
				else if (pg_strcasecmp(pm, "none") == 0)
					partition_method = PART_NONE;
				else
					pg_fatal("%s:%d: invalid partition_method \"%s\" (expected \"range\" or \"hash\")",
							 config_path, v->line_number, pm);
			}
		}
		else if (strcmp(k, "init_steps") == 0)
			parseConfigInitSteps(config_path, config_dir, v, is_init_mode, NULL);
		else if (strcmp(k, "no_vacuum") == 0)
			is_no_vacuum = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "vacuum_all") == 0)
			do_vacuum_accounts = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "quiet") == 0)
			use_quiet = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "debug") == 0)
		{
			if (expectJsonBool(config_path, k, v))
				pg_logging_increase_verbosity();
		}
		else if (strcmp(k, "query_mode") == 0 || strcmp(k, "protocol") == 0)
		{
			const char *qm = expectJsonString(config_path, k, v);

			for (querymode = 0; querymode < NUM_QUERYMODE; querymode++)
				if (strcmp(qm, QUERYMODE[querymode]) == 0)
					break;
			if (querymode >= NUM_QUERYMODE)
				pg_fatal("%s:%d: invalid query_mode \"%s\" (expected \"simple\", \"extended\", or \"prepared\")",
						 config_path, v->line_number, qm);
		}
		else if (strcmp(k, "clients") == 0 || strcmp(k, "client") == 0)
			nclients = expectJsonInt(config_path, k, v, 1, INT_MAX);
		else if (strcmp(k, "threads") == 0 || strcmp(k, "jobs") == 0)
		{
			nthreads = expectJsonInt(config_path, k, v, 1, INT_MAX);
			copy_nthreads = nthreads;
		}
		else if (strcmp(k, "connect") == 0)
			is_connect = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "transactions") == 0 ||
				 strcmp(k, "transactions_per_client") == 0)
		{
			int			t = expectJsonInt(config_path, k, v, 0, INT_MAX);

			nxacts = t;
			if (t > 0 &&
				findObjectField(obj, "duration_sec") == NULL &&
				findObjectField(obj, "time") == NULL &&
				findObjectField(obj, "duration") == NULL)
				duration = 0;
		}
		else if (strcmp(k, "duration_sec") == 0 ||
				 strcmp(k, "time") == 0 ||
				 strcmp(k, "duration") == 0)
		{
			int			d = expectJsonInt(config_path, k, v, 0, INT_MAX);

			duration = d;
			if (d > 0 &&
				findObjectField(obj, "transactions") == NULL &&
				findObjectField(obj, "transactions_per_client") == NULL)
				nxacts = 0;
		}
		else if (strcmp(k, "rate") == 0)
		{
			double		r = expectJsonDouble(config_path, k, v);

			if (r < 0.0)
				pg_fatal("%s:%d: invalid rate limit: %g",
						 config_path, v->line_number, r);
			throttle_delay = (r > 0.0) ? (1000000.0 / r) : 0.0;
		}
		else if (strcmp(k, "latency_limit_ms") == 0 ||
				 strcmp(k, "latency_limit") == 0)
		{
			double		lms = expectJsonDouble(config_path, k, v);

			if (lms < 0.0)
				pg_fatal("%s:%d: invalid latency limit: %g",
						 config_path, v->line_number, lms);
			latency_limit = (lms > 0.0) ? (int64) (lms * 1000.0) : 0;
		}
		else if (strcmp(k, "progress_sec") == 0 || strcmp(k, "progress") == 0)
			progress = expectJsonInt(config_path, k, v, 0, INT_MAX);
		else if (strcmp(k, "progress_timestamp") == 0)
			progress_timestamp = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "log") == 0)
			use_log = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "log_prefix") == 0)
		{
			if (v->type != CJSON_NULL)
			{
				pg_free(logfile_prefix);
				logfile_prefix = pg_strdup(expectJsonString(config_path, k, v));
			}
		}
		else if (strcmp(k, "sampling_rate") == 0)
		{
			double		sr = expectJsonDouble(config_path, k, v);

			if (sr < 0.0 || sr > 1.0)
				pg_fatal("%s:%d: invalid sampling_rate: %g",
						 config_path, v->line_number, sr);
			sample_rate = sr;
		}
		else if (strcmp(k, "aggregate_interval") == 0)
			agg_interval = expectJsonInt(config_path, k, v, 0, INT_MAX);
		else if (strcmp(k, "report_per_command") == 0)
			report_per_command = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "failures_detailed") == 0)
			failures_detailed = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "max_tries") == 0)
			max_tries = (uint32) expectJsonInt(config_path, k, v, 0, INT_MAX);
		else if (strcmp(k, "verbose_errors") == 0)
			verbose_errors = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "exit_on_abort") == 0)
			exit_on_abort = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "continue_on_error") == 0)
			continue_on_error = expectJsonBool(config_path, k, v);
		else if (strcmp(k, "seed") == 0 || strcmp(k, "random_seed") == 0)
		{
			if (v->type == CJSON_STRING || v->type == CJSON_NUMBER)
			{
				if (!set_random_seed(v->scalar_str))
					pg_fatal("%s:%d: error while setting random seed from \"%s\"",
							 config_path, v->line_number, v->scalar_str);
				random_seed_set = true;
			}
			else if (v->type != CJSON_NULL)
				pg_fatal("%s:%d: configuration key \"%s\" expects an integer or \"time\"/\"rand\"",
						 config_path, v->line_number, k);
		}
		else if (strcmp(k, "max_concurrent_sessions") == 0)
			max_concurrent_sessions = expectJsonInt(config_path, k, v, 0, INT_MAX);
		else if (strcmp(k, "ontime_threshold_ms") == 0 ||
				 strcmp(k, "ontime_threshold") == 0)
		{
			double		th = expectJsonDouble(config_path, k, v);

			if (th < 0.0)
				pg_fatal("%s:%d: invalid ontime_threshold_ms: %g",
						 config_path, v->line_number, th);
			ontime_threshold_ms = th;
		}
		else if (strcmp(k, "replay_speed") == 0 || strcmp(k, "tcr") == 0)
		{
			double		spd = expectJsonDouble(config_path, k, v);

			if (spd <= 0.0)
				pg_fatal("%s:%d: invalid replay_speed: %g",
						 config_path, v->line_number, spd);
			replay_speed = spd;
		}
		else if (strcmp(k, "replay") == 0)
		{
			if (v->type != CJSON_NULL && !is_init_mode)
			{
				const char *rpath = expectJsonString(config_path, k, v);
				char	   *resolved = resolveConfigPath(config_dir, rpath);
				char	   *ds_spec;

				pg_free(replay_trace_file);
				replay_trace_file = resolved;
				ds_spec = psprintf("replay=csv:%s", replay_trace_file);
				parseDatasetSpec(ds_spec);
				pg_free(ds_spec);
			}
		}
		else if (strcmp(k, "json_report") == 0)
		{
			if (v->type != CJSON_NULL)
			{
				pg_free(json_report_file);
				json_report_file = pg_strdup(expectJsonString(config_path, k, v));
			}
		}
		else if (strcmp(k, "vector_base") == 0 ||
				 strcmp(k, "ground_truth_table") == 0)
		{
			if (v->type != CJSON_NULL)
			{
				const char *gspec = expectJsonString(config_path, k, v);
				const char *colon1 = strchr(gspec, ':');
				char	   *full_spec = NULL;

				if (colon1 != NULL && colon1 > gspec && colon1[1] != '\0')
				{
					const char *colon2 = strchr(colon1 + 1, ':');

					if (colon2 != NULL && colon2 > colon1 + 1 && colon2[1] != '\0')
					{
						char	   *mid = pnstrdup(colon1 + 1, colon2 - (colon1 + 1));
						int64		parsed_off;

						if (strtoint64(mid, true, &parsed_off))
						{
							char	   *prefix = pnstrdup(gspec, colon2 - gspec);
							char	   *resolved = resolveConfigPath(config_dir, colon2 + 1);

							full_spec = psprintf("%s:%s", prefix, resolved);
							pg_free(prefix);
							pg_free(resolved);
						}
						pg_free(mid);
					}
					if (full_spec == NULL)
					{
						char	   *prefix = pnstrdup(gspec, colon1 - gspec);
						char	   *resolved = resolveConfigPath(config_dir, colon1 + 1);

						full_spec = psprintf("%s:%s", prefix, resolved);
						pg_free(prefix);
						pg_free(resolved);
					}
				}
				if (full_spec == NULL)
					full_spec = resolveConfigPath(config_dir, gspec);
				if (strcmp(k, "vector_base") == 0)
					parseVectorBaseSpec(full_spec);
				else
					parseGroundTruthTableSpec(full_spec);
				pg_free(full_spec);
			}
		}
		else if (strcmp(k, "variables") == 0 || strcmp(k, "define") == 0)
		{
			if (v->type != CJSON_OBJECT)
				pg_fatal("%s:%d: \"%s\" must be a JSON object",
						 config_path, v->line_number, k);
			for (int vf = 0; vf < v->nfields; vf++)
			{
				applyJsonVariable(config_path, client0_vars,
								  v->fields[vf].key, v->fields[vf].val);
				applyJsonVariable(config_path, &config_variables,
								  v->fields[vf].key, v->fields[vf].val);
			}
		}
		else if (strcmp(k, "reservoirs") == 0)
		{
			if (v->type == CJSON_OBJECT)
			{
				for (int rf = 0; rf < v->nfields; rf++)
				{
					int64		cap = expectJsonInt64(config_path, "reservoirs",
													  v->fields[rf].val,
													  1, 100LL * 1000 * 1000);

					registerOrUpdateReservoir(v->fields[rf].key, (uint64) cap);
				}
			}
			else if (v->type == CJSON_ARRAY)
			{
				for (int ri = 0; ri < v->nelems; ri++)
				{
					const ConfigJsonNode *rel = v->elems[ri];

					if (rel->type == CJSON_STRING)
						parseReservoirSpec(rel->scalar_str);
					else if (rel->type == CJSON_OBJECT)
					{
						ConfigJsonField *fn = findObjectField(rel, "name");
						ConfigJsonField *fc = findObjectField(rel, "capacity");
						int64		cap;

						if (fn == NULL || fc == NULL)
							pg_fatal("%s:%d: reservoir object must specify \"name\" and \"capacity\"",
									 config_path, rel->line_number);
						cap = expectJsonInt64(config_path, "reservoirs[].capacity",
											  fc->val, 1, 100LL * 1000 * 1000);
						registerOrUpdateReservoir(
							expectJsonString(config_path, "reservoirs[].name", fn->val),
							(uint64) cap);
					}
					else
						pg_fatal("%s:%d: reservoir array element must be an object or string",
								 config_path, rel->line_number);
				}
			}
			else
				pg_fatal("%s:%d: \"reservoirs\" must be an object or array",
						 config_path, v->line_number);
		}
		else if (strcmp(k, "datasets") == 0 || strcmp(k, "vector_datasets") == 0)
		{
			parseConfigDatasets(config_path, config_dir, v);
		}
		else if (strcmp(k, "copy_datasets") == 0)
		{
			if (v->type == CJSON_OBJECT)
			{
				for (int cf = 0; cf < v->nfields; cf++)
				{
					const char *tbl = expectJsonString(config_path, "copy_datasets",
													   v->fields[cf].val);
					char	   *spec = psprintf("%s:%s", v->fields[cf].key, tbl);

					registerCopyDatasetSpec(spec);
					pg_free(spec);
				}
			}
			else if (v->type == CJSON_ARRAY)
			{
				for (int ci = 0; ci < v->nelems; ci++)
				{
					const ConfigJsonNode *cel = v->elems[ci];

					if (cel->type == CJSON_STRING)
						registerCopyDatasetSpec(cel->scalar_str);
					else if (cel->type == CJSON_OBJECT)
					{
						ConfigJsonField *fds = findObjectField(cel, "dataset");
						ConfigJsonField *ftb = findObjectField(cel, "table");
						char	   *spec;

						if (fds == NULL || ftb == NULL)
							pg_fatal("%s:%d: copy_datasets object must specify \"dataset\" and \"table\"",
									 config_path, cel->line_number);
						spec = psprintf("%s:%s",
										expectJsonString(config_path, "copy_datasets[].dataset", fds->val),
										expectJsonString(config_path, "copy_datasets[].table", ftb->val));
						registerCopyDatasetSpec(spec);
						pg_free(spec);
					}
					else
						pg_fatal("%s:%d: copy_datasets element must be an object or string",
								 config_path, cel->line_number);
				}
			}
			else
				pg_fatal("%s:%d: \"copy_datasets\" must be an object or array",
						 config_path, v->line_number);
		}
		else if (strcmp(k, "builtin") == 0 || strcmp(k, "builtin_weight") == 0)
		{
			if (!is_init_mode && findObjectField(obj, "groups") == NULL &&
				findObjectField(obj, "scripts") == NULL)
			{
				if (v->type == CJSON_NUMBER)
				{
					int			bw = expectJsonInt(config_path, k, v, 0, INT_MAX);

					if (num_scripts > 0)
						sql_script[num_scripts - 1].weight = bw;
					else
					{
						process_builtin(findBuiltin("tpcb-like"), bw);
						internal_script_used = true;
						config_scripts_loaded = true;
					}
				}
				else if (v->type == CJSON_STRING)
				{
					char	   *raw_s = NULL;
					int			bw = parseScriptWeight(v->scalar_str, &raw_s);

					clearConfigScriptsAndGroups();
					process_builtin(findBuiltin(raw_s), bw);
					pg_free(raw_s);
					internal_script_used = true;
					config_scripts_loaded = true;
				}
				else if (v->type == CJSON_OBJECT)
				{
					clearConfigScriptsAndGroups();
					for (int bf = 0; bf < v->nfields; bf++)
					{
						int			bw = expectJsonInt(config_path, k, v->fields[bf].val, 0, INT_MAX);

						process_builtin(findBuiltin(v->fields[bf].key), bw);
						internal_script_used = true;
					}
					config_scripts_loaded = (num_scripts > 0);
				}
				else
					pg_fatal("%s:%d: \"%s\" must be a string, integer, or object",
							 config_path, v->line_number, k);
			}
		}
		else if (strcmp(k, "file") == 0)
		{
			if (!is_init_mode && findObjectField(obj, "groups") == NULL &&
				findObjectField(obj, "scripts") == NULL)
			{
				clearConfigScriptsAndGroups();
				if (v->type == CJSON_ARRAY)
				{
					for (int s = 0; s < v->nelems; s++)
						(void) parseConfigScriptEntry(config_path, config_dir,
													  v->elems[s], NULL);
				}
				else
					(void) parseConfigScriptEntry(config_path, config_dir, v, NULL);
				config_scripts_loaded = (num_scripts > 0);
			}
		}
		else if (strcmp(k, "select_only") == 0)
		{
			if (expectJsonBool(config_path, k, v) && !is_init_mode &&
				findObjectField(obj, "groups") == NULL &&
				findObjectField(obj, "scripts") == NULL)
			{
				clearConfigScriptsAndGroups();
				process_builtin(findBuiltin("select-only"), 1);
				internal_script_used = true;
				config_scripts_loaded = true;
			}
		}
		else if (strcmp(k, "skip_some_updates") == 0)
		{
			if (expectJsonBool(config_path, k, v) && !is_init_mode &&
				findObjectField(obj, "groups") == NULL &&
				findObjectField(obj, "scripts") == NULL)
			{
				clearConfigScriptsAndGroups();
				process_builtin(findBuiltin("simple-update"), 1);
				internal_script_used = true;
				config_scripts_loaded = true;
			}
		}
		else if (strcmp(k, "scripts") == 0)
		{
			if (v->type != CJSON_ARRAY)
				pg_fatal("%s:%d: \"scripts\" must be an array",
						 config_path, v->line_number);
			if (!is_init_mode && findObjectField(obj, "groups") == NULL)
			{
				clearConfigScriptsAndGroups();
				for (int s = 0; s < v->nelems; s++)
					(void) parseConfigScriptEntry(config_path, config_dir,
												  v->elems[s], NULL);
				config_scripts_loaded = (num_scripts > 0);
			}
		}
		else if (strcmp(k, "groups") == 0)
		{
			if (!is_init_mode)
				parseConfigGroups(config_path, config_dir, v);
		}
		else if (strcmp(k, "sla") == 0)
		{
			if (v->type != CJSON_ARRAY)
				pg_fatal("%s:%d: \"sla\" must be an array of SLA specifications",
						 config_path, v->line_number);
			if (!is_init_mode)
			{
				num_slas = 0;
				for (int s = 0; s < v->nelems; s++)
				{
					const ConfigJsonNode *sel = v->elems[s];

					if (sel->type == CJSON_STRING)
						parseSlaSpec(sel->scalar_str);
					else if (sel->type == CJSON_OBJECT)
						parseSlaVerificationObject(config_path, sel);
					else
						pg_fatal("%s:%d: each element of \"sla\" must be a string or object",
								 config_path, sel->line_number);
				}
			}
		}
		else
			pg_fatal("%s:%d: unrecognized configuration key \"%s\"",
					 config_path, fld->line_number, k);
	}
}

void
loadConfigFile(const char *config_path,
			   bool is_init_mode,
			   Variables *client0_vars,
			   bool *benchmarking_option_set,
			   bool *initialization_option_set,
			   bool *jobs_option_set)
{
	ConfigJsonNode *root = parseConfigJsonFile(config_path);
	char	   *config_dir = getConfigDir(config_path);
	ConfigJsonField *f_cfg = findObjectField(root, "configuration");

	dataset_loading_from_config = true;

	if (f_cfg != NULL && f_cfg->val->type == CJSON_OBJECT)
	{
		/*
		 * The input file is a --json-report file (or wraps settings under a
		 * top-level "configuration" object). Apply "configuration" first, and
		 * fall back to top-level "groups", "scripts", and "sla_verifications"
		 * if not already present inside "configuration".
		 */
		applyConfigObject(config_path, config_dir, f_cfg->val,
						  is_init_mode, client0_vars);

		if (!is_init_mode)
		{
			ConfigJsonField *f_groups = findObjectField(root, "groups");
			ConfigJsonField *f_scripts = findObjectField(root, "scripts");
			ConfigJsonField *f_slas = findObjectField(root, "sla_verifications");

			if (num_groups == 0 && f_groups != NULL &&
				f_groups->val->type == CJSON_ARRAY && f_groups->val->nelems > 0)
				parseConfigGroups(config_path, config_dir, f_groups->val);

			if (num_scripts == 0 && num_groups == 0 && f_scripts != NULL &&
				f_scripts->val->type == CJSON_ARRAY && f_scripts->val->nelems > 0)
			{
				for (int s = 0; s < f_scripts->val->nelems; s++)
					(void) parseConfigScriptEntry(config_path, config_dir,
												  f_scripts->val->elems[s], NULL);
				config_scripts_loaded = (num_scripts > 0);
			}

			if (num_slas == 0 && f_slas != NULL &&
				f_slas->val->type == CJSON_ARRAY && f_slas->val->nelems > 0)
			{
				for (int s = 0; s < f_slas->val->nelems; s++)
				{
					if (f_slas->val->elems[s]->type == CJSON_OBJECT)
						parseSlaVerificationObject(config_path, f_slas->val->elems[s]);
				}
			}
		}
	}
	else
	{
		applyConfigObject(config_path, config_dir, root,
						  is_init_mode, client0_vars);
	}

	dataset_loading_from_config = false;

	pg_free(config_dir);
	freeJsonNode(root);
}

/*-------------------------------------------------------------------------
 * Multi-stage initialization runner (--config=<file> -i)
 *-------------------------------------------------------------------------
 */
void
runConfigInitSteps(Variables *cli_vars)
{
	PGconn	   *con;
	int			global_scale = scale;

	resolveHostAddrOnce();
	con = doConnect();
	if (con == NULL)
		pg_fatal("could not create connection for initialization");
	printVersion(con);
	PQfinish(con);

	for (int s = 0; s < num_config_init_steps; s++)
	{
		ConfigInitStep *step = &config_init_steps[s];
		int			step_scale = (step->scale > 0 && !scale_given) ? step->scale : global_scale;
		int			step_clients = step->clients;
		int			step_threads = (step->threads > step_clients) ? step_clients : step->threads;
		int			step_nxacts = (step->transactions > 0) ? step->transactions : 1;
		CState	   *step_state;
		TState	   *step_threads_arr;
		int			nclients_dealt = 0;
		bool		step_failed = false;

		if (step->builtin_init != NULL)
		{
			int			saved_scale = scale;

			scale = step_scale;
			pg_log_info("running initialization step %d/%d: %s (builtin init-steps \"%s\", scale=%d)",
						s + 1, num_config_init_steps, step->name,
						step->builtin_init, step_scale);
			runInitSteps(step->builtin_init);
			scale = saved_scale;
			continue;
		}

		pg_log_info("running initialization step %d/%d: %s (%s, clients=%d, threads=%d, scale=%d)",
					s + 1, num_config_init_steps, step->name, step->file,
					step_clients, step_threads, step_scale);

		/* Reset script, group, barrier, and engine state for this step */
		clearConfigScriptsAndGroups();
		resetEngineRunState();
		resetPhaseBarriers();

		process_file(step->file, 1);
		for (int i = 0; i < num_scripts; i++)
		{
			Command   **commands = sql_script[i].commands;

			for (int j = 0; commands[j] != NULL; j++)
				if (commands[j]->type == SQL_COMMAND)
					postprocess_sql_command(commands[j]);
			total_weight += sql_script[i].weight;
		}

		nclients = step_clients;
		nthreads = step_threads;
		nxacts = step_nxacts;
		duration = 0;
		throttle_delay = 0.0;
		latency_limit = 0;
		is_no_vacuum = true;

		step_state = pg_malloc0_array(CState, step_clients);
		for (int i = 0; i < step_clients; i++)
		{
			step_state[i].id = i;
			step_state[i].group_id = -1;
			step_state[i].stream_id = i;
			step_state[i].seq_counter = 0;

			/* 1. Copy step-level variables from config */
			if (step->variables.root != NULL)
			{
				if (!copyVariables(&step_state[i].variables, &step->variables))
					exit(1);
			}
			/* 2. Overlay any top-level/CLI -D variables */
			if (cli_vars != NULL && cli_vars->root != NULL)
			{
				VariableScope *sc = cli_vars->root;

				for (int v = 0; v < sc->nvars; v++)
				{
					Variable   *var = &sc->vars[v];

					if (var->value.type != PGBT_NO_VALUE)
					{
						if (!putVariableValue(&step_state[i].variables, "option",
											  var->name, &var->value))
							exit(1);
					}
					else if (var->svalue != NULL)
					{
						if (!putVariable(&step_state[i].variables, "option",
										 var->name, var->svalue))
							exit(1);
					}
				}
			}

			/* 3. Populate standard pgbench automatic variables if not overridden */
			if (lookupVariable(&step_state[i].variables, "scale") == NULL)
			{
				if (!putVariableInt(&step_state[i].variables, "startup", "scale", step_scale))
					exit(1);
			}
			if (lookupVariable(&step_state[i].variables, "client_id") == NULL)
			{
				if (!putVariableInt(&step_state[i].variables, "startup", "client_id", i))
					exit(1);
			}
			if (lookupVariable(&step_state[i].variables, "stream_id") == NULL)
			{
				if (!putVariableInt(&step_state[i].variables, "startup", "stream_id", i))
					exit(1);
			}
			if (lookupVariable(&step_state[i].variables, "default_seed") == NULL)
			{
				uint64		seed = pg_prng_uint64(&base_random_sequence);

				if (!putVariableInt(&step_state[i].variables, "startup", "default_seed", (int64) seed))
					exit(1);
			}
			if (lookupVariable(&step_state[i].variables, "random_seed") == NULL)
			{
				if (!putVariableInt(&step_state[i].variables, "startup", "random_seed", random_seed))
					exit(1);
			}

			step_state[i].cstack = conditional_stack_create();
			initRandomState(&step_state[i].cs_func_rs);
		}

		step_threads_arr = pg_malloc0_array(TState, step_threads);
		for (int i = 0; i < step_threads; i++)
		{
			TState	   *thread = &step_threads_arr[i];

			thread->tid = i;
			thread->group_id = -1;
			thread->state = &step_state[nclients_dealt];
			thread->nstate =
				(step_clients - nclients_dealt + step_threads - i - 1) / (step_threads - i);
			initRandomState(&thread->ts_choose_rs);
			initRandomState(&thread->ts_throttle_rs);
			initRandomState(&thread->ts_sample_rs);
			thread->logfile = NULL;
			thread->latency_late = 0;
			initStats(&thread->stats, 0);

			nclients_dealt += thread->nstate;
		}

		errno = THREAD_BARRIER_INIT(&barrier, step_threads);
		if (errno != 0)
			pg_fatal("could not initialize barrier for init step \"%s\": %m", step->name);

		for (int i = 1; i < step_threads; i++)
		{
			TState	   *thread = &step_threads_arr[i];

			thread->create_time = pg_time_now();
			errno = THREAD_CREATE(&thread->thread, threadRun, thread);
			if (errno != 0)
				pg_fatal("could not create thread for init step \"%s\": %m", step->name);
		}

		step_threads_arr[0].create_time = pg_time_now();
		(void) threadRun(&step_threads_arr[0]);

		for (int i = 0; i < step_threads; i++)
		{
			TState	   *thread = &step_threads_arr[i];

			if (i > 0)
				THREAD_JOIN(thread->thread);
			for (int j = 0; j < thread->nstate; j++)
			{
				if (thread->state[j].state != CSTATE_FINISHED)
					step_failed = true;
			}
		}

		disconnect_all(step_state, step_clients);
		THREAD_BARRIER_DESTROY(&barrier);

		for (int i = 0; i < step_clients; i++)
		{
			if (step_state[i].cstack != NULL)
				conditional_stack_destroy(step_state[i].cstack);
			destroyVariables(&step_state[i].variables);
		}
		pg_free(step_state);
		pg_free(step_threads_arr);

		if (step_failed)
			pg_fatal("initialization step \"%s\" (%s) failed", step->name, step->file);
	}

	if (num_copy_dataset_specs > 0)
	{
		int			saved_nthreads = nthreads;

		con = doConnect();
		if (con == NULL)
			pg_fatal("could not create connection for dataset copy");
		nthreads = copy_nthreads;
		runRegisteredCopyDatasets(con);
		nthreads = saved_nthreads;
		PQfinish(con);
	}
}

/*-------------------------------------------------------------------------
 * Configuration JSON serializer (--dump-config and --json-report)
 *-------------------------------------------------------------------------
 */
static void
fprintIndent(FILE *fp, int indent)
{
	for (int i = 0; i < indent; i++)
		fputc(' ', fp);
}

static void
fprintConfigJsonStr(FILE *fp, const char *str)
{
	if (str == NULL)
	{
		fputs("null", fp);
		return;
	}
	fputc('"', fp);
	for (const unsigned char *p = (const unsigned char *) str; *p != '\0'; p++)
	{
		switch (*p)
		{
			case '"':
				fputs("\\\"", fp);
				break;
			case '\\':
				fputs("\\\\", fp);
				break;
			case '\b':
				fputs("\\b", fp);
				break;
			case '\f':
				fputs("\\f", fp);
				break;
			case '\n':
				fputs("\\n", fp);
				break;
			case '\r':
				fputs("\\r", fp);
				break;
			case '\t':
				fputs("\\t", fp);
				break;
			default:
				if (*p < 0x20)
					fprintf(fp, "\\u%04x", (unsigned int) *p);
				else
					fputc(*p, fp);
				break;
		}
	}
	fputc('"', fp);
}

static void
writeVariablesJsonObject(FILE *fp, const Variables *vars, int indent)
{
	const VariableScope *sc = (vars != NULL) ? vars->root : NULL;

	if (sc == NULL || sc->nvars == 0)
	{
		fputs("{}", fp);
		return;
	}

	fputs("{\n", fp);
	for (int i = 0; i < sc->nvars; i++)
	{
		const Variable *var = &sc->vars[i];

		fprintIndent(fp, indent + 2);
		fprintConfigJsonStr(fp, var->name);
		fputs(": ", fp);

		if (var->value.type == PGBT_INT)
			fprintf(fp, INT64_FORMAT, var->value.u.ival);
		else if (var->value.type == PGBT_DOUBLE)
			fprintf(fp, "%g", var->value.u.dval);
		else if (var->value.type == PGBT_BOOLEAN)
			fputs(var->value.u.bval ? "true" : "false", fp);
		else if (var->value.type == PGBT_NULL)
			fputs("null", fp);
		else if (var->value.type == PGBT_TEXT)
			fprintConfigJsonStr(fp, var->value.u.sval);
		else if (var->value.type == PGBT_ARRAY)
		{
			fputc('[', fp);
			for (int a = 0; a < var->value.u.aval.nelems; a++)
			{
				const PgBenchValue *el = &var->value.u.aval.elems[a];

				if (a > 0)
					fputs(", ", fp);
				if (el->type == PGBT_INT)
					fprintf(fp, INT64_FORMAT, el->u.ival);
				else if (el->type == PGBT_DOUBLE)
					fprintf(fp, "%g", el->u.dval);
				else if (el->type == PGBT_BOOLEAN)
					fputs(el->u.bval ? "true" : "false", fp);
				else if (el->type == PGBT_NULL)
					fputs("null", fp);
				else if (el->type == PGBT_TEXT)
					fprintConfigJsonStr(fp, el->u.sval);
				else
					fputs("null", fp);
			}
			fputc(']', fp);
		}
		else if (var->svalue != NULL)
		{
			int64		ival;
			double		dval;

			if (is_an_int(var->svalue) && strtoint64(var->svalue, true, &ival))
				fprintf(fp, INT64_FORMAT, ival);
			else if (strtodouble(var->svalue, true, &dval))
				fprintf(fp, "%g", dval);
			else
				fprintConfigJsonStr(fp, var->svalue);
		}
		else
			fputs("null", fp);

		fprintf(fp, "%s\n", (i + 1 < sc->nvars) ? "," : "");
	}
	fprintIndent(fp, indent);
	fputc('}', fp);
}

static const char *
datasetFormatToString(DatasetFormat fmt)
{
	switch (fmt)
	{
		case DATASET_FVECS:
			return "fvecs";
		case DATASET_BVECS:
			return "bvecs";
		case DATASET_IVECS:
			return "ivecs";
		case DATASET_CSV:
			return "csv";
		case DATASET_TSV:
			return "tsv";
		default:
			return "csv";
	}
}

void
writeConfigJsonObject(FILE *fp, int base_indent, bool include_init_steps)
{
	int			ind = base_indent + 2;
	int			nres = getNumReservoirs();
	int			nds = getNumDatasets();

	fputs("{\n", fp);

	if (config_suite_name != NULL)
	{
		fprintIndent(fp, ind);
		fputs("\"suite\": ", fp);
		fprintConfigJsonStr(fp, config_suite_name);
		fputs(",\n", fp);
	}
	if (config_description != NULL)
	{
		fprintIndent(fp, ind);
		fputs("\"description\": ", fp);
		fprintConfigJsonStr(fp, config_description);
		fputs(",\n", fp);
	}
	if (pghost != NULL)
	{
		fprintIndent(fp, ind);
		fputs("\"host\": ", fp);
		fprintConfigJsonStr(fp, pghost);
		fputs(",\n", fp);
	}
	if (pgport != NULL)
	{
		fprintIndent(fp, ind);
		fputs("\"port\": ", fp);
		fprintConfigJsonStr(fp, pgport);
		fputs(",\n", fp);
	}
	if (username != NULL)
	{
		fprintIndent(fp, ind);
		fputs("\"username\": ", fp);
		fprintConfigJsonStr(fp, username);
		fputs(",\n", fp);
	}
	if (dbName != NULL)
	{
		fprintIndent(fp, ind);
		fputs("\"dbname\": ", fp);
		fprintConfigJsonStr(fp, dbName);
		fputs(",\n", fp);
	}

	fprintIndent(fp, ind);
	fprintf(fp, "\"scale\": %d,\n", scale);
	fprintIndent(fp, ind);
	fprintf(fp, "\"clients\": %d,\n", nclients);
	fprintIndent(fp, ind);
	fprintf(fp, "\"threads\": %d,\n", nthreads);
	fprintIndent(fp, ind);
	fprintf(fp, "\"duration_sec\": %d,\n", duration);
	fprintIndent(fp, ind);
	fprintf(fp, "\"transactions_per_client\": %d,\n", nxacts);
	fprintIndent(fp, ind);
	fprintf(fp, "\"query_mode\": \"%s\",\n", QUERYMODE[querymode]);
	fprintIndent(fp, ind);
	fprintf(fp, "\"no_vacuum\": %s", is_no_vacuum ? "true" : "false");

	if (do_vacuum_accounts)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"vacuum_all\": true", fp);
	}
	if (use_quiet)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"quiet\": true", fp);
	}
	if (is_connect)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"connect\": true", fp);
	}
	if (fillfactor != 100)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"fillfactor\": %d", fillfactor);
	}
	if (unlogged_tables)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"unlogged_tables\": true", fp);
	}
	if (foreign_keys)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"foreign_keys\": true", fp);
	}
	if (tablespace != NULL)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"tablespace\": ", fp);
		fprintConfigJsonStr(fp, tablespace);
	}
	if (index_tablespace != NULL)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"index_tablespace\": ", fp);
		fprintConfigJsonStr(fp, index_tablespace);
	}
	if (partitions > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"partitions\": %d,\n", partitions);
		fprintIndent(fp, ind);
		fprintf(fp, "\"partition_method\": \"%s\"",
				partition_method == PART_HASH ? "hash" : "range");
	}
	if (throttle_delay > 0.0 && nthreads > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"rate\": %.6g", (1000000.0 * (double) nthreads) / throttle_delay);
	}
	if (latency_limit > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"latency_limit_ms\": %.3f", (double) latency_limit / 1000.0);
	}
	if (progress > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"progress_sec\": %d", progress);
	}
	if (progress_timestamp)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"progress_timestamp\": true", fp);
	}
	if (use_log)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"log\": true", fp);
	}
	if (logfile_prefix != NULL)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"log_prefix\": ", fp);
		fprintConfigJsonStr(fp, logfile_prefix);
	}
	if (sample_rate > 0.0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"sampling_rate\": %g", sample_rate);
	}
	if (agg_interval > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"aggregate_interval\": %d", agg_interval);
	}
	if (report_per_command)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"report_per_command\": true", fp);
	}
	if (failures_detailed)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"failures_detailed\": true", fp);
	}
	if (max_tries != 1)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"max_tries\": %u", max_tries);
	}
	if (verbose_errors)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"verbose_errors\": true", fp);
	}
	if (exit_on_abort)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"exit_on_abort\": true", fp);
	}
	if (continue_on_error)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"continue_on_error\": true", fp);
	}
	if (random_seed_set && random_seed >= 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"seed\": " INT64_FORMAT, random_seed);
	}
	if (max_concurrent_sessions > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"max_concurrent_sessions\": %d", max_concurrent_sessions);
	}
	if (ontime_threshold_ms != 50.0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"ontime_threshold_ms\": %g", ontime_threshold_ms);
	}
	if (replay_speed != 1.0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fprintf(fp, "\"replay_speed\": %g", replay_speed);
	}
	if (replay_trace_file != NULL)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"replay\": ", fp);
		fprintConfigJsonStr(fp, replay_trace_file);
	}
	if (include_init_steps && json_report_file != NULL)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"json_report\": ", fp);
		fprintConfigJsonStr(fp, json_report_file);
	}

	/* init_steps (when dumping config or when present) */
	if (include_init_steps && num_config_init_steps > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"init_steps\": [\n", fp);
		for (int s = 0; s < num_config_init_steps; s++)
		{
			const ConfigInitStep *step = &config_init_steps[s];

			fprintIndent(fp, ind + 2);
			fputs("{\n", fp);
			fprintIndent(fp, ind + 4);
			fputs("\"name\": ", fp);
			fprintConfigJsonStr(fp, step->name);
			fputs(",\n", fp);
			if (step->builtin_init != NULL)
			{
				fprintIndent(fp, ind + 4);
				fputs("\"builtin_init\": ", fp);
				fprintConfigJsonStr(fp, step->builtin_init);
				if (step->scale > 0)
				{
					fputs(",\n", fp);
					fprintIndent(fp, ind + 4);
					fprintf(fp, "\"scale\": %d", step->scale);
				}
				fputs("\n", fp);
			}
			else
			{
				fprintIndent(fp, ind + 4);
				fputs("\"file\": ", fp);
				fprintConfigJsonStr(fp, step->file);
				fputs(",\n", fp);
				fprintIndent(fp, ind + 4);
				fprintf(fp, "\"clients\": %d,\n", step->clients);
				fprintIndent(fp, ind + 4);
				fprintf(fp, "\"threads\": %d,\n", step->threads);
				fprintIndent(fp, ind + 4);
				fprintf(fp, "\"transactions\": %d", step->transactions);
				if (step->scale > 0)
				{
					fputs(",\n", fp);
					fprintIndent(fp, ind + 4);
					fprintf(fp, "\"scale\": %d", step->scale);
				}
				if (step->variables.root != NULL && step->variables.root->nvars > 0)
				{
					fputs(",\n", fp);
					fprintIndent(fp, ind + 4);
					fputs("\"variables\": ", fp);
					writeVariablesJsonObject(fp, &step->variables, ind + 4);
				}
				fputc('\n', fp);
			}
			fprintIndent(fp, ind + 2);
			fprintf(fp, "}%s\n", (s + 1 < num_config_init_steps) ? "," : "");
		}
		fprintIndent(fp, ind);
		fputc(']', fp);
	}
	else if (include_init_steps && initialize_steps != NULL)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"init_steps\": ", fp);
		fprintConfigJsonStr(fp, initialize_steps);
	}

	/* variables */
	if (config_variables.root != NULL && config_variables.root->nvars > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"variables\": ", fp);
		writeVariablesJsonObject(fp, &config_variables, ind);
	}

	/* reservoirs */
	if (nres > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"reservoirs\": {\n", fp);
		for (int r = 0; r < nres; r++)
		{
			const PgBenchReservoir *res = getReservoirByIndex(r);

			fprintIndent(fp, ind + 2);
			fprintConfigJsonStr(fp, res->name);
			fprintf(fp, ": " UINT64_FORMAT "%s\n",
					res->capacity, (r + 1 < nres) ? "," : "");
		}
		fprintIndent(fp, ind);
		fputc('}', fp);
	}

	/* datasets (excluding internal "replay" dataset if replay_trace_file is set) */
	if (nds > 0)
	{
		int			non_replay_ds = 0;

		for (int d = 0; d < nds; d++)
		{
			const PgBenchDataset *ds = getDatasetByIndex(d);

			if (replay_trace_file != NULL && strcmp(ds->name, "replay") == 0)
				continue;
			non_replay_ds++;
		}
		if (non_replay_ds > 0)
		{
			int			emitted = 0;

			fputs(",\n", fp);
			fprintIndent(fp, ind);
			fputs("\"datasets\": {\n", fp);
			for (int d = 0; d < nds; d++)
			{
				const PgBenchDataset *ds = getDatasetByIndex(d);

				if (replay_trace_file != NULL && strcmp(ds->name, "replay") == 0)
					continue;
				emitted++;
				fprintIndent(fp, ind + 2);
				fprintConfigJsonStr(fp, ds->name);
				fputs(": { \"file\": ", fp);
				fprintConfigJsonStr(fp, ds->filepath);
				fprintf(fp, ", \"format\": \"%s\"", datasetFormatToString(ds->format));
				if (ds->force_header)
					fputs(", \"header\": true", fp);
				if (ds->id_offset != 0)
					fprintf(fp, ", \"id_offset\": " INT64_FORMAT, ds->id_offset);
				fprintf(fp, " }%s\n", (emitted < non_replay_ds) ? "," : "");
			}
			fprintIndent(fp, ind);
			fputc('}', fp);
		}
	}

	/* copy_datasets */
	if (num_copy_dataset_specs > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"copy_datasets\": [\n", fp);
		for (int c = 0; c < num_copy_dataset_specs; c++)
		{
			const char *ds_name = NULL;
			const char *tbl_spec = NULL;

			if (getCopyDatasetSpecByIndex(c, &ds_name, &tbl_spec))
			{
				fprintIndent(fp, ind + 2);
				fputs("{ \"dataset\": ", fp);
				fprintConfigJsonStr(fp, ds_name);
				fputs(", \"table\": ", fp);
				fprintConfigJsonStr(fp, tbl_spec);
				fprintf(fp, " }%s\n", (c + 1 < num_copy_dataset_specs) ? "," : "");
			}
		}
		fprintIndent(fp, ind);
		fputc(']', fp);
	}

	/* groups or scripts */
	if (num_groups > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"groups\": [\n", fp);
		for (int g = 0; g < num_groups; g++)
		{
			const ClientGroup *grp = &client_groups[g];

			fprintIndent(fp, ind + 2);
			fputs("{\n", fp);
			fprintIndent(fp, ind + 4);
			fputs("\"name\": ", fp);
			fprintConfigJsonStr(fp, grp->name);
			fputs(",\n", fp);
			fprintIndent(fp, ind + 4);
			fprintf(fp, "\"clients\": %d,\n", grp->nclients);
			fprintIndent(fp, ind + 4);
			fprintf(fp, "\"threads\": %d,\n", grp->nthreads);
			fprintIndent(fp, ind + 4);
			fprintf(fp, "\"sequential\": %s,\n", grp->sequential ? "true" : "false");
			if (grp->rate > 0.0)
			{
				fprintIndent(fp, ind + 4);
				fprintf(fp, "\"rate\": %g,\n", grp->rate);
			}
			fprintIndent(fp, ind + 4);
			fputs("\"scripts\": [\n", fp);
			for (int s = 0; s < grp->num_scripts; s++)
			{
				int			sidx = grp->script_indices[s];

				fprintIndent(fp, ind + 6);
				fputs("{ ", fp);
				if (sql_script[sidx].builtin_name != NULL)
				{
					fputs("\"builtin\": ", fp);
					fprintConfigJsonStr(fp, sql_script[sidx].builtin_name);
				}
				else
				{
					fputs("\"file\": ", fp);
					fprintConfigJsonStr(fp, sql_script[sidx].desc);
				}
				fprintf(fp, ", \"weight\": %d }%s\n",
						sql_script[sidx].weight,
						(s + 1 < grp->num_scripts) ? "," : "");
			}
			fprintIndent(fp, ind + 4);
			fputs("]\n", fp);
			fprintIndent(fp, ind + 2);
			fprintf(fp, "}%s\n", (g + 1 < num_groups) ? "," : "");
		}
		fprintIndent(fp, ind);
		fputc(']', fp);
	}
	else if (num_scripts > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"scripts\": [\n", fp);
		for (int i = 0; i < num_scripts; i++)
		{
			fprintIndent(fp, ind + 2);
			fputs("{ ", fp);
			if (sql_script[i].builtin_name != NULL)
			{
				fputs("\"builtin\": ", fp);
				fprintConfigJsonStr(fp, sql_script[i].builtin_name);
			}
			else
			{
				fputs("\"file\": ", fp);
				fprintConfigJsonStr(fp, sql_script[i].desc);
			}
			fprintf(fp, ", \"weight\": %d }%s\n",
					sql_script[i].weight,
					(i + 1 < num_scripts) ? "," : "");
		}
		fprintIndent(fp, ind);
		fputc(']', fp);
	}

	/* sla */
	if (num_slas > 0)
	{
		fputs(",\n", fp);
		fprintIndent(fp, ind);
		fputs("\"sla\": [\n", fp);
		for (int k = 0; k < num_slas; k++)
		{
			fprintIndent(fp, ind + 2);
			fprintConfigJsonStr(fp, sla_rules[k].raw_spec ? sla_rules[k].raw_spec : "");
			fprintf(fp, "%s\n", (k + 1 < num_slas) ? "," : "");
		}
		fprintIndent(fp, ind);
		fputc(']', fp);
	}

	fputc('\n', fp);
	fprintIndent(fp, base_indent);
	fputc('}', fp);
}

void
dumpConfigFile(const char *dump_path)
{
	FILE	   *fp;

	if (strcmp(dump_path, "-") == 0)
		fp = stdout;
	else
	{
		fp = fopen(dump_path, "w");
		if (fp == NULL)
			pg_fatal("could not open configuration dump file \"%s\": %m", dump_path);
	}

	writeConfigJsonObject(fp, 0, true);
	fputc('\n', fp);

	if (fp != stdout)
		fclose(fp);
}
