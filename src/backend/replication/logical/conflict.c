/*-------------------------------------------------------------------------
 * conflict.c
 *	   Support routines for logging conflicts.
 *
 * Copyright (c) 2024-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/conflict.c
 *
 * This file contains the code for logging conflicts on the subscriber during
 * logical replication.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/commit_ts.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_namespace.h"
#include "catalog/toasting.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "pgstat.h"
#include "replication/conflict.h"
#include "replication/worker_internal.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_lsn.h"

/*
 * String representations for the supported conflict logging destinations.
 */
const char *const ConflictLogDestNames[] = {
	[CONFLICT_LOG_DEST_LOG] = "log",
	[CONFLICT_LOG_DEST_TABLE] = "table",
	[CONFLICT_LOG_DEST_ALL] = "all"
};

StaticAssertDecl(lengthof(ConflictLogDestNames) == CONFLICT_LOG_DEST_ALL + 1,
				 "ConflictLogDestNames length mismatch");


/* Structure to hold metadata for one column of the conflict log table */
typedef struct ConflictLogColumnDef
{
	const char *attname;		/* Column name */
	Oid			atttypid;		/* Data type OID */
} ConflictLogColumnDef;

/*
 * Schema definition for conflict log tables.
 *
 * Defines the fixed schema of the per-subscription conflict log table created
 * in the pg_conflict namespace. Each entry specifies the column name and its
 * type OID; the table is created in this column order by
 * create_conflict_log_table().
 *
 * The tuple/key columns (replica_identity, remote_tuple, local_conflicts) are
 * typed json rather than jsonb on purpose: they hold an exact audit snapshot
 * of the applied tuples and replica identity, and json preserves the verbatim
 * representation whereas jsonb would normalize it. Indexing them (jsonb's main
 * advantage) wouldn't help anyway, as the conflict log is looked up by its
 * scalar columns (relid, conflict_type, commit timestamp) while these json
 * columns are per-conflict payload to inspect, not search keys.
 *
 * 'local_conflicts' is typed as an array of JSON objects (json[]), not a
 * single json object, so that a future conflict type needing to record
 * multiple local rows for one remote operation doesn't require a
 * backward-incompatible schema change.
 */
static const ConflictLogColumnDef ConflictLogSchema[] = {
	{.attname = "relid", .atttypid = OIDOID},
	{.attname = "schemaname", .atttypid = TEXTOID},
	{.attname = "relname", .atttypid = TEXTOID},
	{.attname = "conflict_type", .atttypid = TEXTOID},
	{.attname = "remote_xid", .atttypid = XIDOID},
	{.attname = "remote_commit_lsn", .atttypid = LSNOID},
	{.attname = "remote_commit_ts", .atttypid = TIMESTAMPTZOID},
	{.attname = "remote_origin", .atttypid = TEXTOID},
	{.attname = "replica_identity_full", .atttypid = BOOLOID},
	{.attname = "replica_identity", .atttypid = JSONOID},
	{.attname = "remote_tuple", .atttypid = JSONOID},
	{.attname = "local_conflicts", .atttypid = JSONARRAYOID}
};

#define NUM_CONFLICT_ATTRS ((AttrNumber) lengthof(ConflictLogSchema))

/*
 * Schema for the elements within the 'local_conflicts' JSON array.
 */
static const ConflictLogColumnDef LocalConflictSchema[] =
{
	{.attname = "xid", .atttypid = XIDOID},
	{.attname = "commit_ts", .atttypid = TIMESTAMPTZOID},
	{.attname = "origin", .atttypid = TEXTOID},
	{.attname = "tuple", .atttypid = JSONOID}
};

#define NUM_LOCAL_CONFLICT_ATTRS lengthof(LocalConflictSchema)

static const char *const ConflictTypeNames[] = {
	[CT_INSERT_EXISTS] = "insert_exists",
	[CT_UPDATE_ORIGIN_DIFFERS] = "update_origin_differs",
	[CT_UPDATE_EXISTS] = "update_exists",
	[CT_UPDATE_MISSING] = "update_missing",
	[CT_DELETE_ORIGIN_DIFFERS] = "delete_origin_differs",
	[CT_UPDATE_DELETED] = "update_deleted",
	[CT_DELETE_MISSING] = "delete_missing",
	[CT_MULTIPLE_UNIQUE_CONFLICTS] = "multiple_unique_conflicts"
};

static Relation get_conflictlog_dest_and_table(ConflictLogDest *log_dest);
static int	errcode_apply_conflict(ConflictType type);
static void errdetail_apply_conflict(EState *estate,
									 ResultRelInfo *relinfo,
									 ConflictType type,
									 TupleTableSlot *searchslot,
									 TupleTableSlot *localslot,
									 TupleTableSlot *remoteslot,
									 Oid indexoid, TransactionId localxmin,
									 ReplOriginId localorigin,
									 TimestampTz localts, StringInfo err_msg);
static void get_tuple_desc(EState *estate, ResultRelInfo *relinfo,
						   ConflictType type, char **key_desc,
						   TupleTableSlot *localslot, char **local_desc,
						   TupleTableSlot *remoteslot, char **remote_desc,
						   TupleTableSlot *searchslot, char **search_desc,
						   Oid indexoid);
static void build_index_datums_from_slot(EState *estate, Relation localrel,
										 TupleTableSlot *slot,
										 Relation indexDesc, Datum *values,
										 bool *isnull);
