--
-- B6: VACUUM Overhead (OLTP-style targeted operations)
--
-- Generates dead tuples via targeted updates and small deletes, then
-- measures VACUUM cost. With UNDO, committed operations leave zero
-- dead tuples, so VACUUM should be nearly instant.
--
-- Variables: :scenario, :row_count, :create_opts
--

-- ================================================================
-- Setup
-- ================================================================
DROP TABLE IF EXISTS bench_vacuum;
CREATE TABLE bench_vacuum (
    id integer PRIMARY KEY,
    value integer,
    data text
) :create_opts;

INSERT INTO bench_vacuum
SELECT i, i, md5(i::text) FROM generate_series(1, :row_count) i;

SELECT pg_relation_size('bench_vacuum') AS _size \gset
\echo UNDO_BENCH_RESULT|initial_size|bytes|:_size

-- ================================================================
-- Phase 1: Generate dead tuples via targeted updates
--   - 1000 single-row updates + one 10% batch update
-- ================================================================
\echo '--- Generating dead tuples via targeted updates ---'

CREATE FUNCTION pg_temp.bench_targeted_updates(n integer)
RETURNS void LANGUAGE plpgsql AS $fn$
BEGIN
    FOR i IN 1..n LOOP
        UPDATE bench_vacuum SET value = value + 1 WHERE id = i;
    END LOOP;
END
$fn$;

SELECT pg_temp.bench_targeted_updates(LEAST(:row_count, 1000));
UPDATE bench_vacuum SET value = value + 1 WHERE id % 10 = 0;

-- Pre-VACUUM metrics
SELECT pg_relation_size('bench_vacuum') AS _size \gset
\echo UNDO_BENCH_RESULT|pre_vacuum_size|bytes|:_size

SELECT pg_stat_force_next_flush();
SELECT COALESCE(n_dead_tup, 0) AS _dead
  FROM pg_stat_user_tables WHERE relname = 'bench_vacuum' \gset
\echo UNDO_BENCH_RESULT|dead_pre_vacuum|count|:_dead

-- ================================================================
-- Phase 2: VACUUM timing
-- ================================================================
\echo '--- VACUUM ---'

SELECT clock_timestamp()::text AS _t0 \gset
VACUUM bench_vacuum;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|vacuum_time|time_ms|:_elapsed

SELECT pg_relation_size('bench_vacuum') AS _size \gset
\echo UNDO_BENCH_RESULT|post_vacuum_size|bytes|:_size

SELECT pg_stat_force_next_flush();
SELECT COALESCE(n_dead_tup, 0) AS _dead
  FROM pg_stat_user_tables WHERE relname = 'bench_vacuum' \gset
\echo UNDO_BENCH_RESULT|dead_post_vacuum|count|:_dead

-- ================================================================
-- Phase 3: DELETE 5% + VACUUM cycle
-- ================================================================
\echo '--- DELETE 5% + VACUUM ---'

SELECT clock_timestamp()::text AS _t0 \gset
DELETE FROM bench_vacuum WHERE id % 20 = 0;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|delete_5pct|time_ms|:_elapsed

SELECT clock_timestamp()::text AS _t0 \gset
VACUUM bench_vacuum;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|vacuum_after_delete|time_ms|:_elapsed

SELECT pg_relation_size('bench_vacuum') AS _size \gset
\echo UNDO_BENCH_RESULT|final_size|bytes|:_size

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE bench_vacuum;
