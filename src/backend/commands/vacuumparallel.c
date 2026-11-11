/*-------------------------------------------------------------------------
 *
 * vacuumparallel.c
 *	  Support routines for parallel vacuum and autovacuum execution. In the
 *	  comments below, the word "vacuum" will refer to both vacuum and
 *	  autovacuum.
 *
 * This file contains routines that are intended to support setting up, using,
 * and tearing down a ParallelVacuumState.
 *
 * In a parallel vacuum, we perform both index bulk deletion and index cleanup
 * with parallel worker processes.  Individual indexes are processed by one
 * vacuum process.  ParallelVacuumState contains shared information as well as
 * the memory space for storing dead items allocated in the DSA area.  We
 * launch parallel worker processes at the start of parallel index
 * bulk-deletion and index cleanup and once all indexes are processed, the
 * parallel worker processes exit.  Each time we process indexes in parallel,
 * the parallel context is re-initialized so that the same DSM can be used for
 * multiple passes of index bulk-deletion and index cleanup.
 *
 * For parallel autovacuum, we need to propagate cost-based vacuum delay
 * parameters from the leader to its workers, as the leader's parameters can
 * change even while processing a table (e.g., due to a config reload).
 * The PVSharedCostParams struct manages these parameters using a
 * generation counter. Each parallel worker polls this shared state and
 * refreshes its local delay parameters whenever a change is detected.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuumparallel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/table.h"
#include "access/xact.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/instrument.h"
#include "optimizer/paths.h"
#include "pgstat.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

/*
 * DSM keys for parallel vacuum.  Unlike other parallel execution code, since
 * we don't need to worry about DSM keys conflicting with plan_node_id we can
 * use small integers.
 */
#define PARALLEL_VACUUM_KEY_SHARED			1
#define PARALLEL_VACUUM_KEY_QUERY_TEXT		2
#define PARALLEL_VACUUM_KEY_BUFFER_USAGE	3
#define PARALLEL_VACUUM_KEY_WAL_USAGE		4
#define PARALLEL_VACUUM_KEY_INDEX_STATS		5

/*
 * Shared information among the parallel index vacuum workers.
 */
typedef struct PVIndexShared
{
	PVShared	base;

	/*
	 * Fields for both index vacuum and cleanup.
	 *
	 * reltuples is the total number of input heap tuples.  We set either old
	 * live tuples in the index vacuum case or the new live tuples in the
	 * index cleanup case.
	 *
	 * estimated_count is true if reltuples is an estimated value.  (Note that
	 * reltuples could be -1 in this case, indicating we have no idea.)
	 */
	double		reltuples;
	bool		estimated_count;

	/*
	 * In single process vacuum we could consume more memory during index
	 * vacuuming or cleanup apart from the memory for heap scanning.  In
	 * parallel vacuum, since individual vacuum workers can consume memory
	 * equal to maintenance_work_mem, the new maintenance_work_mem for each
	 * worker is set such that the parallel operation doesn't consume more
	 * memory than single process vacuum.
	 */
	int			maintenance_work_mem_worker;

	/* Counter for vacuuming and cleanup */
	pg_atomic_uint32 idx;
} PVIndexShared;

/* Status used during parallel index vacuum or cleanup */
typedef enum PVIndVacStatus
{
	PARALLEL_INDVAC_STATUS_INITIAL = 0,
	PARALLEL_INDVAC_STATUS_NEED_BULKDELETE,
	PARALLEL_INDVAC_STATUS_NEED_CLEANUP,
	PARALLEL_INDVAC_STATUS_COMPLETED,
} PVIndVacStatus;

/*
 * Struct for index vacuum statistics of an index that is used for parallel vacuum.
 * This includes the status of parallel index vacuum as well as index statistics.
 */
typedef struct PVIndStats
{
	/*
	 * The following two fields are set by leader process before executing
	 * parallel index vacuum or parallel index cleanup.  These fields are not
	 * fixed for the entire VACUUM operation.  They are only fixed for an
	 * individual parallel index vacuum and cleanup.
	 *
	 * parallel_workers_can_process is true if both leader and worker can
	 * process the index, otherwise only leader can process it.
	 */
	PVIndVacStatus status;
	bool		parallel_workers_can_process;

	/*
	 * Individual worker or leader stores the result of index vacuum or
	 * cleanup.
	 */
	bool		istat_updated;	/* are the stats updated? */
	IndexBulkDeleteResult istat;
} PVIndStats;

/*
 * Struct for maintaining a parallel vacuum state. typedef appears in vacuum.h.
 */
struct ParallelVacuumState
{
	/* NULL for worker processes */
	ParallelContext *pcxt;

	/* Parent Heap Relation */
	Relation	heaprel;

	/* Target indexes */
	Relation   *indrels;
	int			nindexes;

	/* Shared information among parallel vacuum workers */
	PVIndexShared *shared;

	/*
	 * Shared index statistics among parallel vacuum workers. The array
	 * element is allocated for every index, even those indexes where parallel
	 * index vacuuming is unsafe or not worthwhile (e.g.,
	 * will_parallel_vacuum[] is false).  During parallel vacuum,
	 * IndexBulkDeleteResult of each index is kept in DSM and is copied into
	 * local memory at the end of parallel vacuum.
	 */
	PVIndStats *indstats;

	/* Shared dead items space among parallel vacuum workers */
	TidStore   *dead_items;

	/* Points to buffer usage area in DSM */
	BufferUsage *buffer_usage;

	/* Points to WAL usage area in DSM */
	WalUsage   *wal_usage;

	/*
	 * False if the index is totally unsuitable target for all parallel
	 * processing. For example, the index could be <
	 * min_parallel_index_scan_size cutoff.
	 */
	bool	   *will_parallel_vacuum;

	/*
	 * The number of indexes that support parallel index bulk-deletion and
	 * parallel index cleanup respectively.
	 */
	int			nindexes_parallel_bulkdel;
	int			nindexes_parallel_cleanup;
	int			nindexes_parallel_condcleanup;

	/* Buffer access strategy used by leader process */
	BufferAccessStrategy bstrategy;

	/*
	 * Error reporting state.  The error callback is set only for workers
	 * processes during parallel index vacuum.
	 */
	char	   *relnamespace;
	char	   *relname;
	char	   *indname;
	PVIndVacStatus status;
};

static PVSharedCostParams *pv_shared_cost_params = NULL;

