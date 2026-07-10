/*-------------------------------------------------------------------------
 *
 * ddl_deparse.c
 *	  Functions to convert utility commands to machine-parseable
 *	  representation
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * This provides JSON blobs representing DDL commands, which ddl_json.c
 * expands back into plain strings.  A consumer can edit the JSON before
 * expanding it, e.g. to change a name.
 *
 * The input is the raw parse tree of the command, plus the list of
 * CollectedCommand its execution produced.  A clause the user did not write
 * is not emitted, so that a replay adopts the defaults in effect where it
 * runs.  What the command itself fixed, such as a constraint name copied by
 * LIKE, is emitted even if the user did not write it.  Names are
 * schema-qualified, and expressions and constraint definitions come from
 * ruleutils.c.
 *
 * XXX: Only CREATE TABLE, ALTER TABLE and DROP TABLE are supported; see
 * ddl_deparse_command_supported().
 *
 * Much of the information in the output blob actually comes from system
 * catalogs, not from the command parse node, as it is impossible to reliably
 * construct a fully-specified command (i.e. one not dependent on search_path
 * etc) looking only at the parse node.  Normalizations that change the
 * spelling but not the effect are allowed.  Type names are spelled
 * canonically, and constraints given with a column definition are emitted as
 * table constraints.
 *
 * XXX: This module supports CREATE TABLE.  LIKE options that copy indexes,
 * comments or extended statistics are not supported, since those would need
 * CREATE INDEX etc. to be deparsed too.  See ddl_deparse_command_supported()
 * for the statements covered.
 *
 * XXX: For LIKE clause, DDL deparse expands the LIKE clause to the actual
 * columns, to allow the deparsed DDL to be replayed in a different
 * environment.  However, it might be worth considering an option to preserve
 * the user intent and simply reproduce the LIKE clause in the deparsed DDL.
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
#include "catalog/partition.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "commands/ddl_deparse.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "tcop/deparse_utility.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
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
 * Value constructors for new_jsonb_VA().
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
 * parse state.  Values are built with jbv_str()/jbv_bool()/jbv_null().
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
 * append_jsonb_pairN(state, k1, v1, ...) appends N (key, JsonbValue) pairs,
 * whose values are built with jbv_*().
 */
#define append_jsonb_pair1(state, k1, v1)	\
	new_jsonb_VA(state, 1, (k1), (v1))
#define append_jsonb_pair2(state, k1, v1, k2, v2)	\
	new_jsonb_VA(state, 2, (k1), (v1), (k2), (v2))
#define append_jsonb_pair3(state, k1, v1, k2, v2, k3, v3)	\
	new_jsonb_VA(state, 3, (k1), (v1), (k2), (v2), (k3), (v3))
#define append_jsonb_pair4(state, k1, v1, k2, v2, k3, v3, k4, v4)	\
	new_jsonb_VA(state, 4, (k1), (v1), (k2), (v2), (k3), (v3), (k4), (v4))

/* Append a JSON null under "key" */
static inline void
new_jsonb_null(JsonbInState *state, char *key)
{
	append_jsonb_pair1(state, key, jbv_null());
}

/*
 * Wrappers around pushJsonbValue() for object and array delimiters.
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
 * Open a clause sub-object under "key", with the given "fmt".  The caller adds
 * the clause's other elements and calls end_jsonb_clause().
 *
 * A clause that is not to be emitted is stored as a JSON null instead (see
 * new_jsonb_null).  The leading space of a clause is in its own "fmt", so
 * enclosing format strings reference bare %{x}s slots.
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
 * Emit a clause sub-object holding a "fmt" and one string value under valkey.
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
 * Add an object with the schemaname and objname of a qualified name, under
 * keyName if it is not NULL.
 *
 * If the namespace OID corresponds to a temp schema, that's set to
 * "pg_temp".
 */