static char *build_index_value_desc(EState *estate, Relation localrel,
									TupleTableSlot *slot, Oid indexoid);
static Datum tuple_table_slot_to_json_datum(TupleTableSlot *slot);
static Datum tuple_table_slot_to_indextup_json(EState *estate,
											   Relation localrel,
											   Oid replica_index,
											   TupleTableSlot *slot);
static TupleDesc build_local_conflicts_tupledesc(void);
static Datum build_local_conflicts_json_array(List *conflicttuples);
static void insert_conflict_log_tuple(EState *estate, Relation rel,
									  Relation conflictlogrel,
									  ConflictType conflict_type,
									  TupleTableSlot *searchslot,
									  List *conflicttuples,
									  TupleTableSlot *remoteslot);

/*
 * Builds the TupleDesc for the conflict log table.
 */
static TupleDesc
create_conflict_log_table_tupdesc(void)
{
	TupleDesc	tupdesc;

	tupdesc = CreateTemplateTupleDesc(NUM_CONFLICT_ATTRS);

	for (int i = 0; i < NUM_CONFLICT_ATTRS; i++)
		TupleDescInitEntry(tupdesc, i + 1,
						   ConflictLogSchema[i].attname,
						   ConflictLogSchema[i].atttypid,
						   -1, 0);

	TupleDescFinalize(tupdesc);

	return tupdesc;
}

/*
 * Create a structured conflict log table for a subscription.
 *
 * The table is created within the system-managed 'pg_conflict' namespace to
 * prevent users from manually dropping or altering it.  This also prevents
 * accidental name collisions with user-created tables with the same name.
 *
 * The table name is generated automatically using the subscription's OID
 * (e.g., "pg_conflict_log_<subid>") to ensure uniqueness within the
 * cluster and to avoid collisions during subscription renames.
 */
Oid
create_conflict_log_table(Oid subid, char *subname, Oid subowner)
{
	TupleDesc	tupdesc;
	Oid			relid;
	char		relname[NAMEDATALEN];

	snprintf(relname, NAMEDATALEN, "pg_conflict_log_%u", subid);

	/* Build the tuple descriptor for the new table. */
	tupdesc = create_conflict_log_table_tupdesc();

	/* Create conflict log table. */
	relid = heap_create_with_catalog(relname,
									 PG_CONFLICT_NAMESPACE,
									 0, /* tablespace */
									 InvalidOid,	/* relid */
									 InvalidOid,	/* reltypeid */
									 InvalidOid,	/* reloftypeid */
									 subowner,
									 HEAP_TABLE_AM_OID,
									 tupdesc,
									 NIL,
									 RELKIND_RELATION,
									 RELPERSISTENCE_PERMANENT,
									 false, /* shared_relation */
									 false, /* mapped_relation */
									 ONCOMMIT_NOOP,
									 (Datum) 0, /* reloptions */
									 false, /* use_user_acl */
									 false, /* allow_system_table_mods */
									 true,	/* is_internal */
									 InvalidOid,	/* relrewrite */
									 NULL); /* typaddress */
	Assert(OidIsValid(relid));

	/* Release tuple descriptor memory. */
	FreeTupleDesc(tupdesc);

	/*
	 * We must bump the command counter to make the newly-created relation
	 * tuple visible for opening.
	 */
	CommandCounterIncrement();

	/*
	 * Create a TOAST table for the conflict log to support out-of-line
	 * storage of large json data.
	 */
	NewRelationCreateToastTable(relid, (Datum) 0);

	ereport(NOTICE,
			(errmsg("created conflict log table \"%s\" for subscription \"%s\"",
					get_qualified_objname(PG_CONFLICT_NAMESPACE, relname),
					subname)));

	return relid;
}

/*
 * Convert the string representation of a conflict logging destination to its
 * corresponding enum value.
 */
ConflictLogDest
GetConflictLogDest(const char *dest)
{
	/* NULL defaults to LOG. */
	if (dest == NULL || pg_strcasecmp(dest, "log") == 0)
		return CONFLICT_LOG_DEST_LOG;

	if (pg_strcasecmp(dest, "table") == 0)
		return CONFLICT_LOG_DEST_TABLE;

	if (pg_strcasecmp(dest, "all") == 0)
		return CONFLICT_LOG_DEST_ALL;

	/* Unrecognized string. */
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized conflict_log_destination value: \"%s\"", dest),
			 errhint("Valid values are \"log\", \"table\", and \"all\".")));
}

/*
 * Get the xmin and commit timestamp data (origin and timestamp) associated
 * with the provided local row.
 *
 * Return true if the commit timestamp data was found, false otherwise.
 */
bool
GetTupleTransactionInfo(TupleTableSlot *localslot, TransactionId *xmin,
						ReplOriginId *localorigin, TimestampTz *localts)
{
	Datum		xminDatum;
	bool		isnull;

	xminDatum = slot_getsysattr(localslot, MinTransactionIdAttributeNumber,
								&isnull);
	*xmin = DatumGetTransactionId(xminDatum);
	Assert(!isnull);

	/*
	 * The commit timestamp data is not available if track_commit_timestamp is
	 * disabled.
	 */
	if (!track_commit_timestamp)
	{
		*localorigin = InvalidReplOriginId;
		*localts = 0;
		return false;
	}

	return TransactionIdGetCommitTsData(*xmin, localts, localorigin);
}