/*
 * Worker-local copy of the last cost-parameter generation this worker has
 * applied.  Initialized to 0; since the leader initializes the shared
 * generation counter to 1, the first call to
 * parallel_vacuum_update_shared_delay_params() will always detect a
 * mismatch and read the initial parameters from shared memory.
 */
static uint32 shared_params_generation_local = 0;

static int	parallel_vacuum_compute_workers(Relation *indrels, int nindexes, int nrequested,
											bool *will_parallel_vacuum);
static void parallel_vacuum_process_all_indexes(ParallelVacuumState *pvs, int num_index_scans,
												bool vacuum, PVWorkerStats *wstats);
static void parallel_vacuum_process_safe_indexes(ParallelVacuumState *pvs);
static void parallel_vacuum_process_unsafe_indexes(ParallelVacuumState *pvs);
static void parallel_vacuum_process_one_index(ParallelVacuumState *pvs, Relation indrel,
											  PVIndStats *indstats);
static bool parallel_vacuum_index_is_parallel_safe(Relation indrel, int num_index_scans,
												   bool vacuum);
static void parallel_vacuum_error_callback(void *arg);
static inline void parallel_vacuum_set_cost_parameters(PVSharedCostParams *params);
static void parallel_vacuum_dsm_detach(dsm_segment *seg, Datum arg);

/*
 * Try to enter parallel mode and create a parallel context.  Then initialize
 * shared memory state.
 *
 * On success, return parallel vacuum state.  Otherwise return NULL.
 */
ParallelVacuumState *
parallel_vacuum_init(Relation rel, Relation *indrels, int nindexes,
					 int nrequested_workers, int vac_work_mem,
					 int elevel, BufferAccessStrategy bstrategy)
{
	ParallelVacuumState *pvs;
	ParallelContext *pcxt;
	PVIndexShared *shared;
	PVIndStats *indstats;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	bool	   *will_parallel_vacuum;
	Size		est_indstats_len;
	Size		est_shared_len;
	int			nindexes_mwm = 0;
	int			parallel_workers = 0;
	int			querylen;

	/*
	 * A parallel vacuum must be requested and there must be indexes on the
	 * relation
	 */
	Assert(nrequested_workers >= 0);
	Assert(nindexes > 0);

	/*
	 * Compute the number of parallel vacuum workers to launch
	 */
	will_parallel_vacuum = palloc0_array(bool, nindexes);
	parallel_workers = parallel_vacuum_compute_workers(indrels, nindexes,
													   nrequested_workers,
													   will_parallel_vacuum);
	if (parallel_workers <= 0)
	{
		/* Can't perform vacuum in parallel -- return NULL */
		pfree(will_parallel_vacuum);
		return NULL;
	}

	pvs = palloc0_object(ParallelVacuumState);
	pvs->indrels = indrels;
	pvs->nindexes = nindexes;
	pvs->will_parallel_vacuum = will_parallel_vacuum;
	pvs->bstrategy = bstrategy;
	pvs->heaprel = rel;

	EnterParallelMode();
	pcxt = CreateParallelContext("postgres", "parallel_vacuum_main",
								 parallel_workers);
	Assert(pcxt->nworkers > 0);
	pvs->pcxt = pcxt;

	/* Estimate size for index vacuum stats -- PARALLEL_VACUUM_KEY_INDEX_STATS */
	est_indstats_len = mul_size(sizeof(PVIndStats), nindexes);
	shm_toc_estimate_chunk(&pcxt->estimator, est_indstats_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Estimate size for shared information -- PARALLEL_VACUUM_KEY_SHARED */
	est_shared_len = sizeof(PVIndexShared);
	shm_toc_estimate_chunk(&pcxt->estimator, est_shared_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * Estimate space for BufferUsage and WalUsage --
	 * PARALLEL_VACUUM_KEY_BUFFER_USAGE and PARALLEL_VACUUM_KEY_WAL_USAGE.
	 *
	 * If there are no extensions loaded that care, we could skip this.  We
	 * have no way of knowing whether anyone's looking at pgBufferUsage or
	 * pgWalUsage, so do it unconditionally.
	 */
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* Finally, estimate PARALLEL_VACUUM_KEY_QUERY_TEXT space */
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;			/* keep compiler quiet */

	InitializeParallelDSM(pcxt);

	/* Prepare index vacuum stats */
	indstats = (PVIndStats *) shm_toc_allocate(pcxt->toc, est_indstats_len);
	MemSet(indstats, 0, est_indstats_len);
	for (int i = 0; i < nindexes; i++)
	{
		Relation	indrel = indrels[i];
		uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

		/*
		 * Cleanup option should be either disabled, always performing in
		 * parallel or conditionally performing in parallel.
		 */
		Assert(((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) ||
			   ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0));
		Assert(vacoptions <= VACUUM_OPTION_MAX_VALID_VALUE);

		if (!will_parallel_vacuum[i])
			continue;

		if (indrel->rd_indam->amusemaintenanceworkmem)
			nindexes_mwm++;

		/*
		 * Remember the number of indexes that support parallel operation for
		 * each phase.
		 */
		if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0)
			pvs->nindexes_parallel_bulkdel++;
		if ((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) != 0)
			pvs->nindexes_parallel_cleanup++;
		if ((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0)
			pvs->nindexes_parallel_condcleanup++;
	}
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_INDEX_STATS, indstats);
	pvs->indstats = indstats;

	/* Prepare shared information */
	shared = (PVIndexShared *) shm_toc_allocate(pcxt->toc, est_shared_len);
	MemSet(shared, 0, est_shared_len);
	parallel_vacuum_init_shared(&shared->base, pcxt, rel, bstrategy,
								vac_work_mem, elevel);
	shared->maintenance_work_mem_worker =
		(nindexes_mwm > 0) ?
		vac_work_mem / Min(parallel_workers, nindexes_mwm) :
		vac_work_mem;

	/* Prepare DSA space for dead items */
	pvs->dead_items = parallel_vacuum_create_dead_items(&shared->base);

	pg_atomic_init_u32(&(shared->idx), 0);

	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_SHARED, shared);
	pvs->shared = shared;

	/*
	 * Allocate space for each worker's BufferUsage and WalUsage; no need to
	 * initialize
	 */
	buffer_usage = shm_toc_allocate(pcxt->toc,
									mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, buffer_usage);
	pvs->buffer_usage = buffer_usage;
	wal_usage = shm_toc_allocate(pcxt->toc,
								 mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_VACUUM_KEY_WAL_USAGE, wal_usage);
	pvs->wal_usage = wal_usage;

	/* Store query string for workers */
	if (debug_query_string)
	{
		char	   *sharedquery;

		sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		sharedquery[querylen] = '\0';
		shm_toc_insert(pcxt->toc,
					   PARALLEL_VACUUM_KEY_QUERY_TEXT, sharedquery);
	}

	/* Success -- return parallel vacuum state */
	return pvs;
}

