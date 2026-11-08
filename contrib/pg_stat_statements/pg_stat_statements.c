/*-------------------------------------------------------------------------
 *
 * pg_stat_statements.c
 *		Track statement planning and execution times as well as resource
 *		usage across a whole database cluster.
 *
 * Execution costs are totaled for each distinct source query, and kept as
 * entries of a custom cumulative statistics kind.  (We track about as many
 * distinct queries as pg_stat_statements.max; see entry_dealloc() for how
 * far the count can exceed it.)
 *
 * Starting in Postgres 9.2, this module normalized query entries.  As of
 * Postgres 14, the normalization is done by the core if compute_query_id is
 * enabled, or optionally by third-party modules.
 *
 * To facilitate presenting entries to users, we create "representative" query
 * strings in which constants are replaced with parameter symbols ($n), to
 * make it clearer what a normalized entry can represent.  These strings are
 * stored in a dynamic shared memory area, whose size is limited by
 * pg_stat_statements.query_text_memory, and are freed with their entries.  A
 * statement whose text does not fit is not tracked.
 *
 * Note about locking issues: the cumulative statistics system manages the
 * hash table and the lifetime of entries.  The kind sets own_hash, so its
 * entries live in a dedicated hash table and DSA area, and its hash table
 * partition locks are not shared with other kinds.  An entry's fields are
 * protected by its lock; executions count in this backend's pending entry,
 * which a flush adds to the shared entry under the lock.  Entries are created
 * one at a time, under pgss->lock held exclusively.  An eviction pass holds
 * pgss->dealloc_lock exclusively, so that passes run one at a time; see
 * entry_dealloc().
 * Query texts are set and freed with the entry lock held exclusively, and
 * read with it held shared.  Lock order: pgss->lock or pgss->dealloc_lock,
 * hash table partition locks, entry locks, query text area locks.
 *
 *
 * Copyright (c) 2008-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_statements/pg_stat_statements.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/pg_authid.h"
#include "common/hashfn.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "lib/dshash.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_stat_statements",
					.version = PG_VERSION
);

/*
 * Custom cumulative statistics kinds, for statement entries and for global
 * statistics.  See https://wiki.postgresql.org/wiki/CustomCumulativeStats.
 */
#define PGSS_STATS_KIND			31
#define PGSS_GLOBAL_STATS_KIND	32

/* XXX: Should USAGE_EXEC reflect execution time and/or buffer usage? */
#define USAGE_EXEC(duration)	(1.0)
#define USAGE_INIT				(1.0)	/* including initial planning */
#define ASSUMED_MEDIAN_INIT		(10.0)	/* initial assumed median usage */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every entry_dealloc */
#define STICKY_DECREASE_FACTOR	(0.50)	/* factor for sticky entries */
#define USAGE_DEALLOC_PERCENT	5	/* free this % of entries at once */
#define MAX_BACKSTOP_SIZE		20000	/* most entries waiting to be freed */
#define IS_STICKY(c)	((c.calls[PGSS_PLAN] + c.calls[PGSS_EXEC]) == 0)

/*
 * Extension version number, for supporting older extension versions' objects
 */
typedef enum pgssVersion
{
	PGSS_V1_0 = 0,
	PGSS_V1_1,
	PGSS_V1_2,
	PGSS_V1_3,
	PGSS_V1_8,
	PGSS_V1_9,
	PGSS_V1_10,
	PGSS_V1_11,
	PGSS_V1_12,
	PGSS_V1_13,
} pgssVersion;

typedef enum pgssStoreKind
{
	PGSS_INVALID = -1,

	/*
	 * PGSS_PLAN and PGSS_EXEC must be respectively 0 and 1 as they're used to
	 * reference the underlying values in the arrays in the Counters struct,
	 * and this order is required in pg_stat_statements_internal().
	 */
	PGSS_PLAN = 0,
	PGSS_EXEC,
} pgssStoreKind;

#define PGSS_NUMKIND (PGSS_EXEC + 1)

/*
 * Hashtable key that defines the identity of a hashtable entry.  We separate
 * queries by user and by database even if they are otherwise identical.
 *
 * If you add a new key to this struct, teach pgss_stats_objid() about it,
 * as entries are identified by the object ID it computes from the key.
 */
typedef struct pgssHashKey
{
	Oid			userid;			/* user OID */
	Oid			dbid;			/* database OID */
	int64		queryid;		/* query identifier */
	bool		toplevel;		/* query executed at top level */
} pgssHashKey;

/*
 * The actual stats counters kept within pgssEntry.
 */
typedef struct Counters
{
	PgStat_Counter calls[PGSS_NUMKIND]; /* # of times planned/executed */
	double		total_time[PGSS_NUMKIND];	/* total planning/execution time,
											 * in msec */
	double		min_time[PGSS_NUMKIND]; /* minimum planning/execution time in
										 * msec since min/max reset */
	double		max_time[PGSS_NUMKIND]; /* maximum planning/execution time in
										 * msec since min/max reset */
	double		mean_time[PGSS_NUMKIND];	/* mean planning/execution time in
											 * msec */
	double		sum_var_time[PGSS_NUMKIND]; /* sum of variances in
											 * planning/execution time in msec */
	PgStat_Counter rows;		/* total # of retrieved or affected rows */
	PgStat_Counter shared_blks_hit; /* # of shared buffer hits */
	PgStat_Counter shared_blks_read;	/* # of shared disk blocks read */
	PgStat_Counter shared_blks_dirtied; /* # of shared disk blocks dirtied */
	PgStat_Counter shared_blks_written; /* # of shared disk blocks written */
	PgStat_Counter local_blks_hit;	/* # of local buffer hits */
	PgStat_Counter local_blks_read; /* # of local disk blocks read */
	PgStat_Counter local_blks_dirtied;	/* # of local disk blocks dirtied */
	PgStat_Counter local_blks_written;	/* # of local disk blocks written */
	PgStat_Counter temp_blks_read;	/* # of temp blocks read */
	PgStat_Counter temp_blks_written;	/* # of temp blocks written */
	double		shared_blk_read_time;	/* time spent reading shared blocks,
										 * in msec */
	double		shared_blk_write_time;	/* time spent writing shared blocks,
										 * in msec */
	double		local_blk_read_time;	/* time spent reading local blocks, in
										 * msec */
	double		local_blk_write_time;	/* time spent writing local blocks, in
										 * msec */
	double		temp_blk_read_time; /* time spent reading temp blocks, in msec */
	double		temp_blk_write_time;	/* time spent writing temp blocks, in
										 * msec */
	PgStat_Counter wal_records; /* # of WAL records generated */
	PgStat_Counter wal_fpi;		/* # of WAL full page images generated */
	uint64		wal_bytes;		/* total amount of WAL generated in bytes */
	PgStat_Counter wal_buffers_full;	/* # of times the WAL buffers became
										 * full */
	PgStat_Counter jit_functions;	/* total number of JIT functions emitted */
	double		jit_generation_time;	/* total time to generate jit code */
	PgStat_Counter jit_inlining_count;	/* number of times inlining time has
										 * been > 0 */
	double		jit_deform_time;	/* total time to deform tuples in jit code */
	PgStat_Counter jit_deform_count;	/* number of times deform time has
										 * been > 0 */

	double		jit_inlining_time;	/* total time to inline jit code */
	PgStat_Counter jit_optimization_count;	/* number of times optimization
											 * time has been > 0 */
	double		jit_optimization_time;	/* total time to optimize jit code */
	PgStat_Counter jit_emission_count;	/* number of times emission time has
										 * been > 0 */
	double		jit_emission_time;	/* total time to emit jit code */
	PgStat_Counter parallel_workers_to_launch;	/* # of parallel workers
												 * planned to be launched */
	PgStat_Counter parallel_workers_launched;	/* # of parallel workers
												 * actually launched */
	PgStat_Counter generic_plan_calls;	/* number of calls using a generic
										 * plan */
	PgStat_Counter custom_plan_calls;	/* number of calls using a custom plan */
} Counters;

/*
 * Global statistics for pg_stat_statements
 */
typedef struct pgssGlobalStats
{
	int64		dealloc;		/* # of times entries were deallocated */
	TimestampTz stats_reset;	/* timestamp with all stats reset */
} pgssGlobalStats;

/*
 * Statistics per statement
 *
 * stats_since is zero while the entry is not live, before entry_alloc() and
 * after entry_remove(); scans skip such entries.
 *
 * An entry restored without its query text, for lack of query text memory,
 * has InvalidDsaPointer as query_text and -1 as query_len.
 *
 * Executions add to usage under the entry lock, or through the pending entry
 * if the lock is busy.  The first usage update sets executed; until then,
 * eviction treats the entry as sticky.
 *
 * A min/max reset cannot reach the pending entries of backends, so it bumps
 * minmax_generation, and pending min/max values of an older generation are
 * discarded instead of merged.  minmax_generation is atomic because
 * executions read it without the entry lock.
 */
typedef struct pgssEntry
{
	pgssHashKey key;			/* hash key of entry - MUST BE FIRST */
	Counters	counters;		/* the statistics for this query */
	double		usage;			/* usage factor */
	bool		executed;		/* executed since creation? */
	dsa_pointer query_text;		/* query text in its query text area */
	int			query_len;		/* # of valid bytes in query string, or -1 */
	int			encoding;		/* query text encoding */
	TimestampTz stats_since;	/* timestamp of entry allocation */
	TimestampTz minmax_stats_since; /* timestamp of last min/max values reset */
	pg_atomic_uint32 minmax_generation; /* bumped by min/max resets */
} pgssEntry;

/*
 * Statement entry in the hash table of the cumulative statistics system
 */
typedef struct pgssSharedEntry
{
	PgStatShared_Common header; /* includes the entry lock */
	pgssEntry	entry;
} pgssSharedEntry;

/*
 * Pending statistics of a statement, which a flush adds to the entry
 */
typedef struct pgssPendingEntry
{
	Counters	counters;
	double		usage;			/* of executions finding the entry locked */
	uint32		minmax_generation;	/* of the entry, as of min/max values */
} pgssPendingEntry;

/*
 * Element of the array of entries sorted by entry_dealloc()
 */
typedef struct pgssDeallocEntry
{
	PgStat_HashKey key;			/* hash key of the entry */
	TimestampTz stats_since;	/* identifies the entry's incarnation */
	double		usage;			/* usage of the entry */
} pgssDeallocEntry;

/*
 * Global shared state
 */
typedef struct pgssSharedState
{
	LWLockPadded lock;			/* serializes entry creation */
	LWLock		dealloc_lock;	/* held exclusively by entry_dealloc() */
	pg_atomic_uint64 num_entries;	/* # of live entries; see entry_remove() */
	slock_t		mutex;			/* protects following fields only: */
	double		cur_median_usage;	/* current median usage in hashtable */
} pgssSharedState;

/*
 * The query text area follows the shared state.  It reserves only its minimum
 * size in the main shared memory segment, and grows in DSM segments.
 */
#define qtext_area_place() \
	((char *) pgss + MAXALIGN(sizeof(pgssSharedState)))

/* Links to shared memory state */
static pgssSharedState *pgss;

static void pgss_shmem_request(void *arg);
static void pgss_shmem_init(void *arg);

static const ShmemCallbacks pgss_shmem_callbacks = {
	.request_fn = pgss_shmem_request,
	.init_fn = pgss_shmem_init,
};

/*---- Local variables ----*/

