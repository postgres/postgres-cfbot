CREATE TABLE bttest_a(id int8);
CREATE TABLE bttest_b(id int8);
CREATE TABLE bttest_multi(id int8, data int8);
CREATE TABLE delete_test_table (a bigint, b bigint, c bigint, d bigint);

-- Stabilize tests
ALTER TABLE bttest_a SET (autovacuum_enabled = false);
ALTER TABLE bttest_b SET (autovacuum_enabled = false);
ALTER TABLE bttest_multi SET (autovacuum_enabled = false);
ALTER TABLE delete_test_table SET (autovacuum_enabled = false);

INSERT INTO bttest_a SELECT * FROM generate_series(1, 100000);
INSERT INTO bttest_b SELECT * FROM generate_series(100000, 1, -1);
INSERT INTO bttest_multi SELECT i, i%2  FROM generate_series(1, 100000) as i;

CREATE INDEX bttest_a_idx ON bttest_a USING btree (id) WITH (deduplicate_items = ON);
CREATE INDEX bttest_b_idx ON bttest_b USING btree (id);
CREATE UNIQUE INDEX bttest_multi_idx ON bttest_multi
USING btree (id) INCLUDE (data);

CREATE ROLE regress_bttest_role;

-- verify permissions are checked (error due to function not callable)
SET ROLE regress_bttest_role;
SELECT bt_index_check('bttest_a_idx'::regclass);
SELECT bt_index_parent_check('bttest_a_idx'::regclass);
RESET ROLE;

-- we, intentionally, don't check relation permissions - it's useful
-- to run this cluster-wide with a restricted account, and as tested
-- above explicit permission has to be granted for that.
GRANT EXECUTE ON FUNCTION bt_index_check(regclass) TO regress_bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_parent_check(regclass) TO regress_bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_check(regclass, boolean) TO regress_bttest_role;
GRANT EXECUTE ON FUNCTION bt_index_parent_check(regclass, boolean) TO regress_bttest_role;
SET ROLE regress_bttest_role;
SELECT bt_index_check('bttest_a_idx');
SELECT bt_index_parent_check('bttest_a_idx');
RESET ROLE;

-- verify plain tables are rejected (error)
SELECT bt_index_check('bttest_a');
SELECT bt_index_parent_check('bttest_a');

-- verify non-existing indexes are rejected (error)
SELECT bt_index_check(17);
SELECT bt_index_parent_check(17);

-- verify wrong index types are rejected (error)
BEGIN;
CREATE INDEX bttest_a_brin_idx ON bttest_a USING brin(id);
SELECT bt_index_parent_check('bttest_a_brin_idx');
ROLLBACK;

-- verify partitioned indexes are rejected (error)
BEGIN;
CREATE TABLE bttest_partitioned (a int, b int) PARTITION BY list (a);
CREATE INDEX bttest_btree_partitioned_idx ON bttest_partitioned USING btree (b);
SELECT bt_index_parent_check('bttest_btree_partitioned_idx');
ROLLBACK;

-- normal check outside of xact
SELECT bt_index_check('bttest_a_idx');
-- more expansive tests
SELECT bt_index_check('bttest_a_idx', true);
SELECT bt_index_parent_check('bttest_b_idx', true);

BEGIN;
SELECT bt_index_check('bttest_a_idx');
SELECT bt_index_parent_check('bttest_b_idx');
-- make sure we don't have any leftover locks
SELECT * FROM pg_locks
WHERE relation = ANY(ARRAY['bttest_a', 'bttest_a_idx', 'bttest_b', 'bttest_b_idx']::regclass[])
    AND pid = pg_backend_pid();
COMMIT;

-- Deduplication
TRUNCATE bttest_a;
INSERT INTO bttest_a SELECT 42 FROM generate_series(1, 2000);
SELECT bt_index_check('bttest_a_idx', true);

-- normal check outside of xact for index with included columns
SELECT bt_index_check('bttest_multi_idx');
-- more expansive tests for index with included columns
SELECT bt_index_parent_check('bttest_multi_idx', true, true);

