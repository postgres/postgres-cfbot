CREATE EXTENSION pgbench;

-- Test Hashing functions
SELECT pgbench.hash_murmur2(12345, 0);
SELECT pgbench.hash_murmur2(12345, 42);
SELECT pgbench.hash_fnv1a(12345, 0);
SELECT pgbench.hash_fnv1a(12345, 42);
-- pgbench.hash is alias for hash_murmur2
SELECT pgbench.hash(12345, 0) = pgbench.hash_murmur2(12345, 0);
SELECT pgbench.hash(12345, 42) = pgbench.hash_murmur2(12345, 42);

-- Test Permutation function (bijective mapping over 0..9)
SELECT array_agg(pgbench.permute(i, 10, 42) ORDER BY i) FROM generate_series(0, 9) i;
SELECT count(DISTINCT pgbench.permute(i, 10, 42)) = 10 FROM generate_series(0, 9) i;
SELECT count(DISTINCT pgbench.permute(i, 100, 12345)) = 100 FROM generate_series(0, 99) i;

-- Test setseed and reproducibility
SELECT pgbench.setseed(0.5);
SELECT pgbench.random(1, 100) AS r_unif,
       pgbench.random_gaussian(1, 100, 2.5) AS r_gauss,
       pgbench.random_exponential(1, 100, 3.0) AS r_exp,
       pgbench.random_zipfian(1, 100, 1.5) AS r_zipf,
       pgbench.random_poisson(50.0) AS r_poiss;

-- Reset seed and verify exact same values
SELECT pgbench.setseed(0.5);
SELECT pgbench.random(1, 100) AS r_unif,
       pgbench.random_gaussian(1, 100, 2.5) AS r_gauss,
       pgbench.random_exponential(1, 100, 3.0) AS r_exp,
       pgbench.random_zipfian(1, 100, 1.5) AS r_zipf,
       pgbench.random_poisson(50.0) AS r_poiss;

-- Test bigint setseed
SELECT pgbench.setseed(123456789::bigint);
SELECT pgbench.random(1, 100);
SELECT pgbench.setseed(123456789::bigint);
SELECT pgbench.random(1, 100);

-- Test integer setseed
SELECT pgbench.setseed(42);
SELECT pgbench.random(1, 100);
SELECT pgbench.setseed(42);
SELECT pgbench.random(1, 100);

-- Test TPC-C NURand, c_last, random strings, and date functions
SELECT pgbench.setseed(42::bigint);
SELECT pgbench.nurand(255, 0, 999) BETWEEN 0 AND 999 AS nurand_in_range,
       pgbench.nurand(255, 0, 999, 100) BETWEEN 0 AND 999 AS nurand_c_in_range;
SELECT pgbench.c_last(0), pgbench.c_last(48), pgbench.c_last(999);
SELECT length(pgbench.random_astring(8, 8)) = 8 AS astring_len_ok,
       pgbench.random_nstring(6, 6) ~ '^[0-9]{6}$' AS nstring_digits_ok,
       octet_length(pgbench.random_bytea(16, 32)) BETWEEN 16 AND 32 AS bytea_len_ok;
SELECT pgbench.date_add('1998-12-01', -90, 'day') AS q1_date,
       pgbench.int_to_date('1998-12-01', -90, 'day') AS q1_int_to_date,
       pgbench.int_to_date('1992-01-01', 0, 10000, '7 years') AS win_start,
       pgbench.int_to_date('1992-01-01', 10000, 10000, '7 years') AS win_end,
       pgbench.to_timestamp('2026-02-28 14:30:00') AS ts_val;
SELECT pgbench.format_string('ORD_%04d_%s_%.2f%%', 42, 'ABC', 12.345) AS fmt_str;
SELECT pgbench.array_generate(4, 7) AS const_arr,
       cardinality(pgbench.array_generate(5, 1, 100)) = 5 AS rand_arr_len_ok;

-- Test Multi-Modal Vector, Sparse Vector, and Graph Edge Generators
SELECT pgbench.setseed(42::bigint);
SELECT round(pgbench.vector_distance(pgbench.random_vector(4, -1.0, 1.0, true), '[0,0,0,0]', 'l2')::numeric, 5) = 1.00000 AS rand_vec_norm_ok,
       round(pgbench.vector_distance(pgbench.random_vector_gaussian(4, 1.0, true), '[0,0,0,0]', 'l2')::numeric, 5) = 1.00000 AS gauss_vec_norm_ok,
       round(pgbench.vector_distance(pgbench.random_vector(4, 'gaussian', true), '[0,0,0,0]', 'l2')::numeric, 5) = 1.00000 AS dist_vec_norm_ok;
SELECT pgbench.clustered_vector(4, 5, 0.0, 42, 2) = pgbench.clustered_vector(4, 5, 0.0, 42, 2) AS centroid_deterministic,
       round(pgbench.vector_distance(pgbench.clustered_vector(4, 5, 0.0, 42, 2), '[0,0,0,0]', 'l2')::numeric, 5) = 1.00000 AS centroid_unit_norm;
SELECT round(pgbench.vector_distance(
           pgbench.planted_vector(8, 100, 0.0, 42),
           pgbench.planted_vector(8, 100, 0.25, 42),
           'l2')::numeric, 5) = 0.25000 AS planted_delta_exact;
SELECT pgbench.random_sparse_vector(100, 3, 0.5, 0.5) ~ '^\{([0-9]+:0\.5(,)?){1,3}\}/100$' AS sparse_fmt_ok,
       pgbench.random_sparse_vector(5000, 300, 0.5, 0.5) ~ '^\{([0-9]+:0\.5,){250}([0-9]+:0\.5,){49}[0-9]+:0\.5\}/5000$' AS sparse_large_nnz_ok;
SELECT cardinality(pgbench.rmat_edge(10, 0.57, 0.19, 0.19, 42)) = 2 AS rmat_len_ok,
       (pgbench.rmat_edge(10))[1] BETWEEN 0 AND 1023 AS rmat_src_in_range,
       (pgbench.rmat_edge(10))[2] BETWEEN 0 AND 1023 AS rmat_dst_in_range;
SELECT pgbench.vector_distance('[3,4,0]', '[0,0,0]', 'l2') AS dist_l2,
       pgbench.vector_distance('[1,0,0]', '[0,1,0]', 'cosine') AS dist_cosine,
       pgbench.vector_distance('[1,2,3]', '[4,5,6]', 'ip') AS dist_ip;

-- Error cases: parameter boundaries
SELECT pgbench.random(10, 5);
SELECT pgbench.random_gaussian(1, 10, 1.5);
SELECT pgbench.random_exponential(1, 10, 0.0);
SELECT pgbench.random_zipfian(1, 10, 1.0);
SELECT pgbench.random_zipfian(1, 10, 1001.0);
SELECT pgbench.random_poisson(0.0);
SELECT pgbench.permute(5, 0, 42);
SELECT pgbench.setseed(1.5);
SELECT pgbench.c_last(1000);
SELECT pgbench.to_timestamp('2026-02-30');
SELECT pgbench.random_vector(0);
SELECT pgbench.vector_distance('[1,2]', '[1,2,3]', 'l2');
SELECT pgbench.vector_distance('[1,2]', '[1,2]', 'hamming');

DROP EXTENSION pgbench;
