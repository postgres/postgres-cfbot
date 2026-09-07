/*-------------------------------------------------------------------------
 *
 * ddlutils.c
 *		Utility functions for generating DDL statements
 *
 * This file contains the pg_get_*_ddl family of functions that generate
 * DDL statements to recreate database objects such as roles, tablespaces,
 * databases, and tables, along with common infrastructure for
 * pretty-printing.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ddlutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/toast_compression.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "rewrite/rewriteDefine.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

/*
 * Object-class kinds that the only / except options on
 * pg_get_table_ddl can filter on.  Members are stored as integers in a
 * Bitmapset on TableDdlContext.  Keep table_ddl_kind_names[] in sync
 * with the order of additions here.
 */
typedef enum TableDdlKind
{
	TABLE_DDL_KIND_TABLE,
	TABLE_DDL_KIND_INDEX,
	TABLE_DDL_KIND_PRIMARY_KEY,
	TABLE_DDL_KIND_UNIQUE,
	TABLE_DDL_KIND_CHECK,
	TABLE_DDL_KIND_FOREIGN_KEY,
	TABLE_DDL_KIND_EXCLUSION,
	TABLE_DDL_KIND_RULE,
	TABLE_DDL_KIND_STATISTICS,
	TABLE_DDL_KIND_TRIGGER,
	TABLE_DDL_KIND_POLICY,
	TABLE_DDL_KIND_RLS,
	TABLE_DDL_KIND_REPLICA_IDENTITY,
	TABLE_DDL_KIND_PARTITION,
}			TableDdlKind;

static const struct
{
	const char *name;
	TableDdlKind kind;
}			table_ddl_kind_names[] =
{
	{"table", TABLE_DDL_KIND_TABLE},
	{"index", TABLE_DDL_KIND_INDEX},
	{"primary_key", TABLE_DDL_KIND_PRIMARY_KEY},
	{"unique", TABLE_DDL_KIND_UNIQUE},
	{"check", TABLE_DDL_KIND_CHECK},
	{"foreign_key", TABLE_DDL_KIND_FOREIGN_KEY},
	{"exclusion", TABLE_DDL_KIND_EXCLUSION},
	{"rule", TABLE_DDL_KIND_RULE},
	{"statistics", TABLE_DDL_KIND_STATISTICS},
	{"trigger", TABLE_DDL_KIND_TRIGGER},
	{"policy", TABLE_DDL_KIND_POLICY},
	{"rls", TABLE_DDL_KIND_RLS},
	{"replica_identity", TABLE_DDL_KIND_REPLICA_IDENTITY},
	{"partition", TABLE_DDL_KIND_PARTITION},
};

static Bitmapset *parse_kind_array(const char *paramname, ArrayType *arr);
static void append_ddl_option(StringInfo buf, bool pretty, int indent,
							  const char *fmt, ...)
			pg_attribute_printf(4, 5);
static void append_guc_value(StringInfo buf, const char *name,
							 const char *value);
static List *pg_get_role_ddl_internal(Oid roleid, bool pretty,
									  bool memberships);
static List *pg_get_tablespace_ddl_internal(Oid tsid, bool pretty, bool no_owner);
static Datum pg_get_tablespace_ddl_srf(FunctionCallInfo fcinfo, Oid tsid);
static List *pg_get_database_ddl_internal(Oid dbid, bool pretty,
										  bool no_owner, bool no_tablespace);

/*
 * Per-column cache of locally-declared NOT NULL constraints.  Built once
 * by collect_local_not_null() and consulted by the column-emit helpers
 * and the post-CREATE constraint loop.  Entries with conoid == InvalidOid
 * mean the column has no local NOT NULL constraint row in pg_constraint
 * (the column may still have attnotnull=true on a pre-PG-18-upgraded
 * catalog, in which case emit plain inline NOT NULL).
 *
 * inherited_notnull is set when a NOT NULL constraint row exists in
 * pg_constraint but conislocal=false (purely inherited).  In that case
 * the NOT NULL must not be emitted in the column list: the INHERITS /
 * PARTITION OF clause already propagates it, and emitting it here would
 * flip conislocal to true on replay.
 */
typedef struct LocalNotNullEntry
{
	Oid			conoid;
	char	   *name;
	bool		is_auto;		/* matches "<table>_<col>_not_null" */
	bool		no_inherit;
	bool		inherited_notnull;	/* row exists but conislocal=false */
	bool		not_valid;		/* constraint has convalidated=false */
}			LocalNotNullEntry;

/*
 * Working context threaded through the per-pass helpers below.  Inputs
 * (caller-provided option flags) are filled in once at the top of
 * pg_get_table_ddl_internal and treated as read-only thereafter.  Derived
 * fields (qualname, nn_entries, skip_notnull_oids) are computed once
 * during setup.  Each pass appends to ctx->buf and pushes the finished
 * statement onto ctx->statements via append_stmt().
 */
typedef struct TableDdlContext
{
	Relation	rel;
	Oid			relid;
	bool		pretty;
	bool		no_owner;
	bool		no_tablespace;
	bool		schema_qualified;

	/*
	 * Object-class filtering.  If only_kinds is non-NULL, only the
	 * kinds in that set are emitted; if except_kinds is non-NULL, all
	 * kinds *except* those in that set are emitted; if both are NULL,
	 * every kind is emitted (the default).  The two are mutually
	 * exclusive at the user-facing layer (the SRF entry rejects
	 * specifying both).
	 */
	Bitmapset  *only_kinds;
	Bitmapset  *except_kinds;

	/* Derived during setup */
	Oid			base_namespace;
	char	   *qualname;
	int			save_nestlevel; /* >= 0 if we narrowed search_path */
	LocalNotNullEntry *nn_entries;
	List	   *skip_notnull_oids;

	/* Mutable working state */
	StringInfoData buf;
	List	   *statements;
}			TableDdlContext;

static List *pg_get_table_ddl_internal(TableDdlContext *ctx);
static bool is_kind_included(const TableDdlContext *ctx, TableDdlKind kind);
static void append_column_defs(StringInfo buf, Relation rel, bool pretty,
							   bool include_check,
							   bool schema_qualified,
							   LocalNotNullEntry *nn_entries);
static void append_typed_column_overrides(StringInfo buf, Relation rel,
										  bool pretty, bool include_check,
										  LocalNotNullEntry *nn_entries);
static void append_inline_check_constraints(StringInfo buf, Relation rel,
											bool pretty, bool *first);
static char *find_attrdef_text(Relation rel, AttrNumber attnum,
							   List **dpcontext);
static bool child_default_inherited_from_parent(Oid childOid,
												 const char *attname,
												 const char *child_adbin);
static char *format_name_for_emit(const char *name, Oid nsp,
								  bool schema_qualified, Oid base_namespace);
static char *lookup_relname_for_emit(Oid relid, bool schema_qualified,
									 Oid base_namespace);
static LocalNotNullEntry *collect_local_not_null(Relation rel,
												  List **skip_oids);
static bool parent_has_default_for_col(Oid childOid, const char *attname);

static void append_stmt(TableDdlContext *ctx);
static void emit_create_table_stmt(TableDdlContext *ctx);
static void emit_owner_stmt(TableDdlContext *ctx);
static void emit_child_default_overrides(TableDdlContext *ctx);
static void emit_attoptions(TableDdlContext *ctx);
static void emit_typed_column_storage(TableDdlContext *ctx);
static void emit_identity_sequence_alterations(TableDdlContext *ctx);
static void emit_index_column_statistics(TableDdlContext *ctx, Oid idxoid,
										 int indnkeyatts);
static void emit_indexes(TableDdlContext *ctx);
static TableDdlKind index_ddl_kind(Oid idxoid);
static void emit_cluster_on(TableDdlContext *ctx);
static void emit_local_constraints(TableDdlContext *ctx);
static void emit_rules(TableDdlContext *ctx);
static void emit_statistics(TableDdlContext *ctx);
static void emit_replica_identity(TableDdlContext *ctx);
static void emit_rls_toggles(TableDdlContext *ctx);
static void emit_partition_children(TableDdlContext *ctx);


/*
 * parse_kind_array
 *		Parse a text[] of object-class kind names into a Bitmapset of
 *		TableDdlKind values.
 *
 * Each element is matched case-insensitively; surrounding whitespace is
 * stripped.  NULL elements are rejected.  Unknown kind names raise an error
 * citing the supplied parameter name.  An empty array is also rejected.
 * Duplicate entries are silently de-duplicated by the Bitmapset.
 */
static Bitmapset *
parse_kind_array(const char *paramname, ArrayType *arr)
{
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	Bitmapset  *result = NULL;

	deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &nelems);

	if (nelems == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" must specify at least one kind",
						paramname)));

	for (int i = 0; i < nelems; i++)
	{
		char	   *raw;
		char	   *token;
		char	   *end;
		bool		found = false;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("parameter \"%s\" must not contain NULL elements",
							paramname)));

		raw = text_to_cstring(DatumGetTextPP(elems[i]));

		/* Trim leading whitespace. */
		token = raw;
		while (*token == ' ' || *token == '\t')
			token++;

		/* Trim trailing whitespace. */
		end = token + strlen(token);
		while (end > token && (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		*end = '\0';

		for (size_t j = 0; j < lengthof(table_ddl_kind_names); j++)
		{
			if (pg_strcasecmp(token, table_ddl_kind_names[j].name) == 0)
			{
				result = bms_add_member(result,
										(int) table_ddl_kind_names[j].kind);
				found = true;
				break;
			}
		}

		if (!found)
		{
			/*
			 * token points into raw, so copy it before freeing raw to avoid
			 * a use-after-free in the error message.
			 */
			char	   *bad_kind = pstrdup(token);

			pfree(raw);
			pfree(elems);
			pfree(nulls);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized kind \"%s\" in parameter \"%s\"",
							bad_kind, paramname)));
		}

		pfree(raw);
	}

	pfree(elems);
	pfree(nulls);
	return result;
}

/*
 * is_kind_included
 *		Determine whether DDL for the given object-class kind should be
 *		emitted under the current options.
 */
static bool
is_kind_included(const TableDdlContext *ctx, TableDdlKind kind)
{
	if (ctx->only_kinds != NULL)
		return bms_is_member((int) kind, ctx->only_kinds);
	if (ctx->except_kinds != NULL)
		return !bms_is_member((int) kind, ctx->except_kinds);
	return true;
}

/*
 * Helper to append a formatted string with optional pretty-printing.
 */
static void
append_ddl_option(StringInfo buf, bool pretty, int indent,
				  const char *fmt, ...)
{
	if (pretty)
	{
		appendStringInfoChar(buf, '\n');
		appendStringInfoSpaces(buf, indent);
	}
	else
		appendStringInfoChar(buf, ' ');

	for (;;)
	{
		va_list		args;
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(buf, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(buf, needed);
	}
}

/*
 * append_guc_value
 *		Append a GUC setting value to buf, handling GUC_LIST_QUOTE properly.
 *
 * Variables marked GUC_LIST_QUOTE were already fully quoted before they
 * were stored in the setconfig array.  We break the list value apart
 * and re-quote the elements as string literals.  For all other variables
 * we simply quote the value as a single string literal.
 *
 * The caller has already appended "SET <name> TO " to buf.
 */
static void
append_guc_value(StringInfo buf, const char *name, const char *value)
{
	char	   *rawval;

	rawval = pstrdup(value);

	if (GetConfigOptionFlags(name, true) & GUC_LIST_QUOTE)
	{
		List	   *namelist;
		bool		first = true;

		/* Parse string into list of identifiers */
		if (!SplitGUCList(rawval, ',', &namelist))
		{
			/* this shouldn't fail really */
			elog(ERROR, "invalid list syntax in setconfig item");
		}
		/* Special case: represent an empty list as NULL */
		if (namelist == NIL)
			appendStringInfoString(buf, "NULL");
		foreach_ptr(char, curname, namelist)
		{
			if (first)
				first = false;
			else
				appendStringInfoString(buf, ", ");
			appendStringInfoString(buf, quote_literal_cstr(curname));
		}
		list_free(namelist);
	}
	else
		appendStringInfoString(buf, quote_literal_cstr(rawval));

	pfree(rawval);
}

/*
 * pg_get_role_ddl_internal
 *		Generate DDL statements to recreate a role
 *
 * Returns a List of palloc'd strings, each being a complete SQL statement.
 * The first list element is always the CREATE ROLE statement; subsequent
 * elements are ALTER ROLE SET statements for any role-specific or
 * role-in-database configuration settings.  If memberships is true,
 * GRANT statements for role memberships are appended.
 */
static List *
pg_get_role_ddl_internal(Oid roleid, bool pretty, bool memberships)
{
	HeapTuple	tuple;
	Form_pg_authid roleform;
	StringInfoData buf;
	char	   *rolname;
	Datum		rolevaliduntil;
	bool		isnull;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	List	   *statements = NIL;

	tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role with OID %u does not exist", roleid)));

	roleform = (Form_pg_authid) GETSTRUCT(tuple);
	rolname = pstrdup(NameStr(roleform->rolname));

	/* User must have SELECT privilege on pg_authid. */
	if (pg_class_aclcheck(AuthIdRelationId, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for role %s", rolname)));
	}

	/*
	 * We don't support generating DDL for system roles.  The primary reason
	 * for this is that users shouldn't be recreating them.
	 */
	if (IsReservedName(rolname))
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved", rolname),
				 errdetail("Role names starting with \"pg_\" are reserved for system roles.")));
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE ROLE %s", quote_identifier(rolname));

	/*
	 * Append role attributes.  The order here follows the same sequence as
	 * you'd typically write them in a CREATE ROLE command, though any order
	 * is actually acceptable to the parser.
	 */
	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolsuper ? "SUPERUSER" : "NOSUPERUSER");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolinherit ? "INHERIT" : "NOINHERIT");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcreaterole ? "CREATEROLE" : "NOCREATEROLE");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcreatedb ? "CREATEDB" : "NOCREATEDB");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolcanlogin ? "LOGIN" : "NOLOGIN");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolreplication ? "REPLICATION" : "NOREPLICATION");

	append_ddl_option(&buf, pretty, 4, "%s",
					  roleform->rolbypassrls ? "BYPASSRLS" : "NOBYPASSRLS");

	/*
	 * CONNECTION LIMIT is only interesting if it's not -1 (the default,
	 * meaning no limit).
	 */
	if (roleform->rolconnlimit >= 0)
		append_ddl_option(&buf, pretty, 4, "CONNECTION LIMIT %d",
						  roleform->rolconnlimit);

	rolevaliduntil = SysCacheGetAttr(AUTHOID, tuple,
									 Anum_pg_authid_rolvaliduntil,
									 &isnull);
	if (!isnull)
	{
		TimestampTz ts;
		int			tz;
		struct pg_tm tm;
		fsec_t		fsec;
		const char *tzn;
		char		ts_str[MAXDATELEN + 1];

		ts = DatumGetTimestampTz(rolevaliduntil);
		if (TIMESTAMP_NOT_FINITE(ts))
			EncodeSpecialTimestamp(ts, ts_str);
		else if (timestamp2tm(ts, &tz, &tm, &fsec, &tzn, NULL) == 0)
			EncodeDateTime(&tm, fsec, true, tz, tzn, USE_ISO_DATES, ts_str);
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		append_ddl_option(&buf, pretty, 4, "VALID UNTIL %s",
						  quote_literal_cstr(ts_str));
	}

	ReleaseSysCache(tuple);

	/*
	 * We intentionally omit PASSWORD.  There's no way to retrieve the
	 * original password text from the stored hash, and even if we could,
	 * exposing passwords through a SQL function would be a security issue.
	 * Users must set passwords separately after recreating roles.
	 */

	appendStringInfoChar(&buf, ';');

	statements = lappend(statements, pstrdup(buf.data));

	/*
	 * Now scan pg_db_role_setting for ALTER ROLE SET configurations.
	 *
	 * These can be role-wide (setdatabase = 0) or specific to a particular
	 * database (setdatabase = a valid DB OID).  It generates one ALTER
	 * statement per setting.
	 */
	rel = table_open(DbRoleSettingRelationId, AccessShareLock);
	ScanKeyInit(&scankey,
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(roleid));
	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  NULL, 1, &scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_db_role_setting setting = (Form_pg_db_role_setting) GETSTRUCT(tuple);
		Oid			datid = setting->setdatabase;
		Datum		datum;
		ArrayType  *role_settings;
		Datum	   *settings;
		bool	   *nulls;
		int			nsettings;
		char	   *datname = NULL;

		/*
		 * If setdatabase is valid, this is a role-in-database setting;
		 * otherwise it's a role-wide setting.  Look up the database name once
		 * for all settings in this row.
		 */
		if (OidIsValid(datid))
		{
			datname = get_database_name(datid);
			/* Database has been dropped; skip all settings in this row. */
			if (datname == NULL)
				continue;
		}

		/*
		 * The setconfig column is a text array in "name=value" format. It
		 * should never be null for a valid row, but be defensive.
		 */
		datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(rel), &isnull);
		if (isnull)
			continue;

		role_settings = DatumGetArrayTypePCopy(datum);

		deconstruct_array_builtin(role_settings, TEXTOID, &settings, &nulls, &nsettings);

		for (int i = 0; i < nsettings; i++)
		{
			char	   *s,
					   *p;

			if (nulls[i])
				continue;

			s = TextDatumGetCString(settings[i]);
			p = strchr(s, '=');
			if (p == NULL)
			{
				pfree(s);
				continue;
			}
			*p++ = '\0';

			/* Build a fresh ALTER ROLE statement for this setting */
			resetStringInfo(&buf);
			appendStringInfo(&buf, "ALTER ROLE %s", quote_identifier(rolname));

			if (datname != NULL)
				appendStringInfo(&buf, " IN DATABASE %s",
								 quote_identifier(datname));

			appendStringInfo(&buf, " SET %s TO ",
							 quote_identifier(s));

			append_guc_value(&buf, s, p);

			appendStringInfoChar(&buf, ';');

			statements = lappend(statements, pstrdup(buf.data));

			pfree(s);
		}

		pfree(settings);
		pfree(nulls);
		pfree(role_settings);

		if (datname != NULL)
			pfree(datname);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * Scan pg_auth_members for role memberships.  We look for rows where
	 * member = roleid, meaning this role has been granted membership in other
	 * roles.
	 */
	if (memberships)
	{
		rel = table_open(AuthMemRelationId, AccessShareLock);
		ScanKeyInit(&scankey,
					Anum_pg_auth_members_member,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));
		scan = systable_beginscan(rel, AuthMemMemRoleIndexId, true,
								  NULL, 1, &scankey);

		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Form_pg_auth_members memform = (Form_pg_auth_members) GETSTRUCT(tuple);
			char	   *granted_role;
			char	   *grantor;

			granted_role = GetUserNameFromId(memform->roleid, false);
			grantor = GetUserNameFromId(memform->grantor, false);

			resetStringInfo(&buf);
			appendStringInfo(&buf, "GRANT %s TO %s",
							 quote_identifier(granted_role),
							 quote_identifier(rolname));
			appendStringInfo(&buf, " WITH ADMIN %s, INHERIT %s, SET %s",
							 memform->admin_option ? "TRUE" : "FALSE",
							 memform->inherit_option ? "TRUE" : "FALSE",
							 memform->set_option ? "TRUE" : "FALSE");
			appendStringInfo(&buf, " GRANTED BY %s;",
							 quote_identifier(grantor));

			statements = lappend(statements, pstrdup(buf.data));

			pfree(granted_role);
			pfree(grantor);
		}

		systable_endscan(scan);
		table_close(rel, AccessShareLock);
	}

	pfree(buf.data);
	pfree(rolname);

	return statements;
}

