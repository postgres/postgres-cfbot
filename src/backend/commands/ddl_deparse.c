/*-------------------------------------------------------------------------
 *
 * ddl_deparse.c
 *	  Functions to convert utility commands to machine-parseable
 *	  representation
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *
 * This is intended to provide JSON blobs representing DDL commands, which
 * can later be re-processed into plain strings by well-defined sprintf-like
 * expansion (see ddl_json.c). These JSON objects are intended to allow for
 * machine-editing of the commands, by replacing certain nodes within the
 * objects.
 *
 * The input is the raw (untransformed) parse tree of the command plus the
 * list of CollectedCommand it produced when it was executed (which supplies
 * the OID of the created object).
 *
 * Two rules decide what the output contains. They divide the state the
 * original command produced between them: the state that came from a
 * default, and the state the command itself determined.
 *
 * The first rule is that the output reproduces the command the user wrote,
 * not the state it left behind. The parse tree therefore determines which
 * clauses appear: a clause the user did not write is not emitted, so that
 * when the reconstructed command is replayed -- possibly on a newer major
 * version -- the then-current defaults apply. A column with no COMPRESSION
 * clause is emitted without one, and a serial column is emitted as serial
 * rather than being expanded into an integer column plus a sequence.
 *
 * The second rule is that replaying the output has to leave the catalogs in
 * the state the original command left them in, defaults aside. What the
 * command determined and a replay would not arrive at on its own is emitted
 * even where the user did not write it: names are schema-qualified, so that
 * the result does not depend on the search_path in effect, and a constraint
 * whose name the command fixed is emitted with that name -- the one a LIKE
 * clause copied from the source table, or the one given on a redundant NOT
 * NULL specification -- because a constraint left unnamed is named after
 * the table it ends up on, which is not the same name.
 *
 * The definitions themselves (data types, expressions, constraint
 * definitions) are obtained from the system catalogs rather than from the
 * parse tree, as it is impossible to reliably construct a fully-specified
 * command (i.e. one not dependent on search_path etc.) looking only at the
 * parse node.
 *
 * Normalizations that change the spelling but not the effect are allowed:
 * type names are spelled canonically, and constraints given with a column
 * definition are emitted as table constraints.
 *
 * Deparsed JsonbValue objects are built with the append_jsonb_pairN()
 * helpers, passing N (key, value) pairs whose values come from
 * jbv_str()/jbv_bool()/jbv_null(), e.g.
 *
 *		append_jsonb_pair1(state, "fmt", jbv_str("..."));
 *
 * Object/array nesting is delimited by begin_jsonb_object()/
 * end_jsonb_object() and begin_jsonb_array()/end_jsonb_array().
 *
 * XXX: This module supports CREATE TABLE command with an exception of some
 * LIKE clause that requires deparsing CREATE INDEX etc. See
 * ddl_deparse_command_supported() for supported statements.
 *
 * XXX: For LIKE clause, DDL deparse expands the LIKE clause to the actual
 * columns to satisfy the second rule also in case where the deparsed DDL
 * is replay in a different environment. However, it might be worth considering
 * an option to prioritize the first rule that preserves the user intent
 * and simply reproduce the LIKE clause in the deparsed DDL.
 *
 * IDENTIFICATION
 *	  src/backend/commands/ddl_deparse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/toast_compression.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "commands/ddl_deparse.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "parser/parse_type.h"
#include "tcop/deparse_utility.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"

/* Estimated length of the generated jsonb string */
#define JSONB_ESTIMATED_LEN 128

static void deparse_DefElem(JsonbInState *state, DefElem *elem,
							bool is_reset);
static IndexStmt *find_collected_index_stmt(List *cmds, Oid indexId);

/*
 * Insert JsonbValue key to the output parse state.
 */
static void
insert_jsonb_key(JsonbInState *state, char *name)
{
	JsonbValue	key;

	/* Push the key */
	key.type = jbvString;
	key.val.string.val = name;
	key.val.string.len = strlen(name);
	pushJsonbValue(state, WJB_KEY, &key);
}

/*
 * Value constructors: build a JsonbValue of the right type. The type lives
 * in the returned value, so call sites are type-checked at construction (you
 * can't pass a bool where jbv_str expects a char *, etc).
 */
static inline JsonbValue
jbv_str(const char *value)
{
	JsonbValue	v = {0};

	v.type = jbvString;
	v.val.string.val = pstrdup(value);
	v.val.string.len = strlen(v.val.string.val);
	return v;
}

static inline JsonbValue
jbv_bool(bool value)
{
	JsonbValue	v = {0};

	v.type = jbvBool;
	v.val.boolean = value;
	return v;
}

static inline JsonbValue
jbv_null(void)
{
	JsonbValue	v = {0};

	v.type = jbvNull;
	return v;
}

/*
 * Append "numobjs" (key, JsonbValue) pairs from the varargs to the output
 * parse state. Values are built with jbv_str()/jbv_bool()/jbv_null().
 */
static void
new_jsonb_VA(JsonbInState *state, int numobjs, ...)
{
	va_list		args;

	va_start(args, numobjs);

	for (int i = 0; i < numobjs; i++)
	{
		char	   *name = va_arg(args, char *);
		JsonbValue	val = va_arg(args, JsonbValue);

		insert_jsonb_key(state, name);
		pushJsonbValue(state, WJB_VALUE, &val);
	}

	va_end(args);
}

/*
 * append_jsonb_pairN(state, k1, v1, ...): append exactly N (key, JsonbValue)
 * pairs (values built with jbv_*()). Thin macros over new_jsonb_VA() so the
 * count lives in the name and there is no sentinel to forget.
 */
#define append_jsonb_pair1(state, k1, v1)	\
	new_jsonb_VA(state, 1, (k1), (v1))
#define append_jsonb_pair2(state, k1, v1, k2, v2)	\
	new_jsonb_VA(state, 2, (k1), (v1), (k2), (v2))
#define append_jsonb_pair3(state, k1, v1, k2, v2, k3, v3)	\
	new_jsonb_VA(state, 3, (k1), (v1), (k2), (v2), (k3), (v3))
#define append_jsonb_pair4(state, k1, v1, k2, v2, k3, v3, k4, v4)	\
	new_jsonb_VA(state, 4, (k1), (v1), (k2), (v2), (k3), (v3), (k4), (v4))
#define append_jsonb_pair5(state, k1, v1, k2, v2, k3, v3, k4, v4, k5, v5)	\
	new_jsonb_VA(state, 5, (k1), (v1), (k2), (v2), (k3), (v3), (k4), (v4), \
				 (k5), (v5))

/* Convenience: append a JSON null under "key". */
static inline void
new_jsonb_null(JsonbInState *state, char *key)
{
	append_jsonb_pair1(state, key, jbv_null());
}

/*
 * Thin wrappers around pushJsonbValue for object/array delimiters, so the
 * begin/end pairing is visible and self-documenting at call sites.
 */
static inline void
begin_jsonb_object(JsonbInState *state)
{
	pushJsonbValue(state, WJB_BEGIN_OBJECT, NULL);
}

static inline void
end_jsonb_object(JsonbInState *state)
{
	pushJsonbValue(state, WJB_END_OBJECT, NULL);
}

static inline void
begin_jsonb_array(JsonbInState *state)
{
	pushJsonbValue(state, WJB_BEGIN_ARRAY, NULL);
}

static inline void
end_jsonb_array(JsonbInState *state)
{
	pushJsonbValue(state, WJB_END_ARRAY, NULL);
}

/*
 * Open a clause sub-object under "key": insert the key, begin the object,
 * and emit its "fmt". The caller adds any further data (e.g. a nested type
 * or qualified-name object) and then calls end_jsonb_clause().
 *
 * A clause is only emitted when the user actually wrote it; a clause that
 * was not written (or does not apply) is emitted as a JSON null
 * (new_jsonb_null) instead, which expands to nothing. The caller typically
 * writes:
 *
 *		if (user wrote it)
 *			... begin_jsonb_clause(state, key, fmt) ... end_jsonb_clause() ...
 *		else
 *			new_jsonb_null(state, key);
 *
 * The leading space of each clause lives in its own "fmt", so enclosing
 * format strings reference bare %{x}s slots.
 */
static void
begin_jsonb_clause(JsonbInState *state, char *key, char *fmt)
{
	insert_jsonb_key(state, key);
	begin_jsonb_object(state);
	append_jsonb_pair1(state, "fmt", jbv_str(fmt));
}

static inline void
end_jsonb_clause(JsonbInState *state)
{
	end_jsonb_object(state);
}

/* Emit a clause sub-object holding just a "fmt" */
static void
new_jsonb_clause(JsonbInState *state, char *key, char *fmt)
{
	begin_jsonb_clause(state, key, fmt);
	end_jsonb_clause(state);
}

/*
 * Emit a clause sub-object holding a "fmt" and a single string value: the
 * common
 *		begin_jsonb_clause(state, key, fmt);
 *		append_jsonb_pair1(state, valkey, jbv_str(val));
 *		end_jsonb_clause(state);
 * idiom, where fmt references the value as %{valkey}X.
 */
