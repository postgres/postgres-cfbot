LOAD 'test_upper_paths_hook';

CREATE FUNCTION test_upper_paths_hook_seen() RETURNS bool
AS 'test_upper_paths_hook', 'test_upper_paths_hook_seen' LANGUAGE C;

CREATE FUNCTION test_upper_paths_hook_input_null() RETURNS bool
AS 'test_upper_paths_hook', 'test_upper_paths_hook_input_null' LANGUAGE C;

CREATE FUNCTION test_upper_paths_hook_output_seen() RETURNS bool
AS 'test_upper_paths_hook', 'test_upper_paths_hook_output_seen' LANGUAGE C;

CREATE FUNCTION test_upper_paths_hook_reset() RETURNS void
AS 'test_upper_paths_hook', 'test_upper_paths_hook_reset' LANGUAGE C;

SELECT test_upper_paths_hook_reset();

-- Force partial paths and make parallel plans attractive.
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;

CREATE TABLE test_upper_paths_hook_t (a int, b int)
  WITH (autovacuum_enabled = false);
INSERT INTO test_upper_paths_hook_t
SELECT g % 100, g % 100 FROM generate_series(1, 20000) g;

-- Extended statistics give the multi-column key a usable estimate.
CREATE STATISTICS test_upper_paths_hook_nd (ndistinct)
ON a, b FROM test_upper_paths_hook_t;
ANALYZE test_upper_paths_hook_t;

-- Planning this UNION creates the partial setop upper rel.
EXPLAIN (COSTS OFF)
SELECT a, b FROM test_upper_paths_hook_t
UNION
SELECT a, b FROM test_upper_paths_hook_t;

SELECT test_upper_paths_hook_seen();
SELECT test_upper_paths_hook_input_null();
SELECT test_upper_paths_hook_output_seen();

-- The partial path is still built when statistics are unreliable, so the
-- hook receives the stage.  Whether it is chosen depends on the memory-aware
-- cost; for this small table the worst-case table fits in hash_mem, so it is
-- chosen.
SELECT test_upper_paths_hook_reset();
CREATE TABLE test_upper_paths_hook_nostat (a int, b int)
  WITH (autovacuum_enabled = false);
INSERT INTO test_upper_paths_hook_nostat
SELECT g % 100, g % 100 FROM generate_series(1, 20000) g;
EXPLAIN (COSTS OFF)
SELECT a, b FROM test_upper_paths_hook_nostat
UNION
SELECT a, b FROM test_upper_paths_hook_nostat;
SELECT test_upper_paths_hook_seen();
DROP TABLE test_upper_paths_hook_nostat;

-- A query without a set operation must not trigger the hook.
SELECT test_upper_paths_hook_reset();
SELECT count(*) FROM test_upper_paths_hook_t;
SELECT test_upper_paths_hook_seen();

DROP TABLE test_upper_paths_hook_t;

RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;