/*
 * pg_get_role_ddl
 *		Return DDL to recreate a role as a set of text rows.
 *
 * Each row is a complete SQL statement.  The first row is always the
 * CREATE ROLE statement; subsequent rows are ALTER ROLE SET statements
 * and optionally GRANT statements for role memberships.
 */
Datum
pg_get_role_ddl(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			roleid;
		bool		pretty;
		bool		memberships;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		roleid = PG_GETARG_OID(0);
		pretty = PG_GETARG_BOOL(1);
		memberships = PG_GETARG_BOOL(2);

		statements = pg_get_role_ddl_internal(roleid, pretty, memberships);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * pg_get_tablespace_ddl_internal
 *		Generate DDL statements to recreate a tablespace.
 *
 * Returns a List of palloc'd strings.  The first element is the
 * CREATE TABLESPACE statement; if the tablespace has reloptions,
 * a second element with ALTER TABLESPACE SET (...) is appended.
 */
static List *
pg_get_tablespace_ddl_internal(Oid tsid, bool pretty, bool no_owner)
{
	HeapTuple	tuple;
	Form_pg_tablespace tspForm;
	StringInfoData buf;
	char	   *spcname;
	char	   *spcowner;
	char	   *path;
	bool		isNull;
	Datum		datum;
	List	   *statements = NIL;

	tuple = SearchSysCache1(TABLESPACEOID, ObjectIdGetDatum(tsid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with OID %u does not exist",
						tsid)));

	tspForm = (Form_pg_tablespace) GETSTRUCT(tuple);
	spcname = pstrdup(NameStr(tspForm->spcname));

	/* User must have SELECT privilege on pg_tablespace. */
	if (pg_class_aclcheck(TableSpaceRelationId, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
	{
		ReleaseSysCache(tuple);
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE, spcname);
	}

	/*
	 * We don't support generating DDL for system tablespaces.  The primary
	 * reason for this is that users shouldn't be recreating them.
	 */
	if (IsReservedName(spcname))
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("tablespace name \"%s\" is reserved", spcname),
				 errdetail("Tablespace names starting with \"pg_\" are reserved for system tablespaces.")));
	}

	initStringInfo(&buf);

	/* Start building the CREATE TABLESPACE statement */
	appendStringInfo(&buf, "CREATE TABLESPACE %s", quote_identifier(spcname));

	/* Add OWNER clause */
	if (!no_owner)
	{
		spcowner = GetUserNameFromId(tspForm->spcowner, false);
		append_ddl_option(&buf, pretty, 4, "OWNER %s",
						  quote_identifier(spcowner));
		pfree(spcowner);
	}

	/* Find tablespace directory path */
	path = get_tablespace_location(tsid);

	/* Add directory LOCATION (path), if it exists */
	if (path[0] != '\0')
	{
		/*
		 * Special case: if the tablespace was created with GUC
		 * "allow_in_place_tablespaces = true" and "LOCATION ''", path will
		 * begin with "pg_tblspc/". In that case, show "LOCATION ''" as the
		 * user originally specified.
		 */
		if (strncmp(PG_TBLSPC_DIR_SLASH, path, strlen(PG_TBLSPC_DIR_SLASH)) == 0)
			append_ddl_option(&buf, pretty, 4, "LOCATION ''");
		else
			append_ddl_option(&buf, pretty, 4, "LOCATION %s",
							  quote_literal_cstr(path));
	}
	pfree(path);

	appendStringInfoChar(&buf, ';');
	statements = lappend(statements, pstrdup(buf.data));

	/* Check for tablespace options */
	datum = SysCacheGetAttr(TABLESPACEOID, tuple,
							Anum_pg_tablespace_spcoptions, &isNull);
	if (!isNull)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER TABLESPACE %s SET (",
						 quote_identifier(spcname));
		get_reloptions(&buf, datum);
		appendStringInfoString(&buf, ");");
		statements = lappend(statements, pstrdup(buf.data));
	}

	ReleaseSysCache(tuple);
	pfree(spcname);
	pfree(buf.data);

	return statements;
}

/*
 * pg_get_tablespace_ddl_srf - common SRF logic for tablespace DDL
 */
static Datum
pg_get_tablespace_ddl_srf(FunctionCallInfo fcinfo, Oid tsid)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		bool		pretty;
		bool		no_owner;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		pretty = PG_GETARG_BOOL(1);
		no_owner = !PG_GETARG_BOOL(2);

		statements = pg_get_tablespace_ddl_internal(tsid, pretty, no_owner);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = (char *) list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * pg_get_tablespace_ddl_oid
 *		Return DDL to recreate a tablespace, taking OID.
 */
Datum
pg_get_tablespace_ddl_oid(PG_FUNCTION_ARGS)
{
	Oid			tsid = PG_GETARG_OID(0);

	return pg_get_tablespace_ddl_srf(fcinfo, tsid);
}

/*
 * pg_get_tablespace_ddl_name
 *		Return DDL to recreate a tablespace, taking name.
 */
Datum
pg_get_tablespace_ddl_name(PG_FUNCTION_ARGS)
{
	Name		tspname = PG_GETARG_NAME(0);
	Oid			tsid = get_tablespace_oid(NameStr(*tspname), false);

	return pg_get_tablespace_ddl_srf(fcinfo, tsid);
}

/*
 * pg_get_database_ddl_internal
 *		Generate DDL statements to recreate a database.
 *
 * Returns a List of palloc'd strings.  The first element is the
 * CREATE DATABASE statement; subsequent elements are ALTER DATABASE
 * statements for properties and configuration settings.
 */
static List *
pg_get_database_ddl_internal(Oid dbid, bool pretty,
							 bool no_owner, bool no_tablespace)
{
	HeapTuple	tuple;
	Form_pg_database dbform;
	StringInfoData buf;
	bool		isnull;
	Datum		datum;
	const char *encoding;
	char	   *dbname;
	char	   *collate;
	char	   *ctype;
	Relation	rel;
	ScanKeyData scankey[2];
	SysScanDesc scan;
	List	   *statements = NIL;
	AclResult	aclresult;

	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("database with OID %u does not exist", dbid)));

	/* User must have connect privilege for target database. */
	aclresult = object_aclcheck(DatabaseRelationId, dbid, GetUserId(), ACL_CONNECT);
	if (aclresult != ACLCHECK_OK)
	{
		ReleaseSysCache(tuple);
		aclcheck_error(aclresult, OBJECT_DATABASE, get_database_name(dbid));
	}

	dbform = (Form_pg_database) GETSTRUCT(tuple);
	dbname = pstrdup(NameStr(dbform->datname));

	/*
	 * Reject invalid databases. Deparsing a pg_database row in invalid state
	 * can produce SQL that is not executable, such as CONNECTION LIMIT = -2.
	 */
	if (database_is_invalid_form(dbform))
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot generate DDL for invalid database \"%s\"",
						dbname)));
	}

	/*
	 * We don't support generating DDL for system databases.  The primary
	 * reason for this is that users shouldn't be recreating them.
	 */
	if (strcmp(dbname, "template0") == 0 || strcmp(dbname, "template1") == 0)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("database \"%s\" is a system database", dbname),
				 errdetail("DDL generation is not supported for template0 and template1.")));
	}

	initStringInfo(&buf);

	/* --- Build CREATE DATABASE statement --- */
	appendStringInfo(&buf, "CREATE DATABASE %s", quote_identifier(dbname));

	/*
	 * Always use template0: the target database already contains the catalog
	 * data from whatever template was used originally, so we must start from
	 * the pristine template to avoid duplication.
	 */
	append_ddl_option(&buf, pretty, 4, "WITH TEMPLATE = template0");

	/* ENCODING */
	encoding = pg_encoding_to_char(dbform->encoding);
	if (strlen(encoding) > 0)
		append_ddl_option(&buf, pretty, 4, "ENCODING = %s",
						  quote_literal_cstr(encoding));

	/* LOCALE_PROVIDER */
	if (dbform->datlocprovider == COLLPROVIDER_BUILTIN ||
		dbform->datlocprovider == COLLPROVIDER_ICU ||
		dbform->datlocprovider == COLLPROVIDER_LIBC)
		append_ddl_option(&buf, pretty, 4, "LOCALE_PROVIDER = %s",
						  collprovider_name(dbform->datlocprovider));
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("unrecognized locale provider: %c",
						dbform->datlocprovider)));

	/* LOCALE, LC_COLLATE, LC_CTYPE */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datcollate, &isnull);
	collate = isnull ? NULL : TextDatumGetCString(datum);
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datctype, &isnull);
	ctype = isnull ? NULL : TextDatumGetCString(datum);
	if (collate != NULL && ctype != NULL && strcmp(collate, ctype) == 0)
	{
		append_ddl_option(&buf, pretty, 4, "LOCALE = %s",
						  quote_literal_cstr(collate));
	}
	else
	{
		if (collate != NULL)
			append_ddl_option(&buf, pretty, 4, "LC_COLLATE = %s",
							  quote_literal_cstr(collate));
		if (ctype != NULL)
			append_ddl_option(&buf, pretty, 4, "LC_CTYPE = %s",
							  quote_literal_cstr(ctype));
	}

	/* LOCALE (provider-specific) */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_datlocale, &isnull);
	if (!isnull)
	{
		const char *locale = TextDatumGetCString(datum);

		if (dbform->datlocprovider == COLLPROVIDER_BUILTIN)
			append_ddl_option(&buf, pretty, 4, "BUILTIN_LOCALE = %s",
							  quote_literal_cstr(locale));
		else if (dbform->datlocprovider == COLLPROVIDER_ICU)
			append_ddl_option(&buf, pretty, 4, "ICU_LOCALE = %s",
							  quote_literal_cstr(locale));
	}

	/* ICU_RULES */
	datum = SysCacheGetAttr(DATABASEOID, tuple,
							Anum_pg_database_daticurules, &isnull);
	if (!isnull && dbform->datlocprovider == COLLPROVIDER_ICU)
		append_ddl_option(&buf, pretty, 4, "ICU_RULES = %s",
						  quote_literal_cstr(TextDatumGetCString(datum)));

	/* TABLESPACE */
	if (!no_tablespace && OidIsValid(dbform->dattablespace))
	{
		char	   *spcname = get_tablespace_name(dbform->dattablespace);

		if (spcname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace with OID %u does not exist",
							dbform->dattablespace),
					 errdetail("It may have been concurrently dropped.")));

		if (pg_strcasecmp(spcname, "pg_default") != 0)
			append_ddl_option(&buf, pretty, 4, "TABLESPACE = %s",
							  quote_identifier(spcname));
	}

	appendStringInfoChar(&buf, ';');
	statements = lappend(statements, pstrdup(buf.data));

	/* OWNER */
	if (!no_owner && OidIsValid(dbform->datdba))
	{
		char	   *owner = GetUserNameFromId(dbform->datdba, false);

		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s OWNER TO %s;",
						 quote_identifier(dbname), quote_identifier(owner));
		pfree(owner);
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* CONNECTION LIMIT */
	if (dbform->datconnlimit != -1)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s CONNECTION LIMIT = %d;",
						 quote_identifier(dbname), dbform->datconnlimit);
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* IS_TEMPLATE */
	if (dbform->datistemplate)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s IS_TEMPLATE = true;",
						 quote_identifier(dbname));
		statements = lappend(statements, pstrdup(buf.data));
	}

	/* ALLOW_CONNECTIONS */
	if (!dbform->datallowconn)
	{
		resetStringInfo(&buf);
		appendStringInfo(&buf, "ALTER DATABASE %s ALLOW_CONNECTIONS = false;",
						 quote_identifier(dbname));
		statements = lappend(statements, pstrdup(buf.data));
	}

	ReleaseSysCache(tuple);

	/*
	 * Now scan pg_db_role_setting for ALTER DATABASE SET configurations.
	 *
	 * It is only database-wide (setrole = 0). It generates one ALTER
	 * statement per setting.
	 */
	rel = table_open(DbRoleSettingRelationId, AccessShareLock);
	ScanKeyInit(&scankey[0],
				Anum_pg_db_role_setting_setdatabase,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));
	ScanKeyInit(&scankey[1],
				Anum_pg_db_role_setting_setrole,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));

	scan = systable_beginscan(rel, DbRoleSettingDatidRolidIndexId, true,
							  NULL, 2, scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ArrayType  *dbconfig;
		Datum	   *settings;
		bool	   *nulls;
		int			nsettings;

		/*
		 * The setconfig column is a text array in "name=value" format. It
		 * should never be null for a valid row, but be defensive.
		 */
		datum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
							 RelationGetDescr(rel), &isnull);
		if (isnull)
			continue;

		dbconfig = DatumGetArrayTypePCopy(datum);

		deconstruct_array_builtin(dbconfig, TEXTOID, &settings, &nulls, &nsettings);

		for (int i = 0; i < nsettings; i++)
		{
			char	   *s,
					   *p;

			if (nulls[i])
				continue;

			s = TextDatumGetCString(settings[i]);
			p = strchr(s, '=');
			if (p == NULL)
			{
				pfree(s);
				continue;
			}
			*p++ = '\0';

			resetStringInfo(&buf);
			appendStringInfo(&buf, "ALTER DATABASE %s SET %s TO ",
							 quote_identifier(dbname),
							 quote_identifier(s));

			append_guc_value(&buf, s, p);

			appendStringInfoChar(&buf, ';');

			statements = lappend(statements, pstrdup(buf.data));

			pfree(s);
		}

		pfree(settings);
		pfree(nulls);
		pfree(dbconfig);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	pfree(buf.data);
	pfree(dbname);

	return statements;
}

/*
 * pg_get_database_ddl
 *		Return DDL to recreate a database as a set of text rows.
 */
