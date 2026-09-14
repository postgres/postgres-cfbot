--
-- b8_large_transaction.sql
--
-- Benchmark: Large-transaction rollback cost characterization
--
-- Measures INSERT, UPDATE, and DELETE rollback time at multiple transaction
-- sizes.  This quantifies the O(N) rollback cost of UNDO vs. the O(1) CLOG
-- rollback of standard heap (which defers the O(N) cost to VACUUM).
--
-- Variables: :scenario, :row_count, :create_opts
--

-- ================================================================
-- Setup
-- ================================================================
DROP TABLE IF EXISTS bench_large_txn;
CREATE TABLE bench_large_txn (id integer, data text) :create_opts;

-- Pre-populate for UPDATE/DELETE tests
INSERT INTO bench_large_txn
SELECT i, md5(i::text)
FROM generate_series(1, :row_count) i;

-- ================================================================
-- Test 1: INSERT + ROLLBACK
-- ================================================================
\echo '--- INSERT + ROLLBACK ---'

TRUNCATE bench_large_txn;

SELECT clock_timestamp()::text AS _t0 \gset
BEGIN;
INSERT INTO bench_large_txn
SELECT i, md5(i::text) FROM generate_series(1, :row_count) i;
SELECT clock_timestamp()::text AS _t_insert \gset
ROLLBACK;
SELECT clock_timestamp()::text AS _t_rollback \gset

SELECT round(extract(epoch FROM
    (:'_t_insert'::timestamptz - :'_t0'::timestamptz)) * 1000, 2) AS _ins_ms \gset
SELECT round(extract(epoch FROM
    (:'_t_rollback'::timestamptz - :'_t_insert'::timestamptz)) * 1000, 2) AS _rb_ms \gset

\echo UNDO_BENCH_RESULT|insert_ms|time_ms|:_ins_ms
\echo UNDO_BENCH_RESULT|insert_rollback_ms|time_ms|:_rb_ms

SELECT count(*) AS _cnt FROM bench_large_txn \gset
\echo UNDO_BENCH_RESULT|post_insert_rollback_rows|count|:_cnt

-- ================================================================
-- Test 2: UPDATE (half rows, constant string) + ROLLBACK
--
-- Uses a constant assignment rather than md5(data) to avoid confounding
-- measurement with CPU overhead; DML time reflects I/O + WAL cost.
-- ================================================================
\echo '--- UPDATE + ROLLBACK ---'

-- Re-populate
TRUNCATE bench_large_txn;
INSERT INTO bench_large_txn
SELECT i, md5(i::text) FROM generate_series(1, :row_count) i;

SELECT clock_timestamp()::text AS _t0 \gset
BEGIN;
UPDATE bench_large_txn SET data = lpad('', 50, 'u') WHERE id <= :row_count / 2;
SELECT clock_timestamp()::text AS _t_update \gset
ROLLBACK;
SELECT clock_timestamp()::text AS _t_rollback \gset

SELECT round(extract(epoch FROM
    (:'_t_update'::timestamptz - :'_t0'::timestamptz)) * 1000, 2) AS _upd_ms \gset
SELECT round(extract(epoch FROM
    (:'_t_rollback'::timestamptz - :'_t_update'::timestamptz)) * 1000, 2) AS _rb_ms \gset

\echo UNDO_BENCH_RESULT|update_ms|time_ms|:_upd_ms
\echo UNDO_BENCH_RESULT|update_rollback_ms|time_ms|:_rb_ms

-- ================================================================
-- Test 3: DELETE + ROLLBACK
-- ================================================================
\echo '--- DELETE + ROLLBACK ---'

SELECT clock_timestamp()::text AS _t0 \gset
BEGIN;
DELETE FROM bench_large_txn;
SELECT clock_timestamp()::text AS _t_delete \gset
ROLLBACK;
SELECT clock_timestamp()::text AS _t_rollback \gset

SELECT round(extract(epoch FROM
    (:'_t_delete'::timestamptz - :'_t0'::timestamptz)) * 1000, 2) AS _del_ms \gset
SELECT round(extract(epoch FROM
    (:'_t_rollback'::timestamptz - :'_t_delete'::timestamptz)) * 1000, 2) AS _rb_ms \gset

\echo UNDO_BENCH_RESULT|delete_ms|time_ms|:_del_ms
\echo UNDO_BENCH_RESULT|delete_rollback_ms|time_ms|:_rb_ms

SELECT count(*) AS _cnt FROM bench_large_txn \gset
\echo UNDO_BENCH_RESULT|post_delete_rollback_rows|count|:_cnt

-- ================================================================
-- Note: Cold-WAL rollback scenario
--
-- The tests above measure warm-cache rollback (UNDO WAL records may be
-- in the OS buffer cache).  To measure cold-WAL rollback (WAL reads from
-- disk), use the crash-recovery path: run a large uncommitted transaction,
-- stop postgres with pg_ctl stop -m immediate, then restart and measure
-- the recovery time via pg_stat_recovery_prefetch or server logs.
-- The run_undo_bench.sh harness can be extended with a crash-recovery
-- benchmark variant for this purpose.
-- ================================================================

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE bench_large_txn;