static void
new_jsonb_string_clause(JsonbInState *state, char *key, char *fmt,
						char *valkey, const char *val)
{
	begin_jsonb_clause(state, key, fmt);
	append_jsonb_pair1(state, valkey, jbv_str(val));
	end_jsonb_clause(state);
}

/*
 * A helper routine to insert jsonb for typId to the output parse state.
 */
static void
new_jsonb_for_type(JsonbInState *state, char *parentKey,
				   Oid typId, int32 typmod)
{
	Oid			typnspid;
	char	   *type_nsp;
	char	   *type_name = NULL;
	char	   *typmodstr;
	bool		type_array;

	Assert(parentKey);

	format_type_detailed(typId, typmod, &typnspid, &type_name, &typmodstr,
						 &type_array);

	if (OidIsValid(typnspid))
		type_nsp = get_namespace_name_or_temp(typnspid);
	else
		type_nsp = pstrdup("");

	insert_jsonb_key(state, parentKey);
	begin_jsonb_object(state);
	append_jsonb_pair4(state,
					   "schemaname", jbv_str(type_nsp),
					   "typename", jbv_str(type_name),
					   "typmod", jbv_str(typmodstr),
					   "typarray", jbv_bool(type_array));
	end_jsonb_object(state);
}

/*
 * A helper routine to set up schemaname and objname.
 *
 * If the namespace OID corresponds to a temp schema, that's set to
 * "pg_temp".
 */
static void
new_jsonb_for_qualname(JsonbInState *state, Oid nspid, char *objName,
					   char *keyName, bool createObject)
{
	char	   *namespace;

	if (isAnyTempNamespace(nspid))
		namespace = pstrdup("pg_temp");
	else
		namespace = get_namespace_name(nspid);

	/* Push the key first */
	if (keyName)
		insert_jsonb_key(state, keyName);

	if (createObject)
		begin_jsonb_object(state);

	append_jsonb_pair2(state,
					   "schemaname", jbv_str(namespace),
					   "objname", jbv_str(objName));

	if (createObject)
		end_jsonb_object(state);
}

/*
 * A helper routine to set up schemaname and objname by the given classId
 * and objectId.
 */
static void
new_jsonb_for_qualname_id(JsonbInState *state, Oid classId, Oid objectId,
						  char *keyName, bool createObject)
{
	Relation	catalog;
	HeapTuple	catobj;
	Datum		obj_nsp;
	Datum		obj_name;
	AttrNumber	Anum_name;
	AttrNumber	Anum_namespace;
	AttrNumber	Anum_oid = get_object_attnum_oid(classId);
	bool		isnull;

	catalog = table_open(classId, AccessShareLock);

	catobj = get_catalog_object_by_oid(catalog, Anum_oid, objectId);
	if (!catobj)
		elog(ERROR, "cache lookup failed for object with OID %u of catalog \"%s\"",
			 objectId, RelationGetRelationName(catalog));
	Anum_name = get_object_attnum_name(classId);
	Anum_namespace = get_object_attnum_namespace(classId);

	obj_nsp = heap_getattr(catobj, Anum_namespace, RelationGetDescr(catalog),
						   &isnull);
	if (isnull)
		elog(ERROR, "null namespace for object %u", objectId);

	obj_name = heap_getattr(catobj, Anum_name, RelationGetDescr(catalog),
							&isnull);
	if (isnull)
		elog(ERROR, "null attribute name for object %u", objectId);

	new_jsonb_for_qualname(state, DatumGetObjectId(obj_nsp),
						   NameStr(*DatumGetName(obj_name)),
						   keyName, createObject);
	table_close(catalog, AccessShareLock);
}

/*
 * A helper routine to insert key:value where value is array of qualname to
 * the output parse state.
 */
static void
new_jsonbArray_for_qualname_id(JsonbInState *state, char *keyname, List *array)
{
	ListCell   *lc;

	/* Push the key first */
	insert_jsonb_key(state, keyname);

	begin_jsonb_array(state);

	/* Push the array elements now */
	foreach(lc, array)
		new_jsonb_for_qualname_id(state, RelationRelationId, lfirst_oid(lc),
								  NULL, true);

	end_jsonb_array(state);
}

/*
 * Return the string representation of the given RELPERSISTENCE value, or
 * NULL for a permanent relation.
 */