/* Current nesting depth of planner/ExecutorRun/ProcessUtility calls */
static int	nesting_level = 0;

/* This backend's mapping of the query text area */
static dsa_area *qtext_dsa = NULL;

/* Query texts restored from the statistics file, freed if it is discarded */
static dsa_pointer *pgss_restored_texts = NULL;
static int	pgss_num_restored_texts = 0;

/*
 * State saved in a QueryDesc at ExecutorStart, for use at ExecutorEnd.  A
 * query may end at a different nesting level than the one it started at,
 * for example a cursor declared in a transaction and closed at COMMIT.
 */
typedef struct pgssQueryState
{
	int			nesting_level;	/* nesting level at ExecutorStart */
} pgssQueryState;

/* ID of pg_stat_statements for QueryDesc extension state */
static int	pgss_queryDesc_id = -1;

/* Saved hook values */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/*---- GUC variables ----*/

typedef enum
{
	PGSS_TRACK_NONE,			/* track no statements */
	PGSS_TRACK_TOP,				/* only top level statements */
	PGSS_TRACK_ALL,				/* all statements, including nested ones */
}			PGSSTrackLevel;

static const struct config_enum_entry track_options[] =
{
	{"none", PGSS_TRACK_NONE, false},
	{"top", PGSS_TRACK_TOP, false},
	{"all", PGSS_TRACK_ALL, false},
	{NULL, 0, false}
};

static int	pgss_max = 5000;	/* max # statements to track */
static int	pgss_query_text_memory = 65536; /* max kB of query texts */
static int	qtext_applied_limit = 0;	/* query_text_memory this backend
										 * applied to the area, or 0 */
static int	pgss_track = PGSS_TRACK_TOP;	/* tracking level */
static bool pgss_track_utility = true;	/* whether to track utility commands */
static bool pgss_track_planning = false;	/* whether to track planning
											 * duration */
static bool pgss_save = true;	/* whether to save stats across shutdown */

#define pgss_enabled(level) \
	(!IsParallelWorker() && \
	(pgss_track == PGSS_TRACK_ALL || \
	(pgss_track == PGSS_TRACK_TOP && (level) == 0)))

/*---- Function declarations ----*/

PG_FUNCTION_INFO_V1(pg_stat_statements_reset);
PG_FUNCTION_INFO_V1(pg_stat_statements_reset_1_7);
PG_FUNCTION_INFO_V1(pg_stat_statements_reset_1_11);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_2);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_3);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_8);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_9);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_10);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_11);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_12);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_13);
PG_FUNCTION_INFO_V1(pg_stat_statements);
PG_FUNCTION_INFO_V1(pg_stat_statements_info);

static bool pgss_to_serialized_data_cb(const PgStat_HashKey *key,
									   const PgStatShared_Common *header,
									   FILE *statfile);
static bool pgss_from_serialized_data_cb(const PgStat_HashKey *key,
										 PgStatShared_Common *header,
										 FILE *statfile);
static void pgss_finish_cb(PgStat_StatsFileOp status);
static void pgss_global_stats_init_shmem_cb(void *stats);
static inline pgssGlobalStats *pgss_global_stats(void);
static void pgss_global_stats_reset_all_cb(TimestampTz ts);
static void pgss_global_stats_snapshot_cb(void);
static void pgss_post_parse_analyze(ParseState *pstate, Query *query,
									const JumbleState *jstate);
static PlannedStmt *pgss_planner(Query *parse,
								 const char *query_string,
								 int cursorOptions,
								 ParamListInfo boundParams,
								 ExplainState *es);
static void pgss_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgss_ExecutorRun(QueryDesc *queryDesc,
							 ScanDirection direction,
							 uint64 count);
static void pgss_ExecutorFinish(QueryDesc *queryDesc);
static void pgss_ExecutorEnd(QueryDesc *queryDesc);
static void pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								bool readOnlyTree,
								ProcessUtilityContext context, ParamListInfo params,
								QueryEnvironment *queryEnv,
								DestReceiver *dest, QueryCompletion *qc);
static void pgss_store(const char *query, int64 queryId,
					   int query_location, int query_len,
					   bool toplevel,
					   pgssStoreKind kind,
					   double total_time, uint64 rows,
					   const BufferUsage *bufusage,
					   const WalUsage *walusage,
					   const struct JitInstrumentation *jitusage,
					   const JumbleState *jstate,
					   int parallel_workers_to_launch,
					   int parallel_workers_launched,
					   PlannedStmtOrigin planOrigin);
static void pg_stat_statements_internal(FunctionCallInfo fcinfo,
										pgssVersion api_version,
										bool showtext);
static PgStat_EntryRef *entry_alloc(const pgssHashKey *key,
									const char *query, int query_len,
									int encoding, bool sticky);
static uint64 pgss_stats_objid(const pgssHashKey *key);
static inline pgssSharedEntry *entry_from_hash(PgStatShared_HashEntry *hash_entry);
static void entry_seq_init(dshash_seq_status *hstat);
static pgssSharedEntry *entry_seq_next(dshash_seq_status *hstat,
									   PgStat_HashKey *key);
static void entry_drop(const PgStat_HashKey *key);
static PgStat_EntryRef *entry_find(const pgssHashKey *key);
static void entry_add_usage(pgssEntry *entry, double usage);
static PgStat_FlushResult pgss_flush_pending_cb(PgStat_EntryRef *entry_ref,
												bool nowait, bool xact_boundary);
static void entry_dealloc(void);
static dsa_pointer qtext_store(const char *query, int query_len);
static dsa_area *qtext_area(void);
static const char *qtext_fetch(const pgssEntry *entry);
static bool entry_remove(pgssSharedEntry *shared_entry,
						 PgStat_EntryRef *entry_ref,
						 const pgssDeallocEntry *victim);
static void entry_reset_minmax(Counters *counters);
static void entry_reset_shared_minmax(pgssSharedEntry *shared_entry,
									  TimestampTz stats_reset);
static void entry_reset_single(const pgssHashKey *key, bool minmax_only,
							   TimestampTz stats_reset);
static bool entry_reset_matches(const pgssEntry *entry, const pgssHashKey *key);
static bool entry_reset_drop_matching(PgStatShared_HashEntry *hash_entry,
									  Datum match_data);
static TimestampTz entry_reset(Oid userid, Oid dbid, int64 queryid, bool minmax_only);
static char *generate_normalized_query(const JumbleState *jstate,
									   const char *query,
									   int query_loc, int *query_len_p);

static const PgStat_KindInfo pgss_stats_kind = {
	.name = "pg_stat_statements",
	.write_to_file = true,
	.accessed_across_databases = true,
	.own_hash = true,
	.track_entry_count = true,
	.release_refs_after_flush = true,
	.shared_size = sizeof(pgssSharedEntry),
	.shared_data_off = offsetof(pgssSharedEntry, entry),
	.shared_data_len = sizeof(pgssEntry),
	.pending_size = sizeof(pgssPendingEntry),
	.flush_pending_cb = pgss_flush_pending_cb,
	.to_serialized_data = pgss_to_serialized_data_cb,
	.from_serialized_data = pgss_from_serialized_data_cb,
	.finish = pgss_finish_cb,
};