/*
 * This function is used to report a conflict while applying replication
 * changes.
 *
 * 'searchslot' should contain the tuple used to search the local row to be
 * updated or deleted.
 *
 * 'remoteslot' should contain the remote new tuple, if any.
 *
 * conflicttuples is a list of local rows that caused the conflict and the
 * conflict related information. See ConflictTupleInfo.
 *
 * The caller must ensure that all the indexes passed in ConflictTupleInfo are
 * locked so that we can fetch and display the conflicting key values.
 */
void
ReportApplyConflict(EState *estate, ResultRelInfo *relinfo, int elevel,
					ConflictType type, TupleTableSlot *searchslot,
					TupleTableSlot *remoteslot, List *conflicttuples)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	ConflictLogDest dest;
	Relation	conflictlogrel = NULL;
	bool		log_dest_table = false;
	bool		log_dest_logfile = false;

	pgstat_report_subscription_conflict(MySubscription->oid, type);

	/*
	 * Only LOG-level conflicts (i.e. resolved conflicts where the transaction
	 * continues) are recorded in the conflict log table.  ERROR-level
	 * conflicts halt replication and abort the transaction, so they are
	 * always reported exclusively to the server log.
	 */
	if (elevel < ERROR)
	{
		conflictlogrel = get_conflictlog_dest_and_table(&dest);
		log_dest_table = CONFLICTS_LOGGED_TO_TABLE(dest);
		log_dest_logfile = CONFLICTS_LOGGED_TO_LOG(dest);

		/*
		 * If a conflict log table was requested but it has been dropped
		 * concurrently (e.g. a concurrent ALTER SUBSCRIPTION changed
		 * conflict_log_destination), get_conflictlog_dest_and_table()
		 * returned NULL.  Fall back to logging to the server log so that the
		 * conflict is not lost.
		 */
		if (log_dest_table && conflictlogrel == NULL)
		{
			log_dest_table = false;
			log_dest_logfile = true;
		}
	}
	else
	{
		/* ERROR-level conflicts always go to the server log with full detail */
		log_dest_logfile = true;
	}

	/*
	 * Report the conflict to the server log.  When the server log is one of
	 * the destinations (or for ERROR-level conflicts), emit the full details.
	 * Otherwise (table-only for LOG-level conflicts), emit a shorter message
	 * noting that the details are captured in the conflict log table.
	 */
	if (log_dest_logfile)
	{
		StringInfoData err_detail;

		initStringInfo(&err_detail);

		/* Form errdetail message by combining conflicting tuples information. */
		if (conflicttuples != NIL)
		{
			foreach_ptr(ConflictTupleInfo, conflicttuple, conflicttuples)
				errdetail_apply_conflict(estate, relinfo, type, searchslot,
										 conflicttuple->slot, remoteslot,
										 conflicttuple->indexoid,
										 conflicttuple->xmin,
										 conflicttuple->origin,
										 conflicttuple->ts,
										 &err_detail);
		}
		else
		{
			errdetail_apply_conflict(estate, relinfo, type, searchslot,
									 NULL, remoteslot,
									 InvalidOid,
									 InvalidTransactionId,
									 InvalidReplOriginId,
									 0,
									 &err_detail);
		}

		/* Standard reporting with full internal details. */
		ereport(elevel,
				errcode_apply_conflict(type),
				errmsg("conflict detected on relation \"%s\": conflict=%s",
					   RelationGetQualifiedRelationName(localrel),
					   ConflictTypeNames[type]),
				errdetail_internal("%s", err_detail.data));
	}
	else if (log_dest_table)
	{
		/*
		 * Not logging conflict details to the server log; report the conflict
		 * but omit raw tuple data since it is captured in the conflict log
		 * table.
		 */
		ereport(elevel,
				errcode_apply_conflict(type),
				errmsg("conflict detected on relation \"%s\": conflict=%s",
					   RelationGetQualifiedRelationName(localrel),
					   ConflictTypeNames[type]),
				errdetail("Conflict details are logged to the conflict log table: %s",
						  RelationGetQualifiedRelationName(conflictlogrel)));
	}

	/*
	 * Insert into the conflict log table for LOG-level conflicts if
	 * requested.
	 */
	if (log_dest_table)
	{
		Assert(conflictlogrel != NULL);
		Assert(elevel < ERROR);

		insert_conflict_log_tuple(estate,
								  relinfo->ri_RelationDesc,
								  conflictlogrel,
								  type,
								  searchslot,
								  conflicttuples,
								  remoteslot);
		table_close(conflictlogrel, NoLock);
	}
}

/*
 * Find all unique indexes to check for a conflict and store them into
 * ResultRelInfo.
 */
void
InitConflictIndexes(ResultRelInfo *relInfo)
{
	List	   *uniqueIndexes = NIL;

	for (int i = 0; i < relInfo->ri_NumIndices; i++)
	{
		Relation	indexRelation = relInfo->ri_IndexRelationDescs[i];

		if (indexRelation == NULL)
			continue;

		/* Detect conflict only for unique indexes */
		if (!relInfo->ri_IndexRelationInfo[i]->ii_Unique)
			continue;

		/* Don't support conflict detection for deferrable index */
		if (!indexRelation->rd_index->indimmediate)
			continue;

		uniqueIndexes = lappend_oid(uniqueIndexes,
									RelationGetRelid(indexRelation));
	}

	relInfo->ri_onConflictArbiterIndexes = uniqueIndexes;
}

