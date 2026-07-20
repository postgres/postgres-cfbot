/*
 * ext_vacuum_statistics - Extended vacuum statistics for PostgreSQL
 *
 * This module collects detailed vacuum statistics (I/O, WAL, timing, etc.)
 * at relation and database level by hooking into the vacuum reporting path.
 * Statistics are stored via pgstat custom statistics. Management of statistics
 * storage and output functions are implemented in this module.
 */
#include "postgres.h"

#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/pgstat_internal.h"
#include "utils/pgstat_kind.h"
#include "utils/tuplestore.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* Custom stats kind for per-relation vacuum statistics */
#define PGSTAT_KIND_EXTVAC_RELATION	24

#define SJ_NODENAME		"vacuum_statistics"

/*  GUCs  */
static bool evs_enabled = true;

/*  Hooks  */
static set_report_vacuum_hook_type prev_report_vacuum_hook = NULL;
static object_access_hook_type prev_object_access_hook = NULL;

/*  Forward declarations  */
static void pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
										  PgStat_VacuumRelationCounts * params);
static void extvac_object_access(ObjectAccessType access, Oid classId,
								 Oid objectId, int subId, void *arg);

/* Shared memory entry for vacuum stats; one per relation or database. */
typedef struct PgStatShared_ExtVacEntry
{
	PgStatShared_Common header;
	PgStat_VacuumRelationCounts stats;
}			PgStatShared_ExtVacEntry;

/* PgStat kind for per-relation vacuum statistics (tables/indexes) */
static const PgStat_KindInfo extvac_relation_kind_info = {
	.name = "ext_vacuum_statistics_relation",
	.fixed_amount = false,
	.accessed_across_databases = true,
	.write_to_file = true,
	.shared_size = sizeof(PgStatShared_ExtVacEntry),
	.shared_data_off = offsetof(PgStatShared_ExtVacEntry, stats),
	.shared_data_len = sizeof(PgStat_VacuumRelationCounts),
	.pending_size = 0,
	.flush_pending_cb = NULL,
};

static inline void
pgstat_accumulate_common(PgStat_CommonCounts * dst, const PgStat_CommonCounts * src)
{
	dst->tuples_deleted += src->tuples_deleted;
}

static inline void
pgstat_accumulate_extvac_stats(PgStat_VacuumRelationCounts * dst,
							   const PgStat_VacuumRelationCounts * src)
{
	if (dst->type == PGSTAT_EXTVAC_INVALID)
		dst->type = src->type;

	Assert(src->type != PGSTAT_EXTVAC_INVALID && src->type != PGSTAT_EXTVAC_DB);
	Assert(src->type == dst->type);

	pgstat_accumulate_common(&dst->common, &src->common);

	if (dst->type == PGSTAT_EXTVAC_TABLE)
	{
		dst->table.tuples_frozen += src->table.tuples_frozen;
		dst->table.recently_dead_tuples += src->table.recently_dead_tuples;
		dst->table.missed_dead_tuples += src->table.missed_dead_tuples;
	}
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ext_vacuum_statistics module could be loaded only on startup."),
				 errdetail("Add 'ext_vacuum_statistics' into the shared_preload_libraries list.")));

	DefineCustomBoolVariable("vacuum_statistics.enabled",
							 "Enable extended vacuum statistics collection.",
							 NULL, &evs_enabled, true,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	MarkGUCPrefixReserved(SJ_NODENAME);

	pgstat_register_kind(PGSTAT_KIND_EXTVAC_RELATION, &extvac_relation_kind_info);

	prev_report_vacuum_hook = set_report_vacuum_hook;
	set_report_vacuum_hook = pgstat_report_vacuum_extstats;

	prev_object_access_hook = object_access_hook;
	object_access_hook = extvac_object_access;
}

/*
 * Object access hook: drop the statistics of a dropped relation, and reset
 * old statistics when a new relation is created.
 *
 * Otherwise the statistics of a dropped relation stay in memory and in the
 * statistics file, and a new relation with the same OID gets them.  Both
 * actions are transactional, as for the built-in relation statistics.
 *
 * Statistics are keyed by the relation OID, and shared catalogs are never
 * created or dropped after initdb, so we do not need to read pg_class.
 *
 * Dropped databases need no handling: dropping a database drops all its
 * statistics entries, ours included.
 */
static void
extvac_object_access(ObjectAccessType access, Oid classId, Oid objectId,
					 int subId, void *arg)
{
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	/* only whole relations are of interest, not their columns */
	if (classId != RelationRelationId || subId != 0)
		return;

	if (access == OAT_DROP)
		pgstat_drop_transactional(PGSTAT_KIND_EXTVAC_RELATION, MyDatabaseId,
								  objectId);
	else if (access == OAT_POST_CREATE)
	{
		/*
		 * Discard whatever an earlier owner of this OID left behind, and
		 * arrange for the entry to be dropped again should the creating
		 * transaction roll back.  This mirrors what pgstat_create_relation()
		 * does for the built-in relation statistics.
		 */
		pgstat_create_transactional(PGSTAT_KIND_EXTVAC_RELATION, MyDatabaseId,
									objectId);
	}
}

/*
 * Store incoming vacuum stats into a per-relation pgstat custom statistics
 * entry.  Uses pgstat_get_entry_ref_locked and pgstat_accumulate_* for
 * atomic updates.
 */