Datum
pg_get_database_ddl(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Oid			dbid;
		bool		pretty;
		bool		no_owner;
		bool		no_tablespace;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		dbid = PG_GETARG_OID(0);
		pretty = PG_GETARG_BOOL(1);
		no_owner = !PG_GETARG_BOOL(2);
		no_tablespace = !PG_GETARG_BOOL(3);

		statements = pg_get_database_ddl_internal(dbid, pretty, no_owner,
												  no_tablespace);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * format_name_for_emit
 *		Return a (possibly schema-qualified) SQL identifier for a non-relation
 *		object (statistics, sequences, etc.) given its name and namespace OID.
 *		Follows the same schema_qualified / base_namespace semantics as
 *		lookup_relname_for_emit: bare name only when schema_qualified is false
 *		and the object lives in base_namespace; qualified otherwise.
 *
 * Caller is responsible for pfree()ing the result.
 */
static char *
format_name_for_emit(const char *name, Oid nsp,
					 bool schema_qualified, Oid base_namespace)
{
	if (!schema_qualified && nsp == base_namespace)
		return pstrdup(quote_identifier(name));

	{
		char	   *nspname = get_namespace_name(nsp);
		char	   *result;

		if (nspname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("schema with OID %u does not exist", nsp),
					 errdetail("It may have been concurrently dropped.")));
		result = quote_qualified_identifier(nspname, name);
		pfree(nspname);
		return result;
	}
}

/*
 * lookup_relname_for_emit
 *		Return either the schema-qualified or the bare quoted name of a
 *		relation, depending on the schema_qualified flag.
 *
 * Temporary relations are never schema-qualified regardless of
 * schema_qualified: the TEMPORARY keyword in CREATE TEMPORARY TABLE
 * already places the table in the session's temp schema, so emitting
 * pg_temp_NN.relname would produce DDL that cannot be replayed.
 *
 * When schema_qualified is true the schema-qualified name is always
 * returned for non-temporary relations.  When false, the bare relname
 * is returned only if the target relation lives in base_namespace (the
 * namespace of the table whose DDL is being generated); otherwise the
 * schema-qualified form is returned, because cross-schema references
 * (for example an inheritance parent or foreign key target in a
 * different schema) are not safe to omit.
 *
 * This replaces the unsafe pattern
 *	  quote_qualified_identifier(get_namespace_name(get_rel_namespace(oid)),
 *	                             get_rel_name(oid))
 * which dereferences NULL when a concurrent transaction has dropped the
 * referenced relation (or its schema) between when we cached its OID and
 * when we ask the syscache for its name.  Holding AccessShareLock on a
 * dependent relation makes this race vanishingly unlikely in practice, but
 * we still defend against it because the alternative is a SIGSEGV.
 *
 * Caller is responsible for pfree()ing the result.
 */
static char *
lookup_relname_for_emit(Oid relid, bool schema_qualified, Oid base_namespace)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("relation with OID %u does not exist", relid),
				 errdetail("It may have been concurrently dropped.")));

	reltup = (Form_pg_class) GETSTRUCT(tp);

	/*
	 * Temporary relations are never schema-qualified: the TEMPORARY keyword
	 * already places them in pg_temp, and pg_temp_NN.relname cannot be
	 * replayed in any other session.
	 */
	if (isTempNamespace(reltup->relnamespace))
	{
		result = pstrdup(quote_identifier(NameStr(reltup->relname)));
		ReleaseSysCache(tp);
		return result;
	}

	/* Bare name only when caller asked and target is in the base namespace. */
	if (!schema_qualified && reltup->relnamespace == base_namespace)
	{
		result = pstrdup(quote_identifier(NameStr(reltup->relname)));
		ReleaseSysCache(tp);
		return result;
	}

	nspname = get_namespace_name(reltup->relnamespace);
	if (nspname == NULL)
	{
		Oid			nspoid = reltup->relnamespace;

		ReleaseSysCache(tp);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("schema with OID %u does not exist", nspoid),
				 errdetail("It may have been concurrently dropped.")));
	}

	result = quote_qualified_identifier(nspname, NameStr(reltup->relname));

	pfree(nspname);
	ReleaseSysCache(tp);

	return result;
}

/*
 * collect_local_not_null
 *		Scan pg_constraint once for locally-declared NOT NULL constraints
 *		on rel, returning a palloc'd array indexed by attnum (1..natts).
 *
 * Entries with conoid==InvalidOid mean "no local NOT NULL row for this
 * column".  When the constraint name does not match the auto-generated
 * pattern "<tablename>_<columnname>_not_null", the constraint OID is
 * appended to *skip_oids so the post-CREATE constraint loop can avoid
 * re-emitting it as ALTER TABLE ... ADD CONSTRAINT - the column-emit
 * pass will produce it inline as "CONSTRAINT name NOT NULL" instead.
 */
static LocalNotNullEntry *
collect_local_not_null(Relation rel, List **skip_oids)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			natts = tupdesc->natts;
	LocalNotNullEntry *entries;
	const char *relname = RelationGetRelationName(rel);
	Relation	conRel;
	SysScanDesc conScan;
	ScanKeyData conKey;
	HeapTuple	conTup;

	entries = (LocalNotNullEntry *) palloc0(sizeof(LocalNotNullEntry) *
											(natts + 1));

	conRel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&conKey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	conScan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId,
								 true, NULL, 1, &conKey);

	while (HeapTupleIsValid(conTup = systable_getnext(conScan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);
		Datum		conkeyDat;
		bool		conkeyNull;
		ArrayType  *conkeyArr;
		int16	   *conkeyVals;
		int			attnum;
		Form_pg_attribute att;
		char	   *autoname;

		if (con->contype != CONSTRAINT_NOTNULL)
			continue;

		conkeyDat = heap_getattr(conTup, Anum_pg_constraint_conkey,
								 RelationGetDescr(conRel), &conkeyNull);
		if (conkeyNull)
			continue;
		conkeyArr = DatumGetArrayTypeP(conkeyDat);

		/*
		 * Defend against a malformed conkey: a NOT NULL constraint is always
		 * a single-column 1-D int2 array, but a corrupted catalog or a future
		 * patch that stores wider conkeys mustn't trip us into reading past
		 * the array header.
		 */
		if (ARR_NDIM(conkeyArr) != 1 ||
			ARR_DIMS(conkeyArr)[0] < 1 ||
			ARR_HASNULL(conkeyArr) ||
			ARR_ELEMTYPE(conkeyArr) != INT2OID)
			continue;

		conkeyVals = (int16 *) ARR_DATA_PTR(conkeyArr);
		attnum = conkeyVals[0];
		if (attnum < 1 || attnum > natts)
			continue;

		/*
		 * Inherited-only constraint: record the fact so column-emit helpers
		 * can suppress inline NOT NULL (the parent's INHERITS clause already
		 * propagates it; re-emitting it would flip conislocal to true on
		 * replay).
		 */
		if (!con->conislocal)
		{
			entries[attnum].inherited_notnull = true;
			continue;
		}

		att = TupleDescAttr(tupdesc, attnum - 1);
		autoname = makeObjectName(relname, NameStr(att->attname), "not_null");

		entries[attnum].conoid = con->oid;
		entries[attnum].name = pstrdup(NameStr(con->conname));
		entries[attnum].is_auto = (strcmp(NameStr(con->conname), autoname) == 0);
		entries[attnum].no_inherit = con->connoinherit;
		entries[attnum].not_valid = !con->convalidated;
		pfree(autoname);

		/*
		 * Track which NOT NULL OIDs must not be re-emitted out-of-line by
		 * emit_local_constraints:
		 *
		 * - For attislocal columns: the inline pass in append_column_defs
		 *   materializes the constraint; a second ALTER TABLE would collide.
		 *
		 * - For inherited columns with a user-defined name (!is_auto):
		 *   emit_create_table_stmt emits these as table-level CONSTRAINT
		 *   clauses in the CREATE TABLE or PARTITION OF body so the name is
		 *   preserved when the inherited NOT NULL is established.  An
		 *   out-of-line ALTER TABLE ADD CONSTRAINT would collide with the
		 *   already-propagated inherited NOT NULL.
		 *
		 * - For inherited columns with an auto-name on a partition child:
		 *   PARTITION OF re-creates an equivalent constraint (possibly under
		 *   the parent-derived name), making the out-of-line ALTER TABLE
		 *   redundant and collision-prone.  Suppress it; the auto-name
		 *   change is acceptable.
		 *
		 * For inherited columns with an auto-name on a plain INHERITS child,
		 * the out-of-line ALTER TABLE is still safe and preserves the name.
		 */
		if (skip_oids != NULL &&
			!entries[attnum].not_valid &&	/* don't skip NOT VALID - must go through emit_local_constraints */
			(att->attislocal ||
			 !entries[attnum].is_auto ||
			 rel->rd_rel->relispartition))
			*skip_oids = lappend_oid(*skip_oids, con->oid);
	}
	systable_endscan(conScan);
	table_close(conRel, AccessShareLock);

	return entries;
}

/*
 * find_attrdef_text
 *		Return the deparsed DEFAULT/GENERATED expression for attnum on rel,
 *		or NULL if no entry exists in TupleConstr->defval.
 *
 * The caller passes a List ** so that the deparse context is built lazily
 * and reused across calls (deparse_context_for is not cheap).  Returned
 * string is palloc'd in the current memory context; caller pfree's it.
 */
static char *
find_attrdef_text(Relation rel, AttrNumber attnum, List **dpcontext)
{
	TupleConstr *constr = RelationGetDescr(rel)->constr;

	if (constr == NULL)
		return NULL;

	for (int j = 0; j < constr->num_defval; j++)
	{
		if (constr->defval[j].adnum != attnum)
			continue;

		if (*dpcontext == NIL)
			*dpcontext = deparse_context_for(RelationGetRelationName(rel),
											 RelationGetRelid(rel));

		return deparse_expression(stringToNode(constr->defval[j].adbin),
								  *dpcontext, false, false);
	}
	return NULL;
}

/*
 * append_inline_check_constraints
 *		Emit each locally-declared CHECK constraint on rel as
 *		"CONSTRAINT name <pg_get_constraintdef>", separated by ',' from any
 *		previously-emitted column or constraint.
 *
 * *first tracks whether anything has been emitted on this list yet, so the
 * caller can chain column emission and constraint emission through the same
 * buffer.  Inherited CHECK constraints (!conislocal) come from the parent's
 * DDL and aren't repeated here.
 */
static void
append_inline_check_constraints(StringInfo buf, Relation rel, bool pretty,
								bool *first)
{
	Relation	conRel;
	SysScanDesc conScan;
	ScanKeyData conKey;
	HeapTuple	conTup;

	conRel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&conKey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	conScan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId,
								 true, NULL, 1, &conKey);

	while (HeapTupleIsValid(conTup = systable_getnext(conScan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);
		char	   *defbody;

		if (con->contype != CONSTRAINT_CHECK)
			continue;
		if (!con->conislocal)
			continue;

		/*
		 * NOT VALID constraints must not be emitted inline: CREATE TABLE
		 * always validates the constraint against the (empty) table, so the
		 * reconstructed constraint would come back as validated even though
		 * the source had convalidated = false.  Leave them for
		 * emit_local_constraints, which emits ALTER TABLE ... ADD CONSTRAINT
		 * ... NOT VALID and preserves the flag.
		 */
		if (!con->convalidated)
			continue;

		if (!*first)
			appendStringInfoChar(buf, ',');
		if (pretty)
			appendStringInfoString(buf, "\n    ");
		else if (!*first)
			appendStringInfoChar(buf, ' ');
		*first = false;

		defbody = pg_get_constraintdef_body(con->oid, false);
		appendStringInfo(buf, "CONSTRAINT %s %s",
						 quote_identifier(NameStr(con->conname)),
						 defbody);
		pfree(defbody);
	}
	systable_endscan(conScan);
	table_close(conRel, AccessShareLock);
}

/*
 * append_column_defs
 *		Append the comma-separated column definition list for a table.
 *
 * Emits each non-dropped, locally-declared column as
 *		name type [COLLATE x] [STORAGE s] [COMPRESSION c]
 *		[GENERATED ... | DEFAULT e] [NOT NULL]
 * followed by any locally-declared inline CHECK constraints.  Optional
 * clauses are omitted when their value matches what the system would
 * reapply on round-trip (e.g. type-default COLLATE, type-default STORAGE).
 */
