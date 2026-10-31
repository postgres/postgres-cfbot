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
 * Estimate the DSM space for the shared state struct of a parallel index
 * build, including the parallel table scan descriptor that trails it.
 *
 * am_shared_size is sizeof() the access method's whole shared struct (which
 * embeds ParallelIndexBuildShared as its first member).
 */
Size
ParallelIndexBuildEstimateShared(Relation heap, Snapshot snapshot,
								 Size am_shared_size)
{
	/* c.f. shm_toc_allocate as to why BUFFERALIGN is used */
	return add_size(BUFFERALIGN(am_shared_size),
					table_parallelscan_estimate(heap, snapshot));
}

/*
 * Initialize the common shared state for a parallel index build.
 *
 * The caller has already allocated the embedding shared struct in DSM; this
 * fills in the common header and initializes the parallel heap scan
 * descriptor that follows the (whole) embedding struct, which the caller
 * passes as pscan.  AM-specific fields are the caller's responsibility.
 */
void
ParallelIndexBuildInitShared(ParallelIndexBuildShared *shared,
							 Relation heap, Relation index,
							 bool isconcurrent, int scantuplesortstates,
							 ParallelTableScanDesc pscan, Snapshot snapshot)
{
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

	table_parallelscan_initialize(heap, pscan, snapshot);
}

/*
 * Wait, in the leader, for all participants to finish their portion of the
 * scan, then read back the accumulated per-build results.
 *
 * reltuples and indtuples receive the totals.  brokenhotchain receives whether
 * any participant saw a broken HOT chain; the caller must propagate it to the
 * IndexInfo passed to ambuild.  havedead is optional (pass NULL when the AM
 * does not track it): it reports whether any worker saw RECENTLY_DEAD tuples.
 */
void
ParallelIndexBuildWaitForWorkers(ParallelIndexBuildShared *shared,
								 int nparticipants,
								 double *reltuples, double *indtuples,
								 bool *havedead, bool *brokenhotchain)
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
			if (brokenhotchain)
				*brokenhotchain = shared->brokenhotchain;
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
 * Report that a participant (a worker, or the leader when it participates as
 * a worker) has finished its portion of the scan.
 *
 * Accumulates the participant's tuple counts into the shared totals, ORs in
 * the havedead/brokenhotchain flags, and signals the leader.  brokenhotchain
 * must be the ii_BrokenHotChain of the IndexInfo used for the participant's
 * heap scan; havedead is only tracked by nbtree (others pass false).  Must be
 * paired with ParallelIndexBuildWaitForWorkers in the leader.
 */
void
ParallelIndexBuildReportScanDone(ParallelIndexBuildShared *shared,
								 double reltuples, double indtuples,
								 bool havedead, bool brokenhotchain)
{
	SpinLockAcquire(&shared->mutex);
	shared->nparticipantsdone++;
	shared->reltuples += reltuples;
	shared->indtuples += indtuples;
	if (havedead)
		shared->havedead = true;
	if (brokenhotchain)
		shared->brokenhotchain = true;
	SpinLockRelease(&shared->mutex);

	/* Notify leader */
	ConditionVariableSignal(&shared->workersdonecv);
}
