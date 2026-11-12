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
 * XXX: A LIKE clause is expanded into the columns it produced, so that the
 * replay doesn't need the source table.  An option to reproduce the clause
 * itself might be worth having.
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
#include "catalog/dependency.h"
#include "catalog/objectaddress.h"
#include "catalog/partition.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "commands/ddl_deparse.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "tcop/deparse_utility.h"
#include "tcop/utility.h"
#include "utils/acl.h"
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
	char	   *type_name;
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
 * Return the deparsed default or generation expression of a column.  sub is
 * the collected ADD COLUMN subcommand, or NULL for CREATE TABLE.  For ADD
 * COLUMN, use the expression the subcommand saved, since a later subcommand
 * may have replaced the stored one.  missing_ok is as for
 * get_stored_default_expr().
 */
static char *
get_column_default_expr(Oid relid, AttrNumber attnum, List *dpcontext,
						const CollectedATSubcmd *sub, bool missing_ok)
{
	if (sub == NULL)
		return get_stored_default_expr(relid, attnum, dpcontext, missing_ok);

	if (sub->default_expr == NULL)
	{
		if (!missing_ok)
			elog(ERROR, "no default value collected for attribute %d of relation %u",
				 attnum, relid);
		return NULL;
	}

	return deparse_expression(sub->default_expr, dpcontext, false, false);
}

/*
 * Emit the DEFAULT clause of a column.  A default that was not stored is
 * emitted as NULL.
 */
static void
deparse_column_default(JsonbInState *state, Oid relid, AttrNumber attnum,
					   List *dpcontext, const CollectedATSubcmd *sub)
{
	char	   *defstr = get_column_default_expr(relid, attnum, dpcontext, sub,
												 true);

	new_jsonb_string_clause(state, "default", " DEFAULT %{default_expr}s",
							"default_expr", defstr != NULL ? defstr : "NULL");
}

/* Emit the GENERATED ... AS (expr) clause of a generated column. */
static void
deparse_column_generated(JsonbInState *state, Oid relid, AttrNumber attnum,
						 List *dpcontext, const CollectedATSubcmd *sub,
						 Constraint *constraint)
{
	char	   *fmt = (constraint->generated_kind == ATTRIBUTE_GENERATED_VIRTUAL) ?
		" GENERATED ALWAYS AS (%{generation_expr}s) VIRTUAL" :
		" GENERATED ALWAYS AS (%{generation_expr}s) STORED";

	new_jsonb_string_clause(state, "generated_column", fmt, "generation_expr",
							get_column_default_expr(relid, attnum, dpcontext,
													sub, false));
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
				  ColumnDef *coldef, const CollectedATSubcmd *sub)
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
		deparse_column_default(state, relid, attrForm->attnum, dpcontext, sub);
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
								 sub, constraint);
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
		deparse_column_default(state, relid, attnum, dpcontext, NULL);
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

		deparse_column_generated(state, relid, attnum, dpcontext, NULL,
								 constraint);
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
		deparse_column_default(state, relid, attrForm->attnum, dpcontext,
							   NULL);
	else
		new_jsonb_null(state, "default");

	/* GENERATED ... AS ( expr ), e.g. overridden in a partition */
	constraint = find_column_constraint(coldef, CONSTR_GENERATED);
	if (constraint != NULL)
		deparse_column_generated(state, relid, attrForm->attnum, dpcontext,
								 NULL, constraint);
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
					   IndexStmt *idxstmt_override, List *typed_names)
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
	 * The IndexStmt that created the index: the subcommand's own for ALTER
	 * TABLE ADD, a collected CREATE INDEX for CREATE TABLE.
	 */
	if (constrForm->contype == CONSTRAINT_PRIMARY ||
		constrForm->contype == CONSTRAINT_UNIQUE ||
		constrForm->contype == CONSTRAINT_EXCLUSION)
	{
		idxstmt = idxstmt_override != NULL ? idxstmt_override :
			find_collected_index_stmt(cmds, constrForm->conindid);

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

	for (int i = 0; i < nconstraints; i++)
		deparse_one_constraint(state, conoids[i], cmds, NULL, typed_names);

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
				deparse_ColumnDef(state, objectId, dpcontext, raw, NULL);
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
										  (ColumnDef *) elt, NULL);
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
 * Return the raw ADD COLUMN definition of the given column.  The collected one
 * has had its constraints and defaults moved out.
 */
static ColumnDef *
find_raw_add_column(AlterTableStmt *rawstmt, const char *colname)
{
	ListCell   *lc;

	foreach(lc, rawstmt->cmds)
	{
		AlterTableCmd *rawcmd = lfirst_node(AlterTableCmd, lc);

		if (rawcmd->subtype == AT_AddColumn &&
			rawcmd->def != NULL &&
			IsA(rawcmd->def, ColumnDef) &&
			strcmp(((ColumnDef *) rawcmd->def)->colname, colname) == 0)
			return (ColumnDef *) rawcmd->def;
	}

	elog(ERROR, "could not find raw ADD COLUMN for column \"%s\"", colname);
	return NULL;				/* keep compiler quiet */
}

/*
 * Return the raw ALTER COLUMN subcommand of the given subtype for the named
 * column.  Parse analysis moves the sequence options of ADD/SET IDENTITY into
 * an internal ALTER SEQUENCE, so only the raw subcommand has them as written.
 *
 * A column can appear in several such subcommands.  *matched holds the raw
 * subcommands already returned, which are skipped.
 */
static AlterTableCmd *
find_raw_alter_column(AlterTableStmt *rawstmt, AlterTableType subtype,
					  const char *colname, List **matched)
{
	ListCell   *lc;

	foreach(lc, rawstmt->cmds)
	{
		AlterTableCmd *rawcmd = lfirst_node(AlterTableCmd, lc);

		if (rawcmd->subtype == subtype &&
			rawcmd->name != NULL &&
			strcmp(rawcmd->name, colname) == 0 &&
			!list_member_ptr(*matched, rawcmd))
		{
			*matched = lappend(*matched, rawcmd);
			return rawcmd;
		}
	}

	elog(ERROR, "could not find raw ALTER COLUMN subcommand for column \"%s\"",
		 colname);
	return NULL;				/* keep compiler quiet */
}

/*
 * Return the constraint names the user wrote in the raw ALTER TABLE, including
 * those given with ADD COLUMN.  The collected subcommands can't tell, since
 * execution fills a derived name into the node that is collected.
 */
static List *
collect_raw_alter_constraint_names(AlterTableStmt *rawstmt)
{
	List	   *names = NIL;
	ListCell   *lc;

	foreach(lc, rawstmt->cmds)
	{
		AlterTableCmd *rawcmd = lfirst_node(AlterTableCmd, lc);
		List	   *constraints = NIL;
		ListCell   *lc2;

		if (rawcmd->def == NULL)
			continue;

		if (rawcmd->subtype == AT_AddConstraint &&
			IsA(rawcmd->def, Constraint))
			constraints = list_make1(rawcmd->def);
		else if (rawcmd->subtype == AT_AddColumn &&
				 IsA(rawcmd->def, ColumnDef))
			constraints = ((ColumnDef *) rawcmd->def)->constraints;

		foreach(lc2, constraints)
		{
			Constraint *constraint = lfirst_node(Constraint, lc2);

			if (constraint->conname != NULL)
				names = lappend(names, constraint->conname);
		}
	}

	return names;
}