static void
append_column_defs(StringInfo buf, Relation rel, bool pretty,
				   bool include_check,
				   bool schema_qualified,
				   LocalNotNullEntry *nn_entries)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Oid			base_namespace = RelationGetNamespace(rel);
	List	   *dpcontext = NIL;
	bool		first = true;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char	   *typstr;

		if (att->attisdropped)
			continue;

		/*
		 * Columns inherited from a parent are covered by the INHERITS clause,
		 * not the column list, unless the child redeclared them locally
		 * (attislocal=true).
		 */
		if (!att->attislocal)
			continue;

		if (!first)
			appendStringInfoChar(buf, ',');
		if (pretty)
			appendStringInfoString(buf, "\n    ");
		else if (!first)
			appendStringInfoChar(buf, ' ');
		first = false;

		appendStringInfoString(buf, quote_identifier(NameStr(att->attname)));
		appendStringInfoChar(buf, ' ');

		typstr = format_type_with_typemod(att->atttypid, att->atttypmod);
		appendStringInfoString(buf, typstr);
		pfree(typstr);

		/* COLLATE clause, only if it differs from the type's default. */
		if (OidIsValid(att->attcollation) &&
			att->attcollation != get_typcollation(att->atttypid))
			appendStringInfo(buf, " COLLATE %s",
							 generate_collation_name(att->attcollation));

		/* STORAGE clause, only if it differs from the type's default. */
		if (att->attstorage != get_typstorage(att->atttypid))
			appendStringInfo(buf, " STORAGE %s", storage_name(att->attstorage));

		/* COMPRESSION clause, only if explicitly set on the column. */
		if (CompressionMethodIsValid(att->attcompression))
			appendStringInfo(buf, " COMPRESSION %s",
							 GetCompressionMethodName(att->attcompression));

		/*
		 * Look up the default/generated expression text up front; generated
		 * columns have atthasdef=true with an entry in pg_attrdef just like
		 * regular defaults.
		 */
		{
			char	   *defexpr = NULL;

			if (att->atthasdef)
				defexpr = find_attrdef_text(rel, att->attnum, &dpcontext);

			/* GENERATED / IDENTITY / DEFAULT are mutually exclusive. */
			if (att->attgenerated == ATTRIBUTE_GENERATED_STORED && defexpr)
				appendStringInfo(buf, " GENERATED ALWAYS AS (%s) STORED", defexpr);
			else if (att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL && defexpr)
				appendStringInfo(buf, " GENERATED ALWAYS AS (%s) VIRTUAL", defexpr);
			else if (att->attidentity == ATTRIBUTE_IDENTITY_ALWAYS ||
					 att->attidentity == ATTRIBUTE_IDENTITY_BY_DEFAULT)
			{
				const char *idkind =
					(att->attidentity == ATTRIBUTE_IDENTITY_ALWAYS)
					? "ALWAYS" : "BY DEFAULT";
				Oid			seqid = getIdentitySequence(rel, att->attnum, true);

				appendStringInfo(buf, " GENERATED %s AS IDENTITY", idkind);

				/*
				 * Emit only the sequence options that differ from their
				 * defaults - mirroring pg_get_database_ddl's pattern of
				 * omitting values that the system would reapply on its own.
				 */
				if (OidIsValid(seqid))
				{
					HeapTuple	seqTup = SearchSysCache1(SEQRELID,
														 ObjectIdGetDatum(seqid));

					if (HeapTupleIsValid(seqTup))
					{
						Form_pg_sequence seq = (Form_pg_sequence) GETSTRUCT(seqTup);
						StringInfoData opts;
						bool		first_opt = true;
						int64		def_min,
									def_max,
									def_start;
						int64		typ_min,
									typ_max;

						/*
						 * Per-type bounds for the sequence's underlying
						 * integer type.  Must use seq->seqtypid (the actual
						 * sequence type), not att->atttypid (the column type):
						 * when AS <type> is emitted because the two differ,
						 * PostgreSQL derives the default MINVALUE/MAXVALUE
						 * from the sequence type, so comparing against the
						 * column type's bounds would produce incorrect
						 * "changed from default" judgements and emit redundant
						 * options that conflict with the AS clause.
						 */
						switch (seq->seqtypid)
						{
							case INT2OID:
								typ_min = PG_INT16_MIN;
								typ_max = PG_INT16_MAX;
								break;
							case INT4OID:
								typ_min = PG_INT32_MIN;
								typ_max = PG_INT32_MAX;
								break;
							default:
								typ_min = PG_INT64_MIN;
								typ_max = PG_INT64_MAX;
								break;
						}

						if (seq->seqincrement > 0)
						{
							def_min = 1;
							def_max = typ_max;
							def_start = def_min;
						}
						else
						{
							def_min = typ_min;
							def_max = -1;
							def_start = def_max;
						}

						initStringInfo(&opts);

						/*
						 * AS type: never emitted inline.
						 * generateSerialExtraStmts() always prepends
						 * "AS <column-type>" to the sequence options, so
						 * adding a second AS clause here would trigger a
						 * "redundant options" error.  When the sequence type
						 * was changed to something different from the column
						 * type (e.g. via ALTER SEQUENCE ... AS smallint), the
						 * mismatch is handled by a post-CREATE ALTER SEQUENCE
						 * statement emitted by emit_identity_sequence_alterations().
						 */
						/*
						 * SEQUENCE NAME - omit when it matches the implicit
						 * "<tablename>_<columnname>_seq" pattern in the same
						 * schema, since CREATE TABLE will regenerate that
						 * exact name.  The sequence is an INTERNAL dependency
						 * of the column, so the lock we hold on the table
						 * also pins it, but the lookup helper still defends
						 * against a missing pg_class row.
						 */
						{
							HeapTuple	seqClassTup;
							Form_pg_class seqClass;
							char	   *autoname;

							seqClassTup = SearchSysCache1(RELOID,
														  ObjectIdGetDatum(seqid));
							if (!HeapTupleIsValid(seqClassTup))
								ereport(ERROR,
										(errcode(ERRCODE_UNDEFINED_OBJECT),
										 errmsg("identity sequence with OID %u does not exist",
												seqid),
										 errdetail("It may have been concurrently dropped.")));
							seqClass = (Form_pg_class) GETSTRUCT(seqClassTup);

							autoname = makeObjectName(RelationGetRelationName(rel),
													  NameStr(att->attname), "seq");
							if (seqClass->relnamespace != RelationGetNamespace(rel) ||
								strcmp(NameStr(seqClass->relname), autoname) != 0)
							{
								char	   *seqQual =
									lookup_relname_for_emit(seqid,
															schema_qualified,
															base_namespace);

								appendStringInfo(&opts, "%sSEQUENCE NAME %s",
												 first_opt ? "" : " ", seqQual);
								first_opt = false;
								pfree(seqQual);
							}
							pfree(autoname);
							ReleaseSysCache(seqClassTup);
						}

						if (seq->seqstart != def_start)
						{
							appendStringInfo(&opts, "%sSTART WITH " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqstart);
							first_opt = false;
						}
						if (seq->seqincrement != 1)
						{
							appendStringInfo(&opts, "%sINCREMENT BY " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqincrement);
							first_opt = false;
						}
						if (seq->seqmin != def_min)
						{
							appendStringInfo(&opts, "%sMINVALUE " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqmin);
							first_opt = false;
						}
						if (seq->seqmax != def_max)
						{
							appendStringInfo(&opts, "%sMAXVALUE " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqmax);
							first_opt = false;
						}
						if (seq->seqcache != 1)
						{
							appendStringInfo(&opts, "%sCACHE " INT64_FORMAT,
											 first_opt ? "" : " ", seq->seqcache);
							first_opt = false;
						}
						if (seq->seqcycle)
						{
							appendStringInfo(&opts, "%sCYCLE", first_opt ? "" : " ");
							first_opt = false;
						}

						if (!first_opt)
							appendStringInfo(buf, " (%s)", opts.data);

						pfree(opts.data);
						ReleaseSysCache(seqTup);
					}
				}
			}
			else if (defexpr)
				appendStringInfo(buf, " DEFAULT %s", defexpr);

			if (defexpr)
				pfree(defexpr);
		}

		if (att->attnotnull)
		{
			LocalNotNullEntry *nn = &nn_entries[att->attnum];

			/*
			 * Suppress NOT NULL when the constraint is inherited-only
			 * (conislocal=false).  The INHERITS clause already propagates it;
			 * emitting it here would make conislocal true on replay.  When
			 * there is no pg_constraint row at all (pre-PG18 catalog with
			 * attnotnull=true), inherited_notnull is false so we fall through
			 * and emit plain NOT NULL as before.
			 *
			 * Also suppress NOT VALID constraints here: they must be emitted
			 * via ALTER TABLE ... ADD CONSTRAINT ... NOT VALID to preserve
			 * the convalidated=false flag.
			 */
			if (!nn->inherited_notnull && !nn->not_valid)
			{
				if (nn->name != NULL && !nn->is_auto)
					appendStringInfo(buf, " CONSTRAINT %s NOT NULL",
									 quote_identifier(nn->name));
				else
					appendStringInfoString(buf, " NOT NULL");
				if (nn->name != NULL && nn->no_inherit)
					appendStringInfoString(buf, " NO INHERIT");
			}
		}
	}

	/*
	 * Table-level CHECK constraints - emitted inline in the CREATE TABLE body
	 * so they appear alongside the columns (the pg_dump shape).  The
	 * constraint loop later in pg_get_table_ddl_internal skips CHECK
	 * constraints to avoid double-emission.
	 */
	if (include_check)
		append_inline_check_constraints(buf, rel, pretty, &first);
}

/*
 * append_typed_column_overrides
 *		For a typed table (CREATE TABLE ... OF type_name), append the
 *		optional "(col WITH OPTIONS ..., ...)" list carrying locally
 *		applied per-column overrides - DEFAULT, NOT NULL, and any locally
 *		declared CHECK constraints.
 *
 * Columns whose type is fully dictated by reloftype emit nothing.  The
 * parenthesised list is suppressed entirely when no column needs an
 * override and there are no locally-declared CHECK constraints, matching
 * the canonical "CREATE TABLE x OF t;" shape.
 */
static void
append_typed_column_overrides(StringInfo buf, Relation rel, bool pretty,
							  bool include_check,
							  LocalNotNullEntry *nn_entries)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	List	   *dpcontext = NIL;
	StringInfoData inner;
	bool		first = true;

	initStringInfo(&inner);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char	   *defexpr = NULL;
		bool		has_default;
		bool		has_notnull;

		if (att->attisdropped)
			continue;

		if (att->atthasdef)
			defexpr = find_attrdef_text(rel, att->attnum, &dpcontext);

		has_default = (defexpr != NULL);
		/* inherited-only and NOT VALID NOT NULLs are not inline overrides */
		has_notnull = att->attnotnull &&
			!nn_entries[att->attnum].inherited_notnull &&
			!nn_entries[att->attnum].not_valid;

		if (!has_default && !has_notnull)
		{
			if (defexpr)
				pfree(defexpr);
			continue;
		}

		if (!first)
			appendStringInfoChar(&inner, ',');
		if (pretty)
			appendStringInfoString(&inner, "\n    ");
		else if (!first)
			appendStringInfoChar(&inner, ' ');
		first = false;

		appendStringInfo(&inner, "%s WITH OPTIONS",
						 quote_identifier(NameStr(att->attname)));
		if (has_default)
			appendStringInfo(&inner, " DEFAULT %s", defexpr);
		if (has_notnull)
		{
			LocalNotNullEntry *nn = &nn_entries[att->attnum];

			if (nn->name != NULL && !nn->is_auto)
				appendStringInfo(&inner, " CONSTRAINT %s NOT NULL",
								 quote_identifier(nn->name));
			else
				appendStringInfoString(&inner, " NOT NULL");
			if (nn->name != NULL && nn->no_inherit)
				appendStringInfoString(&inner, " NO INHERIT");
		}

		if (defexpr)
			pfree(defexpr);
	}

	/*
	 * Locally-declared CHECK constraints on a typed table belong in the
	 * column-list parentheses, same as for an untyped table.  The out-of-line
	 * constraint loop later still skips CHECKs.
	 */
	if (include_check)
		append_inline_check_constraints(&inner, rel, pretty, &first);

	if (!first)
	{
		appendStringInfoString(buf, " (");
		appendStringInfoString(buf, inner.data);
		if (pretty)
			appendStringInfoString(buf, "\n)");
		else
			appendStringInfoChar(buf, ')');
	}
	pfree(inner.data);
}

/*
 * append_stmt
 *		Push ctx->buf onto ctx->statements.
 *
 * Used for all DDL emissions.  When schema_qualified is false, the
 * active search_path has already been narrowed to the base schema, so
 * ruleutils helpers (pg_get_indexdef_ddl, pg_get_ruledef_ddl,
 * pg_get_constraintdef_body, pg_get_statisticsobjdef_ddl) produce
 * unqualified names for same-schema objects automatically.
 */
static void
append_stmt(TableDdlContext *ctx)
{
	ctx->statements = lappend(ctx->statements, pstrdup(ctx->buf.data));
}

/*
 * emit_create_table_stmt
 *		Build the leading CREATE TABLE statement, including persistence
 *		(TEMPORARY / UNLOGGED), body (column list / OF type_name /
 *		PARTITION OF parent), INHERITS, PARTITION BY, USING method,
 *		WITH (reloptions), TABLESPACE, and ON COMMIT.
 */
