#ifndef MICROBENCH_TIMING_MAGIC_H
#define MICROBENCH_TIMING_MAGIC_H

#include <string.h>

#include "portability/instr_time.h"
#include "multiprocessing.h"

#define INIT_TIMING_SCOPE() \
	int64 timing_operation_id = 0

#define BEGIN_TIMING(name, iterations) \
	do { \
		instr_time t0, t1, dt; \
		const char *timing_name = (name); \
		double		avg_ns; \
		synchronize_backends(rsinfo); \
		INSTR_TIME_SET_CURRENT(t0); \
		for (int64 i = 0; i < (iterations); ++i) \
		{

#define END_TIMING \
		} \
		INSTR_TIME_SET_CURRENT(t1); \
		INSTR_TIME_SET_ZERO(dt); \
		INSTR_TIME_ACCUM_DIFF(dt, t1, t0); \
		avg_ns = (double) INSTR_TIME_GET_NANOSEC(dt) / (double) (iterations); \
		microbench_mp_emit_sample(rsinfo, timing_name, avg_ns, (iterations), \
								  ++timing_operation_id, 0, true); \
	} while (0)

#define BEGIN_GROUPED_TIMING(name, iterations, n_groups) \
	do { \
		int64		_n_groups = (n_groups); \
		int64		_n = (iterations); \
		int64		group_count[n_groups]; \
		instr_time	group_dt[n_groups]; \
		instr_time t0, t1; \
		const char *timing_name = (name); \
		int64		g; \
		int64		i; \
		memset(group_count, 0, sizeof(group_count)); \
		for (g = 0; g < _n_groups; g++) \
			INSTR_TIME_SET_ZERO(group_dt[g]); \
		synchronize_backends(rsinfo); \
		for (i = 0; i < _n; ++i) \
		{ \
			int64		group_id = 0; \
			INSTR_TIME_SET_CURRENT(t0);

#define END_GROUPED_TIMING \
			INSTR_TIME_SET_CURRENT(t1); \
			if (group_id < 0 || group_id >= _n_groups) \
				elog(ERROR, \
					 "group_id " INT64_FORMAT " out of range [0, " INT64_FORMAT ")", \
					 group_id, _n_groups); \
			INSTR_TIME_ACCUM_DIFF(group_dt[group_id], t1, t0); \
			group_count[group_id]++; \
		} \
		for (g = 0; g < _n_groups; g++) \
		{ \
			int64		cnt; \
			double		avg_ns; \
			cnt = group_count[g]; \
			if (cnt == 0) \
				continue; \
			avg_ns = (double) INSTR_TIME_GET_NANOSEC(group_dt[g]) / (double) cnt; \
			microbench_mp_emit_sample(rsinfo, timing_name, avg_ns, cnt, \
									  ++timing_operation_id, g, false); \
		} \
	} while (0)

#endif