static char *
get_persistence_str(char persistence)
{
	switch (persistence)
	{
		case RELPERSISTENCE_TEMP:
			return "TEMPORARY";
		case RELPERSISTENCE_UNLOGGED:
			return "UNLOGGED";
		case RELPERSISTENCE_PERMANENT:
			return NULL;
		default:
			elog(ERROR, "unexpected persistence marking %c", persistence);
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * If the given (raw) TypeName is one of the serial pseudo-types, return its
 * spelling as the user typed it; otherwise return NULL.
 *
 * This mirrors the serial detection in transformColumnDefinition().
 */
static const char *
get_serial_typename(const TypeName *typeName)
{
	const char *name;

	if (list_length(typeName->names) != 1 || typeName->pct_type)
		return NULL;

	name = strVal(linitial(typeName->names));

	if (strcmp(name, "smallserial") == 0 ||
		strcmp(name, "serial2") == 0 ||
		strcmp(name, "serial") == 0 ||
		strcmp(name, "serial4") == 0 ||
		strcmp(name, "bigserial") == 0 ||
		strcmp(name, "serial8") == 0)
		return name;

	return NULL;
}

/*
 * Return the first constraint of the given type attached to the column
 * definition, or NULL if there is none. If more than one is present,
 * prefer a named one: for redundant constraints such as "NOT NULL
 * CONSTRAINT foo NOT NULL", the explicitly given name is the one that ends
 * up in the catalogs.
 */
static Constraint *
find_column_constraint(const ColumnDef *coldef, ConstrType contype)
{
	Constraint *result = NULL;

	foreach_node(Constraint, constraint, coldef->constraints)
	{
		if (constraint->contype != contype)
			continue;

		if (result == NULL ||
			(result->conname == NULL && constraint->conname != NULL))
			result = constraint;
	}

	return result;
}

/*
 * Obtain the deparsed form of the stored (cooked) expression in pg_attrdef
 * for the given column; used for DEFAULT and GENERATED columns.
 *
 * A DEFAULT clause whose cooked expression is a plain NULL constant is
 * deliberately not stored in pg_attrdef (see AddRelationNewConstraints), so
 * a raw DEFAULT constraint may legitimately have no stored expression;
 * missing_ok callers get a NULL return for that case.
 *
 * Note we don't use build_column_default() here: it substitutes the type's
 * default when the column has none, returns a NextValueExpr for identity
 * columns, and may wrap the stored expression in another coercion; we want
 * exactly the expression that this command stored.
 */
static char *
get_stored_default_expr(Oid relid, AttrNumber attnum, List *dpcontext,
						bool missing_ok)
{
	Relation	attrdefrel;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	Datum		adbin;
	bool		isnull;
	Node	   *expr;
	char	   *defstr;

	attrdefrel = table_open(AttrDefaultRelationId, AccessShareLock);
	ScanKeyInit(&keys[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&keys[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));
	scan = systable_beginscan(attrdefrel, AttrDefaultIndexId, true,
							  NULL, 2, keys);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
		{
			systable_endscan(scan);
			table_close(attrdefrel, AccessShareLock);
			return NULL;
		}

		elog(ERROR, "could not find default value for attribute %d of relation %u",
			 attnum, relid);
	}

	adbin = heap_getattr(tuple, Anum_pg_attrdef_adbin,
						 RelationGetDescr(attrdefrel), &isnull);
	if (isnull)
		elog(ERROR, "null adbin for attribute %d of relation %u",
			 attnum, relid);

	expr = stringToNode(TextDatumGetCString(adbin));

	systable_endscan(scan);
	table_close(attrdefrel, AccessShareLock);

	defstr = deparse_expression(expr, dpcontext, false, false);

	return defstr;
}

/*
 * Return the list of inheritance parent relations of the given relation, in
 * inhseqno order (which is the order in which they were specified in the
 * command).
 */
static List *
get_inheritance_parents(Oid relid)
{
	List	   *parents = NIL;
	Relation	inhrel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;

	inhrel = table_open(InheritsRelationId, AccessShareLock);
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(inhrel, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_inherits inhForm = (Form_pg_inherits) GETSTRUCT(tuple);

		parents = lappend_oid(parents, inhForm->inhparent);
	}

	systable_endscan(scan);
	table_close(inhrel, AccessShareLock);

	return parents;
}

/*
 * Return the deparsed partition bound specification of the given relation,
 * from pg_class.relpartbound. We cannot use the parse tree's partbound as
 * it's the untransformed expression.
 */
static char *
get_partbound_spec_string(Oid relid)
{
	char	   *result;
	Datum		bound;
	Node	   *node;
	HeapTuple	tuple;
	bool		isnull;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation with OID %u", relid);

	bound = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relpartbound,
							&isnull);
	if (isnull)
		elog(ERROR, "null relpartbound for relation with OID %u", relid);

	node = (Node *) stringToNode(TextDatumGetCString(bound));
	ReleaseSysCache(tuple);

	result = deparse_expression(node, NIL, false, false);

	return result;
}

/*
 * Render one user-specified sequence option (of a GENERATED AS IDENTITY
 * column) as a plain string. Only the options the user wrote are rendered;
 * we do not fill in the rest from pg_sequence, so that replaying the
 * command derives the same defaults the original did.
 *
 * This must handle everything gram.y's SeqOptElem production admits, since
 * we work on the raw options list: parse analysis consumes SEQUENCE NAME
 * (see generateSerialExtraStmts), but it is still present in the raw tree.
 * The exception is OWNED BY, which the caller filters out.
 */
static char *
deparse_SeqOptElem(DefElem *elem)
{
	if (strcmp(elem->defname, "as") == 0)
		return psprintf("AS %s", TypeNameToString(defGetTypeName(elem)));
	else if (strcmp(elem->defname, "cache") == 0)
		return psprintf("CACHE " INT64_FORMAT, defGetInt64(elem));
	else if (strcmp(elem->defname, "cycle") == 0)
		return defGetBoolean(elem) ? "CYCLE" : "NO CYCLE";
	else if (strcmp(elem->defname, "increment") == 0)
		return psprintf("INCREMENT BY " INT64_FORMAT, defGetInt64(elem));
	else if (strcmp(elem->defname, "maxvalue") == 0)
		return elem->arg ?
			psprintf("MAXVALUE " INT64_FORMAT, defGetInt64(elem)) :
			"NO MAXVALUE";
	else if (strcmp(elem->defname, "minvalue") == 0)
		return elem->arg ?
			psprintf("MINVALUE " INT64_FORMAT, defGetInt64(elem)) :
			"NO MINVALUE";
	else if (strcmp(elem->defname, "start") == 0)
		return psprintf("START WITH " INT64_FORMAT, defGetInt64(elem));
	else if (strcmp(elem->defname, "restart") == 0)
		return elem->arg ?
			psprintf("RESTART WITH " INT64_FORMAT, defGetInt64(elem)) :
			"RESTART";
	else if (strcmp(elem->defname, "logged") == 0)
		return "LOGGED";
	else if (strcmp(elem->defname, "unlogged") == 0)
		return "UNLOGGED";
	else if (strcmp(elem->defname, "sequence_name") == 0)
		return psprintf("SEQUENCE NAME %s",
						NameListToQuotedString(defGetQualifiedName(elem)));

	elog(ERROR, "unsupported sequence option \"%s\"", elem->defname);
	return NULL;				/* keep compiler quiet */
}

/*
 * Deparse the definition of a column identity to Jsonb.
 *
 * The GENERATED { ALWAYS | BY DEFAULT } keyword comes from the raw
 * constraint; sequence options are emitted only if the user wrote any.
 */
static void
deparse_ColumnIdentity(JsonbInState *state, char *parentKey,
					   Constraint *constraint)
{
	List	   *options = NIL;

	insert_jsonb_key(state, parentKey);
	begin_jsonb_object(state);

	append_jsonb_pair2(state,
					   "fmt", jbv_str(" GENERATED %{option}s AS IDENTITY%{seq_options}s"),
					   "option", jbv_str(constraint->generated_when == ATTRIBUTE_IDENTITY_ALWAYS ?
										 "ALWAYS" : "BY DEFAULT"));

	/*
	 * OWNED BY is accepted among identity sequence options, but execution
	 * overrides it: the sequence always ends up owned by the identity column.
	 * Omitting it preserves the command's effects, and is also necessary for
	 * a faithful replay, since the name it carries need not be
	 * schema-qualified.
	 */
	foreach_node(DefElem, elem, constraint->options)
	{
		if (strcmp(elem->defname, "owned_by") == 0)
			continue;

		options = lappend(options, elem);
	}

	if (options != NIL)
	{
		begin_jsonb_clause(state, "seq_options", " (%{options: }s)");
		insert_jsonb_key(state, "options");
		begin_jsonb_array(state);
		foreach_node(DefElem, elem, options)
		{
			JsonbValue	val = jbv_str(deparse_SeqOptElem(elem));

			pushJsonbValue(state, WJB_ELEM, &val);
		}
		end_jsonb_array(state);
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "seq_options");

	end_jsonb_object(state);
}

/*
 * Deparse the NOT NULL clause of a column definition, including an optional
 * constraint name and NO INHERIT, all from the raw constraint.
 */
static void
deparse_ColumnNotNull(JsonbInState *state, char *parentKey,
					  Constraint *constraint)
{
	insert_jsonb_key(state, parentKey);
	begin_jsonb_object(state);

	append_jsonb_pair1(state,
					   "fmt", jbv_str(" %{name}sNOT NULL%{no_inherit}s"));

	if (constraint->conname != NULL)
		new_jsonb_string_clause(state, "name", "CONSTRAINT %{conname}I ",
								"conname", constraint->conname);
	else
		new_jsonb_null(state, "name");

	if (constraint->is_no_inherit)
		new_jsonb_clause(state, "no_inherit", " NO INHERIT");
	else
		new_jsonb_null(state, "no_inherit");

	end_jsonb_object(state);
}

/*
 * Emit the DEFAULT clause of a column, under "default", from the stored
 * (cooked) expression in pg_attrdef. A DEFAULT whose cooked expression is a
 * plain NULL constant is not stored there (see get_stored_default_expr) and
 * is emitted as written. The caller has established the column has a DEFAULT.
 */
static void
deparse_column_default(JsonbInState *state, Oid relid, AttrNumber attnum,
					   List *dpcontext)
{
	char	   *defstr = get_stored_default_expr(relid, attnum, dpcontext, true);

	new_jsonb_string_clause(state, "default", " DEFAULT %{default_expr}s",
							"default_expr", defstr != NULL ? defstr : "NULL");
}

/*
 * Emit the GENERATED ... AS ( expr ) STORED/VIRTUAL clause of a generated
 * column, under "generated_column", from the stored generation expression.
 * The caller has established the column is generated.
 */
static void
deparse_column_generated(JsonbInState *state, Oid relid, AttrNumber attnum,
						 List *dpcontext, Constraint *constraint)
{
	char	   *fmt = (constraint->generated_kind == ATTRIBUTE_GENERATED_VIRTUAL) ?
		" GENERATED ALWAYS AS (%{generation_expr}s) VIRTUAL" :
		" GENERATED ALWAYS AS (%{generation_expr}s) STORED";

	new_jsonb_string_clause(state, "generated_column", fmt, "generation_expr",
							get_stored_default_expr(relid, attnum, dpcontext,
													false));
}

/*
 * Deparse a ColumnDef node within a regular (non-typed) table creation.
 *
 * Only clauses present in the raw column definition are emitted; clauses the
 * user did not write are emitted as JSON null. The column's data type and
 * the expressions are obtained from the catalogs.
 *
 * NOT NULL constraints in the column definition are emitted directly in the
 * column definition by this routine; other constraints are emitted
 * elsewhere, from pg_constraint (the info in the parse node is incomplete
 * anyway).
 */
static void
deparse_ColumnDef(JsonbInState *state, Oid relid, List *dpcontext,
				  ColumnDef *coldef)
{
	HeapTuple	attrTup;
	Form_pg_attribute attrForm;
	const char *serial_typename;
	Constraint *constraint;

	attrTup = SearchSysCacheAttName(relid, coldef->colname);
	if (!HeapTupleIsValid(attrTup))
		elog(ERROR, "could not find cache entry for column \"%s\" of relation %u",
			 coldef->colname, relid);
	attrForm = (Form_pg_attribute) GETSTRUCT(attrTup);

	/* start making column object */
	begin_jsonb_object(state);

	/* fixed format string: every clause slot is always referenced */
	append_jsonb_pair1(state, "fmt",
					   jbv_str("%{name}I %{coltype}T%{storage}s%{compression}s%{collation}s"
							   "%{not_null}s%{default}s%{identity_column}s%{generated_column}s"));

	/* NAME and TYPE (always present, referenced bare) */
	append_jsonb_pair2(state,
					   "name", jbv_str(coldef->colname),
					   "type", jbv_str("column"));

	/*
	 * COLUMN TYPE. A serial column is emitted exactly as the user typed it;
	 * expanding it into an integer type plus a sequence would defeat the
	 * point of preserving the command (a replay must create its own
	 * sequence). Other types are obtained from the catalog, which normalizes
	 * the spelling but preserves the typmod the user gave.
	 */
	serial_typename = get_serial_typename(coldef->typeName);
	if (serial_typename != NULL)
	{
		insert_jsonb_key(state, "coltype");
		begin_jsonb_object(state);
		append_jsonb_pair4(state,
						   "schemaname", jbv_str(""),
						   "typename", jbv_str(serial_typename),
						   "typmod", jbv_str(""),
						   "typarray", jbv_bool(false));
		end_jsonb_object(state);
	}
	else
		new_jsonb_for_type(state, "coltype", attrForm->atttypid,
						   attrForm->atttypmod);

	/* STORAGE: only if the user wrote it; the value as typed */
	if (coldef->storage_name != NULL)
		new_jsonb_string_clause(state, "storage", " STORAGE %{colstorage}s",
								"colstorage", coldef->storage_name);
	else
		new_jsonb_null(state, "storage");

	/*
	 * COMPRESSION: only if the user wrote it; the value as typed. We
	 * deliberately do not emit the effective attcompression for columns
	 * without the clause, so that a replay adopts whatever the compression
	 * default is there. (The value can be the keyword "default", which
	 * survives being quoted by %I.)
	 */
	if (coldef->compression != NULL)
		new_jsonb_string_clause(state, "compression",
								" COMPRESSION %{compression_method}I",
								"compression_method", coldef->compression);
	else
		new_jsonb_null(state, "compression");

	/* COLLATE: only if the user wrote it; the resolved collation */
	if (coldef->collClause != NULL)
	{
		begin_jsonb_clause(state, "collation", " COLLATE %{collation_name}D");
		new_jsonb_for_qualname_id(state, CollationRelationId,
								  attrForm->attcollation, "collation_name",
								  true);
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "collation");

	/*
	 * NOT NULL: only if the user wrote it on this column. NOT NULL
	 * constraints that the system derives (from a PRIMARY KEY or a serial
	 * type) are not in the raw column definition and are correctly re-derived
	 * on replay.
	 */
	constraint = find_column_constraint(coldef, CONSTR_NOTNULL);
	if (constraint != NULL)
		deparse_ColumnNotNull(state, "not_null", constraint);
	else
		new_jsonb_null(state, "not_null");

	/*
	 * DEFAULT: only if the user wrote it; the stored (cooked) expression from
	 * pg_attrdef. A serial column has no DEFAULT in the raw tree, so the
	 * internally-generated nextval() default does not leak out. A DEFAULT
	 * whose cooked expression is a plain NULL constant has no stored
	 * expression at all; emit it as written.
	 */
	constraint = find_column_constraint(coldef, CONSTR_DEFAULT);
	if (constraint != NULL)
		deparse_column_default(state, relid, attrForm->attnum, dpcontext);
	else
		new_jsonb_null(state, "default");

	/* GENERATED AS IDENTITY */
	constraint = find_column_constraint(coldef, CONSTR_IDENTITY);
	if (constraint != NULL)
		deparse_ColumnIdentity(state, "identity_column", constraint);
	else
		new_jsonb_null(state, "identity_column");

	/* GENERATED ... AS ( expr ) STORED/VIRTUAL */
	constraint = find_column_constraint(coldef, CONSTR_GENERATED);
	if (constraint != NULL)
		deparse_column_generated(state, relid, attrForm->attnum, dpcontext,
								 constraint);
	else
		new_jsonb_null(state, "generated_column");

	ReleaseSysCache(attrTup);

	/* mark the end of one column object */
	end_jsonb_object(state);
}

/*
 * Render an attstorage value back as the keyword the grammar accepts.
 */
static const char *
storage_keyword(char storage)
{
	switch (storage)
	{
		case TYPSTORAGE_PLAIN:
			return "PLAIN";
		case TYPSTORAGE_EXTERNAL:
			return "EXTERNAL";
		case TYPSTORAGE_EXTENDED:
			return "EXTENDED";
		case TYPSTORAGE_MAIN:
			return "MAIN";
	}

	elog(ERROR, "invalid attstorage value \'%c\'", storage);
	return NULL;				/* keep compiler quiet */
}

/*
 * Deparse a column that a LIKE clause contributed.
 *
 * Such a column has no definition in the raw statement -- the clause is
 * expanded against the source table before execution -- so everything is read
 * back from the catalogs. The user-intent rule that governs
 * deparse_ColumnDef() has nothing to work from here and is replaced by the
 * closest equivalent: a clause is emitted when the column's property differs
 * from what a column of that type would get by default, which is exactly when
 * it has to be stated for the replayed column to come out the same. Whether
 * the property was copied because the user asked for it (INCLUDING STORAGE
 * and friends) need not be consulted, since a property that was not copied
 * holds its default and so emits nothing either way.
 */
static void
deparse_ColumnDef_like(JsonbInState *state, Oid relid, List *dpcontext,
					   Form_pg_attribute attrForm)
{
	AttrNumber	attnum = attrForm->attnum;

	begin_jsonb_object(state);

	/* fixed format string: every clause slot is always referenced */
	append_jsonb_pair1(state, "fmt",
					   jbv_str("%{name}I %{coltype}T%{storage}s%{compression}s%{collation}s"
							   "%{not_null}s%{default}s%{identity_column}s%{generated_column}s"));

	append_jsonb_pair2(state,
					   "name", jbv_str(NameStr(attrForm->attname)),
					   "type", jbv_str("column"));

	new_jsonb_for_type(state, "coltype", attrForm->atttypid,
					   attrForm->atttypmod);

	/* STORAGE: only where it differs from the type's own */
	if (attrForm->attstorage != get_typstorage(attrForm->atttypid))
		new_jsonb_string_clause(state, "storage", " STORAGE %{colstorage}s",
								"colstorage",
								storage_keyword(attrForm->attstorage));
	else
		new_jsonb_null(state, "storage");

	/* COMPRESSION: only where one was set */
	if (CompressionMethodIsValid(attrForm->attcompression))
		new_jsonb_string_clause(state, "compression",
								" COMPRESSION %{compression_method}I",
								"compression_method",
								GetCompressionMethodName(attrForm->attcompression));
	else
		new_jsonb_null(state, "compression");

	/* COLLATE: only where it differs from the type's own */
	if (OidIsValid(attrForm->attcollation) &&
		attrForm->attcollation != get_typcollation(attrForm->atttypid))
	{
		begin_jsonb_clause(state, "collation", " COLLATE %{collation_name}D");
		new_jsonb_for_qualname_id(state, CollationRelationId,
								  attrForm->attcollation, "collation_name",
								  true);
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "collation");

	/*
	 * NOT NULL, named. A LIKE clause copies the source's constraint name
	 * verbatim, so a replay that let the name be derived afresh would not
	 * reproduce the command; emit it as a named column constraint.
	 */
	if (attrForm->attnotnull)
	{
		Constraint *constraint = makeNode(Constraint);
		HeapTuple	contup = findNotNullConstraintAttnum(relid, attnum);

		constraint->contype = CONSTR_NOTNULL;
		if (HeapTupleIsValid(contup))
		{
			Form_pg_constraint conForm;

			conForm = (Form_pg_constraint) GETSTRUCT(contup);
			constraint->conname = pstrdup(NameStr(conForm->conname));
			constraint->is_no_inherit = conForm->connoinherit;
			heap_freetuple(contup);
		}

		deparse_ColumnNotNull(state, "not_null", constraint);
	}
	else
		new_jsonb_null(state, "not_null");

	/* DEFAULT, unless the expression is a generation expression */
	if (attrForm->atthasdef && !attrForm->attgenerated)
		deparse_column_default(state, relid, attnum, dpcontext);
	else
		new_jsonb_null(state, "default");

	/* GENERATED AS IDENTITY, with the sequence parameters that were copied */
	if (attrForm->attidentity)
	{
		Relation	rel = relation_open(relid, AccessShareLock);
		Constraint *constraint = makeNode(Constraint);

		constraint->contype = CONSTR_IDENTITY;
		constraint->generated_when = attrForm->attidentity;
		constraint->options =
			sequence_options(getIdentitySequence(rel, attnum, false));
		relation_close(rel, AccessShareLock);

		deparse_ColumnIdentity(state, "identity_column", constraint);
	}
	else
		new_jsonb_null(state, "identity_column");

	/* GENERATED ... AS ( expr ) STORED/VIRTUAL */
	if (attrForm->attgenerated)
	{
		Constraint *constraint = makeNode(Constraint);

		constraint->contype = CONSTR_GENERATED;
		constraint->generated_kind = attrForm->attgenerated;

		deparse_column_generated(state, relid, attnum, dpcontext, constraint);
	}
	else
		new_jsonb_null(state, "generated_column");

	end_jsonb_object(state);
}

/*
 * Deparse a ColumnDef node within a typed or partition table creation.
 * This is simpler than the regular case, because the type comes from the
 * composite type or the parent; only the options the user listed are
 * emitted.
 */
static void
deparse_ColumnDef_typed(JsonbInState *state, Oid relid, List *dpcontext,
						ColumnDef *coldef)
{
	HeapTuple	attrTup;
	Form_pg_attribute attrForm;
	Constraint *constraint;

	attrTup = SearchSysCacheAttName(relid, coldef->colname);
	if (!HeapTupleIsValid(attrTup))
		elog(ERROR, "could not find cache entry for column \"%s\" of relation %u",
			 coldef->colname, relid);
	attrForm = (Form_pg_attribute) GETSTRUCT(attrTup);

	/* Identity columns cannot be specified this way */
	if (find_column_constraint(coldef, CONSTR_IDENTITY) != NULL)
		elog(ERROR, "unexpected constraint in typed or partition table column");

	/* start making column object */
	begin_jsonb_object(state);

	append_jsonb_pair1(state, "fmt",
					   jbv_str("%{name}I WITH OPTIONS%{not_null}s%{default}s%{generated_column}s"));

	/* NAME and TYPE */
	append_jsonb_pair2(state,
					   "name", jbv_str(coldef->colname),
					   "type", jbv_str("column"));

	/* NOT NULL */
	constraint = find_column_constraint(coldef, CONSTR_NOTNULL);
	if (constraint != NULL)
		deparse_ColumnNotNull(state, "not_null", constraint);
	else
		new_jsonb_null(state, "not_null");

	/* DEFAULT; as in deparse_ColumnDef, DEFAULT NULL has no stored expr */
	constraint = find_column_constraint(coldef, CONSTR_DEFAULT);
	if (constraint != NULL)
		deparse_column_default(state, relid, attrForm->attnum, dpcontext);
	else
		new_jsonb_null(state, "default");

	/* GENERATED ... AS ( expr ), e.g. overridden in a partition */
	constraint = find_column_constraint(coldef, CONSTR_GENERATED);
	if (constraint != NULL)
		deparse_column_generated(state, relid, attrForm->attnum, dpcontext,
								 constraint);
	else
		new_jsonb_null(state, "generated_column");

	ReleaseSysCache(attrTup);

	/* mark the end of column object */
	end_jsonb_object(state);
}

/*
 * Deparse a raw table-level NOT NULL constraint (NOT NULL column_name).
 *
 * These are not obtained from pg_constraint like the other table
 * constraints; the raw tree tells us exactly which ones the user wrote.
 */
static void
deparse_TableNotNull(JsonbInState *state, Constraint *constraint)
{
	begin_jsonb_object(state);

	append_jsonb_pair3(state,
					   "fmt", jbv_str("%{name}sNOT NULL %{column}I%{no_inherit}s"),
					   "type", jbv_str("constraint"),
					   "contype", jbv_str("not null"));

	if (constraint->conname != NULL)
		new_jsonb_string_clause(state, "name", "CONSTRAINT %{conname}I ",
								"conname", constraint->conname);
	else
		new_jsonb_null(state, "name");

	append_jsonb_pair1(state, "column",
					   jbv_str(strVal(linitial(constraint->keys))));

	if (constraint->is_no_inherit)
		new_jsonb_clause(state, "no_inherit", " NO INHERIT");
	else
		new_jsonb_null(state, "no_inherit");

	end_jsonb_object(state);
}

/*
 * Verify that "suffix" is an exact trailing substring of "def" and, if so,
 * truncate it off "def" in place and return true; otherwise leave def
 * unchanged and return false. Used to peel catalog-flag-determined trailing
 * clauses off a pg_get_constraintdef result; the caller reports a mismatch,
 * which would mean the regenerated text has fallen out of sync with
 * ruleutils.
 */
static bool
strip_suffix(char *def, const char *suffix)
{
	size_t		deflen = strlen(def);
	size_t		suffixlen = strlen(suffix);

	if (deflen < suffixlen ||
		strcmp(def + deflen - suffixlen, suffix) != 0)
		return false;
	def[deflen - suffixlen] = '\0';
	return true;
}

/*
 * Split the constraint-attribute trailer (DEFERRABLE etc.) off a
 * pg_get_constraintdef result, truncating def in place. The trailer text
 * is reconstructed from the pg_constraint flags exactly as
 * pg_get_constraintdef_worker appends it, so a suffix mismatch means the
 * two have fallen out of sync and is reported as an internal error rather
 * than risking wrong output. Returns the trailer, or NULL if there is
 * none.
 */
static char *
split_constraint_attributes(char *def, Form_pg_constraint constrForm)
{
	StringInfoData trailer;

	initStringInfo(&trailer);

	if (constrForm->condeferrable)
		appendStringInfoString(&trailer, " DEFERRABLE");

	if (constrForm->condeferred)
		appendStringInfoString(&trailer, " INITIALLY DEFERRED");

	if (!constrForm->conenforced)
		appendStringInfoString(&trailer, " NOT ENFORCED");
	else if (!constrForm->convalidated)
		appendStringInfoString(&trailer, " NOT VALID");

	if (trailer.len == 0)
		return NULL;

	if (!strip_suffix(def, trailer.data))
		elog(ERROR, "unexpected format of constraint definition \"%s\"", def);

	return trailer.data;
}

/*
 * Split the predicate (" WHERE (...)") off an exclusion constraint
 * definition whose attribute trailer has already been removed, truncating
 * def in place. As above, the predicate text is regenerated exactly as
 * pg_get_constraintdef produced it -- through pg_get_expr, which uses the
 * same deparse context and the same pretty flags (PRETTYFLAG_INDENT, which
 * can render constructs such as CASE across multiple lines) -- and any
 * mismatch is an internal error. Returns the predicate clause, or NULL if
 * the index has none.
 */
static char *
split_exclusion_where(char *def, Oid indexId, Oid relationId)
{
	HeapTuple	indtup;
	Datum		pred;
	bool		isnull;
	char	   *predstr;
	char	   *wheretext;

	indtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexId));
	if (!HeapTupleIsValid(indtup))
		elog(ERROR, "cache lookup failed for index %u", indexId);
	pred = SysCacheGetAttr(INDEXRELID, indtup, Anum_pg_index_indpred,
						   &isnull);
	if (isnull)
	{
		ReleaseSysCache(indtup);
		return NULL;
	}

	predstr = TextDatumGetCString(DirectFunctionCall2(pg_get_expr, pred,
													  ObjectIdGetDatum(relationId)));
	ReleaseSysCache(indtup);

	wheretext = psprintf(" WHERE (%s)", predstr);

	if (!strip_suffix(def, wheretext))
		elog(ERROR, "unexpected predicate in constraint definition \"%s\"", def);

	return wheretext;
}

