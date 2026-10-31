/*-------------------------------------------------------------------------
 *
 * parallel_index_build.c
 *	  Shared infrastructure for parallel index builds.
 *
 * This file contains the access-method-independent parts of the parallel
 * index build lifecycle shared by different index access methods: setting up
 * the parallel context and shared state, and opening/closing relations and
 * tearing everything down again in the worker and leader.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/parallel_index_build.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/parallel_index_build.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "executor/instrument.h"
#include "pgstat.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

/*
 * Enter parallel mode and create a parallel context for an index build, using
 * the named worker entry point in the named library (as for
 * CreateParallelContext; in-core access methods pass "postgres").
 */
ParallelContext *
ParallelIndexBuildCreateContext(const char *library_name,
								const char *worker_function, int nworkers)
{
	Assert(nworkers > 0);

	EnterParallelMode();

	return CreateParallelContext(library_name, worker_function, nworkers);
}

/*
 * Choose the snapshot for the heap scan of an index build.
 *
 * In a normal index build, we use SnapshotAny because we must retrieve all
 * tuples and do our own time qual checks (because we have to index
 * RECENTLY_DEAD tuples).  In a concurrent build, we take a regular MVCC
 * snapshot and index whatever's live according to that.  The caller is
 * responsible for unregistering an MVCC snapshot by calling
 * ParallelIndexBuildEnd.
 */
Snapshot
ParallelIndexBuildGetSnapshot(bool isconcurrent)
{
	if (!isconcurrent)
		return SnapshotAny;

	return RegisterSnapshot(GetTransactionSnapshot());
}

/*
 * Estimate the DSM space and keys this module needs for a parallel index
 * build, namely the parallel table scan descriptor for the heap scan.  The
 * access method's own shared struct (which embeds ParallelIndexBuildShared)
 * is estimated by the caller, like any other AM-specific chunk.
 *
 * Must be called before InitializeParallelDSM.
 */