static const PgStat_KindInfo pgss_global_stats_kind = {
	.name = "pg_stat_statements_info",
	.fixed_amount = true,
	.write_to_file = true,
	.shared_size = sizeof(pgssGlobalStats),
	.shared_data_len = sizeof(pgssGlobalStats),
	.init_shmem_cb = pgss_global_stats_init_shmem_cb,
	.reset_all_cb = pgss_global_stats_reset_all_cb,
	.snapshot_cb = pgss_global_stats_snapshot_cb,
};

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_statements functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Inform the postmaster that we want to enable query_id calculation if
	 * compute_query_id is set to auto.
	 */
	EnableQueryId();

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable("pg_stat_statements.max",
							"Sets the maximum number of statements tracked by pg_stat_statements.",
							NULL,
							&pgss_max,
							5000,
							100,
							INT_MAX / 2,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_stat_statements.query_text_memory",
							"Sets the maximum amount of memory for the query texts of pg_stat_statements.",
							NULL,
							&pgss_query_text_memory,
							65536,
							1024,
							MAX_KILOBYTES,
							PGC_SIGHUP,
							GUC_UNIT_KB,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("pg_stat_statements.track",
							 "Selects which statements are tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track,
							 PGSS_TRACK_TOP,
							 track_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.track_utility",
							 "Selects whether utility commands are tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track_utility,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.track_planning",
							 "Selects whether planning duration is tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track_planning,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.save",
							 "Save pg_stat_statements statistics across server shutdowns.",
							 NULL,
							 &pgss_save,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_stat_statements");

	/*
	 * Register our custom kinds of cumulative statistics.
	 */
	pgstat_register_kind(PGSS_STATS_KIND, &pgss_stats_kind);
	pgstat_register_kind(PGSS_GLOBAL_STATS_KIND, &pgss_global_stats_kind);

	/*
	 * Register our shared memory needs.
	 */
	RegisterShmemCallbacks(&pgss_shmem_callbacks);

	/*
	 * Get an ID to store state in QueryDesc.
	 */
	pgss_queryDesc_id = GetQueryDescExtensionId("pg_stat_statements");

	/*
	 * Install hooks.
	 */
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pgss_post_parse_analyze;
	prev_planner_hook = planner_hook;
	planner_hook = pgss_planner;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgss_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pgss_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pgss_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgss_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pgss_ProcessUtility;
}

/*
 * shmem request callback: Request shared memory resources.
 *
 * This is called at postmaster startup.  Note that the shared memory isn't
 * allocated here yet, this merely register our needs.
 *
 * In EXEC_BACKEND mode, this is also called in each backend, to re-attach to
 * the shared memory area that was already initialized.
 */
static void
pgss_shmem_request(void *arg)
{
	ShmemRequestStruct(.name = "pg_stat_statements",
					   .size = MAXALIGN(sizeof(pgssSharedState)) +
					   dsa_minimum_size(),
					   .ptr = (void **) &pgss,
		);
}

/*
 * shmem init callback: Initialize our shared memory data structures at
 * postmaster startup.
 */
static void
pgss_shmem_init(void *arg)
{
	int			tranche_id;
	dsa_area   *area;

	/*
	 * We already checked that we're loaded from shared_preload_libraries in
	 * _PG_init(), so we should not get here after postmaster startup.
	 */
	Assert(!IsUnderPostmaster);

	/*
	 * Initialize the shmem area with no statistics.
	 */
	tranche_id = LWLockNewTrancheId("pg_stat_statements");
	LWLockInitialize(&pgss->lock.lock, tranche_id);
	tranche_id = LWLockNewTrancheId("pg_stat_statements_dealloc");
	LWLockInitialize(&pgss->dealloc_lock, tranche_id);
	pgss->cur_median_usage = ASSUMED_MEDIAN_INIT;
	SpinLockInit(&pgss->mutex);
	pg_atomic_init_u64(&pgss->num_entries, 0);

	/* Create the query text area; the postmaster does not use it */
	tranche_id = LWLockNewTrancheId("pg_stat_statements_qtext");
	area = dsa_create_in_place(qtext_area_place(), dsa_minimum_size(),
							   tranche_id, NULL);
	dsa_pin(area);
	dsa_set_size_limit(area, (size_t) pgss_query_text_memory * 1024);
	dsa_detach(area);
}

/*
 * Compute the stats object ID of an entry from its key.  Different keys may
 * map to the same ID; like statements whose queryids collide, they then share
 * an entry.
 */
static uint64
pgss_stats_objid(const pgssHashKey *key)
{
	uint64		hash;

	hash = murmurhash64(((uint64) key->userid << 32) | key->dbid);
	hash = hash_combine64(hash, murmurhash64((uint64) key->queryid));
	hash = hash_combine64(hash, (uint64) key->toplevel);

	return hash;
}

/*
 * Return the statement entry of a hash table entry, or NULL if it has been
 * dropped.  Caller must hold the hash table entry's partition lock.
 */
static inline pgssSharedEntry *
entry_from_hash(PgStatShared_HashEntry *hash_entry)
{
	if (hash_entry->dropped)
		return NULL;

	return (pgssSharedEntry *)
		dsa_get_address(pgStatLocal.kind_dsa[PGSS_STATS_KIND],
						hash_entry->body);
}

/*
 * Scan the statement entries, skipping dropped ones.  entry_seq_next()
 * returns NULL at the end, and the hash key of the entry in *key if not NULL.
 * The entry's partition lock is held until the next call.
 *
 * XXX: The cumulative statistics system has no API to iterate over the
 * entries of a kind, so scans use its hash table and DSA area directly, and
 * lock entries without a reference.
 */
static void
entry_seq_init(dshash_seq_status *hstat)
{
	dshash_seq_init(hstat, pgStatLocal.kind_hash[PGSS_STATS_KIND], false);
}

static pgssSharedEntry *
entry_seq_next(dshash_seq_status *hstat, PgStat_HashKey *key)
{
	PgStatShared_HashEntry *hash_entry;

	/* Skip dropped entries */
	do
		hash_entry = dshash_seq_next(hstat);
	while (hash_entry != NULL && hash_entry->dropped);

	if (hash_entry == NULL)
		return NULL;
	if (key)
		*key = hash_entry->key;
	return entry_from_hash(hash_entry);
}

/*
 * Drop an entry, without requesting a garbage collection of references, see
 * release_refs_after_flush.
 */
static void
entry_drop(const PgStat_HashKey *key)
{
	(void) pgstat_drop_entry(key->kind, key->dboid, key->objid, true);
}

/*
 * Return a reference to the entry for a key, or NULL if there is none.  The
 * entry may be evicted after this returns; callers that must not use an
 * evicted entry check under its lock.
 */
static PgStat_EntryRef *
entry_find(const pgssHashKey *key)
{
	return pgstat_get_entry_ref(PGSS_STATS_KIND, InvalidOid,
								pgss_stats_objid(key), false, NULL);
}

/*
 * Write the query text of an entry after it in the statistics file, a length
 * of -2 if it has no text, or of -1 if the entry is not to be restored.  This
 * runs at shutdown, when nothing else uses the entries.
 */
static bool
pgss_to_serialized_data_cb(const PgStat_HashKey *key,
						   const PgStatShared_Common *header, FILE *statfile)
{
	const pgssEntry *entry = &((const pgssSharedEntry *) header)->entry;
	const char *qstr = NULL;
	int			len = -1;

	/* Don't save texts if told not to, nor uninitialized or sticky entries */
	if (pgss_save && entry->stats_since != 0 && !IS_STICKY(entry->counters))
	{
		qstr = qtext_fetch(entry);
		len = qstr ? entry->query_len : -2;
	}

	if (fwrite(&len, sizeof(len), 1, statfile) != 1)
		return false;
	if (len > 0 && fwrite(qstr, 1, len, statfile) != (size_t) len)
		return false;

	return true;
}

/*
 * Read the query text of an entry from the statistics file into the query
 * text area, dropping entries that cannot be restored.  An entry is kept
 * without its text if it was saved without one, or if there is no query text
 * memory left for it.  This runs at startup, when nothing else uses the
 * entries.
 */
static bool
pgss_from_serialized_data_cb(const PgStat_HashKey *key,
							 PgStatShared_Common *header, FILE *statfile)
{
	pgssEntry  *entry = &((pgssSharedEntry *) header)->entry;
	int			len;

	if (fread(&len, sizeof(len), 1, statfile) != 1)
		return false;
	if (len < -2 || (len >= 0 && (Size) len >= MaxAllocSize))
		return false;

	/*
	 * Restore the entry only if it was saved, and looks sane: initialized,
	 * executed, and with a key matching its stats object ID.  Encoding is the
	 * only other field we can easily sanity-check.  A length of -2 is an
	 * entry saved without its text.
	 */
	if (len == -1 || !pgss_save ||
		entry->stats_since == 0 || IS_STICKY(entry->counters) ||
		entry->key.queryid == INT64CONST(0) ||
		!PG_VALID_BE_ENCODING(entry->encoding) ||
		key->dboid != InvalidOid ||
		key->objid != pgss_stats_objid(&entry->key))
	{
		if (len > 0 && fseek(statfile, len, SEEK_CUR) != 0)
			return false;

		/* Drop the entry, keeping the rest of the file */
		entry_drop(key);
		return true;
	}

	/* Store the query text, if any and there is room for it */
	entry->query_text = InvalidDsaPointer;
	entry->query_len = -1;
	if (len >= 0)
	{
		char	   *qstr = palloc(len + 1);

		if (len > 0 && fread(qstr, 1, len, statfile) != (size_t) len)
		{
			pfree(qstr);
			return false;
		}
		entry->query_text = qtext_store(qstr, len);
		pfree(qstr);
	}
	if (DsaPointerIsValid(entry->query_text))
	{
		/* Remember it, to free it if the file is discarded */
		if (pgss_num_restored_texts % 1024 == 0)
			pgss_restored_texts = pgss_restored_texts ?
				repalloc_array(pgss_restored_texts, dsa_pointer,
							   pgss_num_restored_texts + 1024) :
				palloc_array(dsa_pointer, 1024);
		pgss_restored_texts[pgss_num_restored_texts++] = entry->query_text;
		entry->query_len = len;
	}

	pg_atomic_fetch_add_u64(&pgss->num_entries, 1);

	return true;
}

/*
 * Clean up after the statistics file has been written, read or discarded.
 */
static void
pgss_finish_cb(PgStat_StatsFileOp status)
{
	switch (status)
	{
		case STATS_WRITE:
			break;

		case STATS_READ:
			/* Global statistics are restored only if entries are as well */
			if (!pgss_save)
				pgss_global_stats_reset_all_cb(GetCurrentTimestamp());

			/* Deallocate entries beyond pg_stat_statements.max */
			if (pg_atomic_read_u64(&pgss->num_entries) > (uint64) pgss_max)
				entry_dealloc();
			break;

		case STATS_DISCARD:

			/*
			 * After a crash or a corrupted file, all entries have been
			 * dropped, so free the texts restored for them.
			 */
			for (int i = 0; i < pgss_num_restored_texts; i++)
				dsa_free(qtext_area(), pgss_restored_texts[i]);
			pg_atomic_write_u64(&pgss->num_entries, 0);
			break;
	}

	if (pgss_restored_texts)
		pfree(pgss_restored_texts);
	pgss_restored_texts = NULL;
	pgss_num_restored_texts = 0;
}

/*
 * Initialize the global statistics.
 */
static void
pgss_global_stats_init_shmem_cb(void *stats)
{
	pgssGlobalStats *gstats = (pgssGlobalStats *) stats;

	gstats->dealloc = 0;
	gstats->stats_reset = GetCurrentTimestamp();
}

/*
 * Return the global statistics in shared memory, protected by pgss->mutex.
 */
static inline pgssGlobalStats *
pgss_global_stats(void)
{
	return (pgssGlobalStats *)
		pgstat_get_custom_shmem_data(PGSS_GLOBAL_STATS_KIND);
}

/*
 * Reset the global statistics, as of "ts".
 */
static void
pgss_global_stats_reset_all_cb(TimestampTz ts)
{
	pgssGlobalStats *gstats = pgss_global_stats();

	SpinLockAcquire(&pgss->mutex);
	gstats->dealloc = 0;
	gstats->stats_reset = ts;
	SpinLockRelease(&pgss->mutex);
}

/*
 * Copy the global statistics into the cumulative statistics snapshot.
 */
static void
pgss_global_stats_snapshot_cb(void)
{
	pgssGlobalStats *snap = (pgssGlobalStats *)
		pgstat_get_custom_snapshot_data(PGSS_GLOBAL_STATS_KIND);
	pgssGlobalStats *gstats = pgss_global_stats();

	SpinLockAcquire(&pgss->mutex);
	*snap = *gstats;
	SpinLockRelease(&pgss->mutex);
}

/*
 * Post-parse-analysis hook: mark query with a queryId
 */
static void
pgss_post_parse_analyze(ParseState *pstate, Query *query, const JumbleState *jstate)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	/* Safety check... */
	if (!pgss || !pgss_enabled(nesting_level))
		return;

	/*
	 * If it's EXECUTE, clear the queryId so that stats will accumulate for
	 * the underlying PREPARE.  But don't do this if we're not tracking
	 * utility statements, to avoid messing up another extension that might be
	 * tracking them.
	 */
	if (query->utilityStmt)
	{
		if (pgss_track_utility && IsA(query->utilityStmt, ExecuteStmt))
		{
			query->queryId = INT64CONST(0);
			return;
		}
	}

	/*
	 * If query jumbling were able to identify any ignorable constants, we
	 * immediately create a hash table entry for the query, so that we can
	 * record the normalized form of the query string.  If there were no such
	 * constants, the normalized string would be the same as the query text
	 * anyway, so there's no need for an early entry.
	 */
	if (jstate && jstate->clocations_count > 0)
		pgss_store(pstate->p_sourcetext,
				   query->queryId,
				   query->stmt_location,
				   query->stmt_len,
				   nesting_level == 0,
				   PGSS_INVALID,
				   0,
				   0,
				   NULL,
				   NULL,
				   NULL,
				   jstate,
				   0,
				   0,
				   PLAN_STMT_UNKNOWN);
}

/*
 * Planner hook: forward to regular planner, but measure planning time
 * if needed.
 */
static PlannedStmt *
pgss_planner(Query *parse,
			 const char *query_string,
			 int cursorOptions,
			 ParamListInfo boundParams,
			 ExplainState *es)
{
	PlannedStmt *result;

	/*
	 * We can't process the query if no query_string is provided, as
	 * pgss_store needs it.  We also ignore query without queryid, as it would
	 * be treated as a utility statement, which may not be the case.
	 */
	if (pgss_enabled(nesting_level)
		&& pgss_track_planning && query_string
		&& parse->queryId != INT64CONST(0))
	{
		instr_time	start;
		instr_time	duration;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		/* We need to track buffer usage as the planner can access them. */
		bufusage_start = pgBufferUsage;

		/*
		 * Similarly the planner could write some WAL records in some cases
		 * (e.g. setting a hint bit with those being WAL-logged)
		 */
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		nesting_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions,
										   boundParams, es);
			else
				result = standard_planner(parse, query_string, cursorOptions,
										  boundParams, es);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/* calc differences of buffer counters. */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters. */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

		pgss_store(query_string,
				   parse->queryId,
				   parse->stmt_location,
				   parse->stmt_len,
				   nesting_level == 0,
				   PGSS_PLAN,
				   INSTR_TIME_GET_MILLISEC(duration),
				   0,
				   &bufusage,
				   &walusage,
				   NULL,
				   NULL,
				   0,
				   0,
				   result->planOrigin);
	}
	else
	{
		/*
		 * Even though we're not tracking plan time for this statement, we
		 * must still increment the nesting level, to ensure that functions
		 * evaluated during planning are not seen as top-level calls.
		 */
		nesting_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions,
										   boundParams, es);
			else
				result = standard_planner(parse, query_string, cursorOptions,
										  boundParams, es);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();
	}

	return result;
}