/*
 * Return the raw table-level NOT NULL the user wrote for the given column, or
 * NULL if none is left.  ADD COLUMN ... NOT NULL and ADD PRIMARY KEY generate
 * NOT NULL constraints of their own, so *matched, as in find_raw_alter_column,
 * keeps a raw one from being emitted twice.
 */
static Constraint *
find_raw_table_notnull(AlterTableStmt *rawstmt, const char *colname,
					   List **matched)
{
	ListCell   *lc;

	foreach(lc, rawstmt->cmds)
	{
		AlterTableCmd *rawcmd = lfirst_node(AlterTableCmd, lc);
		Constraint *con;

		if (rawcmd->subtype != AT_AddConstraint ||
			rawcmd->def == NULL || !IsA(rawcmd->def, Constraint) ||
			list_member_ptr(*matched, rawcmd))
			continue;

		con = (Constraint *) rawcmd->def;
		if (con->contype == CONSTR_NOTNULL && con->keys != NIL &&
			strcmp(strVal(linitial(con->keys)), colname) == 0)
		{
			*matched = lappend(*matched, rawcmd);
			return con;
		}
	}
	return NULL;
}

/*
 * Return the raw ADD ... USING INDEX constraint that produced the constraint
 * named conname, or NULL.  An unnamed constraint takes the index's name.  The
 * index's original name, which the adoption may have changed, is only in the
 * raw node.
 */
static Constraint *
find_raw_index_constraint(AlterTableStmt *rawstmt, const char *conname)
{
	ListCell   *lc;

	foreach(lc, rawstmt->cmds)
	{
		AlterTableCmd *rawcmd = lfirst_node(AlterTableCmd, lc);
		Constraint *con;
		const char *finalname;

		if (rawcmd->subtype != AT_AddConstraint ||
			rawcmd->def == NULL || !IsA(rawcmd->def, Constraint))
			continue;

		con = (Constraint *) rawcmd->def;
		if (con->indexname == NULL)
			continue;

		finalname = con->conname != NULL ? con->conname : con->indexname;
		if (strcmp(finalname, conname) == 0)
			return con;
	}
	return NULL;
}

/*
 * Return the raw CLUSTER ON or REPLICA IDENTITY that the collected one came
 * from, or NULL.  ALTER COLUMN TYPE generates these for an index it rebuilds,
 * and the replay does so again.  They are matched by index name; *matched is
 * as in find_raw_alter_column.
 */
static AlterTableCmd *
find_raw_index_marking(AlterTableStmt *rawstmt, AlterTableCmd *subcmd,
					   List **matched)
{
	ListCell   *lc;

	foreach(lc, rawstmt->cmds)
	{
		AlterTableCmd *rawcmd = lfirst_node(AlterTableCmd, lc);

		if (rawcmd->subtype != subcmd->subtype ||
			list_member_ptr(*matched, rawcmd))
			continue;

		if (subcmd->subtype == AT_ClusterOn)
		{
			if (strcmp(rawcmd->name, subcmd->name) != 0)
				continue;
		}
		else if (subcmd->subtype == AT_ReplicaIdentity)
		{
			ReplicaIdentityStmt *rawri = castNode(ReplicaIdentityStmt,
												  rawcmd->def);
			ReplicaIdentityStmt *ri = castNode(ReplicaIdentityStmt,
											   subcmd->def);

			if (rawri->identity_type != ri->identity_type ||
				(ri->identity_type == REPLICA_IDENTITY_INDEX &&
				 strcmp(rawri->name, ri->name) != 0))
				continue;
		}
		else
			elog(ERROR, "unexpected ALTER TABLE subtype %d",
				 (int) subcmd->subtype);

		*matched = lappend(*matched, rawcmd);
		return rawcmd;
	}
	return NULL;
}

/* Emit an ADD CONSTRAINT subcommand; see deparse_one_constraint(). */
static void
emit_add_constraint(JsonbInState *state, Oid conoid, IndexStmt *idxstmt,
					List *typed_names)
{
	begin_jsonb_object(state);
	append_jsonb_pair1(state, "fmt", jbv_str("ADD %{constraint}s"));
	insert_jsonb_key(state, "constraint");
	deparse_one_constraint(state, conoid, NIL, idxstmt, typed_names);
	end_jsonb_object(state);
}

/*
 * Emit ADD [CONSTRAINT name] NOT NULL col from the raw statement and return
 * true, or return false if no raw one is left (see find_raw_table_notnull).  A
 * NOT NULL that merged with an existing one has no pg_constraint row, hence
 * the raw node.
 */
static bool
deparse_AddTableNotNull(JsonbInState *state, AlterTableStmt *rawstmt,
						const char *colname, List **matched)
{
	Constraint *rawcon = find_raw_table_notnull(rawstmt, colname, matched);

	if (rawcon == NULL)
		return false;

	begin_jsonb_object(state);
	append_jsonb_pair2(state,
					   "fmt", jbv_str("ADD %{name}sNOT NULL %{column}I%{no_inherit}s%{not_valid}s"),
					   "column", jbv_str(colname));
	if (rawcon->conname != NULL)
		new_jsonb_string_clause(state, "name", "CONSTRAINT %{conname}I ",
								"conname", rawcon->conname);
	else
		new_jsonb_null(state, "name");
	if (rawcon->is_no_inherit)
		new_jsonb_clause(state, "no_inherit", " NO INHERIT");
	else
		new_jsonb_null(state, "no_inherit");
	if (rawcon->skip_validation)
		new_jsonb_clause(state, "not_valid", " NOT VALID");
	else
		new_jsonb_null(state, "not_valid");
	end_jsonb_object(state);

	return true;
}

/*
 * Did this collected subcommand do nothing?  One whose IF [NOT] EXISTS found
 * nothing to do is collected with an invalid address.  DROP CONSTRAINT IF
 * EXISTS has no address either way, so it is always emitted.
 */
static bool
alter_table_subcmd_is_noop(const CollectedATSubcmd *sub)
{
	AlterTableCmd *subcmd = (AlterTableCmd *) sub->parsetree;

	if (!subcmd->missing_ok || OidIsValid(sub->address.objectId))
		return false;

	switch (subcmd->subtype)
	{
		case AT_AddColumn:
		case AT_DropColumn:
		case AT_DropIdentity:
		case AT_DropExpression:
			return true;
		default:
			return false;
	}
}

/*
 * Deparse one collected ALTER TABLE subcommand.  Returns true if something was
 * emitted, false for an internal subcommand that a sibling covers or the
 * replay generates again.  A subtype without a case here is an error.
 * *matched_rawcmds is as for find_raw_alter_column().
 */
