--
-- B1: Insert Throughput
--
-- Measures UNDO record generation cost on INSERTs.
-- Each INSERT with UNDO writes a 48-byte header (no old-tuple payload).
--
-- Variables: :scenario, :row_count, :create_opts
--

-- ================================================================
-- Setup
-- ================================================================
DROP TABLE IF EXISTS bench_insert;
CREATE TABLE bench_insert (
    id integer,
    value integer,
    data text
) :create_opts;

-- ================================================================
-- B1a: Bulk INSERT via generate_series
-- ================================================================
\echo '--- Bulk INSERT :row_count rows ---'

SELECT clock_timestamp()::text AS _t0 \gset

INSERT INTO bench_insert (id, value, data)
SELECT i, i, md5(i::text) FROM generate_series(1, :row_count) i;

SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|bulk_insert|time_ms|:_elapsed

SELECT pg_relation_size('bench_insert') AS _size \gset
\echo UNDO_BENCH_RESULT|bulk_insert_size|bytes|:_size

-- ================================================================
-- B1b: Individual INSERT (PL/pgSQL loop, capped at 10000)
-- ================================================================
TRUNCATE bench_insert;

SELECT LEAST(:row_count, 10000) AS _ind_count \gset

\echo '--- Individual INSERT :_ind_count rows ---'

-- Use a temp function so the loop limit is passed as a parameter
-- (psql variables are not expanded inside $$ string constants)
CREATE FUNCTION pg_temp.bench_individual_insert(n integer)
RETURNS void LANGUAGE plpgsql AS $fn$
BEGIN
    FOR i IN 1..n LOOP
        INSERT INTO bench_insert (id, value, data)
        VALUES (i, i, md5(i::text));
    END LOOP;
END
$fn$;

SELECT clock_timestamp()::text AS _t0 \gset

SELECT pg_temp.bench_individual_insert(:_ind_count);

SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|individual_insert|time_ms|:_elapsed

SELECT pg_relation_size('bench_insert') AS _size \gset
\echo UNDO_BENCH_RESULT|individual_insert_size|bytes|:_size

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE bench_insert;