static void
emit_create_table_stmt(TableDdlContext *ctx)
{
	Relation	rel = ctx->rel;
	char		relpersistence = rel->rd_rel->relpersistence;
	char		relkind = rel->rd_rel->relkind;
	bool		is_typed = OidIsValid(rel->rd_rel->reloftype);
	HeapTuple	classtup;
	Datum		reloptDatum;
	bool		reloptIsnull;
	char	   *toastOptsStr = NULL;	/* toast.key=value pairs, or NULL */

	classtup = SearchSysCache1(RELOID, ObjectIdGetDatum(ctx->relid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for relation %u", ctx->relid);

	reloptDatum = SysCacheGetAttr(RELOID, classtup,
								  Anum_pg_class_reloptions, &reloptIsnull);

	/*
	 * Read TOAST table reloptions; they live in the TOAST relation's own
	 * pg_class row and must be emitted as "toast.key=value" entries in the
	 * WITH clause.  Read them now while we have the main classtup.
	 */
	{
		bool		toastOidIsnull;
		Datum		toastOidDatum = SysCacheGetAttr(RELOID, classtup,
													Anum_pg_class_reltoastrelid,
													&toastOidIsnull);
		Oid			toastRelOid = toastOidIsnull ? InvalidOid
			: DatumGetObjectId(toastOidDatum);

		if (OidIsValid(toastRelOid))
		{
			HeapTuple	toastTup = SearchSysCache1(RELOID,
												   ObjectIdGetDatum(toastRelOid));

			if (HeapTupleIsValid(toastTup))
			{
				bool		toastOptIsnull;
				Datum		toastOptDatum = SysCacheGetAttr(RELOID, toastTup,
															Anum_pg_class_reloptions,
															&toastOptIsnull);

				if (!toastOptIsnull)
				{
					Datum	   *toastopts;
					int			ntoastopts;
					StringInfoData tbuf;

					initStringInfo(&tbuf);
					deconstruct_array_builtin(DatumGetArrayTypeP(toastOptDatum),
											  TEXTOID, &toastopts, NULL,
											  &ntoastopts);
					for (int ti = 0; ti < ntoastopts; ti++)
					{
						char	   *opt = TextDatumGetCString(toastopts[ti]);
						char	   *sep = strchr(opt, '=');
						char	   *key,
								   *val;

						if (sep)
						{
							*sep = '\0';
							key = opt;
							val = sep + 1;
						}
						else
						{
							key = opt;
							val = "";
						}
						if (ti > 0)
							appendStringInfoString(&tbuf, ", ");
						appendStringInfo(&tbuf, "toast.%s=",
										 quote_identifier(key));
						{
							const char *qid = quote_identifier(val);

							if (qid == val)
								appendStringInfoString(&tbuf, val);
							else
							{
								char	   *qval = quote_literal_cstr(val);

								appendStringInfoString(&tbuf, qval);
								pfree(qval);
								pfree((char *) qid);
							}
						}
						pfree(opt);
					}
					pfree(toastopts);
					toastOptsStr = tbuf.data; /* palloc'd; free after WITH */
				}
				ReleaseSysCache(toastTup);
			}
		}
	}

	resetStringInfo(&ctx->buf);
	appendStringInfoString(&ctx->buf, "CREATE ");
	if (relpersistence == RELPERSISTENCE_TEMP)
		appendStringInfoString(&ctx->buf, "TEMPORARY ");
	else if (relpersistence == RELPERSISTENCE_UNLOGGED)
		appendStringInfoString(&ctx->buf, "UNLOGGED ");
	appendStringInfo(&ctx->buf, "TABLE %s", ctx->qualname);

	if (rel->rd_rel->relispartition)
	{
		Oid			parentOid = get_partition_parent(ctx->relid, true);
		char	   *parentQual = lookup_relname_for_emit(parentOid,
														 ctx->schema_qualified,
														 ctx->base_namespace);
		char	   *parentRelname = get_rel_name(parentOid);
		Datum		boundDatum;
		bool		boundIsnull;
		char	   *forValues = NULL;
		char	   *boundStr = NULL;

		if (parentRelname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("partition parent with OID %u does not exist",
							parentOid),
					 errdetail("It may have been concurrently dropped.")));

		boundDatum = SysCacheGetAttr(RELOID, classtup,
									 Anum_pg_class_relpartbound, &boundIsnull);
		if (!boundIsnull)
		{
			Node	   *boundNode;
			List	   *dpcontext;

			boundStr = TextDatumGetCString(boundDatum);
			boundNode = stringToNode(boundStr);
			dpcontext = deparse_context_for(parentRelname, parentOid);
			forValues = deparse_expression(boundNode, dpcontext, false, false);
		}

		appendStringInfo(&ctx->buf, " PARTITION OF %s", parentQual);

		/*
		 * Include NOT NULL constraints in the PARTITION OF column spec
		 * when the child's constraint name differs from what automatic
		 * inheritance would give.  When PARTITION OF runs, the parent's
		 * NOT NULL propagates to the child under the parent's constraint
		 * name.  If the child carries a different name - whether from a
		 * user-specified CONSTRAINT clause or from a pre-existing table
		 * that was later attached as a partition - we must specify it in
		 * the column spec so the name is preserved on replay.
		 *
		 * We scan pg_constraint directly rather than relying on nn_entries,
		 * because after ATTACH PARTITION the child's constraint can have
		 * conislocal=false (purely inherited) while still carrying the
		 * original local name - a case that collect_local_not_null tracks
		 * only as inherited_notnull without preserving the name.
		 */
		{
			Relation	conRel;
			SysScanDesc conScan;
			ScanKeyData conKey;
			HeapTuple	conTup;
			StringInfoData colspec;
			bool		hasspecs = false;

			initStringInfo(&colspec);

			conRel = table_open(ConstraintRelationId, AccessShareLock);
			ScanKeyInit(&conKey,
						Anum_pg_constraint_conrelid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(ctx->relid));
			conScan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId,
										 true, NULL, 1, &conKey);

			while (HeapTupleIsValid(conTup = systable_getnext(conScan)))
			{
				Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);
				Datum		conkeyDat;
				bool		conkeyNull;
				ArrayType  *conkeyArr;
				AttrNumber	attnum;
				char	   *conname;
				char	   *parent_conname;
				char	   *attname;

				if (con->contype != CONSTRAINT_NOTNULL)
					continue;

				conkeyDat = heap_getattr(conTup, Anum_pg_constraint_conkey,
										 RelationGetDescr(conRel), &conkeyNull);
				if (conkeyNull)
					continue;
				conkeyArr = DatumGetArrayTypeP(conkeyDat);
				if (ARR_NDIM(conkeyArr) != 1 || ARR_DIMS(conkeyArr)[0] < 1 ||
					ARR_HASNULL(conkeyArr) || ARR_ELEMTYPE(conkeyArr) != INT2OID)
					continue;
				attnum = ((int16 *) ARR_DATA_PTR(conkeyArr))[0];

				attname = get_attname(ctx->relid, attnum, true);
				if (attname == NULL)
					continue;

				conname = pstrdup(NameStr(con->conname));

				/*
				 * Check if this name is what PARTITION OF would auto-create.
				 * The parent propagates its own NOT NULL name to the child.
				 * Look up the parent's NOT NULL constraint name for this column.
				 */
				parent_conname = NULL;
				{
					AttrNumber	parentAttnum = get_attnum(parentOid, attname);

					if (parentAttnum != InvalidAttrNumber)
					{
						Relation	pconRel =
							table_open(ConstraintRelationId, AccessShareLock);
						ScanKeyData pconKey;
						SysScanDesc pconScan;
						HeapTuple	pconTup;

						ScanKeyInit(&pconKey,
									Anum_pg_constraint_conrelid,
									BTEqualStrategyNumber, F_OIDEQ,
									ObjectIdGetDatum(parentOid));
						pconScan = systable_beginscan(pconRel,
													  ConstraintRelidTypidNameIndexId,
													  true, NULL, 1, &pconKey);
						while (HeapTupleIsValid(pconTup = systable_getnext(pconScan)))
						{
							Form_pg_constraint pcon =
								(Form_pg_constraint) GETSTRUCT(pconTup);
							Datum		pconkeyDat;
							bool		pconkeyNull;
							ArrayType  *pconkeyArr;

							if (pcon->contype != CONSTRAINT_NOTNULL)
								continue;
							pconkeyDat = heap_getattr(pconTup,
													  Anum_pg_constraint_conkey,
													  RelationGetDescr(pconRel),
													  &pconkeyNull);
							if (pconkeyNull)
								continue;
							pconkeyArr = DatumGetArrayTypeP(pconkeyDat);
							if (ARR_NDIM(pconkeyArr) == 1 &&
								ARR_DIMS(pconkeyArr)[0] >= 1 &&
								!ARR_HASNULL(pconkeyArr) &&
								ARR_ELEMTYPE(pconkeyArr) == INT2OID &&
								((int16 *) ARR_DATA_PTR(pconkeyArr))[0] ==
								parentAttnum)
							{
								parent_conname = pstrdup(NameStr(pcon->conname));
								break;
							}
						}
						systable_endscan(pconScan);
						table_close(pconRel, AccessShareLock);
					}
				}

				/*
				 * The child's NOT NULL needs inline specification when its name
				 * differs from the parent's constraint name (which is what
				 * PARTITION OF would automatically propagate).  If the parent has
				 * no NOT NULL on this column, the child's constraint was purely
				 * local and also needs to be specified inline.
				 */
				if (parent_conname == NULL ||
					strcmp(conname, parent_conname) != 0)
				{
					bool		no_inherit = con->connoinherit;

					if (hasspecs)
						appendStringInfoChar(&colspec, ',');
					if (ctx->pretty)
						appendStringInfoString(&colspec, "\n    ");
					else if (hasspecs)
						appendStringInfoChar(&colspec, ' ');
					hasspecs = true;

					appendStringInfo(&colspec, "CONSTRAINT %s NOT NULL %s",
									 quote_identifier(conname),
									 quote_identifier(attname));
					if (no_inherit)
						appendStringInfoString(&colspec, " NO INHERIT");

					/*
					 * Also add to skip_notnull_oids so emit_local_constraints
					 * does not try to emit it again as ALTER TABLE ADD CONSTRAINT.
					 */
					ctx->skip_notnull_oids =
						lappend_oid(ctx->skip_notnull_oids, con->oid);
				}

				pfree(conname);
				pfree(attname);
				if (parent_conname)
					pfree(parent_conname);
			}

			systable_endscan(conScan);
			table_close(conRel, AccessShareLock);

			if (hasspecs)
			{
				if (ctx->pretty)
					appendStringInfo(&ctx->buf, " (\n%s\n)", colspec.data);
				else
					appendStringInfo(&ctx->buf, " (%s)", colspec.data);
			}
			pfree(colspec.data);
		}

		appendStringInfo(&ctx->buf, " %s", forValues ? forValues : "DEFAULT");
		if (forValues)
			pfree(forValues);
		if (boundStr)
			pfree(boundStr);
		pfree(parentQual);
		pfree(parentRelname);
	}
	else if (is_typed)
	{
		char	   *typname = format_type_be_qualified(rel->rd_rel->reloftype);

		appendStringInfo(&ctx->buf, " OF %s", typname);
		pfree(typname);

		append_typed_column_overrides(&ctx->buf, rel, ctx->pretty,
									  is_kind_included(ctx, TABLE_DDL_KIND_CHECK),
									  ctx->nn_entries);
	}
	else
	{
		List	   *parents;
		ListCell   *lc;
		bool		first;
		int			buf_len_before;

		appendStringInfoString(&ctx->buf, " (");

		buf_len_before = ctx->buf.len;
		append_column_defs(&ctx->buf, rel, ctx->pretty,
						   is_kind_included(ctx, TABLE_DDL_KIND_CHECK),
						   ctx->schema_qualified, ctx->nn_entries);

		/*
		 * For INHERITS children, inherited columns that carry a local NOT
		 * NULL constraint need a table-level NOT NULL clause here so that
		 * the constraint is established before INHERITS propagates the
		 * parent's version.  Without it, an out-of-line ALTER TABLE ADD
		 * CONSTRAINT would find a same-column NOT NULL already propagated
		 * by INHERITS and fail.
		 *
		 * User-defined names:  emit CONSTRAINT name NOT NULL col.  The name
		 * survives and the constraint merges correctly with the inherited one.
		 *
		 * Auto-names: emit NOT NULL col (no CONSTRAINT clause).  PostgreSQL
		 * will assign the child-table auto-name (e.g. ntc_a_not_null),
		 * matching the original.  An out-of-line ALTER TABLE would collide
		 * with the constraint that INHERITS has already propagated under the
		 * parent's name.
		 */
		{
			TupleDesc	tupdesc = RelationGetDescr(rel);
			bool		any_content = (ctx->buf.len > buf_len_before);

			for (int i = 0; i < tupdesc->natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(tupdesc, i);
				LocalNotNullEntry *nn;

				if (att->attisdropped || att->attislocal)
					continue;

				nn = &ctx->nn_entries[att->attnum];
				if (nn->conoid == InvalidOid)
					continue;

				if (any_content)
					appendStringInfoChar(&ctx->buf, ',');
				if (ctx->pretty)
					appendStringInfoString(&ctx->buf, "\n    ");
				else if (any_content)
					appendStringInfoChar(&ctx->buf, ' ');
				any_content = true;

				if (nn->is_auto)
				{
					appendStringInfo(&ctx->buf, "NOT NULL %s",
									 quote_identifier(NameStr(att->attname)));
					/* Mark as emitted so emit_local_constraints skips it. */
					ctx->skip_notnull_oids =
						lappend_oid(ctx->skip_notnull_oids, nn->conoid);
				}
				else
				{
					appendStringInfo(&ctx->buf, "CONSTRAINT %s NOT NULL %s",
									 quote_identifier(nn->name),
									 quote_identifier(NameStr(att->attname)));
				}
				if (nn->no_inherit)
					appendStringInfoString(&ctx->buf, " NO INHERIT");
			}
		}

		if (ctx->pretty)
			appendStringInfoString(&ctx->buf, "\n)");
		else
			appendStringInfoChar(&ctx->buf, ')');

		parents = find_inheritance_parents(ctx->relid, NoLock);
		if (parents != NIL)
		{
			appendStringInfoString(&ctx->buf, " INHERITS (");
			first = true;
			foreach(lc, parents)
			{
				Oid			poid = lfirst_oid(lc);
				char	   *pname = lookup_relname_for_emit(poid,
															ctx->schema_qualified,
															ctx->base_namespace);

				if (!first)
					appendStringInfoString(&ctx->buf, ", ");
				first = false;
				appendStringInfoString(&ctx->buf, pname);
				pfree(pname);
			}
			appendStringInfoChar(&ctx->buf, ')');
			list_free(parents);
		}
	}

	if (relkind == RELKIND_PARTITIONED_TABLE)
	{
		Datum		partkeyDatum;
		char	   *partkey;

		partkeyDatum = OidFunctionCall1(F_PG_GET_PARTKEYDEF,
										ObjectIdGetDatum(ctx->relid));
		partkey = TextDatumGetCString(partkeyDatum);
		appendStringInfo(&ctx->buf, " PARTITION BY %s", partkey);
		pfree(partkey);
	}

	if (OidIsValid(rel->rd_rel->relam) &&
		(relkind == RELKIND_PARTITIONED_TABLE ||
		 rel->rd_rel->relam != HEAP_TABLE_AM_OID))
	{
		char	   *amname = get_am_name(rel->rd_rel->relam);

		if (amname != NULL)
		{
			appendStringInfo(&ctx->buf, " USING %s",
							 quote_identifier(amname));
			pfree(amname);
		}
	}

	if (!reloptIsnull || toastOptsStr != NULL)
	{
		appendStringInfoString(&ctx->buf, " WITH (");
		if (!reloptIsnull)
			get_reloptions(&ctx->buf, reloptDatum);
		if (toastOptsStr != NULL)
		{
			if (!reloptIsnull)
				appendStringInfoString(&ctx->buf, ", ");
			appendStringInfoString(&ctx->buf, toastOptsStr);
			pfree(toastOptsStr);
		}
		appendStringInfoChar(&ctx->buf, ')');
	}

	ReleaseSysCache(classtup);

	if (relpersistence == RELPERSISTENCE_TEMP)
	{
		OnCommitAction oc = get_on_commit_action(ctx->relid);

		if (oc == ONCOMMIT_DELETE_ROWS)
			appendStringInfoString(&ctx->buf, " ON COMMIT DELETE ROWS");
		else if (oc == ONCOMMIT_DROP)
			appendStringInfoString(&ctx->buf, " ON COMMIT DROP");
	}

	if (!ctx->no_tablespace && OidIsValid(rel->rd_rel->reltablespace))
	{
		char	   *tsname = get_tablespace_name(rel->rd_rel->reltablespace);

		if (tsname != NULL)
		{
			appendStringInfo(&ctx->buf, " TABLESPACE %s",
							 quote_identifier(tsname));
			pfree(tsname);
		}
	}

	appendStringInfoChar(&ctx->buf, ';');
	append_stmt(ctx);
}

/*
 * emit_owner_stmt
 *		ALTER TABLE qualname OWNER TO role.
 */
static void
emit_owner_stmt(TableDdlContext *ctx)
{
	char	   *owner;

	if (ctx->no_owner)
		return;

	owner = GetUserNameFromId(ctx->rel->rd_rel->relowner, false);
	resetStringInfo(&ctx->buf);
	appendStringInfo(&ctx->buf, "ALTER TABLE %s OWNER TO %s;",
					 ctx->qualname, quote_identifier(owner));
	append_stmt(ctx);
	pfree(owner);
}

/*
 * child_default_inherited_from_parent
 *		Return true if child_adbin is identical to the adbin stored for
 *		attname on any immediate parent of childOid.
 *
 *		PostgreSQL copies the adbin expression verbatim from parent to child
 *		when a partition is created, so a byte-for-byte match means the child
 *		did not override the default locally.  For regular inheritance the
 *		parent typically has no default at all (atthasdef=false), so the
 *		function returns false and the override is correctly emitted.
 */
static bool
child_default_inherited_from_parent(Oid childOid, const char *attname,
									const char *child_adbin)
{
	List	   *parents;
	ListCell   *lc;
	bool		result = false;

	parents = find_inheritance_parents(childOid, NoLock);

	foreach(lc, parents)
	{
		Oid			parentOid = lfirst_oid(lc);
		Relation	parentRel;
		AttrNumber	parentAttnum;
		TupleConstr *constr;

		parentRel = try_table_open(parentOid, AccessShareLock);
		if (parentRel == NULL)
			continue;

		parentAttnum = get_attnum(parentOid, attname);
		if (parentAttnum != InvalidAttrNumber)
		{
			Form_pg_attribute parentAtt =
				TupleDescAttr(RelationGetDescr(parentRel), parentAttnum - 1);

			if (parentAtt->atthasdef)
			{
				constr = RelationGetDescr(parentRel)->constr;
				if (constr != NULL)
				{
					for (int j = 0; j < constr->num_defval; j++)
					{
						if (constr->defval[j].adnum == parentAttnum &&
							strcmp(constr->defval[j].adbin, child_adbin) == 0)
						{
							result = true;
							break;
						}
					}
				}
			}
		}

		table_close(parentRel, AccessShareLock);
		if (result)
			break;
	}

	list_free(parents);
	return result;
}

/*
 * parent_has_default_for_col
 *		Return true if any immediate parent of childOid has a non-generated
 *		DEFAULT expression on the column named attname.
 *
 *		Used to detect inherited columns where the child dropped the parent's
 *		default: after PARTITION OF / INHERITS the parent default is re-applied,
 *		so we must emit ALTER TABLE ONLY child ALTER COLUMN col DROP DEFAULT.
 */
static bool
parent_has_default_for_col(Oid childOid, const char *attname)
{
	List	   *parents;
	ListCell   *lc;
	bool		result = false;

	parents = find_inheritance_parents(childOid, NoLock);
	foreach(lc, parents)
	{
		Oid			parentOid = lfirst_oid(lc);
		AttrNumber	parentAttnum = get_attnum(parentOid, attname);

		if (parentAttnum != InvalidAttrNumber)
		{
			HeapTuple	attTup = SearchSysCache2(ATTNUM,
												 ObjectIdGetDatum(parentOid),
												 Int16GetDatum(parentAttnum));

			if (HeapTupleIsValid(attTup))
			{
				Form_pg_attribute parentAtt = (Form_pg_attribute) GETSTRUCT(attTup);

				if (parentAtt->atthasdef && parentAtt->attgenerated == '\0')
					result = true;
				ReleaseSysCache(attTup);
			}
		}
		if (result)
			break;
	}
	list_free(parents);
	return result;
}

/*
 * emit_child_default_overrides
 *		ALTER TABLE ONLY qualname ALTER COLUMN col SET DEFAULT expr - one per
 *		inherited (attislocal=false) non-generated column that carries a
 *		default which differs from every immediate parent's default.  Using
 *		ONLY prevents the statement from cascading into grandchildren and
 *		overwriting their own defaults.  Defaults that are simply inherited
 *		unchanged from a parent are skipped to avoid redundant output.
 *		Generated columns are skipped: their expression is inherited
 *		automatically and SET DEFAULT would fail at replay.
 *
 *		Also emits ALTER TABLE ONLY ... DROP DEFAULT for inherited columns
 *		where the child explicitly dropped the parent's default: after
 *		PARTITION OF / INHERITS the parent default is re-applied, and the
 *		DROP DEFAULT is needed to restore the child's state.
 */
static void
emit_child_default_overrides(TableDdlContext *ctx)
{
	TupleDesc	tupdesc = RelationGetDescr(ctx->rel);
	TupleConstr *constr = tupdesc->constr;
	List	   *dpcontext = NIL;

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		const char *adbin = NULL;
		char	   *defstr;

		if (att->attisdropped || att->attislocal)
			continue;
		if (att->attgenerated != '\0')
			continue;

		if (!att->atthasdef)
		{
			/*
			 * Column has no default but a parent does: after PARTITION OF /
			 * INHERITS the parent's default is re-applied, so emit DROP
			 * DEFAULT to restore the dropped state.
			 */
			if (parent_has_default_for_col(ctx->relid, NameStr(att->attname)))
			{
				resetStringInfo(&ctx->buf);
				appendStringInfo(&ctx->buf,
								 "ALTER TABLE ONLY %s ALTER COLUMN %s DROP DEFAULT;",
								 ctx->qualname,
								 quote_identifier(NameStr(att->attname)));
				append_stmt(ctx);
			}
			continue;
		}

		/* Locate the raw adbin for parent-comparison and for deparse. */
		if (constr != NULL)
		{
			for (int j = 0; j < constr->num_defval; j++)
			{
				if (constr->defval[j].adnum == att->attnum)
				{
					adbin = constr->defval[j].adbin;
					break;
				}
			}
		}
		if (adbin == NULL)
			continue;

		/*
		 * Skip columns whose default expression is identical to a parent's;
		 * the child merely inherited it (typical for partition children).
		 * An explicit local override will have a different adbin.
		 */
		if (child_default_inherited_from_parent(ctx->relid,
												NameStr(att->attname),
												adbin))
			continue;

		defstr = find_attrdef_text(ctx->rel, att->attnum, &dpcontext);
		if (defstr == NULL)
			continue;

		resetStringInfo(&ctx->buf);
		appendStringInfo(&ctx->buf,
						 "ALTER TABLE ONLY %s ALTER COLUMN %s SET DEFAULT %s;",
						 ctx->qualname,
						 quote_identifier(NameStr(att->attname)),
						 defstr);
		append_stmt(ctx);
		pfree(defstr);
	}
}

/*
 * emit_attoptions
 *		ALTER TABLE qualname ALTER COLUMN col SET (...) - one per column
 *		with non-null pg_attribute.attoptions.  The inline form of these
 *		options isn't available in CREATE TABLE, so they come out here.
 */
