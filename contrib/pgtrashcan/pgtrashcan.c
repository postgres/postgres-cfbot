/*-------------------------------------------------------------------------
 *
 * pgtrashcan.c
 *	  Intercept DROP TABLE and move the relation to the pgtrashcan
 *	  schema, allowing later restore (flashback) or explicit purge.
 *
 * Copyright (c) 2014, Peter Eisentraut
 * Portions Copyright (c) 2026, pgtrashcan contributors
 *
 * IDENTIFICATION
 *	  contrib/pgtrashcan/pgtrashcan.c
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>
#include <access/htup_details.h>
#include <access/xact.h>
#include <catalog/namespace.h>
#include <catalog/pg_database.h>
#include <catalog/pg_namespace.h>
#include <catalog/pg_class.h>
#include <catalog/pg_index.h>
#include <catalog/indexing.h>
#include <catalog/pg_depend.h>
#include <catalog/pg_constraint.h>
#include <commands/defrem.h>
#include <commands/tablecmds.h>
#include <commands/extension.h>
#include <commands/trigger.h>
#include <common/hashfn.h>
#include <executor/spi.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <storage/lwlock.h>
#include <tcop/utility.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/relcache.h>
#include <utils/syscache.h>
#include <utils/timestamp.h>
#include <utils/fmgroids.h>
#include <utils/guc.h>
#include <parser/scansup.h>

PG_MODULE_MAGIC;

#define TRASH_SCHEMA "pgtrashcan"
#define TRASH_CATALOG "_trash_catalog"

/*
 * Struct types for dynamic lists used in place of fixed-size arrays.
 * Each wraps information needed to process objects after SPI queries finish.
 */

/* FK constraint info for drop_foreign_keys_permanently */
typedef struct FKInfo
{
	char *conname;
	char *schema;
	char *table;
} FKInfo;

/* Trigger info for drop_triggers_permanently */
typedef struct TriggerInfo
{
	char *tgname;
} TriggerInfo;

/* View info for drop_views_permanently and recursive view detection */
typedef struct ViewInfo
{
	Oid   oid;
	char *schema;
	char *name;
	char  relkind;
	char *depends_on;  /* name of parent view (NULL if depends directly on table) */
} ViewInfo;

/* Rule info for drop_rules_permanently */
typedef struct RuleInfo
{
	char *rulename;
} RuleInfo;

/* Policy info for drop_policies_permanently */
typedef struct PolicyInfo
{
	char *polname;
} PolicyInfo;

/* Dependent object info for move_dependent_objects_cascade */
typedef struct DepInfo
{
	Oid   oid;
	char  relkind;
} DepInfo;

/* Object type enum for type-safe internal identification */
typedef enum TrashObjectType {
	TRASH_OBJ_TABLE,
	TRASH_OBJ_INDEX,
	TRASH_OBJ_SEQUENCE,
	TRASH_OBJ_VIEW,
	TRASH_OBJ_MATVIEW
} TrashObjectType;

static const char *
trash_obj_type_str(TrashObjectType type)
{
	switch (type) {
		case TRASH_OBJ_TABLE:    return "table";
		case TRASH_OBJ_INDEX:    return "index";
		case TRASH_OBJ_SEQUENCE: return "sequence";
		case TRASH_OBJ_VIEW:     return "view";
		case TRASH_OBJ_MATVIEW:  return "matview";
	}
	return "unknown";
}

static TrashObjectType
relkind_to_trash_type(char relkind)
{
	switch (relkind) {
		case 'r': return TRASH_OBJ_TABLE;
		case 'i': return TRASH_OBJ_INDEX;
		case 'S': return TRASH_OBJ_SEQUENCE;
		case 'v': return TRASH_OBJ_VIEW;
		case 'm': return TRASH_OBJ_MATVIEW;
		default:
			elog(ERROR, "unexpected relkind '%c' in relkind_to_trash_type", relkind);
			return TRASH_OBJ_TABLE;
	}
}

static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* protection flag to prevent reentrancy for DDL operations (ProcessUtility hook bypass) */
static bool pgtrashcan_in_progress = false;

/* GUC variable to control access to catalog DML operations (enforced by trigger) */
static bool pgtrashcan_internal_operation = false;

/* GUC variable to toggle the entire trash-can behavior. When false, DROP TABLE
 * on non-pgtrashcan tables falls through to standard PostgreSQL. pgtrashcan-schema
 * protections (catalog DML block, DROP SCHEMA block, etc.) remain active. */
static bool pgtrashcan_enabled = true;

static void pgtrashcan_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
							          bool readOnlyTree,
							          ProcessUtilityContext context,
							          ParamListInfo params,
							          QueryEnvironment *queryEnv,
							          DestReceiver *dest,
							          QueryCompletion *qc);

/* Function Declarations */

/* Public API - Extension Entry Points */
void _PG_init(void);

/* C trigger function for catalog DML protection */
PG_FUNCTION_INFO_V1(pgtrashcan_protect_catalog);

/* Schema & Catalog Management */
static void create_trashcan_schema(void);
static void ensure_trash_catalog_exists(void);
static void insert_catalog_entry(Oid relid, const char *orig_schema, const char *orig_name,
								 TrashObjectType obj_type, Oid dependent_on_oid, Oid cascade_parent_oid, const char *trash_name);
static void delete_catalog_entry_by_trash_name(const char *trash_name);
static void update_catalog_with_complete_metadata(const char *trash_name, const char *fk_metadata_json,
												  const char *trigger_metadata_json, const char *view_metadata_json,
												  const char *rule_metadata_json, const char *policy_metadata_json);

/* Naming & Transformation */
static char *compose_trash_name(const char *orig_name, Oid orig_oid);
static void rename_in_trash(Oid relid, const char *orig_name, const char *trash_name, bool is_index);

/* Foreign Key Handling */
static char *detect_and_build_fk_metadata_json(Oid relid, const char *orig_schema, const char *orig_name);
static const char *fk_action_name(char c);
static void drop_foreign_keys_permanently(Oid relid, List *drop_oids);

/* Trigger Handling */
static char *detect_and_build_trigger_metadata_json(Oid relid);
/* Triggers/rules/policies now preserved with table, not dropped */

/* View Handling */
static char *detect_and_build_view_metadata_json(Oid relid);
static void drop_views_permanently(Oid relid);
static List *collect_views_recursive(Oid relid, const char *depends_on_name, List **visited, int depth, MemoryContext caller_ctx);

/* Rule Handling */
static char *detect_and_build_rule_metadata_json(Oid relid);
static void drop_rules_permanently(Oid relid);

/* RLS Policy Handling */
static char *detect_and_build_policy_metadata_json(Oid relid);
static void drop_policies_permanently(Oid relid);

/* CASCADE Support */
static void move_dependent_objects_cascade(Oid parent_oid, const char *parent_schema, const char *parent_name);
static void move_single_object_to_trash(Oid relid, const char *orig_schema, const char *orig_name, char relkind, Oid parent_oid);
static void check_dependent_objects(Oid relid, const char *relname, const char *nspname, List *drop_oids);
static void check_incoming_fk_dependencies(Oid relid, const char *relname, const char *nspname, List *drop_oids);
static bool is_sequence_owned(Oid seq_oid);

/* Helper Functions */
static char *escape_json_string(const char *str);
static bool extension_exists(void);

/*
 * fk_action_name - Decode pg_constraint confdeltype/confupdtype chars
 * to the human-readable ON DELETE / ON UPDATE action names.
 */
static const char *
fk_action_name(char c)
{
	switch (c)
	{
		case 'a': return "NO ACTION";
		case 'r': return "RESTRICT";
		case 'c': return "CASCADE";
		case 'n': return "SET NULL";
		case 'd': return "SET DEFAULT";
		default:  return "NO ACTION";
	}
}

/* pgtrashcan schema name */
static const char *trashcan_nspname = TRASH_SCHEMA;

/*
 * escape_json_string - Escape a string for embedding in JSON
 *
 * Escapes all characters required by RFC 8259 (JSON spec):
 * - Backslash -> \\
 * - Double quote -> \"
 * - Newline -> \n
 * - Tab -> \t
 * - Carriage return -> \r
 * - Backspace -> \b
 * - Form feed -> \f
 * - All other control characters (0x00-0x1F) -> \uXXXX
 *
 * Returns a newly palloc'd string with JSON escaping applied.
 */
static char *
escape_json_string(const char *str)
{
	StringInfoData buf;
	const char *p;

	if (str == NULL)
		return pstrdup("");

	initStringInfo(&buf);

	for (p = str; *p; p++)
	{
		unsigned char ch = (unsigned char) *p;

		switch (ch)
		{
			case '\\':
				appendStringInfoString(&buf, "\\\\");
				break;
			case '"':
				appendStringInfoString(&buf, "\\\"");
				break;
			case '\n':
				appendStringInfoString(&buf, "\\n");
				break;
			case '\t':
				appendStringInfoString(&buf, "\\t");
				break;
			case '\r':
				appendStringInfoString(&buf, "\\r");
				break;
			case '\b':
				appendStringInfoString(&buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(&buf, "\\f");
				break;
			default:
				if (ch < 0x20)
				{
					/* All other control characters: escape as \u00XX */
					appendStringInfo(&buf, "\\u%04x", ch);
				}
				else
				{
					appendStringInfoChar(&buf, *p);
				}
				break;
		}
	}

	return buf.data;
}

void
_PG_init(void)
{
	elog(DEBUG1, "pgtrashcan loaded");

	DefineCustomBoolVariable(
		"pgtrashcan.internal_operation",
		"Allow internal catalog modifications by pgtrashcan",
		NULL,
		&pgtrashcan_internal_operation,
		false,
		PGC_SUSET,
		0,
		NULL, NULL, NULL);

	DefineCustomBoolVariable(
		"pgtrashcan.enabled",
		"Whether DROP TABLE moves tables to pgtrashcan (true) or drops them normally (false)",
		"When false, DROP TABLE behaves like stock PostgreSQL. pgtrashcan-schema protections remain active.",
		&pgtrashcan_enabled,
		true,
		PGC_USERSET,
		0,
		NULL, NULL, NULL);

	prev_ProcessUtility = ProcessUtility_hook;
	if (!prev_ProcessUtility)
		prev_ProcessUtility = standard_ProcessUtility;
	ProcessUtility_hook = pgtrashcan_ProcessUtility;
}

/*
 * pgtrashcan_protect_catalog - C trigger function to block direct DML
 * on _trash_catalog. Checks the pgtrashcan.internal_operation GUC;
 * if false, raises ERROR. The GUC is PGC_SUSET so only superusers
 * (or SECURITY DEFINER functions owned by superuser) can set it.
 */
Datum
pgtrashcan_protect_catalog(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;

	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function \"pgtrashcan_protect_catalog\" must be called as a trigger")));

	if (!pgtrashcan_internal_operation)
		ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("direct modification of trash catalog table is not allowed"),
			 errhint("Use pgtrashcan functions (restore, purge, rename) to manage trash")));

	if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
		return PointerGetDatum(trigdata->tg_trigtuple);
	else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		return PointerGetDatum(trigdata->tg_newtuple);
	else if (TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		return PointerGetDatum(trigdata->tg_trigtuple);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pgtrashcan_protect_catalog fired for unexpected trigger event")));

	PG_RETURN_NULL();  /* unreachable, added to avoid compiler warning */
}


static RangeVar *
makeRangeVarFromAnyName(List *names)
{
	RangeVar *r = makeNode(RangeVar);

	switch (list_length(names))
	{
		case 1:
			r->catalogname = NULL;
			r->schemaname = NULL;
			r->relname = strVal(linitial(names));
			break;
		case 2:
			r->catalogname = NULL;
			r->schemaname = strVal(linitial(names));
			r->relname = strVal(lsecond(names));
			break;
		case 3:
			r->catalogname = strVal(linitial(names));
			r->schemaname = strVal(lsecond(names));
			r->relname = strVal(lthird(names));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper qualified name (too many dotted names): %s",
							NameListToString(names))));
			break;
	}

	r->relpersistence = RELPERSISTENCE_PERMANENT;
	r->location = -1;

	return r;
}

/*
 * extension_exists - Check if pgtrashcan extension is installed
 *
 * Returns true if the extension exists, false otherwise.
 * Used to determine if we should protect the pgtrashcan schema.
 * If extension was dropped, allow schema to be dropped too.
 */
static bool
extension_exists(void)
{
	bool exists = false;
	Oid ext_oid;

	/* Check if pgtrashcan extension exists in pg_extension */
	ext_oid = get_extension_oid("pgtrashcan", true);
	exists = OidIsValid(ext_oid);

	elog(DEBUG1, "pgtrashcan: extension_exists() = %s", exists ? "true" : "false");
	return exists;
}

/*
 * create_trashcan_schema - Ensure pgtrashcan schema exists (safety net)
 *
 * The schema is normally created by the SQL install script.
 * This is a no-op if the schema already exists.
 */
static void
create_trashcan_schema(void)
{
	HeapTuple   tuple;
	Oid			datdba;

	if (SearchSysCacheExists1(NAMESPACENAME, PointerGetDatum(trashcan_nspname)))
		return;

	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database with OID %u does not exist", MyDatabaseId)));

	datdba = ((Form_pg_database) GETSTRUCT(tuple))->datdba;
	ReleaseSysCache(tuple);

	NamespaceCreate(trashcan_nspname, datdba, false);

	CommandCounterIncrement();
}

/*
 * ensure_trash_catalog_exists - Verify catalog table exists
 *
 * The catalog table and schema are created by the SQL install script
 * (pgtrashcan--1.0.sql). This function just verifies they exist and
 * raises a clear error if not. No schema duplication.
 */
static void
ensure_trash_catalog_exists(void)
{
	Oid schema_oid = get_namespace_oid(TRASH_SCHEMA, true);
	if (!OidIsValid(schema_oid))
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("trash schema \"%s\" does not exist", TRASH_SCHEMA),
			 errhint("Run CREATE EXTENSION pgtrashcan to initialize")));

	if (!OidIsValid(get_relname_relid(TRASH_CATALOG, schema_oid)))
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("trash catalog table \"%s\".\"%s\" does not exist", TRASH_SCHEMA, TRASH_CATALOG),
			 errhint("Run CREATE EXTENSION pgtrashcan to initialize")));
}

/*
 * compose_trash_name - Generate unique trash name for the object: trash_$<oid>
 *
 * Uses PostgreSQL's truncate_identifier() for proper handling of
 * multibyte characters and NAMEDATALEN limits.
 */
static char *
compose_trash_name(const char *orig_name, Oid orig_oid)
{
	char trash_name[NAMEDATALEN];

	/*
	 * Format: trash_$<oid>
	 *
	 * "trash_$" (7 chars) + max OID "4294967295" (10 chars) = 17 chars max.
	 * Always well within NAMEDATALEN-1 (63 chars). No truncation ever needed.
	 *
	 * OID uniqueness guarantees no trash name collisions.
	 */
	snprintf(trash_name, sizeof(trash_name), "trash_$%u", orig_oid);

	elog(DEBUG1, "pgtrashcan: Generated trash_name='%s' from orig_name='%s' oid=%u",
		 trash_name, orig_name, orig_oid);

	return pstrdup(trash_name);
}

/*
 * insert_catalog_entry - Record object in trash catalog
 *
 * Parameters:
 *   relid - OID of the object
 *   orig_schema - Original schema name
 *   orig_name - Original object name
 *   obj_type - Object type (TrashObjectType enum)
 *   dependent_on_oid - OID of parent for true dependents (indexes, sequences); InvalidOid otherwise
 *   cascade_parent_oid - OID of parent for CASCADE'd tables; InvalidOid otherwise
 *   trash_name - Pre-generated trash name
 */
static void
insert_catalog_entry(Oid relid,
					const char *orig_schema,
					const char *orig_name,
					TrashObjectType obj_type,
					Oid dependent_on_oid,
					Oid cascade_parent_oid,
					const char *trash_name)
{
	int ret;
	StringInfoData sql;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in insert_catalog_entry")));

	PG_TRY();
	{
		/* Set flags inside PG_TRY so PG_FINALLY always resets them */
		pgtrashcan_in_progress = true;
		SetConfigOption("pgtrashcan.internal_operation", "true", PGC_SUSET, PGC_S_SESSION);
		initStringInfo(&sql);

		/* Insert catalog entry */
		appendStringInfo(&sql,
			"INSERT INTO \"%s\".\"%s\" "
			"(trash_name, orig_name, orig_schema, orig_oid, orig_obj_type, "
			" drop_time, dropped_by, dependent_on_oid, cascade_parent_oid) "
			"VALUES (%s, %s, %s, %u, %s, now(), current_user, %s, %s)",
			TRASH_SCHEMA, TRASH_CATALOG,
			quote_literal_cstr(trash_name),
			quote_literal_cstr(orig_name),
			quote_literal_cstr(orig_schema),
			relid,
			quote_literal_cstr(trash_obj_type_str(obj_type)),
			OidIsValid(dependent_on_oid) ? psprintf("%u", dependent_on_oid) : "NULL",
			OidIsValid(cascade_parent_oid) ? psprintf("%u", cascade_parent_oid) : "NULL");

		ret = SPI_execute(sql.data, false, 0);
		if (ret != SPI_OK_INSERT)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("failed to insert catalog entry for %s (OID %u)",
							orig_name, relid)));

		elog(DEBUG1, "pgtrashcan: Inserted catalog entry: trash_name=%s, orig_name=%s, type=%s, dependent_on=%u, cascade_parent=%u",
			 trash_name, orig_name, trash_obj_type_str(obj_type), dependent_on_oid, cascade_parent_oid);

		pfree(sql.data);

		CommandCounterIncrement();
	}
	PG_FINALLY();
	{
		/* Always clean up SPI and reset flags, even on error */
		SPI_finish();
		pgtrashcan_in_progress = false;
		SetConfigOption("pgtrashcan.internal_operation", "false", PGC_SUSET, PGC_S_SESSION);
	}
	PG_END_TRY();
}

/*
 * rename_in_trash - Rename object in pgtrashcan schema to trash_name
 *
 * Uses RenameRelationInternal() to directly update the catalog,
 * bypassing the ProcessUtility hook pipeline.
 */
static void
rename_in_trash(Oid relid, const char *orig_name, const char *trash_name, bool is_index)
{
	elog(DEBUG1, "pgtrashcan: Renaming '%s' to '%s' in pgtrashcan schema (OID %u)",
		 orig_name, trash_name, relid);

	RenameRelationInternal(relid, trash_name, true, is_index);

	CommandCounterIncrement();
}

/*
 * delete_catalog_entry_by_trash_name - Remove catalog entry
 *
 * Used when permanently dropping from pgtrashcan
 */