/*
 * Emit one constraint (given by OID) as a JSON constraint object into the
 * current container. Shared by CREATE TABLE (table-element constraints) and
 * ALTER TABLE ADD CONSTRAINT.
 *
 * The bulk of the definition comes from pg_get_constraintdef, but for an
 * index-backed constraint (PRIMARY KEY / UNIQUE / EXCLUDE) the clauses that
 * pg_get_constraintdef either omits (index storage parameters) or renders in
 * the wrong grammatical position (USING INDEX TABLESPACE relative to the
 * attribute trailer and the exclusion predicate) are emitted through their
 * own slots, sourced from the collected CREATE INDEX subcommand (cmds).
 *
 * The constraint name is emitted only if the user specified one -- i.e. its
 * catalog name appears in typed_names, the list of names the user wrote --
 * so that an unnamed constraint gets the same automatically-derived name on
 * replay.
 */
static void
deparse_one_constraint(JsonbInState *state, Oid conoid, List *cmds,
					   List *typed_names)
{
	HeapTuple	tuple;
	Form_pg_constraint constrForm;
	char	   *contype;
	char	   *conname;
	char	   *definition;
	char	   *attributes = NULL;
	char	   *wheretext = NULL;
	bool		user_named = false;
	IndexStmt  *idxstmt = NULL;
	ListCell   *lc;

	tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for constraint %u", conoid);

	constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

	switch (constrForm->contype)
	{
		case CONSTRAINT_CHECK:
			contype = "check";
			break;
		case CONSTRAINT_FOREIGN:
			contype = "foreign key";
			break;
		case CONSTRAINT_PRIMARY:
			contype = "primary key";
			break;
		case CONSTRAINT_UNIQUE:
			contype = "unique";
			break;
		case CONSTRAINT_EXCLUSION:
			contype = "exclusion";
			break;
		default:
			elog(ERROR, "unrecognized constraint type");
	}

	conname = NameStr(constrForm->conname);

	/* Did the user name this constraint? */
	foreach(lc, typed_names)
	{
		if (strcmp(conname, (const char *) lfirst(lc)) == 0)
		{
			user_named = true;
			break;
		}
	}

	/*
	 * For an index-backed constraint, find the collected CREATE INDEX
	 * subcommand that created its index; it records the clauses that
	 * execution honored for it.
	 */
	if (constrForm->contype == CONSTRAINT_PRIMARY ||
		constrForm->contype == CONSTRAINT_UNIQUE ||
		constrForm->contype == CONSTRAINT_EXCLUSION)
	{
		idxstmt = find_collected_index_stmt(cmds, constrForm->conindid);

		if (idxstmt == NULL)
			elog(ERROR, "could not find collected CREATE INDEX command for index %u",
				 constrForm->conindid);
	}

	definition = TextDatumGetCString(DirectFunctionCall1(pg_get_constraintdef,
														 ObjectIdGetDatum(conoid)));

	/*
	 * For index-backed constraints, split the attribute trailer (and the
	 * exclusion predicate) off the definition so that the tablespace clause
	 * can be emitted in its grammatical position before them. CHECK and
	 * FOREIGN KEY constraints have no such clause, so their definitions can
	 * be kept whole.
	 */
	if (idxstmt != NULL)
	{
		attributes = split_constraint_attributes(definition, constrForm);

		if (constrForm->contype == CONSTRAINT_EXCLUSION)
			wheretext = split_exclusion_where(definition,
											  constrForm->conindid,
											  constrForm->conrelid);
	}

	begin_jsonb_object(state);

	/* fixed format string always references the optional slots */
	append_jsonb_pair4(state,
					   "fmt", jbv_str("%{name}s%{definition}s%{with}s"
									  "%{using_index_tablespace}s"
									  "%{exclusion_where}s%{attributes}s"),
					   "type", jbv_str("constraint"),
					   "contype", jbv_str(contype),
					   "definition", jbv_str(definition));

	if (user_named)
		new_jsonb_string_clause(state, "name", "CONSTRAINT %{conname}I ",
								"conname", conname);
	else
		new_jsonb_null(state, "name");

	/*
	 * WITH (index storage parameters), only if the user wrote them. Not for
	 * exclusion constraints: pg_get_constraintdef renders those via
	 * pg_get_indexdef_worker, whose output already carries the WITH clause
	 * (in its grammatical position), unlike the PRIMARY KEY and UNIQUE
	 * output, which omits it.
	 */
	if (idxstmt != NULL && idxstmt->options != NIL &&
		constrForm->contype != CONSTRAINT_EXCLUSION)
	{
		ListCell   *optcell;

		begin_jsonb_clause(state, "with", " WITH (%{options:, }s)");
		insert_jsonb_key(state, "options");
		begin_jsonb_array(state);

		foreach(optcell, idxstmt->options)
			deparse_DefElem(state, (DefElem *) lfirst(optcell), false);

		end_jsonb_array(state);
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "with");

	/* USING INDEX TABLESPACE, only if the user wrote it */
	if (idxstmt != NULL && idxstmt->tableSpace != NULL)
		new_jsonb_string_clause(state, "using_index_tablespace",
								" USING INDEX TABLESPACE %{tblspc}I",
								"tblspc", idxstmt->tableSpace);
	else
		new_jsonb_null(state, "using_index_tablespace");

	/*
	 * The split-off texts are emitted through value slots, not as format
	 * strings: the exclusion predicate is user-derived text that must not be
	 * scanned for conversion specifiers.
	 */
	if (wheretext != NULL)
		new_jsonb_string_clause(state, "exclusion_where", "%{where}s",
								"where", wheretext);
	else
		new_jsonb_null(state, "exclusion_where");

	if (attributes != NULL)
		new_jsonb_string_clause(state, "attributes", "%{attrs}s",
								"attrs", attributes);
	else
		new_jsonb_null(state, "attributes");

	end_jsonb_object(state);

	ReleaseSysCache(tuple);
}

