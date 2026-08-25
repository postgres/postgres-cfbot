CREATE EXTENSION pgbench;

-- Check schema and functions
\dx+ pgbench

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

-- Error cases: parameter boundaries
SELECT pgbench.random(10, 5);
SELECT pgbench.random_gaussian(1, 10, 1.5);
SELECT pgbench.random_exponential(1, 10, 0.0);
SELECT pgbench.random_zipfian(1, 10, 1.0);
SELECT pgbench.random_zipfian(1, 10, 1001.0);
SELECT pgbench.random_poisson(0.0);
SELECT pgbench.permute(5, 0, 42);
SELECT pgbench.setseed(1.5);

DROP EXTENSION pgbench;
