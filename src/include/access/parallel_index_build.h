/*-------------------------------------------------------------------------
 *
 * parallel_index_build.h
 *	  Shared infrastructure for parallel index builds.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/parallel_index_build.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARALLEL_INDEX_BUILD_H
#define PARALLEL_INDEX_BUILD_H

#include "access/parallel.h"
#include "storage/condition_variable.h"
#include "storage/itemptr.h"
#include "storage/spin.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

struct BufferUsage;
struct IndexInfo;
struct WalUsage;

/* Avoid including tableam.h here */
typedef void (*IndexBuildCallback) (Relation index,
									ItemPointer tid,
									Datum *values,
									bool *isnull,
									bool tupleIsAlive,
									void *state);

/*
 * Shared state common to all parallel index builds.
 *
 * Access methods embed this as the first member of their own shared state
 * and append any AM-specific fields.  The immutable fields are set once by
 * the leader before workers are launched; the mutable fields are maintained
 * by the workers under mutex and read back by the leader once all workers
 * have signalled completion.
 */
typedef struct ParallelIndexBuildShared
{
	/* Immutable state, set by the leader before launching workers */
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	int			scantuplesortstates;

	/* Query ID, for report in worker processes */
	int64		queryid;

	/*
	 * workersdonecv is used to monitor the progress of workers.  All parallel
	 * participants must indicate that they are done before the leader can use
	 * the results built by the workers.
	 */
	ConditionVariable workersdonecv;

	/*
	 * mutex protects the mutable fields below.
	 */
	slock_t		mutex;

	/*
	 * Mutable state that is maintained by workers for all index types, and
	 * reported back to leader at end of the scans.
	 *
	 * nparticipantsdone is the number of worker processes finished.
	 *
	 * reltuples is the total number of input heap tuples.
	 *
	 * indtuples is the total number of tuples that made it into the index.
	 *
	 * brokenhotchain indicates if any participant detected a broken HOT chain
	 * during build.  Set by ParallelIndexBuildScan() and propagated to the
	 * IndexInfo passed to ambuild by ParallelIndexBuildWaitForWorkers(), so
	 * that index_build() marks the index with indcheckxmin.
	 */
	int			nparticipantsdone;
	double		reltuples;
	double		indtuples;
	bool		brokenhotchain;

	/*
	 * Mutable state that is only maintained by nbtree, and left false by the
	 * other AMs.
	 *
	 * havedead indicates if RECENTLY_DEAD tuples were encountered during
	 * build.  When building a unique index, nbtree sorts dead tuples in a
	 * separate spool to keep them out of the uniqueness check, and uses this
	 * to skip sorting and merging that spool if it turned out to be empty.
	 */
	bool		havedead;
} ParallelIndexBuildShared;

/* Leader-side helpers */
extern ParallelContext *ParallelIndexBuildCreateContext(const char *library_name,
														const char *worker_function,
														int nworkers);
extern Snapshot ParallelIndexBuildGetSnapshot(bool isconcurrent);
extern void ParallelIndexBuildEstimate(ParallelContext *pcxt, Relation heap,
									   Snapshot snapshot);
extern void ParallelIndexBuildInitShared(ParallelContext *pcxt,
										 ParallelIndexBuildShared *shared,
										 uint64 pscan_key,
										 Relation heap, Relation index,
										 bool isconcurrent,
										 int scantuplesortstates,
										 Snapshot snapshot);
extern void ParallelIndexBuildWaitForWorkers(ParallelIndexBuildShared *shared,
											 int nparticipants,
											 struct IndexInfo *indexInfo,
											 double *reltuples, double *indtuples,
											 bool *havedead);
extern void ParallelIndexBuildAbort(ParallelContext *pcxt, Snapshot snapshot);
extern void ParallelIndexBuildEnd(ParallelContext *pcxt,
								  struct BufferUsage *bufferusage,
								  struct WalUsage *walusage,
								  Snapshot snapshot);

/* Worker-side helpers (also used by the leader when it participates) */
extern double ParallelIndexBuildScan(ParallelIndexBuildShared *shared,
									 shm_toc *toc, uint64 pscan_key,
									 Relation heap, Relation index,
									 bool progress,
									 IndexBuildCallback callback,
									 void *callback_state);
extern void ParallelIndexBuildReportScanDone(ParallelIndexBuildShared *shared,
											 double reltuples, double indtuples,
											 bool havedead);
extern void ParallelIndexBuildOpenRelations(ParallelIndexBuildShared *shared,
											Relation *heapRel, Relation *indexRel);
extern void ParallelIndexBuildCloseRelations(ParallelIndexBuildShared *shared,
											 Relation heapRel, Relation indexRel);

#endif							/* PARALLEL_INDEX_BUILD_H */