/*
 * Destroy the parallel context, and end parallel mode.
 *
 * Since writes are not allowed during parallel mode, copy the
 * updated index statistics from DSM into local memory and then later use that
 * to update the index statistics.  One might think that we can exit from
 * parallel mode, update the index statistics and then destroy parallel
 * context, but that won't be safe (see ExitParallelMode).
 */
void
parallel_vacuum_end(ParallelVacuumState *pvs, IndexBulkDeleteResult **istats)
{
	Assert(!IsParallelWorker());

	/* Copy the updated statistics */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		if (indstats->istat_updated)
		{
			istats[i] = palloc0_object(IndexBulkDeleteResult);
			memcpy(istats[i], &indstats->istat, sizeof(IndexBulkDeleteResult));
		}
		else
			istats[i] = NULL;
	}

	parallel_vacuum_destroy_dead_items(&pvs->shared->base, pvs->dead_items);

	DestroyParallelContext(pvs->pcxt);
	ExitParallelMode();

	if (AmAutoVacuumWorkerProcess())
		pv_shared_cost_params = NULL;

	pfree(pvs->will_parallel_vacuum);
	pfree(pvs);
}

/*
 * DSM detach callback. This is invoked when an autovacuum worker detaches
 * from the DSM segment holding PVIndexShared. It ensures to reset the local
 * pointer to the shared state even if parallel vacuum raises an error and
 * doesn't call parallel_vacuum_end().
 */
static void
parallel_vacuum_dsm_detach(dsm_segment *seg, Datum arg)
{
	Assert(AmAutoVacuumWorkerProcess());
	pv_shared_cost_params = NULL;
}

/*
 * Returns the dead items space and dead items information.
 */
TidStore *
parallel_vacuum_get_dead_items(ParallelVacuumState *pvs, VacDeadItemsInfo **dead_items_info_p)
{
	*dead_items_info_p = &(pvs->shared->base.dead_items_info);
	return pvs->dead_items;
}

/* Forget all items in dead_items */
void
parallel_vacuum_reset_dead_items(ParallelVacuumState *pvs)
{
	/*
	 * Free the current tidstore and return allocated DSA segments to the
	 * operating system. Then we recreate the tidstore with the same max_bytes
	 * limitation we just used.
	 */
	parallel_vacuum_destroy_dead_items(&pvs->shared->base, pvs->dead_items);
	pvs->dead_items = parallel_vacuum_create_dead_items(&pvs->shared->base);
}

/*
 * Do parallel index bulk-deletion with parallel workers.
 */
void
parallel_vacuum_bulkdel_all_indexes(ParallelVacuumState *pvs, double num_table_tuples,
									int num_index_scans, PVWorkerStats *wstats)
{
	Assert(!IsParallelWorker());

	/*
	 * We can only provide an approximate value of num_heap_tuples, at least
	 * for now.
	 */
	pvs->shared->reltuples = num_table_tuples;
	pvs->shared->estimated_count = true;

	parallel_vacuum_process_all_indexes(pvs, num_index_scans, true, wstats);
}

/*
 * Do parallel index cleanup with parallel workers.
 */
void
parallel_vacuum_cleanup_all_indexes(ParallelVacuumState *pvs, double num_table_tuples,
									int num_index_scans, bool estimated_count,
									PVWorkerStats *wstats)
{
	Assert(!IsParallelWorker());

	/*
	 * We can provide a better estimate of total number of surviving tuples
	 * (we assume indexes are more interested in that than in the number of
	 * nominally live tuples).
	 */
	pvs->shared->reltuples = num_table_tuples;
	pvs->shared->estimated_count = estimated_count;

	parallel_vacuum_process_all_indexes(pvs, num_index_scans, false, wstats);
}

/*
 * Fill in the given structure with cost-based vacuum delay parameter values.
 */
static inline void
parallel_vacuum_set_cost_parameters(PVSharedCostParams *params)
{
	params->cost_delay = vacuum_cost_delay;
	params->cost_limit = vacuum_cost_limit;
	params->cost_page_dirty = VacuumCostPageDirty;
	params->cost_page_hit = VacuumCostPageHit;
	params->cost_page_miss = VacuumCostPageMiss;
	params->track_cost_delay_timing = track_cost_delay_timing;
}

/*
 * Updates the cost-based vacuum delay parameters for parallel autovacuum
 * workers.
 *
 * For non-autovacuum parallel workers, this function will have no effect.
 */
void
parallel_vacuum_update_shared_delay_params(void)
{
	uint32		params_generation;

	Assert(IsParallelWorker());

	/* Quick return if the worker is not running for the autovacuum */
	if (pv_shared_cost_params == NULL)
		return;

	params_generation = pg_atomic_read_u32(&pv_shared_cost_params->generation);
	Assert(shared_params_generation_local <= params_generation);

	/* Return if parameters had not changed in the leader */
	if (params_generation == shared_params_generation_local)
		return;

	SpinLockAcquire(&pv_shared_cost_params->mutex);
	VacuumCostDelay = pv_shared_cost_params->cost_delay;
	VacuumCostLimit = pv_shared_cost_params->cost_limit;
	VacuumCostPageDirty = pv_shared_cost_params->cost_page_dirty;
	VacuumCostPageHit = pv_shared_cost_params->cost_page_hit;
	VacuumCostPageMiss = pv_shared_cost_params->cost_page_miss;
	track_cost_delay_timing = pv_shared_cost_params->track_cost_delay_timing;
	SpinLockRelease(&pv_shared_cost_params->mutex);

	VacuumUpdateCosts();

	shared_params_generation_local = params_generation;

	elog(DEBUG2,
		 "parallel autovacuum worker updated cost params: cost_limit=%d, cost_delay=%g, cost_page_miss=%d, cost_page_dirty=%d, cost_page_hit=%d, track_cost_delay_timing=%s",
		 vacuum_cost_limit,
		 vacuum_cost_delay,
		 VacuumCostPageMiss,
		 VacuumCostPageDirty,
		 VacuumCostPageHit,
		 track_cost_delay_timing ? "yes" : "no");
}