/*
 * get_conflictlog_dest_and_table
 *
 * Fetches conflict logging metadata from the cached MySubscription pointer.
 * Sets the destination enum in *log_dest and, if a table is one of the
 * destinations, opens and returns the relation handle for the conflict log
 * table.
 *
 * The table is opened with try_table_open(), so NULL is returned if the
 * conflict log table has been dropped concurrently (e.g. by an ALTER
 * SUBSCRIPTION that changed conflict_log_destination).  Callers must treat a
 * NULL result for a table destination as "table unavailable" and fall back to
 * server-log reporting rather than failing.
 */
static Relation
get_conflictlog_dest_and_table(ConflictLogDest *log_dest)
{
	Oid			conflictlogrelid;

	/*
	 * Convert the text log destination to the internal enum.  MySubscription
	 * already contains the data from pg_subscription.
	 */
	*log_dest = GetConflictLogDest(MySubscription->conflictlogdest);

	/* Quick exit if a conflict log table was not requested. */
	if (!CONFLICTS_LOGGED_TO_TABLE(*log_dest))
		return NULL;

	conflictlogrelid = MySubscription->conflictlogrelid;

	Assert(OidIsValid(conflictlogrelid));

	/*
	 * Use try_table_open(): the table may have been dropped concurrently by
	 * an ALTER SUBSCRIPTION that changed conflict_log_destination.  Returning
	 * NULL lets the caller fall back to the server log instead of failing.
	 */
	return try_table_open(conflictlogrelid, RowExclusiveLock);
}

/*
 * Add SQLSTATE error code to the current conflict report.
 */
static int
errcode_apply_conflict(ConflictType type)
{
	switch (type)
	{
		case CT_INSERT_EXISTS:
		case CT_UPDATE_EXISTS:
		case CT_MULTIPLE_UNIQUE_CONFLICTS:
			return errcode(ERRCODE_UNIQUE_VIOLATION);
		case CT_UPDATE_ORIGIN_DIFFERS:
		case CT_UPDATE_MISSING:
		case CT_DELETE_ORIGIN_DIFFERS:
		case CT_UPDATE_DELETED:
		case CT_DELETE_MISSING:
			return errcode(ERRCODE_T_R_SERIALIZATION_FAILURE);
	}

	Assert(false);
	return 0;					/* silence compiler warning */
}

/*
 * Helper function to build the additional details for conflicting key,
 * local row, remote row, and replica identity columns.
 */
static void
append_tuple_value_detail(StringInfo buf, List *tuple_values)
{
	bool		first = true;

	Assert(buf != NULL && tuple_values != NIL);

	foreach_ptr(char, tuple_value, tuple_values)
	{
		/*
		 * Skip if the value is NULL. This means the current user does not
		 * have enough permissions to see all columns in the table. See
		 * get_tuple_desc().
		 */
		if (!tuple_value)
			continue;

		/* standard SQL punctuation, not translated */
		if (!first)
			appendStringInfoString(buf, ", ");

		appendStringInfoString(buf, tuple_value);
		first = false;
	}
}

/*
 * Add an errdetail() line showing conflict detail.
 *
 * The DETAIL line comprises of two parts:
 * 1. Explanation of the conflict type, including the origin and commit
 *    timestamp of the local row.
 * 2. Display of conflicting key, local row, remote new row, and replica
 *    identity columns, if any. The remote old row is excluded as its
 *    information is covered in the replica identity columns.
 */