static bool
deparse_AlterTableSubcmd(JsonbInState *state, Oid relId, List *dpcontext,
						 CollectedATSubcmd *sub, AlterTableCmd *subcmd,
						 AlterTableStmt *rawstmt, List **matched_rawcmds)
{
	switch (subcmd->subtype)
	{
		case AT_AddColumn:
			{
				ColumnDef  *coldef = castNode(ColumnDef, subcmd->def);
				ColumnDef  *rawcoldef = find_raw_add_column(rawstmt,
															coldef->colname);

				begin_jsonb_object(state);
				append_jsonb_pair1(state, "fmt",
								   jbv_str("ADD COLUMN%{if_not_exists}s %{definition}s"));

				if (subcmd->missing_ok)
					new_jsonb_clause(state, "if_not_exists", " IF NOT EXISTS");
				else
					new_jsonb_null(state, "if_not_exists");

				insert_jsonb_key(state, "definition");
				deparse_ColumnDef(state, relId, dpcontext, rawcoldef, sub);
				end_jsonb_object(state);
			}
			return true;

		case AT_DropColumn:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("DROP COLUMN%{if_exists}s %{column}I%{cascade}s"),
							   "column", jbv_str(subcmd->name));

			if (subcmd->missing_ok)
				new_jsonb_clause(state, "if_exists", " IF EXISTS");
			else
				new_jsonb_null(state, "if_exists");

			if (subcmd->behavior == DROP_CASCADE)
				new_jsonb_clause(state, "cascade", " CASCADE");
			else
				new_jsonb_null(state, "cascade");

			end_jsonb_object(state);
			return true;

		case AT_ColumnDefault:
			begin_jsonb_object(state);
			if (subcmd->def == NULL)
				append_jsonb_pair2(state,
								   "fmt", jbv_str("ALTER COLUMN %{column}I DROP DEFAULT"),
								   "column", jbv_str(subcmd->name));
			else
			{
				/*
				 * Use the expression this subcommand stored; see
				 * get_column_default_expr().
				 */
				append_jsonb_pair2(state,
								   "fmt", jbv_str("ALTER COLUMN %{column}I SET DEFAULT %{definition}s"),
								   "column", jbv_str(subcmd->name));
				append_jsonb_pair1(state, "definition",
								   jbv_str(sub->default_expr != NULL ?
										   deparse_expression(sub->default_expr,
															  dpcontext, false,
															  false) :
										   "NULL"));
			}
			end_jsonb_object(state);
			return true;

		case AT_SetNotNull:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("ALTER COLUMN %{column}I SET NOT NULL"),
							   "column", jbv_str(subcmd->name));
			end_jsonb_object(state);
			return true;

		case AT_DropNotNull:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("ALTER COLUMN %{column}I DROP NOT NULL"),
							   "column", jbv_str(subcmd->name));
			end_jsonb_object(state);
			return true;

		case AT_DropExpression:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("ALTER COLUMN %{column}I DROP EXPRESSION%{if_exists}s"),
							   "column", jbv_str(subcmd->name));

			if (subcmd->missing_ok)
				new_jsonb_clause(state, "if_exists", " IF EXISTS");
			else
				new_jsonb_null(state, "if_exists");

			end_jsonb_object(state);
			return true;

		case AT_AddIdentity:
			{
				/*
				 * Render from the raw subcommand; see
				 * find_raw_alter_column().
				 */
				AlterTableCmd *rawcmd = find_raw_alter_column(rawstmt,
															  AT_AddIdentity,
															  subcmd->name,
															  matched_rawcmds);
				Constraint *coldef = castNode(Constraint, rawcmd->def);

				begin_jsonb_object(state);
				append_jsonb_pair2(state,
								   "fmt", jbv_str("ALTER COLUMN %{column}I ADD%{identity}s"),
								   "column", jbv_str(subcmd->name));
				deparse_ColumnIdentity(state, "identity", coldef);
				end_jsonb_object(state);
			}
			return true;

		case AT_SetIdentity:
			{
				/*
				 * As for ADD IDENTITY.  Each option is SET GENERATED, RESTART
				 * or SET <sequence option>.
				 */
				AlterTableCmd *rawcmd = find_raw_alter_column(rawstmt,
															  AT_SetIdentity,
															  subcmd->name,
															  matched_rawcmds);
				ListCell   *lc;

				begin_jsonb_object(state);
				append_jsonb_pair2(state,
								   "fmt", jbv_str("ALTER COLUMN %{column}I %{options: }s"),
								   "column", jbv_str(subcmd->name));
				insert_jsonb_key(state, "options");
				begin_jsonb_array(state);
				foreach(lc, (List *) rawcmd->def)
				{
					DefElem    *elem = lfirst_node(DefElem, lc);
					JsonbValue	val;

					if (strcmp(elem->defname, "generated") == 0)
						val = jbv_str(psprintf("SET GENERATED %s",
											   intVal(elem->arg) == ATTRIBUTE_IDENTITY_ALWAYS ?
											   "ALWAYS" : "BY DEFAULT"));
					else if (strcmp(elem->defname, "restart") == 0)
						val = jbv_str(deparse_SeqOptElem(elem));
					else
						val = jbv_str(psprintf("SET %s", deparse_SeqOptElem(elem)));

					pushJsonbValue(state, WJB_ELEM, &val);
				}
				end_jsonb_array(state);
				end_jsonb_object(state);
			}
			return true;

		case AT_DropIdentity:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("ALTER COLUMN %{column}I DROP IDENTITY%{if_exists}s"),
							   "column", jbv_str(subcmd->name));

			if (subcmd->missing_ok)
				new_jsonb_clause(state, "if_exists", " IF EXISTS");
			else
				new_jsonb_null(state, "if_exists");

			end_jsonb_object(state);
			return true;

		case AT_SetExpression:
			/* The expression this subcommand stored, as for SET DEFAULT */
			if (sub->default_expr == NULL)
				elog(ERROR, "no generation expression collected for column \"%s\" of relation %u",
					 subcmd->name, relId);

			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("ALTER COLUMN %{column}I SET EXPRESSION AS (%{expression}s)"),
							   "column", jbv_str(subcmd->name));
			append_jsonb_pair1(state, "expression",
							   jbv_str(deparse_expression(sub->default_expr,
														  dpcontext, false,
														  false)));
			end_jsonb_object(state);
			return true;

		case AT_SetStatistics:
			{
				StringInfoData fmtbuf;
				const char *colref;
				char	   *colval;

				/*
				 * SET STATISTICS on an index expression column gives the
				 * column by number.
				 */
				if (subcmd->name != NULL)
				{
					colref = "%{column}I";
					colval = subcmd->name;
				}
				else
				{
					colref = "%{column}s";
					colval = psprintf("%d", subcmd->num);
				}

				/* SET STATISTICS DEFAULT leaves subcmd->def NULL */
				initStringInfo(&fmtbuf);
				if (subcmd->def == NULL)
					appendStringInfo(&fmtbuf,
									 "ALTER COLUMN %s SET STATISTICS DEFAULT", colref);
				else
					appendStringInfo(&fmtbuf,
									 "ALTER COLUMN %s SET STATISTICS %%{statistics}s", colref);

				begin_jsonb_object(state);
				append_jsonb_pair2(state, "fmt", jbv_str(fmtbuf.data),
								   "column", jbv_str(colval));

				if (subcmd->def != NULL)
					append_jsonb_pair1(state, "statistics",
									   jbv_str(psprintf("%d", intVal((Node *) subcmd->def))));

				end_jsonb_object(state);
			}
			return true;

		case AT_SetOptions:
		case AT_ResetOptions:
			{
				ListCell   *opt;

				begin_jsonb_object(state);
				append_jsonb_pair2(state,
								   "fmt", jbv_str(subcmd->subtype == AT_SetOptions ?
												  "ALTER COLUMN %{column}I SET (%{options:, }s)" :
												  "ALTER COLUMN %{column}I RESET (%{options:, }s)"),
								   "column", jbv_str(subcmd->name));
				insert_jsonb_key(state, "options");
				begin_jsonb_array(state);

				foreach(opt, (List *) subcmd->def)
					deparse_DefElem(state, (DefElem *) lfirst(opt),
									subcmd->subtype == AT_ResetOptions);

				end_jsonb_array(state);
				end_jsonb_object(state);
			}
			return true;

		case AT_SetStorage:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("ALTER COLUMN %{column}I SET STORAGE %{storage}I"),
							   "column", jbv_str(subcmd->name));
			append_jsonb_pair1(state, "storage",
							   jbv_str(strVal((Node *) subcmd->def)));
			end_jsonb_object(state);
			return true;

		case AT_SetCompression:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("ALTER COLUMN %{column}I SET COMPRESSION %{compression}I"),
							   "column", jbv_str(subcmd->name));
			append_jsonb_pair1(state, "compression",
							   jbv_str(strVal((Node *) subcmd->def)));
			end_jsonb_object(state);
			return true;

		case AT_AlterColumnType:
			{
				ColumnDef  *coldef = castNode(ColumnDef, subcmd->def);
				HeapTuple	atup;
				Form_pg_attribute attr;

				atup = SearchSysCacheAttName(relId, subcmd->name);
				if (!HeapTupleIsValid(atup))
					elog(ERROR, "could not find column \"%s\" of relation %u",
						 subcmd->name, relId);

				attr = (Form_pg_attribute) GETSTRUCT(atup);

				begin_jsonb_object(state);
				append_jsonb_pair2(state,
								   "fmt", jbv_str("ALTER COLUMN %{column}I SET DATA TYPE "
												  "%{datatype}T%{collation}s%{using}s"),
								   "column", jbv_str(subcmd->name));
				new_jsonb_for_type(state, "datatype", attr->atttypid, attr->atttypmod);

				/* COLLATE, only if the user wrote it */
				if (coldef->collClause != NULL)
				{
					begin_jsonb_clause(state, "collation", " COLLATE %{name}D");
					new_jsonb_for_qualname_id(state, CollationRelationId,
											  attr->attcollation, "name");
					end_jsonb_clause(state);
				}
				else
					new_jsonb_null(state, "collation");

				/*
				 * USING: the text saved at prep time; see
				 * EventTriggerCollectAlterColumnTypeUsing().
				 */
				if (sub->using_text != NULL)
					new_jsonb_string_clause(state, "using", " USING %{expression}s",
											"expression", sub->using_text);
				else if (coldef->cooked_default != NULL)
					elog(ERROR, "no USING expression collected for column \"%s\" of relation %u",
						 subcmd->name, relId);
				else
					new_jsonb_null(state, "using");

				ReleaseSysCache(atup);
				end_jsonb_object(state);
			}
			return true;

		case AT_AddConstraint:
			{
				Constraint *con = (Constraint *) subcmd->def;
				Oid			conoid = sub->address.objectId;

				/*
				 * The user's table-level NOT NULL, or one ADD COLUMN ... NOT
				 * NULL or ADD PRIMARY KEY generated; see
				 * deparse_AddTableNotNull().  Handle it before the lookup
				 * below, since a merged NOT NULL has no row.
				 */
				if (con != NULL && IsA(con, Constraint) &&
					con->contype == CONSTR_NOTNULL)
				{
					if (con->keys == NIL)
						elog(ERROR, "NOT NULL constraint without a column in ALTER TABLE deparse");
					return deparse_AddTableNotNull(state, rawstmt,
												   strVal(linitial(con->keys)),
												   matched_rawcmds);
				}

				/*
				 * A constraint that merged with an inherited one has no new
				 * row, so the address is invalid.  Look it up by name, which
				 * execution filled in if the user gave none, and emit it, as
				 * the replay merges it the same way.
				 */
				if (!OidIsValid(conoid))
				{
					if (con == NULL || !IsA(con, Constraint) || con->conname == NULL)
						elog(ERROR, "could not identify constraint added to relation %u",
							 relId);

					conoid = get_relation_constraint_oid(relId, con->conname,
														 false);
				}

				/* CHECK and FOREIGN KEY are not index-backed. */
				emit_add_constraint(state, conoid, NULL,
									collect_raw_alter_constraint_names(rawstmt));
			}
			return true;

		case AT_AddIndex:
			{
				IndexStmt  *istmt = castNode(IndexStmt, subcmd->def);
				Oid			conoid;
				List	   *typed_names = NIL;

				/*
				 * PRIMARY KEY / UNIQUE / EXCLUDE.  A plain index is a
				 * separate CREATE INDEX command.
				 */
				if (!istmt->isconstraint || !OidIsValid(sub->address.objectId))
					elog(ERROR, "unexpected index addition to relation %u",
						 relId);

				conoid = get_index_constraint(sub->address.objectId);
				if (!OidIsValid(conoid))
					elog(ERROR, "could not find constraint for index %u",
						 sub->address.objectId);

				/* The user named the constraint iff they named the index. */
				if (istmt->idxname != NULL)
					typed_names = list_make1(istmt->idxname);

				emit_add_constraint(state, conoid, istmt, typed_names);
			}
			return true;

		case AT_AddIndexConstraint:
			{
				/*
				 * Emit USING INDEX as such, since deparsing the definition
				 * would make the replay build a second index.  The index's
				 * original name and the deferrability come from the raw node.
				 */
				Oid			conoid = sub->address.objectId;
				char	   *conname;
				Constraint *rawcon;

				conname = get_constraint_name(conoid);
				if (conname == NULL)
					elog(ERROR, "cache lookup failed for constraint %u", conoid);

				rawcon = find_raw_index_constraint(rawstmt, conname);
				if (rawcon == NULL)
					elog(ERROR, "could not find raw ADD CONSTRAINT ... USING INDEX for constraint \"%s\"",
						 conname);

				begin_jsonb_object(state);
				append_jsonb_pair2(state,
								   "fmt", jbv_str("ADD %{name}s%{contype}s USING INDEX %{index}I%{deferrable}s%{initdeferred}s"),
								   "contype", jbv_str(rawcon->contype == CONSTR_PRIMARY ?
													  "PRIMARY KEY" : "UNIQUE"));
				append_jsonb_pair1(state, "index", jbv_str(rawcon->indexname));

				/* Name the constraint only if the user did. */
				if (rawcon->conname != NULL)
					new_jsonb_string_clause(state, "name",
											"CONSTRAINT %{conname}I ",
											"conname", rawcon->conname);
				else
					new_jsonb_null(state, "name");

				if (rawcon->deferrable)
					new_jsonb_clause(state, "deferrable", " DEFERRABLE");
				else
					new_jsonb_null(state, "deferrable");

				if (rawcon->initdeferred)
					new_jsonb_clause(state, "initdeferred", " INITIALLY DEFERRED");
				else
					new_jsonb_null(state, "initdeferred");

				end_jsonb_object(state);
			}
			return true;

		case AT_AlterConstraint:
			{
				/*
				 * Build the clause from what the user altered.  INITIALLY
				 * IMMEDIATE is left implicit.
				 */
				ATAlterConstraint *con = castNode(ATAlterConstraint,
												  subcmd->def);
				StringInfoData fmtbuf;

				initStringInfo(&fmtbuf);
				appendStringInfoString(&fmtbuf, "ALTER CONSTRAINT %{conname}I");
				if (con->alterDeferrability)
				{
					appendStringInfoString(&fmtbuf,
										   con->deferrable ? " DEFERRABLE" :
										   " NOT DEFERRABLE");
					if (con->initdeferred)
						appendStringInfoString(&fmtbuf, " INITIALLY DEFERRED");
				}

				if (con->alterEnforceability)
					appendStringInfoString(&fmtbuf,
										   con->is_enforced ? " ENFORCED" :
										   " NOT ENFORCED");
				if (con->alterInheritability)
					appendStringInfoString(&fmtbuf,
										   con->noinherit ? " NO INHERIT" :
										   " INHERIT");

				begin_jsonb_object(state);
				append_jsonb_pair2(state, "fmt", jbv_str(fmtbuf.data),
								   "conname", jbv_str(con->conname));
				end_jsonb_object(state);
			}
			return true;

		case AT_ValidateConstraint:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("VALIDATE CONSTRAINT %{constraint}I"),
							   "constraint", jbv_str(subcmd->name));
			end_jsonb_object(state);
			return true;

		case AT_DropConstraint:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("DROP CONSTRAINT%{if_exists}s %{constraint}I%{cascade}s"),
							   "constraint", jbv_str(subcmd->name));

			if (subcmd->missing_ok)
				new_jsonb_clause(state, "if_exists", " IF EXISTS");
			else
				new_jsonb_null(state, "if_exists");

			if (subcmd->behavior == DROP_CASCADE)
				new_jsonb_clause(state, "cascade", " CASCADE");
			else
				new_jsonb_null(state, "cascade");

			end_jsonb_object(state);
			return true;

		case AT_DropOids:
			begin_jsonb_object(state);
			append_jsonb_pair1(state, "fmt", jbv_str("SET WITHOUT OIDS"));
			end_jsonb_object(state);
			return true;

		case AT_ChangeOwner:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("OWNER TO %{owner}I"),
							   "owner", jbv_str(get_rolespec_name(subcmd->newowner)));
			end_jsonb_object(state);
			return true;

		case AT_SetLogged:
		case AT_SetUnLogged:
		case AT_DropOf:
		case AT_EnableRowSecurity:
		case AT_DisableRowSecurity:
		case AT_ForceRowSecurity:
		case AT_NoForceRowSecurity:
		case AT_EnableTrigAll:
		case AT_DisableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrigUser:
			{
				char	   *fmt;

				switch (subcmd->subtype)
				{
					case AT_SetLogged:
						fmt = "SET LOGGED";
						break;
					case AT_SetUnLogged:
						fmt = "SET UNLOGGED";
						break;
					case AT_DropOf:
						fmt = "NOT OF";
						break;
					case AT_EnableRowSecurity:
						fmt = "ENABLE ROW LEVEL SECURITY";
						break;
					case AT_DisableRowSecurity:
						fmt = "DISABLE ROW LEVEL SECURITY";
						break;
					case AT_ForceRowSecurity:
						fmt = "FORCE ROW LEVEL SECURITY";
						break;
					case AT_NoForceRowSecurity:
						fmt = "NO FORCE ROW LEVEL SECURITY";
						break;
					case AT_EnableTrigAll:
						fmt = "ENABLE TRIGGER ALL";
						break;
					case AT_DisableTrigAll:
						fmt = "DISABLE TRIGGER ALL";
						break;
					case AT_EnableTrigUser:
						fmt = "ENABLE TRIGGER USER";
						break;
					default:
						fmt = "DISABLE TRIGGER USER";
						break;
				}

				begin_jsonb_object(state);
				append_jsonb_pair1(state, "fmt", jbv_str(fmt));
				end_jsonb_object(state);
			}
			return true;

		case AT_SetAccessMethod:
			begin_jsonb_object(state);
			if (subcmd->name != NULL)
				append_jsonb_pair2(state,
								   "fmt", jbv_str("SET ACCESS METHOD %{access_method}I"),
								   "access_method", jbv_str(subcmd->name));
			else
				append_jsonb_pair1(state, "fmt",
								   jbv_str("SET ACCESS METHOD DEFAULT"));

			end_jsonb_object(state);
			return true;

		case AT_SetTableSpace:
			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("SET TABLESPACE %{tablespace}I"),
							   "tablespace", jbv_str(subcmd->name));
			end_jsonb_object(state);
			return true;

		case AT_SetRelOptions:
		case AT_ResetRelOptions:
			{
				ListCell   *opt;

				begin_jsonb_object(state);
				append_jsonb_pair1(state, "fmt",
								   jbv_str(subcmd->subtype == AT_SetRelOptions ?
										   "SET (%{options:, }s)" :
										   "RESET (%{options:, }s)"));
				insert_jsonb_key(state, "options");
				begin_jsonb_array(state);
				foreach(opt, (List *) subcmd->def)
					deparse_DefElem(state, (DefElem *) lfirst(opt),
									subcmd->subtype == AT_ResetRelOptions);
				end_jsonb_array(state);
				end_jsonb_object(state);
			}
			return true;

		case AT_EnableTrig:
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_DisableTrig:
			{
				char	   *fmt;

				switch (subcmd->subtype)
				{
					case AT_EnableTrig:
						fmt = "ENABLE TRIGGER %{trigger}I";
						break;
					case AT_EnableAlwaysTrig:
						fmt = "ENABLE ALWAYS TRIGGER %{trigger}I";
						break;
					case AT_EnableReplicaTrig:
						fmt = "ENABLE REPLICA TRIGGER %{trigger}I";
						break;
					default:
						fmt = "DISABLE TRIGGER %{trigger}I";
						break;
				}

				begin_jsonb_object(state);
				append_jsonb_pair2(state, "fmt", jbv_str(fmt),
								   "trigger", jbv_str(subcmd->name));
				end_jsonb_object(state);
			}
			return true;

		case AT_EnableRule:
		case AT_EnableAlwaysRule:
		case AT_EnableReplicaRule:
		case AT_DisableRule:
			{
				char	   *fmt;

				switch (subcmd->subtype)
				{
					case AT_EnableRule:
						fmt = "ENABLE RULE %{rule}I";
						break;
					case AT_EnableAlwaysRule:
						fmt = "ENABLE ALWAYS RULE %{rule}I";
						break;
					case AT_EnableReplicaRule:
						fmt = "ENABLE REPLICA RULE %{rule}I";
						break;
					default:
						fmt = "DISABLE RULE %{rule}I";
						break;
				}

				begin_jsonb_object(state);
				append_jsonb_pair2(state, "fmt", jbv_str(fmt),
								   "rule", jbv_str(subcmd->name));
				end_jsonb_object(state);
			}
			return true;

		case AT_AddInherit:
			begin_jsonb_object(state);
			append_jsonb_pair1(state, "fmt", jbv_str("INHERIT %{parent}D"));
			new_jsonb_for_qualname_id(state, RelationRelationId,
									  sub->address.objectId, "parent");
			end_jsonb_object(state);
			return true;

		case AT_DropInherit:
			begin_jsonb_object(state);
			append_jsonb_pair1(state, "fmt", jbv_str("NO INHERIT %{parent}D"));
			new_jsonb_for_qualname_id(state, RelationRelationId,
									  sub->address.objectId, "parent");
			end_jsonb_object(state);
			return true;

		case AT_AddOf:
			begin_jsonb_object(state);
			append_jsonb_pair1(state, "fmt", jbv_str("OF %{type}T"));
			new_jsonb_for_type(state, "type", sub->address.objectId, -1);
			end_jsonb_object(state);
			return true;

		case AT_AttachPartition:
			begin_jsonb_object(state);
			append_jsonb_pair1(state, "fmt",
							   jbv_str("ATTACH PARTITION %{partition}D %{bound}s"));
			new_jsonb_for_qualname_id(state, RelationRelationId,
									  sub->address.objectId, "partition");
			append_jsonb_pair1(state, "bound",
							   jbv_str(get_partbound_spec_string(sub->address.objectId)));
			end_jsonb_object(state);
			return true;

		case AT_DetachPartition:
			/* CONCURRENTLY is not supported */
			begin_jsonb_object(state);
			append_jsonb_pair1(state, "fmt",
							   jbv_str("DETACH PARTITION %{partition}D"));
			new_jsonb_for_qualname_id(state, RelationRelationId,
									  sub->address.objectId, "partition");
			end_jsonb_object(state);
			return true;

			/*
			 * CLUSTER ON and REPLICA IDENTITY are emitted only if the user
			 * wrote them; see find_raw_index_marking.
			 */
		case AT_ClusterOn:
			if (find_raw_index_marking(rawstmt, subcmd, matched_rawcmds) == NULL)
				return false;

			begin_jsonb_object(state);
			append_jsonb_pair2(state, "fmt", jbv_str("CLUSTER ON %{index}I"),
							   "index", jbv_str(subcmd->name));
			end_jsonb_object(state);
			return true;

		case AT_DropCluster:
			begin_jsonb_object(state);
			append_jsonb_pair1(state, "fmt", jbv_str("SET WITHOUT CLUSTER"));
			end_jsonb_object(state);
			return true;

		case AT_ReplicaIdentity:
			{
				ReplicaIdentityStmt *ri = castNode(ReplicaIdentityStmt,
												   subcmd->def);

				if (find_raw_index_marking(rawstmt, subcmd,
										   matched_rawcmds) == NULL)
					return false;

				begin_jsonb_object(state);
				switch (ri->identity_type)
				{
					case REPLICA_IDENTITY_DEFAULT:
						append_jsonb_pair1(state, "fmt",
										   jbv_str("REPLICA IDENTITY DEFAULT"));
						break;
					case REPLICA_IDENTITY_FULL:
						append_jsonb_pair1(state, "fmt",
										   jbv_str("REPLICA IDENTITY FULL"));
						break;
					case REPLICA_IDENTITY_NOTHING:
						append_jsonb_pair1(state, "fmt",
										   jbv_str("REPLICA IDENTITY NOTHING"));
						break;
					case REPLICA_IDENTITY_INDEX:
						append_jsonb_pair2(state,
										   "fmt", jbv_str("REPLICA IDENTITY USING INDEX %{index}I"),
										   "index", jbv_str(ri->name));
						break;
					default:
						elog(ERROR, "unexpected replica identity type %c",
							 ri->identity_type);
				}

				end_jsonb_object(state);
			}
			return true;

			/*
			 * Internal subtypes used for the execution machinery.  They carry
			 * no user-visible clause of their own.
			 */
		case AT_ReAddIndex:
		case AT_ReAddConstraint:
		case AT_ReAddDomainConstraint:
		case AT_ReAddComment:
		case AT_ReAddStatistics:
		case AT_ReplaceRelOptions:
			return false;

		default:
			elog(ERROR, "unexpected ALTER TABLE subtype %d in deparse",
				 (int) subcmd->subtype);
	}

	return false;				/* keep compiler quiet */
}