/*
 * Store the cost-based vacuum delay parameters in the shared memory so that
 * parallel vacuum workers can consume them (see
 * parallel_vacuum_update_shared_delay_params()).
 */
void
parallel_vacuum_propagate_shared_delay_params(void)
{
	Assert(AmAutoVacuumWorkerProcess());

	/*
	 * Quick return if the leader process is not sharing the delay parameters.
	 */
	if (pv_shared_cost_params == NULL)
		return;

	/*
	 * Check if any delay parameters have changed. We can read them without
	 * locks as only the leader can modify them.
	 */
	if (vacuum_cost_delay == pv_shared_cost_params->cost_delay &&
		vacuum_cost_limit == pv_shared_cost_params->cost_limit &&
		VacuumCostPageDirty == pv_shared_cost_params->cost_page_dirty &&
		VacuumCostPageHit == pv_shared_cost_params->cost_page_hit &&
		VacuumCostPageMiss == pv_shared_cost_params->cost_page_miss &&
		track_cost_delay_timing == pv_shared_cost_params->track_cost_delay_timing)
		return;

	/* Update the shared delay parameters */
	SpinLockAcquire(&pv_shared_cost_params->mutex);
	parallel_vacuum_set_cost_parameters(pv_shared_cost_params);
	SpinLockRelease(&pv_shared_cost_params->mutex);

	/*
	 * Increment the generation of the parameters, i.e. let parallel workers
	 * know that they should re-read shared cost params.
	 */
	pg_atomic_fetch_add_u32(&pv_shared_cost_params->generation, 1);
}

/*
 * Reload the configuration file if requested, and refresh the cost-based
 * delay parameters of an autovacuum worker running a parallel vacuum
 * (leader), propagating any change to its parallel workers.
 */
void
parallel_vacuum_refresh_cost_params(void)
{
	Assert(AmAutoVacuumWorkerProcess());

	/*
	 * Quick return if the leader is not sharing the delay parameters with
	 * parallel workers.
	 */
	if (pv_shared_cost_params == NULL)
		return;

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);

		/* This re-divides the cost limit too */
		VacuumUpdateCosts();
	}
	else
	{
		/* The number of workers sharing the cost limit may have changed */
		AutoVacuumUpdateCostLimit();
	}

	parallel_vacuum_propagate_shared_delay_params();
}

/*
 * Set up the shared state in the parallel context's DSM segment.
 */
void
parallel_vacuum_init_shared(PVShared *shared, ParallelContext *pcxt,
							Relation rel, BufferAccessStrategy bstrategy,
							int vac_work_mem, int elevel)
{
	Assert(!IsParallelWorker());

	shared->relid = RelationGetRelid(rel);
	shared->elevel = elevel;
	shared->queryid = pgstat_get_my_query_id();

	/* Use the same buffer size for all workers */
	shared->ring_nbuffers = GetAccessStrategyBufferCount(bstrategy);

	shared->is_autovacuum = AmAutoVacuumWorkerProcess();

	shared->dead_items_dsa_handle = DSA_HANDLE_INVALID;
	shared->dead_items_handle = InvalidDsaPointer;
	shared->dead_items_info.max_bytes = vac_work_mem * (size_t) 1024;
	shared->dead_items_info.num_items = 0;

	pg_atomic_init_u32(&(shared->cost_balance), 0);
	pg_atomic_init_u32(&(shared->active_nworkers), 0);

	/*
	 * Initialize shared cost-based vacuum delay parameters if it's for
	 * autovacuum and the parallel context has workers. Note that the parallel
	 * context falls back to the leader's private memory with no workers and
	 * no segment when the maximum number of DSM segments has been reached.
	 * There are then no workers to propagate the parameters to, and no
	 * segment to register the detach callback on.
	 */
	shared->share_params = shared->is_autovacuum && pcxt->nworkers > 0;
	if (shared->share_params)
	{
		parallel_vacuum_set_cost_parameters(&shared->cost_params);
		pg_atomic_init_u32(&shared->cost_params.generation, 1);
		SpinLockInit(&shared->cost_params.mutex);

		pv_shared_cost_params = &(shared->cost_params);
		on_dsm_detach(pcxt->seg, parallel_vacuum_dsm_detach, (Datum) 0);
	}
}

/*
 * Create the dead items store and set its DSA handle and pointer.
 */
TidStore *
parallel_vacuum_create_dead_items(PVShared *shared)
{
	TidStore   *dead_items;

	Assert(!IsParallelWorker());

	dead_items = TidStoreCreateShared(shared->dead_items_info.max_bytes,
									  LWTRANCHE_PARALLEL_VACUUM_DSA);
	shared->dead_items_info.num_items = 0;
	parallel_vacuum_set_dead_items(shared, dead_items);

	return dead_items;
}

/*
 * Destroy the dead items store.
 */
void
parallel_vacuum_destroy_dead_items(PVShared *shared, TidStore *dead_items)
{
	Assert(!IsParallelWorker());

	TidStoreDestroy(dead_items);
	parallel_vacuum_set_dead_items(shared, NULL);
}

/*
 * Set the DSA handle and pointer for the shared TidStore.
 */
void
parallel_vacuum_set_dead_items(PVShared *shared, TidStore *dead_items)
{
	Assert(!IsParallelWorker());

	if (dead_items == NULL)
	{
		shared->dead_items_dsa_handle = DSA_HANDLE_INVALID;
		shared->dead_items_handle = InvalidDsaPointer;
	}
	else
	{
		shared->dead_items_dsa_handle =
			dsa_get_handle(TidStoreGetDSA(dead_items));
		shared->dead_items_handle = TidStoreGetHandle(dead_items);
	}
}

/*
 * Launch the workers and start sharing the cost-based vacuum delay with them.
 */