void
ParallelIndexBuildEstimate(ParallelContext *pcxt, Relation heap,
						   Snapshot snapshot)
{
	shm_toc_estimate_chunk(&pcxt->estimator,
						   table_parallelscan_estimate(heap, snapshot));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/*
 * Initialize the common shared state for a parallel index build.
 *
 * The caller has already allocated the embedding shared struct in DSM; this
 * fills in the common header.  It also allocates and initializes the parallel
 * heap scan descriptor, for which ParallelIndexBuildEstimate reserved space,
 * and stores it in the TOC under pscan_key (one of the caller's own keys).
 * Participants join the scan through ParallelIndexBuildScan(), which also
 * takes care of reporting broken HOT chains.  AM-specific fields are the
 * caller's responsibility.
 */
void
ParallelIndexBuildInitShared(ParallelContext *pcxt,
							 ParallelIndexBuildShared *shared,
							 uint64 pscan_key,
							 Relation heap, Relation index,
							 bool isconcurrent, int scantuplesortstates,
							 Snapshot snapshot)
{
	ParallelTableScanDesc pscan;

	/* Initialize immutable state */
	shared->heaprelid = RelationGetRelid(heap);
	shared->indexrelid = RelationGetRelid(index);
	shared->isconcurrent = isconcurrent;
	shared->scantuplesortstates = scantuplesortstates;
	shared->queryid = pgstat_get_my_query_id();
	ConditionVariableInit(&shared->workersdonecv);
	SpinLockInit(&shared->mutex);

	/* Initialize mutable state */
	shared->nparticipantsdone = 0;
	shared->reltuples = 0.0;
	shared->indtuples = 0.0;
	shared->brokenhotchain = false;
	shared->havedead = false;

	/* Set up the parallel heap scan, in its own TOC entry */
	pscan = (ParallelTableScanDesc)
		shm_toc_allocate(pcxt->toc,
						 table_parallelscan_estimate(heap, snapshot));
	table_parallelscan_initialize(heap, pscan, snapshot);
	shm_toc_insert(pcxt->toc, pscan_key, pscan);
}

/*
 * Wait, in the leader, for all participants to finish their portion of the
 * scan, then read back the accumulated per-build results.
 *
 * reltuples and indtuples receive the totals.  indexInfo must be the IndexInfo
 * passed to ambuild: if any participant's scan detected a broken HOT chain,
 * its ii_BrokenHotChain is set, so that index_build() marks the index with
 * indcheckxmin.  havedead is optional (pass NULL when the AM does not track
 * it): it reports whether any worker saw RECENTLY_DEAD tuples.
 */
void
ParallelIndexBuildWaitForWorkers(ParallelIndexBuildShared *shared,
								 int nparticipants, IndexInfo *indexInfo,
								 double *reltuples, double *indtuples,
								 bool *havedead)
{
	for (;;)
	{
		SpinLockAcquire(&shared->mutex);
		if (shared->nparticipantsdone == nparticipants)
		{
			*reltuples = shared->reltuples;
			*indtuples = shared->indtuples;
			if (havedead)
				*havedead = shared->havedead;
			if (shared->brokenhotchain)
				indexInfo->ii_BrokenHotChain = true;
			SpinLockRelease(&shared->mutex);
			break;
		}
		SpinLockRelease(&shared->mutex);

		ConditionVariableSleep(&shared->workersdonecv,
							   WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
	}

	ConditionVariableCancelSleep();
}

/*
 * Back out of a parallel index build before any workers were launched, for
 * example because InitializeParallelDSM could not create a DSM segment.  The
 * caller then proceeds with a serial build.
 *
 * Releases the MVCC snapshot if one was used, destroys the parallel context,
 * and exits parallel mode.  Once workers have been launched, use
 * ParallelIndexBuildEnd instead.
 */
void
ParallelIndexBuildAbort(ParallelContext *pcxt, Snapshot snapshot)
{
	Assert(pcxt->nworkers_launched == 0);

	if (IsMVCCSnapshot(snapshot))
		UnregisterSnapshot(snapshot);
	DestroyParallelContext(pcxt);
	ExitParallelMode();
}

/*
 * Shut down the workers and tear down a parallel index build.
 *
 * Waits for all workers to finish, accumulates their buffer/WAL usage into the
 * leader's stats, releases the MVCC snapshot if one was used, and exits
 * parallel mode.  bufferusage and walusage are the per-worker arrays the
 * leader allocated in DSM, which each worker filled in via
 * InstrEndParallelQuery.
 */
void
ParallelIndexBuildEnd(ParallelContext *pcxt, BufferUsage *bufferusage,
					  WalUsage *walusage, Snapshot snapshot)
{
	/* Shutdown worker processes */
	WaitForParallelWorkersToFinish(pcxt);

	/*
	 * Next, accumulate WAL and buffer usage.  (This must wait for the workers
	 * to finish, or we might get incomplete data.)
	 */
	for (int i = 0; i < pcxt->nworkers_launched; i++)
		InstrAccumParallelQuery(&bufferusage[i], &walusage[i]);

	/* Free last reference to MVCC snapshot, if one was used */
	if (IsMVCCSnapshot(snapshot))
		UnregisterSnapshot(snapshot);
	DestroyParallelContext(pcxt);
	ExitParallelMode();
}

/*
 * Select the lock modes used by a parallel index build worker.  These match
 * the lock modes known to be obtained by index.c in the leader, which differ
 * for concurrent builds.
 */
static void
ParallelIndexBuildLockModes(ParallelIndexBuildShared *shared,
							LOCKMODE *heapLockmode, LOCKMODE *indexLockmode)
{
	if (!shared->isconcurrent)
	{
		*heapLockmode = ShareLock;
		*indexLockmode = AccessExclusiveLock;
	}
	else
	{
		*heapLockmode = ShareUpdateExclusiveLock;
		*indexLockmode = RowExclusiveLock;
	}
}

/*
 * Open the heap and index relations in a parallel index build worker.
 *
 * Reports the query ID and opens both relations with the lock modes used by
 * the leader.  Must be paired with ParallelIndexBuildCloseRelations.
 */
void
ParallelIndexBuildOpenRelations(ParallelIndexBuildShared *shared,
								Relation *heapRel, Relation *indexRel)
{
	LOCKMODE	heapLockmode;
	LOCKMODE	indexLockmode;

	ParallelIndexBuildLockModes(shared, &heapLockmode, &indexLockmode);

	/* Track query ID */
	pgstat_report_query_id(shared->queryid, false);

	/* Open relations within worker */
	*heapRel = table_open(shared->heaprelid, heapLockmode);
	*indexRel = index_open(shared->indexrelid, indexLockmode);
}

/*
 * Close the relations opened by ParallelIndexBuildOpenRelations.
 */
void
ParallelIndexBuildCloseRelations(ParallelIndexBuildShared *shared,
								 Relation heapRel, Relation indexRel)
{
	LOCKMODE	heapLockmode;
	LOCKMODE	indexLockmode;

	ParallelIndexBuildLockModes(shared, &heapLockmode, &indexLockmode);

	index_close(indexRel, indexLockmode);
	table_close(heapRel, heapLockmode);
}

/*
 * Run a participant's portion of the parallel heap scan.
 *
 * Builds the IndexInfo for the scan, joins the shared parallel scan and runs
 * table_index_build_scan() with the given callback, as a serial build would.
 * Returns the number of heap tuples this participant scanned.
 *
 * toc is the parallel build's table of contents: the one passed to the worker
 * entry point in a worker, or the ParallelContext's in the leader.  pscan_key
 * must be the key passed to ParallelIndexBuildInitShared.
 *
 * If the scan detected a broken HOT chain, that is recorded in the shared
 * state here, so that ParallelIndexBuildWaitForWorkers() can propagate it to
 * the IndexInfo passed to ambuild.  Doing this here rather than in
 * ParallelIndexBuildReportScanDone() means an AM cannot forget to report it.
 * It is safe to set the flag before the participant reports done, since the
 * leader only reads it once every participant has done so.
 */
double
ParallelIndexBuildScan(ParallelIndexBuildShared *shared, shm_toc *toc,
					   uint64 pscan_key, Relation heap, Relation index,
					   bool progress, IndexBuildCallback callback,
					   void *callback_state)
{
	ParallelTableScanDesc pscan;
	IndexInfo  *indexInfo;
	TableScanDesc scan;
	double		reltuples;

	indexInfo = BuildIndexInfo(index);
	indexInfo->ii_Concurrent = shared->isconcurrent;

	pscan = shm_toc_lookup(toc, pscan_key, false);
	scan = table_beginscan_parallel(heap, pscan, SO_NONE);
	reltuples = table_index_build_scan(heap, index, indexInfo, true, progress,
									   callback, callback_state, scan);

	if (indexInfo->ii_BrokenHotChain)
	{
		SpinLockAcquire(&shared->mutex);
		shared->brokenhotchain = true;
		SpinLockRelease(&shared->mutex);
	}

	return reltuples;
}

/*
 * Report that a participant (a worker, or the leader when it participates as
 * a worker) has finished its portion of the scan.
 *
 * Accumulates the participant's tuple counts into the shared totals, ORs in
 * the havedead flag (only tracked by nbtree; others pass false), and signals
 * the leader.  Must be paired with ParallelIndexBuildWaitForWorkers in the
 * leader.  Broken HOT chains are reported by ParallelIndexBuildScan() instead.
 */
void
ParallelIndexBuildReportScanDone(ParallelIndexBuildShared *shared,
								 double reltuples, double indtuples,
								 bool havedead)
{
	SpinLockAcquire(&shared->mutex);
	shared->nparticipantsdone++;
	shared->reltuples += reltuples;
	shared->indtuples += indtuples;
	if (havedead)
		shared->havedead = true;
	SpinLockRelease(&shared->mutex);

	/* Notify leader */
	ConditionVariableSignal(&shared->workersdonecv);
}