/*
 * ExecutorStart hook: start up tracking if needed
 */
static void
pgss_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	pgssQueryState *qstate;

	/*
	 * If query has queryId zero, don't track it.  This prevents double
	 * counting of optimizable statements that are directly contained in
	 * utility statements.
	 */
	if (pgss_enabled(nesting_level) && queryDesc->plannedstmt->queryId != INT64CONST(0))
	{
		/* Request all summary instrumentation, i.e. timing, buffers and WAL */
		queryDesc->query_instr_options |= INSTRUMENT_ALL;

		/* Remember the nesting level for ExecutorEnd */
		qstate = MemoryContextAlloc(GetMemoryChunkContext(queryDesc),
									sizeof(pgssQueryState));
		qstate->nesting_level = nesting_level;
		SetQueryDescExtensionState(queryDesc, pgss_queryDesc_id, qstate);
	}

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorFinish(QueryDesc *queryDesc)
{
	nesting_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		nesting_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: store results if needed
 */
static void
pgss_ExecutorEnd(QueryDesc *queryDesc)
{
	int64		queryId = queryDesc->plannedstmt->queryId;
	pgssQueryState *qstate = GetQueryDescExtensionState(queryDesc,
														pgss_queryDesc_id);

	/*
	 * Use the nesting level saved at ExecutorStart, if available, since the
	 * query may end at a different level (e.g. a cursor closed at COMMIT).
	 */
	if (queryId != INT64CONST(0) && queryDesc->query_instr &&
		qstate != NULL &&
		pgss_enabled(qstate->nesting_level))
	{
		pgss_store(queryDesc->sourceText,
				   queryId,
				   queryDesc->plannedstmt->stmt_location,
				   queryDesc->plannedstmt->stmt_len,
				   qstate->nesting_level == 0,
				   PGSS_EXEC,
				   INSTR_TIME_GET_MILLISEC(queryDesc->query_instr->total),
				   queryDesc->estate->es_total_processed,
				   &queryDesc->query_instr->bufusage,
				   &queryDesc->query_instr->walusage,
				   queryDesc->estate->es_jit ? &queryDesc->estate->es_jit->instr : NULL,
				   NULL,
				   queryDesc->estate->es_parallel_workers_to_launch,
				   queryDesc->estate->es_parallel_workers_launched,
				   queryDesc->plannedstmt->planOrigin);
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ProcessUtility hook
 */
static void
pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree,
					ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	int64		saved_queryId = pstmt->queryId;
	int			saved_stmt_location = pstmt->stmt_location;
	int			saved_stmt_len = pstmt->stmt_len;
	PlannedStmtOrigin saved_planOrigin = pstmt->planOrigin;
	bool		enabled = pgss_track_utility && pgss_enabled(nesting_level);

	/*
	 * Force utility statements to get queryId zero.  We do this even in cases
	 * where the statement contains an optimizable statement for which a
	 * queryId could be derived (such as EXPLAIN or DECLARE CURSOR).  For such
	 * cases, runtime control will first go through ProcessUtility and then
	 * the executor, and we don't want the executor hooks to do anything,
	 * since we are already measuring the statement's costs at the utility
	 * level.
	 *
	 * Note that this is only done if pg_stat_statements is enabled and
	 * configured to track utility statements, in the unlikely possibility
	 * that user configured another extension to handle utility statements
	 * only.
	 */
	if (enabled)
		pstmt->queryId = INT64CONST(0);

	/*
	 * If it's an EXECUTE statement, we don't track it and don't increment the
	 * nesting level.  This allows the cycles to be charged to the underlying
	 * PREPARE instead (by the Executor hooks), which is much more useful.
	 *
	 * We also don't track execution of PREPARE.  If we did, we would get one
	 * hash table entry for the PREPARE (with hash calculated from the query
	 * string), and then a different one with the same query string (but hash
	 * calculated from the query tree) would be used to accumulate costs of
	 * ensuing EXECUTEs.  This would be confusing.  Since PREPARE doesn't
	 * actually run the planner (only parse+rewrite), its costs are generally
	 * pretty negligible and it seems okay to just ignore it.
	 */
	if (enabled &&
		!IsA(parsetree, ExecuteStmt) &&
		!IsA(parsetree, PrepareStmt))
	{
		instr_time	start;
		instr_time	duration;
		uint64		rows;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		bufusage_start = pgBufferUsage;
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		nesting_level++;
		PG_TRY();
		{
			if (prev_ProcessUtility)
				prev_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
			else
				standard_ProcessUtility(pstmt, queryString, readOnlyTree,
										context, params, queryEnv,
										dest, qc);
		}
		PG_FINALLY();
		{
			nesting_level--;
		}
		PG_END_TRY();

		/*
		 * CAUTION: do not access the *pstmt data structure again below here.
		 * If it was a ROLLBACK or similar, that data structure may have been
		 * freed.  We must copy everything we still need into local variables,
		 * which we did above.
		 *
		 * For the same reason, we can't risk restoring pstmt->queryId to its
		 * former value, which'd otherwise be a good idea.
		 */
		pstmt = NULL;

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/*
		 * Track the total number of rows retrieved or affected by the utility
		 * statements of COPY, FETCH, CREATE TABLE AS, CREATE MATERIALIZED
		 * VIEW, REFRESH MATERIALIZED VIEW and SELECT INTO.
		 */
		rows = (qc && (qc->commandTag == CMDTAG_COPY ||
					   qc->commandTag == CMDTAG_FETCH ||
					   qc->commandTag == CMDTAG_SELECT ||
					   qc->commandTag == CMDTAG_REFRESH_MATERIALIZED_VIEW)) ?
			qc->nprocessed : 0;

		/* calc differences of buffer counters. */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters. */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

		pgss_store(queryString,
				   saved_queryId,
				   saved_stmt_location,
				   saved_stmt_len,
				   nesting_level == 0,
				   PGSS_EXEC,
				   INSTR_TIME_GET_MILLISEC(duration),
				   rows,
				   &bufusage,
				   &walusage,
				   NULL,
				   NULL,
				   0,
				   0,
				   saved_planOrigin);
	}
	else
	{
		/*
		 * Even though we're not tracking execution time for this statement,
		 * we must still increment the nesting level, to ensure that functions
		 * evaluated within it are not seen as top-level calls.  But don't do
		 * so for EXECUTE; that way, when control reaches pgss_planner or
		 * pgss_ExecutorStart, we will treat the costs as top-level if
		 * appropriate.  Likewise, don't bump for PREPARE, so that parse
		 * analysis will treat the statement as top-level if appropriate.
		 *
		 * To be absolutely certain we don't mess up the nesting level,
		 * evaluate the bump_level condition just once.
		 */
		bool		bump_level =
			!IsA(parsetree, ExecuteStmt) &&
			!IsA(parsetree, PrepareStmt);

		if (bump_level)
			nesting_level++;
		PG_TRY();
		{
			if (prev_ProcessUtility)
				prev_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
			else
				standard_ProcessUtility(pstmt, queryString, readOnlyTree,
										context, params, queryEnv,
										dest, qc);
		}
		PG_FINALLY();
		{
			if (bump_level)
				nesting_level--;
		}
		PG_END_TRY();
	}
}

/*
 * Store some statistics for a statement.
 *
 * If jstate is not NULL then we're trying to create an entry for which
 * we have no statistics as yet; we just want to record the normalized
 * query string.  total_time, rows, bufusage and walusage are ignored in this
 * case.
 *
 * If kind is PGSS_PLAN or PGSS_EXEC, its value is used as the array position
 * for the arrays in the Counters field.
 */
static void
pgss_store(const char *query, int64 queryId,
		   int query_location, int query_len,
		   bool toplevel,
		   pgssStoreKind kind,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage,
		   const WalUsage *walusage,
		   const struct JitInstrumentation *jitusage,
		   const JumbleState *jstate,
		   int parallel_workers_to_launch,
		   int parallel_workers_launched,
		   PlannedStmtOrigin planOrigin)
{
	pgssHashKey key;
	PgStat_EntryRef *entry_ref;
	pgssSharedEntry *shared_entry;
	pgssPendingEntry *entry;
	char	   *norm_query = NULL;
	int			encoding = GetDatabaseEncoding();

	Assert(query != NULL);

	/* Safety check... */
	if (!pgss)
		return;

	/*
	 * Nothing to do if compute_query_id isn't enabled and no other module
	 * computed a query identifier.
	 */
	if (queryId == INT64CONST(0))
		return;

	/*
	 * Confine our attention to the relevant part of the string, if the query
	 * is a portion of a multi-statement source string, and update query
	 * location and length if needed.
	 */
	query = CleanQuerytext(query, &query_location, &query_len);

	/* Set up key for hashtable search */

	/* clear padding */
	memset(&key, 0, sizeof(pgssHashKey));

	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.queryid = queryId;
	key.toplevel = toplevel;

	/* Lookup the hash table entry */
	entry_ref = entry_find(&key);

	/* Create new entry, if not present */
	if (!entry_ref)
	{
		/*
		 * If as many entries wait to be freed as pg_stat_statements.max, up
		 * to MAX_BACKSTOP_SIZE, don't create a new entry. This is to prevent
		 * unbounded growth of the hash table and should be rare.
		 */
		if (pgstat_get_entry_count(PGSS_STATS_KIND) >=
			(uint64) pgss_max + Min((uint64) pgss_max, MAX_BACKSTOP_SIZE))
			return;

		/* Make space if needed */
		if (pg_atomic_read_u64(&pgss->num_entries) >= (uint64) pgss_max)
			entry_dealloc();

		/*
		 * Create a new, normalized query string if caller asked.  We don't
		 * need to hold the lock while doing this work.  (Note: in any case,
		 * it's possible that someone else creates a duplicate hashtable entry
		 * in the interval where we don't hold the lock below.  That case is
		 * handled by entry_alloc.)
		 */
		if (jstate)
			norm_query = generate_normalized_query(jstate, query,
												   query_location,
												   &query_len);

		/* Need exclusive lock to make a new hashtable entry */
		LWLockAcquire(&pgss->lock.lock, LW_EXCLUSIVE);
		entry_ref = entry_alloc(&key, norm_query ? norm_query : query,
								query_len, encoding, jstate != NULL);
		LWLockRelease(&pgss->lock.lock);

		if (norm_query)
			pfree(norm_query);

		/* Give up if the text did not fit, or the entry was evicted */
		if (!entry_ref)
			return;
	}

	shared_entry = (pgssSharedEntry *) entry_ref->shared_stats;

	/* Increment the counts, except when jstate is not NULL */
	if (!jstate)
	{
		uint32		minmax_generation;

		Assert(kind == PGSS_PLAN || kind == PGSS_EXEC);

		/*
		 * If the entry was evicted since entry_find(), our reference keeps
		 * its memory: the usage goes to the evicted entry, and the flush
		 * discards the counts.
		 */
		pgstat_prep_pending_from_entry_ref(entry_ref);
		entry = (pgssPendingEntry *) entry_ref->pending;

		/* Discard min/max values from before a min/max reset, see pgssEntry */
		minmax_generation = pg_atomic_read_u32(&shared_entry->entry.minmax_generation);
		if (entry->minmax_generation != minmax_generation)
		{
			entry_reset_minmax(&entry->counters);
			entry->minmax_generation = minmax_generation;
		}

		/*
		 * Update the usage under the entry lock, so that eviction passes see
		 * it.  If the lock is busy, the statement is running concurrently, so
		 * hot enough not to be evicted meanwhile: leave its usage to the
		 * flush rather than wait on the lock.
		 */
		if (pgstat_lock_entry(entry_ref, true))
		{
			entry_add_usage(&shared_entry->entry, USAGE_EXEC(total_time));
			pgstat_unlock_entry(entry_ref);
		}
		else
			entry->usage += USAGE_EXEC(total_time);

		entry->counters.calls[kind] += 1;
		entry->counters.total_time[kind] += total_time;

		if (entry->counters.calls[kind] == 1)
		{
			entry->counters.min_time[kind] = total_time;
			entry->counters.max_time[kind] = total_time;
			entry->counters.mean_time[kind] = total_time;
		}
		else
		{
			/*
			 * Welford's method for accurately computing variance. See
			 * <http://www.johndcook.com/blog/standard_deviation/>
			 */
			double		old_mean = entry->counters.mean_time[kind];

			entry->counters.mean_time[kind] +=
				(total_time - old_mean) / entry->counters.calls[kind];
			entry->counters.sum_var_time[kind] +=
				(total_time - old_mean) * (total_time - entry->counters.mean_time[kind]);

			/*
			 * Calculate min and max time. min = 0 and max = 0 means that the
			 * min/max statistics were reset
			 */
			if (entry->counters.min_time[kind] == 0
				&& entry->counters.max_time[kind] == 0)
			{
				entry->counters.min_time[kind] = total_time;
				entry->counters.max_time[kind] = total_time;
			}
			else
			{
				if (entry->counters.min_time[kind] > total_time)
					entry->counters.min_time[kind] = total_time;
				if (entry->counters.max_time[kind] < total_time)
					entry->counters.max_time[kind] = total_time;
			}
		}
		entry->counters.rows += rows;
		entry->counters.shared_blks_hit += bufusage->shared_blks_hit;
		entry->counters.shared_blks_read += bufusage->shared_blks_read;
		entry->counters.shared_blks_dirtied += bufusage->shared_blks_dirtied;
		entry->counters.shared_blks_written += bufusage->shared_blks_written;
		entry->counters.local_blks_hit += bufusage->local_blks_hit;
		entry->counters.local_blks_read += bufusage->local_blks_read;
		entry->counters.local_blks_dirtied += bufusage->local_blks_dirtied;
		entry->counters.local_blks_written += bufusage->local_blks_written;
		entry->counters.temp_blks_read += bufusage->temp_blks_read;
		entry->counters.temp_blks_written += bufusage->temp_blks_written;
		entry->counters.shared_blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->shared_blk_read_time);
		entry->counters.shared_blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->shared_blk_write_time);
		entry->counters.local_blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->local_blk_read_time);
		entry->counters.local_blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->local_blk_write_time);
		entry->counters.temp_blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->temp_blk_read_time);
		entry->counters.temp_blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->temp_blk_write_time);
		entry->counters.wal_records += walusage->wal_records;
		entry->counters.wal_fpi += walusage->wal_fpi;
		entry->counters.wal_bytes += walusage->wal_bytes;
		entry->counters.wal_buffers_full += walusage->wal_buffers_full;
		if (jitusage)
		{
			entry->counters.jit_functions += jitusage->created_functions;
			entry->counters.jit_generation_time += INSTR_TIME_GET_MILLISEC(jitusage->generation_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->deform_counter))
				entry->counters.jit_deform_count++;
			entry->counters.jit_deform_time += INSTR_TIME_GET_MILLISEC(jitusage->deform_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->inlining_counter))
				entry->counters.jit_inlining_count++;
			entry->counters.jit_inlining_time += INSTR_TIME_GET_MILLISEC(jitusage->inlining_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->optimization_counter))
				entry->counters.jit_optimization_count++;
			entry->counters.jit_optimization_time += INSTR_TIME_GET_MILLISEC(jitusage->optimization_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->emission_counter))
				entry->counters.jit_emission_count++;
			entry->counters.jit_emission_time += INSTR_TIME_GET_MILLISEC(jitusage->emission_counter);
		}

		/* parallel worker counters */
		entry->counters.parallel_workers_to_launch += parallel_workers_to_launch;
		entry->counters.parallel_workers_launched += parallel_workers_launched;

		/* plan cache counters */
		if (planOrigin == PLAN_STMT_CACHE_GENERIC)
			entry->counters.generic_plan_calls++;
		else if (planOrigin == PLAN_STMT_CACHE_CUSTOM)
			entry->counters.custom_plan_calls++;
	}
}