void
parallel_vacuum_launch_workers(ParallelContext *pcxt, PVShared *shared)
{
	Assert(!IsParallelWorker());
	Assert(VacuumSharedCostBalance == NULL);

	/*
	 * Set up shared cost balance and the number of active workers for vacuum
	 * delay.  We need to do this before launching workers as otherwise, they
	 * might not see the updated values for these parameters.
	 */
	pg_atomic_write_u32(&(shared->cost_balance), VacuumCostBalance);
	pg_atomic_write_u32(&(shared->active_nworkers), 0);

	/*
	 * The leader propagates delay parameter changes to one shared state at a
	 * time, so make it this one.
	 */
	if (shared->share_params)
	{
		pv_shared_cost_params = &(shared->cost_params);
		parallel_vacuum_propagate_shared_delay_params();
	}

	LaunchParallelWorkers(pcxt);

	if (pcxt->nworkers_launched > 0)
	{
		/*
		 * Reset the local cost values for leader backend as we have already
		 * accumulated the remaining balance in the shared balance.
		 */
		VacuumCostBalance = 0;
		VacuumCostBalanceLocal = 0;

		/* Enable shared cost balance for leader backend */
		VacuumSharedCostBalance = &(shared->cost_balance);
		VacuumActiveNWorkers = &(shared->active_nworkers);
	}
}

/*
 * Wait for the workers to finish, and take the shared cost balance back.
 */
void
parallel_vacuum_wait_for_workers(ParallelContext *pcxt, PVShared *shared)
{
	Assert(!IsParallelWorker());

	WaitForParallelWorkersToFinish(pcxt);

	Assert(VacuumSharedCostBalance == NULL ||
		   VacuumSharedCostBalance == &shared->cost_balance);

	/* Carry the shared balance value back and disable shared costing */
	if (VacuumSharedCostBalance)
	{
		VacuumCostBalance = pg_atomic_read_u32(VacuumSharedCostBalance);
		VacuumSharedCostBalance = NULL;
		VacuumActiveNWorkers = NULL;
	}
}

/*
 * The setup every parallel vacuum worker does.
 */
Relation
parallel_vacuum_begin_worker(PVShared *shared, const char *sharedquery,
							 TidStore **dead_items,
							 BufferAccessStrategy *bstrategy)
{
	Relation	rel;

	/*
	 * A parallel vacuum worker carries only the PROC_IN_VACUUM flag. The
	 * leader, whether it's a backend running a VACUUM command or an
	 * autovacuum worker, sets PROC_IN_VACUUM when it starts vacuuming the
	 * table, and the worker inherits the flag by importing the leader's
	 * snapshot (see ProcArrayInstallRestoredXmin). The leader's other flags
	 * don't reach the worker: the snapshot import copies only the
	 * PROC_XMIN_FLAGS bits, so PROC_VACUUM_FOR_WRAPAROUND isn't carried over,
	 * and PROC_IS_AUTOVACUUM is never set on the worker in the first place
	 * since parallel workers run as regular background workers, not
	 * autovacuum workers.
	 */
	Assert(IsParallelWorker());
	Assert(MyProc->statusFlags == PROC_IN_VACUUM);

	/* Set debug_query_string for individual workers */
	debug_query_string = sharedquery;
	pgstat_report_activity(STATE_RUNNING, debug_query_string);

	/* Track query ID */
	pgstat_report_query_id(shared->queryid, false);

	/*
	 * Open table.  The lock mode is the same as the leader process.  It's
	 * okay because the lock mode does not conflict among the parallel
	 * workers.
	 */
	rel = table_open(shared->relid, ShareUpdateExclusiveLock);

	/* Each parallel VACUUM worker gets its own access strategy. */
	*bstrategy = GetAccessStrategyWithSize(BAS_VACUUM,
										   shared->ring_nbuffers * (BLCKSZ / 1024));

	/* Find dead_items in shared memory, if the leader shared any */
	if (shared->dead_items_dsa_handle == DSA_HANDLE_INVALID)
		*dead_items = NULL;
	else
		*dead_items = TidStoreAttach(shared->dead_items_dsa_handle,
									 shared->dead_items_handle);

	/* Set cost-based vacuum delay */
	if (shared->share_params)
	{
		/*
		 * Parallel autovacuum workers initialize cost-based delay parameters
		 * from the leader's shared state rather than GUC defaults, because
		 * the leader may have applied per-table or autovacuum-specific
		 * overrides. pv_shared_cost_params must be set before calling
		 * parallel_vacuum_update_shared_delay_params().
		 */
		pv_shared_cost_params = &(shared->cost_params);
		parallel_vacuum_update_shared_delay_params();
	}
	else
		VacuumUpdateCosts();

	VacuumCostBalance = 0;
	VacuumCostBalanceLocal = 0;
	VacuumSharedCostBalance = &(shared->cost_balance);
	VacuumActiveNWorkers = &(shared->active_nworkers);

	/* Prepare to track buffer usage during parallel execution */
	InstrStartParallelQuery();

	/* Register this worker for vacuum progress reporting */
	pgstat_progress_start_command(PROGRESS_COMMAND_VACUUM, shared->relid);

	return rel;
}

/*
 * Release the resources a parallel vacuum worker took.
 */
void
parallel_vacuum_end_worker(PVShared *shared, Relation rel,
						   TidStore *dead_items, BufferAccessStrategy bstrategy)
{
	Assert(IsParallelWorker());

	/* The store must be the one we attached, if we attached one */
	Assert((shared->dead_items_dsa_handle == DSA_HANDLE_INVALID) ==
		   (dead_items == NULL));

	/* Report any remaining cost-based vacuum delay time */
	if (parallel_vacuum_worker_delay_ns > 0)
		pgstat_progress_parallel_incr_param(PROGRESS_VACUUM_DELAY_TIME,
											parallel_vacuum_worker_delay_ns);

	if (dead_items != NULL)
		TidStoreDetach(dead_items);

	pgstat_progress_end_command();

	table_close(rel, ShareUpdateExclusiveLock);
	FreeAccessStrategy(bstrategy);

	if (shared->share_params)
		pv_shared_cost_params = NULL;
}

/*
 * Compute the number of parallel worker processes to request.  Both index
 * vacuum and index cleanup can be executed with parallel workers.
 * The index is eligible for parallel vacuum iff its size is greater than
 * min_parallel_index_scan_size as invoking workers for very small indexes
 * can hurt performance.
 *
 * nrequested is the number of parallel workers that user requested.  If
 * nrequested is 0, we compute the parallel degree based on nindexes, that is
 * the number of indexes that support parallel vacuum.  This function also
 * sets will_parallel_vacuum to remember indexes that participate in parallel
 * vacuum.
 */
