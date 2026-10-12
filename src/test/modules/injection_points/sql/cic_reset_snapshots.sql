CREATE EXTENSION injection_points;

SELECT injection_points_set_local();
-- Reset on every page boundary to make resets deterministic.
SET concurrent_index_reset_snapshot_interval = 0;
SELECT injection_points_attach('heap_reset_scan_snapshot_xmin_pinned', 'error');
SELECT injection_points_attach('heap_reset_scan_snapshot_xid_assigned', 'notice');
SELECT injection_points_attach('heap_beginscan_reset_snapshot', 'notice');
SELECT injection_points_attach('table_parallelscan_initialize_reset_snapshot', 'notice');
-- No concurrent deletes occur here.  Deferral is only expected in the
-- tests below that deliberately exhaust the heap-fetch budget.
SELECT injection_points_attach('bt_load_defer_unique_check', 'error');

CREATE SCHEMA cic_reset_snap;
-- Pad rows to fit exactly seven per page at any block size.  A scan of
-- 35 rows then resets its snapshot five times with a zero interval.
-- Account for the page header (24), line pointer (4), and tuple header
-- plus two integers (36), and round the tuple size down to a multiple of 8.
SELECT ((current_setting('block_size')::int - 24) / 7 - 4) / 8 * 8 - 36 AS pad \gset
CREATE TABLE cic_reset_snap.tbl(i int primary key, j int, pad char(:pad));
ALTER TABLE cic_reset_snap.tbl ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO cic_reset_snap.tbl SELECT i, i * I, 'x' FROM generate_series(1, 35) s(i);

-- Count the snapshot resets during one scan.
SELECT injection_points_attach('heap_reset_scan_snapshot_swap', 'notice');
CREATE INDEX CONCURRENTLY rotation_probe ON cic_reset_snap.tbl(i);
SELECT injection_points_detach('heap_reset_scan_snapshot_swap');
DROP INDEX CONCURRENTLY cic_reset_snap.rotation_probe;

CREATE FUNCTION cic_reset_snap.predicate_stable(integer) RETURNS bool IMMUTABLE
									  LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE 'SELECT txid_current()';
    RETURN MOD($1, 2) = 0;
END; $$;

CREATE FUNCTION cic_reset_snap.predicate_stable_no_param() RETURNS bool IMMUTABLE
									  LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE 'SELECT txid_current()';
    RETURN false;
END; $$;

----------------
ALTER TABLE cic_reset_snap.tbl SET (parallel_workers=0);