static void
errdetail_apply_conflict(EState *estate, ResultRelInfo *relinfo,
						 ConflictType type, TupleTableSlot *searchslot,
						 TupleTableSlot *localslot, TupleTableSlot *remoteslot,
						 Oid indexoid, TransactionId localxmin,
						 ReplOriginId localorigin, TimestampTz localts,
						 StringInfo err_msg)
{
	StringInfoData err_detail;
	StringInfoData tuple_buf;
	char	   *origin_name;
	char	   *key_desc = NULL;
	char	   *local_desc = NULL;
	char	   *remote_desc = NULL;
	char	   *search_desc = NULL;

	/* Get key, replica identity, remote, and local value data */
	get_tuple_desc(estate, relinfo, type, &key_desc,
				   localslot, &local_desc,
				   remoteslot, &remote_desc,
				   searchslot, &search_desc,
				   indexoid);

	initStringInfo(&err_detail);
	initStringInfo(&tuple_buf);

	/* Construct a detailed message describing the type of conflict */
	switch (type)
	{
		case CT_INSERT_EXISTS:
		case CT_UPDATE_EXISTS:
		case CT_MULTIPLE_UNIQUE_CONFLICTS:
			Assert(OidIsValid(indexoid) &&
				   CheckRelationOidLockedByMe(indexoid, RowExclusiveLock, true));

			if (err_msg->len == 0)
			{
				append_tuple_value_detail(&tuple_buf,
										  list_make2(remote_desc, search_desc));

				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Could not apply remote change: %s.\n"),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Could not apply remote change.\n"));


				resetStringInfo(&tuple_buf);
			}

			append_tuple_value_detail(&tuple_buf,
									  list_make2(key_desc, local_desc));

			if (localts)
			{
				if (localorigin == InvalidReplOriginId)
				{
					if (tuple_buf.len)
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified locally in transaction %u at %s: %s."),
										 get_rel_name(indexoid),
										 localxmin, timestamptz_to_str(localts),
										 tuple_buf.data);
					else
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified locally in transaction %u at %s."),
										 get_rel_name(indexoid),
										 localxmin, timestamptz_to_str(localts));
				}
				else if (replorigin_by_oid(localorigin, true, &origin_name))
				{
					if (tuple_buf.len)
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by origin \"%s\" in transaction %u at %s: %s."),
										 get_rel_name(indexoid), origin_name,
										 localxmin, timestamptz_to_str(localts),
										 tuple_buf.data);
					else
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by origin \"%s\" in transaction %u at %s."),
										 get_rel_name(indexoid), origin_name,
										 localxmin, timestamptz_to_str(localts));
				}

				/*
				 * The origin that modified this row has been removed. This
				 * can happen if the origin was created by a different apply
				 * worker and its associated subscription and origin were
				 * dropped after updating the row, or if the origin was
				 * manually dropped by the user.
				 */
				else
				{
					if (tuple_buf.len)
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by a non-existent origin in transaction %u at %s: %s."),
										 get_rel_name(indexoid),
										 localxmin, timestamptz_to_str(localts),
										 tuple_buf.data);
					else
						appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified by a non-existent origin in transaction %u at %s."),
										 get_rel_name(indexoid),
										 localxmin, timestamptz_to_str(localts));
				}
			}
			else
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified in transaction %u: %s."),
									 get_rel_name(indexoid), localxmin,
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Key already exists in unique index \"%s\", modified in transaction %u."),
									 get_rel_name(indexoid), localxmin);
			}

			break;

		case CT_UPDATE_ORIGIN_DIFFERS:
			append_tuple_value_detail(&tuple_buf,
									  list_make3(local_desc, remote_desc,
												 search_desc));

			if (localorigin == InvalidReplOriginId)
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Updating the row that was modified locally in transaction %u at %s: %s."),
									 localxmin, timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Updating the row that was modified locally in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}
			else if (replorigin_by_oid(localorigin, true, &origin_name))
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Updating the row that was modified by a different origin \"%s\" in transaction %u at %s: %s."),
									 origin_name, localxmin,
									 timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Updating the row that was modified by a different origin \"%s\" in transaction %u at %s."),
									 origin_name, localxmin,
									 timestamptz_to_str(localts));
			}

			/* The origin that modified this row has been removed. */
			else
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Updating the row that was modified by a non-existent origin in transaction %u at %s: %s."),
									 localxmin, timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Updating the row that was modified by a non-existent origin in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}

			break;

		case CT_UPDATE_DELETED:
			append_tuple_value_detail(&tuple_buf,
									  list_make2(remote_desc, search_desc));

			if (tuple_buf.len)
				appendStringInfo(&err_detail, _("Could not find the row to be updated: %s.\n"),
								 tuple_buf.data);
			else
				appendStringInfo(&err_detail, _("Could not find the row to be updated.\n"));

			if (localts)
			{
				if (localorigin == InvalidReplOriginId)
					appendStringInfo(&err_detail, _("The row to be updated was deleted locally in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
				else if (replorigin_by_oid(localorigin, true, &origin_name))
					appendStringInfo(&err_detail, _("The row to be updated was deleted by a different origin \"%s\" in transaction %u at %s."),
									 origin_name, localxmin, timestamptz_to_str(localts));

				/* The origin that modified this row has been removed. */
				else
					appendStringInfo(&err_detail, _("The row to be updated was deleted by a non-existent origin in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}
			else
				appendStringInfoString(&err_detail, _("The row to be updated was deleted."));

			break;

		case CT_UPDATE_MISSING:
			append_tuple_value_detail(&tuple_buf,
									  list_make2(remote_desc, search_desc));

			if (tuple_buf.len)
				appendStringInfo(&err_detail, _("Could not find the row to be updated: %s."),
								 tuple_buf.data);
			else
				appendStringInfo(&err_detail, _("Could not find the row to be updated."));

			break;

		case CT_DELETE_ORIGIN_DIFFERS:
			append_tuple_value_detail(&tuple_buf,
									  list_make3(local_desc, remote_desc,
												 search_desc));

			if (localorigin == InvalidReplOriginId)
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Deleting the row that was modified locally in transaction %u at %s: %s."),
									 localxmin, timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Deleting the row that was modified locally in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}
			else if (replorigin_by_oid(localorigin, true, &origin_name))
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Deleting the row that was modified by a different origin \"%s\" in transaction %u at %s: %s."),
									 origin_name, localxmin,
									 timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Deleting the row that was modified by a different origin \"%s\" in transaction %u at %s."),
									 origin_name, localxmin,
									 timestamptz_to_str(localts));
			}

			/* The origin that modified this row has been removed. */
			else
			{
				if (tuple_buf.len)
					appendStringInfo(&err_detail, _("Deleting the row that was modified by a non-existent origin in transaction %u at %s: %s."),
									 localxmin, timestamptz_to_str(localts),
									 tuple_buf.data);
				else
					appendStringInfo(&err_detail, _("Deleting the row that was modified by a non-existent origin in transaction %u at %s."),
									 localxmin, timestamptz_to_str(localts));
			}

			break;

		case CT_DELETE_MISSING:
			append_tuple_value_detail(&tuple_buf,
									  list_make1(search_desc));

			if (tuple_buf.len)
				appendStringInfo(&err_detail, _("Could not find the row to be deleted: %s."),
								 tuple_buf.data);
			else
				appendStringInfo(&err_detail, _("Could not find the row to be deleted."));

			break;
	}

	Assert(err_detail.len > 0);

	/*
	 * Insert a blank line to visually separate the new detail line from the
	 * existing ones.
	 */
	if (err_msg->len > 0)
		appendStringInfoChar(err_msg, '\n');

	appendStringInfoString(err_msg, err_detail.data);
}