-- repeat expansive tests for index built using insertions
TRUNCATE bttest_multi;
INSERT INTO bttest_multi SELECT i, i%2  FROM generate_series(1, 100000) as i;
SELECT bt_index_parent_check('bttest_multi_idx', true, true);

--
-- Test for multilevel page deletion/downlink present checks, and rootdescend
-- checks
--
INSERT INTO delete_test_table SELECT i, 1, 2, 3 FROM generate_series(1,80000) i;
ALTER TABLE delete_test_table ADD PRIMARY KEY (a,b,c,d);
-- Delete most entries, and vacuum, deleting internal pages and creating "fast
-- root"
DELETE FROM delete_test_table WHERE a < 79990;
VACUUM delete_test_table;
SELECT bt_index_parent_check('delete_test_table_pkey', true);

--
-- BUG #15597: must not assume consistent input toasting state when forming
-- tuple.  Bloom filter must fingerprint normalized index tuple representation.
--
CREATE TABLE toast_bug(buggy text);
ALTER TABLE toast_bug ALTER COLUMN buggy SET STORAGE extended;
CREATE INDEX toasty ON toast_bug(buggy);

-- pg_attribute entry for toasty.buggy (the index) will have plain storage:
UPDATE pg_attribute SET attstorage = 'p'
WHERE attrelid = 'toasty'::regclass AND attname = 'buggy';

-- Whereas pg_attribute entry for toast_bug.buggy (the table) still has extended storage:
SELECT attstorage FROM pg_attribute
WHERE attrelid = 'toast_bug'::regclass AND attname = 'buggy';

-- Insert compressible heap tuple (comfortably exceeds TOAST_TUPLE_THRESHOLD):
INSERT INTO toast_bug SELECT repeat('a', 2200);
-- Should not get false positive report of corruption:
SELECT bt_index_check('toasty', true);

--
-- Check that index expressions and predicates are run as the table's owner,
-- with empty search_path
--
TRUNCATE bttest_a;
INSERT INTO bttest_a SELECT * FROM generate_series(1, 1000);
ALTER TABLE bttest_a OWNER TO regress_bttest_role;
-- A dummy index function checking current_user
CREATE FUNCTION ifun(int8) RETURNS int8 AS $$
BEGIN
	ASSERT current_setting('search_path') = 'pg_catalog, pg_temp',
		format('ifun(%s) called with current_schemas %s, search_path %s',
			$1, current_schemas(true), current_setting('search_path'));
	ASSERT "current_user"() = 'regress_bttest_role',
		format('ifun(%s) called by %s', $1, current_user);
	RETURN $1;
END;
$$ LANGUAGE plpgsql IMMUTABLE;

CREATE INDEX bttest_a_expr_idx ON bttest_a ((ifun(id) + ifun(0)))
	WHERE ifun(id + 10) > ifun(10);
BEGIN;
SET LOCAL check_function_bodies = off;
CREATE SCHEMA preempt;
GRANT USAGE ON SCHEMA preempt TO regress_bttest_role;
SET LOCAL search_path = preempt, pg_catalog, public;
CREATE FUNCTION "current_user"() RETURNS name AS $$
	broken
$$ LANGUAGE sql STABLE PARALLEL SAFE STRICT;
SELECT bt_index_check('bttest_a_expr_idx', true);
ROLLBACK;

-- UNIQUE constraint check
SELECT bt_index_check('bttest_a_idx', heapallindexed => true, checkunique => true);
SELECT bt_index_check('bttest_b_idx', heapallindexed => false, checkunique => true);
SELECT bt_index_parent_check('bttest_a_idx', heapallindexed => true, rootdescend => true, checkunique => true);
SELECT bt_index_parent_check('bttest_b_idx', heapallindexed => true, rootdescend => false, checkunique => true);