static void
delete_catalog_entry_by_trash_name(const char *trash_name)
{
	int ret;
	StringInfoData sql;
	Oid parent_oid = InvalidOid;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed")));

	PG_TRY();
	{
		/* Set flags inside PG_TRY so PG_FINALLY always resets them */
		pgtrashcan_in_progress = true;
		SetConfigOption("pgtrashcan.internal_operation", "true", PGC_SUSET, PGC_S_SESSION);

		initStringInfo(&sql);

		/* First, get the orig_oid of the table being dropped (to find dependents) */
		appendStringInfo(&sql,
			"SELECT orig_oid FROM \"%s\".\"%s\" WHERE trash_name = %s AND orig_obj_type = 'table'",
			TRASH_SCHEMA, TRASH_CATALOG, quote_literal_cstr(trash_name));

		ret = SPI_execute(sql.data, true, 1);
		if (ret == SPI_OK_SELECT && SPI_processed > 0)
		{
			bool isnull;
			TupleDesc tupdesc = SPI_tuptable->tupdesc;
			int fno_orig_oid = SPI_fnumber(tupdesc, "orig_oid");
			if (fno_orig_oid < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("column 'orig_oid' not found in query result")));
			parent_oid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], tupdesc, fno_orig_oid, &isnull));
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("orig_oid is NULL for trash_name '%s'", trash_name)));
		}

		/* Delete the entry itself and all dependent entries */
		resetStringInfo(&sql);
		if (OidIsValid(parent_oid))
		{
			/* Delete parent and all dependents */
			appendStringInfo(&sql,
				"DELETE FROM \"%s\".\"%s\" WHERE trash_name = %s OR dependent_on_oid = %u",
				TRASH_SCHEMA, TRASH_CATALOG, quote_literal_cstr(trash_name), parent_oid);
		}
		else
		{
			/* Just delete the single entry (might be an index or sequence) */
			appendStringInfo(&sql,
				"DELETE FROM \"%s\".\"%s\" WHERE trash_name = %s",
				TRASH_SCHEMA, TRASH_CATALOG, quote_literal_cstr(trash_name));
		}

		ret = SPI_execute(sql.data, false, 0);
		if (ret != SPI_OK_DELETE)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to delete catalog entry for %s", trash_name)));

		elog(DEBUG1, "pgtrashcan: Deleted catalog entry for trash_name=%s (and dependents)", trash_name);

		pfree(sql.data);
		CommandCounterIncrement();
	}
	PG_FINALLY();
	{
		/* Always clean up SPI and reset flags, even on error */
		SPI_finish();
		pgtrashcan_in_progress = false;
		SetConfigOption("pgtrashcan.internal_operation", "false", PGC_SUSET, PGC_S_SESSION);
	}
	PG_END_TRY();
}

/*
 * detect_and_build_fk_metadata_json - Detect all FK constraints and build JSON metadata
 *
 * Queries pg_constraint for:
 * - Outgoing FKs (where conrelid = relid): This table references others
 * - Incoming FKs (where confrelid = relid): Other tables reference this table
 *
 * Returns a JSON string with FK definitions for storage in catalog metadata column
 */
static char *
detect_and_build_fk_metadata_json(Oid relid, const char *orig_schema, const char *orig_name)
{
	StringInfoData json;
	StringInfoData sql;
	int ret;
	bool first_fk = true;
	SPITupleTable *tuptable;
	int i;

	initStringInfo(&json);
	initStringInfo(&sql);

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in detect_and_build_fk_metadata_json")));

	PG_TRY();
	{
		appendStringInfoString(&json, "{\"foreign_keys_dropped\": [");

		/*
		 * Outgoing FKs — constraints owned by this table that reference another table.
		 * The ALTER TABLE target in the reconstructed DDL is the trashed table itself
		 * (orig_schema.orig_name), passed in by the caller.
		 */
		appendStringInfo(&sql,
			"SELECT c.conname, "
			"  n.nspname AS ref_schema, cl.relname AS ref_table, "
			"  c.confdeltype, c.confupdtype, c.condeferrable, c.condeferred, "
			"  (SELECT COALESCE(array_to_json(array_agg(a.attname ORDER BY k.ord))::text, '[]') "
			"     FROM unnest(c.conkey) WITH ORDINALITY AS k(attnum, ord) "
			"     JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = k.attnum) AS cols_json, "
			"  (SELECT COALESCE(array_to_json(array_agg(a.attname ORDER BY k.ord))::text, '[]') "
			"     FROM unnest(c.confkey) WITH ORDINALITY AS k(attnum, ord) "
			"     JOIN pg_attribute a ON a.attrelid = c.confrelid AND a.attnum = k.attnum) AS refcols_json, "
			"  pg_get_constraintdef(c.oid, true) AS defclause "
			"FROM pg_constraint c "
			"JOIN pg_class cl ON c.confrelid = cl.oid "
			"JOIN pg_namespace n ON cl.relnamespace = n.oid "
			"WHERE c.conrelid = %u AND c.contype = 'f'", relid);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query outgoing foreign keys")));

		tuptable = SPI_tuptable;
		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			Datum datum;
			char *conname, *ref_schema, *ref_table;
			char *cols_json, *refcols_json, *defclause;
			char confdeltype, confupdtype;
			bool condeferrable, condeferred;
			StringInfoData ddl;

			if (!first_fk)
				appendStringInfoString(&json, ", ");
			first_fk = false;

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "conname"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("conname is NULL in FK query")));
			conname = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "ref_schema"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("ref_schema is NULL in FK query")));
			ref_schema = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "ref_table"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("ref_table is NULL in FK query")));
			ref_table = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "confdeltype"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("confdeltype is NULL in FK query")));
			confdeltype = DatumGetChar(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "confupdtype"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("confupdtype is NULL in FK query")));
			confupdtype = DatumGetChar(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "condeferrable"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("condeferrable is NULL in FK query")));
			condeferrable = DatumGetBool(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "condeferred"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("condeferred is NULL in FK query")));
			condeferred = DatumGetBool(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "cols_json"), &isnull);
			cols_json = isnull ? pstrdup("[]") : TextDatumGetCString(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "refcols_json"), &isnull);
			refcols_json = isnull ? pstrdup("[]") : TextDatumGetCString(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "defclause"), &isnull);
			defclause = isnull ? pstrdup("") : TextDatumGetCString(datum);

			/* Build the runnable DDL: ALTER TABLE <orig_schema>.<orig_name> ADD CONSTRAINT <conname> <defclause>; */
			initStringInfo(&ddl);
			appendStringInfo(&ddl, "ALTER TABLE %s.%s ADD CONSTRAINT %s %s;",
				quote_identifier(orig_schema),
				quote_identifier(orig_name),
				quote_identifier(conname),
				defclause);

			{
				char *esc_conname = escape_json_string(conname);
				char *esc_ref_schema = escape_json_string(ref_schema);
				char *esc_ref_table = escape_json_string(ref_table);
				char *esc_ddl = escape_json_string(ddl.data);

				appendStringInfo(&json,
					"{\"constraint_name\": \"%s\", "
					"\"constraint_type\": \"outgoing\", "
					"\"referenced_schema\": \"%s\", "
					"\"referenced_table\": \"%s\", "
					"\"columns\": %s, "
					"\"referenced_columns\": %s, "
					"\"on_delete\": \"%s\", "
					"\"on_update\": \"%s\", "
					"\"deferrable\": %s, "
					"\"deferred\": %s, "
					"\"ddl\": \"%s\"}",
					esc_conname,
					esc_ref_schema,
					esc_ref_table,
					cols_json,
					refcols_json,
					fk_action_name(confdeltype),
					fk_action_name(confupdtype),
					condeferrable ? "true" : "false",
					condeferred ? "true" : "false",
					esc_ddl);

				pfree(esc_conname);
				pfree(esc_ref_schema);
				pfree(esc_ref_table);
				pfree(esc_ddl);
			}

			pfree(ddl.data);
			pfree(cols_json);
			pfree(refcols_json);
			pfree(defclause);
		}

		/*
		 * Incoming FKs — constraints on OTHER tables that reference this table.
		 * Self-referencing FKs are already captured above as outgoing; exclude them here.
		 * The ALTER TABLE target in the reconstructed DDL is the source (other) table.
		 */
		resetStringInfo(&sql);
		appendStringInfo(&sql,
			"SELECT c.conname, "
			"  sn.nspname AS source_schema, scl.relname AS source_table, "
			"  rn.nspname AS ref_schema,    rcl.relname AS ref_table, "
			"  c.confdeltype, c.confupdtype, c.condeferrable, c.condeferred, "
			"  (SELECT COALESCE(array_to_json(array_agg(a.attname ORDER BY k.ord))::text, '[]') "
			"     FROM unnest(c.conkey) WITH ORDINALITY AS k(attnum, ord) "
			"     JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = k.attnum) AS cols_json, "
			"  (SELECT COALESCE(array_to_json(array_agg(a.attname ORDER BY k.ord))::text, '[]') "
			"     FROM unnest(c.confkey) WITH ORDINALITY AS k(attnum, ord) "
			"     JOIN pg_attribute a ON a.attrelid = c.confrelid AND a.attnum = k.attnum) AS refcols_json, "
			"  pg_get_constraintdef(c.oid, true) AS defclause "
			"FROM pg_constraint c "
			"JOIN pg_class scl  ON c.conrelid  = scl.oid "
			"JOIN pg_namespace sn ON scl.relnamespace = sn.oid "
			"JOIN pg_class rcl  ON c.confrelid = rcl.oid "
			"JOIN pg_namespace rn ON rcl.relnamespace = rn.oid "
			"WHERE c.confrelid = %u AND c.contype = 'f' AND c.conrelid != c.confrelid", relid);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query incoming foreign keys")));

		tuptable = SPI_tuptable;
		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			Datum datum;
			char *conname, *source_schema, *source_table, *ref_schema, *ref_table;
			char *cols_json, *refcols_json, *defclause;
			char confdeltype, confupdtype;
			bool condeferrable, condeferred;
			StringInfoData ddl;

			if (!first_fk)
				appendStringInfoString(&json, ", ");
			first_fk = false;

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "conname"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("conname is NULL in incoming FK query")));
			conname = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "source_schema"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("source_schema is NULL in incoming FK query")));
			source_schema = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "source_table"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("source_table is NULL in incoming FK query")));
			source_table = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "ref_schema"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("ref_schema is NULL in incoming FK query")));
			ref_schema = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "ref_table"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("ref_table is NULL in incoming FK query")));
			ref_table = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "confdeltype"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("confdeltype is NULL in incoming FK query")));
			confdeltype = DatumGetChar(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "confupdtype"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("confupdtype is NULL in incoming FK query")));
			confupdtype = DatumGetChar(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "condeferrable"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("condeferrable is NULL in incoming FK query")));
			condeferrable = DatumGetBool(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "condeferred"), &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("condeferred is NULL in incoming FK query")));
			condeferred = DatumGetBool(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "cols_json"), &isnull);
			cols_json = isnull ? pstrdup("[]") : TextDatumGetCString(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "refcols_json"), &isnull);
			refcols_json = isnull ? pstrdup("[]") : TextDatumGetCString(datum);

			datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, "defclause"), &isnull);
			defclause = isnull ? pstrdup("") : TextDatumGetCString(datum);

			/* ALTER TABLE target = source (live) table that owns the FK. */
			initStringInfo(&ddl);
			appendStringInfo(&ddl, "ALTER TABLE %s.%s ADD CONSTRAINT %s %s;",
				quote_identifier(source_schema),
				quote_identifier(source_table),
				quote_identifier(conname),
				defclause);

			{
				char *esc_conname = escape_json_string(conname);
				char *esc_source_schema = escape_json_string(source_schema);
				char *esc_source_table = escape_json_string(source_table);
				char *esc_ref_schema = escape_json_string(ref_schema);
				char *esc_ref_table = escape_json_string(ref_table);
				char *esc_ddl = escape_json_string(ddl.data);

				appendStringInfo(&json,
					"{\"constraint_name\": \"%s\", "
					"\"constraint_type\": \"incoming\", "
					"\"source_schema\": \"%s\", "
					"\"source_table\": \"%s\", "
					"\"referenced_schema\": \"%s\", "
					"\"referenced_table\": \"%s\", "
					"\"columns\": %s, "
					"\"referenced_columns\": %s, "
					"\"on_delete\": \"%s\", "
					"\"on_update\": \"%s\", "
					"\"deferrable\": %s, "
					"\"deferred\": %s, "
					"\"ddl\": \"%s\"}",
					esc_conname,
					esc_source_schema,
					esc_source_table,
					esc_ref_schema,
					esc_ref_table,
					cols_json,
					refcols_json,
					fk_action_name(confdeltype),
					fk_action_name(confupdtype),
					condeferrable ? "true" : "false",
					condeferred ? "true" : "false",
					esc_ddl);

				pfree(esc_conname);
				pfree(esc_source_schema);
				pfree(esc_source_table);
				pfree(esc_ref_schema);
				pfree(esc_ref_table);
				pfree(esc_ddl);
			}

			pfree(ddl.data);
			pfree(cols_json);
			pfree(refcols_json);
			pfree(defclause);
		}

		appendStringInfoString(&json, "]}");

		pfree(sql.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	return json.data;
}

/*
 * check_incoming_fk_dependencies - Check for incoming FK constraints and error if found
 *
 * This function enforces PostgreSQL's standard dependency checking behavior.
 * If other tables have FKs referencing this table, the drop should fail
 * unless CASCADE is specified.
 *
 * Parameters:
 *   drop_oids - List of OIDs being dropped in the same DROP statement
 *               These are excluded from dependency checks (PostgreSQL behavior)
 */
static void
check_incoming_fk_dependencies(Oid relid, const char *relname, const char *nspname, List *drop_oids)
{
	StringInfoData sql;
	StringInfoData exclusion;
	int ret;
	int incoming_count;
	StringInfoData detail_msg;

	initStringInfo(&sql);
	initStringInfo(&exclusion);
	initStringInfo(&detail_msg);

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in check_incoming_fk_dependencies")));

	PG_TRY();
	{
		/*
		 * Build exclusion clause for tables in the same DROP statement
		 * PostgreSQL allows DROP TABLE parent, child even when child has FK to parent
		 */
		if (drop_oids != NIL)
		{
			ListCell *lc;
			bool first = true;

			appendStringInfoString(&exclusion, " AND c.conrelid NOT IN (");
			foreach(lc, drop_oids)
			{
				if (!first)
					appendStringInfoString(&exclusion, ", ");
				appendStringInfo(&exclusion, "%u", lfirst_oid(lc));
				first = false;
			}
			appendStringInfoString(&exclusion, ")");
		}

		/*
		 * Check for incoming FKs (other tables reference this table)
		 * Exclude:
		 * - Self-referencing FKs - those are internal to the table being dropped
		 * - Tables in the same DROP statement (drop_oids list)
		 */
		appendStringInfo(&sql,
			"SELECT c.conname, n.nspname || '.' || cl.relname AS referencing_table "
			"FROM pg_constraint c "
			"JOIN pg_class cl ON c.conrelid = cl.oid "
			"JOIN pg_namespace n ON cl.relnamespace = n.oid "
			"WHERE c.confrelid = %u AND c.contype = 'f' AND c.conrelid != c.confrelid "
			"%s "  /* Exclusion clause */
			"LIMIT 5", relid, exclusion.data);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query incoming foreign keys")));

		incoming_count = SPI_processed;

		if (incoming_count > 0)
		{
			SPITupleTable *tuptable = SPI_tuptable;
			int i;

			/* Build detailed error message listing the dependent constraints */
			for (i = 0; i < incoming_count; i++)
			{
				HeapTuple tuple = tuptable->vals[i];
				TupleDesc tupdesc = tuptable->tupdesc;
				bool isnull;
				char *conname;
				char *ref_table;
				Datum datum;
				int fno_conname, fno_referencing_table;

				/* Get column numbers */
				fno_conname = SPI_fnumber(tupdesc, "conname");
				fno_referencing_table = SPI_fnumber(tupdesc, "referencing_table");

				if (fno_conname < 0)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("column 'conname' not found in FK dependency check")));
				if (fno_referencing_table < 0)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("column 'referencing_table' not found in FK dependency check")));

				/* Get constraint name (NAME type) */
				datum = SPI_getbinval(tuple, tupdesc, fno_conname, &isnull);
				if (isnull)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("conname is NULL in FK dependency check")));
				conname = NameStr(*DatumGetName(datum));

				/* Get referencing table (TEXT type - concatenated string) */
				datum = SPI_getbinval(tuple, tupdesc, fno_referencing_table, &isnull);
				if (isnull)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("referencing_table is NULL in FK dependency check")));
				ref_table = TextDatumGetCString(datum);

				if (i > 0)
					appendStringInfoString(&detail_msg, "\n");
				appendStringInfo(&detail_msg, "constraint %s on table %s depends on table %s.%s",
								 conname, ref_table, nspname, relname);

				pfree(ref_table);  /* Free the TextDatumGetCString allocation */
			}

			/*
			 * Raise error - PG_FINALLY will call SPI_finish() before the error
			 * propagates to the caller.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop table %s.%s because other objects depend on it",
							nspname, relname),
					 errdetail("%s", detail_msg.data),
					 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));
		}

		pfree(sql.data);
		pfree(exclusion.data);
		pfree(detail_msg.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();
}

/*
 * check_dependent_objects - Check for any dependent objects and error if found
 *
 * This function enforces PostgreSQL's standard dependency checking behavior.
 * If DROP RESTRICT is specified and there are dependent objects (views, matviews,
 * tables with FKs, sequences), the drop should fail with an error message.
 *
 * This is called before moving the table to trash to match PostgreSQL's behavior.
 *
 * Parameters:
 *   drop_oids - List of OIDs being dropped in the same DROP statement
 *               These are excluded from dependency checks (PostgreSQL behavior)
 */
