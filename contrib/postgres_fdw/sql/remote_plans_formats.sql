-- ===================================================================
-- test EXPLAIN (REMOTE_PLANS) in the non-text output formats
-- ===================================================================
-- The behaviour of REMOTE_PLANS itself is tested in postgres_fdw.sql, next
-- to the tests for the plans it reports on.  This file only covers how the
-- collected remote plans are embedded in each output format.
-- This runs after postgres_fdw.sql in the same database and reuses the
-- server, user mapping and foreign tables created there.

LOAD 'postgres_fdw';

EXPLAIN (REMOTE_PLANS, FORMAT JSON, COSTS OFF)
SELECT c1 FROM ft1 t1 WHERE t1.c1 = 101;

EXPLAIN (REMOTE_PLANS, FORMAT XML, COSTS OFF)
SELECT c1 FROM ft1 t1 WHERE t1.c1 = 101;

EXPLAIN (REMOTE_PLANS, FORMAT YAML, COSTS OFF)
SELECT c1 FROM ft1 t1 WHERE t1.c1 = 101;