/*
 * Extract conflicting key, local row, remote row, and replica identity
 * columns. Results are set at xxx_desc.
 *
 * If the output is NULL, it indicates that the current user lacks permissions
 * to view the columns involved.
 */
static void
get_tuple_desc(EState *estate, ResultRelInfo *relinfo, ConflictType type,
			   char **key_desc,
			   TupleTableSlot *localslot, char **local_desc,
			   TupleTableSlot *remoteslot, char **remote_desc,
			   TupleTableSlot *searchslot, char **search_desc,
			   Oid indexoid)
{
	Relation	localrel = relinfo->ri_RelationDesc;
	Oid			relid = RelationGetRelid(localrel);
	TupleDesc	tupdesc = RelationGetDescr(localrel);
	char	   *desc = NULL;

	Assert((localslot && local_desc) || (remoteslot && remote_desc) ||
		   (searchslot && search_desc));

	/*
	 * Report the conflicting key values in the case of a unique constraint
	 * violation.
	 */
	if (type == CT_INSERT_EXISTS || type == CT_UPDATE_EXISTS ||
		type == CT_MULTIPLE_UNIQUE_CONFLICTS)
	{
		Assert(OidIsValid(indexoid) && localslot);

		desc = build_index_value_desc(estate, localrel, localslot,
									  indexoid);

		if (desc)
			*key_desc = psprintf(_("key %s"), desc);
	}

	if (localslot)
	{
		/*
		 * The 'modifiedCols' only applies to the new tuple, hence we pass
		 * NULL for the local row.
		 */
		desc = ExecBuildSlotValueDescription(relid, localslot, tupdesc,
											 NULL, 64);

		if (desc)
			*local_desc = psprintf(_("local row %s"), desc);
	}

	if (remoteslot)
	{
		Bitmapset  *modifiedCols;

		/*
		 * Although logical replication doesn't maintain the bitmap for the
		 * columns being inserted, we still use it to create 'modifiedCols'
		 * for consistency with other calls to ExecBuildSlotValueDescription.
		 *
		 * Note that generated columns are formed locally on the subscriber.
		 */
		modifiedCols = bms_union(ExecGetInsertedCols(relinfo, estate),
								 ExecGetUpdatedCols(relinfo, estate));
		desc = ExecBuildSlotValueDescription(relid, remoteslot,
											 tupdesc, modifiedCols,
											 64);

		if (desc)
			*remote_desc = psprintf(_("remote row %s"), desc);
	}

	if (searchslot)
	{
		/*
		 * Note that while index other than replica identity may be used (see
		 * IsIndexUsableForReplicaIdentityFull for details) to find the tuple
		 * when applying update or delete, such an index scan may not result
		 * in a unique tuple and we still compare the complete tuple in such
		 * cases, thus such indexes are not used here.
		 */
		Oid			replica_index = GetRelationIdentityOrPK(localrel);

		Assert(type != CT_INSERT_EXISTS);

		/*
		 * If the table has a valid replica identity index, build the index
		 * key value string. Otherwise, construct the full tuple value for
		 * REPLICA IDENTITY FULL cases.
		 */
		if (OidIsValid(replica_index))
			desc = build_index_value_desc(estate, localrel, searchslot, replica_index);
		else
			desc = ExecBuildSlotValueDescription(relid, searchslot, tupdesc, NULL, 64);

		if (desc)
		{
			if (OidIsValid(replica_index))
				*search_desc = psprintf(_("replica identity %s"), desc);
			else
				*search_desc = psprintf(_("replica identity full %s"), desc);
		}
	}
}

/*
 * Helper function to extract the "raw" index key Datums and their null flags
 * from a TupleTableSlot, given an already open index descriptor.
 * This is the reusable core logic.
 */
static void
build_index_datums_from_slot(EState *estate, Relation localrel,
							 TupleTableSlot *slot,
							 Relation indexDesc, Datum *values,
							 bool *isnull)
{
	TupleTableSlot *tableslot = slot;

	/*
	 * If the slot is a virtual slot, copy it into a heap tuple slot as
	 * FormIndexDatum only works with heap tuple slots.
	 */
	if (TTS_IS_VIRTUAL(slot))
	{
		/* Slot is created within the EState's tuple table */
		tableslot = table_slot_create(localrel, &estate->es_tupleTable);
		tableslot = ExecCopySlot(tableslot, slot);
	}

	/*
	 * Initialize ecxt_scantuple for potential use in FormIndexDatum
	 */
	GetPerTupleExprContext(estate)->ecxt_scantuple = tableslot;

	/* Form the index datums */
	FormIndexDatum(BuildIndexInfo(indexDesc), tableslot, estate, values,
				   isnull);
}

/*
 * Helper functions to construct a string describing the contents of an index
 * entry. See BuildIndexValueDescription for details.
 *
 * The caller must ensure that the index with the OID 'indexoid' is locked so
 * that we can fetch and display the conflicting key value.
 */