static void
check_dependent_objects(Oid relid, const char *relname, const char *nspname, List *drop_oids)
{
	StringInfoData sql;
	StringInfoData exclusion;
	int ret;
	int dependent_count;
	StringInfoData detail_msg;
	SPITupleTable *tuptable;
	int i;

	initStringInfo(&sql);
	initStringInfo(&exclusion);
	initStringInfo(&detail_msg);

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in check_dependent_objects")));

	PG_TRY();
	{
		/*
		 * Build exclusion clause for tables in the same DROP statement
		 * PostgreSQL allows DROP TABLE parent, child even when child has FK to parent
		 */
		if (drop_oids != NIL)
		{
			ListCell *lc;
			bool first = true;

			appendStringInfoString(&exclusion, " AND c.oid NOT IN (");
			foreach(lc, drop_oids)
			{
				if (!first)
					appendStringInfoString(&exclusion, ", ");
				appendStringInfo(&exclusion, "%u", lfirst_oid(lc));
				first = false;
			}
			appendStringInfoString(&exclusion, ")");
		}

		/*
		 * Check for all dependent objects (views, matviews, tables, sequences)
		 * Exclude:
		 * - Indexes (they move with tables automatically)
		 * - Self-dependencies
		 * - Objects already in pgtrashcan schema
		 * - System objects
		 * - Auto dependencies (deptype 'a') - these are OWNED BY relationships (e.g., SERIAL sequences)
		 *   that are automatically dropped/moved with the table
		 * - Tables in the same DROP statement (drop_oids list)
		 *
		 * Note: We only check for normal dependencies (deptype 'n') which require CASCADE.
		 * Views have dependencies via pg_rewrite, so we need a UNION query:
		 * 1. Direct dependencies (tables, etc.)
		 * 2. View dependencies via pg_rewrite rules
		 */
		appendStringInfo(&sql,
			"SELECT DISTINCT c.relkind, n.nspname, c.relname, "
			"  CASE c.relkind "
			"    WHEN 'v' THEN 'view' "
			"    WHEN 'm' THEN 'materialized view' "
			"    WHEN 'r' THEN 'table' "
			"    WHEN 'S' THEN 'sequence' "
			"    ELSE 'unknown' END AS obj_type "
			"FROM pg_depend d "
			"JOIN pg_class c ON c.oid = d.objid "
			"JOIN pg_namespace n ON n.oid = c.relnamespace "
			"WHERE d.refobjid = %u "
			"  AND d.deptype = 'n' "  /* Only normal dependencies, not auto ('a') or internal ('i') */
			"  AND c.relkind IN ('r', 'v', 'm', 'S') "
			"  AND c.oid != %u "
			"  AND c.oid >= 16384 "
			"  AND n.nspname != 'pgtrashcan' "
			"  %s "  /* Exclusion clause */
			"UNION "
			"SELECT DISTINCT c.relkind, n.nspname, c.relname, "
			"  CASE c.relkind "
			"    WHEN 'v' THEN 'view' "
			"    WHEN 'm' THEN 'materialized view' "
			"    ELSE 'unknown' END AS obj_type "
			"FROM pg_depend d "
			"JOIN pg_rewrite rw ON rw.oid = d.objid "
			"JOIN pg_class c ON c.oid = rw.ev_class "
			"JOIN pg_namespace n ON n.oid = c.relnamespace "
			"WHERE d.refobjid = %u "
			"  AND d.deptype = 'n' "  /* Only normal dependencies */
			"  AND c.relkind IN ('v', 'm') "
			"  AND c.oid != %u "
			"  AND c.oid >= 16384 "
			"  AND n.nspname != 'pgtrashcan' "
			"  %s "  /* Exclusion clause */
			"LIMIT 5", relid, relid, exclusion.data, relid, relid, exclusion.data);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query dependent objects")));

		dependent_count = SPI_processed;

		elog(DEBUG1, "pgtrashcan: check_dependent_objects for %s.%s (OID %u) found %d dependents",
			 nspname, relname, relid, dependent_count);

		if (dependent_count > 0)
		{
			tuptable = SPI_tuptable;

			/* Build detailed error message listing the dependent objects */
			for (i = 0; i < dependent_count; i++)
			{
				HeapTuple tuple = tuptable->vals[i];
				TupleDesc tupdesc = tuptable->tupdesc;
				bool isnull;
				char *dep_schema;
				char *dep_name;
				char *dep_type;
				Datum datum;
				int fno_nspname, fno_relname, fno_obj_type;

				/* Get column numbers */
				fno_nspname = SPI_fnumber(tupdesc, "nspname");
				fno_relname = SPI_fnumber(tupdesc, "relname");
				fno_obj_type = SPI_fnumber(tupdesc, "obj_type");

				if (fno_nspname < 0 || fno_relname < 0 || fno_obj_type < 0)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("required column not found in dependent objects query")));

				/* Get dependent object schema */
				datum = SPI_getbinval(tuple, tupdesc, fno_nspname, &isnull);
				if (isnull)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("nspname is NULL in dependent objects query")));
				dep_schema = NameStr(*DatumGetName(datum));

				/* Get dependent object name */
				datum = SPI_getbinval(tuple, tupdesc, fno_relname, &isnull);
				if (isnull)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("relname is NULL in dependent objects query")));
				dep_name = NameStr(*DatumGetName(datum));

				/* Get dependent object type */
				datum = SPI_getbinval(tuple, tupdesc, fno_obj_type, &isnull);
				if (isnull)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("obj_type is NULL in dependent objects query")));
				dep_type = TextDatumGetCString(datum);

				if (i > 0)
					appendStringInfoString(&detail_msg, "\n");
				appendStringInfo(&detail_msg, "%s %s.%s depends on table %s.%s",
								 dep_type, dep_schema, dep_name, nspname, relname);

				pfree(dep_type);  /* Free the TextDatumGetCString allocation */
			}

			/*
			 * Raise error - PG_FINALLY will call SPI_finish() before the error
			 * propagates to the caller.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop table %s.%s because other objects depend on it",
							nspname, relname),
					 errdetail("%s", detail_msg.data),
					 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));
		}

		pfree(sql.data);
		pfree(exclusion.data);
		pfree(detail_msg.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();
}


/*
 * drop_foreign_keys_permanently - Drop all FK constraints permanently before moving to pgtrashcan
 *
 * Drops both:
 * - Outgoing FKs (this table references others)
 * - Incoming FKs (other tables reference this table)
 *
 * Parameters:
 *   drop_oids - List of OIDs being dropped in the same DROP statement
 *               Incoming FKs from these tables are skipped (they'll be handled when that table is processed)
 *
 * Uses catalog deletion for cleanliness
 */
static void
drop_foreign_keys_permanently(Oid relid, List *drop_oids)
{
	StringInfoData sql;
	StringInfoData drop_cmd;
	StringInfoData exclusion;
	int ret;
	SPITupleTable *tuptable;
	int i;
	int fk_count = 0;
	int outgoing_count = 0;
	int incoming_count = 0;
	char *relname;
	char *nspname;
	MemoryContext caller_ctx;

	/* Dynamic lists for FK info before we start dropping them */
	List *outgoing_fks = NIL;
	List *incoming_fks = NIL;

	initStringInfo(&sql);
	initStringInfo(&drop_cmd);
	initStringInfo(&exclusion);

	/* Get table name and schema name for DROP CONSTRAINT commands */
	relname = get_rel_name(relid);
	nspname = get_namespace_name(get_rel_namespace(relid));

	if (!relname || !nspname)
	{
		elog(DEBUG1, "pgtrashcan: Table OID %u no longer exists, skipping FK drop", relid);
		return;
	}

	/* Save caller's memory context before SPI switches it */
	caller_ctx = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in drop_foreign_keys_permanently")));

	PG_TRY();
	{
		/*
		 * STEP 1: Collect all outgoing FK information
		 * (this table has FK to another table, including self-referencing)
		 */
		appendStringInfo(&sql,
			"SELECT c.oid, c.conname "
			"FROM pg_constraint c "
			"WHERE c.conrelid = %u AND c.contype = 'f'", relid);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query outgoing foreign keys for deletion")));

		tuptable = SPI_tuptable;
		outgoing_count = SPI_processed;

		for (i = 0; i < outgoing_count; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			char *conname;
			int fno_conname;
			FKInfo *fkinfo;

			fno_conname = SPI_fnumber(tupdesc, "conname");
			if (fno_conname < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("column 'conname' not found in outgoing FK query")));

			conname = NameStr(*DatumGetName(SPI_getbinval(tuple, tupdesc, fno_conname, &isnull)));
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("conname is NULL in outgoing FK collection")));

			/* Allocate in caller's context so data survives SPI_finish */
			{
				MemoryContext spi_ctx = MemoryContextSwitchTo(caller_ctx);
				fkinfo = palloc(sizeof(FKInfo));
				fkinfo->conname = pstrdup(conname);
				fkinfo->schema = pstrdup(nspname);
				fkinfo->table = pstrdup(relname);
				outgoing_fks = lappend(outgoing_fks, fkinfo);
				MemoryContextSwitchTo(spi_ctx);
			}
		}

		/*
		 * STEP 2: Collect all incoming FK information
		 * (other tables reference this table)
		 * Exclude:
		 * - Self-referencing FKs - they're already in outgoing_fks
		 * - Tables already in pgtrashcan schema (they were handled by CASCADE)
		 * - Tables in the same DROP statement (drop_oids) - they'll be processed when we move that table
		 */

		/* Build exclusion clause for tables in the same DROP statement */
		if (drop_oids != NIL)
		{
			ListCell *lc;
			bool first = true;

			appendStringInfoString(&exclusion, " AND c.conrelid NOT IN (");
			foreach(lc, drop_oids)
			{
				if (!first)
					appendStringInfoString(&exclusion, ", ");
				appendStringInfo(&exclusion, "%u", lfirst_oid(lc));
				first = false;
			}
			appendStringInfoString(&exclusion, ")");

			elog(DEBUG1, "pgtrashcan: Excluding incoming FKs from tables in DROP list: %s", exclusion.data);
		}

		resetStringInfo(&sql);
		appendStringInfo(&sql,
			"SELECT c.oid, c.conname, n.nspname AS source_schema, cl.relname AS source_table "
			"FROM pg_constraint c "
			"JOIN pg_class cl ON c.conrelid = cl.oid "
			"JOIN pg_namespace n ON cl.relnamespace = n.oid "
			"WHERE c.confrelid = %u AND c.contype = 'f' AND c.conrelid != c.confrelid "
			"AND n.nspname != 'pgtrashcan' "
			"%s", relid, exclusion.data);  /* Add exclusion clause */

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query incoming foreign keys for deletion")));

		tuptable = SPI_tuptable;
		incoming_count = SPI_processed;

		for (i = 0; i < incoming_count; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			char *conname;
			char *source_schema;
			char *source_table;
			int fno_conname, fno_source_schema, fno_source_table;
			FKInfo *fkinfo;

			fno_conname = SPI_fnumber(tupdesc, "conname");
			fno_source_schema = SPI_fnumber(tupdesc, "source_schema");
			fno_source_table = SPI_fnumber(tupdesc, "source_table");

			if (fno_conname < 0 || fno_source_schema < 0 || fno_source_table < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("required column not found in incoming FK collection query")));

			conname = NameStr(*DatumGetName(SPI_getbinval(tuple, tupdesc, fno_conname, &isnull)));
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("conname is NULL in incoming FK collection")));

			source_schema = NameStr(*DatumGetName(SPI_getbinval(tuple, tupdesc, fno_source_schema, &isnull)));
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("source_schema is NULL in incoming FK collection")));

			source_table = NameStr(*DatumGetName(SPI_getbinval(tuple, tupdesc, fno_source_table, &isnull)));
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("source_table is NULL in incoming FK collection")));

			/* Allocate in caller's context so data survives SPI_finish */
			{
				MemoryContext spi_ctx = MemoryContextSwitchTo(caller_ctx);
				fkinfo = palloc(sizeof(FKInfo));
				fkinfo->conname = pstrdup(conname);
				fkinfo->schema = pstrdup(source_schema);
				fkinfo->table = pstrdup(source_table);
				incoming_fks = lappend(incoming_fks, fkinfo);
				MemoryContextSwitchTo(spi_ctx);
			}
		}

		/*
		 * STEP 3: Now drop all outgoing FKs
		 * At this point we've collected all info, safe to execute DDL
		 */
		{
			ListCell *lc;
			foreach(lc, outgoing_fks)
			{
				FKInfo *fkinfo = (FKInfo *) lfirst(lc);

				elog(DEBUG1, "pgtrashcan: Dropping outgoing FK constraint: %s from %s.%s",
					 fkinfo->conname, fkinfo->schema, fkinfo->table);

				resetStringInfo(&drop_cmd);
				appendStringInfo(&drop_cmd, "ALTER TABLE %s.%s DROP CONSTRAINT %s",
								 quote_identifier(fkinfo->schema),
								 quote_identifier(fkinfo->table),
								 quote_identifier(fkinfo->conname));

				ret = SPI_execute(drop_cmd.data, false, 0);
				if (ret != SPI_OK_UTILITY)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("pgtrashcan: Failed to drop outgoing FK constraint %s", fkinfo->conname),
							 errhint("Cannot move table to trash if FK constraints cannot be dropped")));

				fk_count++;
			}
		}

		/*
		 * STEP 4: Drop all incoming FKs
		 * Skip if the source table no longer exists (was moved by CASCADE)
		 */
		{
			ListCell *lc;
			foreach(lc, incoming_fks)
			{
				FKInfo *fkinfo = (FKInfo *) lfirst(lc);
				Oid source_relid;

				/*
				 * Check if the source table still exists in its original schema
				 * If it was moved to pgtrashcan by CASCADE, skip this FK (already handled)
				 */
				source_relid = get_relname_relid(fkinfo->table,
												 get_namespace_oid(fkinfo->schema, true));
				if (!OidIsValid(source_relid))
				{
					elog(DEBUG1, "pgtrashcan: Skipping incoming FK %s - source table %s.%s no longer exists (moved by CASCADE)",
						 fkinfo->conname, fkinfo->schema, fkinfo->table);
					continue;
				}

				elog(DEBUG1, "pgtrashcan: Dropping incoming FK constraint: %s from table %s.%s",
					 fkinfo->conname, fkinfo->schema, fkinfo->table);

				resetStringInfo(&drop_cmd);
				appendStringInfo(&drop_cmd, "ALTER TABLE %s.%s DROP CONSTRAINT %s",
								 quote_identifier(fkinfo->schema),
								 quote_identifier(fkinfo->table),
								 quote_identifier(fkinfo->conname));

				ret = SPI_execute(drop_cmd.data, false, 0);
				if (ret != SPI_OK_UTILITY)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("pgtrashcan: Failed to drop incoming FK constraint %s from %s.%s",
									fkinfo->conname, fkinfo->schema, fkinfo->table),
							 errhint("Cannot move table to trash if FK constraints cannot be dropped")));

				fk_count++;
			}
		}

		/* Done with SPI - list data is in caller's memory context, safe to finish */
		pfree(sql.data);
		pfree(drop_cmd.data);
		pfree(exclusion.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	if (fk_count > 0)
	{
		StringInfoData fk_names;
		ListCell *lc;
		bool first;

		initStringInfo(&fk_names);

		/* Build list of FK names */
		first = true;
		foreach(lc, outgoing_fks)
		{
			FKInfo *fkinfo = (FKInfo *) lfirst(lc);
			if (!first)
				appendStringInfoString(&fk_names, ", ");
			appendStringInfo(&fk_names, "%s", fkinfo->conname);
			first = false;
		}
		foreach(lc, incoming_fks)
		{
			FKInfo *fkinfo = (FKInfo *) lfirst(lc);
			if (!first)
				appendStringInfoString(&fk_names, ", ");
			appendStringInfo(&fk_names, "%s (from %s.%s)",
				fkinfo->conname,
				fkinfo->schema,
				fkinfo->table);
			first = false;
		}

		elog(NOTICE, "%d foreign key constraint(s) permanently dropped (cannot be moved to trash): %s",
			fk_count, fk_names.data);
		elog(NOTICE, "Foreign key definitions stored in pgtrashcan._trash_catalog.metadata");

		pfree(fk_names.data);
	}

	/* Free allocated memory for FK info lists */
	{
		ListCell *lc;
		foreach(lc, outgoing_fks)
		{
			FKInfo *fkinfo = (FKInfo *) lfirst(lc);
			pfree(fkinfo->conname);
			pfree(fkinfo->schema);
			pfree(fkinfo->table);
		}
		list_free_deep(outgoing_fks);
		foreach(lc, incoming_fks)
		{
			FKInfo *fkinfo = (FKInfo *) lfirst(lc);
			pfree(fkinfo->conname);
			pfree(fkinfo->schema);
			pfree(fkinfo->table);
		}
		list_free_deep(incoming_fks);
	}

	/* Free table/schema names */
	pfree(relname);
	pfree(nspname);
}

/*
 * update_catalog_with_complete_metadata - Update the catalog row with all metadata types
 *
 * Merges FK, trigger, view, rule, and policy metadata into a single JSON object and stores it
 * Result: {"foreign_keys_dropped": [...], "triggers_dropped": [...], "views_dropped": [...], "rules_dropped": [...], "policies_dropped": [...]}
 */
static void
update_catalog_with_complete_metadata(const char *trash_name, const char *fk_metadata_json, const char *trigger_metadata_json, const char *view_metadata_json, const char *rule_metadata_json, const char *policy_metadata_json)
{
	StringInfoData sql;
	StringInfoData merged_json;
	int ret;
	bool has_fk = (fk_metadata_json && strlen(fk_metadata_json) > 0);
	bool has_trigger = (trigger_metadata_json && strlen(trigger_metadata_json) > 0);
	bool has_view = (view_metadata_json && strlen(view_metadata_json) > 0);
	bool has_rule = (rule_metadata_json && strlen(rule_metadata_json) > 0);
	bool has_policy = (policy_metadata_json && strlen(policy_metadata_json) > 0);
	bool need_comma = false;

	if (!has_fk && !has_trigger && !has_view && !has_rule && !has_policy)
		return;

	initStringInfo(&sql);
	initStringInfo(&merged_json);

	/* Build merged JSON object */
	appendStringInfoString(&merged_json, "{");

	/* Add FK metadata */
	if (has_fk)
	{
		size_t fk_len = strlen(fk_metadata_json);
		appendBinaryStringInfo(&merged_json, fk_metadata_json + 1, fk_len - 2);  /* Skip { and } */
		need_comma = true;
	}

	/* Add trigger metadata */
	if (has_trigger)
	{
		size_t trigger_len = strlen(trigger_metadata_json);
		if (need_comma)
			appendStringInfoString(&merged_json, ", ");
		appendBinaryStringInfo(&merged_json, trigger_metadata_json + 1, trigger_len - 2);  /* Skip { and } */
		need_comma = true;
	}

	/* Add view metadata */
	if (has_view)
	{
		size_t view_len = strlen(view_metadata_json);
		if (need_comma)
			appendStringInfoString(&merged_json, ", ");
		appendBinaryStringInfo(&merged_json, view_metadata_json + 1, view_len - 2);  /* Skip { and } */
		need_comma = true;
	}

	/* Add rule metadata */
	if (has_rule)
	{
		size_t rule_len = strlen(rule_metadata_json);
		if (need_comma)
			appendStringInfoString(&merged_json, ", ");
		appendBinaryStringInfo(&merged_json, rule_metadata_json + 1, rule_len - 2);  /* Skip { and } */
		need_comma = true;
	}

	/* Add policy metadata */
	if (has_policy)
	{
		size_t policy_len = strlen(policy_metadata_json);
		if (need_comma)
			appendStringInfoString(&merged_json, ", ");
		appendBinaryStringInfo(&merged_json, policy_metadata_json + 1, policy_len - 2);  /* Skip { and } */
	}

	appendStringInfoString(&merged_json, "}");

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in update_catalog_with_complete_metadata")));

	PG_TRY();
	{
		/* Set flags inside PG_TRY so PG_FINALLY always resets them */
		pgtrashcan_in_progress = true;
		SetConfigOption("pgtrashcan.internal_operation", "true", PGC_SUSET, PGC_S_SESSION);

		appendStringInfo(&sql,
			"UPDATE \"%s\".\"%s\" SET metadata = %s::jsonb "
			"WHERE trash_name = %s",
			TRASH_SCHEMA, TRASH_CATALOG,
			quote_literal_cstr(merged_json.data),
			quote_literal_cstr(trash_name));

		ret = SPI_execute(sql.data, false, 0);
		if (ret != SPI_OK_UPDATE)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to update catalog with metadata")));

		elog(DEBUG1, "pgtrashcan: Updated catalog for %s with complete metadata (FK, triggers, views, rules, policies)", trash_name);

		pfree(sql.data);
		pfree(merged_json.data);
		CommandCounterIncrement();
	}
	PG_FINALLY();
	{
		/* Always clean up SPI and reset flags, even on error */
		SPI_finish();
		pgtrashcan_in_progress = false;
		SetConfigOption("pgtrashcan.internal_operation", "false", PGC_SUSET, PGC_S_SESSION);
	}
	PG_END_TRY();
}

/*
 * detect_and_build_trigger_metadata_json - Detect all triggers and build JSON metadata
 *
 * Queries pg_trigger for all user-defined triggers on the table and stores their definitions
 * for documentation purposes (triggers are permanently dropped, not restored)
 *
 * Returns a JSON string with trigger definitions for storage in catalog metadata column
 */