-- Check that null values in an unique index are not treated as equal
CREATE TABLE bttest_unique_nulls (a serial, b int, c int UNIQUE);
INSERT INTO bttest_unique_nulls VALUES (generate_series(1, 10000), 2, default);
SELECT bt_index_check('bttest_unique_nulls_c_key', heapallindexed => true, checkunique => true);
CREATE INDEX on bttest_unique_nulls (b,c);
SELECT bt_index_check('bttest_unique_nulls_b_c_idx', heapallindexed => true, checkunique => true);

-- Check support of both 1B and 4B header sizes of short varlena datum
CREATE TABLE varlena_bug (v text);
ALTER TABLE varlena_bug ALTER column v SET storage plain;
INSERT INTO varlena_bug VALUES ('x');
COPY varlena_bug from stdin;
x
\.
CREATE INDEX varlena_bug_idx on varlena_bug(v);
SELECT bt_index_check('varlena_bug_idx', true);

-- Also check that we compress varlena values, which were previously stored
-- uncompressed in index.
INSERT INTO varlena_bug VALUES (repeat('Test', 250));
ALTER TABLE varlena_bug ALTER COLUMN v SET STORAGE extended;
SELECT bt_index_check('varlena_bug_idx', true);

-- HOT updates of rows indexed by expression and partial indexes must leave
-- every index entry consistent with the heap.
CREATE TABLE hot_expr (id int PRIMARY KEY, s text, d jsonb,
                       status text, v int, n numeric) WITH (fillfactor = 50);
CREATE INDEX hot_expr_lower ON hot_expr (lower(s));
CREATE INDEX hot_expr_name ON hot_expr ((d->>'name'));
CREATE INDEX hot_expr_partial ON hot_expr (v) WHERE status = 'active';
CREATE INDEX hot_expr_numeric ON hot_expr ((n + 0));
INSERT INTO hot_expr
  SELECT g, 'abc' || g, jsonb_build_object('name', 'n' || g, 'age', g),
         CASE WHEN g % 2 = 0 THEN 'active' ELSE 'done' END, g, 3
    FROM generate_series(1, 200) g;
UPDATE hot_expr SET s = upper(s);
UPDATE hot_expr SET d = jsonb_set(d, '{age}', '0');
UPDATE hot_expr SET v = v + 1000 WHERE status = 'done';
UPDATE hot_expr SET status = 'done' WHERE id % 4 = 0;
UPDATE hot_expr SET status = 'active' WHERE id % 3 = 0;
UPDATE hot_expr SET n = 3.0 WHERE id % 5 = 0;
UPDATE hot_expr SET d = jsonb_set(d, '{name}', '"x"') WHERE id % 7 = 0;
SELECT bt_index_check('hot_expr_lower', heapallindexed => true);
SELECT bt_index_check('hot_expr_name', heapallindexed => true);
SELECT bt_index_check('hot_expr_partial', heapallindexed => true);
SELECT bt_index_check('hot_expr_numeric', heapallindexed => true);
SELECT bt_index_parent_check('hot_expr_partial', heapallindexed => true);
DROP TABLE hot_expr;

-- One UPDATE in which different rows are decided by different indexes: an
-- expression index created first, a partial index created after it.
CREATE TABLE hot_expr_multi (id int PRIMARY KEY, doc jsonb) WITH (fillfactor = 30);
CREATE INDEX hot_expr_multi_status ON hot_expr_multi ((doc->>'status'));
CREATE INDEX hot_expr_multi_vip ON hot_expr_multi ((doc->>'email'))
  WHERE (doc->>'vip')::bool;
INSERT INTO hot_expr_multi
  SELECT g, jsonb_build_object('status', 'a', 'email', 'e' || g,
                               'vip', g % 2 = 0, 'n', 0)
    FROM generate_series(1, 3000) g;
UPDATE hot_expr_multi SET doc = CASE
    WHEN id % 4 = 0 THEN jsonb_set(doc, '{status}', '"b"')
    WHEN id % 4 = 1 THEN jsonb_set(doc, '{vip}', to_jsonb(NOT (doc->>'vip')::bool))
    WHEN id % 4 = 2 THEN jsonb_set(jsonb_set(doc, '{status}', '"c"'),
                                   '{vip}', to_jsonb(NOT (doc->>'vip')::bool))
    ELSE jsonb_set(doc, '{n}', '1') END;