static char *
build_index_value_desc(EState *estate, Relation localrel, TupleTableSlot *slot,
					   Oid indexoid)
{
	char	   *index_value;
	Relation	indexDesc;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	if (!slot)
		return NULL;

	Assert(CheckRelationOidLockedByMe(indexoid, RowExclusiveLock, true));

	indexDesc = index_open(indexoid, NoLock);

	build_index_datums_from_slot(estate, localrel, slot, indexDesc, values,
								 isnull);

	index_value = BuildIndexValueDescription(indexDesc, values, isnull);

	index_close(indexDesc, NoLock);

	return index_value;
}

/*
 * tuple_table_slot_to_json_datum
 *
 * Helper function to convert a TupleTableSlot to JSON.
 */
static Datum
tuple_table_slot_to_json_datum(TupleTableSlot *slot)
{
	HeapTuple	tuple;
	Datum		datum;
	Datum		json;

	Assert(slot != NULL);

	tuple = ExecCopySlotHeapTuple(slot);
	datum = heap_copy_tuple_as_datum(tuple, slot->tts_tupleDescriptor);

	json = DirectFunctionCall1(row_to_json, datum);
	heap_freetuple(tuple);

	return json;
}

/*
 * tuple_table_slot_to_indextup_json
 *
 * Fetch replica identity key from the tuple table slot and convert into a
 * JSON datum.
 *
 * The caller must ensure that the index with the OID 'indexid' is locked so
 * that we can safely fetch and construct the replica identity key values.
 */
static Datum
tuple_table_slot_to_indextup_json(EState *estate, Relation localrel,
								  Oid indexid, TupleTableSlot *slot)
{
	Relation	indexDesc;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	Datum		datum;

	Assert(slot != NULL);

	Assert(CheckRelationOidLockedByMe(indexid, RowExclusiveLock, true));

	indexDesc = index_open(indexid, NoLock);

	build_index_datums_from_slot(estate, localrel, slot, indexDesc, values,
								 isnull);
	tupdesc = CreateTupleDescCopy(RelationGetDescr(indexDesc));

	/* Bless the tupdesc so it can be looked up by row_to_json. */
	BlessTupleDesc(tupdesc);

	/* Form the replica identity tuple. */
	tuple = heap_form_tuple(tupdesc, values, isnull);
	datum = heap_copy_tuple_as_datum(tuple, tupdesc);

	heap_freetuple(tuple);
	index_close(indexDesc, NoLock);

	/* Convert to a JSON datum. */
	datum = DirectFunctionCall1(row_to_json, datum);
	FreeTupleDesc(tupdesc);

	return datum;
}

/*
 * build_local_conflicts_tupledesc
 *
 * Build and bless a tuple descriptor for the conflict log table based on the
 * predefined LocalConflictSchema.
 */
static TupleDesc
build_local_conflicts_tupledesc(void)
{
	static TupleDesc cached_tupdesc = NULL;

	if (cached_tupdesc == NULL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

		cached_tupdesc = CreateTemplateTupleDesc(NUM_LOCAL_CONFLICT_ATTRS);

		for (int i = 0; i < NUM_LOCAL_CONFLICT_ATTRS; i++)
			TupleDescInitEntry(cached_tupdesc,
							   (AttrNumber) (i + 1),
							   LocalConflictSchema[i].attname,
							   LocalConflictSchema[i].atttypid,
							   -1, 0);

		TupleDescFinalize(cached_tupdesc);

		/*
		 * Bless once so it can be used as a RECORD type (e.g. for row_to_json
		 * or other record-based operations).
		 */
		BlessTupleDesc(cached_tupdesc);

		MemoryContextSwitchTo(oldcxt);
	}

	return cached_tupdesc;
}

/*
 * Builds the local conflicts JSON array column from the list of
 * ConflictTupleInfo objects.
 *
 * Example output structure:
 * [ { "xid": "1001", "commit_ts": "...", "origin": "...", "tuple": {...} }, ... ]
 */
