--
-- B5: Rollback Cost
--
-- The key UNDO differentiator. Standard PG rollback is near-instant (marks
-- xact aborted, leaves dead tuples). UNDO rollback synchronously walks the
-- chain and physically reverses each operation.
--
-- Tests INSERT rollback at 100, 1000, 10000, 100000 rows; UPDATE and DELETE
-- rollback at 10000 rows. Captures WAL volume and dead tuple counts.
--
-- Variables: :scenario, :row_count (unused, B5 uses internal sizes), :create_opts
--

-- ================================================================
-- Setup
-- ================================================================
DROP TABLE IF EXISTS bench_rollback;
CREATE TABLE bench_rollback (id integer, data text) :create_opts;

-- ================================================================
-- INSERT Rollback: 100 rows
-- ================================================================
\echo '--- INSERT rollback: 100 rows ---'

BEGIN;
INSERT INTO bench_rollback SELECT i, md5(i::text) FROM generate_series(1, 100) i;
SELECT clock_timestamp()::text AS _t1 \gset
ROLLBACK;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t1'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|ins_rollback_100|time_ms|:_elapsed

SELECT count(*) AS _cnt FROM bench_rollback \gset
\echo UNDO_BENCH_RESULT|ins_rollback_100_rows|count|:_cnt

-- ================================================================
-- INSERT Rollback: 1,000 rows
-- ================================================================
\echo '--- INSERT rollback: 1,000 rows ---'

BEGIN;
INSERT INTO bench_rollback SELECT i, md5(i::text) FROM generate_series(1, 1000) i;
SELECT clock_timestamp()::text AS _t1 \gset
ROLLBACK;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t1'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|ins_rollback_1k|time_ms|:_elapsed

SELECT count(*) AS _cnt FROM bench_rollback \gset
\echo UNDO_BENCH_RESULT|ins_rollback_1k_rows|count|:_cnt

-- ================================================================
-- INSERT Rollback: 10,000 rows
-- ================================================================
\echo '--- INSERT rollback: 10,000 rows ---'

BEGIN;
INSERT INTO bench_rollback SELECT i, md5(i::text) FROM generate_series(1, 10000) i;
SELECT clock_timestamp()::text AS _t1 \gset
ROLLBACK;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t1'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|ins_rollback_10k|time_ms|:_elapsed

SELECT count(*) AS _cnt FROM bench_rollback \gset
\echo UNDO_BENCH_RESULT|ins_rollback_10k_rows|count|:_cnt

-- ================================================================
-- INSERT Rollback: 100,000 rows
-- ================================================================
\echo '--- INSERT rollback: 100,000 rows ---'

BEGIN;
INSERT INTO bench_rollback SELECT i, md5(i::text) FROM generate_series(1, 100000) i;
SELECT clock_timestamp()::text AS _t1 \gset
ROLLBACK;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t1'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|ins_rollback_100k|time_ms|:_elapsed

SELECT count(*) AS _cnt FROM bench_rollback \gset
\echo UNDO_BENCH_RESULT|ins_rollback_100k_rows|count|:_cnt

-- ================================================================
-- UPDATE Rollback: 10,000 rows
-- ================================================================
\echo '--- UPDATE rollback: 10,000 rows ---'

-- Insert baseline data for update test
INSERT INTO bench_rollback SELECT i, 'baseline_' || i FROM generate_series(1, 10000) i;

BEGIN;
UPDATE bench_rollback SET data = 'modified';
SELECT clock_timestamp()::text AS _t1 \gset
ROLLBACK;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t1'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|upd_rollback_10k|time_ms|:_elapsed

-- Verify original data preserved
SELECT count(*) AS _cnt FROM bench_rollback WHERE data LIKE 'baseline_%' \gset
\echo UNDO_BENCH_RESULT|upd_rollback_preserved|count|:_cnt

-- ================================================================
-- DELETE Rollback: 10,000 rows
-- ================================================================
\echo '--- DELETE rollback: 10,000 rows ---'

BEGIN;
DELETE FROM bench_rollback;
SELECT clock_timestamp()::text AS _t1 \gset
ROLLBACK;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t1'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|del_rollback_10k|time_ms|:_elapsed

-- Verify rows restored
SELECT count(*) AS _cnt FROM bench_rollback \gset
\echo UNDO_BENCH_RESULT|del_rollback_restored|count|:_cnt

-- ================================================================
-- WAL Volume: INSERT + ROLLBACK of 10,000 rows
-- ================================================================
\echo '--- WAL volume: 10K row insert+rollback ---'

TRUNCATE bench_rollback;
SELECT pg_current_wal_lsn()::text AS _wal0 \gset

BEGIN;
INSERT INTO bench_rollback SELECT i, md5(i::text) FROM generate_series(1, 10000) i;
ROLLBACK;

SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), :'_wal0'::pg_lsn)::bigint AS _wal_bytes \gset
\echo UNDO_BENCH_RESULT|wal_10k_ins_rollback|bytes|:_wal_bytes

-- ================================================================
-- Dead tuple check after all rollbacks
-- ================================================================
SELECT pg_stat_force_next_flush();
SELECT COALESCE(n_dead_tup, 0) AS _dead
  FROM pg_stat_user_tables WHERE relname = 'bench_rollback' \gset
\echo UNDO_BENCH_RESULT|dead_after_rollbacks|count|:_dead

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE bench_rollback;