UPDATE hot_expr_multi SET doc = CASE
    WHEN id % 3 = 0 THEN jsonb_set(doc, '{vip}', to_jsonb(NOT (doc->>'vip')::bool))
    WHEN id % 3 = 1 THEN jsonb_set(doc, '{status}', '"d"')
    ELSE jsonb_set(doc, '{n}', '2') END;
SELECT bt_index_check('hot_expr_multi_status', heapallindexed => true);
SELECT bt_index_check('hot_expr_multi_vip', heapallindexed => true);
DROP TABLE hot_expr_multi;

-- An UPDATE that writes back the value a row has, when the old value is
-- stored compressed or out of line and the new one is not yet, is HOT: the
-- index would store the same bytes.  Index-only scans must return the new
-- row's value.
CREATE TABLE hot_bt_image (id int, t text, e text) WITH (fillfactor = 50);
ALTER TABLE hot_bt_image ALTER COLUMN e SET STORAGE EXTERNAL;
CREATE INDEX hot_bt_image_t ON hot_bt_image (t);
CREATE INDEX hot_bt_image_e ON hot_bt_image (e) INCLUDE (id);
INSERT INTO hot_bt_image
  SELECT g, repeat('x', 3000) || g, repeat('y', 2500) || g
    FROM generate_series(1, 20) g;
SELECT pg_column_compression(t) IS NOT NULL AS t_compressed,
       pg_column_size(e) > 2500 AS e_out_of_line
  FROM hot_bt_image WHERE id = 1;
CREATE FUNCTION hot_bt_image_count(OUT updated bigint, OUT hot bigint)
LANGUAGE sql AS $$
  SELECT pg_stat_get_xact_tuples_updated('hot_bt_image'::regclass),
         pg_stat_get_xact_tuples_hot_updated('hot_bt_image'::regclass) $$;
BEGIN;
UPDATE hot_bt_image SET t = repeat('x', 3000) || id;
SELECT * FROM hot_bt_image_count();
UPDATE hot_bt_image SET e = e || '';
SELECT * FROM hot_bt_image_count();
UPDATE hot_bt_image SET t = repeat('x', 3000) || (id + 100) WHERE id <= 5;
SELECT * FROM hot_bt_image_count();
COMMIT;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
CREATE FUNCTION hot_bt_image_plan() RETURNS SETOF text LANGUAGE plpgsql AS
$$ DECLARE l text; BEGIN
  FOR l IN EXPLAIN (COSTS OFF)
    SELECT right(t, 3) FROM hot_bt_image WHERE t > repeat('x', 3000)
  LOOP RETURN NEXT left(l, 40); END LOOP; END $$;
SELECT * FROM hot_bt_image_plan();
SELECT right(t, 3) FROM hot_bt_image WHERE t > repeat('x', 3000)
  ORDER BY t COLLATE "C" LIMIT 3;
SELECT count(*) FROM hot_bt_image WHERE t > repeat('x', 3000);
SELECT count(*) FROM hot_bt_image WHERE e > repeat('y', 2500);
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT bt_index_check('hot_bt_image_t', heapallindexed => true);
SELECT bt_index_check('hot_bt_image_e', heapallindexed => true);
DROP TABLE hot_bt_image;
DROP FUNCTION hot_bt_image_count;
DROP FUNCTION hot_bt_image_plan;

-- cleanup
DROP TABLE bttest_a;
DROP TABLE bttest_b;
DROP TABLE bttest_multi;
DROP TABLE delete_test_table;
DROP TABLE toast_bug;
DROP FUNCTION ifun(int8);
DROP TABLE bttest_unique_nulls;
DROP OWNED BY regress_bttest_role; -- permissions
DROP ROLE regress_bttest_role;
DROP TABLE varlena_bug;