/*
 * Reset statement statistics corresponding to userid, dbid, and queryid.
 */
Datum
pg_stat_statements_reset_1_7(PG_FUNCTION_ARGS)
{
	Oid			userid;
	Oid			dbid;
	int64		queryid;

	userid = PG_GETARG_OID(0);
	dbid = PG_GETARG_OID(1);
	queryid = PG_GETARG_INT64(2);

	entry_reset(userid, dbid, queryid, false);

	PG_RETURN_VOID();
}

Datum
pg_stat_statements_reset_1_11(PG_FUNCTION_ARGS)
{
	Oid			userid;
	Oid			dbid;
	int64		queryid;
	bool		minmax_only;

	userid = PG_GETARG_OID(0);
	dbid = PG_GETARG_OID(1);
	queryid = PG_GETARG_INT64(2);
	minmax_only = PG_GETARG_BOOL(3);

	PG_RETURN_TIMESTAMPTZ(entry_reset(userid, dbid, queryid, minmax_only));
}

/*
 * Reset statement statistics.
 */
Datum
pg_stat_statements_reset(PG_FUNCTION_ARGS)
{
	entry_reset(0, 0, 0, false);

	PG_RETURN_VOID();
}

/* Number of output arguments (columns) for various API versions */
#define PG_STAT_STATEMENTS_COLS_V1_0	14
#define PG_STAT_STATEMENTS_COLS_V1_1	18
#define PG_STAT_STATEMENTS_COLS_V1_2	19
#define PG_STAT_STATEMENTS_COLS_V1_3	23
#define PG_STAT_STATEMENTS_COLS_V1_8	32
#define PG_STAT_STATEMENTS_COLS_V1_9	33
#define PG_STAT_STATEMENTS_COLS_V1_10	43
#define PG_STAT_STATEMENTS_COLS_V1_11	49
#define PG_STAT_STATEMENTS_COLS_V1_12	52
#define PG_STAT_STATEMENTS_COLS_V1_13	54
#define PG_STAT_STATEMENTS_COLS			54	/* maximum of above */

/*
 * Retrieve statement statistics.
 *
 * The SQL API of this function has changed multiple times, and will likely
 * do so again in future.  To support the case where a newer version of this
 * loadable module is being used with an old SQL declaration of the function,
 * we continue to support the older API versions.  For 1.2 and later, the
 * expected API version is identified by embedding it in the C name of the
 * function.  Unfortunately we weren't bright enough to do that for 1.1.
 */