static char *
detect_and_build_trigger_metadata_json(Oid relid)
{
	StringInfoData json;
	StringInfoData sql;
	int ret;
	bool first_trigger = true;
	SPITupleTable *tuptable;
	int i;

	initStringInfo(&json);
	initStringInfo(&sql);

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in detect_and_build_trigger_metadata_json")));

	PG_TRY();
	{
		appendStringInfoString(&json, "{\"triggers_dropped\": [");

		/*
		 * Query for all user-defined triggers on this table
		 * Exclude internal triggers (tgisinternal = true)
		 */
		appendStringInfo(&sql,
			"SELECT t.tgname, "
			"  pg_get_triggerdef(t.oid) AS trigger_def, "
			"  CASE "
			"    WHEN t.tgtype & 2 = 2 THEN 'BEFORE' "
			"    WHEN t.tgtype & 64 = 64 THEN 'INSTEAD OF' "
			"    ELSE 'AFTER' "
			"  END AS timing, "
			"  CASE "
			"    WHEN t.tgtype & 4 = 4 THEN 'INSERT' "
			"    WHEN t.tgtype & 8 = 8 THEN 'DELETE' "
			"    WHEN t.tgtype & 16 = 16 THEN 'UPDATE' "
			"    ELSE 'TRUNCATE' "
			"  END AS event, "
			"  CASE "
			"    WHEN t.tgtype & 1 = 1 THEN 'ROW' "
			"    ELSE 'STATEMENT' "
			"  END AS level, "
			"  p.proname AS function_name, "
			"  n.nspname AS function_schema "
			"FROM pg_trigger t "
			"JOIN pg_proc p ON t.tgfoid = p.oid "
			"JOIN pg_namespace n ON p.pronamespace = n.oid "
			"WHERE t.tgrelid = %u "
			"  AND NOT t.tgisinternal "
			"ORDER BY t.tgname", relid);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query triggers")));

		tuptable = SPI_tuptable;
		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			Datum datum;
			char *tgname;
			char *trigger_def;
			char *timing;
			char *event;
			char *level;
			char *function_name;
			char *function_schema;
			bool free_trigger_def = false;
			bool free_timing = false;
			bool free_event = false;
			bool free_level = false;
			int fno_tgname, fno_trigger_def, fno_timing, fno_event, fno_level, fno_function_name, fno_function_schema;

			if (!first_trigger)
				appendStringInfoString(&json, ", ");
			first_trigger = false;

			/* Get column numbers */
			fno_tgname = SPI_fnumber(tupdesc, "tgname");
			fno_trigger_def = SPI_fnumber(tupdesc, "trigger_def");
			fno_timing = SPI_fnumber(tupdesc, "timing");
			fno_event = SPI_fnumber(tupdesc, "event");
			fno_level = SPI_fnumber(tupdesc, "level");
			fno_function_name = SPI_fnumber(tupdesc, "function_name");
			fno_function_schema = SPI_fnumber(tupdesc, "function_schema");

			if (fno_tgname < 0 || fno_trigger_def < 0 || fno_timing < 0 || fno_event < 0 ||
				fno_level < 0 || fno_function_name < 0 || fno_function_schema < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("required column not found in trigger query")));

			/* Extract trigger information */
			datum = SPI_getbinval(tuple, tupdesc, fno_tgname, &isnull);
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("tgname is NULL in trigger query")));
			tgname = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, fno_trigger_def, &isnull);
			if (isnull)
			{
				trigger_def = "";
			}
			else
			{
				trigger_def = TextDatumGetCString(datum);
				free_trigger_def = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_timing, &isnull);
			if (isnull)
			{
				timing = "AFTER";
			}
			else
			{
				timing = TextDatumGetCString(datum);
				free_timing = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_event, &isnull);
			if (isnull)
			{
				event = "unknown";
			}
			else
			{
				event = TextDatumGetCString(datum);
				free_event = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_level, &isnull);
			if (isnull)
			{
				level = "STATEMENT";
			}
			else
			{
				level = TextDatumGetCString(datum);
				free_level = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_function_name, &isnull);
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("function_name is NULL in trigger query")));
			function_name = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, fno_function_schema, &isnull);
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("function_schema is NULL in trigger query")));
			function_schema = NameStr(*DatumGetName(datum));

			/* Build JSON object for this trigger */
			{
				char *esc_tgname = escape_json_string(tgname);
				char *esc_timing = escape_json_string(timing);
				char *esc_event = escape_json_string(event);
				char *esc_level = escape_json_string(level);
				char *esc_func_schema = escape_json_string(function_schema);
				char *esc_func_name = escape_json_string(function_name);
				char *esc_trigger_def = escape_json_string(trigger_def);

				appendStringInfo(&json,
					"{\"trigger_name\": \"%s\", "
					"\"timing\": \"%s\", "
					"\"event\": \"%s\", "
					"\"level\": \"%s\", "
					"\"function\": \"%s.%s()\", "
					"\"definition\": \"%s\"}",
					esc_tgname,
					esc_timing,
					esc_event,
					esc_level,
					esc_func_schema,
					esc_func_name,
					esc_trigger_def);

				pfree(esc_tgname);
				pfree(esc_timing);
				pfree(esc_event);
				pfree(esc_level);
				pfree(esc_func_schema);
				pfree(esc_func_name);
				pfree(esc_trigger_def);
			}

			/* Free TextDatumGetCString allocations */
			if (free_trigger_def)
				pfree(trigger_def);
			if (free_timing)
				pfree(timing);
			if (free_event)
				pfree(event);
			if (free_level)
				pfree(level);
		}

		appendStringInfoString(&json, "]}");

		pfree(sql.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	return json.data;
}

/* drop_triggers_permanently removed — triggers now preserved with table */
#if 0
/*
 * drop_triggers_permanently - Drop all user-defined triggers on the table
 *
 * Similar to drop_foreign_keys_permanently(), this permanently removes all triggers
 * before moving the table to pgtrashcan. Trigger definitions are stored in metadata
 * for documentation, but triggers are not restored.
 */
static void
drop_triggers_permanently(Oid relid)
{
	StringInfoData sql;
	StringInfoData drop_cmd;
	int ret;
	SPITupleTable *tuptable;
	int i;
	int trigger_count = 0;
	char *relname;
	char *nspname;
	MemoryContext caller_ctx;

	/* Dynamic list for trigger info before we start dropping them */
	List *triggers = NIL;

	initStringInfo(&sql);
	initStringInfo(&drop_cmd);

	/* Get table name and schema name for DROP TRIGGER commands */
	relname = get_rel_name(relid);
	nspname = get_namespace_name(get_rel_namespace(relid));

	if (!relname || !nspname)
	{
		elog(DEBUG1, "pgtrashcan: Table OID %u no longer exists, skipping trigger drop", relid);
		return;
	}

	/* Save caller's memory context before SPI switches it */
	caller_ctx = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in drop_triggers_permanently")));

	PG_TRY();
	{
		/*
		 * STEP 1: Collect all trigger information
		 * (user-defined triggers only, exclude internal triggers)
		 */
		appendStringInfo(&sql,
			"SELECT t.tgname "
			"FROM pg_trigger t "
			"WHERE t.tgrelid = %u "
			"  AND NOT t.tgisinternal", relid);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query triggers for deletion")));

		tuptable = SPI_tuptable;
		trigger_count = SPI_processed;

		for (i = 0; i < trigger_count; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			char *tgname;
			int fno_tgname;
			TriggerInfo *tinfo;

			fno_tgname = SPI_fnumber(tupdesc, "tgname");
			if (fno_tgname < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("column 'tgname' not found in trigger deletion query")));

			tgname = NameStr(*DatumGetName(SPI_getbinval(tuple, tupdesc, fno_tgname, &isnull)));
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("tgname is NULL in trigger deletion query")));

			/* Allocate in caller's context so data survives SPI_finish */
			{
				MemoryContext spi_ctx = MemoryContextSwitchTo(caller_ctx);
				tinfo = palloc(sizeof(TriggerInfo));
				tinfo->tgname = pstrdup(tgname);
				triggers = lappend(triggers, tinfo);
				MemoryContextSwitchTo(spi_ctx);
			}
		}

		/*
		 * STEP 2: Drop all triggers using stored info
		 */
		{
			ListCell *lc;
			foreach(lc, triggers)
			{
				TriggerInfo *tinfo = (TriggerInfo *) lfirst(lc);

				elog(DEBUG1, "pgtrashcan: Dropping trigger: %s from %s.%s",
					 tinfo->tgname, nspname, relname);

				resetStringInfo(&drop_cmd);
				appendStringInfo(&drop_cmd, "DROP TRIGGER %s ON %s.%s",
								 quote_identifier(tinfo->tgname),
								 quote_identifier(nspname),
								 quote_identifier(relname));

				ret = SPI_execute(drop_cmd.data, false, 0);
				if (ret != SPI_OK_UTILITY)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("pgtrashcan: Failed to drop trigger %s", tinfo->tgname),
							 errhint("Cannot move table to trash if triggers cannot be dropped")));
			}
		}

		/* Done with SPI - list data is in caller's memory context, safe to finish */
		pfree(sql.data);
		pfree(drop_cmd.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	if (trigger_count > 0)
	{
		StringInfoData trigger_names;
		ListCell *lc;
		bool first = true;

		initStringInfo(&trigger_names);

		/* Build list of trigger names */
		foreach(lc, triggers)
		{
			TriggerInfo *tinfo = (TriggerInfo *) lfirst(lc);
			if (!first)
				appendStringInfoString(&trigger_names, ", ");
			appendStringInfo(&trigger_names, "%s", tinfo->tgname);
			first = false;
		}

		elog(NOTICE, "%d trigger(s) permanently dropped (cannot be moved to trash): %s",
			 trigger_count, trigger_names.data);
		elog(NOTICE, "Trigger definitions stored in pgtrashcan._trash_catalog.metadata");

		pfree(trigger_names.data);
	}

	/* Free allocated memory for trigger info list */
	{
		ListCell *lc;
		foreach(lc, triggers)
		{
			TriggerInfo *tinfo = (TriggerInfo *) lfirst(lc);
			pfree(tinfo->tgname);
		}
		list_free_deep(triggers);
	}

	/* Free table/schema names */
	pfree(relname);
	pfree(nspname);
}
#endif /* drop_triggers_permanently */

/*
 * collect_views_recursive - Recursively collect all views that depend on a relation
 *
 * Walks the pg_depend/pg_rewrite dependency graph to find ALL views in the chain.
 * For a table T with view_B depending on it, and view_C depending on view_B:
 *   - First recurses to find view_C (which depends on view_B)
 *   - Then records view_B
 *   - Result list is in bottom-up order (leaves first): [view_C, view_B]
 *
 * This ensures:
 *   1. All nested view definitions are captured in metadata JSON
 *   2. Views can be dropped in leaf-first order without CASCADE
 *
 * Parameters:
 *   relid           - OID of the relation to find dependent views for
 *   depends_on_name - Name of the parent view (NULL if relid is the original table)
 *   visited         - List of already-visited OIDs to prevent cycles
 *   depth           - Recursion depth (safety limit at 100)
 *
 * Returns: List of ViewInfo* in bottom-up order (must be freed by caller)
 *
 * IMPORTANT: Caller must have SPI connected before calling this function.
 * This function executes SPI queries but does NOT call SPI_connect/SPI_finish.
 */
static List *
collect_views_recursive(Oid relid, const char *depends_on_name, List **visited, int depth, MemoryContext caller_ctx)
{
	List *result = NIL;
	StringInfoData sql;
	int ret;
	SPITupleTable *tuptable;
	int view_count;
	int i;

	/* Safety limit to prevent runaway recursion */
	if (depth > 100)
	{
		elog(WARNING, "pgtrashcan: view dependency chain exceeds 100 levels, stopping recursion");
		return NIL;
	}

	/* Check if we've already visited this OID (cycle detection) */
	{
		ListCell *lc;
		foreach(lc, *visited)
		{
			if (lfirst_oid(lc) == relid)
				return NIL;  /* Already visited, skip */
		}
	}

	{
		MemoryContext spi_ctx = MemoryContextSwitchTo(caller_ctx);
		*visited = lappend_oid(*visited, relid);
		MemoryContextSwitchTo(spi_ctx);
	}

	initStringInfo(&sql);

	/*
	 * Query for views and materialized views that depend on this relation.
	 * Views depend via pg_rewrite rules, so join through pg_rewrite.
	 */
	appendStringInfo(&sql,
		"SELECT DISTINCT c.oid, n.nspname, c.relname, c.relkind "
		"FROM pg_depend d "
		"JOIN pg_rewrite rw ON rw.oid = d.objid "
		"JOIN pg_class c ON c.oid = rw.ev_class "
		"JOIN pg_namespace n ON n.oid = c.relnamespace "
		"WHERE d.refobjid = %u "
		"  AND d.deptype IN ('n', 'a') "
		"  AND c.relkind IN ('v', 'm') "
		"  AND c.oid != %u "
		"  AND c.oid >= 16384 "
		"ORDER BY c.relname", relid, relid);

	ret = SPI_execute(sql.data, true, 0);
	if (ret != SPI_OK_SELECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("failed to query views in collect_views_recursive")));

	tuptable = SPI_tuptable;
	view_count = SPI_processed;

	/*
	 * First pass: collect basic info from SPI results into a temporary list.
	 * We must copy data out before recursing, since recursive SPI queries
	 * will overwrite SPI_tuptable/SPI_processed.
	 */
	{
		typedef struct TmpViewInfo {
			Oid   oid;
			char *schema;
			char *name;
			char  relkind;
		} TmpViewInfo;

		List *tmp_views = NIL;

		for (i = 0; i < view_count; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			Datum datum;
			int fno_oid, fno_nspname, fno_relname, fno_relkind;
			TmpViewInfo *tmp;

			fno_oid = SPI_fnumber(tupdesc, "oid");
			fno_nspname = SPI_fnumber(tupdesc, "nspname");
			fno_relname = SPI_fnumber(tupdesc, "relname");
			fno_relkind = SPI_fnumber(tupdesc, "relkind");

			if (fno_oid < 0 || fno_nspname < 0 || fno_relname < 0 || fno_relkind < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("required column not found in recursive view query")));

			/* Extract values while in SPI context */
			datum = SPI_getbinval(tuple, tupdesc, fno_oid, &isnull);
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("oid is NULL in recursive view query")));

			{
				Oid tmp_oid = DatumGetObjectId(datum);
				char *tmp_schema_str;
				char *tmp_name_str;
				char tmp_relkind;

				datum = SPI_getbinval(tuple, tupdesc, fno_nspname, &isnull);
				if (isnull)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("nspname is NULL in recursive view query")));
				tmp_schema_str = NameStr(*DatumGetName(datum));

				datum = SPI_getbinval(tuple, tupdesc, fno_relname, &isnull);
				if (isnull)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("relname is NULL in recursive view query")));
				tmp_name_str = NameStr(*DatumGetName(datum));

				datum = SPI_getbinval(tuple, tupdesc, fno_relkind, &isnull);
				if (isnull)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("relkind is NULL in recursive view query")));
				tmp_relkind = DatumGetChar(datum);

				/* Skip if already visited (handles diamond dependencies) */
				{
					bool already_visited = false;
					ListCell *lc;
					foreach(lc, *visited)
					{
						if (lfirst_oid(lc) == tmp_oid)
						{
							already_visited = true;
							break;
						}
					}
					if (already_visited)
						continue;
				}

				/* Allocate in caller's context so data survives SPI_finish */
				{
					MemoryContext spi_ctx = MemoryContextSwitchTo(caller_ctx);
					tmp = palloc(sizeof(TmpViewInfo));
					tmp->oid = tmp_oid;
					tmp->schema = pstrdup(tmp_schema_str);
					tmp->name = pstrdup(tmp_name_str);
					tmp->relkind = tmp_relkind;
					tmp_views = lappend(tmp_views, tmp);
					MemoryContextSwitchTo(spi_ctx);
				}
			}
		}

		/*
		 * Second pass: for each discovered view, recurse to find its dependents
		 * before adding it to the result list. This produces bottom-up ordering.
		 */
		{
			ListCell *lc;
			foreach(lc, tmp_views)
			{
				TmpViewInfo *tmp = (TmpViewInfo *) lfirst(lc);
				List *children;
				ViewInfo *vinfo;

				/* Recurse: find views that depend on THIS view */
				children = collect_views_recursive(tmp->oid, tmp->name, visited, depth + 1, caller_ctx);

				/* Allocate in caller's context so data survives SPI_finish */
				{
					MemoryContext spi_ctx = MemoryContextSwitchTo(caller_ctx);

					/* Add children first (bottom-up order) */
					result = list_concat(result, children);

					/* Then add this view itself */
					vinfo = palloc(sizeof(ViewInfo));
					vinfo->oid = tmp->oid;
					vinfo->schema = tmp->schema;  /* transfer ownership */
					vinfo->name = tmp->name;       /* transfer ownership */
					vinfo->relkind = tmp->relkind;
					vinfo->depends_on = depends_on_name ? pstrdup(depends_on_name) : NULL;

					result = lappend(result, vinfo);

					MemoryContextSwitchTo(spi_ctx);
				}

				pfree(tmp);  /* free TmpViewInfo shell only, strings transferred */
			}
		}

		list_free(tmp_views);  /* shallow free, TmpViewInfo structs already freed */
	}

	pfree(sql.data);

	return result;
}

/*
 * detect_and_build_view_metadata_json - Detect all views/matviews and build JSON metadata
 *
 * Recursively finds ALL views in the dependency chain (not just first-level).
 * For example: table_A → view_B → view_C will capture both view_B and view_C.
 *
 * Each view entry includes a "depends_on" field showing what it directly depended on:
 *   - null for views directly on the table
 *   - parent view name for nested views
 *
 * Views are listed in bottom-up order (leaves first): [view_C, view_B]
 *
 * Returns a JSON string with view definitions for storage in catalog metadata column
 */
