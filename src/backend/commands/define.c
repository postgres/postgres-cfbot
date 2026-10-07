/*-------------------------------------------------------------------------
 *
 * define.c
 *	  Support routines for various kinds of object creation.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/define.c
 *
 * DESCRIPTION
 *	  Support routines for dealing with DefElem nodes.
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "utils/fmgrprotos.h"

/*
 * Extract a string value (otherwise uninterpreted) from a DefElem.
 */
char *
defGetString(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return psprintf("%d", intVal(def->arg));
		case T_Float:
			return castNode(Float, def->arg)->fval;
		case T_Boolean:
			return boolVal(def->arg) ? "true" : "false";
		case T_String:
			return strVal(def->arg);
		case T_TypeName:
			return TypeNameToString((TypeName *) def->arg);
		case T_List:
			return NameListToString((List *) def->arg);
		case T_A_Star:
			return pstrdup("*");
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));
	}
	return NULL;				/* keep compiler quiet */
}

/*
 * Extract a numeric value (actually double) from a DefElem.
 */
double
defGetNumeric(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (double) intVal(def->arg);
		case T_Float:
			return floatVal(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* keep compiler quiet */
}

/*
 * Extract a boolean value from a DefElem.
 */
bool
defGetBoolean(DefElem *def)
{
	/*
	 * If no parameter value given, assume "true" is meant.
	 */
	if (def->arg == NULL)
		return true;

	/*
	 * Allow 0, 1, "true", "false", "on", "off"
	 */
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			switch (intVal(def->arg))
			{
				case 0:
					return false;
				case 1:
					return true;
				default:
					/* otherwise, error out below */
					break;
			}
			break;
		default:
			{
				char	   *sval = defGetString(def);

				/*
				 * The set of strings accepted here should match up with the
				 * grammar's opt_boolean_or_string production.
				 */
				if (pg_strcasecmp(sval, "true") == 0)
					return true;
				if (pg_strcasecmp(sval, "false") == 0)
					return false;
				if (pg_strcasecmp(sval, "on") == 0)
					return true;
				if (pg_strcasecmp(sval, "off") == 0)
					return false;
			}
			break;
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("%s requires a Boolean value",
					def->defname)));
	return false;				/* keep compiler quiet */
}

/*
 * Extract an int32 value from a DefElem.
 */
int32
defGetInt32(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires an integer value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (int32) intVal(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires an integer value",
							def->defname)));
	}
	return 0;					/* keep compiler quiet */
}

/*
 * Extract an int64 value from a DefElem.
 */
int64
defGetInt64(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (int64) intVal(def->arg);
		case T_Float:

			/*
			 * Values too large for int4 will be represented as Float
			 * constants by the lexer.  Accept these if they are valid int8
			 * strings.
			 */
			return DatumGetInt64(DirectFunctionCall1(int8in,
													 CStringGetDatum(castNode(Float, def->arg)->fval)));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* keep compiler quiet */
}

/*
 * Extract an OID value from a DefElem.
 */
Oid
defGetObjectId(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return (Oid) intVal(def->arg);
		case T_Float:

			/*
			 * Values too large for int4 will be represented as Float
			 * constants by the lexer.  Accept these if they are valid OID
			 * strings.
			 */
			return DatumGetObjectId(DirectFunctionCall1(oidin,
														CStringGetDatum(castNode(Float, def->arg)->fval)));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires a numeric value",
							def->defname)));
	}
	return 0;					/* keep compiler quiet */
}

/*
 * Extract a possibly-qualified name (as a List of Strings) from a DefElem.
 */
List *
defGetQualifiedName(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_TypeName:
			return ((TypeName *) def->arg)->names;
		case T_List:
			return (List *) def->arg;
		case T_String:
			/* Allow quoted name for backwards compatibility */
			return list_make1(def->arg);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("argument of %s must be a name",
							def->defname)));
	}
	return NIL;					/* keep compiler quiet */
}