Datum
pg_stat_statements_1_13(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_13, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_12(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_12, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_11(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_11, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_10(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_10, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_9(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_9, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_8(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_8, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_3(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_3, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_2(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_2, showtext);

	return (Datum) 0;
}

/*
 * Legacy entry point for pg_stat_statements() API versions 1.0 and 1.1.
 * This can be removed someday, perhaps.
 */
Datum
pg_stat_statements(PG_FUNCTION_ARGS)
{
	/* If it's really API 1.1, we'll figure that out below */
	pg_stat_statements_internal(fcinfo, PGSS_V1_0, true);

	return (Datum) 0;
}

/* Common code for all versions of pg_stat_statements() */
static void
pg_stat_statements_internal(FunctionCallInfo fcinfo,
							pgssVersion api_version,
							bool showtext)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			userid = GetUserId();
	bool		is_allowed_role = false;
	dshash_seq_status hstat;
	pgssSharedEntry *shared_entry;
	pgssEntry  *entries;
	char	  **qstrs;
	int			nentries = 0;
	int			nalloc;

	/*
	 * Superusers or roles with the privileges of pg_read_all_stats members
	 * are allowed
	 */
	is_allowed_role = has_privs_of_role(userid, ROLE_PG_READ_ALL_STATS);

	/* hash table must exist already */
	if (!pgss)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via \"shared_preload_libraries\"")));

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * Check we have the expected number of output arguments.  Aside from
	 * being a good safety check, we need a kluge here to detect API version
	 * 1.1, which was wedged into the code in an ill-considered way.
	 */
	switch (rsinfo->setDesc->natts)
	{
		case PG_STAT_STATEMENTS_COLS_V1_0:
			if (api_version != PGSS_V1_0)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_1:
			/* pg_stat_statements() should have told us 1.0 */
			if (api_version != PGSS_V1_0)
				elog(ERROR, "incorrect number of output arguments");
			api_version = PGSS_V1_1;
			break;
		case PG_STAT_STATEMENTS_COLS_V1_2:
			if (api_version != PGSS_V1_2)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_3:
			if (api_version != PGSS_V1_3)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_8:
			if (api_version != PGSS_V1_8)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_9:
			if (api_version != PGSS_V1_9)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_10:
			if (api_version != PGSS_V1_10)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_11:
			if (api_version != PGSS_V1_11)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_12:
			if (api_version != PGSS_V1_12)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_13:
			if (api_version != PGSS_V1_13)
				elog(ERROR, "incorrect number of output arguments");
			break;
		default:
			elog(ERROR, "incorrect number of output arguments");
	}

	/*
	 * Make this backend's pending statistics visible to this read, as this
	 * flushes the pending statistics of all kinds.  Versions before 1.14 may
	 * run this in a parallel worker, which cannot do that.
	 */
	if (!IsParallelWorker())
		pgstat_force_next_flush();

	/*
	 * Copy the initialized entries, with their query texts if needed, so that
	 * rows are built without holding the hash table partition locks, which
	 * block the creation of entries.
	 */
	nalloc = 64;
	entries = palloc_array(pgssEntry, nalloc);
	qstrs = palloc_array(char *, nalloc);
	entry_seq_init(&hstat);
	while ((shared_entry = entry_seq_next(&hstat, NULL)) != NULL)
	{
		pgssEntry  *entry;
		const char *qstr = NULL;
		bool		keep;

		if (nentries >= nalloc)
		{
			nalloc *= 2;
			entries = repalloc_huge(entries, nalloc * sizeof(pgssEntry));
			qstrs = repalloc_huge(qstrs, nalloc * sizeof(char *));
		}

		LWLockAcquire(&shared_entry->header.lock, LW_SHARED);
		entry = &entries[nentries];
		*entry = shared_entry->entry;

		/*
		 * Skip entry if not initialized yet, or unexecuted (ie, it's a
		 * pending "sticky" entry)
		 */
		keep = entry->stats_since != 0 && !IS_STICKY(entry->counters);
		if (keep && showtext &&
			(is_allowed_role || entry->key.userid == userid))
			qstr = qtext_fetch(entry);
		qstrs[nentries] = qstr ? pnstrdup(qstr, entry->query_len) : NULL;
		LWLockRelease(&shared_entry->header.lock);

		if (keep)
			nentries++;
	}
	dshash_seq_term(&hstat);

	for (int n = 0; n < nentries; n++)
	{
		pgssEntry  *entry = &entries[n];
		Datum		values[PG_STAT_STATEMENTS_COLS];
		bool		nulls[PG_STAT_STATEMENTS_COLS];
		int			i = 0;
		Counters	tmp;
		double		stddev;
		int64		queryid = entry->key.queryid;
		TimestampTz stats_since;
		TimestampTz minmax_stats_since;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		if (api_version >= PGSS_V1_9)
			values[i++] = BoolGetDatum(entry->key.toplevel);

		if (is_allowed_role || entry->key.userid == userid)
		{
			if (api_version >= PGSS_V1_2)
				values[i++] = Int64GetDatumFast(queryid);

			if (showtext)
			{
				char	   *qstr = qstrs[n];

				if (qstr)
				{
					char	   *enc;

					enc = pg_any_to_server(qstr,
										   entry->query_len,
										   entry->encoding);

					values[i++] = CStringGetTextDatum(enc);

					if (enc != qstr)
						pfree(enc);
					pfree(qstr);
				}
				else
				{
					/* Just return a null if we fail to find the text */
					nulls[i++] = true;
				}
			}
			else
			{
				/* Query text not requested */
				nulls[i++] = true;
			}
		}
		else
		{
			/* Don't show queryid */
			if (api_version >= PGSS_V1_2)
				nulls[i++] = true;

			/*
			 * Don't show query text, but hint as to the reason for not doing
			 * so if it was requested
			 */
			if (showtext)
				values[i++] = CStringGetTextDatum("<insufficient privilege>");
			else
				nulls[i++] = true;
		}

		tmp = entry->counters;
		stats_since = entry->stats_since;
		minmax_stats_since = entry->minmax_stats_since;

		/* Note that we rely on PGSS_PLAN being 0 and PGSS_EXEC being 1. */
		for (int kind = 0; kind < PGSS_NUMKIND; kind++)
		{
			if (kind == PGSS_EXEC || api_version >= PGSS_V1_8)
			{
				values[i++] = Int64GetDatumFast(tmp.calls[kind]);
				values[i++] = Float8GetDatumFast(tmp.total_time[kind]);
			}

			if ((kind == PGSS_EXEC && api_version >= PGSS_V1_3) ||
				api_version >= PGSS_V1_8)
			{
				values[i++] = Float8GetDatumFast(tmp.min_time[kind]);
				values[i++] = Float8GetDatumFast(tmp.max_time[kind]);
				values[i++] = Float8GetDatumFast(tmp.mean_time[kind]);

				/*
				 * Note we are calculating the population variance here, not
				 * the sample variance, as we have data for the whole
				 * population, so Bessel's correction is not used, and we
				 * don't divide by tmp.calls - 1.
				 */
				if (tmp.calls[kind] > 1)
					stddev = sqrt(tmp.sum_var_time[kind] / tmp.calls[kind]);
				else
					stddev = 0.0;
				values[i++] = Float8GetDatumFast(stddev);
			}
		}
		values[i++] = Int64GetDatumFast(tmp.rows);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_read);
		if (api_version >= PGSS_V1_1)
			values[i++] = Int64GetDatumFast(tmp.shared_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_written);
		values[i++] = Int64GetDatumFast(tmp.local_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.local_blks_read);
		if (api_version >= PGSS_V1_1)
			values[i++] = Int64GetDatumFast(tmp.local_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.local_blks_written);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_read);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_written);
		if (api_version >= PGSS_V1_1)
		{
			values[i++] = Float8GetDatumFast(tmp.shared_blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.shared_blk_write_time);
		}
		if (api_version >= PGSS_V1_11)
		{
			values[i++] = Float8GetDatumFast(tmp.local_blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.local_blk_write_time);
		}
		if (api_version >= PGSS_V1_10)
		{
			values[i++] = Float8GetDatumFast(tmp.temp_blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.temp_blk_write_time);
		}
		if (api_version >= PGSS_V1_8)
		{
			char		buf[256];
			Datum		wal_bytes;

			values[i++] = Int64GetDatumFast(tmp.wal_records);
			values[i++] = Int64GetDatumFast(tmp.wal_fpi);

			snprintf(buf, sizeof buf, UINT64_FORMAT, tmp.wal_bytes);

			/* Convert to numeric. */
			wal_bytes = DirectFunctionCall3(numeric_in,
											CStringGetDatum(buf),
											ObjectIdGetDatum(0),
											Int32GetDatum(-1));
			values[i++] = wal_bytes;
		}
		if (api_version >= PGSS_V1_12)
		{
			values[i++] = Int64GetDatumFast(tmp.wal_buffers_full);
		}
		if (api_version >= PGSS_V1_10)
		{
			values[i++] = Int64GetDatumFast(tmp.jit_functions);
			values[i++] = Float8GetDatumFast(tmp.jit_generation_time);
			values[i++] = Int64GetDatumFast(tmp.jit_inlining_count);
			values[i++] = Float8GetDatumFast(tmp.jit_inlining_time);
			values[i++] = Int64GetDatumFast(tmp.jit_optimization_count);
			values[i++] = Float8GetDatumFast(tmp.jit_optimization_time);
			values[i++] = Int64GetDatumFast(tmp.jit_emission_count);
			values[i++] = Float8GetDatumFast(tmp.jit_emission_time);
		}
		if (api_version >= PGSS_V1_11)
		{
			values[i++] = Int64GetDatumFast(tmp.jit_deform_count);
			values[i++] = Float8GetDatumFast(tmp.jit_deform_time);
		}
		if (api_version >= PGSS_V1_12)
		{
			values[i++] = Int64GetDatumFast(tmp.parallel_workers_to_launch);
			values[i++] = Int64GetDatumFast(tmp.parallel_workers_launched);
		}
		if (api_version >= PGSS_V1_13)
		{
			values[i++] = Int64GetDatumFast(tmp.generic_plan_calls);
			values[i++] = Int64GetDatumFast(tmp.custom_plan_calls);
		}
		if (api_version >= PGSS_V1_11)
		{
			values[i++] = TimestampTzGetDatum(stats_since);
			values[i++] = TimestampTzGetDatum(minmax_stats_since);
		}

		Assert(i == (api_version == PGSS_V1_0 ? PG_STAT_STATEMENTS_COLS_V1_0 :
					 api_version == PGSS_V1_1 ? PG_STAT_STATEMENTS_COLS_V1_1 :
					 api_version == PGSS_V1_2 ? PG_STAT_STATEMENTS_COLS_V1_2 :
					 api_version == PGSS_V1_3 ? PG_STAT_STATEMENTS_COLS_V1_3 :
					 api_version == PGSS_V1_8 ? PG_STAT_STATEMENTS_COLS_V1_8 :
					 api_version == PGSS_V1_9 ? PG_STAT_STATEMENTS_COLS_V1_9 :
					 api_version == PGSS_V1_10 ? PG_STAT_STATEMENTS_COLS_V1_10 :
					 api_version == PGSS_V1_11 ? PG_STAT_STATEMENTS_COLS_V1_11 :
					 api_version == PGSS_V1_12 ? PG_STAT_STATEMENTS_COLS_V1_12 :
					 api_version == PGSS_V1_13 ? PG_STAT_STATEMENTS_COLS_V1_13 :
					 -1 /* fail if you forget to update this assert */ ));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	pfree(entries);
	pfree(qstrs);
}

/* Number of output arguments (columns) for pg_stat_statements_info */
#define PG_STAT_STATEMENTS_INFO_COLS_V1_9	2
#define PG_STAT_STATEMENTS_INFO_COLS	3

/*
 * Return statistics of pg_stat_statements.
 */
Datum
pg_stat_statements_info(PG_FUNCTION_ARGS)
{
	pgssGlobalStats stats;
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_STATEMENTS_INFO_COLS] = {0};
	bool		nulls[PG_STAT_STATEMENTS_INFO_COLS] = {0};

	if (!pgss)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via \"shared_preload_libraries\"")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/* Read global statistics for pg_stat_statements */
	SpinLockAcquire(&pgss->mutex);
	stats = *pgss_global_stats();
	SpinLockRelease(&pgss->mutex);

	values[0] = Int64GetDatum(stats.dealloc);
	values[1] = TimestampTzGetDatum(stats.stats_reset);

	/* Memory of the query text area, since version 1.14 */
	if (tupdesc->natts > PG_STAT_STATEMENTS_INFO_COLS_V1_9)
		values[2] = Int64GetDatum((int64) dsa_get_total_size(qtext_area()));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Allocate a new hashtable entry, with its key and query text.  If another
 * backend created the entry since we looked it up, return theirs: the caller
 * counts in it, with their text.  Caller must hold pgss->lock exclusively.
 *
 * "query" need not be null-terminated; we rely on query_len instead
 *
 * If "sticky" is true, make the new entry artificially sticky so that it will
 * probably still be there when the query finishes execution.  We do this by
 * giving it a median usage value rather than the normal value.  (Strictly
 * speaking, query strings are normalized on a best effort basis, though it
 * would be difficult to demonstrate this even under artificial conditions.)
 *
 * Returns NULL if the query text does not fit in query text memory, as the
 * statement is then not tracked, or if the entry was evicted meanwhile.
 *
 * XXX: The cumulative statistics system makes a new entry visible to other
 * backends, zeroed, before its creator can initialize it.  A way to
 * initialize an entry's data atomically with its insertion, for example a
 * callback of PgStat_KindInfo called by pgstat_get_entry_ref() while the hash
 * table partition lock is held, would make that window go away.
 */
static PgStat_EntryRef *
entry_alloc(const pgssHashKey *key, const char *query, int query_len,
			int encoding, bool sticky)
{
	PgStat_EntryRef *entry_ref;
	pgssEntry  *entry;
	dsa_pointer query_text;
	bool		created;

	entry_ref = pgstat_get_entry_ref(PGSS_STATS_KIND, InvalidOid,
									 pgss_stats_objid(key), true, &created);
	if (!created)
		return entry_ref;
	entry = &((pgssSharedEntry *) entry_ref->shared_stats)->entry;

	/* Store the text once the entry exists, not to leak it on an error */
	query_text = qtext_store(query, query_len);
	if (!DsaPointerIsValid(query_text))
	{
		entry_drop(&entry_ref->shared_entry->key);
		return NULL;
	}

	pgstat_lock_entry(entry_ref, false);

	if (pgstat_entry_ref_is_stale(entry_ref))
	{
		pgstat_unlock_entry(entry_ref);
		dsa_free(qtext_area(), query_text);
		return NULL;
	}
	entry->key = *key;

	/* Set the initial usage, unless concurrent executions set it already */
	if (!entry->executed && sticky)
	{
		SpinLockAcquire(&pgss->mutex);
		entry->usage = pgss->cur_median_usage;
		SpinLockRelease(&pgss->mutex);
	}
	else if (!entry->executed)
		entry->usage = USAGE_INIT;
	/* ... and don't forget the query text metadata */
	entry->query_text = query_text;
	entry->query_len = query_len;
	entry->encoding = encoding;

	/*
	 * Count the entry before marking it live, as entry_remove() only
	 * decrements the count for live entries.
	 */
	pg_atomic_fetch_add_u64(&pgss->num_entries, 1);

	entry->stats_since = GetCurrentTimestamp();
	entry->minmax_stats_since = entry->stats_since;

	pgstat_unlock_entry(entry_ref);

	return entry_ref;
}