static char *
detect_and_build_view_metadata_json(Oid relid)
{
	StringInfoData json;
	List *views;
	List *visited = NIL;
	ListCell *lc;
	bool first_view = true;
	MemoryContext caller_ctx;

	initStringInfo(&json);

	/* Save caller's memory context before SPI switches it */
	caller_ctx = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in detect_and_build_view_metadata_json")));

	PG_TRY();
	{
		/* Recursively collect all views in the dependency chain */
		views = collect_views_recursive(relid, NULL, &visited, 0, caller_ctx);

		appendStringInfoString(&json, "{\"views_dropped\": [");

		/*
		 * For each view, get its definition via pg_get_viewdef() and build JSON.
		 * The list is in bottom-up order (leaves first).
		 */
		foreach(lc, views)
		{
			ViewInfo *vinfo = (ViewInfo *) lfirst(lc);
			char *view_type;
			char *view_def;
			char *escaped_name, *escaped_schema, *escaped_type, *escaped_def;
			StringInfoData view_def_query;

			if (!first_view)
				appendStringInfoString(&json, ", ");
			first_view = false;

			view_type = (vinfo->relkind == 'v') ? "view" : "materialized view";

			/* Get view definition using pg_get_viewdef() */
			initStringInfo(&view_def_query);
			appendStringInfo(&view_def_query,
				"SELECT pg_get_viewdef(%u, true)", vinfo->oid);

			if (SPI_execute(view_def_query.data, true, 1) == SPI_OK_SELECT && SPI_processed > 0)
			{
				bool isnull;
				int fno_def;
				Datum def_datum;
				TupleDesc def_tupdesc = SPI_tuptable->tupdesc;
				fno_def = SPI_fnumber(def_tupdesc, "pg_get_viewdef");
				if (fno_def < 0)
					fno_def = 1;  /* Fallback to first column if name not found */
				def_datum = SPI_getbinval(SPI_tuptable->vals[0], def_tupdesc, fno_def, &isnull);
				view_def = isnull ? NULL : TextDatumGetCString(def_datum);
			}
			else
			{
				view_def = NULL;
			}

			/* Strip trailing ';' and whitespace from pg_get_viewdef output so the
			 * constructed DDL doesn't end in ";;" */
			if (view_def)
			{
				int def_len = strlen(view_def);
				while (def_len > 0 &&
					(view_def[def_len - 1] == ';' ||
					 view_def[def_len - 1] == ' ' ||
					 view_def[def_len - 1] == '\t' ||
					 view_def[def_len - 1] == '\n' ||
					 view_def[def_len - 1] == '\r'))
					view_def[--def_len] = '\0';
			}

			/* Build the runnable DDL */
			{
				StringInfoData ddl;
				const char *body = view_def ? view_def : "/* definition unavailable */";

				initStringInfo(&ddl);
				if (vinfo->relkind == 'v')
					appendStringInfo(&ddl,
						"CREATE OR REPLACE VIEW %s.%s AS %s;",
						quote_identifier(vinfo->schema),
						quote_identifier(vinfo->name),
						body);
				else
					/* materialized view — no OR REPLACE variant exists; IF NOT EXISTS
					 * is the closest runnable replay option. WITH DATA triggers a
					 * refresh so the matview is re-populated from the restored base. */
					appendStringInfo(&ddl,
						"CREATE MATERIALIZED VIEW IF NOT EXISTS %s.%s AS %s WITH DATA;",
						quote_identifier(vinfo->schema),
						quote_identifier(vinfo->name),
						body);

				/* Escape strings for JSON */
				escaped_name = escape_json_string(vinfo->name);
				escaped_schema = escape_json_string(vinfo->schema);
				escaped_type = escape_json_string(view_type);
				escaped_def = escape_json_string(body);
				{
					char *escaped_ddl = escape_json_string(ddl.data);

					/* Build JSON object for this view */
					appendStringInfo(&json,
						"{\"view_name\": \"%s\", "
						"\"view_schema\": \"%s\", "
						"\"view_type\": \"%s\", "
						"\"definition\": \"%s\", "
						"\"ddl\": \"%s\"",
						escaped_name,
						escaped_schema,
						escaped_type,
						escaped_def,
						escaped_ddl);

					pfree(escaped_ddl);
				}
				pfree(ddl.data);
			}

			/* Add depends_on field for nested views */
			if (vinfo->depends_on)
			{
				char *escaped_depends = escape_json_string(vinfo->depends_on);
				appendStringInfo(&json, ", \"depends_on\": \"%s\"", escaped_depends);
				pfree(escaped_depends);
			}

			appendStringInfoChar(&json, '}');

			/* Free escaped strings */
			pfree(escaped_name);
			pfree(escaped_schema);
			pfree(escaped_type);
			pfree(escaped_def);
			if (view_def)
				pfree(view_def);
			pfree(view_def_query.data);
		}

		appendStringInfoString(&json, "]}");
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	/* Free the collected view info list (in caller's context, safe after SPI_finish) */
	foreach(lc, views)
	{
		ViewInfo *vinfo = (ViewInfo *) lfirst(lc);
		pfree(vinfo->schema);
		pfree(vinfo->name);
		if (vinfo->depends_on)
			pfree(vinfo->depends_on);
	}
	list_free_deep(views);
	list_free(visited);

	return json.data;
}

/*
 * drop_views_permanently - Drop all views/matviews that depend on the table
 *
 * Recursively finds ALL views in the dependency chain (including nested views)
 * and drops them in bottom-up order (leaf views first, so no CASCADE needed).
 *
 * View definitions are stored in metadata for documentation, but views are not restored.
 */
static void
drop_views_permanently(Oid relid)
{
	StringInfoData drop_cmd;
	int ret;
	int view_count;
	MemoryContext caller_ctx;

	/* Recursively collect all views in the dependency chain */
	List *views = NIL;
	List *visited = NIL;
	ListCell *lc;

	initStringInfo(&drop_cmd);

	/* Save caller's memory context before SPI switches it */
	caller_ctx = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in drop_views_permanently")));

	PG_TRY();
	{
		/*
		 * STEP 1: Recursively collect all views (including nested chains).
		 * Result is in bottom-up order: leaves first, direct dependents last.
		 * This means we can drop in list order without needing CASCADE.
		 */
		views = collect_views_recursive(relid, NULL, &visited, 0, caller_ctx);
		view_count = list_length(views);

		/*
		 * STEP 2: Drop all views in bottom-up order (leaves first)
		 */
		foreach(lc, views)
		{
			ViewInfo *vinfo = (ViewInfo *) lfirst(lc);
			char *obj_type = (vinfo->relkind == 'v') ? "VIEW" : "MATERIALIZED VIEW";

			elog(DEBUG1, "pgtrashcan: Dropping %s: %s.%s%s",
				 obj_type, vinfo->schema, vinfo->name,
				 vinfo->depends_on ? vinfo->depends_on : "(direct)");

			resetStringInfo(&drop_cmd);
			appendStringInfo(&drop_cmd, "DROP %s IF EXISTS %s.%s",
							 obj_type,
							 quote_identifier(vinfo->schema),
							 quote_identifier(vinfo->name));

			ret = SPI_execute(drop_cmd.data, false, 0);
			if (ret != SPI_OK_UTILITY)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("pgtrashcan: Failed to drop %s %s.%s",
								obj_type, vinfo->schema, vinfo->name),
						 errhint("Cannot move table to trash if views cannot be dropped")));
		}

		/* Done with SPI - list data is in caller's memory context, safe to finish */
		pfree(drop_cmd.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	if (view_count > 0)
	{
		StringInfoData view_names;
		bool first = true;

		initStringInfo(&view_names);

		/* Build list of view names */
		foreach(lc, views)
		{
			ViewInfo *vinfo = (ViewInfo *) lfirst(lc);
			if (!first)
				appendStringInfoString(&view_names, ", ");
			appendStringInfo(&view_names, "%s.%s (%s)",
				vinfo->schema,
				vinfo->name,
				vinfo->relkind == 'v' ? "view" : "materialized view");
			first = false;
		}

		elog(NOTICE, "%d view(s)/materialized view(s) permanently dropped (cannot be moved to trash): %s",
			 view_count, view_names.data);
		elog(NOTICE, "View definitions stored in pgtrashcan._trash_catalog.metadata");

		pfree(view_names.data);
	}

	/* Free allocated memory for view info list */
	foreach(lc, views)
	{
		ViewInfo *vinfo = (ViewInfo *) lfirst(lc);
		pfree(vinfo->schema);
		pfree(vinfo->name);
		if (vinfo->depends_on)
			pfree(vinfo->depends_on);
	}
	list_free_deep(views);
	list_free(visited);
}

/*
 * detect_and_build_rule_metadata_json - Detect all rules and build JSON metadata
 *
 * Queries pg_rewrite for all user-defined rules on the table and stores their definitions
 * for documentation purposes (rules are permanently dropped, not restored)
 *
 * Returns a JSON string with rule definitions for storage in catalog metadata column
 */
static char *
detect_and_build_rule_metadata_json(Oid relid)
{
	StringInfoData json;
	StringInfoData sql;
	int ret;
	bool first_rule = true;
	SPITupleTable *tuptable;
	int i;

	initStringInfo(&json);
	initStringInfo(&sql);

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in detect_and_build_rule_metadata_json")));

	PG_TRY();
	{
		appendStringInfoString(&json, "{\"rules_dropped\": [");

		/*
		 * Query for all user-defined rules on this table
		 * Exclude internal rules (ev_type = '1' for SELECT rules that are actually views)
		 */
		appendStringInfo(&sql,
			"SELECT r.rulename, "
			"  pg_get_ruledef(r.oid) AS rule_def, "
			"  CASE r.ev_type "
			"    WHEN '1' THEN 'SELECT' "
			"    WHEN '2' THEN 'UPDATE' "
			"    WHEN '3' THEN 'INSERT' "
			"    WHEN '4' THEN 'DELETE' "
			"    ELSE 'UNKNOWN' "
			"  END AS event "
			"FROM pg_rewrite r "
			"WHERE r.ev_class = %u "
			"  AND r.rulename != '_RETURN' "
			"ORDER BY r.rulename", relid);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query rules")));

		tuptable = SPI_tuptable;
		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			Datum datum;
			char *rulename;
			char *rule_def;
			char *event;
			bool free_rule_def = false;
			bool free_event = false;
			int fno_rulename, fno_rule_def, fno_event;

			if (!first_rule)
				appendStringInfoString(&json, ", ");
			first_rule = false;

			/* Get column numbers */
			fno_rulename = SPI_fnumber(tupdesc, "rulename");
			fno_rule_def = SPI_fnumber(tupdesc, "rule_def");
			fno_event = SPI_fnumber(tupdesc, "event");

			if (fno_rulename < 0 || fno_rule_def < 0 || fno_event < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("required column not found in rule query")));

			/* Extract rule information */
			datum = SPI_getbinval(tuple, tupdesc, fno_rulename, &isnull);
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("rulename is NULL in rule query")));
			rulename = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, fno_rule_def, &isnull);
			if (isnull)
			{
				rule_def = "";
			}
			else
			{
				rule_def = TextDatumGetCString(datum);
				free_rule_def = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_event, &isnull);
			if (isnull)
			{
				event = "UNKNOWN";
			}
			else
			{
				event = TextDatumGetCString(datum);
				free_event = true;
			}

			/* Build JSON object for this rule */
			{
				char *esc_rulename = escape_json_string(rulename);
				char *esc_event = escape_json_string(event);
				char *esc_rule_def = escape_json_string(rule_def);

				appendStringInfo(&json,
					"{\"rule_name\": \"%s\", "
					"\"event\": \"%s\", "
					"\"definition\": \"%s\"}",
					esc_rulename,
					esc_event,
					esc_rule_def);

				pfree(esc_rulename);
				pfree(esc_event);
				pfree(esc_rule_def);
			}

			/* Free TextDatumGetCString allocations */
			if (free_rule_def)
				pfree(rule_def);
			if (free_event)
				pfree(event);
		}

		appendStringInfoString(&json, "]}");

		pfree(sql.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	return json.data;
}

/*
 * drop_rules_permanently - Drop all user-defined rules on the table
 *
 * Similar to drop_triggers_permanently(), this permanently removes all rules
 * before moving the table to pgtrashcan. Rule definitions are stored in metadata
 * for documentation, but rules are not restored.
 */
static void
drop_rules_permanently(Oid relid)
{
	StringInfoData sql;
	StringInfoData drop_cmd;
	int ret;
	SPITupleTable *tuptable;
	int i;
	int rule_count = 0;
	char *relname;
	char *nspname;
	MemoryContext caller_ctx;

	/* Dynamic list for rule info before we start dropping them */
	List *rules = NIL;

	initStringInfo(&sql);
	initStringInfo(&drop_cmd);

	/* Get table name and schema name for DROP RULE commands */
	relname = get_rel_name(relid);
	nspname = get_namespace_name(get_rel_namespace(relid));

	if (!relname || !nspname)
	{
		elog(DEBUG1, "pgtrashcan: Table OID %u no longer exists, skipping rule drop", relid);
		return;
	}

	/* Save caller's memory context before SPI switches it */
	caller_ctx = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in drop_rules_permanently")));

	PG_TRY();
	{
		/*
		 * STEP 1: Collect all rule information
		 * (user-defined rules only, exclude _RETURN which is for views)
		 */
		appendStringInfo(&sql,
			"SELECT r.rulename "
			"FROM pg_rewrite r "
			"WHERE r.ev_class = %u "
			"  AND r.rulename != '_RETURN'", relid);

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query rules for deletion")));

		tuptable = SPI_tuptable;
		rule_count = SPI_processed;

		for (i = 0; i < rule_count; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			char *rulename;
			int fno_rulename;
			RuleInfo *rinfo;

			fno_rulename = SPI_fnumber(tupdesc, "rulename");
			if (fno_rulename < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("column 'rulename' not found in rule deletion query")));

			rulename = NameStr(*DatumGetName(SPI_getbinval(tuple, tupdesc, fno_rulename, &isnull)));
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("rulename is NULL in rule deletion query")));

			/* Allocate in caller's context so data survives SPI_finish */
			{
				MemoryContext spi_ctx = MemoryContextSwitchTo(caller_ctx);
				rinfo = palloc(sizeof(RuleInfo));
				rinfo->rulename = pstrdup(rulename);
				rules = lappend(rules, rinfo);
				MemoryContextSwitchTo(spi_ctx);
			}
		}

		/*
		 * STEP 2: Drop all rules using stored info
		 */
		{
			ListCell *lc;
			foreach(lc, rules)
			{
				RuleInfo *rinfo = (RuleInfo *) lfirst(lc);

				elog(DEBUG1, "pgtrashcan: Dropping rule: %s from %s.%s",
					 rinfo->rulename, nspname, relname);

				resetStringInfo(&drop_cmd);
				appendStringInfo(&drop_cmd, "DROP RULE %s ON %s.%s",
								 quote_identifier(rinfo->rulename),
								 quote_identifier(nspname),
								 quote_identifier(relname));

				ret = SPI_execute(drop_cmd.data, false, 0);
				if (ret != SPI_OK_UTILITY)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("pgtrashcan: Failed to drop rule %s", rinfo->rulename),
							 errhint("Cannot move table to trash if rules cannot be dropped")));
			}
		}

		/* Done with SPI - list data is in caller's memory context, safe to finish */
		pfree(sql.data);
		pfree(drop_cmd.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	if (rule_count > 0)
	{
		StringInfoData rule_names;
		ListCell *lc;
		bool first = true;

		initStringInfo(&rule_names);

		/* Build list of rule names */
		foreach(lc, rules)
		{
			RuleInfo *rinfo = (RuleInfo *) lfirst(lc);
			if (!first)
				appendStringInfoString(&rule_names, ", ");
			appendStringInfo(&rule_names, "%s", rinfo->rulename);
			first = false;
		}

		elog(NOTICE, "%d rule(s) permanently dropped (cannot be moved to trash): %s",
			 rule_count, rule_names.data);
		elog(NOTICE, "Rule definitions stored in pgtrashcan._trash_catalog.metadata");

		pfree(rule_names.data);
	}

	/* Free allocated memory for rule info list */
	{
		ListCell *lc;
		foreach(lc, rules)
		{
			RuleInfo *rinfo = (RuleInfo *) lfirst(lc);
			pfree(rinfo->rulename);
		}
		list_free_deep(rules);
	}

	/* Free table/schema names */
	pfree(relname);
	pfree(nspname);
}

/*
 * detect_and_build_policy_metadata_json - Detect all RLS policies and build JSON metadata
 *
 * Queries pg_policy for all policies on the table and stores their definitions
 * for documentation purposes (policies are permanently dropped, not restored)
 *
 * Returns a JSON string with policy definitions for storage in catalog metadata column
 */
static char *
detect_and_build_policy_metadata_json(Oid relid)
{
	StringInfoData json;
	StringInfoData sql;
	int ret;
	bool first_policy = true;
	SPITupleTable *tuptable;
	int i;

	initStringInfo(&json);
	initStringInfo(&sql);

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in detect_and_build_policy_metadata_json")));

	PG_TRY();
	{
		appendStringInfoString(&json, "{\"policies_dropped\": [");

		/*
		 * Query for all policies on this table
		 * Note: pg_policy available in PostgreSQL 9.5+
		 */
		appendStringInfo(&sql,
			"SELECT polname, "
			"  CASE polcmd "
			"    WHEN 'r' THEN 'SELECT' "
			"    WHEN 'a' THEN 'INSERT' "
			"    WHEN 'w' THEN 'UPDATE' "
			"    WHEN 'd' THEN 'DELETE' "
			"    WHEN '*' THEN 'ALL' "
			"    ELSE 'UNKNOWN' "
			"  END AS command, "
			"  CASE "
			"    WHEN polpermissive THEN 'PERMISSIVE' "
			"    ELSE 'RESTRICTIVE' "
			"  END AS permissive, "
			"  pg_get_expr(polqual, polrelid) AS using_expr, "
			"  pg_get_expr(polwithcheck, polrelid) AS check_expr, "
			"  (SELECT array_to_string(ARRAY(SELECT rolname FROM pg_roles WHERE oid = ANY(polroles)), ', ')) AS roles "
			"FROM pg_policy "
			"WHERE polrelid = %u "
			"ORDER BY polname", relid);

		ret = SPI_execute(sql.data, true, 0);

		/* pg_policy might not exist in older PostgreSQL versions - just close JSON */
		if (ret == SPI_OK_SELECT)
		{
		tuptable = SPI_tuptable;
		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			Datum datum;
			char *polname;
			char *command;
			char *permissive;
			char *using_expr;
			char *check_expr;
			char *roles;
			bool free_command = false;
			bool free_permissive = false;
			bool free_using_expr = false;
			bool free_check_expr = false;
			bool free_roles = false;
			int fno_polname, fno_command, fno_permissive, fno_using_expr, fno_check_expr, fno_roles;

			if (!first_policy)
				appendStringInfoString(&json, ", ");
			first_policy = false;

			/* Get column numbers */
			fno_polname = SPI_fnumber(tupdesc, "polname");
			fno_command = SPI_fnumber(tupdesc, "command");
			fno_permissive = SPI_fnumber(tupdesc, "permissive");
			fno_using_expr = SPI_fnumber(tupdesc, "using_expr");
			fno_check_expr = SPI_fnumber(tupdesc, "check_expr");
			fno_roles = SPI_fnumber(tupdesc, "roles");

			if (fno_polname < 0 || fno_command < 0 || fno_permissive < 0 ||
				fno_using_expr < 0 || fno_check_expr < 0 || fno_roles < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("required column not found in policy query")));

			/* Extract policy information */
			datum = SPI_getbinval(tuple, tupdesc, fno_polname, &isnull);
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("polname is NULL in policy query")));
			polname = NameStr(*DatumGetName(datum));

			datum = SPI_getbinval(tuple, tupdesc, fno_command, &isnull);
			if (isnull)
			{
				command = "ALL";
			}
			else
			{
				command = TextDatumGetCString(datum);
				free_command = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_permissive, &isnull);
			if (isnull)
			{
				permissive = "PERMISSIVE";
			}
			else
			{
				permissive = TextDatumGetCString(datum);
				free_permissive = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_using_expr, &isnull);
			if (isnull)
			{
				using_expr = "";
			}
			else
			{
				using_expr = TextDatumGetCString(datum);
				free_using_expr = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_check_expr, &isnull);
			if (isnull)
			{
				check_expr = "";
			}
			else
			{
				check_expr = TextDatumGetCString(datum);
				free_check_expr = true;
			}

			datum = SPI_getbinval(tuple, tupdesc, fno_roles, &isnull);
			if (isnull)
			{
				roles = "public";
			}
			else
			{
				roles = TextDatumGetCString(datum);
				free_roles = true;
			}

			/* Build JSON object for this policy */
			{
				char *esc_polname = escape_json_string(polname);
				char *esc_command = escape_json_string(command);
				char *esc_permissive = escape_json_string(permissive);
				char *esc_roles = escape_json_string(roles);
				char *esc_using_expr = escape_json_string(using_expr);
				char *esc_check_expr = escape_json_string(check_expr);

				appendStringInfo(&json,
					"{\"policy_name\": \"%s\", "
					"\"command\": \"%s\", "
					"\"permissive\": \"%s\", "
					"\"roles\": \"%s\", "
					"\"using\": \"%s\", "
					"\"check\": \"%s\"}",
					esc_polname,
					esc_command,
					esc_permissive,
					esc_roles,
					esc_using_expr,
					esc_check_expr);

				pfree(esc_polname);
				pfree(esc_command);
				pfree(esc_permissive);
				pfree(esc_roles);
				pfree(esc_using_expr);
				pfree(esc_check_expr);
			}

			/* Free TextDatumGetCString allocations */
			if (free_command)
				pfree(command);
			if (free_permissive)
				pfree(permissive);
			if (free_using_expr)
				pfree(using_expr);
			if (free_check_expr)
				pfree(check_expr);
			if (free_roles)
				pfree(roles);
		}
		}  /* end if (ret == SPI_OK_SELECT) */

		appendStringInfoString(&json, "]}");

		pfree(sql.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	return json.data;
}

