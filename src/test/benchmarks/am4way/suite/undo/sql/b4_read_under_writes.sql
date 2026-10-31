--
-- B4: Read Under Writes (OLTP-style targeted operations)
--
-- Tests read stability after targeted writes rather than full-table scans.
-- UNDO-enabled tables should maintain consistent read performance since
-- there are no dead tuples to skip.
--
-- Variables: :scenario, :row_count, :create_opts
--

-- ================================================================
-- Setup
-- ================================================================
DROP TABLE IF EXISTS bench_ruw;
CREATE TABLE bench_ruw (
    id integer PRIMARY KEY,
    value integer,
    data text
) :create_opts;

INSERT INTO bench_ruw
SELECT i, i % 1000, md5(i::text)
FROM generate_series(1, :row_count) i;

CREATE INDEX bench_ruw_value_idx ON bench_ruw (value);

-- ================================================================
-- Baseline reads on clean table
-- ================================================================
\echo '--- Baseline sequential scan ---'

SELECT clock_timestamp()::text AS _t0 \gset
SELECT count(*), sum(value) FROM bench_ruw;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|baseline_seqscan|time_ms|:_elapsed

\echo '--- Baseline index scan ---'
SET enable_seqscan = off;
SELECT clock_timestamp()::text AS _t0 \gset
SELECT count(*) FROM bench_ruw WHERE value BETWEEN 100 AND 200;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|baseline_idxscan|time_ms|:_elapsed
RESET enable_seqscan;

-- ================================================================
-- Phase 1: Targeted single-row updates (1000 rows, no VACUUM)
-- ================================================================
\echo '--- 1000 single-row updates (no VACUUM) ---'

CREATE FUNCTION pg_temp.bench_targeted_updates(n integer)
RETURNS void LANGUAGE plpgsql AS $fn$
BEGIN
    FOR i IN 1..n LOOP
        UPDATE bench_ruw SET value = value + 1 WHERE id = i;
    END LOOP;
END
$fn$;

SELECT pg_temp.bench_targeted_updates(LEAST(:row_count, 1000));

-- Post-targeted-update reads
\echo '--- Post-targeted-update sequential scan ---'
SELECT clock_timestamp()::text AS _t0 \gset
SELECT count(*), sum(value) FROM bench_ruw;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|post_targeted_seqscan|time_ms|:_elapsed

\echo '--- Post-targeted-update index scan ---'
SET enable_seqscan = off;
SELECT clock_timestamp()::text AS _t0 \gset
SELECT count(*) FROM bench_ruw WHERE value BETWEEN 100 AND 200;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|post_targeted_idxscan|time_ms|:_elapsed
RESET enable_seqscan;

-- ================================================================
-- Phase 2: Batch updates (10% of rows in one statement)
-- ================================================================
\echo '--- 10% batch update (no VACUUM) ---'

UPDATE bench_ruw SET value = value + 1 WHERE id % 10 = 0;

\echo '--- Post-batch-update sequential scan ---'
SELECT clock_timestamp()::text AS _t0 \gset
SELECT count(*), sum(value) FROM bench_ruw;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|post_batch_seqscan|time_ms|:_elapsed

-- ================================================================
-- Phase 3: Interleaved single-row write + PK read (100 cycles)
-- ================================================================
\echo '--- Interleaved write+read: 100 cycles ---'

CREATE FUNCTION pg_temp.bench_interleaved_rw(n integer)
RETURNS void LANGUAGE plpgsql AS $fn$
DECLARE
    _val integer;
BEGIN
    FOR i IN 1..n LOOP
        UPDATE bench_ruw SET value = value + 1 WHERE id = i;
        SELECT value INTO _val FROM bench_ruw WHERE id = i;
    END LOOP;
END
$fn$;

SELECT clock_timestamp()::text AS _t0 \gset
SELECT pg_temp.bench_interleaved_rw(LEAST(:row_count, 100));
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|interleaved_rw_100|time_ms|:_elapsed

-- ================================================================
-- Phase 4: Read after VACUUM (heap should recover)
-- ================================================================
VACUUM bench_ruw;

\echo '--- Post-VACUUM sequential scan ---'
SELECT clock_timestamp()::text AS _t0 \gset
SELECT count(*), sum(value) FROM bench_ruw;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|post_vacuum_seqscan|time_ms|:_elapsed

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE bench_ruw;