static int
parallel_vacuum_compute_workers(Relation *indrels, int nindexes, int nrequested,
								bool *will_parallel_vacuum)
{
	int			nindexes_parallel = 0;
	int			nindexes_parallel_bulkdel = 0;
	int			nindexes_parallel_cleanup = 0;
	int			parallel_workers;
	int			max_workers;

	max_workers = AmAutoVacuumWorkerProcess() ?
		autovacuum_max_parallel_workers :
		max_parallel_maintenance_workers;

	/*
	 * We don't allow performing parallel operation in standalone backend or
	 * when parallelism is disabled.
	 */
	if (!IsUnderPostmaster || max_workers == 0)
		return 0;

	/*
	 * Compute the number of indexes that can participate in parallel vacuum.
	 */
	for (int i = 0; i < nindexes; i++)
	{
		Relation	indrel = indrels[i];
		uint8		vacoptions = indrel->rd_indam->amparallelvacuumoptions;

		/* Skip index that is not a suitable target for parallel index vacuum */
		if (vacoptions == VACUUM_OPTION_NO_PARALLEL ||
			RelationGetNumberOfBlocks(indrel) < min_parallel_index_scan_size)
			continue;

		will_parallel_vacuum[i] = true;

		if ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0)
			nindexes_parallel_bulkdel++;
		if (((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) != 0) ||
			((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0))
			nindexes_parallel_cleanup++;
	}

	nindexes_parallel = Max(nindexes_parallel_bulkdel,
							nindexes_parallel_cleanup);

	/* The leader process takes one index */
	nindexes_parallel--;

	/* No index supports parallel vacuum */
	if (nindexes_parallel <= 0)
		return 0;

	/* Compute the parallel degree */
	parallel_workers = (nrequested > 0) ?
		Min(nrequested, nindexes_parallel) : nindexes_parallel;

	/* Cap by GUC variable */
	parallel_workers = Min(parallel_workers, max_workers);

	return parallel_workers;
}

/*
 * Perform index vacuum or index cleanup with parallel workers.  This function
 * must be used by the parallel vacuum leader process.
 *
 * If wstats is not NULL, the parallel worker statistics are updated.
 */
static void
parallel_vacuum_process_all_indexes(ParallelVacuumState *pvs, int num_index_scans,
									bool vacuum, PVWorkerStats *wstats)
{
	int			nworkers;
	PVIndVacStatus new_status;

	Assert(!IsParallelWorker());

	if (vacuum)
	{
		new_status = PARALLEL_INDVAC_STATUS_NEED_BULKDELETE;

		/* Determine the number of parallel workers to launch */
		nworkers = pvs->nindexes_parallel_bulkdel;
	}
	else
	{
		new_status = PARALLEL_INDVAC_STATUS_NEED_CLEANUP;

		/* Determine the number of parallel workers to launch */
		nworkers = pvs->nindexes_parallel_cleanup;

		/* Add conditionally parallel-aware indexes if in the first time call */
		if (num_index_scans == 0)
			nworkers += pvs->nindexes_parallel_condcleanup;
	}

	/* The leader process will participate */
	nworkers--;

	/*
	 * It is possible that parallel context is initialized with fewer workers
	 * than the number of indexes that need a separate worker in the current
	 * phase, so we need to consider it.  See
	 * parallel_vacuum_compute_workers().
	 */
	nworkers = Min(nworkers, pvs->pcxt->nworkers);

	/* Update the statistics, if we asked to */
	if (wstats != NULL && nworkers > 0)
		wstats->nplanned += nworkers;

	/*
	 * Set index vacuum status and mark whether parallel vacuum worker can
	 * process it.
	 */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		Assert(indstats->status == PARALLEL_INDVAC_STATUS_INITIAL);
		indstats->status = new_status;
		indstats->parallel_workers_can_process =
			(pvs->will_parallel_vacuum[i] &&
			 parallel_vacuum_index_is_parallel_safe(pvs->indrels[i],
													num_index_scans,
													vacuum));
	}

	/* Reset the parallel index processing and progress counters */
	pg_atomic_write_u32(&(pvs->shared->idx), 0);

	/* Setup the shared cost-based vacuum delay and launch workers */
	if (nworkers > 0)
	{
		/* Reinitialize parallel context to relaunch parallel workers */
		if (num_index_scans > 0)
			ReinitializeParallelDSM(pvs->pcxt);

		/*
		 * The number of workers can vary between bulkdelete and cleanup
		 * phase.
		 */
		ReinitializeParallelWorkers(pvs->pcxt, nworkers);

		parallel_vacuum_launch_workers(pvs->pcxt, &pvs->shared->base);

		/* Update the statistics, if we asked to */
		if (wstats != NULL)
			wstats->nlaunched += pvs->pcxt->nworkers_launched;

		if (vacuum)
			ereport(pvs->shared->base.elevel,
					(errmsg(ngettext("launched %d parallel vacuum worker for index vacuuming (planned: %d)",
									 "launched %d parallel vacuum workers for index vacuuming (planned: %d)",
									 pvs->pcxt->nworkers_launched),
							pvs->pcxt->nworkers_launched, nworkers)));
		else
			ereport(pvs->shared->base.elevel,
					(errmsg(ngettext("launched %d parallel vacuum worker for index cleanup (planned: %d)",
									 "launched %d parallel vacuum workers for index cleanup (planned: %d)",
									 pvs->pcxt->nworkers_launched),
							pvs->pcxt->nworkers_launched, nworkers)));
	}

	/* Vacuum the indexes that can be processed by only leader process */
	parallel_vacuum_process_unsafe_indexes(pvs);

	/*
	 * Join as a parallel worker.  The leader vacuums alone processes all
	 * parallel-safe indexes in the case where no workers are launched.
	 */
	parallel_vacuum_process_safe_indexes(pvs);

	/*
	 * Next, accumulate buffer and WAL usage.  (This must wait for the workers
	 * to finish, or we might get incomplete data.)
	 */
	if (nworkers > 0)
	{
		/* Wait for all vacuum workers to finish */
		parallel_vacuum_wait_for_workers(pvs->pcxt, &pvs->shared->base);

		for (int i = 0; i < pvs->pcxt->nworkers_launched; i++)
			InstrAccumParallelQuery(&pvs->buffer_usage[i], &pvs->wal_usage[i]);
	}

	/*
	 * Reset all index status back to initial (while checking that we have
	 * vacuumed all indexes).
	 */
	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		if (indstats->status != PARALLEL_INDVAC_STATUS_COMPLETED)
			elog(ERROR, "parallel index vacuum on index \"%s\" is not completed",
				 RelationGetRelationName(pvs->indrels[i]));

		indstats->status = PARALLEL_INDVAC_STATUS_INITIAL;
	}
}