/*
 * Deparse an ALTER TABLE.  The subcommands of every SCT_AlterTable collected
 * for the relation are emitted as one statement, keeping the original's single
 * pass.  Subcommands recursed to inheritance children are skipped, since the
 * replay recurses the same way.
 *
 * Returns false if every subcommand did nothing; the caller must then discard
 * what was added to state.
 */
static bool
deparse_AlterTableStmt(JsonbInState *state, CollectedCommand *cmd,
					   AlterTableStmt *rawstmt, List *cmds)
{
	Oid			relId = cmd->d.alterTable.objectId;
	Relation	rel;
	List	   *dpcontext;
	bool		did_something = false;
	int			nsubcmds = 0;
	List	   *matched_rawcmds = NIL;
	ListCell   *cell;

	Assert(IsA(rawstmt, AlterTableStmt));

	rel = relation_open(relId, AccessShareLock);
	dpcontext = deparse_context_for(RelationGetRelationName(rel), relId);

	begin_jsonb_object(state);
	append_jsonb_pair1(state, "fmt",
					   jbv_str("ALTER TABLE%{if_exists}s%{only}s %{identity}D %{subcmds:, }s"));

	/* IF EXISTS and ONLY, from the raw statement */
	if (rawstmt->missing_ok)
		new_jsonb_clause(state, "if_exists", " IF EXISTS");
	else
		new_jsonb_null(state, "if_exists");

	if (!rawstmt->relation->inh)
		new_jsonb_clause(state, "only", " ONLY");
	else
		new_jsonb_null(state, "only");

	/* IDENTITY (table name) */
	new_jsonb_for_qualname(state, rel->rd_rel->relnamespace,
						   RelationGetRelationName(rel), "identity");

	/*
	 * SUBCOMMANDS.  A subcommand that runs a nested internal ALTER TABLE on
	 * the relation, such as ADD IDENTITY, opens an SCT_AlterTable of its own,
	 * so gather the subcommands of all of them.
	 */
	insert_jsonb_key(state, "subcmds");
	begin_jsonb_array(state);
	foreach(cell, cmds)
	{
		CollectedCommand *cc = (CollectedCommand *) lfirst(cell);
		ListCell   *subcell;

		if (cc->type != SCT_AlterTable ||
			cc->d.alterTable.objectId != relId)
			continue;

		foreach(subcell, cc->d.alterTable.subcmds)
		{
			CollectedATSubcmd *sub = (CollectedATSubcmd *) lfirst(subcell);
			AlterTableCmd *subcmd = (AlterTableCmd *) sub->parsetree;

			Assert(IsA(subcmd, AlterTableCmd));

			/*
			 * Skip a subcommand recursed to a child.  Exempt the subtypes
			 * that name another relation on purpose, and CLUSTER ON, whose
			 * index can have a pg_inherits row; none of them recurse.
			 */
			if (sub->address.classId == RelationRelationId &&
				subcmd->subtype != AT_AttachPartition &&
				subcmd->subtype != AT_DetachPartition &&
				subcmd->subtype != AT_AddInherit &&
				subcmd->subtype != AT_DropInherit &&
				subcmd->subtype != AT_ClusterOn &&
				OidIsValid(sub->address.objectId) &&
				sub->address.objectId != relId &&
				has_superclass(sub->address.objectId))
				continue;

			if (alter_table_subcmd_is_noop(sub))
				continue;
			did_something = true;

			if (deparse_AlterTableSubcmd(state, relId, dpcontext, sub, subcmd,
										 rawstmt, &matched_rawcmds))
				nsubcmds++;
		}
	}

	relation_close(rel, AccessShareLock);

	if (!did_something)
		return false;

	/*
	 * A subcommand that did something must have been emitted; otherwise the
	 * result would be a bare "ALTER TABLE name".
	 */
	if (nsubcmds == 0)
		elog(ERROR, "no subcommand deparsed for ALTER TABLE on relation %u",
			 relId);

	end_jsonb_array(state);

	end_jsonb_object(state);

	return true;
}

