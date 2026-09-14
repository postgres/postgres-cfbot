/*-------------------------------------------------------------------------
 *
 * lwlock/bench.c
 *		Micro-benchmark of LWLock acquire/release paths.
 *
 * IDENTIFICATION
 *	  src/test/modules/microbench/lwlock/bench.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "portability/instr_time.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "multiprocessing.h"
#include "randomize.h"
#include "timing-magic.h"


PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bench_lwlock);

static void
run_lwlock_bench(int proc_id, int n_parallel, int rounds, int iterations,
				 ReturnSetInfo *rsinfo)
{
	LWLock	  **locks;
	pg_prng_state rng;
	volatile int64 sink PG_USED_FOR_ASSERTS_ONLY = 0;

	(void) n_parallel;

	locks = palloc(sizeof(LWLock *) * iterations);
	pg_prng_seed(&rng, 0xDA7ABA5E ^ (uint64) proc_id * 0x5EED);
	for (int i = 0; i < iterations; i++)
		locks[i] = BufMappingPartitionLock(i);

	if (!timing_initialized)
		pg_initialize_timing();

	for (int64 r = 0; r < rounds; r++)
	{
		INIT_TIMING_SCOPE();

		/*
		 * Shuffling every round to reduce collision distribution bias
		 */
		shuffle_pointers(&rng, (void **) locks, iterations);
		{
			BufferDesc *buf_desc = GetBufferDescriptor(1);

			BEGIN_GROUPED_TIMING("spin-lock", iterations, 2)
				{
					LockBufHdr(buf_desc);
					UnlockBufHdr(buf_desc);
				}
			END_GROUPED_TIMING;
		}

		BEGIN_GROUPED_TIMING("LWLock-ex", iterations, 2)
			{
				LWLock	   *lock = locks[i];

				group_id = LWLockAcquire(lock, LW_EXCLUSIVE) ? 0 : 1;
				LWLockRelease(lock);
			}
		END_GROUPED_TIMING;


		BEGIN_GROUPED_TIMING("LWLock-cond", iterations, 2)
			{
				LWLock	   *lock = locks[i];

				if (LWLockConditionalAcquire(lock, LW_EXCLUSIVE))
				{
					group_id = 0;
					LWLockRelease(lock);
				}
				else
					group_id = 1;
			}
		END_GROUPED_TIMING;

		BEGIN_GROUPED_TIMING("nop", iterations, 2)
			{
				LWLock	   *lock = locks[i];

				sink += (int64) (uintptr_t) lock;
			}
		END_GROUPED_TIMING;
	}

	(void) sink;
}

static void
microbench_parallel_work(int proc_id, int n_parallel, int rounds, int iterations)
{
	run_lwlock_bench(proc_id, n_parallel, rounds, iterations, NULL);
}

/*
 * bench_lwlock(n_parallel, rounds, iterations) -> SETOF
 *     (op text, avg_ns float8, batch_size int8, id int8, group int8)
 */
Datum
bench_lwlock(PG_FUNCTION_ARGS)
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
	run_lwlock_bench(proc_id, n_parallel, rounds, iterations, rsinfo);
	microbench_mp_leave(rsinfo);

	return (Datum) 0;
}
