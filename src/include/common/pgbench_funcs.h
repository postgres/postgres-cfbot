/*-------------------------------------------------------------------------
 *
 * pgbench_funcs.h
 *	  Shared random distribution, permutation, and hashing functions for
 *	  pgbench and backend extensions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/pgbench_funcs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGBENCH_FUNCS_H
#define PGBENCH_FUNCS_H

#include "common/pg_prng.h"

/* Parameter boundaries for statistical distributions */
#define PGBENCH_MIN_GAUSSIAN_PARAM		2.0
#define PGBENCH_MIN_ZIPFIAN_PARAM		1.001
#define PGBENCH_MAX_ZIPFIAN_PARAM		1000.0

/* Hashing Constants */
#define PGBENCH_FNV_PRIME				UINT64CONST(0x100000001b3)
#define PGBENCH_FNV_OFFSET_BASIS		UINT64CONST(0xcbf29ce484222325)
#define PGBENCH_MM2_MUL					UINT64CONST(0xc6a4a7935bd1e995)
#define PGBENCH_MM2_MUL_TIMES_8			UINT64CONST(0x35253c9ade8f4ca8)
#define PGBENCH_MM2_ROT					47

/* Random Distribution Functions */
extern int64 pgbench_random(pg_prng_state *state, int64 min, int64 max);
extern int64 pgbench_random_gaussian(pg_prng_state *state, int64 min, int64 max,
									 double parameter);
extern int64 pgbench_random_exponential(pg_prng_state *state, int64 min, int64 max,
										double parameter);
extern int64 pgbench_random_zipfian(pg_prng_state *state, int64 min, int64 max,
									double s);
extern int64 pgbench_random_poisson(pg_prng_state *state, double center);

/* Hashing Functions */
extern int64 pgbench_hash_fnv1a(int64 val, uint64 seed);
extern int64 pgbench_hash_murmur2(int64 val, uint64 seed);

/* Permutation Function */
extern int64 pgbench_permute(int64 val, int64 isize, int64 seed);

/* TPC-C Non-Uniform Random (NURand) and Syllable Generation */
extern int64 pgbench_nurand(pg_prng_state *state, int64 A, int64 min, int64 max, int64 C);
extern bool pgbench_c_last(int64 num, char *buf, size_t buflen);

/* Random Alphanumeric, Numeric, and Binary Payload Generation */
extern void pgbench_random_astring(pg_prng_state *state, int64 min_len, int64 max_len, char *buf);
extern void pgbench_random_nstring(pg_prng_state *state, int64 min_len, int64 max_len, char *buf);
extern int64 pgbench_random_bytea(pg_prng_state *state, int64 min_len, int64 max_len, uint8 *buf);

/* Timestamp / Date Parsing, Formatting, and Arithmetic (microseconds since 2000-01-01) */
extern bool pgbench_parse_timestamp(const char *str, int64 *usec_out);
extern void pgbench_format_timestamp(int64 usec, char *buf, size_t buflen);
extern bool pgbench_date_add(int64 base_usec, int64 offset, const char *unit, int64 *result_usec);
extern bool pgbench_int_to_date_window(int64 base_usec, int64 val, int64 max_val,
									   const char *interval_str, int64 *result_usec);

/* Vector Distance Metrics */
typedef enum
{
	PGBENCH_VEC_DIST_L2 = 0,
	PGBENCH_VEC_DIST_COSINE,
	PGBENCH_VEC_DIST_IP
} PgBenchVecDistMetric;

#define PGBENCH_MAX_VECTOR_DIM			65536

/* Multi-Modal Vector, Sparse Vector, and Graph Edge Generators */
extern void pgbench_normalize_vector(float *vec, int dim);
extern void pgbench_random_vector(pg_prng_state *state, int dim,
								  double min_val, double max_val,
								  bool normalize, float *out);
extern void pgbench_random_vector_gaussian(pg_prng_state *state, int dim,
										   double sigma, bool normalize,
										   float *out);
extern void pgbench_clustered_vector(pg_prng_state *state, int dim,
									 int num_clusters, double sigma,
									 int64 seed, int cluster_id, float *out);
extern void pgbench_planted_vector(pg_prng_state *state, int dim,
								   int64 target_id, double delta,
								   int64 seed, float *out);
extern bool pgbench_random_sparse_vector(pg_prng_state *state, int dim,
										 int nnz, double min_val, double max_val,
										 char *buf, size_t buflen);
extern bool pgbench_rmat_edge(pg_prng_state *state, int scale_log2,
							  double a, double b, double c, int64 seed,
							  int64 *src_out, int64 *dst_out);
extern bool pgbench_parse_vec_dist_metric(const char *metric_str,
										  PgBenchVecDistMetric *metric_out);
extern double pgbench_vector_distance(const float *v1, const float *v2,
									  int dim, PgBenchVecDistMetric metric);
extern size_t pgbench_format_vector(const float *vec, int dim,
									char *buf, size_t buflen);
extern bool pgbench_parse_vector_count(const char *str, int *dim_out);
extern bool pgbench_parse_vector_into(const char *str, float *out, int expected_dim);

#endif							/* PGBENCH_FUNCS_H */