/*
 * Deparse ALTER TABLE ... RENAME.  The old name is gone from the catalogs, so
 * it comes from the parse node.
 */
static void
deparse_RenameStmt(JsonbInState *state, CollectedCommand *cmd)
{
	RenameStmt *node = castNode(RenameStmt, cmd->parsetree);
	Oid			objectId = cmd->d.simple.address.objectId;
	Oid			nspid;

	switch (node->renameType)
	{
		case OBJECT_TABLE:
			nspid = get_rel_namespace(objectId);
			if (!OidIsValid(nspid))
				elog(ERROR, "cache lookup failed for relation %u", objectId);

			begin_jsonb_object(state);
			append_jsonb_pair2(state,
							   "fmt", jbv_str("ALTER TABLE%{if_exists}s %{identity}D RENAME TO %{newname}I"),
							   "newname", jbv_str(node->newname));

			if (node->missing_ok)
				new_jsonb_clause(state, "if_exists", " IF EXISTS");
			else
				new_jsonb_null(state, "if_exists");

			new_jsonb_for_qualname(state, nspid, node->relation->relname,
								   "identity");
			end_jsonb_object(state);
			break;

		case OBJECT_COLUMN:
			begin_jsonb_object(state);
			append_jsonb_pair3(state,
							   "fmt", jbv_str("ALTER TABLE%{if_exists}s%{only}s %{identity}D "
											  "RENAME COLUMN %{oldname}I TO %{newname}I"),
							   "oldname", jbv_str(node->subname),
							   "newname", jbv_str(node->newname));
			if (node->missing_ok)
				new_jsonb_clause(state, "if_exists", " IF EXISTS");
			else
				new_jsonb_null(state, "if_exists");
			if (!node->relation->inh)
				new_jsonb_clause(state, "only", " ONLY");
			else
				new_jsonb_null(state, "only");

			new_jsonb_for_qualname_id(state, RelationRelationId, objectId,
									  "identity");
			end_jsonb_object(state);
			break;

		case OBJECT_TABCONSTRAINT:
			{
				HeapTuple	tup = SearchSysCache1(CONSTROID,
												  ObjectIdGetDatum(objectId));
				Form_pg_constraint con;

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "cache lookup failed for constraint %u",
						 objectId);
				con = (Form_pg_constraint) GETSTRUCT(tup);

				begin_jsonb_object(state);
				append_jsonb_pair3(state,
								   "fmt", jbv_str("ALTER TABLE%{if_exists}s%{only}s %{identity}D "
												  "RENAME CONSTRAINT %{oldname}I TO %{newname}I"),
								   "oldname", jbv_str(node->subname),
								   "newname", jbv_str(node->newname));
				if (node->missing_ok)
					new_jsonb_clause(state, "if_exists", " IF EXISTS");
				else
					new_jsonb_null(state, "if_exists");
				if (!node->relation->inh)
					new_jsonb_clause(state, "only", " ONLY");
				else
					new_jsonb_null(state, "only");

				new_jsonb_for_qualname_id(state, RelationRelationId,
										  con->conrelid, "identity");
				end_jsonb_object(state);
				ReleaseSysCache(tup);
			}
			break;

		default:
			elog(ERROR, "unsupported rename type %d in deparse",
				 (int) node->renameType);
	}
}