static Datum
build_local_conflicts_json_array(List *conflicttuples)
{
	Datum	   *json_datum_array;
	Datum		json_array_datum;
	int			num_conflicts;
	int			i = 0;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	TupleDesc	tupdesc;

	Assert(conflicttuples != NIL);

	/* Build local conflicts tuple descriptor. */
	tupdesc = build_local_conflicts_tupledesc();

	num_conflicts = list_length(conflicttuples);
	json_datum_array = palloc_array(Datum, num_conflicts);

	/* Process local conflict tuple list and prepare an array of JSON. */
	foreach_ptr(ConflictTupleInfo, conflicttuple, conflicttuples)
	{
		Datum		values[NUM_LOCAL_CONFLICT_ATTRS] = {0};
		bool		nulls[NUM_LOCAL_CONFLICT_ATTRS] = {0};
		char	   *origin_name = NULL;
		HeapTuple	tuple;
		Datum		datum;
		Datum		json_datum;
		int			attno;

		attno = 0;
		if (TransactionIdIsValid(conflicttuple->xmin))
			values[attno++] = TransactionIdGetDatum(conflicttuple->xmin);
		else
			nulls[attno++] = true;

		if (conflicttuple->ts)
			values[attno++] = TimestampTzGetDatum(conflicttuple->ts);
		else
			nulls[attno++] = true;

		if (conflicttuple->origin != InvalidReplOriginId)
			replorigin_by_oid(conflicttuple->origin, true, &origin_name);

		/*
		 * Set NULL if origin name for the tuple is InvalidReplOriginId or not
		 * found.
		 */
		if (origin_name != NULL)
			values[attno++] = CStringGetTextDatum(origin_name);
		else
			nulls[attno++] = true;

		/* Convert conflicting tuple to JSON datum. */
		if (conflicttuple->slot)
			values[attno] = tuple_table_slot_to_json_datum(conflicttuple->slot);
		else
			nulls[attno] = true;

		Assert(attno + 1 == NUM_LOCAL_CONFLICT_ATTRS);

		tuple = heap_form_tuple(tupdesc, values, nulls);

		datum = heap_copy_tuple_as_datum(tuple, tupdesc);

		/*
		 * Build the higher level JSON datum in format described in function
		 * header.
		 */
		json_datum = DirectFunctionCall1(row_to_json, datum);

		/* Done with the temporary tuple. */
		heap_freetuple(tuple);

		/* Add to the array element. */
		json_datum_array[i++] = json_datum;
	}

	Assert(i == num_conflicts);

	/* Construct the JSON array Datum. */
	get_typlenbyvalalign(JSONOID, &typlen, &typbyval, &typalign);
	json_array_datum = PointerGetDatum(construct_array(json_datum_array,
													   num_conflicts,
													   JSONOID,
													   typlen,
													   typbyval,
													   typalign));
	pfree(json_datum_array);

	return json_array_datum;
}

/*
 * insert_conflict_log_tuple
 *
 * Prepares and inserts a tuple detailing a conflict encountered during
 * logical replication into the conflict log table.
 */
static void
insert_conflict_log_tuple(EState *estate, Relation rel,
						  Relation conflictlogrel,
						  ConflictType conflict_type,
						  TupleTableSlot *searchslot,
						  List *conflicttuples,
						  TupleTableSlot *remoteslot)
{
	Datum		values[NUM_CONFLICT_ATTRS] = {0};
	bool		nulls[NUM_CONFLICT_ATTRS] = {0};
	int			attno;
	char	   *remote_origin = NULL;
	TransactionId remote_xid;
	XLogRecPtr	remote_final_lsn;
	TimestampTz remote_commit_ts;
	HeapTuple	tuple;

	Assert(conflictlogrel != NULL);

	/* Fetch the information of the remote transaction being applied. */
	GetRemoteTransactionInfoForConflict(&remote_xid, &remote_final_lsn,
										&remote_commit_ts);

	/* Populate the values and nulls arrays. */
	attno = 0;
	values[attno++] = ObjectIdGetDatum(RelationGetRelid(rel));

	values[attno++] =
		CStringGetTextDatum(get_namespace_name(RelationGetNamespace(rel)));

	values[attno++] = CStringGetTextDatum(RelationGetRelationName(rel));

	values[attno++] = CStringGetTextDatum(ConflictTypeNames[conflict_type]);

	if (TransactionIdIsValid(remote_xid))
		values[attno++] = TransactionIdGetDatum(remote_xid);
	else
		nulls[attno++] = true;

	if (XLogRecPtrIsValid(remote_final_lsn))
		values[attno++] = LSNGetDatum(remote_final_lsn);
	else
		nulls[attno++] = true;

	if (remote_commit_ts > 0)
		values[attno++] = TimestampTzGetDatum(remote_commit_ts);
	else
		nulls[attno++] = true;

	if (replorigin_xact_state.origin != InvalidReplOriginId)
		replorigin_by_oid(replorigin_xact_state.origin, true, &remote_origin);

	if (remote_origin != NULL)
		values[attno++] = CStringGetTextDatum(remote_origin);
	else
		nulls[attno++] = true;

	if (!TupIsNull(searchslot))
	{
		Oid			replica_index = GetRelationIdentityOrPK(rel);

		/*
		 * If the table has a valid replica identity index, build the index
		 * JSON datum from key value. Otherwise, construct it from the
		 * complete tuple in REPLICA IDENTITY FULL cases.
		 */
		if (OidIsValid(replica_index))
		{
			values[attno++] = BoolGetDatum(false);
			values[attno++] = tuple_table_slot_to_indextup_json(estate, rel,
																replica_index,
																searchslot);
		}
		else
		{
			values[attno++] = BoolGetDatum(true);
			values[attno++] = tuple_table_slot_to_json_datum(searchslot);
		}
	}
	else
	{
		nulls[attno++] = true;
		nulls[attno++] = true;
	}

	if (!TupIsNull(remoteslot))
		values[attno++] = tuple_table_slot_to_json_datum(remoteslot);
	else
		nulls[attno++] = true;

	/*
	 * In update_missing and delete_missing conflicts, there are no local
	 * conflicting rows, so set local_conflicts to NULL.
	 */
	if (conflicttuples != NIL)
		values[attno] = build_local_conflicts_json_array(conflicttuples);
	else
		nulls[attno] = true;

	Assert(attno + 1 == NUM_CONFLICT_ATTRS);

	tuple = heap_form_tuple(RelationGetDescr(conflictlogrel), values, nulls);

	heap_insert(conflictlogrel, tuple,
				GetCurrentCommandId(true), HEAP_INSERT_NO_LOGICAL, NULL);

	heap_freetuple(tuple);
}