static void
emit_attoptions(TableDdlContext *ctx)
{
	TupleDesc	tupdesc = RelationGetDescr(ctx->rel);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		HeapTuple	attTup;
		Datum		optDatum;
		bool		optIsnull;

		if (att->attisdropped)
			continue;

		attTup = SearchSysCache2(ATTNUM,
								 ObjectIdGetDatum(ctx->relid),
								 Int16GetDatum(att->attnum));
		if (!HeapTupleIsValid(attTup))
			continue;

		optDatum = SysCacheGetAttr(ATTNUM, attTup,
								   Anum_pg_attribute_attoptions, &optIsnull);
		if (!optIsnull)
		{
			resetStringInfo(&ctx->buf);
			appendStringInfo(&ctx->buf,
							 "ALTER TABLE %s ALTER COLUMN %s SET (",
							 ctx->qualname,
							 quote_identifier(NameStr(att->attname)));
			get_reloptions(&ctx->buf, optDatum);
			appendStringInfoString(&ctx->buf, ");");
			append_stmt(ctx);
		}

		{
			bool		statisnull;
			Datum		statDatum = SysCacheGetAttr(ATTNUM, attTup,
													Anum_pg_attribute_attstattarget,
													&statisnull);

			if (!statisnull && DatumGetInt16(statDatum) >= 0)
			{
				resetStringInfo(&ctx->buf);
				appendStringInfo(&ctx->buf,
								 "ALTER TABLE %s ALTER COLUMN %s SET STATISTICS %d;",
								 ctx->qualname,
								 quote_identifier(NameStr(att->attname)),
								 (int) DatumGetInt16(statDatum));
				append_stmt(ctx);
			}
		}

		ReleaseSysCache(attTup);
	}
}

/*
 * emit_typed_column_storage
 *		For typed tables (CREATE TABLE ... OF type_name) and partition
 *		children, emit per-column STORAGE and COMPRESSION overrides as
 *		ALTER TABLE statements.
 *
 *		For typed tables, the grammar only allows DEFAULT / NOT NULL / CHECK
 *		in the column-options list, so STORAGE and COMPRESSION cannot appear
 *		inline and must be emitted post-CREATE.
 *
 *		For partition children, we compare each column's storage/compression
 *		against the parent partition's values and emit only when the child
 *		explicitly overrides.
 */
static void
emit_typed_column_storage(TableDdlContext *ctx)
{
	TupleDesc	tupdesc;
	bool		is_typed = OidIsValid(ctx->rel->rd_rel->reloftype);
	bool		is_partition = ctx->rel->rd_rel->relispartition;
	Oid			partParentOid = InvalidOid;

	if (!is_typed && !is_partition)
		return;

	tupdesc = RelationGetDescr(ctx->rel);

	/*
	 * For partition children, look up the parent OID once outside the loop;
	 * each inherited column will need it to find the parent's baseline.
	 */
	if (is_partition)
		partParentOid = get_partition_parent(ctx->relid, false);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		char		baseline_storage;
		char		baseline_compression;

		if (att->attisdropped)
			continue;

		if (is_partition)
		{
			/*
			 * For partition children, compare against the parent partition's
			 * column values; only emit when the child explicitly overrides.
			 */
			if (att->attislocal)
			{
				/* locally added column: baseline is the type default */
				baseline_storage = get_typstorage(att->atttypid);
				baseline_compression = InvalidCompressionMethod;
			}
			else
			{
				/* inherited column: get parent's values */
				HeapTuple	parentAttTup =
					SearchSysCache2(ATTNAME,
									ObjectIdGetDatum(partParentOid),
									CStringGetDatum(NameStr(att->attname)));

				if (!HeapTupleIsValid(parentAttTup))
				{
					baseline_storage = get_typstorage(att->atttypid);
					baseline_compression = InvalidCompressionMethod;
				}
				else
				{
					Form_pg_attribute parentAtt =
						(Form_pg_attribute) GETSTRUCT(parentAttTup);

					baseline_storage = parentAtt->attstorage;
					baseline_compression = parentAtt->attcompression;
					ReleaseSysCache(parentAttTup);
				}
			}
		}
		else
		{
			/* typed table: baseline is always the type default */
			baseline_storage = get_typstorage(att->atttypid);
			baseline_compression = InvalidCompressionMethod;
		}

		if (att->attstorage != baseline_storage)
		{
			resetStringInfo(&ctx->buf);
			appendStringInfo(&ctx->buf,
							 "ALTER TABLE ONLY %s ALTER COLUMN %s SET STORAGE %s;",
							 ctx->qualname,
							 quote_identifier(NameStr(att->attname)),
							 storage_name(att->attstorage));
			append_stmt(ctx);
		}

		if (att->attcompression != baseline_compression &&
			CompressionMethodIsValid(att->attcompression))
		{
			resetStringInfo(&ctx->buf);
			appendStringInfo(&ctx->buf,
							 "ALTER TABLE ONLY %s ALTER COLUMN %s SET COMPRESSION %s;",
							 ctx->qualname,
							 quote_identifier(NameStr(att->attname)),
							 GetCompressionMethodName(att->attcompression));
			append_stmt(ctx);
		}
	}
}

/*
 * emit_identity_sequence_alterations
 *		For identity sequence columns whose sequence has properties that
 *		cannot be expressed inline in CREATE TABLE, emit the corresponding
 *		ALTER SEQUENCE statement(s) after the CREATE TABLE.
 *
 *		Currently handles:
 *		  - Non-default persistence (e.g. UNLOGGED): the inline syntax has
 *		    no way to specify sequence persistence.
 *		  - Sequence type differing from the column type (e.g. AS smallint
 *		    on an integer column): generateSerialExtraStmts() always
 *		    prepends AS <col-type> to the inline options, so a second AS
 *		    clause would be rejected as a conflicting option.
 */
static void
emit_identity_sequence_alterations(TableDdlContext *ctx)
{
	TupleDesc	tupdesc = RelationGetDescr(ctx->rel);

	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			seqid;
		HeapTuple	seqClassTup;
		Form_pg_class seqClass;

		if (att->attisdropped)
			continue;
		if (att->attidentity == '\0')
			continue;

		seqid = getIdentitySequence(ctx->rel, att->attnum, true);
		if (!OidIsValid(seqid))
			continue;

		seqClassTup = SearchSysCache1(RELOID, ObjectIdGetDatum(seqid));
		if (!HeapTupleIsValid(seqClassTup))
			continue;
		seqClass = (Form_pg_class) GETSTRUCT(seqClassTup);

		/*
		 * Fetch the pg_sequence row once and handle both post-CREATE
		 * alterations (persistence, AS type) before releasing either cache
		 * entry.
		 */
		{
			HeapTuple	seqTup = SearchSysCache1(SEQRELID,
												 ObjectIdGetDatum(seqid));

			if (HeapTupleIsValid(seqTup))
			{
				Form_pg_sequence seq = (Form_pg_sequence) GETSTRUCT(seqTup);

				if (seqClass->relpersistence == RELPERSISTENCE_UNLOGGED)
				{
					char	   *seqname = lookup_relname_for_emit(seqid,
																  ctx->schema_qualified,
																  ctx->base_namespace);

					resetStringInfo(&ctx->buf);
					appendStringInfo(&ctx->buf, "ALTER SEQUENCE %s SET UNLOGGED;",
									 seqname);
					append_stmt(ctx);
					pfree(seqname);
				}

				/*
				 * AS type: emit when the sequence type differs from the
				 * column type.  CREATE TABLE ... GENERATED AS IDENTITY cannot
				 * express this inline because generateSerialExtraStmts()
				 * already prepends AS <col-type>, so a conflicting AS clause
				 * would be rejected.  A post-CREATE ALTER SEQUENCE avoids the
				 * conflict and correctly adjusts MINVALUE/MAXVALUE to the new
				 * type's defaults (matching what ALTER SEQUENCE does when the
				 * old values were at their type-default).
				 */
				if (seq->seqtypid != att->atttypid)
				{
					char	   *typname = format_type_be(seq->seqtypid);
					char	   *seqname = lookup_relname_for_emit(seqid,
																  ctx->schema_qualified,
																  ctx->base_namespace);

					resetStringInfo(&ctx->buf);
					appendStringInfo(&ctx->buf, "ALTER SEQUENCE %s AS %s;",
									 seqname, typname);
					append_stmt(ctx);
					pfree(seqname);
					pfree(typname);
				}

				ReleaseSysCache(seqTup);
			}
		}

		ReleaseSysCache(seqClassTup);
	}
}

/*
 * emit_index_column_statistics
 *		Emit ALTER INDEX name ALTER COLUMN n SET STATISTICS for each key
 *		column of idxoid that has a non-default (non-null, >= 0) statistics
 *		target.  Only expression columns of an index support SET STATISTICS,
 *		but the catalog check is the same regardless.
 *
 *		Called from emit_indexes (non-constraint indexes) and from
 *		emit_local_constraints (PK/UNIQUE/EXCLUSION backing indexes).
 */
static void
emit_index_column_statistics(TableDdlContext *ctx, Oid idxoid, int indnkeyatts)
{
	char	   *idxqualname = lookup_relname_for_emit(idxoid,
													  ctx->schema_qualified,
													  ctx->base_namespace);

	for (int j = 1; j <= indnkeyatts; j++)
	{
		HeapTuple	attTup = SearchSysCache2(ATTNUM,
											 ObjectIdGetDatum(idxoid),
											 Int16GetDatum(j));

		if (HeapTupleIsValid(attTup))
		{
			bool		isnull;
			Datum		stattargetDatum =
				SysCacheGetAttr(ATTNUM, attTup,
								Anum_pg_attribute_attstattarget,
								&isnull);

			if (!isnull && DatumGetInt16(stattargetDatum) >= 0)
			{
				resetStringInfo(&ctx->buf);
				appendStringInfo(&ctx->buf,
								 "ALTER INDEX %s ALTER COLUMN %d SET STATISTICS %d;",
								 idxqualname, j,
								 (int) DatumGetInt16(stattargetDatum));
				append_stmt(ctx);
			}
			ReleaseSysCache(attTup);
		}
	}
	pfree(idxqualname);
}

/*
 * emit_indexes
 *		CREATE INDEX per non-constraint-backed index on the relation.
 *		Indexes that back PK / UNIQUE / EXCLUDE constraints are emitted
 *		out-of-line by emit_local_constraints (the ALTER TABLE ... ADD
 *		CONSTRAINT statement creates the index implicitly).
 */
static void
emit_indexes(TableDdlContext *ctx)
{
	List	   *indexoids;
	ListCell   *lc;

	if (!is_kind_included(ctx, TABLE_DDL_KIND_INDEX))
		return;

	indexoids = RelationGetIndexList(ctx->rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		HeapTuple	indTup;
		Form_pg_index idxform;
		char	   *idxdef;
		int16		indnkeyatts;

		if (OidIsValid(get_index_constraint(idxoid)))
			continue;

		indTup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(idxoid));
		if (!HeapTupleIsValid(indTup))
			continue;
		idxform = (Form_pg_index) GETSTRUCT(indTup);

		/*
		 * Skip invalid indexes unless this is a partitioned index
		 * (relkind='I').  A partitioned index created with ON ONLY, or
		 * one whose parent was marked invalid by the invalidate_parent
		 * logic during ATTACH PARTITION, legitimately has indisvalid=false
		 * and must still be emitted.  A plain index (relkind='i') with
		 * indisvalid=false was left by a failed CREATE INDEX CONCURRENTLY
		 * and is intentionally omitted.
		 */
		if (!idxform->indisvalid &&
			get_rel_relkind(idxoid) != RELKIND_PARTITIONED_INDEX)
		{
			ReleaseSysCache(indTup);
			continue;
		}

		indnkeyatts = idxform->indnkeyatts;
		ReleaseSysCache(indTup);

		idxdef = pg_get_indexdef_ddl(idxoid, ctx->no_tablespace);
		resetStringInfo(&ctx->buf);
		appendStringInfo(&ctx->buf, "%s;", idxdef);
		append_stmt(ctx);
		pfree(idxdef);

		emit_index_column_statistics(ctx, idxoid, indnkeyatts);

		/*
		 * For partitioned indexes (relkind='I'), emit ALTER INDEX ... ATTACH
		 * PARTITION for each direct partition-child index.  This is done here
		 * (from the parent index's perspective) rather than from the child
		 * partition table's perspective because emit_partition_children is now
		 * called before emit_indexes: by the time we reach this point all
		 * child partition tables and their CREATE INDEX statements have already
		 * been emitted, so both sides of the ATTACH exist.
		 *
		 * Gate on TABLE_DDL_KIND_PARTITION: if partition children are not
		 * being emitted (e.g. only_kinds=>'index') the child tables do not
		 * exist in the replay target and ATTACH PARTITION would fail.
		 */
		if (get_rel_relkind(idxoid) == RELKIND_PARTITIONED_INDEX &&
			is_kind_included(ctx, TABLE_DDL_KIND_PARTITION))
		{
			List	   *childIdxOids = find_inheritance_children(idxoid,
																 AccessShareLock);
			ListCell   *clc;

			if (childIdxOids != NIL)
			{
				char	   *parentIdxName = lookup_relname_for_emit(idxoid,
																	ctx->schema_qualified,
																	ctx->base_namespace);

				foreach(clc, childIdxOids)
				{
					Oid			childIdxOid = lfirst_oid(clc);
					char	   *childIdxName = lookup_relname_for_emit(childIdxOid,
																	   ctx->schema_qualified,
																	   ctx->base_namespace);

					resetStringInfo(&ctx->buf);
					appendStringInfo(&ctx->buf,
									 "ALTER INDEX %s ATTACH PARTITION %s;",
									 parentIdxName,
									 childIdxName);
					append_stmt(ctx);
					pfree(childIdxName);
				}
				pfree(parentIdxName);
			}
			list_free(childIdxOids);
		}
	}
	list_free(indexoids);
}

/*
 * index_ddl_kind
 *		Return the TableDdlKind responsible for emitting idxoid.
 *		Constraint-backed indexes (PK / UNIQUE / EXCLUSION) use the kind that
 *		emits their parent constraint; a plain CREATE INDEX uses
 *		TABLE_DDL_KIND_INDEX.
 */
static TableDdlKind
index_ddl_kind(Oid idxoid)
{
	TableDdlKind kind = TABLE_DDL_KIND_INDEX;
	Oid			conoid = get_index_constraint(idxoid);

	if (OidIsValid(conoid))
	{
		HeapTuple	conTup = SearchSysCache1(CONSTROID,
											 ObjectIdGetDatum(conoid));

		if (HeapTupleIsValid(conTup))
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);

			switch (con->contype)
			{
				case CONSTRAINT_PRIMARY:
					kind = TABLE_DDL_KIND_PRIMARY_KEY;
					break;
				case CONSTRAINT_UNIQUE:
					kind = TABLE_DDL_KIND_UNIQUE;
					break;
				case CONSTRAINT_EXCLUSION:
					kind = TABLE_DDL_KIND_EXCLUSION;
					break;
				default:
					break;
			}
			ReleaseSysCache(conTup);
		}
	}

	return kind;
}

/*
 * emit_cluster_on
 *		ALTER TABLE qualname CLUSTER ON index_name - emitted when any index
 *		on the relation has indisclustered=true.
 */
static void
emit_cluster_on(TableDdlContext *ctx)
{
	List	   *indexoids;
	ListCell   *lc;

	indexoids = RelationGetIndexList(ctx->rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		HeapTuple	indTup;
		Form_pg_index idxform;

		indTup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(idxoid));
		if (!HeapTupleIsValid(indTup))
			continue;
		idxform = (Form_pg_index) GETSTRUCT(indTup);

		if (idxform->indisclustered)
		{
			/*
			 * Skip CLUSTER ON silently when the index's kind is filtered --
			 * the index won't be emitted, so the hint would dangle.
			 */
			TableDdlKind clust_kind = index_ddl_kind(idxoid);
			char	   *idxname;

			idxname = get_rel_name(idxoid);
			ReleaseSysCache(indTup);

			if (idxname != NULL)
			{
				if (is_kind_included(ctx, clust_kind))
				{
					resetStringInfo(&ctx->buf);
					appendStringInfo(&ctx->buf,
									 "ALTER TABLE %s CLUSTER ON %s;",
									 ctx->qualname,
									 quote_identifier(idxname));
					append_stmt(ctx);
				}
				pfree(idxname);
			}
			break;				/* at most one index per table can be clustered */
		}
		ReleaseSysCache(indTup);
	}
	list_free(indexoids);
}

/*
 * emit_local_constraints
 *		ALTER TABLE ... ADD CONSTRAINT for each locally-defined constraint
 *		on the relation.  Inherited constraints (conislocal=false) come
 *		from the parent's DDL.  CHECK constraints are emitted inline for
 *		regular/typed tables (skip here) but out-of-line for partition
 *		children (no column list to live in).
 *
 *		NOT NULL constraints are tracked via skip_notnull_oids to avoid
 *		double-emission.  The set covers: (a) local columns, where the
 *		column-emit helpers materialise the constraint inline; (b) inherited
 *		columns with a user-defined name, where emit_create_table_stmt
 *		emits a table-level CONSTRAINT clause so the name survives the
 *		PARTITION OF / INHERITS replay; and (c) auto-named NOT NULLs on
 *		partition children, which are re-created by PARTITION OF and would
 *		collide with an out-of-line ALTER TABLE ADD CONSTRAINT.  Any NOT
 *		NULL not in skip_notnull_oids is emitted here (e.g. inherited
 *		columns with an auto-name on a plain INHERITS child).
 *
 *		Each contype is gated on the matching kind in the only / except
 *		vocabulary, so callers can produce e.g. an FK-only pass
 *		(only => 'foreign_key') or a pub/sub clone that keeps only
 *		the primary key (except => 'unique,check,foreign_key,exclusion').
 *		NOT NULL is intentionally not in the vocabulary - always emitted
 *		so cloned schemas don't silently accept NULLs the source would
 *		have rejected.
 */