static void
new_jsonb_for_qualname(JsonbInState *state, Oid nspid, char *objName,
					   char *keyName)
{
	char	   *namespace;

	if (isAnyTempNamespace(nspid))
		namespace = pstrdup("pg_temp");
	else
	{
		namespace = get_namespace_name(nspid);
		if (namespace == NULL)
			elog(ERROR, "cache lookup failed for namespace %u", nspid);
	}

	/* Push the key first */
	if (keyName)
		insert_jsonb_key(state, keyName);

	begin_jsonb_object(state);
	append_jsonb_pair2(state,
					   "schemaname", jbv_str(namespace),
					   "objname", jbv_str(objName));
	end_jsonb_object(state);
}

/*
 * As new_jsonb_for_qualname(), for the object with the given classId and
 * objectId.
 */
static void
new_jsonb_for_qualname_id(JsonbInState *state, Oid classId, Oid objectId,
						  char *keyName)
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
						   keyName);
	table_close(catalog, AccessShareLock);
}

/*
 * Insert an array of qualified relation names, given as a list of OIDs, under
 * keyname.
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
								  NULL);

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
 * spelling as the user typed it.  Otherwise return NULL.
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
 * definition, or NULL if there is none.  If there are several, prefer a named
 * one, since its name is the one that ends up in the catalogs (e.g. "NOT NULL
 * CONSTRAINT foo NOT NULL").
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
 * Return the deparsed default or generation expression of a column, as stored
 * in pg_attrdef.  A DEFAULT whose expression is a plain NULL constant is not
 * stored; with missing_ok, NULL is returned for it.
 *
 * We don't use build_column_default(), which can return something other than
 * what the command stored, such as the type's default.
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
 * Return the inheritance parents of a relation in inhseqno order, which is the
 * order the command gave them in.
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

/* Return the deparsed partition bound of a relation, from relpartbound. */
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
 * Render a sequence option of a GENERATED AS IDENTITY column as the user wrote
 * it.  This must handle everything gram.y's SeqOptElem admits, except OWNED
 * BY, which the caller filters out, and AS, which identity columns reject.
 */
