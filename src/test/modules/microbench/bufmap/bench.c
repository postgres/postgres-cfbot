/*-------------------------------------------------------------------------
 *
 * bufmap/bench.c
 *		Micro-benchmark BufTable insert/lookup/delete with synthetic tags.
 *		No anchor table required - uses free buffer slots directly.
 *
 * IDENTIFICATION
 *	  src/test/modules/microbench/bufmap/bench.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/namespace.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "multiprocessing.h"
#include "randomize.h"
#include "timing-magic.h"

/* synthetic relfilenodes for bench tags — unlikely to collide with anything real */
#define BENCH_SPC_OID  0xB0B0
#define BENCH_DB_OID   0xB1B1
#define BENCH_REL_PRESENT  ((RelFileNumber) 0x7E570001)
#define BENCH_REL_ABSENT   ((RelFileNumber) 0x7E570002)

PG_FUNCTION_INFO_V1(bench_bufmap);

static void
run_bufmap_bench(int proc_id, int n_parallel, int rounds, int iterations,
				 ReturnSetInfo *rsinfo)
{
	intptr_t	   *blks;
	BufferTag	   *ptags;
	BufferTag	   *atags;
	intptr_t	   *bufids;
	pg_prng_state	rng;
	int				nfree = 0;
	LWLock         *arbitrary_lock;
	volatile int64 sink PG_USED_FOR_ASSERTS_ONLY = 0;
	int				start_buf;
	RelFileLocator rp = {.spcOid = BENCH_SPC_OID, .dbOid = BENCH_DB_OID, .relNumber = BENCH_REL_PRESENT};
	RelFileLocator ra = {.spcOid = BENCH_SPC_OID, .dbOid = BENCH_DB_OID, .relNumber = BENCH_REL_ABSENT};

	blks = palloc(sizeof(intptr_t) * iterations);
	ptags = palloc0(sizeof(BufferTag) * iterations);
	atags = palloc0(sizeof(BufferTag) * iterations);
	bufids = palloc(sizeof(intptr_t) * iterations);
	pg_prng_seed(&rng, 0xB0FF0A00 ^ (uint64)proc_id);

	/*
	 * Each worker collects buffer IDs from a distinct range to avoid spinlock
	 * contention. Worker 1 starts at buffer 0, worker 2 at iterations, etc.
	 * We need n_parallel * iterations total free buffers.
	 */
	start_buf = (proc_id - 1) * iterations;

	elog(LOG, "Worker %d: starting buffer collection from %d", proc_id, start_buf);

	arbitrary_lock = BufMappingPartitionLock(0);
	LWLockAcquire(arbitrary_lock, LW_EXCLUSIVE);
	for (int i = start_buf; i < NBuffers && nfree < iterations; i++)
	{
		BufferDesc *desc = GetBufferDescriptor(i);
		uint64		state = pg_atomic_read_u64(&desc->state);

		if (!(state & (BM_TAG_VALID | BM_LOCKED | BUF_REFCOUNT_MASK)))
		{
			state = LockBufHdr(desc);
			UnlockBufHdrExt(desc, state, 0, 0, 1);
			bufids[nfree++] = (intptr_t) i;
		}
	}
	LWLockRelease(arbitrary_lock);
	elog(LOG, "Worker %d: collected %d buffers", proc_id, nfree);

	if (nfree < iterations)
		goto teardown;

	for (int i = 0; i < iterations; ++i)
	{
		int idx = n_parallel * i + proc_id - 1;
		blks[i] = (intptr_t) i;

		InitBufferTag(&ptags[i], &rp, MAIN_FORKNUM, (BlockNumber) idx);
		InitBufferTag(&atags[i], &ra, MAIN_FORKNUM, (BlockNumber) idx);
	}

	if (!timing_initialized)
		pg_initialize_timing();

	elog(LOG, "Worker %d: setup complete, entering timing loops", proc_id);

	for (int64 r = 0; r < rounds; r++)
	{
		INIT_TIMING_SCOPE();
		shuffle_pointers(&rng, (void **) blks, iterations);
		shuffle_pointers(&rng, (void **) bufids, iterations);

		BEGIN_GROUPED_TIMING("insert", iterations, 2)
			{
				BufferTag  *tag = &ptags[blks[i]];
				uint32		hash;
				LWLock	   *lock;

				hash = BufTableHashCode(tag);
				lock = BufMappingPartitionLock(hash);
				group_id = LWLockAcquire(lock, LW_EXCLUSIVE) ? 0 : 1;
				sink += BufTableInsert(tag, hash, (Buffer)bufids[i]);
				LWLockRelease(lock);
			}
		END_GROUPED_TIMING;

		BEGIN_GROUPED_TIMING("hit", iterations, 2)
			{
				BufferTag  *tag = &ptags[blks[i]];
				uint32		hash;
				LWLock	   *lock;

				hash = BufTableHashCode(tag);
				lock = BufMappingPartitionLock(hash);
				group_id = LWLockAcquire(lock, LW_SHARED) ? 0 : 1;
				BufTableLookup(tag, hash);
				LWLockRelease(lock);
			}
		END_GROUPED_TIMING;

		BEGIN_GROUPED_TIMING("miss", iterations, 2)
			{
				BufferTag  *tag = &atags[blks[i]];
				uint32		hash;
				LWLock	   *lock;

				hash = BufTableHashCode(tag);
				lock = BufMappingPartitionLock(hash);
				group_id = LWLockAcquire(lock, LW_SHARED) ? 0 : 1;
				BufTableLookup(tag, hash);
				LWLockRelease(lock);
			}
		END_GROUPED_TIMING;

		BEGIN_GROUPED_TIMING("delete", iterations, 2)
			{
				BufferTag  *tag = &ptags[blks[i]];
				uint32		hash;
				LWLock	   *lock;

				hash = BufTableHashCode(tag);
				lock = BufMappingPartitionLock(hash);
				group_id = LWLockAcquire(lock, LW_EXCLUSIVE) ? 0 : 1;
				BufTableDelete(tag, hash);
				LWLockRelease(lock);
			}
		END_GROUPED_TIMING;

		BEGIN_GROUPED_TIMING("LWLock", iterations, 2)
			{
				BufferTag  *tag = &ptags[blks[i]];
				uint32		hash;
				LWLock	   *lock;

				hash = BufTableHashCode(tag);
				lock = BufMappingPartitionLock(hash);
				group_id = LWLockAcquire(lock, LW_EXCLUSIVE) ? 0 : 1;
				LWLockRelease(lock);
			}
		END_GROUPED_TIMING;
	}

teardown:
	for (int i = 0; i < nfree; ++i){
		BufferDesc *desc = GetBufferDescriptor(bufids[i]);
		uint64 state = LockBufHdr(desc);
		UnlockBufHdrExt(desc, state, 0, 0, -1);
	}

	pfree(bufids);
	pfree(ptags);
	pfree(atags);
	pfree(blks);
	(void) sink;
	if(nfree < iterations)
		elog(ERROR, "Couldn't get enough free buffers");
}

static void
microbench_parallel_work(int proc_id, int n_parallel, int rounds, int iterations)
{
	run_bufmap_bench(proc_id, n_parallel, rounds, iterations, NULL);
}

/*
 * bench_bufmap(n_parallel, rounds, iterations) -> SETOF microbench_sample
 */
Datum
bench_bufmap(PG_FUNCTION_ARGS)
{
	int			n_parallel = PG_ARGISNULL(0) ? 1 : PG_GETARG_INT32(0);
	int			rounds = PG_ARGISNULL(1) ? 1 : PG_GETARG_INT64(1);
	int			iterations = PG_ARGISNULL(2) ? 128 : PG_GETARG_INT64(2);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			proc_id;

	if (n_parallel <= 0 || rounds <= 0 || iterations <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("n_parallel, rounds, and iterations must be positive")));

	InitMaterializedSRF(fcinfo, 0);

	proc_id = replicate_backend(n_parallel, rounds, iterations, 2,
								microbench_parallel_work);
	run_bufmap_bench(proc_id, n_parallel, rounds, iterations, rsinfo);
	microbench_mp_leave(rsinfo);

	return (Datum) 0;
}