/*
 * drop_policies_permanently - Drop all RLS policies on the table
 *
 * Similar to drop_triggers_permanently(), this permanently removes all policies
 * before moving the table to pgtrashcan. Policy definitions are stored in metadata
 * for documentation, but policies are not restored.
 */
static void
drop_policies_permanently(Oid relid)
{
	StringInfoData sql;
	StringInfoData drop_cmd;
	int ret;
	SPITupleTable *tuptable;
	int i;
	int policy_count = 0;
	char *relname;
	char *nspname;
	MemoryContext caller_ctx;

	/* Dynamic list for policy info before we start dropping them */
	List *policies = NIL;

	initStringInfo(&sql);
	initStringInfo(&drop_cmd);

	/* Get table name and schema name for DROP POLICY commands */
	relname = get_rel_name(relid);
	nspname = get_namespace_name(get_rel_namespace(relid));

	if (!relname || !nspname)
	{
		elog(DEBUG1, "pgtrashcan: Table OID %u no longer exists, skipping policy drop", relid);
		return;
	}

	/* Save caller's memory context before SPI switches it */
	caller_ctx = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in drop_policies_permanently")));

	PG_TRY();
	{
		/*
		 * STEP 1: Collect all policy information
		 */
		appendStringInfo(&sql,
			"SELECT polname "
			"FROM pg_policy "
			"WHERE polrelid = %u", relid);

		ret = SPI_execute(sql.data, true, 0);

		/* pg_policy might not exist in older PostgreSQL versions - just skip */
		if (ret == SPI_OK_SELECT)
		{
		tuptable = SPI_tuptable;
		policy_count = SPI_processed;

		for (i = 0; i < policy_count; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			char *polname;
			int fno_polname;
			PolicyInfo *pinfo;

			fno_polname = SPI_fnumber(tupdesc, "polname");
			if (fno_polname < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("column 'polname' not found in policy deletion query")));

			polname = NameStr(*DatumGetName(SPI_getbinval(tuple, tupdesc, fno_polname, &isnull)));
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("polname is NULL in policy deletion query")));

			/* Allocate in caller's context so data survives SPI_finish */
			{
				MemoryContext spi_ctx = MemoryContextSwitchTo(caller_ctx);
				pinfo = palloc(sizeof(PolicyInfo));
				pinfo->polname = pstrdup(polname);
				policies = lappend(policies, pinfo);
				MemoryContextSwitchTo(spi_ctx);
			}
		}

		/*
		 * STEP 2: Drop all policies using stored info
		 */
		{
			ListCell *lc;
			foreach(lc, policies)
			{
				PolicyInfo *pinfo = (PolicyInfo *) lfirst(lc);

				elog(DEBUG1, "pgtrashcan: Dropping policy: %s from %s.%s",
					 pinfo->polname, nspname, relname);

				resetStringInfo(&drop_cmd);
				appendStringInfo(&drop_cmd, "DROP POLICY %s ON %s.%s",
								 quote_identifier(pinfo->polname),
								 quote_identifier(nspname),
								 quote_identifier(relname));

				ret = SPI_execute(drop_cmd.data, false, 0);
				if (ret != SPI_OK_UTILITY)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("pgtrashcan: Failed to drop policy %s", pinfo->polname),
							 errhint("Cannot move table to trash if policies cannot be dropped")));
			}
		}
		}  /* end if (ret == SPI_OK_SELECT) */

		/* Done with SPI - list data is in caller's memory context, safe to finish */
		pfree(sql.data);
		pfree(drop_cmd.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	if (policy_count > 0)
	{
		StringInfoData policy_names;
		ListCell *lc;
		bool first = true;

		initStringInfo(&policy_names);

		/* Build list of policy names */
		foreach(lc, policies)
		{
			PolicyInfo *pinfo = (PolicyInfo *) lfirst(lc);
			if (!first)
				appendStringInfoString(&policy_names, ", ");
			appendStringInfo(&policy_names, "%s", pinfo->polname);
			first = false;
		}

		elog(NOTICE, "%d RLS polic%s permanently dropped (cannot be moved to trash): %s",
			 policy_count, policy_count == 1 ? "y" : "ies", policy_names.data);
		elog(NOTICE, "RLS policy definitions stored in pgtrashcan._trash_catalog.metadata");

		pfree(policy_names.data);
	}

	/* Free allocated memory for policy info list */
	{
		ListCell *lc;
		foreach(lc, policies)
		{
			PolicyInfo *pinfo = (PolicyInfo *) lfirst(lc);
			pfree(pinfo->polname);
		}
		list_free_deep(policies);
	}

	/* Free table/schema names */
	pfree(relname);
	pfree(nspname);
}

/*
 * is_sequence_owned - Check if a sequence is owned by a table column (SERIAL)
 *
 * Owned sequences cannot be moved independently - they move with their owner table.
 * Returns true if the sequence is owned by a column.
 */
static bool
is_sequence_owned(Oid seq_oid)
{
	StringInfoData sql;
	int ret;
	bool is_owned = false;

	initStringInfo(&sql);

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in is_sequence_owned")));

	PG_TRY();
	{
		/*
		 * Check if this sequence has an 'a' (auto) or 'i' (internal) dependency
		 * on a table column (refobjsubid > 0 means it's a column dependency)
		 */
		appendStringInfo(&sql,
			"SELECT 1 FROM pg_depend "
			"WHERE objid = %u "
			"  AND deptype IN ('a', 'i') "
			"  AND refobjsubid > 0 "
			"LIMIT 1", seq_oid);

		ret = SPI_execute(sql.data, true, 1);
		if (ret == SPI_OK_SELECT && SPI_processed > 0)
			is_owned = true;

		pfree(sql.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	return is_owned;
}

/*
 * move_single_object_to_trash - Move a single object (table or sequence) to pgtrashcan
 *
 * This is a helper function for CASCADE support.
 * It handles moving one object and its indexes to the pgtrashcan schema.
 * Note: Views and matviews are NOT moved to trash - they are dropped permanently.
 * Note: Owned sequences (SERIAL columns) are skipped - they move with their owner table.
 */
static void
move_single_object_to_trash(Oid relid, const char *orig_schema, const char *orig_name, char relkind, Oid parent_oid)
{
	AlterObjectSchemaStmt *alter_stmt;
	PlannedStmt *wrapper;
	RangeVar *rv;
	char *trash_name;
	TrashObjectType obj_type;
	List *indexoidlist = NIL;
	ListCell *lc;
	Relation rel;

	/* Determine object type */
	switch (relkind)
	{
		case 'r':  /* ordinary table */
			obj_type = relkind_to_trash_type(relkind);
			break;
		case 'S':  /* sequence */
			/* Skip owned sequences - they move automatically with their owner table */
			if (is_sequence_owned(relid))
			{
				elog(DEBUG1, "pgtrashcan: Skipping owned sequence %s.%s (moves with owner table)",
					 orig_schema, orig_name);
				return;
			}
			obj_type = relkind_to_trash_type(relkind);
			break;
		default:
			ereport(WARNING, (errmsg("pgtrashcan: Unsupported relkind '%c' for CASCADE (only tables and sequences are moved to trash)", relkind)));
			return;
	}

	elog(DEBUG1, "pgtrashcan CASCADE: Moving %s, orig_schema='%s', orig_name='%s', relid=%u",
		 trash_obj_type_str(obj_type), orig_schema ? orig_schema : "NULL", orig_name ? orig_name : "NULL", relid);

	/* Generate trash name */
	trash_name = compose_trash_name(orig_name, relid);

	/* Insert catalog entry for this object */
	/* For CASCADE'd tables, parent_oid is the CASCADE parent (cascade_parent_oid) */
	/* For indexes/sequences, we'll use dependent_on_oid later */
	insert_catalog_entry(relid, orig_schema, orig_name, obj_type, InvalidOid, parent_oid, trash_name);

	/* For tables, handle indexes, FKs, triggers, views, rules, policies */
	if (relkind == 'r')
	{
		char *fk_metadata;
		char *trigger_metadata;
		char *view_metadata;
		char *rule_metadata;
		char *policy_metadata;

		/* Get list of indexes before moving */
		rel = RelationIdGetRelation(relid);
		if (RelationIsValid(rel))
		{
			indexoidlist = RelationGetIndexList(rel);
			RelationClose(rel);
		}

		/* Insert catalog entries for indexes */
		foreach(lc, indexoidlist)
		{
			Oid indexoid = lfirst_oid(lc);
			char *indexname = get_rel_name(indexoid);
			char *index_trash_name;

			if (indexname)
			{
				index_trash_name = compose_trash_name(indexname, indexoid);
				/* Indexes are true dependents - use dependent_on_oid */
				insert_catalog_entry(indexoid, orig_schema, indexname,
									 TRASH_OBJ_INDEX, relid, InvalidOid, index_trash_name);
				pfree(index_trash_name);
				pfree(indexname);
			}
		}

		/* Insert catalog entries for owned sequences */
		{
			StringInfoData seq_sql;
			int seq_ret;
			SPITupleTable *seq_tuptable;
			int seq_count;
			int seq_i;

			if (SPI_connect() != SPI_OK_CONNECT)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SPI_connect failed in CASCADE sequence tracking")));

			initStringInfo(&seq_sql);

			PG_TRY();
			{
				/* Query for sequences owned by this table */
				appendStringInfo(&seq_sql,
					"SELECT d.objid, c.relname "
					"FROM pg_depend d "
					"JOIN pg_class c ON c.oid = d.objid "
					"WHERE d.refobjid = %u "
					"  AND d.deptype IN ('a', 'i') "
					"  AND c.relkind = 'S'",
					relid);

				seq_ret = SPI_execute(seq_sql.data, true, 0);
				if (seq_ret != SPI_OK_SELECT)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("failed to query owned sequences in CASCADE")));

				seq_tuptable = SPI_tuptable;
				seq_count = SPI_processed;

				elog(DEBUG1, "pgtrashcan CASCADE: Found %d owned sequence(s) to track", seq_count);

				/* Insert catalog entries for each sequence */
				for (seq_i = 0; seq_i < seq_count; seq_i++)
				{
					HeapTuple seq_tuple = seq_tuptable->vals[seq_i];
					TupleDesc seq_tupdesc = seq_tuptable->tupdesc;
					bool isnull;
					Oid seq_oid;
					char *seq_name;
					char *seq_trash_name;
					int fno_objid, fno_relname;

					fno_objid = SPI_fnumber(seq_tupdesc, "objid");
					fno_relname = SPI_fnumber(seq_tupdesc, "relname");

					if (fno_objid < 0 || fno_relname < 0)
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("required column not found in sequence query")));

					seq_oid = DatumGetObjectId(SPI_getbinval(seq_tuple, seq_tupdesc, fno_objid, &isnull));
					if (isnull)
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("objid is NULL in sequence query")));

					{
						Datum seq_name_datum = SPI_getbinval(seq_tuple, seq_tupdesc, fno_relname, &isnull);
						if (isnull)
							ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("relname is NULL in sequence query")));
						seq_name = pstrdup(NameStr(*DatumGetName(seq_name_datum)));
					}

					/* Generate trash name for sequence */
					seq_trash_name = compose_trash_name(seq_name, seq_oid);

					/* Insert catalog entry for sequence (it's a true dependent) */
					insert_catalog_entry(seq_oid, orig_schema, seq_name,
										 TRASH_OBJ_SEQUENCE, relid, InvalidOid, seq_trash_name);

					pfree(seq_trash_name);
					pfree(seq_name);
				}

				pfree(seq_sql.data);
			}
			PG_FINALLY();
			{
				SPI_finish();
			}
			PG_END_TRY();
		}


		/* Handle foreign keys */
		fk_metadata = detect_and_build_fk_metadata_json(relid, orig_schema, orig_name);
		drop_foreign_keys_permanently(relid, NIL);  /* CASCADE: processing single table at a time */

		/* Handle triggers — detect and record in metadata, but keep them attached */
		trigger_metadata = detect_and_build_trigger_metadata_json(relid);

		/* Handle views */
		view_metadata = detect_and_build_view_metadata_json(relid);
		drop_views_permanently(relid);

		/* Handle rules — permanently drop (rule actions reference external tables
		 * by name and break restored table if those tables no longer exist) */
		rule_metadata = detect_and_build_rule_metadata_json(relid);
		drop_rules_permanently(relid);

		/* Handle policies — permanently drop (silent deny-all on restore is dangerous) */
		policy_metadata = detect_and_build_policy_metadata_json(relid);
		drop_policies_permanently(relid);

		/* Update catalog with complete metadata */
		update_catalog_with_complete_metadata(trash_name, fk_metadata, trigger_metadata, view_metadata, rule_metadata, policy_metadata);

		/* Free metadata strings */
		if (fk_metadata) pfree(fk_metadata);
		if (trigger_metadata) pfree(trigger_metadata);
		if (view_metadata) pfree(view_metadata);
		if (rule_metadata) pfree(rule_metadata);
		if (policy_metadata) pfree(policy_metadata);

		list_free(indexoidlist);
	}

	/* Create ALTER SCHEMA statement */
	rv = makeNode(RangeVar);
	rv->catalogname = NULL;
	rv->schemaname = pstrdup(orig_schema);
	rv->relname = pstrdup(orig_name);
	rv->relpersistence = RELPERSISTENCE_PERMANENT;
	rv->location = -1;

	alter_stmt = makeNode(AlterObjectSchemaStmt);
	alter_stmt->objectType = (relkind == 'S') ? OBJECT_SEQUENCE : OBJECT_TABLE;
	alter_stmt->relation = rv;
	alter_stmt->newschema = pstrdup(trashcan_nspname);
	alter_stmt->missing_ok = false;

	/* Wrap in PlannedStmt */
	wrapper = makeNode(PlannedStmt);
	wrapper->commandType = CMD_UTILITY;
	wrapper->utilityStmt = (Node *) alter_stmt;
	wrapper->canSetTag = false;

	/* Execute ALTER SCHEMA */
	elog(DEBUG1, "pgtrashcan CASCADE: About to execute ALTER SCHEMA for rv->schemaname='%s', rv->relname='%s', newschema='%s'",
		 rv->schemaname ? rv->schemaname : "NULL",
		 rv->relname ? rv->relname : "NULL",
		 alter_stmt->newschema ? alter_stmt->newschema : "NULL");
	(*prev_ProcessUtility)(wrapper, NULL, false, PROCESS_UTILITY_TOPLEVEL,
						   NULL, NULL, NULL, NULL);

	CommandCounterIncrement();

	/* Rename the object in pgtrashcan schema */
	rename_in_trash(relid, orig_name, trash_name, false);

	/* For tables, rename indexes as well */
	if (relkind == 'r')
	{
		rel = RelationIdGetRelation(relid);
		if (RelationIsValid(rel))
		{
			indexoidlist = RelationGetIndexList(rel);
			foreach(lc, indexoidlist)
			{
				Oid indexoid = lfirst_oid(lc);
				char *indexname = get_rel_name(indexoid);
				char *index_trash_name;

				if (indexname)
				{
					index_trash_name = compose_trash_name(indexname, indexoid);
					rename_in_trash(indexoid, indexname, index_trash_name, true);
					pfree(index_trash_name);
					pfree(indexname);
				}
			}
			RelationClose(rel);
			list_free(indexoidlist);
		}

		/* Rename owned sequences in pgtrashcan schema */
		{
			StringInfoData seq_sql;
			int seq_ret;
			SPITupleTable *seq_tuptable;
			int seq_count;
			int seq_i;

			if (SPI_connect() != SPI_OK_CONNECT)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("SPI_connect failed in CASCADE sequence rename")));

			initStringInfo(&seq_sql);

			PG_TRY();
			{
				/* Query for sequences owned by this table */
				appendStringInfo(&seq_sql,
					"SELECT d.objid, c.relname "
					"FROM pg_depend d "
					"JOIN pg_class c ON c.oid = d.objid "
					"WHERE d.refobjid = %u "
					"  AND d.deptype IN ('a', 'i') "
					"  AND c.relkind = 'S'",
					relid);

				seq_ret = SPI_execute(seq_sql.data, true, 0);
				if (seq_ret != SPI_OK_SELECT)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("failed to query sequences for rename in CASCADE")));

				seq_tuptable = SPI_tuptable;
				seq_count = SPI_processed;

				elog(DEBUG1, "pgtrashcan CASCADE: Renaming %d owned sequence(s)", seq_count);

				/* Rename each sequence */
				for (seq_i = 0; seq_i < seq_count; seq_i++)
				{
					HeapTuple seq_tuple = seq_tuptable->vals[seq_i];
					TupleDesc seq_tupdesc = seq_tuptable->tupdesc;
					bool isnull;
					Oid seq_oid;
					char *seq_name;
					char *seq_trash_name;
					int fno_objid, fno_relname;

					fno_objid = SPI_fnumber(seq_tupdesc, "objid");
					fno_relname = SPI_fnumber(seq_tupdesc, "relname");

					if (fno_objid < 0 || fno_relname < 0)
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("required column not found in sequence rename query")));

					seq_oid = DatumGetObjectId(SPI_getbinval(seq_tuple, seq_tupdesc, fno_objid, &isnull));
					if (isnull)
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("objid is NULL in sequence rename query")));

					{
						Datum seq_name_datum = SPI_getbinval(seq_tuple, seq_tupdesc, fno_relname, &isnull);
						if (isnull)
							ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("relname is NULL in sequence rename query")));
						seq_name = pstrdup(NameStr(*DatumGetName(seq_name_datum)));
					}

					/* Generate trash name for sequence */
					seq_trash_name = compose_trash_name(seq_name, seq_oid);

					/* Rename sequence in pgtrashcan schema */
					rename_in_trash(seq_oid, seq_name, seq_trash_name, false);

					pfree(seq_trash_name);
					pfree(seq_name);
				}

				pfree(seq_sql.data);
			}
			PG_FINALLY();
			{
				SPI_finish();
			}
			PG_END_TRY();
		}
	}

	elog(NOTICE, "%s \"%s.%s\" moved to trash as \"%s\"",
		 trash_obj_type_str(obj_type), orig_schema, orig_name, trash_name);

	pfree(trash_name);
}

/*
 * move_dependent_objects_cascade - Find and move pg_depend-dependent objects to pgtrashcan
 *
 * This function implements CASCADE behavior by:
 * 1. Querying pg_depend to find dependent objects (tables via normal deps, sequences via auto deps)
 * 2. Recursively moving dependents (if they have dependents)
 * 3. Moving the dependent to pgtrashcan
 *
 * Note: This does NOT handle FK-referencing child tables. PostgreSQL's native CASCADE
 * only drops FK constraints on child tables, not the child tables themselves.
 * FK constraint handling is done separately by drop_foreign_keys_permanently().
 *
 * Called when DROP TABLE ... CASCADE is executed.
 */