/*
 * Subroutine for CREATE TABLE deparsing.
 *
 * Given a table OID, obtain its constraints from pg_constraint and append
 * them to the given JsonbInState, as table-level constraints.
 *
 * All the local constraints of the table were created by this command (LIKE
 * clauses are not supported), so emitting all of them preserves what the
 * user wrote; constraints given with a column definition are normalized to
 * table constraints, which has the same effects. The constraint name is
 * emitted only if the user specified one (i.e. the name appears in
 * typed_names, collected from the raw tree); an unnamed constraint gets the
 * same automatically-derived name on replay. NOT NULL constraints are
 * handled from the raw tree instead (see deparse_ColumnNotNull and
 * deparse_TableNotNull).
 *
 * cmds is the list of commands collected for the query; the CREATE INDEX
 * subcommands in it tell us whether a USING INDEX TABLESPACE clause was in
 * effect for each index-backed constraint, so the clause is only emitted if
 * the user wrote it (an index can end up in a non-default tablespace via
 * the default_tablespace GUC as well, and that decision is not ours to
 * freeze).
 *
 * Returns the number of constraint elements emitted, which the caller adds to
 * its own count of the elements in the list.
 */
static int
deparse_Constraints(JsonbInState *state, Oid relationId, List *typed_names,
					List *cmds)
{
	Relation	conRel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid		   *conoids;
	int			nconstraints = 0;
	int			maxconstraints = 8;

	Assert(OidIsValid(relationId));

	/*
	 * Scan pg_constraint to fetch all constraints linked to the given
	 * relation. The index scan returns them in name order; sort them by OID
	 * instead, which for constraints created by a single command is their
	 * creation order, so that replaying the deparsed command creates them in
	 * the same order as the original did.
	 */
	conoids = palloc_array(Oid, maxconstraints);
	conRel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relationId));
	scan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint constrForm;

		constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/* No need to deparse constraints inherited from parent table. */
		if (!constrForm->conislocal)
			continue;

		/* NOT NULL constraints are emitted from the raw tree instead */
		if (constrForm->contype == CONSTRAINT_NOTNULL)
			continue;

		if (nconstraints >= maxconstraints)
		{
			maxconstraints *= 2;
			conoids = repalloc_array(conoids, Oid, maxconstraints);
		}
		conoids[nconstraints++] = constrForm->oid;
	}

	systable_endscan(scan);
	table_close(conRel, AccessShareLock);

	qsort(conoids, nconstraints, sizeof(Oid), oid_cmp);

	/* Emit each constraint, in creation (OID) order. */
	for (int i = 0; i < nconstraints; i++)
		deparse_one_constraint(state, conoids[i], cmds, typed_names);

	pfree(conoids);

	return nconstraints;
}

