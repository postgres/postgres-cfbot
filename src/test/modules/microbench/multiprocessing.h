#ifndef MICROBENCH_MULTIPROCESSING_H
#define MICROBENCH_MULTIPROCESSING_H

struct ReturnSetInfo;

typedef void (*microbench_parallel_work_fn) (int proc_id, int n_parallel,
											 int rounds, int iterations);

/*
 * Launch n-1 parallel workers (same infrastructure as parallel index
 * builds) and return 1 in the leader.  Workers enter through
 * microbench_parallel_main and call the work function passed here.
 *
 * synchronize_backends() spins until the whole party has arrived, the
 * leader flushes new DSM samples, then it releases everyone.  It is a
 * no-op when n_parallel <= 1.  microbench_mp_leave() waits for workers
 * and flushes the last batch, then exits parallel mode.
 */
extern int	replicate_backend(int n_parallel, int rounds, int iterations,
							  int samples_per_round,
							  microbench_parallel_work_fn work);
extern void synchronize_backends(struct ReturnSetInfo *rsinfo);
extern void microbench_mp_leave(struct ReturnSetInfo *rsinfo);
extern bool microbench_mp_recording(void);
extern void microbench_mp_emit_sample(struct ReturnSetInfo *rsinfo,
									  const char *op, double avg_ns,
									  int64 batch_size, int64 id,
									  int64 group, bool group_isnull);

#endif