/*
 * Index vacuum/cleanup routine used by the leader process and parallel
 * vacuum worker processes to vacuum the indexes in parallel.
 */
static void
parallel_vacuum_process_safe_indexes(ParallelVacuumState *pvs)
{
	/*
	 * Increment the active worker count if we are able to launch any worker.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	/* Loop until all indexes are vacuumed */
	for (;;)
	{
		int			idx;
		PVIndStats *indstats;

		/* Get an index number to process */
		idx = pg_atomic_fetch_add_u32(&(pvs->shared->idx), 1);

		/* Done for all indexes? */
		if (idx >= pvs->nindexes)
			break;

		indstats = &(pvs->indstats[idx]);

		/*
		 * Skip vacuuming index that is unsafe for workers or has an
		 * unsuitable target for parallel index vacuum (this is vacuumed in
		 * parallel_vacuum_process_unsafe_indexes() by the leader).
		 */
		if (!indstats->parallel_workers_can_process)
			continue;

		/* Do vacuum or cleanup of the index */
		parallel_vacuum_process_one_index(pvs, pvs->indrels[idx], indstats);
	}

	/*
	 * We have completed the index vacuum so decrement the active worker
	 * count.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * Perform parallel vacuuming of indexes in leader process.
 *
 * Handles index vacuuming (or index cleanup) for indexes that are not
 * parallel safe.  It's possible that this will vary for a given index, based
 * on details like whether we're performing index cleanup right now.
 *
 * Also performs vacuuming of smaller indexes that fell under the size cutoff
 * enforced by parallel_vacuum_compute_workers().
 */
static void
parallel_vacuum_process_unsafe_indexes(ParallelVacuumState *pvs)
{
	Assert(!IsParallelWorker());

	/*
	 * Increment the active worker count if we are able to launch any worker.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_add_fetch_u32(VacuumActiveNWorkers, 1);

	for (int i = 0; i < pvs->nindexes; i++)
	{
		PVIndStats *indstats = &(pvs->indstats[i]);

		/* Skip, indexes that are safe for workers */
		if (indstats->parallel_workers_can_process)
			continue;

		/* Do vacuum or cleanup of the index */
		parallel_vacuum_process_one_index(pvs, pvs->indrels[i], indstats);
	}

	/*
	 * We have completed the index vacuum so decrement the active worker
	 * count.
	 */
	if (VacuumActiveNWorkers)
		pg_atomic_sub_fetch_u32(VacuumActiveNWorkers, 1);
}

/*
 * Vacuum or cleanup index either by leader process or by one of the worker
 * process.  After vacuuming the index this function copies the index
 * statistics returned from ambulkdelete and amvacuumcleanup to the DSM
 * segment.
 */
static void
parallel_vacuum_process_one_index(ParallelVacuumState *pvs, Relation indrel,
								  PVIndStats *indstats)
{
	IndexBulkDeleteResult *istat = NULL;
	IndexBulkDeleteResult *istat_res;
	IndexVacuumInfo ivinfo;
	const int	progress_index[] = {
		PROGRESS_VACUUM_PHASE,
		PROGRESS_VACUUM_CURRENT_INDEX_RELID
	};
	int64		progress_val[2];
	const int	reset_index[] = {
		PROGRESS_VACUUM_CURRENT_INDEX_RELID,
		PROGRESS_SCAN_BLOCKS_TOTAL,
		PROGRESS_SCAN_BLOCKS_DONE
	};
	const int64 reset_val[] = {(int64) InvalidOid, 0, 0};

	/*
	 * Update the pointer to the corresponding bulk-deletion result if someone
	 * has already updated it
	 */
	if (indstats->istat_updated)
		istat = &(indstats->istat);

	ivinfo.index = indrel;
	ivinfo.heaprel = pvs->heaprel;
	ivinfo.analyze_only = false;
	ivinfo.is_autovacuum = pvs->shared->base.is_autovacuum;
	ivinfo.report_progress = true;
	ivinfo.message_level = DEBUG2;
	ivinfo.estimated_count = pvs->shared->estimated_count;
	ivinfo.num_heap_tuples = pvs->shared->reltuples;
	ivinfo.strategy = pvs->bstrategy;

	/* Update error traceback information */
	pvs->indname = pstrdup(RelationGetRelationName(indrel));
	pvs->status = indstats->status;

	/* Report the phase and the index we're about to process */
	progress_val[0] = (indstats->status == PARALLEL_INDVAC_STATUS_NEED_BULKDELETE)
		? PROGRESS_VACUUM_PHASE_VACUUM_INDEX
		: PROGRESS_VACUUM_PHASE_INDEX_CLEANUP;
	progress_val[1] = (int64) RelationGetRelid(indrel);
	pgstat_progress_update_multi_param(2, progress_index, progress_val);

	switch (indstats->status)
	{
		case PARALLEL_INDVAC_STATUS_NEED_BULKDELETE:
			istat_res = vac_bulkdel_one_index(&ivinfo, istat, pvs->dead_items,
											  &pvs->shared->base.dead_items_info);
			break;
		case PARALLEL_INDVAC_STATUS_NEED_CLEANUP:
			istat_res = vac_cleanup_one_index(&ivinfo, istat);
			break;
		default:
			elog(ERROR, "unexpected parallel vacuum index status %d for index \"%s\"",
				 indstats->status,
				 RelationGetRelationName(indrel));
	}

	pgstat_progress_update_multi_param(3, reset_index, reset_val);

	/*
	 * Copy the index bulk-deletion result returned from ambulkdelete and
	 * amvacuumcleanup to the DSM segment if it's the first cycle because they
	 * allocate locally and it's possible that an index will be vacuumed by a
	 * different vacuum process the next cycle.  Copying the result normally
	 * happens only the first time an index is vacuumed.  For any additional
	 * vacuum pass, we directly point to the result on the DSM segment and
	 * pass it to vacuum index APIs so that workers can update it directly.
	 *
	 * Since all vacuum workers write the bulk-deletion result at different
	 * slots we can write them without locking.
	 */
	if (!indstats->istat_updated && istat_res != NULL)
	{
		memcpy(&(indstats->istat), istat_res, sizeof(IndexBulkDeleteResult));
		indstats->istat_updated = true;

		/* Free the locally-allocated bulk-deletion result */
		pfree(istat_res);
	}

	/*
	 * Update the status to completed. No need to lock here since each worker
	 * touches different indexes.
	 */
	indstats->status = PARALLEL_INDVAC_STATUS_COMPLETED;

	/* Reset error traceback information */
	pvs->status = PARALLEL_INDVAC_STATUS_COMPLETED;
	pfree(pvs->indname);
	pvs->indname = NULL;

	/*
	 * Call the parallel variant of pgstat_progress_incr_param so workers can
	 * report progress of index vacuum to the leader.
	 */
	pgstat_progress_parallel_incr_param(PROGRESS_VACUUM_INDEXES_PROCESSED, 1);
}

