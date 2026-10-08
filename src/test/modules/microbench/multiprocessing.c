/*-------------------------------------------------------------------------
 *
 * multiprocessing.c
 *		Launch and synchronize micro-benchmark parallel workers.
 *
 * Follows the parallel btree-build pattern: EnterParallelMode,
 * CreateParallelContext, shm_toc, LaunchParallelWorkers,
 * WaitForParallelWorkersToFinish.  Cooperating backends rendezvous with
 * an atomic spin barrier (not BarrierArriveAndWait) so sync stays off
 * the kernel path.  Worker timing rows are stored in DSM and copied
 * into the leader tuplestore at each synchronize and at leave.
 *
 * IDENTIFICATION
 *	  src/test/modules/microbench/multiprocessing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <string.h>

#include "access/parallel.h"
#include "access/xact.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/s_lock.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "multiprocessing.h"

#define PARALLEL_KEY_MICROBENCH_SHARED		UINT64CONST(0xA1100001)
#define MICROBENCH_MAX_OPS_PER_ROUND		32
#define MICROBENCH_OP_LEN					64

typedef struct MicrobenchSample
{
	char		op[MICROBENCH_OP_LEN];
	double		avg_ns;
	int64		batch_size;
	int64		id;
	int64		group;
	bool		group_isnull;
} MicrobenchSample;

typedef struct MicrobenchShared
{
	pg_atomic_uint32 ready;
	pg_atomic_uint32 nsamples;
	pg_atomic_uint32 arrived;
	pg_atomic_uint32 generation;
	int			n_parallel;
	int			rounds;
	int			iterations;
	int			max_group_size;
	int			max_samples;
	ptrdiff_t	work_off;
	MicrobenchSample samples[FLEXIBLE_ARRAY_MEMBER];
} MicrobenchShared;

extern PGDLLEXPORT void microbench_parallel_main(dsm_segment *seg, shm_toc *toc);

static MicrobenchShared *microbench_mp_state = NULL;
static ParallelContext *microbench_mp_pcxt = NULL;
static int	microbench_mp_id = 0;

static void
microbench_mp_flush_samples(ReturnSetInfo *rsinfo)
{
	MicrobenchShared *shared = microbench_mp_state;
	uint32		n;
	uint32		i;
	Datum		values[5];
	bool		nulls[5] = {0};

	if (shared == NULL || rsinfo == NULL || rsinfo->setResult == NULL)
		return;

	n = pg_atomic_read_u32(&shared->nsamples);
	if (n > (uint32) shared->max_samples)
		n = (uint32) shared->max_samples;

	for (i = 0; i < n; i++)
	{
		MicrobenchSample *s = &shared->samples[i];

		values[0] = CStringGetTextDatum(s->op);
		values[1] = Float8GetDatum(s->avg_ns);
		values[2] = Int64GetDatum(s->batch_size);
		values[3] = Int64GetDatum(s->id);
		values[4] = Int64GetDatum(s->group);
		nulls[4] = s->group_isnull;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	if(!pg_atomic_compare_exchange_u32(&shared->nsamples, &n, 0))
	{
		elog(FATAL, "Race condition while flushing results.");
	}
}

static void
microbench_mp_teardown(ReturnSetInfo *rsinfo)
{
	if (IsParallelWorker())
		return;

	if (microbench_mp_pcxt != NULL)
	{
		WaitForParallelWorkersToFinish(microbench_mp_pcxt);
		microbench_mp_flush_samples(rsinfo);
		DestroyParallelContext(microbench_mp_pcxt);
		microbench_mp_pcxt = NULL;
		ExitParallelMode();
	}

	microbench_mp_state = NULL;
	microbench_mp_id = 0;
}

bool
microbench_mp_recording(void)
{
	return microbench_mp_state != NULL && microbench_mp_state->n_parallel > 1;
}

void
microbench_mp_emit_sample(ReturnSetInfo *rsinfo, const char *op,
						  double avg_ns, int64 batch_size, int64 id,
						  int64 group, bool group_isnull)
{
	if (rsinfo == NULL)
	{
		MicrobenchShared *shared = microbench_mp_state;
		uint32		slot;

		slot = pg_atomic_fetch_add_u32(&shared->nsamples, 1);
		if (slot >= (uint32) shared->max_samples)
			elog(ERROR, "microbench sample buffer overflow");

		strlcpy(shared->samples[slot].op, op, MICROBENCH_OP_LEN);
		shared->samples[slot].avg_ns = avg_ns;
		shared->samples[slot].batch_size = batch_size;
		shared->samples[slot].id = id;
		shared->samples[slot].group = group;
		shared->samples[slot].group_isnull = group_isnull;
	}
	else if (rsinfo != NULL && rsinfo->setResult != NULL)
	{
		Datum		values[5];
		bool		nulls[5] = {0};

		values[0] = CStringGetTextDatum(op);
		values[1] = Float8GetDatum(avg_ns);
		values[2] = Int64GetDatum(batch_size);
		values[3] = Int64GetDatum(id);
		values[4] = Int64GetDatum(group);
		nulls[4] = group_isnull;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
}

int
replicate_backend(int n_parallel, int rounds, int iterations, int groups,
				  microbench_parallel_work_fn work)
{
	ParallelContext *pcxt;
	MicrobenchShared *shared;
	int			nworkers 	= n_parallel - 1;
	int			max_samples = nworkers * groups;
	int			shared_size = offsetof(MicrobenchShared, samples)
				+ max_samples * sizeof(MicrobenchSample);
	if (n_parallel <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("n_parallel must be positive")));

	if (n_parallel <= 1)
	{
		microbench_mp_id = 1;
		return 1;
	}

	if (microbench_mp_id != 0)
		return microbench_mp_id;

	EnterParallelMode();
	pcxt = CreateParallelContext("microbench", "microbench_parallel_main",
								 nworkers);
	shm_toc_estimate_chunk(&pcxt->estimator, shared_size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	InitializeParallelDSM(pcxt);

	if (pcxt->seg == NULL)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		microbench_mp_id = 1;
		return 1;
	}

	shared = (MicrobenchShared *) shm_toc_allocate(pcxt->toc, shared_size);
	memset(shared, 0, shared_size);
	pg_atomic_init_u32(&shared->ready, 0);
	pg_atomic_init_u32(&shared->nsamples, 0);
	pg_atomic_init_u32(&shared->arrived, 0);
	pg_atomic_init_u32(&shared->generation, 0);
	shared->max_samples = max_samples;
	shared->n_parallel = n_parallel;
	shared->rounds = rounds;
	shared->iterations = iterations;
	shared->work_off = (uintptr_t) work - (uintptr_t) microbench_parallel_main;
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_MICROBENCH_SHARED, shared);

	LaunchParallelWorkers(pcxt);

	if (pcxt->nworkers_launched < nworkers)
	{
		int			got = pcxt->nworkers_launched;

		DestroyParallelContext(pcxt);
		ExitParallelMode();
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not launch %d parallel workers (got %d)",
						nworkers, got),
				 errhint("Increase max_worker_processes and max_parallel_workers.")));
	}

	microbench_mp_state = shared;
	microbench_mp_pcxt = pcxt;
	microbench_mp_id = 1;
	pg_atomic_write_membarrier_u32(&shared->ready, 1);

	WaitForParallelWorkersToAttach(pcxt);
	return 1;
}

static void
wait_eq(volatile pg_atomic_uint32 *ptr, uint32 value)
{
	uint32		spins = 0;

	while (pg_atomic_read_membarrier_u32(ptr) != value)
	{
		SPIN_DELAY();
		if ((++spins & 0xFFFF) == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

void
synchronize_backends(ReturnSetInfo *rsinfo)
{
	MicrobenchShared *shared = microbench_mp_state;
	uint32		gen;

	if (shared == NULL || shared->n_parallel <= 1)
		return;

	/*
	 * Sense-reversing spin barrier.  Capture the generation first, then
	 * announce arrival.  The leader waits until everyone is here, copies
	 * samples, then advances the generation so waiters return together.
	 */
	gen = pg_atomic_read_u32(&shared->generation);
	pg_atomic_add_fetch_u32(&shared->arrived, 1);

	if (!IsParallelWorker())
	{
		wait_eq(&shared->arrived, (uint32) shared->n_parallel);
		microbench_mp_flush_samples(rsinfo);
		pg_atomic_write_u32(&shared->arrived, 0);
		pg_atomic_write_membarrier_u32(&shared->generation, gen + 1);
	}
	else
		wait_eq(&shared->generation, gen + 1);
}

void
microbench_mp_leave(ReturnSetInfo *rsinfo)
{
	microbench_mp_teardown(rsinfo);
}

/*
 * Parallel-worker entry point.  Looked up by name in the worker process,
 * the same way _bt_parallel_build_main is.
 */
PGDLLEXPORT void
microbench_parallel_main(dsm_segment *seg, shm_toc *toc)
{
	MicrobenchShared *shared;
	microbench_parallel_work_fn work;
	uint32		spins = 0;

	shared = shm_toc_lookup(toc, PARALLEL_KEY_MICROBENCH_SHARED, false);

	while (pg_atomic_read_u32(&shared->ready) == 0)
	{
		SPIN_DELAY();
		if ((++spins & 0xFFFF) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	microbench_mp_state = shared;
	microbench_mp_id = ParallelWorkerNumber + 2;
	work = (microbench_parallel_work_fn)
		((uintptr_t) microbench_parallel_main + shared->work_off);
	work(microbench_mp_id, shared->n_parallel,
		 shared->rounds, shared->iterations);
}