/*
 * Every constraint name on the relation, as a list of plain strings in the
 * form deparse_one_constraint() expects for its "user named it" test.
 */
static List *
all_constraint_names(Oid relationId)
{
	Relation	conRel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *names = NIL;

	conRel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key, Anum_pg_constraint_conrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(relationId));
	scan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		names = lappend(names, pstrdup(NameStr(constrForm->conname)));
	}

	systable_endscan(scan);
	table_close(conRel, AccessShareLock);

	return names;
}

/*
 * Does this raw table element list contain a LIKE clause?
 */
static bool
has_like_clause(List *tableElts)
{
	ListCell   *lc;

	foreach(lc, tableElts)
	{
		if (IsA(lfirst(lc), TableLikeClause))
			return true;
	}
	return false;
}

/*
 * Subroutine for CREATE TABLE deparsing.
 *
 * Insert columns and constraints elements (if any) in output JsonbInState.
 */
static void
add_table_elems(JsonbInState *state, Oid objectId, CreateStmt *stmt,
				List *dpcontext, List *typed_names,
				List *cmds, bool typed)
{
	bool		parenthesize;
	int			nelems = 0;
	ListCell   *lc;

	insert_jsonb_key(state, "table_elements");
	begin_jsonb_object(state);

	/*
	 * Process table elements: column definitions and table-level NOT NULL
	 * constraints come from the raw parse tree; the other constraints come
	 * from pg_constraint, because the parse node lacks things such as the
	 * definitive constraint definition.
	 */
	insert_jsonb_key(state, "elements");
	begin_jsonb_array(state);

	if (has_like_clause(stmt->tableElts))
	{
		Relation	rel;
		TupleDesc	tupdesc;

		/*
		 * A LIKE clause contributed columns that are not in the raw
		 * statement, so the column list has to be driven by the catalog to
		 * pick them up. Walking it in attribute order also keeps the columns
		 * in the order the original command produced them, which the raw
		 * element list no longer describes once a LIKE clause is interleaved
		 * with written-out columns.
		 *
		 * A column that does appear in the raw statement is still deparsed
		 * from there, so that what the user wrote about it is preserved as
		 * usual; only the ones the clause brought in fall back to the
		 * catalog.
		 */
		rel = relation_open(objectId, AccessShareLock);
		tupdesc = RelationGetDescr(rel);

		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			ColumnDef  *raw = NULL;

			if (att->attisdropped)
				continue;

			/* an inherited column is re-derived by the INHERITS clause */
			if (!att->attislocal)
				continue;

			/* Find the raw definition of the named column */
			foreach(lc, stmt->tableElts)
			{
				Node	   *elt = (Node *) lfirst(lc);

				if (IsA(elt, ColumnDef) &&
					strcmp(((ColumnDef *) elt)->colname, NameStr(att->attname)) == 0)
				{
					raw = (ColumnDef *) elt;
					break;
				}
			}

			if (raw != NULL)
				deparse_ColumnDef(state, objectId, dpcontext, raw);
			else
				deparse_ColumnDef_like(state, objectId, dpcontext, att);

			nelems++;
		}

		relation_close(rel, AccessShareLock);

		/*
		 * A LIKE clause copies constraint names verbatim, so the names in the
		 * catalog are the ones the command produced and every one of them has
		 * to be emitted. Treating them all as user-named does that; for a
		 * constraint whose name was in fact derived, emitting it is harmless,
		 * since the name derived on replay is the same one.
		 */
		typed_names = all_constraint_names(objectId);

		/* table-level NOT NULL constraints, as in the plain case below */
		foreach(lc, stmt->tableElts)
		{
			Node	   *elt = (Node *) lfirst(lc);

			if (IsA(elt, Constraint) &&
				((Constraint *) elt)->contype == CONSTR_NOTNULL)
			{
				deparse_TableNotNull(state, (Constraint *) elt);
				nelems++;
			}
		}
	}
	else
	{
		foreach(lc, stmt->tableElts)
		{
			Node	   *elt = (Node *) lfirst(lc);

			switch (nodeTag(elt))
			{
				case T_ColumnDef:
					if (typed)
						deparse_ColumnDef_typed(state, objectId, dpcontext,
												(ColumnDef *) elt);
					else
						deparse_ColumnDef(state, objectId, dpcontext,
										  (ColumnDef *) elt);
					nelems++;

					break;
				case T_Constraint:
					{
						Constraint *constraint = (Constraint *) elt;

						/*
						 * Raw table-level NOT NULL constraints are emitted
						 * from the parse tree; the others from pg_constraint
						 * below.
						 */
						if (constraint->contype == CONSTR_NOTNULL)
						{
							deparse_TableNotNull(state, constraint);
							nelems++;
						}
					}

					break;
				default:
					elog(ERROR, "invalid node type %d", nodeTag(elt));
			}
		}
	}

	nelems += deparse_Constraints(state, objectId, typed_names, cmds);

	end_jsonb_array(state);

	/*
	 * Decide whether to put '()' around the element list. Parens are needed
	 * when actual elements are present (eg: create table t1 (a int)), or in
	 * the inherit case even with no local elements (eg: create table t1 ()
	 * inherits (t2)); they are not needed for the 'partition of' or 'of type'
	 * cases with no local elements, where the list must expand to nothing.
	 */
	parenthesize = (stmt->partbound == NULL && !typed) || nelems > 0;

	/* leading space lives here; root references a bare %{table_elements}s */
	append_jsonb_pair1(state,
					   "fmt", jbv_str(parenthesize ? " (%{elements:, }s)" : "%{elements:, }s"));

	end_jsonb_object(state);
}