/*
 * Extract a TypeName from a DefElem.
 *
 * Note: we do not accept a List arg here, because the parser will only
 * return a bare List when the name looks like an operator name.
 */
TypeName *
defGetTypeName(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_TypeName:
			return (TypeName *) def->arg;
		case T_String:
			/* Allow quoted typename for backwards compatibility */
			return makeTypeNameFromNameList(list_make1(def->arg));
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("argument of %s must be a type name",
							def->defname)));
	}
	return NULL;				/* keep compiler quiet */
}

/*
 * Extract a type length indicator (either absolute bytes, or
 * -1 for "variable") from a DefElem.
 */
int
defGetTypeLength(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			return intVal(def->arg);
		case T_Float:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("%s requires an integer value",
							def->defname)));
			break;
		case T_String:
			if (pg_strcasecmp(strVal(def->arg), "variable") == 0)
				return -1;		/* variable length */
			break;
		case T_TypeName:
			/* cope if grammar chooses to believe "variable" is a typename */
			if (pg_strcasecmp(TypeNameToString((TypeName *) def->arg),
							  "variable") == 0)
				return -1;		/* variable length */
			break;
		case T_List:
			/* must be an operator name */
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid argument for %s: \"%s\"",
					def->defname, defGetString(def))));
	return 0;					/* keep compiler quiet */
}

/*
 * Extract a list of string values (otherwise uninterpreted) from a DefElem.
 */
List *
defGetStringList(DefElem *def)
{
	ListCell   *cell;

	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a parameter",
						def->defname)));
	if (!IsA(def->arg, List))
		elog(ERROR, "unrecognized node type: %d", (int) nodeTag(def->arg));

	foreach(cell, (List *) def->arg)
	{
		Node	   *str = (Node *) lfirst(cell);

		if (!IsA(str, String))
			elog(ERROR, "unexpected node type in name list: %d",
				 (int) nodeTag(str));
	}

	return (List *) def->arg;
}

/*
 * Raise an error about a conflicting DefElem.
 */
void
errorConflictingDefElem(DefElem *defel, ParseState *pstate)
{
	ereport(ERROR,
			errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("conflicting or redundant options"),
			parser_errposition(pstate, defel->location));
}

/*
 * Collapse a DefElem option list to "last specification wins" semantics.
 *
 * Several utility commands (VACUUM, EXPLAIN, CHECKPOINT, REPACK, ...) accept a
 * parenthesized list of options and intend that, when an option is given more
 * than once, the last specification takes effect.  Historically each command
 * implemented its own parse loop, and getting last-wins right for every option
 * was easy to botch -- for example a loop that OR'd a boolean would keep an
 * option enabled even if its final specification was OFF.
 *
 * This helper centralizes that behavior.  It returns a newly-built list that
 * contains, for each distinct option name (compared case-sensitively on
 * defname), only the DefElem for its last occurrence in the input, keeping the
 * relative order of those surviving entries.  Callers can then run their
 * existing option-parsing loop over the result without worrying about repeated
 * options at all.
 *
 * The input list is not modified; the DefElem nodes themselves are not copied,
 * only referenced from the returned list.  A NIL input yields NIL.
 */
List *
deduplicateDefElemList(List *options)
{
	List	   *result = NIL;
	ListCell   *outer;

	foreach(outer, options)
	{
		DefElem    *opt = (DefElem *) lfirst(outer);
		bool		superseded = false;
		ListCell   *inner;

		/*
		 * Keep this entry only if no later entry carries the same option
		 * name.  This is O(n^2) in the number of options, but option lists are
		 * short (a handful of entries), so a hash table would be overkill.
		 */
		for_each_cell(inner, options, lnext(options, outer))
		{
			DefElem    *later = (DefElem *) lfirst(inner);

			if (strcmp(opt->defname, later->defname) == 0)
			{
				superseded = true;
				break;
			}
		}

		if (!superseded)
			result = lappend(result, opt);
	}

	return result;
}