/*
 * Deparse ALTER TABLE ... SET SCHEMA.  The old schema is the command's
 * secondary object.
 */
static void
deparse_AlterObjectSchemaStmt(JsonbInState *state, CollectedCommand *cmd)
{
	AlterObjectSchemaStmt *node = castNode(AlterObjectSchemaStmt,
										   cmd->parsetree);
	Oid			objectId = cmd->d.simple.address.objectId;
	Oid			old_schema = cmd->d.simple.secondaryObject.objectId;
	char	   *relname;

	relname = get_rel_name(objectId);
	if (relname == NULL)
		elog(ERROR, "cache lookup failed for relation %u", objectId);

	begin_jsonb_object(state);
	append_jsonb_pair2(state,
					   "fmt", jbv_str("ALTER TABLE%{if_exists}s %{identity}D SET SCHEMA %{newschema}I"),
					   "newschema", jbv_str(node->newschema));
	if (node->missing_ok)
		new_jsonb_clause(state, "if_exists", " IF EXISTS");
	else
		new_jsonb_null(state, "if_exists");
	new_jsonb_for_qualname(state, old_schema, relname, "identity");
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

	if (parsetree == NULL)
		return false;

	switch (nodeTag(parsetree))
	{
		case T_CreateStmt:

			/*
			 * LIKE options that copy indexes, comments or statistics would
			 * need CREATE INDEX etc. to be deparsed too.
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

		case T_AlterTableStmt:
			{
				const AlterTableStmt *atstmt = (const AlterTableStmt *) parsetree;

				/*
				 * ALTER INDEX/VIEW/etc. are not supported.  ALTER TABLE
				 * naming another kind of relation is deparsed as written.
				 */
				if (atstmt->objtype != OBJECT_TABLE)
					return false;

				/*
				 * Decline the whole statement if any subcommand can't be
				 * deparsed.
				 */
				foreach(lc, atstmt->cmds)
				{
					AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

					switch (cmd->subtype)
					{
						case AT_AddColumn:

							/*
							 * Column OPTIONS become an ALTER COLUMN ...
							 * OPTIONS subcommand, which is not supported; see
							 * below.
							 */
							if (castNode(ColumnDef, cmd->def)->fdwoptions != NIL)
								return false;
							continue;
						case AT_AlterColumnType:
						case AT_DropColumn:
						case AT_ColumnDefault:
						case AT_SetNotNull:
						case AT_DropNotNull:
						case AT_DropExpression:
						case AT_AddIdentity:
						case AT_SetIdentity:
						case AT_DropIdentity:
						case AT_SetExpression:
						case AT_SetStatistics:
						case AT_SetOptions:
						case AT_ResetOptions:
						case AT_SetStorage:
						case AT_SetCompression:
						case AT_AlterConstraint:
						case AT_ValidateConstraint:
						case AT_DropConstraint:
						case AT_ChangeOwner:
						case AT_SetLogged:
						case AT_SetUnLogged:
						case AT_SetAccessMethod:
						case AT_SetTableSpace:
						case AT_SetRelOptions:
						case AT_ResetRelOptions:
						case AT_EnableTrig:
						case AT_EnableAlwaysTrig:
						case AT_EnableReplicaTrig:
						case AT_DisableTrig:
						case AT_EnableTrigAll:
						case AT_DisableTrigAll:
						case AT_EnableTrigUser:
						case AT_DisableTrigUser:
						case AT_EnableRule:
						case AT_EnableAlwaysRule:
						case AT_EnableReplicaRule:
						case AT_DisableRule:
						case AT_EnableRowSecurity:
						case AT_DisableRowSecurity:
						case AT_ForceRowSecurity:
						case AT_NoForceRowSecurity:
						case AT_AddInherit:
						case AT_DropInherit:
						case AT_AddOf:
						case AT_DropOf:
						case AT_ReplicaIdentity:
						case AT_ClusterOn:
						case AT_DropCluster:
						case AT_DropOids:
						case AT_AttachPartition:
							continue;
						case AT_DetachPartition:

							/*
							 * DETACH CONCURRENTLY runs in several
							 * transactions.  Not supported for now.
							 */
							if (castNode(PartitionCmd, cmd->def)->concurrent)
								return false;
							continue;
						case AT_DetachPartitionFinalize:

							/*
							 * FINALIZE completes an interrupted DETACH
							 * PARTITION CONCURRENTLY, which we don't support,
							 * so a replay would find nothing to finalize.
							 */
							return false;
						case AT_AddConstraint:

							/*
							 * All constraint types, including NOT NULL and
							 * USING INDEX
							 */
							continue;

							/* Foreign-table options are not supported */
						case AT_GenericOptions:
						case AT_AlterColumnGenericOptions:
							return false;

							/*
							 * These never appear in a raw statement; parse
							 * analysis or execution generates them.
							 */
						case AT_CookedColumnDefault:
						case AT_AddIndex:
						case AT_AddIndexConstraint:
						case AT_AddColumnToView:
						case AT_ReplaceRelOptions:
						case AT_ReAddIndex:
						case AT_ReAddConstraint:
						case AT_ReAddDomainConstraint:
						case AT_ReAddComment:
						case AT_ReAddStatistics:
							return false;

							/*
							 * A new AlterTableType value must be classified
							 * above.
							 */
						default:
							Assert(false);
							return false;
					}
				}

				return true;
			}

		case T_RenameStmt:
			{
				const RenameStmt *rn = (const RenameStmt *) parsetree;

				switch (rn->renameType)
				{
					case OBJECT_TABLE:
						return true;
					case OBJECT_COLUMN:
						/* only when the relation is an ordinary table */
						return rn->relationType == OBJECT_TABLE;
					case OBJECT_TABCONSTRAINT:

						/*
						 * The grammar produces this only for ALTER TABLE ...
						 * RENAME CONSTRAINT, without setting relationType.
						 */
						return true;
					default:
						return false;
				}
			}

		case T_AlterObjectSchemaStmt:
			return ((const AlterObjectSchemaStmt *) parsetree)->objectType ==
				OBJECT_TABLE;

		default:
			return false;
	}
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
 * Returns NULL if the command did nothing: IF [NOT] EXISTS found nothing to
 * do for the table or for every subcommand of an ALTER TABLE.  Also returns
 * NULL for a command run by an extension script.  It is an error to pass a
 * command for which ddl_deparse_command_supported() returns false.
 *
 * The result is allocated in the caller's memory context.
 */