/*
 * Add executions to the usage of an entry, "unsticking" it at its first
 * execution.  Caller must hold the entry lock.
 */
static void
entry_add_usage(pgssEntry *entry, double usage)
{
	entry->usage = (entry->executed ? entry->usage : USAGE_INIT) + usage;
	entry->executed = true;
}

/*
 * Add pending statistics to the entry.
 */
static PgStat_FlushResult
pgss_flush_pending_cb(PgStat_EntryRef *entry_ref, bool nowait, bool xact_boundary)
{
	pgssSharedEntry *shared_entry = (pgssSharedEntry *) entry_ref->shared_stats;
	pgssPendingEntry *pending = (pgssPendingEntry *) entry_ref->pending;
	Counters   *dst = &shared_entry->entry.counters;
	Counters   *src = &pending->counters;
	bool		merge_minmax;

	/*
	 * Discard the statistics of an evicted entry; the reference is released
	 * after the flush.
	 */
	if (pgstat_entry_ref_is_stale(entry_ref))
	{
		memset(pending, 0, sizeof(pgssPendingEntry));
		return PGSTAT_FLUSH_DONE;
	}

	if (!pgstat_lock_entry(entry_ref, nowait))
		return PGSTAT_FLUSH_LOCK_CONFLICT;

	merge_minmax = pending->minmax_generation ==
		pg_atomic_read_u32(&shared_entry->entry.minmax_generation);

	if (pending->usage > 0)
		entry_add_usage(&shared_entry->entry, pending->usage);

	for (int kind = 0; kind < PGSS_NUMKIND; kind++)
	{
		PgStat_Counter n1 = dst->calls[kind];
		PgStat_Counter n2 = src->calls[kind];
		double		delta;

		if (n2 == 0)
			continue;

		/* Combine means and variances of both sets of calls */
		delta = src->mean_time[kind] - dst->mean_time[kind];
		dst->calls[kind] = n1 + n2;
		dst->mean_time[kind] += delta * n2 / (n1 + n2);
		dst->sum_var_time[kind] += src->sum_var_time[kind] +
			delta * delta * n1 * n2 / (n1 + n2);
		dst->total_time[kind] += src->total_time[kind];

		/* min = 0 and max = 0 means that the values were reset */
		if (merge_minmax && (src->min_time[kind] != 0 || src->max_time[kind] != 0))
		{
			if (dst->min_time[kind] == 0 && dst->max_time[kind] == 0)
			{
				dst->min_time[kind] = src->min_time[kind];
				dst->max_time[kind] = src->max_time[kind];
			}
			else
			{
				dst->min_time[kind] = Min(dst->min_time[kind], src->min_time[kind]);
				dst->max_time[kind] = Max(dst->max_time[kind], src->max_time[kind]);
			}
		}
	}

	dst->rows += src->rows;
	dst->shared_blks_hit += src->shared_blks_hit;
	dst->shared_blks_read += src->shared_blks_read;
	dst->shared_blks_dirtied += src->shared_blks_dirtied;
	dst->shared_blks_written += src->shared_blks_written;
	dst->local_blks_hit += src->local_blks_hit;
	dst->local_blks_read += src->local_blks_read;
	dst->local_blks_dirtied += src->local_blks_dirtied;
	dst->local_blks_written += src->local_blks_written;
	dst->temp_blks_read += src->temp_blks_read;
	dst->temp_blks_written += src->temp_blks_written;
	dst->shared_blk_read_time += src->shared_blk_read_time;
	dst->shared_blk_write_time += src->shared_blk_write_time;
	dst->local_blk_read_time += src->local_blk_read_time;
	dst->local_blk_write_time += src->local_blk_write_time;
	dst->temp_blk_read_time += src->temp_blk_read_time;
	dst->temp_blk_write_time += src->temp_blk_write_time;
	dst->wal_records += src->wal_records;
	dst->wal_fpi += src->wal_fpi;
	dst->wal_bytes += src->wal_bytes;
	dst->wal_buffers_full += src->wal_buffers_full;
	dst->jit_functions += src->jit_functions;
	dst->jit_generation_time += src->jit_generation_time;
	dst->jit_inlining_count += src->jit_inlining_count;
	dst->jit_deform_time += src->jit_deform_time;
	dst->jit_deform_count += src->jit_deform_count;
	dst->jit_inlining_time += src->jit_inlining_time;
	dst->jit_optimization_count += src->jit_optimization_count;
	dst->jit_optimization_time += src->jit_optimization_time;
	dst->jit_emission_count += src->jit_emission_count;
	dst->jit_emission_time += src->jit_emission_time;
	dst->parallel_workers_to_launch += src->parallel_workers_to_launch;
	dst->parallel_workers_launched += src->parallel_workers_launched;
	dst->generic_plan_calls += src->generic_plan_calls;
	dst->custom_plan_calls += src->custom_plan_calls;

	pgstat_unlock_entry(entry_ref);

	/* The pending entry may be kept for reuse; don't add it twice */
	memset(pending, 0, sizeof(pgssPendingEntry));

	return PGSTAT_FLUSH_DONE;
}

/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = ((const pgssDeallocEntry *) lhs)->usage;
	double		r_usage = ((const pgssDeallocEntry *) rhs)->usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}

/*
 * Deallocate least-used entries, plus any excess over pg_stat_statements.max,
 * once there are that many entries.
 *
 * Passes run one at a time, under pgss->dealloc_lock, which new entries
 * don't need: they wait for a pass only at the limit.  If another backend is
 * running one, return at once, or once that pass is done if entries exceed
 * the maximum by USAGE_DEALLOC_PERCENT percent, the excess a pass removes.
 */
static void
entry_dealloc(void)
{
	dshash_seq_status hstat;
	pgssSharedEntry *shared_entry;
	PgStat_HashKey key;
	pgssDeallocEntry *entries;
	int			nalloc;
	uint64		num_entries;
	int			nvictims;
	int			i;
	pgssGlobalStats *gstats;
	uint64		max = (uint64) pgss_max;
	bool		wait;

	/* At the limit, wait for the running pass instead of giving up */
	wait = pg_atomic_read_u64(&pgss->num_entries) >=
		max + Max(max * USAGE_DEALLOC_PERCENT / 100, 1);
	if (wait ? !LWLockAcquireOrWait(&pgss->dealloc_lock, LW_EXCLUSIVE) :
		!LWLockConditionalAcquire(&pgss->dealloc_lock, LW_EXCLUSIVE))
		return;

	/* Someone else may have deallocated entries meanwhile */
	num_entries = pg_atomic_read_u64(&pgss->num_entries);
	if (num_entries < max)
	{
		LWLockRelease(&pgss->dealloc_lock);
		return;
	}

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values.  Note that the new cur_median_usage includes the entries we're
	 * about to zap.
	 *
	 * Entries whose lock is busy are in use, so skip them rather than wait;
	 * they miss this pass's decay.  Entries may be initialized meanwhile, so
	 * grow the array as needed.
	 */
	nalloc = (int) Min(Max(num_entries, 64),
					   (uint64) MaxAllocHugeSize / sizeof(pgssDeallocEntry));
	entries = palloc_extended(nalloc * sizeof(pgssDeallocEntry), MCXT_ALLOC_HUGE);

	i = 0;

	entry_seq_init(&hstat);
	while ((shared_entry = entry_seq_next(&hstat, &key)) != NULL)
	{
		pgssEntry  *entry;

		if (!LWLockConditionalAcquire(&shared_entry->header.lock, LW_EXCLUSIVE))
			continue;

		entry = &shared_entry->entry;
		if (entry->stats_since == 0)
		{
			LWLockRelease(&shared_entry->header.lock);
			continue;
		}

		/* "Sticky" entries get a different usage decay rate. */
		if (!entry->executed)
			entry->usage *= STICKY_DECREASE_FACTOR;
		else
			entry->usage *= USAGE_DECREASE_FACTOR;

		if (i >= nalloc)
		{
			nalloc *= 2;
			entries = repalloc_huge(entries, nalloc * sizeof(pgssDeallocEntry));
		}
		entries[i].key = key;
		entries[i].stats_since = entry->stats_since;
		entries[i].usage = entry->usage;
		i++;

		LWLockRelease(&shared_entry->header.lock);
	}
	dshash_seq_term(&hstat);

	/* Sort into increasing order by usage */
	qsort(entries, i, sizeof(pgssDeallocEntry), entry_cmp);

	/*
	 * Record the (approximate) median usage, under the mutex as entries are
	 * created meanwhile
	 */
	if (i > 0)
	{
		SpinLockAcquire(&pgss->mutex);
		pgss->cur_median_usage = entries[i / 2].usage;
		SpinLockRelease(&pgss->mutex);
	}

	/*
	 * Now zap an appropriate fraction of lowest-usage entries, plus the
	 * entries exceeding pg_stat_statements.max.
	 */
	nvictims = Max(10, (int) (Min((uint64) i, max) *
							  USAGE_DEALLOC_PERCENT / 100));
	num_entries = pg_atomic_read_u64(&pgss->num_entries);
	if (num_entries > max)
		nvictims += (int) Min(num_entries - max, (uint64) i);
	nvictims = Min(nvictims, i);

	/*
	 * Remove the victims, unless used or reused since, removed by someone
	 * else, or busy; the next pass deallocates any excess left
	 */
	for (i = 0; i < nvictims; i++)
	{
		PgStat_EntryRef *entry_ref;

		entry_ref = pgstat_get_entry_ref(entries[i].key.kind,
										 entries[i].key.dboid,
										 entries[i].key.objid, false, NULL);
		if (entry_ref != NULL &&
			entry_remove((pgssSharedEntry *) entry_ref->shared_stats,
						 entry_ref, &entries[i]))
			entry_drop(&entries[i].key);
	}

	pfree(entries);

	/* Increment the number of times entries are deallocated */
	gstats = pgss_global_stats();
	SpinLockAcquire(&pgss->mutex);
	gstats->dealloc += 1;
	SpinLockRelease(&pgss->mutex);

	LWLockRelease(&pgss->dealloc_lock);
}

/*
 * Copy a query string (not necessarily null-terminated) into the query text
 * area.  Returns its pointer in the area, or InvalidDsaPointer if there is no
 * room for it within pg_stat_statements.query_text_memory.
 */
static dsa_pointer
qtext_store(const char *query, int query_len)
{
	dsa_area   *area = qtext_area();
	dsa_pointer qptr;
	char	   *qstr;

	/*
	 * Apply pg_stat_statements.query_text_memory to the area, once in each
	 * backend and then when it changes.  A lowered limit prevents new
	 * segments and frees empty ones; texts in the others stay until their
	 * entries are removed.
	 */
	if (qtext_applied_limit != pgss_query_text_memory)
	{
		int			applied = qtext_applied_limit;

		qtext_applied_limit = pgss_query_text_memory;
		dsa_set_size_limit(area, (size_t) pgss_query_text_memory * 1024);
		if (applied != 0 && pgss_query_text_memory < applied)
			dsa_trim(area);
	}

	if ((Size) query_len >= MaxAllocSize)
		return InvalidDsaPointer;
	qptr = dsa_allocate_extended(area, (Size) query_len + 1, DSA_ALLOC_NO_OOM);
	if (DsaPointerIsValid(qptr))
	{
		qstr = dsa_get_address(area, qptr);
		memcpy(qstr, query, query_len);
		qstr[query_len] = '\0';
	}

	return qptr;
}