/*
 * Returns false, if the given index can't participate in the next execution of
 * parallel index vacuum or parallel index cleanup.
 */
static bool
parallel_vacuum_index_is_parallel_safe(Relation indrel, int num_index_scans,
									   bool vacuum)
{
	uint8		vacoptions;

	vacoptions = indrel->rd_indam->amparallelvacuumoptions;

	/* In parallel vacuum case, check if it supports parallel bulk-deletion */
	if (vacuum)
		return ((vacoptions & VACUUM_OPTION_PARALLEL_BULKDEL) != 0);

	/* Not safe, if the index does not support parallel cleanup */
	if (((vacoptions & VACUUM_OPTION_PARALLEL_CLEANUP) == 0) &&
		((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) == 0))
		return false;

	/*
	 * Not safe, if the index supports parallel cleanup conditionally, but we
	 * have already processed the index (for bulkdelete).  We do this to avoid
	 * the need to invoke workers when parallel index cleanup doesn't need to
	 * scan the index.  See the comments for option
	 * VACUUM_OPTION_PARALLEL_COND_CLEANUP to know when indexes support
	 * parallel cleanup conditionally.
	 */
	if (num_index_scans > 0 &&
		((vacoptions & VACUUM_OPTION_PARALLEL_COND_CLEANUP) != 0))
		return false;

	return true;
}

/*
 * Perform work within a launched parallel process.
 *
 * Parallel vacuum workers perform only index vacuum or index cleanup; they
 * report progress for the index they are processing.
 */
void
parallel_vacuum_main(dsm_segment *seg, shm_toc *toc)
{
	ParallelVacuumState pvs;
	Relation	rel;
	Relation   *indrels;
	PVIndStats *indstats;
	PVIndexShared *shared;
	TidStore   *dead_items;
	BufferUsage *buffer_usage;
	WalUsage   *wal_usage;
	int			nindexes;
	char	   *sharedquery;
	ErrorContextCallback errcallback;

	elog(DEBUG1, "starting parallel vacuum worker");

	/* Used by tests to hold a worker while it is attached to its leader */
	INJECTION_POINT("parallel-vacuum-worker-start", NULL);

	shared = (PVIndexShared *) shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_SHARED,
											  false);

	sharedquery = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_QUERY_TEXT, true);

	rel = parallel_vacuum_begin_worker(&shared->base, sharedquery,
									   &dead_items, &pvs.bstrategy);

	/*
	 * Open all indexes. indrels are sorted in order by OID, which should be
	 * matched to the leader's one.
	 */
	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indrels);
	Assert(nindexes > 0);

	/*
	 * Apply the desired value of maintenance_work_mem within this process.
	 * Really we should use SetConfigOption() to change a GUC, but since we're
	 * already in parallel mode guc.c would complain about that.  Fortunately,
	 * by the same token guc.c will not let any user-defined code change it.
	 * So just avert your eyes while we do this:
	 */
	if (shared->maintenance_work_mem_worker > 0)
		maintenance_work_mem = shared->maintenance_work_mem_worker;

	/* Set index statistics */
	indstats = (PVIndStats *) shm_toc_lookup(toc,
											 PARALLEL_VACUUM_KEY_INDEX_STATS,
											 false);

	Assert(dead_items != NULL);

	/* Set parallel vacuum state */
	pvs.indrels = indrels;
	pvs.nindexes = nindexes;
	pvs.indstats = indstats;
	pvs.shared = shared;
	pvs.dead_items = dead_items;
	pvs.relnamespace = get_namespace_name(RelationGetNamespace(rel));
	pvs.relname = pstrdup(RelationGetRelationName(rel));
	pvs.heaprel = rel;

	/* These fields will be filled during index vacuum or cleanup */
	pvs.indname = NULL;
	pvs.status = PARALLEL_INDVAC_STATUS_INITIAL;

	/* Setup error traceback support for ereport() */
	errcallback.callback = parallel_vacuum_error_callback;
	errcallback.arg = &pvs;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* Process indexes to perform vacuum/cleanup */
	parallel_vacuum_process_safe_indexes(&pvs);

	/* Report buffer/WAL usage during parallel execution */
	buffer_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_BUFFER_USAGE, false);
	wal_usage = shm_toc_lookup(toc, PARALLEL_VACUUM_KEY_WAL_USAGE, false);
	InstrEndParallelQuery(&buffer_usage[ParallelWorkerNumber],
						  &wal_usage[ParallelWorkerNumber]);

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;

	vac_close_indexes(nindexes, indrels, RowExclusiveLock);

	parallel_vacuum_end_worker(&shared->base, rel, dead_items,
							   pvs.bstrategy);
}

/*
 * Error context callback for errors occurring during parallel index vacuum.
 * The error context messages should match the messages set in the lazy vacuum
 * error context.  If you change this function, change vacuum_error_callback()
 * as well.
 */
static void
parallel_vacuum_error_callback(void *arg)
{
	ParallelVacuumState *errinfo = arg;

	switch (errinfo->status)
	{
		case PARALLEL_INDVAC_STATUS_NEED_BULKDELETE:
			errcontext("while vacuuming index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname,
					   errinfo->relnamespace,
					   errinfo->relname);
			break;
		case PARALLEL_INDVAC_STATUS_NEED_CLEANUP:
			errcontext("while cleaning up index \"%s\" of relation \"%s.%s\"",
					   errinfo->indname,
					   errinfo->relnamespace,
					   errinfo->relname);
			break;
		case PARALLEL_INDVAC_STATUS_INITIAL:
		case PARALLEL_INDVAC_STATUS_COMPLETED:
		default:
			return;
	}
}