char *
deparse_ddl_command(const Node *original_parsetree, List *cmds)
{
	MemoryContext oldcxt;
	MemoryContext tmpcxt;
	CollectedCommand *thiscmd = NULL;
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
	 * Find the collected command that corresponds to the raw statement.  The
	 * transformed parse trees in the collected commands are used only for
	 * this.  All deparsing works on the raw tree and the catalogs.
	 */
	foreach(lc, cmds)
	{
		CollectedCommand *cmd = (CollectedCommand *) lfirst(lc);

		if (nodeTag(original_parsetree) == T_AlterTableStmt)
		{
			if (cmd->type == SCT_AlterTable)
			{
				thiscmd = cmd;
				break;
			}
		}
		else if (cmd->type == SCT_Simple && cmd->parsetree != NULL &&
				 nodeTag(cmd->parsetree) == nodeTag(original_parsetree))
		{
			/* CREATE TABLE, RENAME, and SET SCHEMA are SCT_Simple */
			thiscmd = cmd;
			break;
		}
	}

	/*
	 * Nothing was collected.  For CREATE, the object was not created, for
	 * example IF NOT EXISTS on an existing table, whose SCT_Simple carries an
	 * invalid OID.  For ALTER, there is no SCT_AlterTable, for example IF
	 * EXISTS on a missing table.  Either way there is nothing to reproduce.
	 */
	if (thiscmd == NULL)
		return NULL;

	if (thiscmd->type == SCT_Simple &&
		!OidIsValid(thiscmd->d.simple.address.objectId))
		return NULL;

	/* Commands run by extension scripts are not to be reproduced */
	if (thiscmd->in_extension)
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
	switch (nodeTag(original_parsetree))
	{
		case T_CreateStmt:
			deparse_CreateStmt(&state, thiscmd->d.simple.address.objectId,
							   (CreateStmt *) original_parsetree, cmds);
			break;
		case T_AlterTableStmt:
			if (!deparse_AlterTableStmt(&state, thiscmd,
										(AlterTableStmt *) original_parsetree,
										cmds))
			{
				/* every subcommand's IF [NOT] EXISTS found nothing to do */
				AtEOXact_GUC(true, save_nestlevel);
				MemoryContextSwitchTo(oldcxt);
				MemoryContextDelete(tmpcxt);
				pfree(str.data);
				return NULL;
			}
			break;
		case T_RenameStmt:
			deparse_RenameStmt(&state, thiscmd);
			break;
		case T_AlterObjectSchemaStmt:
			deparse_AlterObjectSchemaStmt(&state, thiscmd);
			break;
		default:
			elog(ERROR, "unexpected node type %d in deparse",
				 (int) nodeTag(original_parsetree));
	}

	end_jsonb_object(&state);

	jsonb = JsonbValueToJsonb(state.result);

	AtEOXact_GUC(true, save_nestlevel);

	command = JsonbToCString(&str, &jsonb->root, JSONB_ESTIMATED_LEN);

	/* Clean up; the output string is in the caller's context. */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);

	return command;
}