/*
 * Deparse DefElems, as used by Create Table
 */
static void
deparse_DefElem(JsonbInState *state, DefElem *elem, bool is_reset)
{
	begin_jsonb_object(state);

	/* fixed format string: the optional value slot is always referenced */
	append_jsonb_pair1(state, "fmt", jbv_str("%{label}s%{value}s"));

	/* LABEL: a possibly schema-qualified identifier */
	insert_jsonb_key(state, "label");
	begin_jsonb_object(state);
	append_jsonb_pair2(state,
					   "fmt", jbv_str("%{schema}s%{label}I"),
					   "label", jbv_str(elem->defname));

	if (elem->defnamespace != NULL)
		new_jsonb_string_clause(state, "schema", "%{schema_name}I.",
								"schema_name", elem->defnamespace);
	else
		new_jsonb_null(state, "schema");
	end_jsonb_object(state);

	/* VALUE (omitted for RESET) */
	if (!is_reset)
		new_jsonb_string_clause(state, "value", " = %{value_literal}L",
								"value_literal",
								elem->arg ? defGetString(elem) :
								defGetBoolean(elem) ? "true" : "false");
	else
		new_jsonb_null(state, "value");

	end_jsonb_object(state);
}

/*
 * Deparse WITH clause, as used by Create Table.
 */
static void
deparse_withObj(JsonbInState *state, CreateStmt *stmt)
{
	/* WITH */
	insert_jsonb_key(state, "with");
	begin_jsonb_array(state);

	/* add elements to array */
	foreach_node(DefElem, opt, stmt->options)
	{
		deparse_DefElem(state, opt, false);
	}

	/* with's array end */
	end_jsonb_array(state);
}

/*
 * Collect, from the raw parse tree, the set of constraint names the user
 * specified.
 */
static void
collect_raw_constraint_names(CreateStmt *stmt, List **typed_names)
{
	ListCell   *lc;

	*typed_names = NIL;

	foreach(lc, stmt->tableElts)
	{
		Node	   *elt = (Node *) lfirst(lc);
		List	   *constraints = NIL;

		if (IsA(elt, ColumnDef))
			constraints = ((ColumnDef *) elt)->constraints;
		else if (IsA(elt, Constraint))
			constraints = list_make1(elt);

		foreach_node(Constraint, constraint, constraints)
		{
			if (constraint->conname != NULL)
				*typed_names = lappend(*typed_names, constraint->conname);
		}
	}
}

/*
 * Find, in the list of commands collected for this query, the CREATE INDEX
 * subcommand that created the index with the given OID, and return its
 * IndexStmt.
 *
 * For an index-backed constraint of CREATE TABLE, this is the (merged)
 * IndexStmt that transformCreateStmt generated for it, so it records
 * exactly the clauses that execution honored -- notably whether a USING
 * INDEX TABLESPACE clause was in effect. Matching by index OID (through
 * pg_constraint.conindid) is immune to transformIndexConstraints having
 * merged redundant raw constraints, which any pairing based on the raw
 * parse tree is not.
 */
static IndexStmt *
find_collected_index_stmt(List *cmds, Oid indexId)
{
	ListCell   *lc;

	foreach(lc, cmds)
	{
		CollectedCommand *cmd = (CollectedCommand *) lfirst(lc);

		if (cmd->type == SCT_Simple &&
			cmd->parsetree != NULL &&
			IsA(cmd->parsetree, IndexStmt) &&
			cmd->d.simple.address.objectId == indexId)
			return (IndexStmt *) cmd->parsetree;
	}

	return NULL;
}

/*
 * Deparse a CreateStmt (CREATE TABLE).
 *
 * Given a table OID and the raw parse tree that created it, append the
 * JSONB object representing the creation command to the given state.
 *
 * Each clause that the user wrote is emitted as a sub-object carrying its
 * own "fmt"; clauses that the user did not write are emitted as JSON null.
 * The leading space of each clause lives in its own "fmt", so the fixed
 * top-level format string references bare %{x}s slots.
 */
