-- create_upper_paths_hook must be fired for UPPERREL_PARTIAL_GROUP_AGG when
-- the planner considers partial aggregation.

LOAD 'test_extensible';

CREATE TABLE test_extensible_agg_tbl (a int, b int);
INSERT INTO test_extensible_agg_tbl SELECT i % 3, i FROM generate_series(1, 10) i;

SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;

SELECT a, sum(b) FROM test_extensible_agg_tbl GROUP BY a ORDER BY a;

DROP TABLE test_extensible_agg_tbl;