static void
emit_local_constraints(TableDdlContext *ctx)
{
	Relation	conRel;
	SysScanDesc conScan;
	ScanKeyData conKey;
	HeapTuple	conTup;
	bool		is_partition = ctx->rel->rd_rel->relispartition;
	List	   *fk_stmts = NIL;

	conRel = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&conKey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ctx->relid));
	conScan = systable_beginscan(conRel, ConstraintRelidTypidNameIndexId,
								 true, NULL, 1, &conKey);

	while (HeapTupleIsValid(conTup = systable_getnext(conScan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);

		if (!con->conislocal)
			continue;

		/*
		 * Each contype is gated on its kind in the only / except
		 * vocabulary.  CHECK is also skipped for non-partition relations
		 * because append_inline_check_constraints emits those inline in
		 * the CREATE TABLE body; partition children have no column list
		 * to live in, so they come through this loop.  NOT NULL is not a
		 * filterable kind - emitted unconditionally to avoid producing
		 * schemas that silently accept NULLs the source would have
		 * rejected.
		 */
		switch (con->contype)
		{
			case CONSTRAINT_PRIMARY:
				if (!is_kind_included(ctx, TABLE_DDL_KIND_PRIMARY_KEY))
					continue;
				break;
			case CONSTRAINT_UNIQUE:
				if (!is_kind_included(ctx, TABLE_DDL_KIND_UNIQUE))
					continue;
				break;
			case CONSTRAINT_CHECK:
				if (!is_kind_included(ctx, TABLE_DDL_KIND_CHECK))
					continue;

				/*
				 * For non-partition relations, CHECK is normally emitted
				 * inline in the CREATE TABLE body by
				 * append_inline_check_constraints, so we skip it here to
				 * avoid double emission.  But there are two exceptions:
				 *
				 * - When KIND_TABLE is not in the active filter (e.g.
				 *   only=>'check'), the inline pass never runs; fall through
				 *   and emit each CHECK via ALTER TABLE.
				 *
				 * - NOT VALID constraints are intentionally excluded from
				 *   the inline pass: CREATE TABLE validates against the empty
				 *   table and would mark them valid.  They must come through
				 *   here as ALTER TABLE ... ADD CONSTRAINT ... NOT VALID so
				 *   the convalidated flag is preserved on round-trip.
				 */
				if (!is_partition &&
					is_kind_included(ctx, TABLE_DDL_KIND_TABLE) &&
					con->convalidated)
					continue;
				break;
			case CONSTRAINT_FOREIGN:
				if (!is_kind_included(ctx, TABLE_DDL_KIND_FOREIGN_KEY))
					continue;
				/*
				 * Self-referential FK: if the index it references on the
				 * same table would not be emitted under the active filter,
				 * the FK can't be created on replay.  Error now so the
				 * emitted DDL never dangles.
				 *
				 * con->conindid points directly at the unique index that
				 * backs the referenced columns -- whether that index is
				 * plain (CREATE UNIQUE INDEX) or created implicitly by a
				 * PK/UNIQUE constraint.  index_ddl_kind() maps that OID
				 * to the kind responsible for emitting it, covering both
				 * shapes without a separate pg_constraint scan.
				 */
				if (con->confrelid == ctx->relid &&
					OidIsValid(con->conindid))
				{
					TableDdlKind ref_kind = index_ddl_kind(con->conindid);

					if (!is_kind_included(ctx, ref_kind))
					{
						const char *kind_name =
							table_ddl_kind_names[(int) ref_kind].name;

						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("self-referential foreign key \"%s\" on table \"%s\" requires kind \"%s\" to be emitted",
										NameStr(con->conname),
										RelationGetRelationName(ctx->rel),
										kind_name),
								 errdetail("The key references an index produced by the \"%s\" kind, which is not in the active filter.",
										   kind_name),
								 errhint("Either add \"%s\" to the filter or remove \"foreign_key\" from it.",
										 kind_name)));
					}
				}
				break;
			case CONSTRAINT_EXCLUSION:
				if (!is_kind_included(ctx, TABLE_DDL_KIND_EXCLUSION))
					continue;
				break;
			case CONSTRAINT_NOTNULL:
				/*
				 * Out-of-line NOT NULL is conceptually part of the
				 * table definition: gating it on KIND_TABLE means an
				 * only=foreign_key second pass does not re-emit
				 * NOT NULLs that the first pass already created,
				 * while the no-options default still emits them.
				 *
				 * skip_notnull_oids covers all cases where NOT NULL was
				 * already materialised inline: local columns (via
				 * append_column_defs), inherited columns with a user-defined
				 * name (via the table-level CONSTRAINT clause emitted by
				 * emit_create_table_stmt), and auto-named NOT NULLs on
				 * partition children (re-created by PARTITION OF).
				 */
				if (!is_kind_included(ctx, TABLE_DDL_KIND_TABLE))
					continue;
				if (list_member_oid(ctx->skip_notnull_oids, con->oid))
					continue;
				break;
			default:
				/*
				 * Any future contype the vocabulary does not yet cover:
				 * fall through and emit it via pg_get_constraintdef_command,
				 * matching the original loop's "emit unless filtered"
				 * behavior.  This is unreachable in current PG (every
				 * contype above is enumerated) but kept defensive against
				 * a new contype being introduced.
				 */
				break;
		}

		{
			char	   *conbody = pg_get_constraintdef_body(con->oid, ctx->no_tablespace);

			resetStringInfo(&ctx->buf);
			appendStringInfo(&ctx->buf, "ALTER TABLE %s ADD CONSTRAINT %s %s;",
							 ctx->qualname,
							 quote_identifier(NameStr(con->conname)),
							 conbody);

			/*
			 * Defer FK statements so they are emitted after all other
			 * constraints.  A self-referencing FK (REFERENCES same_table)
			 * requires the PK/UNIQUE it targets to exist first, and because
			 * the catalog scan returns constraints in name order, an FK whose
			 * name sorts before the PK name would otherwise be emitted first
			 * and fail with "there is no unique constraint matching given
			 * keys".
			 */
			if (con->contype == CONSTRAINT_FOREIGN)
				fk_stmts = lappend(fk_stmts, pstrdup(ctx->buf.data));
			else
				append_stmt(ctx);

			pfree(conbody);

			/*
			 * Emit ALTER INDEX ... ALTER COLUMN n SET STATISTICS for
			 * constraint-backed indexes (PK, UNIQUE, EXCLUSION).  FK
			 * constraints have no backing index, so skip them.
			 */
			if (con->contype != CONSTRAINT_FOREIGN &&
				con->contype != CONSTRAINT_CHECK &&
				con->contype != CONSTRAINT_NOTNULL)
			{
				Oid			conidxOid = con->conindid;

				if (OidIsValid(conidxOid))
				{
					HeapTuple	indTup2 = SearchSysCache1(INDEXRELID,
														  ObjectIdGetDatum(conidxOid));

					if (HeapTupleIsValid(indTup2))
					{
						int			indnkeyatts2 =
							((Form_pg_index) GETSTRUCT(indTup2))->indnkeyatts;

						ReleaseSysCache(indTup2);
						emit_index_column_statistics(ctx, conidxOid, indnkeyatts2);
					}
				}
			}
		}
	}
	systable_endscan(conScan);
	table_close(conRel, AccessShareLock);

	/* Append deferred FK statements after all other constraints. */
	ctx->statements = list_concat(ctx->statements, fk_stmts);
}

/*
 * emit_rules
 *		CREATE RULE per cached rewrite rule on the relation.
 */
static void
emit_rules(TableDdlContext *ctx)
{
	if (!is_kind_included(ctx, TABLE_DDL_KIND_RULE) ||
		ctx->rel->rd_rules == NULL)
		return;

	for (int i = 0; i < ctx->rel->rd_rules->numLocks; i++)
	{
		Oid			ruleid = ctx->rel->rd_rules->rules[i]->ruleId;
		char		enabled = ctx->rel->rd_rules->rules[i]->enabled;
		char	   *ruledef_str;

		ruledef_str = pg_get_ruledef_ddl(ruleid);
		resetStringInfo(&ctx->buf);
		appendStringInfoString(&ctx->buf, ruledef_str);
		append_stmt(ctx);
		pfree(ruledef_str);

		/*
		 * If the rule's firing state differs from the default (ORIGIN),
		 * emit the appropriate ALTER TABLE ... ENABLE/DISABLE RULE.
		 * We need the rule's name from pg_rewrite for this statement.
		 */
		if (enabled != RULE_FIRES_ON_ORIGIN)
		{
			Relation	rewriteRel;
			ScanKeyData scankey;
			SysScanDesc scan;
			HeapTuple	ruleTup;
			char	   *rulename;

			rewriteRel = table_open(RewriteRelationId, AccessShareLock);
			ScanKeyInit(&scankey,
						Anum_pg_rewrite_oid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(ruleid));
			scan = systable_beginscan(rewriteRel, RewriteOidIndexId,
									  true, NULL, 1, &scankey);
			ruleTup = systable_getnext(scan);
			if (!HeapTupleIsValid(ruleTup))
				elog(ERROR, "cache lookup failed for rule %u", ruleid);
			rulename = pstrdup(NameStr(((Form_pg_rewrite) GETSTRUCT(ruleTup))->rulename));
			systable_endscan(scan);
			table_close(rewriteRel, AccessShareLock);

			switch (enabled)
			{
				case RULE_DISABLED:
					resetStringInfo(&ctx->buf);
					appendStringInfo(&ctx->buf,
									 "ALTER TABLE %s DISABLE RULE %s;",
									 ctx->qualname,
									 quote_identifier(rulename));
					append_stmt(ctx);
					break;
				case RULE_FIRES_ON_REPLICA:
					resetStringInfo(&ctx->buf);
					appendStringInfo(&ctx->buf,
									 "ALTER TABLE %s ENABLE REPLICA RULE %s;",
									 ctx->qualname,
									 quote_identifier(rulename));
					append_stmt(ctx);
					break;
				case RULE_FIRES_ALWAYS:
					resetStringInfo(&ctx->buf);
					appendStringInfo(&ctx->buf,
									 "ALTER TABLE %s ENABLE ALWAYS RULE %s;",
									 ctx->qualname,
									 quote_identifier(rulename));
					append_stmt(ctx);
					break;
				default:
					break;
			}
			pfree(rulename);
		}
	}
}

/*
 * emit_statistics
 *		CREATE STATISTICS per extended statistics object on the relation.
 */
static void
emit_statistics(TableDdlContext *ctx)
{
	Relation	statRel;
	SysScanDesc statScan;
	ScanKeyData statKey;
	HeapTuple	statTup;

	if (!is_kind_included(ctx, TABLE_DDL_KIND_STATISTICS))
		return;

	statRel = table_open(StatisticExtRelationId, AccessShareLock);
	ScanKeyInit(&statKey,
				Anum_pg_statistic_ext_stxrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ctx->relid));
	statScan = systable_beginscan(statRel, StatisticExtRelidIndexId,
								  true, NULL, 1, &statKey);

	while (HeapTupleIsValid(statTup = systable_getnext(statScan)))
	{
		Form_pg_statistic_ext stat = (Form_pg_statistic_ext) GETSTRUCT(statTup);
		char	   *statdef = pg_get_statisticsobjdef_ddl(stat->oid);

		resetStringInfo(&ctx->buf);
		appendStringInfo(&ctx->buf, "%s;", statdef);
		append_stmt(ctx);
		pfree(statdef);

		/* Emit ALTER STATISTICS ... SET STATISTICS if target is non-default */
		{
			bool		statnull;
			Datum		stattargetDatum = SysCacheGetAttr(STATEXTOID, statTup,
														  Anum_pg_statistic_ext_stxstattarget,
														  &statnull);

			if (!statnull)
			{
				int16		stattarget = DatumGetInt16(stattargetDatum);

				if (stattarget >= 0)
				{
					char	   *statname =
						format_name_for_emit(NameStr(stat->stxname),
											 stat->stxnamespace,
											 ctx->schema_qualified,
											 ctx->base_namespace);

					resetStringInfo(&ctx->buf);
					appendStringInfo(&ctx->buf,
									 "ALTER STATISTICS %s SET STATISTICS %d;",
									 statname, (int) stattarget);
					append_stmt(ctx);
					pfree(statname);
				}
			}
		}
	}
	systable_endscan(statScan);
	table_close(statRel, AccessShareLock);
}

/*
 * emit_replica_identity
 *		ALTER TABLE qualname REPLICA IDENTITY ... - emitted only when the
 *		relreplident differs from the default ('d' = use primary key).
 */
static void
emit_replica_identity(TableDdlContext *ctx)
{
	if (!is_kind_included(ctx, TABLE_DDL_KIND_REPLICA_IDENTITY))
		return;
	if (ctx->rel->rd_rel->relreplident == REPLICA_IDENTITY_DEFAULT)
		return;

	resetStringInfo(&ctx->buf);
	switch (ctx->rel->rd_rel->relreplident)
	{
		case REPLICA_IDENTITY_NOTHING:
			appendStringInfo(&ctx->buf,
							 "ALTER TABLE %s REPLICA IDENTITY NOTHING;",
							 ctx->qualname);
			append_stmt(ctx);
			break;
		case REPLICA_IDENTITY_FULL:
			appendStringInfo(&ctx->buf,
							 "ALTER TABLE %s REPLICA IDENTITY FULL;",
							 ctx->qualname);
			append_stmt(ctx);
			break;
		case REPLICA_IDENTITY_INDEX:
			{
				Oid			replidx = RelationGetReplicaIndex(ctx->rel);

				if (OidIsValid(replidx))
				{
					char	   *idxname = get_rel_name(replidx);

					if (idxname == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("replica identity index with OID %u does not exist",
										replidx),
								 errdetail("It may have been concurrently dropped.")));

					appendStringInfo(&ctx->buf,
									 "ALTER TABLE %s REPLICA IDENTITY USING INDEX %s;",
									 ctx->qualname,
									 quote_identifier(idxname));
					append_stmt(ctx);
					pfree(idxname);
				}
			}
			break;
	}
}

/*
 * emit_rls_toggles
 *		ALTER TABLE qualname ENABLE / FORCE ROW LEVEL SECURITY.
 *
 *		Emits the toggle statements, then warns if the table carries any
 *		policies that cannot yet be reconstructed (pg_get_policy_ddl is not
 *		yet wired up).  Without the warning, replaying the output would
 *		produce a table with RLS enabled but zero policies, silently denying
 *		all rows to non-superusers.
 */
static void
emit_rls_toggles(TableDdlContext *ctx)
{
	if (!is_kind_included(ctx, TABLE_DDL_KIND_RLS))
		return;

	if (ctx->rel->rd_rel->relrowsecurity)
	{
		Relation	polRel;
		ScanKeyData polKey;
		SysScanDesc polScan;
		bool		has_policies;

		resetStringInfo(&ctx->buf);
		appendStringInfo(&ctx->buf,
						 "ALTER TABLE %s ENABLE ROW LEVEL SECURITY;",
						 ctx->qualname);
		append_stmt(ctx);

		/*
		 * Warn if the table has policies that cannot yet be reconstructed.
		 * Without the warning, replaying the output would produce a table
		 * with RLS enabled but zero policies, silently denying all rows to
		 * non-superusers.  CREATE POLICY reconstruction is pending
		 * pg_get_policy_ddl().
		 */
		polRel = table_open(PolicyRelationId, AccessShareLock);
		ScanKeyInit(&polKey,
					Anum_pg_policy_polrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(ctx->relid));
		polScan = systable_beginscan(polRel, PolicyPolrelidPolnameIndexId,
									 true, NULL, 1, &polKey);
		has_policies = HeapTupleIsValid(systable_getnext(polScan));
		systable_endscan(polScan);
		table_close(polRel, AccessShareLock);

		if (has_policies)
			ereport(WARNING,
					(errcode(ERRCODE_WARNING),
					 errmsg("row-level security policies on table \"%s\" are not reconstructed",
							RelationGetRelationName(ctx->rel)),
					 errdetail("pg_get_table_ddl() does not yet emit CREATE POLICY "
							   "statements; policies must be added manually.")));
	}
	if (ctx->rel->rd_rel->relforcerowsecurity)
	{
		resetStringInfo(&ctx->buf);
		appendStringInfo(&ctx->buf,
						 "ALTER TABLE %s FORCE ROW LEVEL SECURITY;",
						 ctx->qualname);
		append_stmt(ctx);
	}
}