static void
extvac_store(Oid dboid, Oid relid, PgStat_VacuumRelationCounts * params)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_ExtVacEntry *shared;

	if (!evs_enabled)
		return;

	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_EXTVAC_RELATION, dboid, relid, false);
	if (entry_ref)
	{
		shared = (PgStatShared_ExtVacEntry *) entry_ref->shared_stats;
		if (shared->stats.type == PGSTAT_EXTVAC_INVALID)
		{
			memset(&shared->stats, 0, sizeof(shared->stats));
			shared->stats.type = params->type;
		}
		pgstat_accumulate_extvac_stats(&shared->stats, params);
		pgstat_unlock_entry(entry_ref);
	}
}

/*
 * Vacuum report hook: called when vacuum finishes. Stores stats per-relation,
 * then chains to previous hook.
 */
static void
pgstat_report_vacuum_extstats(Oid tableoid, bool shared,
							  PgStat_VacuumRelationCounts * params)
{
	Oid			dboid = shared ? InvalidOid : MyDatabaseId;

	if (evs_enabled)
		extvac_store(dboid, tableoid, params);
	if (prev_report_vacuum_hook)
		prev_report_vacuum_hook(tableoid, shared, params);
}

/* Reset statistics for a single relation entry. */
static bool
extvac_reset_by_relid(Oid dboid, Oid relid)
{
	pgstat_reset_entry(PGSTAT_KIND_EXTVAC_RELATION, dboid, relid, 0);
	return true;
}

/* Reset all vacuum statistics. */
static int64
extvac_stat_reset(void)
{
	pgstat_reset_of_kind(PGSTAT_KIND_EXTVAC_RELATION);
	return 0;					/* count not available */
}

PG_FUNCTION_INFO_V1(vacuum_statistics_reset);
PG_FUNCTION_INFO_V1(extvac_reset_entry);

Datum
vacuum_statistics_reset(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(extvac_stat_reset());
}

Datum
extvac_reset_entry(PG_FUNCTION_ARGS)
{
	Oid			dboid = PG_GETARG_OID(0);
	Oid			relid = PG_GETARG_OID(1);

	PG_RETURN_BOOL(extvac_reset_by_relid(dboid, relid));
}

/*
 * Output vacuum statistics (tables or indexes).
 */
#define EXTVAC_HEAP_STAT_COLS	5
#define EXTVAC_IDX_STAT_COLS	2
#define EXTVAC_MAX_STAT_COLS	Max(EXTVAC_HEAP_STAT_COLS, EXTVAC_IDX_STAT_COLS)

static void
tuplestore_put_for_relation(Oid relid, Tuplestorestate *tupstore,
							TupleDesc tupdesc, PgStat_VacuumRelationCounts * vacuum_ext)
{
	Datum		values[EXTVAC_MAX_STAT_COLS];
	bool		nulls[EXTVAC_MAX_STAT_COLS];
	int			i = 0;

	memset(nulls, 0, sizeof(nulls));
	values[i++] = ObjectIdGetDatum(relid);

	if (vacuum_ext->type == PGSTAT_EXTVAC_TABLE)
	{
		values[i++] = Int64GetDatum(vacuum_ext->common.tuples_deleted);
		values[i++] = Int64GetDatum(vacuum_ext->table.tuples_frozen);
		values[i++] = Int64GetDatum(vacuum_ext->table.recently_dead_tuples);
		values[i++] = Int64GetDatum(vacuum_ext->table.missed_dead_tuples);
	}
	else if (vacuum_ext->type == PGSTAT_EXTVAC_INDEX)
	{
		values[i++] = Int64GetDatum(vacuum_ext->common.tuples_deleted);
	}

	Assert(i == ((vacuum_ext->type == PGSTAT_EXTVAC_TABLE) ? EXTVAC_HEAP_STAT_COLS : EXTVAC_IDX_STAT_COLS));
	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
}

static Datum
pg_stats_vacuum(FunctionCallInfo fcinfo, int type)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	Oid			dbid = PG_GETARG_OID(0);

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ext_vacuum_statistics: set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ext_vacuum_statistics: materialize mode required")));

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "ext_vacuum_statistics: return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (type == PGSTAT_EXTVAC_INDEX || type == PGSTAT_EXTVAC_TABLE)
	{
		Oid			relid = PG_GETARG_OID(1);
		PgStat_VacuumRelationCounts *stats;

		if (!OidIsValid(relid))
			return (Datum) 0;

		stats = (PgStat_VacuumRelationCounts *)
			pgstat_fetch_entry(PGSTAT_KIND_EXTVAC_RELATION, dbid, relid, NULL);

		if (!stats)
			stats = (PgStat_VacuumRelationCounts *)
				pgstat_fetch_entry(PGSTAT_KIND_EXTVAC_RELATION, InvalidOid,
								   relid, NULL);

		if (stats && stats->type == type)
			tuplestore_put_for_relation(relid, tupstore, tupdesc, stats);
	}
	else
		elog(PANIC, "ext_vacuum_statistics: invalid type %d", type);

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(pg_stats_get_vacuum_tables);
PG_FUNCTION_INFO_V1(pg_stats_get_vacuum_indexes);

Datum
pg_stats_get_vacuum_tables(PG_FUNCTION_ARGS)
{
	return pg_stats_vacuum(fcinfo, PGSTAT_EXTVAC_TABLE);
}

Datum
pg_stats_get_vacuum_indexes(PG_FUNCTION_ARGS)
{
	return pg_stats_vacuum(fcinfo, PGSTAT_EXTVAC_INDEX);
}
