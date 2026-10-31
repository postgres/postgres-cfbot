--
-- B3: Delete Performance (OLTP-style targeted operations)
--
-- Measures UNDO cost for realistic delete patterns: single-row PK
-- lookups, small batches, targeted percentage, and dead tuple tracking.
--
-- Variables: :scenario, :row_count, :create_opts
--

-- ================================================================
-- Setup
-- ================================================================
DROP TABLE IF EXISTS bench_delete;
CREATE TABLE bench_delete (
    id integer PRIMARY KEY,
    value integer,
    data text
) :create_opts;

INSERT INTO bench_delete
SELECT i, i, md5(i::text) FROM generate_series(1, :row_count) i;

SELECT pg_relation_size('bench_delete') AS _size \gset
\echo UNDO_BENCH_RESULT|initial_size|bytes|:_size

-- ================================================================
-- B3a: Single-row DELETE by PK (500 rows)
-- ================================================================
\echo '--- Single-row DELETE by PK (500 rows) ---'

CREATE FUNCTION pg_temp.bench_single_deletes(n integer, max_id integer)
RETURNS void LANGUAGE plpgsql AS $fn$
BEGIN
    -- Delete from the end so we don't affect later tests
    FOR i IN REVERSE max_id..(max_id - n + 1) LOOP
        DELETE FROM bench_delete WHERE id = i;
    END LOOP;
END
$fn$;

SELECT LEAST(:row_count / 2, 500) AS _del_n \gset

SELECT clock_timestamp()::text AS _t0 \gset
SELECT pg_temp.bench_single_deletes(:_del_n, :row_count);
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|single_row_delete|time_ms|:_elapsed

SELECT count(*) AS _cnt FROM bench_delete \gset
\echo UNDO_BENCH_RESULT|rows_after_single_delete|count|:_cnt

-- ================================================================
-- B3b: Small batch DELETE (10-row batches, 50 batches)
-- ================================================================
\echo '--- Batch DELETE (10-row batches, 50 batches) ---'

SELECT clock_timestamp()::text AS _t0 \gset

CREATE FUNCTION pg_temp.bench_batch_deletes(batches integer, batch_sz integer, max_id integer)
RETURNS void LANGUAGE plpgsql AS $fn$
DECLARE
    start_id integer;
BEGIN
    FOR b IN 1..batches LOOP
        start_id := ((b - 1) * batch_sz) % max_id + 1;
        DELETE FROM bench_delete
        WHERE id >= start_id AND id < start_id + batch_sz;
    END LOOP;
END
$fn$;

SELECT pg_temp.bench_batch_deletes(50, 10, :row_count);
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|batch_delete_10x50|time_ms|:_elapsed

SELECT count(*) AS _cnt FROM bench_delete \gset
\echo UNDO_BENCH_RESULT|rows_after_batch_delete|count|:_cnt

-- ================================================================
-- B3c: 5% targeted DELETE
-- ================================================================
\echo '--- 5% targeted DELETE ---'

SELECT clock_timestamp()::text AS _t0 \gset
DELETE FROM bench_delete WHERE id % 20 = 0;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|targeted_5pct_delete|time_ms|:_elapsed

SELECT count(*) AS _cnt FROM bench_delete \gset
\echo UNDO_BENCH_RESULT|rows_after_targeted_delete|count|:_cnt

-- Dead tuple check
SELECT pg_stat_force_next_flush();
SELECT COALESCE(n_dead_tup, 0) AS _dead
  FROM pg_stat_user_tables WHERE relname = 'bench_delete' \gset
\echo UNDO_BENCH_RESULT|dead_after_deletes|count|:_dead

SELECT pg_relation_size('bench_delete') AS _size \gset
\echo UNDO_BENCH_RESULT|final_size|bytes|:_size

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE bench_delete;
