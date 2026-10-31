--
-- B7: Storage Footprint (OLTP-style targeted operations)
--
-- Compares table sizes after targeted updates rather than repeated
-- full-table passes. Measures fresh load, post-targeted-update,
-- and post-VACUUM sizes. UNDO log size measured by orchestrator.
--
-- Variables: :scenario, :row_count, :create_opts
--

-- ================================================================
-- Test 1: Fresh load size
-- ================================================================
\echo '--- Fresh load: :row_count rows ---'

DROP TABLE IF EXISTS bench_storage;
CREATE TABLE bench_storage (
    id integer PRIMARY KEY,
    counter integer DEFAULT 0,
    payload text DEFAULT repeat('x', 50)
) :create_opts;

INSERT INTO bench_storage (id)
SELECT i FROM generate_series(1, :row_count) i;

SELECT pg_relation_size('bench_storage') AS _size \gset
\echo UNDO_BENCH_RESULT|fresh_table_size|bytes|:_size

SELECT pg_total_relation_size('bench_storage') AS _size \gset
\echo UNDO_BENCH_RESULT|fresh_total_size|bytes|:_size

SELECT pg_indexes_size('bench_storage') AS _size \gset
\echo UNDO_BENCH_RESULT|fresh_index_size|bytes|:_size

-- ================================================================
-- Test 2: Post targeted-update sizes (1000 single + 10% batch)
-- ================================================================
\echo '--- Targeted updates (1000 single-row + 10% batch) ---'

CREATE FUNCTION pg_temp.bench_targeted_updates(n integer)
RETURNS void LANGUAGE plpgsql AS $fn$
BEGIN
    FOR i IN 1..n LOOP
        UPDATE bench_storage SET counter = counter + 1 WHERE id = i;
    END LOOP;
END
$fn$;

SELECT pg_temp.bench_targeted_updates(LEAST(:row_count, 1000));
UPDATE bench_storage SET counter = counter + 1 WHERE id % 10 = 0;

SELECT pg_relation_size('bench_storage') AS _size \gset
\echo UNDO_BENCH_RESULT|post_update_table_size|bytes|:_size

SELECT pg_total_relation_size('bench_storage') AS _size \gset
\echo UNDO_BENCH_RESULT|post_update_total_size|bytes|:_size

-- ================================================================
-- Test 3: Post-VACUUM sizes
-- ================================================================
\echo '--- After VACUUM ---'

VACUUM bench_storage;

SELECT pg_relation_size('bench_storage') AS _size \gset
\echo UNDO_BENCH_RESULT|post_vacuum_table_size|bytes|:_size

SELECT pg_total_relation_size('bench_storage') AS _size \gset
\echo UNDO_BENCH_RESULT|post_vacuum_total_size|bytes|:_size

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE bench_storage;