/*
 * emit_partition_children
 *		For each direct partition child of a partitioned-table parent,
 *		recursively call pg_get_table_ddl_internal and append the child's
 *		statements.  Each child's own DDL handles further levels through
 *		the same recursion.
 */
static void
emit_partition_children(TableDdlContext *ctx)
{
	List	   *children;
	ListCell   *lc;

	if (!is_kind_included(ctx, TABLE_DDL_KIND_PARTITION) ||
		ctx->rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return;

	children = find_inheritance_children(ctx->relid, AccessShareLock);
	foreach(lc, children)
	{
		Oid			childoid = lfirst_oid(lc);
		TableDdlContext childctx = {0};
		List	   *childstmts;

		check_stack_depth();
		CHECK_FOR_INTERRUPTS();

		/*
		 * Each recursive invocation re-derives its own rel, namespace,
		 * qualname, NOT NULL cache, buffer, and statement list inside
		 * pg_get_table_ddl_internal.  The user-supplied option fields
		 * carry over verbatim with one exception: KIND_PARTITION is a
		 * "gate" kind that controls whether we recurse at all, not a
		 * kind that the children themselves ever emit.  If we propagated
		 * an only-set that contained PARTITION into a child, the child
		 * would not emit its own CREATE TABLE (KIND_TABLE absent) nor
		 * any sub-objects, and the recursion would produce nothing.  So
		 * strip PARTITION out of only_kinds when recursing; if that
		 * empties the set, drop the filter entirely so the child emits
		 * its full DDL.  except_kinds passes through unchanged because
		 * PARTITION in the except-set already stopped us from getting
		 * here.
		 */
		childctx.relid = childoid;
		childctx.pretty = ctx->pretty;
		childctx.no_owner = ctx->no_owner;
		childctx.no_tablespace = ctx->no_tablespace;
		childctx.schema_qualified = ctx->schema_qualified;
		childctx.only_kinds = ctx->only_kinds;
		childctx.except_kinds = ctx->except_kinds;

		/*
		 * Judge every name in the child's DDL against the namespace of the
		 * table the caller asked for, not the child's own: when
		 * schema_qualified is false, that is the schema search_path stands
		 * in for on replay.  A child living in another schema then still
		 * comes out fully qualified, while its reference back to the parent
		 * stays bare, matching how the parent emitted itself.
		 */
		childctx.base_namespace = ctx->base_namespace;

		if (childctx.only_kinds != NULL &&
			bms_is_member((int) TABLE_DDL_KIND_PARTITION,
						  childctx.only_kinds))
		{
			Bitmapset  *child_only = bms_copy(childctx.only_kinds);

			child_only = bms_del_member(child_only,
										(int) TABLE_DDL_KIND_PARTITION);
			if (bms_is_empty(child_only))
			{
				bms_free(child_only);
				childctx.only_kinds = NULL;
			}
			else
				childctx.only_kinds = child_only;
		}

		{
			char		childkind = get_rel_relkind(childoid);

			if (childkind == RELKIND_FOREIGN_TABLE)
			{
				char	   *childname = get_rel_name(childoid);

				ereport(WARNING,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("skipping foreign-table partition \"%s\"",
								childname ? childname : "?"),
						 errdetail("pg_get_table_ddl() cannot reconstruct foreign table partitions.")));
				if (childname)
					pfree(childname);
				continue;
			}
			childstmts = pg_get_table_ddl_internal(&childctx);
		}
		ctx->statements = list_concat(ctx->statements, childstmts);
	}
	list_free(children);
}

/*
 * pg_get_table_ddl_internal
 *		Generate DDL statements to recreate a regular or partitioned table.
 *
 * The caller initializes *ctx with the user-supplied option fields
 * (relid, pretty, no_owner, no_tablespace, schema_qualified,
 * only_kinds, except_kinds).  This function opens the relation,
 * validates access, populates the derived fields (rel, qualname,
 * nn_entries, ...), runs the emission passes, and returns the
 * accumulated statement list.
 *
 * Each emission helper consults is_kind_included() to decide whether
 * it should run.  The table-proper passes (CREATE TABLE / OWNER /
 * ALTER COLUMN ... SET DEFAULT / ALTER COLUMN ... SET (...)) are
 * grouped under KIND_TABLE so the FK-only / sub-object-only workflow
 * is expressible as a single only or except list.
 *
 * Trigger and policy emission are scaffolded but currently disabled
 * (#if 0) - they will become a single helper call once the standalone
 * pg_get_trigger_ddl / pg_get_policy_ddl helpers land.
 */
static List *
pg_get_table_ddl_internal(TableDdlContext *ctx)
{
	Relation	rel;
	char		relkind;
	AclResult	aclresult;

	rel = table_open(ctx->relid, AccessShareLock);

	/* Caller needs SELECT on the table to read its definition. */
	aclresult = pg_class_aclcheck(ctx->relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		char	   *relname = pstrdup(RelationGetRelationName(rel));

		table_close(rel, AccessShareLock);
		aclcheck_error(aclresult, OBJECT_TABLE, relname);
	}

	relkind = rel->rd_rel->relkind;

	/*
	 * The initial cut only supports ordinary and partitioned tables.  Views,
	 * matviews, foreign tables, sequences, indexes, composite types, and
	 * TOAST tables are out of scope for now.
	 */
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE)
	{
		char	   *relname = pstrdup(RelationGetRelationName(rel));

		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an ordinary or partitioned table",
						relname)));
	}

	/*
	 * Validation: if the table has REPLICA IDENTITY USING INDEX and the
	 * referenced index would not be emitted under the active filter, the
	 * emitted REPLICA IDENTITY clause would reference an index the same DDL
	 * never produced.  Determine which kind would emit the index (one of
	 * primary_key / unique / exclusion for constraint-backed indexes,
	 * otherwise the generic "index" kind) and require it to be in scope
	 * whenever "replica_identity" is.  The check uses is_kind_included so
	 * it covers both forms naturally: an "except" list that omits the
	 * source kind, and an "only" list that omits it.
	 */
	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_INDEX &&
		is_kind_included(ctx, TABLE_DDL_KIND_REPLICA_IDENTITY))
	{
		Oid			replidx = RelationGetReplicaIndex(rel);

		if (OidIsValid(replidx))
		{
			TableDdlKind idx_kind = index_ddl_kind(replidx);

			if (!is_kind_included(ctx, idx_kind))
			{
				char	   *relname = pstrdup(RelationGetRelationName(rel));
				const char *idx_name = table_ddl_kind_names[(int) idx_kind].name;

				table_close(rel, AccessShareLock);
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("REPLICA IDENTITY for table \"%s\" requires kind \"%s\" to be emitted",
								relname, idx_name),
						 errdetail("The table's REPLICA IDENTITY USING INDEX references an index produced by the \"%s\" kind, which is not in the active filter.",
								   idx_name),
						 errhint("Either add \"%s\" to the filter or remove \"replica_identity\" from it.",
								 idx_name)));
			}
		}
	}

	/*
	 * Populate derived fields now that the relation is open and validated.
	 * The remaining derived fields (nn_entries, skip_notnull_oids, buf,
	 * statements) start zeroed via the caller's `TableDdlContext ctx = {0}`
	 * initializer; the NOT NULL cache is populated by collect_local_not_null,
	 * the buffer is initStringInfo'd, and each emit pass appends to
	 * statements via lappend.
	 */
	ctx->rel = rel;
	/*
	 * A partition child inherits the caller's base namespace; see
	 * emit_partition_children.
	 */
	if (!OidIsValid(ctx->base_namespace))
		ctx->base_namespace = RelationGetNamespace(rel);
	ctx->save_nestlevel = -1;
	ctx->qualname = lookup_relname_for_emit(ctx->relid, ctx->schema_qualified,
											ctx->base_namespace);

	/*
	 * Temporarily override search_path so that the ruleutils helpers
	 * (pg_get_indexdef_ddl, pg_get_constraintdef_body,
	 * pg_get_statisticsobjdef_ddl, pg_get_ruledef_ddl, etc.) produce names
	 * that match the schema_qualified flag.  Those helpers decide whether to
	 * qualify a name by calling RelationIsVisible(), which checks whether the
	 * object's schema appears in the active search_path.
	 *
	 * schema_qualified = false: narrow to the base schema so that same-schema
	 * references in DEFAULT expressions, FK targets, indexes, rules, and
	 * statistics come out unqualified automatically.  Cross-schema references
	 * stay qualified, which is the correctness requirement.
	 *
	 * schema_qualified = true: narrow to pg_catalog only so that objects in
	 * the base schema (or any user schema the caller placed on search_path)
	 * are not reachable without qualification, forcing fully-qualified output
	 * from every helper regardless of the caller's session search_path.
	 *
	 * AtEOXact_GUC cleans up at xact end if anything throws between here and
	 * the explicit restore below; on the normal path we restore right before
	 * returning.
	 */
	if (!ctx->schema_qualified)
	{
		char	   *nspname = get_namespace_name(ctx->base_namespace);

		if (nspname != NULL)
		{
			const char *qnsp = quote_identifier(nspname);

			ctx->save_nestlevel = NewGUCNestLevel();
			(void) set_config_option("search_path", qnsp,
									 PGC_USERSET, PGC_S_SESSION,
									 GUC_ACTION_SAVE, true, 0, false);
			if (qnsp != nspname)
				pfree((char *) qnsp);
			pfree(nspname);
		}
	}
	else
	{
		ctx->save_nestlevel = NewGUCNestLevel();
		(void) set_config_option("search_path", "pg_catalog",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	}

	/*
	 * Cache locally-declared NOT NULL constraint metadata so the column- emit
	 * helpers can produce "CONSTRAINT name NOT NULL" inline for user-named
	 * constraints, and so the constraint loop can avoid double-emitting them.
	 */
	ctx->nn_entries = collect_local_not_null(rel, &ctx->skip_notnull_oids);

	initStringInfo(&ctx->buf);

	/*
	 * Emission passes.  Order is significant: CREATE TABLE first; OWNER and
	 * the per-column ALTER COLUMN passes before sub-object emission;
	 * partition children before parent indexes (see comment on
	 * emit_partition_children below); non-constraint indexes before
	 * constraints (constraint-backed indexes are emitted out-of-line by the
	 * constraint loop).  Each helper gates itself on is_kind_included for
	 * the relevant TABLE_DDL_KIND_*; the four "table itself" passes are
	 * grouped here under KIND_TABLE so all of CREATE TABLE / OWNER /
	 * SET DEFAULT / SET (...) drop out together when the user asks for only
	 * sub-objects (e.g. only => 'foreign_key' for the second pass of a
	 * two-pass FK clone).
	 */
	if (is_kind_included(ctx, TABLE_DDL_KIND_TABLE))
	{
		emit_create_table_stmt(ctx);
		emit_owner_stmt(ctx);
		emit_child_default_overrides(ctx);
		emit_attoptions(ctx);
		emit_typed_column_storage(ctx);
		emit_identity_sequence_alterations(ctx);
	}

	/*
	 * Emit partition children before the parent's indexes so that
	 * CREATE TABLE ... PARTITION OF is executed while no parent indexes
	 * exist yet.  If parent indexes were emitted first, each PARTITION OF
	 * would auto-create and auto-attach a child index, making the explicit
	 * CREATE INDEX + ALTER INDEX ... ATTACH PARTITION that follows
	 * redundant (and the CREATE INDEX would fail with "already exists").
	 * With children emitted first, the parent's emit_indexes then emits
	 * CREATE INDEX ON ONLY for each partitioned index and immediately
	 * follows it with ALTER INDEX ... ATTACH PARTITION for every direct
	 * child partition index.
	 */
	emit_partition_children(ctx);
	emit_indexes(ctx);
	emit_local_constraints(ctx);
	emit_cluster_on(ctx);
	emit_rules(ctx);
	emit_statistics(ctx);
	emit_replica_identity(ctx);
	emit_rls_toggles(ctx);

	/*
	 * Triggers and row-level security policies - disabled until the
	 * standalone pg_get_trigger_ddl() and pg_get_policy_ddl() helpers land.
	 * The scan + lock + filter scaffolding below is preserved (inside #if 0)
	 * so wiring up each emission becomes a one-liner in the loop body once
	 * those helpers are available.
	 */
#if 0
	if (is_kind_included(ctx, TABLE_DDL_KIND_TRIGGER))
	{
		Relation	trigRel;
		SysScanDesc trigScan;
		ScanKeyData trigKey;
		HeapTuple	trigTup;

		trigRel = table_open(TriggerRelationId, AccessShareLock);
		ScanKeyInit(&trigKey,
					Anum_pg_trigger_tgrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(ctx->relid));
		trigScan = systable_beginscan(trigRel, TriggerRelidNameIndexId,
									  true, NULL, 1, &trigKey);
		while (HeapTupleIsValid(trigTup = systable_getnext(trigScan)))
		{
			Form_pg_trigger trg = (Form_pg_trigger) GETSTRUCT(trigTup);

			if (trg->tgisinternal)
				continue;

			/* TODO: append pg_get_trigger_ddl(trg->oid) output here. */
		}
		systable_endscan(trigScan);
		table_close(trigRel, AccessShareLock);
	}

	if (is_kind_included(ctx, TABLE_DDL_KIND_POLICY))
	{
		Relation	polRel;
		SysScanDesc polScan;
		ScanKeyData polKey;
		HeapTuple	polTup;

		polRel = table_open(PolicyRelationId, AccessShareLock);
		ScanKeyInit(&polKey,
					Anum_pg_policy_polrelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(ctx->relid));
		polScan = systable_beginscan(polRel, PolicyPolrelidPolnameIndexId,
									 true, NULL, 1, &polKey);
		while (HeapTupleIsValid(polTup = systable_getnext(polScan)))
		{
			/* TODO: append pg_get_policy_ddl(relid, polname) output here. */
		}
		systable_endscan(polScan);
		table_close(polRel, AccessShareLock);
	}
#endif

	{
		int			natts = RelationGetDescr(rel)->natts;

		for (int i = 1; i <= natts; i++)
			if (ctx->nn_entries[i].name != NULL)
				pfree(ctx->nn_entries[i].name);
	}
	pfree(ctx->nn_entries);

	table_close(rel, AccessShareLock);
	pfree(ctx->buf.data);
	pfree(ctx->qualname);
	list_free(ctx->skip_notnull_oids);

	/*
	 * Pop the narrowed search_path now that all helpers have run.  Errors
	 * thrown earlier are cleaned up by AtEOXact_GUC at xact end.
	 */
	if (ctx->save_nestlevel >= 0)
		AtEOXact_GUC(false, ctx->save_nestlevel);

	return ctx->statements;
}

/*
 * pg_get_table_ddl
 *		Return DDL to recreate a table as a set of text rows.
 */
Datum
pg_get_table_ddl(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *statements;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TableDdlContext ctx = {0};

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (PG_ARGISNULL(0))
		{
			MemoryContextSwitchTo(oldcontext);
			SRF_RETURN_DONE(funcctx);
		}

		if (!PG_ARGISNULL(5) && !PG_ARGISNULL(6))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"only_kinds\" and \"except_kinds\" parameters are mutually exclusive")));

		/* Option defaults (match proargdefaults in pg_proc.dat). */
		ctx.relid = PG_GETARG_OID(0);
		ctx.pretty = false;
		ctx.no_owner = false;		/* owner DEFAULT true  -> no_owner = false */
		ctx.no_tablespace = false;	/* tablespace DEFAULT true -> no_tablespace = false */
		ctx.schema_qualified = true;
		ctx.only_kinds = NULL;
		ctx.except_kinds = NULL;

		/* Override defaults with any explicitly supplied values. */
		if (!PG_ARGISNULL(1))
			ctx.pretty = PG_GETARG_BOOL(1);
		if (!PG_ARGISNULL(2))
			ctx.no_owner = !PG_GETARG_BOOL(2);
		if (!PG_ARGISNULL(3))
			ctx.no_tablespace = !PG_GETARG_BOOL(3);
		if (!PG_ARGISNULL(4))
			ctx.schema_qualified = PG_GETARG_BOOL(4);
		if (!PG_ARGISNULL(5))
			ctx.only_kinds = parse_kind_array("only_kinds",
											  PG_GETARG_ARRAYTYPE_P(5));
		if (!PG_ARGISNULL(6))
			ctx.except_kinds = parse_kind_array("except_kinds",
												PG_GETARG_ARRAYTYPE_P(6));

		statements = pg_get_table_ddl_internal(&ctx);
		funcctx->user_fctx = statements;
		funcctx->max_calls = list_length(statements);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	statements = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		char	   *stmt;

		stmt = list_nth(statements, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(stmt));
	}
	else
	{
		list_free_deep(statements);
		SRF_RETURN_DONE(funcctx);
	}
}