static void
move_dependent_objects_cascade(Oid parent_oid, const char *parent_schema, const char *parent_name)
{
	StringInfoData sql;
	int ret;
	SPITupleTable *tuptable;
	int i;
	MemoryContext caller_ctx;

	/*
	 * Dynamic list for dependent info - store OIDs and relkind.
	 * IMPORTANT: These must be allocated in the caller's memory context,
	 * not in SPI's memory context, because SPI_finish() destroys its
	 * memory context and we need to iterate the list afterwards.
	 */
	List *dependents = NIL;
	int dependent_count = 0;

	initStringInfo(&sql);

	elog(DEBUG1, "pgtrashcan CASCADE: move_dependent_objects_cascade called for parent_oid=%u, parent_schema='%s', parent_name='%s'",
		 parent_oid, parent_schema ? parent_schema : "NULL", parent_name ? parent_name : "NULL");

	/* Save caller's memory context before SPI switches it */
	caller_ctx = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed in move_dependent_objects_cascade")));

	PG_TRY();
	{
		/*
		 * Query pg_depend for dependent objects (owned sequences, etc.)
		 * FK-referencing child tables are NOT included here — see comment below.
		 */
		{
			char *quoted_nspname = quote_literal_cstr(trashcan_nspname);
			/*
			 * Only query pg_depend for true object dependencies (owned sequences, etc.)
			 *
			 * We do NOT query pg_constraint here for FK-referencing child tables.
			 * PostgreSQL's native CASCADE only drops FK constraints on child tables,
			 * it does not drop the child tables themselves. FK constraint handling
			 * is done separately by drop_foreign_keys_permanently().
			 */
			appendStringInfo(&sql,
				"SELECT DISTINCT c.oid, c.relkind, "
				"  CASE c.relkind "
				"    WHEN 'r' THEN 1 "  /* tables first */
				"    WHEN 'S' THEN 2 "  /* sequences second */
				"    ELSE 3 END AS sort_order "
				"FROM pg_depend d "
				"JOIN pg_class c ON c.oid = d.objid "
				"JOIN pg_namespace n ON n.oid = c.relnamespace "
				"WHERE d.refobjid = %u "
				"  AND d.deptype IN ('n', 'a') "
				"  AND c.relkind IN ('r', 'S') "
				"  AND c.oid >= 16384 "
				"  AND n.nspname != %s "
				"ORDER BY sort_order",
				parent_oid, quoted_nspname);
			pfree(quoted_nspname);
		}

		ret = SPI_execute(sql.data, true, 0);
		if (ret != SPI_OK_SELECT)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to query dependent objects for CASCADE")));

		tuptable = SPI_tuptable;
		dependent_count = SPI_processed;

		elog(DEBUG1, "pgtrashcan: CASCADE found %d dependent object(s) for %s.%s",
			 dependent_count, parent_schema, parent_name);

		/*
		 * Store OIDs and relkind in the caller's memory context so they
		 * survive SPI_finish().  palloc() inside SPI allocates in SPI's
		 * procCxt which is destroyed by SPI_finish().
		 */
		for (i = 0; i < dependent_count; i++)
		{
			HeapTuple tuple = tuptable->vals[i];
			TupleDesc tupdesc = tuptable->tupdesc;
			bool isnull;
			Datum datum;
			int fno_oid, fno_relkind;
			DepInfo *dinfo;
			MemoryContext spi_ctx;

			fno_oid = SPI_fnumber(tupdesc, "oid");
			fno_relkind = SPI_fnumber(tupdesc, "relkind");

			if (fno_oid < 0 || fno_relkind < 0)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("required column not found in CASCADE dependent query")));

			datum = SPI_getbinval(tuple, tupdesc, fno_oid, &isnull);
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("oid is NULL in CASCADE dependent query")));

			/* Switch to caller's context for allocation that must survive SPI_finish */
			spi_ctx = MemoryContextSwitchTo(caller_ctx);

			dinfo = palloc(sizeof(DepInfo));
			dinfo->oid = DatumGetObjectId(datum);

			/* Switch back to SPI context for the next SPI_getbinval call */
			MemoryContextSwitchTo(spi_ctx);

			datum = SPI_getbinval(tuple, tupdesc, fno_relkind, &isnull);
			if (isnull)
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("relkind is NULL in CASCADE dependent query")));
			dinfo->relkind = DatumGetChar(datum);

			elog(DEBUG1, "pgtrashcan CASCADE: Found dependent %d: oid=%u, relkind='%c'",
				 i, dinfo->oid, dinfo->relkind);

			/* Switch to caller's context for lappend (allocates List cells) */
			spi_ctx = MemoryContextSwitchTo(caller_ctx);
			dependents = lappend(dependents, dinfo);
			MemoryContextSwitchTo(spi_ctx);
		}

		/* Now safe to finish SPI - dependents list is in caller's memory context */
		pfree(sql.data);
	}
	PG_FINALLY();
	{
		SPI_finish();
	}
	PG_END_TRY();

	/* Now move all dependents - get names fresh from system caches */
	{
		ListCell *lc;
		int dep_num = 0;
		foreach(lc, dependents)
		{
			DepInfo *dinfo = (DepInfo *) lfirst(lc);
			Oid dep_oid = dinfo->oid;
			char dep_relkind = dinfo->relkind;
			char *dep_name;
			char *dep_schema;
			Oid dep_namespace;

			dep_num++;

			/* Get fresh name and schema from system caches - these work outside SPI */
			dep_name = get_rel_name(dep_oid);
			dep_namespace = get_rel_namespace(dep_oid);
			dep_schema = get_namespace_name(dep_namespace);

			if (dep_name == NULL || dep_schema == NULL)
			{
				ereport(WARNING, (errmsg("CASCADE: Could not get name/schema for OID %u, skipping", dep_oid)));
				continue;
			}

			elog(DEBUG1, "pgtrashcan CASCADE: Processing dependent %d/%d: schema='%s', name='%s', oid=%u, relkind='%c'",
				 dep_num, dependent_count, dep_schema, dep_name, dep_oid, dep_relkind);

			/* Recursively move this dependent's dependents first */
			move_dependent_objects_cascade(dep_oid, dep_schema, dep_name);

			/* Now move the dependent itself */
			elog(DEBUG1, "pgtrashcan CASCADE: Moving dependent %s.%s to pgtrashcan", dep_schema, dep_name);
			move_single_object_to_trash(dep_oid, dep_schema, dep_name, dep_relkind, parent_oid);
			elog(DEBUG1, "pgtrashcan CASCADE: Completed moving %s.%s", dep_schema, dep_name);

			/* Free the names allocated by get_rel_name and get_namespace_name */
			pfree(dep_name);
			pfree(dep_schema);
		}
	}

	elog(DEBUG1, "pgtrashcan: CASCADE completed moving %d dependent(s) of %s.%s",
		 dependent_count, parent_schema, parent_name);

	/* Free the dependents list */
	list_free_deep(dependents);
}