static void
deparse_CreateStmt(JsonbInState *state, Oid objectId, CreateStmt *stmt,
				   List *cmds)
{
	Relation	relation = relation_open(objectId, AccessShareLock);
	Oid			nspid = relation->rd_rel->relnamespace;
	char	   *relname = RelationGetRelationName(relation);
	List	   *dpcontext;
	List	   *typed_names;
	char	   *perstr;
	bool		typed = stmt->ofTypename != NULL || stmt->partbound != NULL;

	collect_raw_constraint_names(stmt, &typed_names);

	/* mark the begin of the command object and start adding elements */
	begin_jsonb_object(state);

	/* fixed format string: every clause slot is always referenced */
	append_jsonb_pair1(state, "fmt",
					   jbv_str("CREATE%{persistence}s TABLE%{if_not_exists}s %{identity}D"
							   "%{of_type}s%{partition_of}s%{table_elements}s%{inherits}s"
							   "%{partition_bound}s%{partition_by}s%{access_method}s"
							   "%{with_clause}s%{on_commit}s%{tablespace}s"));

	/* PERSISTENCE (TEMPORARY/UNLOGGED only; permanent has no keyword) */
	perstr = get_persistence_str(stmt->relation->relpersistence);
	if (perstr != NULL)
		new_jsonb_string_clause(state, "persistence", " %{persistence_type}s",
								"persistence_type", perstr);
	else
		new_jsonb_null(state, "persistence");

	/* IF NOT EXISTS */
	if (stmt->if_not_exists)
		new_jsonb_clause(state, "if_not_exists", " IF NOT EXISTS");
	else
		new_jsonb_null(state, "if_not_exists");

	/* IDENTITY (table name) -- required, referenced bare as %{identity}D */
	new_jsonb_for_qualname(state, nspid, relname, "identity", true);

	dpcontext = deparse_context_for(relname, objectId);

	/* OF type / PARTITION OF parent (mutually exclusive) */

	if (stmt->ofTypename)
	{
		Assert(!stmt->partbound);

		begin_jsonb_clause(state, "of_type", " OF %{type}T");
		new_jsonb_for_type(state, "type", relation->rd_rel->reloftype, -1);
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "of_type");

	if (stmt->partbound)
	{
		List	   *parents = get_inheritance_parents(objectId);

		Assert(list_length(parents) == 1);
		Assert(!stmt->ofTypename);

		begin_jsonb_clause(state, "partition_of", " PARTITION OF %{parent}D");
		new_jsonb_for_qualname_id(state, RelationRelationId,
								  linitial_oid(parents), "parent", true);
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "partition_of");

	/*
	 * TABLE ELEMENTS (structural; always emitted, parenthesization decided
	 * inside).
	 */
	add_table_elems(state, objectId, stmt, dpcontext, typed_names,
					cmds, typed);

	/*
	 * INHERITS. We cannot simply use the list of parents from the parse stmt,
	 * because that may lack the qualified names of the parent relations, and
	 * we deparse under a restricted search_path. Grab the resolved parents
	 * from pg_inherits instead.
	 */
	if (stmt->inhRelations != NIL && stmt->partbound == NULL)
	{
		begin_jsonb_clause(state, "inherits", " INHERITS (%{parents:, }D)");
		new_jsonbArray_for_qualname_id(state, "parents",
									   get_inheritance_parents(objectId));
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "inherits");

	/*
	 * FOR VALUES (partition bound). Get pg_class.relpartbound; we cannot use
	 * partbound in the parse tree directly as it's the untransformed
	 * expression.
	 */
	if (stmt->partbound != NULL)
		new_jsonb_string_clause(state, "partition_bound", " %{spec}s",
								"spec", get_partbound_spec_string(objectId));
	else
		new_jsonb_null(state, "partition_bound");

	/* PARTITION BY */
	if (stmt->partspec != NULL)
		new_jsonb_string_clause(state, "partition_by",
								" PARTITION BY %{definition}s", "definition",
								TextDatumGetCString(DirectFunctionCall1(pg_get_partkeydef,
																		ObjectIdGetDatum(objectId))));
	else
		new_jsonb_null(state, "partition_by");

	/* USING access method: only if the user wrote it; the value as typed */
	if (stmt->accessMethod != NULL)
		new_jsonb_string_clause(state, "access_method",
								" USING %{access_method_name}I",
								"access_method_name", stmt->accessMethod);
	else
		new_jsonb_null(state, "access_method");

	/* WITH (...): only the options the user wrote, as typed */
	if (stmt->options)
	{
		begin_jsonb_clause(state, "with_clause", " WITH (%{with:, }s)");
		deparse_withObj(state, stmt);
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "with_clause");

	/* ON COMMIT */
	if (stmt->oncommit != ONCOMMIT_NOOP)
	{
		char	   *oncommit_value;

		switch (stmt->oncommit)
		{
			case ONCOMMIT_PRESERVE_ROWS:
				oncommit_value = "PRESERVE ROWS";
				break;
			case ONCOMMIT_DELETE_ROWS:
				oncommit_value = "DELETE ROWS";
				break;
			case ONCOMMIT_DROP:
				oncommit_value = "DROP";
				break;
			default:
				elog(ERROR, "unexpected oncommit value %d", stmt->oncommit);
		}

		new_jsonb_string_clause(state, "on_commit",
								" ON COMMIT %{on_commit_value}s",
								"on_commit_value", oncommit_value);
	}
	else
		new_jsonb_null(state, "on_commit");

	/* TABLESPACE */
	if (stmt->tablespacename != NULL)
		new_jsonb_string_clause(state, "tablespace",
								" TABLESPACE %{tablespace_name}I",
								"tablespace_name", stmt->tablespacename);
	else
		new_jsonb_null(state, "tablespace");

	relation_close(relation, AccessShareLock);

	/* Mark the end of the command object */
	end_jsonb_object(state);
}

/*
 * Can the given raw statement be deparsed by deparse_ddl_command()?
 *
 * This is a pure parse-tree check that performs no catalog access, so it
 * can be used before the statement is executed.
 *
 * Every caller of deparse_ddl_command() must ask this first and decide for
 * itself what to do with a command that cannot be reproduced -- execute it
 * unchanged, warn, or refuse it. That policy differs between consumers and
 * so does not belong here; the deparser only answers whether it can.
 *
 * The answer must err towards false: a command reported as supported and
 * then deparsed into something that does not reproduce it is a silent
 * corruption of whatever the consumer does with the result, while a command
 * needlessly reported as unsupported merely goes unhandled.
 */
bool
ddl_deparse_command_supported(const Node *parsetree)
{
	ListCell   *lc;

	if (parsetree == NULL || nodeTag(parsetree) != T_CreateStmt)
		return false;

	/*
	 * A LIKE clause is expanded against the source table before the statement
	 * executes. What it contributes to the table itself is recovered from the
	 * catalogs (see deparse_ColumnDef_like), but the options below each copy
	 * an object that needs a statement of its own -- CREATE INDEX, COMMENT
	 * ON, CREATE STATISTICS -- and none of those is deparsable yet.
	 */
	foreach(lc, ((const CreateStmt *) parsetree)->tableElts)
	{
		if (IsA(lfirst(lc), TableLikeClause) &&
			(((TableLikeClause *) lfirst(lc))->options &
			 (CREATE_TABLE_LIKE_INDEXES |
			  CREATE_TABLE_LIKE_COMMENTS |
			  CREATE_TABLE_LIKE_STATISTICS)))
			return false;
	}

	return true;
}

/*
 * Deparse a DDL command into a JSON blob.
 *
 * original_parsetree is the raw (untransformed) parse tree of the command;
 * cmds is the list of CollectedCommand its execution produced (see
 * ddl_collect.c), which supplies the OID of the created object. The result
 * is a JSON envelope of the form
 *
 *		{ "tag": <command tag>, "command": { <fmt object> } }
 *
 * that deparse_ddl_json_to_string() can expand back into a (single) plain
 * command. Sibling elements of "command" may be added in the future (e.g.
 * the user that ran the command). Information provided by the transport
 * layer of an eventual consumer (such as the transaction ID or origin, for
 * logical replication) deliberately has no place here.
 *
 * Returns NULL when the command left nothing to reproduce: an IF [NOT]
 * EXISTS guard that did nothing, or a command run by an extension script.
 * That is an ordinary outcome, not a refusal, and the caller has nothing to
 * do about it.
 *
 * A command the deparser cannot reproduce is a different matter, and is
 * reported as an error rather than as a NULL that a caller might mistake for
 * the case above. Callers are expected to have asked
 * ddl_deparse_command_supported() beforehand and to have applied their own
 * policy to the answer, so reaching that error means the caller did not.
 *
 * The result is allocated in the caller's memory context.
 */
char *
deparse_ddl_command(const Node *original_parsetree, List *cmds)
{
	MemoryContext oldcxt;
	MemoryContext tmpcxt;
	Oid			objectId = InvalidOid;
	char	   *command = NULL;
	StringInfoData str;
	Jsonb	   *jsonb;
	ListCell   *lc;
	int			save_nestlevel;
	JsonbInState state = {0};

	/*
	 * Asking for a command we cannot reproduce is a caller error: the answer
	 * would otherwise have to be indistinguishable from "there was nothing to
	 * reproduce" below, and a consumer that cannot tell the two apart drops
	 * the command silently.
	 */
	if (!ddl_deparse_command_supported(original_parsetree))
		elog(ERROR, "cannot deparse unsupported command of node type %d",
			 (int) nodeTag(original_parsetree));

	/*
	 * Find the object created by this command. The transformed parse trees in
	 * the collected commands are used only for this; all deparsing works on
	 * the raw tree and the catalogs.
	 */
	foreach(lc, cmds)
	{
		CollectedCommand *cmd = (CollectedCommand *) lfirst(lc);

		if (cmd->type != SCT_Simple || cmd->parsetree == NULL ||
			!IsA(cmd->parsetree, CreateStmt))
			continue;

		/* Commands run by extension scripts are not to be reproduced */
		if (cmd->in_extension)
			return NULL;

		objectId = cmd->d.simple.address.objectId;
		break;
	}

	/* Nothing was created (e.g. IF NOT EXISTS on an existing table) */
	if (!OidIsValid(objectId))
		return NULL;

	/*
	 * Allocate everything done by the deparsing routines into a temp context,
	 * to avoid having to sprinkle them with memory handling code, but
	 * allocate the output StringInfo before switching.
	 */
	initStringInfo(&str);
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "deparse ctx",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/*
	 * Many routines underlying this one invoke ruleutils.c to deparse
	 * expressions, and we want all object names in those results to be
	 * schema-qualified so the output is portable across search_path settings.
	 * Restrict the search path centrally here rather than at each call site.
	 */
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/* Build the envelope */
	begin_jsonb_object(&state);
	append_jsonb_pair1(&state, "tag",
					   jbv_str(CreateCommandName((Node *) original_parsetree)));
	insert_jsonb_key(&state, "command");
	deparse_CreateStmt(&state, objectId, (CreateStmt *) original_parsetree,
					   cmds);
	end_jsonb_object(&state);

	jsonb = JsonbValueToJsonb(state.result);

	AtEOXact_GUC(true, save_nestlevel);

	command = JsonbToCString(&str, &jsonb->root, JSONB_ESTIMATED_LEN);

	/*
	 * Clean up. Note that since we created the StringInfo in the caller's
	 * context, the output string is not deleted here.
	 */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);

	return command;
}