CREATE UNIQUE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(MOD(i, 2), j) WHERE MOD(i, 2) = 0;
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i, j) WHERE cic_reset_snap.predicate_stable(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i, j) WHERE cic_reset_snap.predicate_stable_no_param();
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING BRIN(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

-- Check TOAST handling when an opclass's STORAGE type differs from its
-- input type.  The build must use the input type to identify varlena values.
CREATE OPERATOR CLASS cic_reset_snap.int4_bloom_storage_ops FOR TYPE int4
    USING brin AS
    OPERATOR 1 =(int4, int4),
    FUNCTION 1 brin_bloom_opcinfo(internal),
    FUNCTION 2 brin_bloom_add_value(internal, internal, internal, internal),
    FUNCTION 3 brin_bloom_consistent(internal, internal, internal, int4),
    FUNCTION 4 brin_bloom_union(internal, internal, internal),
    FUNCTION 5 brin_bloom_options(internal),
    FUNCTION 11 hashint4(int4),
    STORAGE pg_brin_bloom_summary;
CREATE INDEX CONCURRENTLY idx
    ON cic_reset_snap.tbl USING BRIN(i cic_reset_snap.int4_bloom_storage_ops);
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING HASH(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING GIST(point(i, j));
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING SPGIST(point(i, j));
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING GIN((ARRAY[i, j]));
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

-- The same in parallel mode
ALTER TABLE cic_reset_snap.tbl SET (parallel_workers=2);

CREATE UNIQUE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(MOD(i, 2), j) WHERE MOD(i, 2) = 0;
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i, j) WHERE cic_reset_snap.predicate_stable(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

-- Constant folding of the predicate assigns an xid before the build.
-- Parallel work distribution makes the leader's reset count variable,
-- so do not check the count.
SELECT injection_points_detach('heap_reset_scan_snapshot_xid_assigned');
CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i, j) WHERE cic_reset_snap.predicate_stable_no_param();
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;
SELECT injection_points_attach('heap_reset_scan_snapshot_xid_assigned', 'notice');

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i DESC NULLS LAST);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING BRIN(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING GIN((ARRAY[i, j]));
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

BEGIN TRANSACTION;
CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
ROLLBACK ;

-- REPEATABLE READ and above retain the transaction snapshot, so the
-- scan must not replace its snapshot.
ALTER TABLE cic_reset_snap.tbl SET (parallel_workers=0);
SELECT injection_points_attach('heap_reset_scan_snapshot_swap', 'notice');
SET default_transaction_isolation = 'repeatable read';
CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

SET default_transaction_isolation = serializable;
CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;
RESET default_transaction_isolation;
SELECT injection_points_detach('heap_reset_scan_snapshot_swap');

-- Check that each duplicate-detection path reports a uniqueness violation.
-- Drop the invalid index left by each failed build before the next test.
CREATE TABLE cic_reset_snap.dup(i int, j int);
ALTER TABLE cic_reset_snap.dup SET (parallel_workers=0);
INSERT INTO cic_reset_snap.dup SELECT i, i FROM generate_series(1, 1500) s(i);
INSERT INTO cic_reset_snap.dup VALUES (1500, 1500);
INSERT INTO cic_reset_snap.dup SELECT i, i FROM generate_series(1501, 3000) s(i);

-- The fail-fast check in the tuplesort comparator, in-memory sort.
CREATE UNIQUE INDEX CONCURRENTLY dup_idx ON cic_reset_snap.dup(i);
DROP INDEX cic_reset_snap.dup_idx;

-- The same with sort runs dumped (and compared) while the heap scan is
-- still in progress.
SET maintenance_work_mem = '64kB';
CREATE UNIQUE INDEX CONCURRENTLY dup_idx ON cic_reset_snap.dup(i);
RESET maintenance_work_mem;
DROP INDEX cic_reset_snap.dup_idx;

-- With both budgets disabled, _bt_load() skips the duplicate and validation
-- reports it.  The notice confirms that the entry was left for validation.
SELECT injection_points_detach('bt_load_defer_unique_check');
SELECT injection_points_attach('bt_load_defer_unique_check', 'notice');
SET index_build_duplicate_check_scale_factor = 0;
CREATE UNIQUE INDEX CONCURRENTLY dup_idx ON cic_reset_snap.dup(i);
RESET index_build_duplicate_check_scale_factor;
DROP INDEX cic_reset_snap.dup_idx;

-- INCLUDE columns switch tuple comparison to the per-column path.
CREATE UNIQUE INDEX CONCURRENTLY dup_idx ON cic_reset_snap.dup(i) INCLUDE (j);
DROP INDEX cic_reset_snap.dup_idx;

-- Either a worker or the leader may report the error, so hide the CONTEXT line.
ALTER TABLE cic_reset_snap.dup SET (parallel_workers=2);
\set VERBOSITY terse
CREATE UNIQUE INDEX CONCURRENTLY dup_idx ON cic_reset_snap.dup(i);
\set VERBOSITY default
DROP INDEX cic_reset_snap.dup_idx;

-- The parallel leader must also leave unchecked duplicates for validation.
SET index_build_duplicate_check_scale_factor = 0;
CREATE UNIQUE INDEX CONCURRENTLY dup_idx ON cic_reset_snap.dup(i);
RESET index_build_duplicate_check_scale_factor;
DROP TABLE cic_reset_snap.dup;

-- With no heap-fetch budget, ordinary NULLs must still be written; only
-- NULLS NOT DISTINCT may defer them to validation and report a conflict.
SET index_build_duplicate_check_scale_factor = 0;
CREATE TABLE cic_reset_snap.dupnull(i int);
ALTER TABLE cic_reset_snap.dupnull SET (parallel_workers=0);
INSERT INTO cic_reset_snap.dupnull SELECT g FROM generate_series(1, 3000) g;
INSERT INTO cic_reset_snap.dupnull VALUES (NULL), (NULL);
CREATE UNIQUE INDEX CONCURRENTLY dupnull_idx ON cic_reset_snap.dupnull(i);
DROP INDEX CONCURRENTLY cic_reset_snap.dupnull_idx;
CREATE UNIQUE INDEX CONCURRENTLY dupnull_idx ON cic_reset_snap.dupnull(i) NULLS NOT DISTINCT;
DROP TABLE cic_reset_snap.dupnull;
RESET index_build_duplicate_check_scale_factor;

-- numeric values 1500 and 1500.000 are equal despite different binary
-- representations.  With no heap-fetch budget, _bt_load() must recognize
-- the equal keys and leave the second entry for validation.
CREATE TABLE cic_reset_snap.dupn(n numeric);
ALTER TABLE cic_reset_snap.dupn SET (parallel_workers=0);
INSERT INTO cic_reset_snap.dupn SELECT g FROM generate_series(1, 1500) g;
INSERT INTO cic_reset_snap.dupn VALUES (1500.000);
INSERT INTO cic_reset_snap.dupn SELECT g FROM generate_series(1501, 3000) g;
SET index_build_duplicate_check_scale_factor = 0;
CREATE UNIQUE INDEX CONCURRENTLY dupn_idx ON cic_reset_snap.dupn(n);
RESET index_build_duplicate_check_scale_factor;
DROP TABLE cic_reset_snap.dupn;
SELECT injection_points_detach('bt_load_defer_unique_check');
SELECT injection_points_attach('bt_load_defer_unique_check', 'error');

-- Comparing enum values added with BEFORE/AFTER can acquire a catalog
-- snapshot on a syscache miss.  Force misses with debug_discard_caches,
-- where available, to test this during snapshot resets.
CREATE TYPE cic_reset_snap.rainbow AS ENUM ('red','green','blue');
ALTER TYPE cic_reset_snap.rainbow ADD VALUE 'teal' BEFORE 'green';
CREATE TABLE cic_reset_snap.enum_tbl(c cic_reset_snap.rainbow);
ALTER TABLE cic_reset_snap.enum_tbl SET (parallel_workers=0);
INSERT INTO cic_reset_snap.enum_tbl
    SELECT (CASE WHEN g % 2 = 0 THEN 'teal' ELSE 'blue' END)::cic_reset_snap.rainbow
    FROM generate_series(1, 200) g;
SELECT (max_val::int > 0) AS discard_caches_enabled
  FROM pg_settings WHERE name = 'debug_discard_caches' \gset
\if :discard_caches_enabled
SET debug_discard_caches = 1;
\endif
CREATE INDEX CONCURRENTLY enum_idx ON cic_reset_snap.enum_tbl(c);
RESET debug_discard_caches;
DROP TABLE cic_reset_snap.enum_tbl;

-- A cursor left open by a support function retains the scan snapshot.
-- The build must tolerate this, even though it keeps xmin pinned.
CREATE FUNCTION cic_reset_snap.leak_cursor(i int, at int) RETURNS bool IMMUTABLE
LANGUAGE plpgsql AS $$
DECLARE c refcursor := 'cic_leaked_cursor_' || at;
BEGIN
    IF i = at THEN OPEN c FOR SELECT 1; END IF;
    RETURN true;
END; $$;

-- Report the horizon pinned by the cursor instead of raising an error.
SELECT injection_points_detach('heap_reset_scan_snapshot_xmin_pinned');
SELECT injection_points_attach('heap_reset_scan_snapshot_xmin_pinned', 'notice');

-- No reset occurs after this small scan opens the cursor.
CREATE TABLE cic_reset_snap.cur_small(i int);
ALTER TABLE cic_reset_snap.cur_small SET (parallel_workers=0);
INSERT INTO cic_reset_snap.cur_small SELECT g FROM generate_series(1, 10) g;
CREATE INDEX CONCURRENTLY cur_small_idx ON cic_reset_snap.cur_small(i)
    WHERE cic_reset_snap.leak_cursor(i, 5);
DROP TABLE cic_reset_snap.cur_small;

-- The larger table reaches another reset after opening the cursor.
-- Disable the notice: its count depends on the page layout.
SELECT injection_points_detach('heap_reset_scan_snapshot_xmin_pinned');

CREATE TABLE cic_reset_snap.cur_tbl(i int);
ALTER TABLE cic_reset_snap.cur_tbl SET (parallel_workers=0);
INSERT INTO cic_reset_snap.cur_tbl SELECT g FROM generate_series(1, 5000) g;
CREATE INDEX CONCURRENTLY cur_idx ON cic_reset_snap.cur_tbl(i)
    WHERE cic_reset_snap.leak_cursor(i, 3000);
DROP TABLE cic_reset_snap.cur_tbl;

DROP SCHEMA cic_reset_snap CASCADE;

SELECT injection_points_detach('heap_beginscan_reset_snapshot');
SELECT injection_points_detach('table_parallelscan_initialize_reset_snapshot');
SELECT injection_points_detach('bt_load_defer_unique_check');

DROP EXTENSION injection_points;