static void
pgtrashcan_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						   bool readOnlyTree,
						   ProcessUtilityContext context,
						   ParamListInfo params,
						   QueryEnvironment *queryEnv,
						   DestReceiver *dest,
						   QueryCompletion *qc)
{
	Node *parsetree = pstmt->utilityStmt;
	PlannedStmt *newpstmt = NULL;

	elog(DEBUG1, "pgtrashcan: ProcessUtility called, nodeTag=%d", nodeTag(parsetree));

	/*
	 * Bypass all protections if extension is performing internal operations
	 * or if we're inside CREATE EXTENSION / ALTER EXTENSION UPDATE
	 * This prevents the extension from blocking its own catalog operations
	 */
	if (pgtrashcan_in_progress || creating_extension)
	{
		elog(DEBUG2, "pgtrashcan: Bypassing protections - internal operation in progress");
		goto execute_utility;
	}

	/*
	 * Block TRUNCATE on _trash_catalog
	 * (INSERT/UPDATE/DELETE protection is handled by the pgtrashcan_protect_catalog trigger)
	 * TRUNCATE is a utility command so it does go through ProcessUtility
	 */
	if (IsA(parsetree, TruncateStmt))
	{
		TruncateStmt *truncstmt = (TruncateStmt*)parsetree;
		ListCell *lc;
		foreach(lc, truncstmt->relations)
		{
			RangeVar *rv = (RangeVar*)lfirst(lc);
			if (rv->schemaname && rv->relname &&
				strcmp(rv->schemaname, trashcan_nspname) == 0 &&
				strcmp(rv->relname, "_trash_catalog") == 0)
			{
				ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("direct modification of trash catalog table is not allowed"),
					 errhint("Use pgtrashcan functions (restore, purge, rename) to manage trash")));
			}
		}
	}

	/*
	 * Block ALTER TABLE operations on _trash_catalog
	 * Schema changes to catalog table could break the extension
	 */
	if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt*)parsetree;
		if (stmt->relation && stmt->relation->schemaname && stmt->relation->relname &&
			strcmp(stmt->relation->schemaname, trashcan_nspname) == 0 &&
			strcmp(stmt->relation->relname, "_trash_catalog") == 0)
		{
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot alter trash catalog table structure"),
				 errhint("pgtrashcan catalog schema is managed by the extension")));
		}
	}

	/*
	 * Block CREATE operations in trash schema
	 * pgtrashcan schema is reserved for system-managed objects only
	 */
	if (IsA(parsetree, CreateStmt) ||      /* CREATE TABLE */
		IsA(parsetree, IndexStmt) ||       /* CREATE INDEX */
		IsA(parsetree, ViewStmt) ||        /* CREATE VIEW */
		IsA(parsetree, CreateSeqStmt) ||   /* CREATE SEQUENCE */
		IsA(parsetree, CreateTrigStmt) ||  /* CREATE TRIGGER */
		IsA(parsetree, CreateFunctionStmt)) /* CREATE FUNCTION */
	{
		RangeVar *relation = NULL;
		char *schema_name = NULL;

		if (IsA(parsetree, CreateStmt))
		{
			relation = ((CreateStmt*)parsetree)->relation;
			schema_name = relation ? relation->schemaname : NULL;
		}
		else if (IsA(parsetree, IndexStmt))
		{
			relation = ((IndexStmt*)parsetree)->relation;
			schema_name = relation ? relation->schemaname : NULL;
		}
		else if (IsA(parsetree, ViewStmt))
		{
			relation = ((ViewStmt*)parsetree)->view;
			schema_name = relation ? relation->schemaname : NULL;
		}
		else if (IsA(parsetree, CreateSeqStmt))
		{
			relation = ((CreateSeqStmt*)parsetree)->sequence;
			schema_name = relation ? relation->schemaname : NULL;
		}
		else if (IsA(parsetree, CreateTrigStmt))
		{
			CreateTrigStmt *stmt = (CreateTrigStmt*)parsetree;
			relation = stmt->relation;
			schema_name = relation ? relation->schemaname : NULL;
		}
		/* For CreateFunctionStmt, we'll check if function is being created in pgtrashcan schema */
		else if (IsA(parsetree, CreateFunctionStmt))
		{
			CreateFunctionStmt *stmt = (CreateFunctionStmt*)parsetree;
			/* Check if any of the function names specify the pgtrashcan schema */
			if (stmt->funcname && list_length(stmt->funcname) >= 2)
			{
				ListCell *lc = list_head(stmt->funcname);
				schema_name = strVal(lfirst(lc));
			}
		}

		if (schema_name && strcmp(schema_name, trashcan_nspname) == 0)
		{
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot create objects in trash schema"),
				 errhint("pgtrashcan schema is reserved for system use only")));
		}
	}

	/*
	 * Block ALTER operations on trash objects
	 * Trashed objects should remain immutable until restored
	 */
	if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt*)parsetree;
		if (stmt->relation && stmt->relation->schemaname &&
			strcmp(stmt->relation->schemaname, trashcan_nspname) == 0)
		{
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot alter trash objects: %s", stmt->relation->relname),
				 errhint("Restore the table first to make changes")));
		}
	}

	/*
	 * Block ALTER ... SET SCHEMA on trash objects
	 * Prevents users from moving objects out of pgtrashcan without catalog cleanup.
	 * Use pgtrashcan_restore() or pgtrashcan_move_to_schema() instead.
	 * SECURITY DEFINER functions (restore, move_to_schema) bypass this via
	 * the pgtrashcan_internal_operation GUC.
	 */
	if (IsA(parsetree, AlterObjectSchemaStmt) && !pgtrashcan_internal_operation)
	{
		AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parsetree;
		if (stmt->relation && stmt->relation->schemaname &&
			strcmp(stmt->relation->schemaname, trashcan_nspname) == 0)
		{
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot move trash objects directly"),
				 errhint("Use pgtrashcan_restore() to restore to original schema, "
						 "pgtrashcan_move_to_schema() to move to a different schema, "
						 "or pgtrashcan_purge() to permanently delete.")));
		}
	}

	/*
	 * Block RENAME on trash objects
	 * Prevents users from renaming objects in pgtrashcan directly, bypassing the catalog.
	 * Use pgtrashcan_rename_in_trash() for legitimate renames.
	 * SECURITY DEFINER functions bypass this via the pgtrashcan_internal_operation GUC.
	 */
	if (IsA(parsetree, RenameStmt) && !pgtrashcan_internal_operation)
	{
		RenameStmt *stmt = (RenameStmt *) parsetree;
		if (stmt->relation && stmt->relation->schemaname &&
			strcmp(stmt->relation->schemaname, trashcan_nspname) == 0)
		{
			ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot rename trash objects directly"),
				 errhint("Use pgtrashcan_rename_in_trash(trash_name, new_name) "
						 "to change the restore name.")));
		}
	}

	/*
	 * Block TRUNCATE operations on trash objects
	 * This would destroy data without proper catalog cleanup
	 */
	if (IsA(parsetree, TruncateStmt))
	{
		TruncateStmt *stmt = (TruncateStmt*)parsetree;
		ListCell *lc;

		foreach(lc, stmt->relations)
		{
			RangeVar *rv = (RangeVar*)lfirst(lc);
			if (rv->schemaname && strcmp(rv->schemaname, trashcan_nspname) == 0)
			{
				ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("cannot truncate trash objects: %s", rv->relname),
					 errhint("Use DROP to permanently delete or restore first")));
			}
		}
	}

	if (nodeTag(parsetree) == T_DropStmt)
	{
		DropStmt *stmt = (DropStmt *) parsetree;
		int num_tables;
		ListCell *obj_lc;
		int table_num = 0;

		elog(DEBUG1, "pgtrashcan: DROP statement detected, removeType=%d", stmt->removeType);

		/*
		 * Protect pgtrashcan schema from being dropped by users
		 * PostgreSQL also has extension dependency protection, but we provide
		 * a more helpful error message here
		 */
	if (stmt->removeType == OBJECT_SCHEMA)
	{
		ListCell *lc;

		elog(DEBUG1, "pgtrashcan: DROP SCHEMA detected in ProcessUtility hook");
		elog(DEBUG1, "pgtrashcan: Number of schemas: %d", list_length(stmt->objects));

		/* Only protect pgtrashcan schema if extension is still installed */
		if (!extension_exists())
		{
			elog(DEBUG1, "pgtrashcan: Extension not installed, allowing DROP SCHEMA to proceed");
			/* Fall through to standard_ProcessUtility */
		}
		else
		{
			foreach(lc, stmt->objects)
			{
				char *schema_name;
				Node *object = (Node *) lfirst(lc);

				/*
				 * For DROP SCHEMA, objects are String nodes directly,
				 * not Lists like DROP TABLE uses
				 */
				if (IsA(object, String))
				{
					schema_name = strVal(object);
					elog(DEBUG1, "pgtrashcan: Schema name=\"%s\", trashcan_nspname=\"%s\"",
						schema_name, trashcan_nspname);

					if (strcmp(schema_name, trashcan_nspname) == 0)
					{
						elog(DEBUG1, "pgtrashcan: MATCH! Throwing error");
						ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("cannot drop trash schema \"%s\"", trashcan_nspname),
							 errdetail("The \"%s\" schema is managed by pgtrashcan extension", trashcan_nspname),
							 errhint("Use pgtrashcan_purge_all() to empty trash, or DROP EXTENSION pgtrashcan CASCADE to uninstall")));
					}
				}
				else if (IsA(object, List))
				{
					/* Handle qualified schema names (schema.object) if they exist */
					List *objname = (List *) object;
					if (list_length(objname) >= 1)
					{
						schema_name = strVal(linitial(objname));
						elog(DEBUG1, "pgtrashcan: Qualified schema name=\"%s\"", schema_name);

						if (strcmp(schema_name, trashcan_nspname) == 0)
						{
							ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
								 errmsg("cannot drop trash schema \"%s\"", trashcan_nspname),
								 errdetail("The \"%s\" schema is managed by pgtrashcan extension", trashcan_nspname),
								 errhint("Use pgtrashcan_purge_all() to empty trash, or DROP EXTENSION pgtrashcan CASCADE to uninstall")));
						}
					}
				}
				else
				{
					elog(DEBUG1, "pgtrashcan: Unexpected object type in DROP SCHEMA: %d", nodeTag(object));
				}
			}
		}  /* end else (extension exists) */

		/* Schema is not pgtrashcan - allow standard DROP SCHEMA to proceed */
	}

	if (stmt->removeType == OBJECT_TABLE)
	{
		List *drop_oids = NIL;  /* List of OIDs being dropped in this statement */
		List *temp_table_objects = NIL;  /* Temp tables to drop separately (can't be trashed) */
		bool has_trash_deletions = false;  /* Track if any tables need permanent deletion from pgtrashcan */
		bool has_non_trash_tables = false; /* Track if any tables are outside pgtrashcan */
		ListCell *collect_lc;

		num_tables = list_length(stmt->objects);
		elog(DEBUG1, "pgtrashcan: DROP TABLE detected with %d table(s), intercepting", num_tables);

		/*
		 * FIRST PASS: Collect all table OIDs being dropped
		 * This allows us to exclude tables in the same DROP statement from dependency checks
		 * Note: DROP TABLE parent, child succeeds even when child has FK to parent (This matches PostgreSQL default behavior)
		 */
		foreach(collect_lc, stmt->objects)
		{
			RangeVar *r = makeRangeVarFromAnyName(lfirst(collect_lc));
			Oid relid;

			/* Track if in pgtrashcan schema (permanent deletion) */
			if (r->schemaname && strcmp(r->schemaname, trashcan_nspname) == 0)
			{
				has_trash_deletions = true;
				continue;
			}

			has_non_trash_tables = true;

			/* Get OID without locking (NoLock - we'll lock properly in second pass) */
			relid = RangeVarGetRelid(r, NoLock, stmt->missing_ok);
			if (OidIsValid(relid))
			{
				drop_oids = lappend_oid(drop_oids, relid);
				elog(DEBUG1, "pgtrashcan: Collected OID %u for dependency exclusion", relid);
			}
		}

		/*
		 * Block mixed drops: cannot combine pgtrashcan and non-pgtrashcan tables in one statement.
		 * e.g., DROP TABLE public.foo, pgtrashcan.bar_$12345 is not allowed.
		 * This avoids corruption where the original statement is passed to
		 * prev_ProcessUtility with already-moved tables still in its object list.
		 */
		if (has_trash_deletions && has_non_trash_tables)
		{
			list_free(drop_oids);
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot mix regular tables and pgtrashcan objects in a single DROP TABLE statement"),
				 errhint("Drop regular tables and pgtrashcan objects in separate statements.")));
		}

		/*
		 * If pgtrashcan.enabled is false, skip the trash-can transformation for
		 * non-pgtrashcan tables and let standard PostgreSQL handle the DROP. pgtrashcan
		 * protections fired above remain active. Pure DROPs of pgtrashcan objects
		 * (has_trash_deletions only) continue through the existing permanent
		 * purge path below.
		 */
		if (!pgtrashcan_enabled && has_non_trash_tables)
		{
			list_free(drop_oids);
			elog(DEBUG1, "pgtrashcan: disabled via GUC, falling through to standard DROP");
			goto execute_utility;
		}

		/*
		 * SECOND PASS: Loop through all tables and move them to pgtrashcan
		 * This handles both single-table: DROP TABLE t1;
		 * and multi-table: DROP TABLE t1, t2, t3;
		 */
		foreach(obj_lc, stmt->objects)
		{
			RangeVar *r;
			AlterObjectSchemaStmt *newstmt = makeNode(AlterObjectSchemaStmt);

			table_num++;
			elog(DEBUG1, "pgtrashcan: Processing table %d of %d", table_num, num_tables);

			r = makeRangeVarFromAnyName(lfirst(obj_lc));
			r->alias = NULL;

			/*
			 * Protect _trash_catalog table from being dropped by users
			 * This would destroy all recovery metadata
			 */
			if (r->schemaname && r->relname &&
				strcmp(r->schemaname, trashcan_nspname) == 0 &&
				strcmp(r->relname, "_trash_catalog") == 0)
			{
				ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("cannot drop trash catalog table"),
					 errhint("pgtrashcan catalog is managed automatically by the extension")));
			}

			newstmt->objectType = stmt->removeType;
			newstmt->newschema = pstrdup(trashcan_nspname);
			newstmt->missing_ok = stmt->missing_ok;

			newstmt->relation = r;

		if (!r->schemaname || strcmp(r->schemaname, trashcan_nspname) != 0)
		{
			Oid relid;
			Oid nspoid;
			const char *orig_schema;
			char *trash_name;
			char *fk_metadata;      /* FK metadata JSON */
			char *trigger_metadata; /* Trigger metadata JSON */
			char *view_metadata;    /* View metadata JSON */
			char *rule_metadata;    /* Rule metadata JSON */
			char *policy_metadata;  /* RLS Policy metadata JSON */
			List *indexoidlist;
			ListCell *lc;
			Relation rel;

			elog(DEBUG1, "pgtrashcan: Converting DROP to ALTER SCHEMA, moving to pgtrashcan");

			/* Get the table OID and lock it */
			relid = RangeVarGetRelid(r, AccessExclusiveLock, stmt->missing_ok);
			if (!OidIsValid(relid))
			{
				/*
				 * Table doesn't exist and missing_ok is true.
				 * Emit the standard PostgreSQL notice and skip to next table.
				 * We must NOT goto execute_utility here because in a multi-table
				 * DROP (e.g., DROP TABLE IF EXISTS missing, real), that would
				 * pass the entire original DropStmt to the standard handler,
				 * permanently dropping remaining tables instead of moving to trash.
				 */
				ereport(NOTICE,
					(errmsg("table \"%s\" does not exist, skipping",
							r->relname)));
				continue;
			}

			/* Block partitioned tables - not yet supported */
			if (get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
			{
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pgtrashcan does not support partitioned tables yet"),
					 errhint("Use DROP TABLE without pgtrashcan loaded to drop partitioned tables.")));
			}

			/* Block partition children - not yet supported */
			if (get_rel_relispartition(relid))
			{
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pgtrashcan does not support partition children yet"),
					 errhint("Use DROP TABLE without pgtrashcan loaded to drop partition children.")));
			}

			/* Block foreign tables - not supported */
			if (get_rel_relkind(relid) == RELKIND_FOREIGN_TABLE)
			{
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pgtrashcan does not support foreign tables"),
					 errhint("Use DROP FOREIGN TABLE to drop foreign tables.")));
			}

			/* Get the actual schema from the table's OID */
			nspoid = get_rel_namespace(relid);

			/* Skip temp tables — they can't be moved across schemas */
			if (isTempNamespace(nspoid))
			{
				ereport(NOTICE,
					(errmsg("temporary table \"%s\" will be permanently dropped (not moved to trash)",
							r->relname)));
				temp_table_objects = lappend(temp_table_objects, lfirst(obj_lc));
				continue;
			}

			orig_schema = get_namespace_name(nspoid);

			if (!orig_schema)
			{
				elog(DEBUG1, "pgtrashcan: Schema for table OID %u no longer exists, skipping", relid);
				continue;
			}

			/*
			 * Note: We rely on PostgreSQL's native locking for schema protection:
			 * - RangeVarGetRelid() acquires AccessExclusiveLock on the table
			 * - DROP SCHEMA will block when trying to drop this locked table
			 * - This allows concurrent DROP TABLE operations on different tables
			 */

			/* Generate trash name */
			trash_name = compose_trash_name(r->relname, relid);

			elog(DEBUG1, "pgtrashcan: Table %s.%s (OID %u) will be moved to pgtrashcan as %s",
				 orig_schema, r->relname, relid, trash_name);

			/*
			 * Check for dependencies BEFORE making any changes
			 * - If DROP_CASCADE: We'll move dependents after setting up catalog
			 * - Otherwise (default or RESTRICT): Block the drop if any dependencies exist
			 */
			elog(DEBUG1, "pgtrashcan: stmt->behavior = %d, DROP_CASCADE = %d", stmt->behavior, DROP_CASCADE);
			if (stmt->behavior != DROP_CASCADE)
			{
				elog(DEBUG1, "pgtrashcan: Running dependency checks");
				/* RESTRICT (or default): Block if any dependent objects exist (views, tables with FKs, etc.) */
				/* Pass drop_oids to exclude tables in the same DROP statement from dependency checks */
				check_dependent_objects(relid, r->relname, orig_schema, drop_oids);
				/* Also check for incoming foreign key constraints from other tables */
				check_incoming_fk_dependencies(relid, r->relname, orig_schema, drop_oids);
			}
			else
			{
				elog(DEBUG1, "pgtrashcan: Skipping dependency checks (CASCADE mode)");
			}

			parsetree = (Node *) newstmt;
			/* Create a new PlannedStmt wrapping the rewritten statement */
			newpstmt = makeNode(PlannedStmt);
			newpstmt->commandType = CMD_UTILITY;
			newpstmt->canSetTag = pstmt->canSetTag;
			newpstmt->utilityStmt = (Node *) newstmt;
			newpstmt->stmt_location = pstmt->stmt_location;
			newpstmt->stmt_len = pstmt->stmt_len;

			/* Create pgtrashcan schema and catalog if needed */
			create_trashcan_schema();
			ensure_trash_catalog_exists();

			/* Insert catalog entry for TABLE - this is the main table being dropped */
			insert_catalog_entry(relid, orig_schema, r->relname, TRASH_OBJ_TABLE, InvalidOid, InvalidOid, trash_name);

			/*
			 * CASCADE support - move dependent objects
			 */
			if (stmt->behavior == DROP_CASCADE)
			{
				/* CASCADE: Move dependent objects to pgtrashcan first */
				elog(DEBUG1, "pgtrashcan: DROP CASCADE detected, moving dependents first");
				move_dependent_objects_cascade(relid, orig_schema, r->relname);
				elog(DEBUG1, "pgtrashcan: CASCADE recursion completed");

				/*
				 * Make CASCADE changes visible to subsequent queries
				 * This ensures FK drops and table moves done during CASCADE
				 * are visible when we process the parent table's FKs
				 */
				CommandCounterIncrement();
			}

			/*
			 * Foreign Key, Trigger, View, Rule, and Policy Handling
			 * Detect all FK constraints, triggers, views, rules, and RLS policies, and store them in the metadata
			 * All are permanently dropped before moving to pgtrashcan
			 */
			fk_metadata = detect_and_build_fk_metadata_json(relid, orig_schema, r->relname);
			drop_foreign_keys_permanently(relid, drop_oids);  /* Pass drop_oids to skip incoming FKs from tables in same DROP */

			trigger_metadata = detect_and_build_trigger_metadata_json(relid);

			view_metadata = detect_and_build_view_metadata_json(relid);
			drop_views_permanently(relid);

			rule_metadata = detect_and_build_rule_metadata_json(relid);
			drop_rules_permanently(relid);

			policy_metadata = detect_and_build_policy_metadata_json(relid);
			drop_policies_permanently(relid);

			/* Get list of indexes on this table */
			rel = RelationIdGetRelation(relid);
			if (!RelationIsValid(rel))
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not open relation with OID %u", relid)));

			indexoidlist = RelationGetIndexList(rel);

			/* Insert catalog entries for each INDEX */
			foreach(lc, indexoidlist)
			{
				Oid indexoid = lfirst_oid(lc);
				char *indexname = get_rel_name(indexoid);
				char *index_trash_name;

				if (indexname)
				{
					index_trash_name = compose_trash_name(indexname, indexoid);
					/* Indexes are true dependents - use dependent_on_oid */
					insert_catalog_entry(indexoid, orig_schema, indexname,
									   TRASH_OBJ_INDEX, relid, InvalidOid, index_trash_name);
					pfree(index_trash_name);
					pfree(indexname);
				}
			}

			RelationClose(rel);
			list_free(indexoidlist);

			/*
			 * Get sequences owned by this table (SERIAL columns) and insert catalog entries
			 * These will automatically move to pgtrashcan with the table (via OWNED BY dependency)
			 */
			{
				StringInfoData seq_sql;
				int seq_ret;
				SPITupleTable *seq_tuptable;
				int seq_count;
				int seq_i;

				if (SPI_connect() != SPI_OK_CONNECT)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("SPI_connect failed while querying owned sequences")));

				initStringInfo(&seq_sql);

				PG_TRY();
				{
					/* Query for sequences owned by this table (dependency type 'a' or 'i') */
					appendStringInfo(&seq_sql,
						"SELECT d.objid, c.relname "
						"FROM pg_depend d "
						"JOIN pg_class c ON c.oid = d.objid "
						"WHERE d.refobjid = %u "
						"  AND d.deptype IN ('a', 'i') "
						"  AND c.relkind = 'S'",
						relid);

					elog(DEBUG1, "pgtrashcan: Querying owned sequences BEFORE move: %s", seq_sql.data);

					seq_ret = SPI_execute(seq_sql.data, true, 0);
					if (seq_ret != SPI_OK_SELECT)
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("failed to query owned sequences")));

					seq_tuptable = SPI_tuptable;
					seq_count = SPI_processed;

					elog(DEBUG1, "pgtrashcan: Found %d owned sequence(s) to track", seq_count);

					/* Insert catalog entries for each sequence */
					for (seq_i = 0; seq_i < seq_count; seq_i++)
					{
						HeapTuple seq_tuple = seq_tuptable->vals[seq_i];
						TupleDesc seq_tupdesc = seq_tuptable->tupdesc;
						bool isnull;
						Oid seq_oid;
						char *seq_name;
						char *seq_trash_name;
						int fno_objid, fno_relname;

						fno_objid = SPI_fnumber(seq_tupdesc, "objid");
						fno_relname = SPI_fnumber(seq_tupdesc, "relname");

						if (fno_objid < 0 || fno_relname < 0)
							ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("required column not found in owned sequence query")));

						seq_oid = DatumGetObjectId(SPI_getbinval(seq_tuple, seq_tupdesc, fno_objid, &isnull));
						if (isnull)
							ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("objid is NULL in owned sequence query")));

						{
							Datum seq_name_datum = SPI_getbinval(seq_tuple, seq_tupdesc, fno_relname, &isnull);
							if (isnull)
								ereport(ERROR,
									(errcode(ERRCODE_INTERNAL_ERROR),
									 errmsg("relname is NULL in owned sequence query")));
							seq_name = pstrdup(NameStr(*DatumGetName(seq_name_datum)));
						}

						elog(DEBUG1, "pgtrashcan: Tracking owned sequence: %s (OID %u)", seq_name, seq_oid);

						/* Generate trash name for sequence */
						seq_trash_name = compose_trash_name(seq_name, seq_oid);

						/* Insert catalog entry for sequence (it's a true dependent) */
						insert_catalog_entry(seq_oid, orig_schema, seq_name,
											 TRASH_OBJ_SEQUENCE, relid, InvalidOid, seq_trash_name);

						pfree(seq_trash_name);
						pfree(seq_name);
					}

					pfree(seq_sql.data);
				}
				PG_FINALLY();
				{
					SPI_finish();
				}
				PG_END_TRY();
			}

			/* Execute the ALTER SCHEMA (this moves table + indexes + sequences + constraints automatically) */
			elog(DEBUG1, "pgtrashcan: Executing ALTER TABLE SET SCHEMA");
			(*prev_ProcessUtility)(newpstmt ? newpstmt : pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);


			/* CRITICAL: Make ALTER TABLE SET SCHEMA visible */
			CommandCounterIncrement();
			elog(DEBUG1, "pgtrashcan: Schema change made visible");
			/* Now rename the table in pgtrashcan schema */
			rename_in_trash(relid, r->relname, trash_name, false);

			/* Rename indexes in pgtrashcan schema */
			rel = RelationIdGetRelation(relid);
			if (!RelationIsValid(rel))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("could not open relation with OID %u after moving it to the pgtrashcan schema",
								relid)));
			indexoidlist = RelationGetIndexList(rel);
			foreach(lc, indexoidlist)
			{
				Oid indexoid = lfirst_oid(lc);
				char *indexname = get_rel_name(indexoid);
				char *index_trash_name;

				if (indexname)
				{
					index_trash_name = compose_trash_name(indexname, indexoid);
					rename_in_trash(indexoid, indexname, index_trash_name, true);
					pfree(index_trash_name);
					pfree(indexname);
				}
			}
			RelationClose(rel);

			list_free(indexoidlist);

			/* Rename owned sequences in pgtrashcan schema */
			{
				StringInfoData seq_sql;
				int seq_ret;
				SPITupleTable *seq_tuptable;
				int seq_count;
				int seq_i;

				if (SPI_connect() != SPI_OK_CONNECT)
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("SPI_connect failed while querying sequences for rename")));

				initStringInfo(&seq_sql);

				PG_TRY();
				{
					/* Query for sequences owned by this table (they're now in pgtrashcan schema) */
					appendStringInfo(&seq_sql,
						"SELECT d.objid, c.relname "
						"FROM pg_depend d "
						"JOIN pg_class c ON c.oid = d.objid "
						"WHERE d.refobjid = %u "
						"  AND d.deptype IN ('a', 'i') "
						"  AND c.relkind = 'S'",
						relid);

					seq_ret = SPI_execute(seq_sql.data, true, 0);
					if (seq_ret != SPI_OK_SELECT)
						ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("failed to query sequences for rename")));

					seq_tuptable = SPI_tuptable;
					seq_count = SPI_processed;

					elog(DEBUG1, "pgtrashcan: Renaming %d owned sequence(s)", seq_count);

					/* Rename each sequence */
					for (seq_i = 0; seq_i < seq_count; seq_i++)
					{
						HeapTuple seq_tuple = seq_tuptable->vals[seq_i];
						TupleDesc seq_tupdesc = seq_tuptable->tupdesc;
						bool isnull;
						Oid seq_oid;
						char *seq_name;
						char *seq_trash_name;
						int fno_objid, fno_relname;

						fno_objid = SPI_fnumber(seq_tupdesc, "objid");
						fno_relname = SPI_fnumber(seq_tupdesc, "relname");

						if (fno_objid < 0 || fno_relname < 0)
							ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("required column not found in sequence rename query")));

						seq_oid = DatumGetObjectId(SPI_getbinval(seq_tuple, seq_tupdesc, fno_objid, &isnull));
						if (isnull)
							ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("objid is NULL in sequence rename query")));

						{
							Datum seq_name_datum = SPI_getbinval(seq_tuple, seq_tupdesc, fno_relname, &isnull);
							if (isnull)
								ereport(ERROR,
									(errcode(ERRCODE_INTERNAL_ERROR),
									 errmsg("relname is NULL in sequence rename query")));
							seq_name = pstrdup(NameStr(*DatumGetName(seq_name_datum)));
						}

						/* Generate trash name for sequence */
						seq_trash_name = compose_trash_name(seq_name, seq_oid);

						elog(DEBUG1, "pgtrashcan: Renaming sequence %s to %s", seq_name, seq_trash_name);

						/* Rename sequence in pgtrashcan schema */
						rename_in_trash(seq_oid, seq_name, seq_trash_name, false);

						pfree(seq_trash_name);
						pfree(seq_name);
					}

					pfree(seq_sql.data);
				}
				PG_FINALLY();
				{
					SPI_finish();
				}
				PG_END_TRY();
			}


			/* Update catalog with complete metadata (FK + triggers + views + rules + policies) */
			update_catalog_with_complete_metadata(trash_name, fk_metadata, trigger_metadata, view_metadata, rule_metadata, policy_metadata);

			/* Free metadata strings */
			if (fk_metadata) pfree(fk_metadata);
			if (trigger_metadata) pfree(trigger_metadata);
			if (view_metadata) pfree(view_metadata);
			if (rule_metadata) pfree(rule_metadata);
			if (policy_metadata) pfree(policy_metadata);

			elog(NOTICE, "Table \"%s.%s\" moved to trash as \"%s\"",
				 orig_schema, r->relname, trash_name);

			pfree(trash_name);
			/* Continue to next table in multi-table DROP */
		}
		else
		{
			/* Dropping from pgtrashcan - permanent deletion */
			elog(NOTICE, "Permanently dropping object from pgtrashcan: %s", r->relname);
			delete_catalog_entry_by_trash_name(r->relname);
			has_trash_deletions = true;  /* Mark that we need to execute actual DROP */

			/*
			 * For tables in pgtrashcan, we need PostgreSQL to execute the DROP
			 * We'll fall through to execute_utility after processing all tables
			 */
		}
		}  /* end foreach table */

		/*
		 * Drop any temp tables that were skipped above.
		 * Temp tables can't be moved to pgtrashcan (cross-schema move not allowed for
		 * temp objects), so we construct a separate DropStmt and execute it directly.
		 */
		if (temp_table_objects != NIL)
		{
			DropStmt *temp_drop = makeNode(DropStmt);
			temp_drop->removeType = OBJECT_TABLE;
			temp_drop->missing_ok = stmt->missing_ok;
			temp_drop->behavior = stmt->behavior;
			temp_drop->concurrent = stmt->concurrent;
			temp_drop->objects = temp_table_objects;

			pgtrashcan_in_progress = true;
			PG_TRY();
			{
				PlannedStmt *temp_pstmt = makeNode(PlannedStmt);
				temp_pstmt->commandType = CMD_UTILITY;
				temp_pstmt->canSetTag = pstmt->canSetTag;
				temp_pstmt->utilityStmt = (Node *) temp_drop;
				temp_pstmt->stmt_location = pstmt->stmt_location;
				temp_pstmt->stmt_len = pstmt->stmt_len;
				(*prev_ProcessUtility)(temp_pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
			}
			PG_FINALLY();
			{
				pgtrashcan_in_progress = false;
			}
			PG_END_TRY();
		}

		/*
		 * All tables processed.
		 * - If all tables were moved to trash: return (don't execute DROP)
		 * - If any tables were in pgtrashcan (permanent deletion): fall through to execute DROP
		 */
		if (!has_trash_deletions)
		{
			list_free(drop_oids);
			return;  /* All tables moved to trash - don't execute original DROP */
		}
		/* else: has_trash_deletions == true, fall through to execute_utility */
		list_free(drop_oids);
	}
}

execute_utility:
	(*prev_ProcessUtility)(newpstmt ? newpstmt : pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
}