static char *
deparse_SeqOptElem(DefElem *elem)
{
	if (strcmp(elem->defname, "cache") == 0)
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
 * Deparse a GENERATED ... AS IDENTITY clause, with the sequence options the
 * user wrote.
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
	 * Omit OWNED BY.  Execution makes the identity column own the sequence
	 * regardless, and the name it gives need not be schema-qualified.
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
 * Emit the DEFAULT clause of a column.  A default that was not stored is
 * emitted as NULL.
 */
static void
deparse_column_default(JsonbInState *state, Oid relid, AttrNumber attnum,
					   List *dpcontext)
{
	char	   *defstr = get_stored_default_expr(relid, attnum, dpcontext, true);

	new_jsonb_string_clause(state, "default", " DEFAULT %{default_expr}s",
							"default_expr", defstr != NULL ? defstr : "NULL");
}

/* Emit the GENERATED ... AS (expr) clause of a generated column. */
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
 * Format string of a column definition, shared by deparse_ColumnDef() and
 * deparse_ColumnDef_like() so that both produce the same shape.
 */
#define COLUMN_DEF_FMT \
	"%{name}I %{coltype}T%{storage}s%{compression}s%{collation}s" \
	"%{not_null}s%{default}s%{identity_column}s%{generated_column}s"

/*
 * Deparse a ColumnDef of a regular (non-typed) table.  Only the clauses the
 * user wrote are emitted.  NOT NULL is emitted here; the other constraints are
 * emitted from pg_constraint, as table constraints.
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

	append_jsonb_pair1(state, "fmt", jbv_str(COLUMN_DEF_FMT));

	/* NAME and TYPE (always present, referenced bare) */
	append_jsonb_pair2(state,
					   "name", jbv_str(coldef->colname),
					   "type", jbv_str("column"));

	/*
	 * COLUMN TYPE.  A serial column is emitted as written, so that the replay
	 * creates its own sequence.  Other types come from the catalog.
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

	/* STORAGE: the value as typed */
	if (coldef->storage_name != NULL)
		new_jsonb_string_clause(state, "storage", " STORAGE %{colstorage}I",
								"colstorage", coldef->storage_name);
	else
		new_jsonb_null(state, "storage");

	/* COMPRESSION, as written.  "default" survives quoting by %I. */
	if (coldef->compression != NULL)
		new_jsonb_string_clause(state, "compression",
								" COMPRESSION %{compression_method}I",
								"compression_method", coldef->compression);
	else
		new_jsonb_null(state, "compression");

	/* COLLATE: the resolved collation, not the name as typed */
	if (coldef->collClause != NULL)
	{
		begin_jsonb_clause(state, "collation", " COLLATE %{collation_name}D");
		new_jsonb_for_qualname_id(state, CollationRelationId,
								  attrForm->attcollation, "collation_name");
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "collation");

	/*
	 * NOT NULL, from the raw definition only.  One that a PRIMARY KEY or a
	 * serial type implies is derived again on replay.
	 */
	constraint = find_column_constraint(coldef, CONSTR_NOTNULL);
	if (constraint != NULL)
		deparse_ColumnNotNull(state, "not_null", constraint);
	else
		new_jsonb_null(state, "not_null");

	/*
	 * DEFAULT.  A serial column has none in the raw tree, so its nextval()
	 * default is not emitted.
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
 * Deparse a column that a LIKE clause contributed.  It has no raw definition,
 * so everything comes from the catalogs, and a clause is emitted where the
 * column differs from what its type gives by default.
 */
static void
deparse_ColumnDef_like(JsonbInState *state, Oid relid, List *dpcontext,
					   Form_pg_attribute attrForm)
{
	AttrNumber	attnum = attrForm->attnum;

	begin_jsonb_object(state);

	append_jsonb_pair1(state, "fmt", jbv_str(COLUMN_DEF_FMT));

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
								  attrForm->attcollation, "collation_name");
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "collation");

	/*
	 * NOT NULL, with the name that LIKE copied, which the replay would not
	 * derive.  Not if only the parent the column merged with has it, since
	 * INHERITS re-derives that one.
	 */
	if (attrForm->attnotnull)
	{
		Constraint *constraint = makeNode(Constraint);
		HeapTuple	contup = findNotNullConstraintAttnum(relid, attnum);
		bool		local = true;

		constraint->contype = CONSTR_NOTNULL;
		if (HeapTupleIsValid(contup))
		{
			Form_pg_constraint conForm;

			conForm = (Form_pg_constraint) GETSTRUCT(contup);
			constraint->conname = pstrdup(NameStr(conForm->conname));
			constraint->is_no_inherit = conForm->connoinherit;
			local = conForm->conislocal;
			heap_freetuple(contup);
		}

		if (local)
			deparse_ColumnNotNull(state, "not_null", constraint);
		else
			new_jsonb_null(state, "not_null");
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
 * Deparse a ColumnDef of a typed table or a partition, whose type comes from
 * the composite type or the parent.
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

	/* DEFAULT */
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

/* Deparse a table-level NOT NULL constraint the user wrote. */
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
 * If def ends with suffix, remove it and return true; otherwise return false.
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
 * Split the attribute trailer (DEFERRABLE etc.) off the pg_get_constraintdef
 * output for an index-backed constraint, and return it, or NULL if there is
 * none.  The trailer is rebuilt from the catalog flags as
 * pg_get_constraintdef_worker builds it; a mismatch is an internal error.  An
 * index-backed constraint can't be NOT ENFORCED or NOT VALID.
 */
static char *
split_constraint_attributes(char *def, Form_pg_constraint constrForm)
{
	StringInfoData trailer;

	Assert(constrForm->conenforced && constrForm->convalidated);

	initStringInfo(&trailer);

	if (constrForm->condeferrable)
		appendStringInfoString(&trailer, " DEFERRABLE");

	if (constrForm->condeferred)
		appendStringInfoString(&trailer, " INITIALLY DEFERRED");

	if (trailer.len == 0)
		return NULL;

	if (!strip_suffix(def, trailer.data))
		elog(ERROR, "unexpected format of constraint definition \"%s\"", def);

	return trailer.data;
}

/*
 * Split the predicate (" WHERE (...)") off an exclusion constraint definition
 * whose trailer has been removed, and return it, or NULL.  The text is
 * regenerated with pg_get_expr(), which uses the same context and pretty flags
 * as pg_get_constraintdef().
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
 * Emit a constraint, given by OID, as a JSON constraint object.  Used for
 * CREATE TABLE and ALTER TABLE ADD CONSTRAINT.
 *
 * The definition comes from pg_get_constraintdef().  For an index-backed
 * constraint, the clauses it omits or puts in the wrong place (index storage
 * parameters and USING INDEX TABLESPACE) come from the IndexStmt that created
 * the index, idxstmt_override or a CREATE INDEX command in cmds, and are
 * emitted only if the user wrote them.
 *
 * The name is emitted only if it is in typed_names, the names the user wrote.
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
	 * subcommand that created its index.  It records the clauses that
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
	 * Split off the trailing clauses, so that USING INDEX TABLESPACE can be
	 * emitted before them, where the grammar wants it.
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
	 * WITH (storage parameters), if the user wrote them.  For EXCLUDE,
	 * pg_get_constraintdef() already includes them.
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
	 * The split-off texts go into value slots, so that the predicate is not
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
 * Emit the table constraints of a new table from pg_constraint, and return how
 * many.  NOT NULL constraints are emitted from the raw tree instead.
 * typed_names and cmds are as for deparse_one_constraint().
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
	 * Emit the constraints in OID order, which is the order the command
	 * created them in, so that the replay creates them in the same order.
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

/* Emit the table elements of a CREATE TABLE: columns and constraints. */
static void
add_table_elems(JsonbInState *state, Oid objectId, CreateStmt *stmt,
				List *dpcontext, List *cmds, bool typed)
{
	bool		parenthesize;
	int			nelems = 0;
	List	   *typed_names = NIL;
	ListCell   *lc;

	insert_jsonb_key(state, "table_elements");
	begin_jsonb_object(state);

	/*
	 * Columns and table-level NOT NULL constraints come from the raw tree,
	 * the other constraints from pg_constraint.
	 */
	insert_jsonb_key(state, "elements");
	begin_jsonb_array(state);

	if (has_like_clause(stmt->tableElts))
	{
		Relation	rel;
		TupleDesc	tupdesc;

		/*
		 * LIKE contributed columns that the raw statement lacks, so walk the
		 * columns in attribute order.  A column the user wrote is still
		 * deparsed from its raw definition.
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
		 * LIKE copies constraint names verbatim, so emit every name.  A
		 * derived one comes out the same on replay anyway.
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
		/* Also collect the constraint names the user wrote */
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

					foreach_node(Constraint, constraint,
								 ((ColumnDef *) elt)->constraints)
					{
						if (constraint->conname != NULL)
							typed_names = lappend(typed_names,
												  constraint->conname);
					}
					break;
				case T_Constraint:
					{
						Constraint *constraint = (Constraint *) elt;

						if (constraint->conname != NULL)
							typed_names = lappend(typed_names,
												  constraint->conname);

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
	 * Parenthesize the element list, unless it is empty and the table is a
	 * partition or typed table, where it must expand to nothing.  INHERITS
	 * needs the parentheses even then.
	 */
	parenthesize = (stmt->partbound == NULL && !typed) || nelems > 0;

	/* leading space lives here; root references a bare %{table_elements}s */
	append_jsonb_pair1(state,
					   "fmt", jbv_str(parenthesize ? " (%{elements:, }s)" : "%{elements:, }s"));

	end_jsonb_object(state);
}

/*
 * Deparse one DefElem of a WITH, SET or RESET option list.  For RESET
 * (is_reset), only the name is emitted.
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
 * Return the IndexStmt of the CREATE INDEX command in cmds that created the
 * given index, or NULL.  For CREATE TABLE, that is the IndexStmt
 * transformCreateStmt generated after merging redundant index constraints, so
 * it shows which clauses execution honored.
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
 * Given a table OID and the raw parse tree that created it, append the JSONB
 * object representing the creation command to the given state.
 */
static void
deparse_CreateStmt(JsonbInState *state, Oid objectId, CreateStmt *stmt,
				   List *cmds)
{
	Relation	relation = relation_open(objectId, AccessShareLock);
	Oid			nspid = relation->rd_rel->relnamespace;
	char	   *relname = RelationGetRelationName(relation);
	List	   *dpcontext;
	char	   *perstr;
	bool		typed = stmt->ofTypename != NULL || stmt->partbound != NULL;

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

	/* IDENTITY (table name), referenced bare as %{identity}D */
	new_jsonb_for_qualname(state, nspid, relname, "identity");

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
		Assert(!stmt->ofTypename);

		begin_jsonb_clause(state, "partition_of", " PARTITION OF %{parent}D");
		new_jsonb_for_qualname_id(state, RelationRelationId,
								  get_partition_parent(objectId, false),
								  "parent");
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "partition_of");

	/* TABLE ELEMENTS */
	add_table_elems(state, objectId, stmt, dpcontext, cmds, typed);

	/* INHERITS, from pg_inherits, since the raw names may not be qualified. */
	if (stmt->inhRelations != NIL && stmt->partbound == NULL)
	{
		begin_jsonb_clause(state, "inherits", " INHERITS (%{parents:, }D)");
		new_jsonbArray_for_qualname_id(state, "parents",
									   get_inheritance_parents(objectId));
		end_jsonb_clause(state);
	}
	else
		new_jsonb_null(state, "inherits");

	/* FOR VALUES, from relpartbound, since the raw bound is untransformed. */
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

	/* USING access method, as written */
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
 * This looks only at the parse tree, so it can be called before the statement
 * is executed.  The caller decides what to do with a statement we can't
 * deparse.  When in doubt, return false: deparsing a command wrongly is worse
 * than not deparsing it.
 */
bool
ddl_deparse_command_supported(const Node *parsetree)
{
	ListCell   *lc;

	if (parsetree == NULL || nodeTag(parsetree) != T_CreateStmt)
		return false;

	/*
	 * A LIKE clause is expanded against the source table before the statement
	 * executes.  What it contributes to the table itself is recovered from
	 * the catalogs (see deparse_ColumnDef_like).  The options below each copy
	 * an object that needs a statement of its own, namely CREATE INDEX,
	 * COMMENT ON and CREATE STATISTICS, and none of those is deparsable yet.
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
 * original_parsetree is the raw (untransformed) parse tree of the command.
 * cmds is the list of CollectedCommand its execution produced, which supplies
 * the OID of the created object.  The two must come from the same complete
 * query, as they do when both are taken from the event trigger state by the
 * same event trigger.  The result
 * is a JSON envelope of the form
 *
 *		{ "tag": <command tag>, "command": { <fmt object> } }
 *
 * The result is a JSON object with the command tag under "tag" and the
 * command under "command", which deparse_ddl_json_to_string() expands back
 * into one command.  Names in it are schema-qualified except those in
 * pg_catalog, so it must be executed with pg_catalog first in the search_path,
 * as search_path = '' does.
 *
 * Returns NULL if the command did nothing, because IF NOT EXISTS found the
 * table already there.  Also returns NULL for a command run by an extension
 * script.  It is an error to pass a command for which
 * ddl_deparse_command_supported() returns false.
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

	/* Callers must check ddl_deparse_command_supported() first */
	if (!ddl_deparse_command_supported(original_parsetree))
		elog(ERROR, "cannot deparse unsupported command of node type %d",
			 (int) nodeTag(original_parsetree));

	/*
	 * Find the object created by this command.  The transformed parse trees
	 * in the collected commands are used only for this.  All deparsing works
	 * on the raw tree and the catalogs.
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

	/* Do the work in a temporary context, except for the output string. */
	initStringInfo(&str);
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "deparse ctx",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* Have ruleutils schema-qualify the names in expressions. */
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

	/* Clean up; the output string is in the caller's context. */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);

	return command;
}