/*
 * The query text area, attached to at first use.
 */
static dsa_area *
qtext_area(void)
{
	if (qtext_dsa == NULL)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		qtext_dsa = dsa_attach_in_place(qtext_area_place(), NULL);
		dsa_pin_mapping(qtext_dsa);
		MemoryContextSwitchTo(oldcontext);
	}

	return qtext_dsa;
}

/*
 * Return the query text of an entry, or NULL if it has none.  Caller must
 * hold the entry lock, and copy the text before releasing it.
 */
static const char *
qtext_fetch(const pgssEntry *entry)
{
	if (!DsaPointerIsValid(entry->query_text))
		return NULL;

	return dsa_get_address(qtext_area(), entry->query_text);
}

/*
 * Remove a live entry: readers skip it, and pgss->num_entries no longer
 * counts it.  Its query text is freed.  Returns true if this call removed
 * it, for the caller to drop it, or false if it was not live, or another
 * removal got it first.
 *
 * With entry_ref, also return false if the entry was reused since the
 * reference was obtained.  With victim, also return false if the entry is
 * not the one entry_dealloc() chose, was used since, or its lock is busy.
 *
 * XXX: pgss->num_entries is needed as the entry counts of the cumulative
 * statistics system include dropped entries until their references are
 * released.
 */
static bool
entry_remove(pgssSharedEntry *shared_entry, PgStat_EntryRef *entry_ref,
			 const pgssDeallocEntry *victim)
{
	pgssEntry  *entry = &shared_entry->entry;
	bool		removed = false;

	if (victim == NULL)
		LWLockAcquire(&shared_entry->header.lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(&shared_entry->header.lock, LW_EXCLUSIVE))
		return false;

	if (entry->stats_since != 0 &&
		(entry_ref == NULL || !pgstat_entry_ref_is_stale(entry_ref)) &&
		(victim == NULL ||
		 (entry->stats_since == victim->stats_since &&
		  entry->usage == victim->usage)))
	{
		entry->stats_since = 0;
		if (DsaPointerIsValid(entry->query_text))
		{
			dsa_free(qtext_area(), entry->query_text);
			entry->query_text = InvalidDsaPointer;
			entry->query_len = -1;
		}
		pg_atomic_sub_fetch_u64(&pgss->num_entries, 1);
		removed = true;
	}
	LWLockRelease(&shared_entry->header.lock);

	return removed;
}

/*
 * Reset min/max statistics.  For a shared entry's counters, caller must hold
 * the entry lock exclusively.
 */
static void
entry_reset_minmax(Counters *counters)
{
	for (int kind = 0; kind < PGSS_NUMKIND; kind++)
	{
		counters->max_time[kind] = 0;
		counters->min_time[kind] = 0;
	}
}

/*
 * Reset the min/max statistics of a shared entry, and have backends discard
 * their pending ones.  Caller must hold the entry lock exclusively.
 */
static void
entry_reset_shared_minmax(pgssSharedEntry *shared_entry,
						  TimestampTz stats_reset)
{
	entry_reset_minmax(&shared_entry->entry.counters);
	shared_entry->entry.minmax_stats_since = stats_reset;
	pg_atomic_fetch_add_u32(&shared_entry->entry.minmax_generation, 1);
}

/*
 * Reset the entry of a key, if it exists.
 */
static void
entry_reset_single(const pgssHashKey *key, bool minmax_only,
				   TimestampTz stats_reset)
{
	PgStat_EntryRef *entry_ref;
	pgssSharedEntry *shared_entry;

	entry_ref = entry_find(key);
	if (!entry_ref)
		return;

	shared_entry = (pgssSharedEntry *) entry_ref->shared_stats;

	if (minmax_only)
	{
		pgstat_lock_entry(entry_ref, false);
		if (!pgstat_entry_ref_is_stale(entry_ref) &&
			shared_entry->entry.stats_since != 0)
			entry_reset_shared_minmax(shared_entry, stats_reset);
		pgstat_unlock_entry(entry_ref);
	}
	/* Remove the entry, unless it is not initialized or was evicted meanwhile */
	else if (entry_remove(shared_entry, entry_ref, NULL))
		entry_drop(&entry_ref->shared_entry->key);
}

/*
 * Does an entry match the key fields of entry_reset()?  Zero fields match
 * anything.
 */
static bool
entry_reset_matches(const pgssEntry *entry, const pgssHashKey *key)
{
	return (!key->userid || entry->key.userid == key->userid) &&
		(!key->dbid || entry->key.dbid == key->dbid) &&
		(!key->queryid || entry->key.queryid == key->queryid);
}

/*
 * pgstat_drop_matching_entries() callback for entry_reset(), removing the
 * matching entries.  Caller holds the entry's partition lock exclusively.
 */
static bool
entry_reset_drop_matching(PgStatShared_HashEntry *hash_entry, Datum match_data)
{
	pgssHashKey *key = (pgssHashKey *) DatumGetPointer(match_data);
	pgssSharedEntry *shared_entry = entry_from_hash(hash_entry);
	bool		matches;

	/* pgstat_drop_matching_entries() skips dropped entries */
	Assert(shared_entry != NULL);

	/*
	 * Skip entries not initialized yet, which have no key to match, and
	 * entries being removed by someone else.
	 */
	LWLockAcquire(&shared_entry->header.lock, LW_SHARED);
	matches = shared_entry->entry.stats_since != 0 &&
		entry_reset_matches(&shared_entry->entry, key);
	LWLockRelease(&shared_entry->header.lock);

	return matches && entry_remove(shared_entry, NULL, NULL);
}

/*
 * Reset entries corresponding to parameters passed.
 */
static TimestampTz
entry_reset(Oid userid, Oid dbid, int64 queryid, bool minmax_only)
{
	pgssHashKey key;
	TimestampTz stats_reset;

	if (!pgss)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via \"shared_preload_libraries\"")));

	memset(&key, 0, sizeof(pgssHashKey));
	key.userid = userid;
	key.dbid = dbid;
	key.queryid = queryid;

	stats_reset = GetCurrentTimestamp();

	if (userid != 0 && dbid != 0 && queryid != INT64CONST(0))
	{
		/* If all the parameters are available, use the fast path. */

		/*
		 * Reset the entry if it exists, starting with the non-top-level
		 * entry.
		 */
		key.toplevel = false;
		entry_reset_single(&key, minmax_only, stats_reset);

		/* Also reset the top-level entry if it exists. */
		key.toplevel = true;
		entry_reset_single(&key, minmax_only, stats_reset);
	}
	else if (minmax_only)
	{
		dshash_seq_status hstat;
		pgssSharedEntry *shared_entry;

		/* Reset min/max of entries corresponding to valid parameters, or all */
		entry_seq_init(&hstat);
		while ((shared_entry = entry_seq_next(&hstat, NULL)) != NULL)
		{
			LWLockAcquire(&shared_entry->header.lock, LW_EXCLUSIVE);
			if (shared_entry->entry.stats_since != 0 &&
				entry_reset_matches(&shared_entry->entry, &key))
				entry_reset_shared_minmax(shared_entry, stats_reset);
			LWLockRelease(&shared_entry->header.lock);
		}
		dshash_seq_term(&hstat);
	}
	else
	{
		/* Reset entries corresponding to valid parameters, or all */
		pgstat_drop_matching_entries(PGSS_STATS_KIND,
									 entry_reset_drop_matching,
									 PointerGetDatum(&key));
	}

	/*
	 * Reset global statistics for pg_stat_statements if all entries were
	 * removed, even if new ones were created meanwhile.
	 */
	if (!minmax_only &&
		((userid == 0 && dbid == 0 && queryid == INT64CONST(0)) ||
		 pg_atomic_read_u64(&pgss->num_entries) == 0))
		pgss_global_stats_reset_all_cb(stats_reset);

	return stats_reset;
}

/*
 * Generate a normalized version of the query string that will be used to
 * represent all similar queries.
 *
 * Note that the normalized representation may well vary depending on
 * just which "equivalent" query is used to create the hashtable entry.
 * We assume this is OK.
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * *query_len_p contains the input string length, and is updated with
 * the result string length on exit.  The resulting string might be longer
 * or shorter depending on what happens with replacement of constants.
 *
 * Returns a palloc'd string.
 */
static char *
generate_normalized_query(const JumbleState *jstate, const char *query,
						  int query_loc, int *query_len_p)
{
	StringInfoData norm_query;
	int			query_len = *query_len_p;
	int			len_to_wrt,		/* Length (in bytes) to write */
				quer_loc = 0,	/* Source query byte location */
				last_off = 0,	/* Offset from start for previous tok */
				last_tok_len = 0;	/* Length (in bytes) of that tok */
	int			num_constants_replaced = 0;
	LocationLen *locs = NULL;

	/*
	 * Our output buffer is an expansible StringInfo, but avoid enlarging it
	 * in most cases by reserving extra space for each constant location.
	 */
	Assert(jstate->clocations_count > 0);
	initStringInfoExt(&norm_query, query_len + jstate->clocations_count * 10);

	/*
	 * Determine constants' lengths (core system only gives us locations), and
	 * return a sorted copy of jstate's LocationLen data with lengths filled
	 * in.
	 */
	locs = ComputeConstantLengths(jstate, query, query_loc);

	for (int i = 0; i < jstate->clocations_count; i++)
	{
		int			off,		/* Offset from start for cur tok */
					tok_len;	/* Length (in bytes) of that tok */

		/*
		 * If we have an external param at this location, but no lists are
		 * being squashed across the query, then we skip here; this will make
		 * us print the characters found in the original query that represent
		 * the parameter in the next iteration (or after the loop is done),
		 * which is a bit odd but seems to work okay in most cases.
		 */
		if (locs[i].extern_param && !jstate->has_squashed_lists)
			continue;

		off = locs[i].location;

		/* Adjust recorded location if we're dealing with partial string */
		off -= query_loc;

		tok_len = locs[i].length;

		if (tok_len < 0)
			continue;			/* ignore any duplicates */

		/* Copy next chunk (what precedes the next constant) */
		len_to_wrt = off - last_off;
		len_to_wrt -= last_tok_len;
		Assert(len_to_wrt >= 0);
		appendBinaryStringInfo(&norm_query, query + quer_loc, len_to_wrt);

		/*
		 * And insert a param symbol in place of the constant token; and, if
		 * we have a squashable list, insert a placeholder comment starting
		 * from the list's second value.
		 */
		appendStringInfo(&norm_query, "$%d%s",
						 num_constants_replaced + 1 + jstate->highest_extern_param_id,
						 locs[i].squashed ? " /*, ... */" : "");
		num_constants_replaced++;

		/* move forward */
		quer_loc = off + tok_len;
		last_off = off;
		last_tok_len = tok_len;
	}

	/* Clean up, if needed */
	if (locs)
		pfree(locs);

	/*
	 * We've copied up until the last ignorable constant.  Copy over the
	 * remaining bytes of the original query string.
	 */
	len_to_wrt = query_len - quer_loc;

	Assert(len_to_wrt >= 0);
	appendBinaryStringInfo(&norm_query, query + quer_loc, len_to_wrt);

	*query_len_p = norm_query.len;
	return norm_query.data;
}
