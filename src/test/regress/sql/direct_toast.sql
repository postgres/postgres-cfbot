--
-- Tests for direct TOAST flavour
--

SET toast_default_flavour = 'direct';

-- Check GUC
SHOW toast_default_flavour;

CREATE TABLE dirtoasttest(descr text, f1 text) WITH (toast_flavour = 'direct');
ALTER TABLE dirtoasttest ALTER COLUMN f1 SET STORAGE EXTERNAL;

-- Single-chunk toast (or small multi-chunk)
INSERT INTO dirtoasttest VALUES ('toasted-1', repeat('1234567890', 1000)); -- 10KB (uncompressed, so ~5 chunks)

-- Multi-chunk toast
INSERT INTO dirtoasttest VALUES ('toasted-multi', repeat('1234567890', 5000)); -- 50KB (uncompressed, so ~25 chunks)
REINDEX TABLE dirtoasttest;

-- Verify toast table structure and contents
DO $$
DECLARE
    toast_relname text;
    all_id_null bool;
    root_chunks int;
    has_leaf_chunks bool;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'dirtoasttest');
    IF toast_relname IS NOT NULL THEN
        EXECUTE 'SELECT bool_and(chunk_id IS NULL), count(*) FILTER (WHERE chunk_tids IS NOT NULL), bool_or(chunk_tids IS NULL) FROM ' || toast_relname
            INTO all_id_null, root_chunks, has_leaf_chunks;
        RAISE NOTICE 'direct toast structure: all_id_null=%, root_chunks=%, leaf_chunks=%',
            all_id_null, root_chunks, has_leaf_chunks;
    ELSE
        RAISE NOTICE 'no toast table';
    END IF;
END$$;

-- Read only descr (should work)
SELECT descr FROM dirtoasttest;
-- Read f1 IS NULL (should work, and return false)
SELECT descr, f1 IS NULL FROM dirtoasttest;

-- Read values while GUC is still 'direct'
SELECT descr, length(f1), substring(f1, 1, 10), substring(f1, length(f1)-9, 10) FROM dirtoasttest;

-- Reset GUC to plain and try reading (should still work because read path is automatic)
SET toast_default_flavour = 'plain';
SHOW toast_default_flavour;

SELECT descr, length(f1), substring(f1, 1, 10), substring(f1, length(f1)-9, 10) FROM dirtoasttest;

-- Test slice reading
SELECT descr, substring(f1, 500, 20) FROM dirtoasttest WHERE descr = 'toasted-multi';
SELECT descr, substring(f1, 45000, 20) FROM dirtoasttest WHERE descr = 'toasted-multi';

-- Test update after setting table toast_flavour to 'plain'
ALTER TABLE dirtoasttest SET (toast_flavour = 'plain');
UPDATE dirtoasttest SET f1 = f1 || 'edited' WHERE descr = 'toasted-1';
SELECT descr, length(f1), substring(f1, length(f1)-9, 10) FROM dirtoasttest WHERE descr = 'toasted-1';

-- Toast table should now contain some 'plain' toast (no tid array) and some 'direct' toast.
-- The updated 'toasted-1' should be plain.
-- Let's check toast table again.
DO $$
DECLARE
    toast_relname text;
    direct_roots int;
    has_plain_chunks bool;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'dirtoasttest');
    IF toast_relname IS NOT NULL THEN
        EXECUTE 'SELECT count(*) FILTER (WHERE chunk_id IS NULL AND chunk_tids IS NOT NULL), bool_or(chunk_id IS NOT NULL) FROM ' || toast_relname
            INTO direct_roots, has_plain_chunks;
        RAISE NOTICE 'mixed toast structure: direct_roots=%, plain_chunks=%',
            direct_roots, has_plain_chunks;
    END IF;
END$$;

-- Delete and vacuum
DELETE FROM dirtoasttest;
VACUUM dirtoasttest;

-- Toast table should be empty
DO $$
DECLARE
    toast_relname text;
    cnt int;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'dirtoasttest');
    IF toast_relname IS NOT NULL THEN
        EXECUTE 'SELECT count(*) FROM ' || toast_relname INTO cnt;
        RAISE NOTICE 'toast table row count: %', cnt;
    ELSE
        RAISE NOTICE 'no toast table';
    END IF;
END$$;

-- Verify index skip and InvalidOid usage
-- We insert two direct toast values. They should both get chunk_id = NULL.
-- Since the index is partial (WHERE chunk_id IS NOT NULL), they won't be indexed,
-- and thus won't conflict on the unique index.
ALTER TABLE dirtoasttest SET (toast_flavour = 'direct');
INSERT INTO dirtoasttest VALUES ('toasted-idx-1', repeat('a', 3000));
INSERT INTO dirtoasttest VALUES ('toasted-idx-2', repeat('b', 3000));
-- Should succeed.

-- Verify they have chunk_id = NULL
DO $$
DECLARE
    toast_relname text;
    r record;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'dirtoasttest');
    FOR r IN EXECUTE 'SELECT chunk_id IS NULL as is_direct, chunk_seq, chunk_tids IS NULL as tids_isnull FROM ' || toast_relname || ' ORDER BY chunk_seq, tids_isnull' LOOP
        RAISE NOTICE 'dirtoasttest chunk: is_direct=%, seq=%, tids_isnull=%', r.is_direct, r.chunk_seq, r.tids_isnull;
    END LOOP;
END$$;

REINDEX TABLE dirtoasttest;

DROP TABLE dirtoasttest;

-- Test GUC 'toast_default_flavour' and Table Storage Parameter 'toast_flavour'
SET toast_default_flavour = 'plain'; -- GUC is plain

-- 1. Table option 'direct' creates a 5-column TOAST table and writes direct TOAST
CREATE TABLE tab_direct(descr text, f1 text) WITH (toast_flavour = 'direct');
ALTER TABLE tab_direct ALTER COLUMN f1 SET STORAGE EXTERNAL;
INSERT INTO tab_direct VALUES ('opt-direct', repeat('d', 3000));

-- Verify it is direct (chunk_tids is not null, and chunk_id is NULL)
DO $$
DECLARE
    toast_relname text;
    r record;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'tab_direct');
    FOR r IN EXECUTE 'SELECT chunk_id IS NULL as is_direct, chunk_tids IS NULL as tids_isnull FROM ' || toast_relname LOOP
        RAISE NOTICE 'tab_direct chunk: is_direct=%, tids_isnull=%', r.is_direct, r.tids_isnull;
    END LOOP;
END$$;

-- 2. Table option 'plain', GUC is 'direct' (creates 5-column TOAST table, writes plain)
SET toast_default_flavour = 'direct';
CREATE TABLE tab_plain(descr text, f1 text) WITH (toast_flavour = 'plain');
ALTER TABLE tab_plain ALTER COLUMN f1 SET STORAGE EXTERNAL;
INSERT INTO tab_plain VALUES ('opt-plain', repeat('p', 3000));

-- Verify it is plain (chunk_tids is null, chunk_id is NOT NULL)
DO $$
DECLARE
    toast_relname text;
    r record;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'tab_plain');
    FOR r IN EXECUTE 'SELECT chunk_id IS NULL as is_direct, chunk_tids IS NULL as tids_isnull FROM ' || toast_relname LOOP
        RAISE NOTICE 'tab_plain chunk: is_direct=%, tids_isnull=%', r.is_direct, r.tids_isnull;
    END LOOP;
END$$;

-- 3. Default (no table option) with toast_default_flavour = 'direct':
-- TOAST table is created in 5-column direct format, while actual writes use default 'plain'
CREATE TABLE tab_default(descr text, f1 text);
ALTER TABLE tab_default ALTER COLUMN f1 SET STORAGE EXTERNAL;

-- Even when toast_default_flavour is 'direct', inserts use table's toast_flavour ('plain')
INSERT INTO tab_default VALUES ('default-plain-1', repeat('g', 3000));

SET toast_default_flavour = 'plain';
INSERT INTO tab_default VALUES ('default-plain-2', repeat('h', 3000));

-- Verify contents (TOAST table has chunk_tids column, all chunks are plain)
DO $$
DECLARE
    toast_relname text;
    r record;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'tab_default');
    FOR r IN EXECUTE 'SELECT chunk_id IS NULL as is_direct, chunk_tids IS NULL as tids_isnull FROM ' || toast_relname || ' ORDER BY is_direct desc, tids_isnull' LOOP
        RAISE NOTICE 'tab_default chunk: is_direct=%, tids_isnull=%', r.is_direct, r.tids_isnull;
    END LOOP;
END$$;

-- 4. Alter table SET toast_flavour
ALTER TABLE tab_default SET (toast_flavour = 'direct');
-- GUC is plain -> should write direct because of table option (chunk_id = NULL)
INSERT INTO tab_default VALUES ('default-altered-direct', repeat('i', 3000));

-- 5. Alter table RESET toast_flavour
ALTER TABLE tab_default RESET (toast_flavour);
-- Should write plain (chunk_id <> NULL)
INSERT INTO tab_default VALUES ('default-reset-plain', repeat('j', 3000));

-- Verify after alters
DO $$
DECLARE
    toast_relname text;
    r record;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'tab_default');
    FOR r IN EXECUTE 'SELECT chunk_id IS NULL as is_direct, chunk_tids IS NULL as tids_isnull FROM ' || toast_relname || ' ORDER BY is_direct desc, tids_isnull' LOOP
        RAISE NOTICE 'tab_default altered chunk: is_direct=%, tids_isnull=%', r.is_direct, r.tids_isnull;
    END LOOP;
END$$;

-- Clean up
DROP TABLE tab_direct;
DROP TABLE tab_plain;
DROP TABLE tab_default;

--
-- Test Recursive Tree Direct TOAST (>100 chunks, with chunk_tid_offsets)
--
CREATE TABLE tab_tree(descr text, f1 text) WITH (toast_flavour = 'direct');
ALTER TABLE tab_tree ALTER COLUMN f1 SET STORAGE EXTERNAL;

-- 241,200 bytes (~120 chunks > 100 threshold -> 120 leaf chunks, 3 level-1 nodes, 1 root node)
INSERT INTO tab_tree VALUES ('tree-toast-1', repeat('abcdefghijklmnopqrstuvwxyz0123456789', 6700));

-- Verify table length and checksum
SELECT descr, length(f1), md5(f1) = md5(repeat('abcdefghijklmnopqrstuvwxyz0123456789', 6700)) as md5_match FROM tab_tree;

-- Verify slices: start, middle crossing chunk/node boundaries, end
SELECT descr, substring(f1, 1, 36) FROM tab_tree;
SELECT descr, substring(f1, 1990, 36) FROM tab_tree;
SELECT descr, substring(f1, 99990, 36) FROM tab_tree;
SELECT descr, substring(f1, 241165, 36) FROM tab_tree;

-- Inspect toast table structure for tree nodes
DO $$
DECLARE
    toast_relname text;
    r record;
    leaf_count int := 0;
    node_count int := 0;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'tab_tree');
    FOR r IN EXECUTE 'SELECT chunk_id IS NULL as id_null, chunk_data IS NULL as data_null, array_length(chunk_tids, 1) as num_tids, array_length(chunk_tid_offsets, 1) as num_offsets FROM ' || toast_relname || ' ORDER BY chunk_seq' LOOP
        IF r.data_null THEN
            node_count := node_count + 1;
            IF r.num_offsets <> r.num_tids + 1 THEN
                RAISE EXCEPTION 'offset count % does not match tid count + 1 (%)', r.num_offsets, r.num_tids + 1;
            END IF;
        ELSE
            leaf_count := leaf_count + 1;
        END IF;
    END LOOP;
    RAISE NOTICE 'tree toast structure: leaf_count=%, node_count=%', leaf_count, node_count;
END$$;

-- Verify root node offsets span from 0 to full length
DO $$
DECLARE
    toast_relname text;
    root_offsets bigint[];
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'tab_tree');
    EXECUTE 'SELECT chunk_tid_offsets FROM ' || toast_relname || ' WHERE chunk_data IS NULL ORDER BY chunk_seq DESC LIMIT 1' INTO root_offsets;
    RAISE NOTICE 'root offsets: first=%, last=%', root_offsets[1], root_offsets[array_length(root_offsets, 1)];
END$$;

-- Test update with tree toast
UPDATE tab_tree SET f1 = f1 || '_updated';
SELECT descr, length(f1), substring(f1, 241200, 9) FROM tab_tree;

-- Delete and vacuum
DELETE FROM tab_tree;
VACUUM tab_tree;

DO $$
DECLARE
    toast_relname text;
    cnt int;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'tab_tree');
    EXECUTE 'SELECT count(*) FROM ' || toast_relname INTO cnt;
    RAISE NOTICE 'tree toast table count after vacuum: %', cnt;
END$$;

DROP TABLE tab_tree;

--
-- Test Compression and Chunk ID Introspection on Direct TOAST
--
SET default_toast_compression = 'pglz';

CREATE TABLE tab_intro_plain(descr text, f text) WITH (toast_flavour = 'plain');
CREATE TABLE tab_intro_direct(descr text, f text) WITH (toast_flavour = 'direct');

INSERT INTO tab_intro_plain SELECT 'uncompressed-external-plain', string_agg(md5(i::text), '') FROM generate_series(1, 200) i;
INSERT INTO tab_intro_direct SELECT 'uncompressed-external-direct', string_agg(md5(i::text), '') FROM generate_series(1, 200) i;

INSERT INTO tab_intro_plain VALUES ('compressed-external-plain', repeat('abcdefghijklmnopqrstuvwxyz0123456789', 5000));
INSERT INTO tab_intro_direct VALUES ('compressed-external-direct', repeat('abcdefghijklmnopqrstuvwxyz0123456789', 5000));

SELECT p.descr, pg_column_compression(p.f) AS plain_comp, pg_column_toast_chunk_id(p.f) IS NOT NULL AS plain_has_chunk_id
FROM tab_intro_plain p
ORDER BY p.descr;

SELECT d.descr, pg_column_compression(d.f) AS direct_comp, pg_column_toast_chunk_id(d.f) AS direct_chunk_id
FROM tab_intro_direct d
ORDER BY d.descr;

DROP TABLE tab_intro_plain;
DROP TABLE tab_intro_direct;

RESET default_toast_compression;

--
-- Test Partitioned Tables with Mixed Toast Flavours and Cross-Partition Updates
--
CREATE TABLE part_toast(id int, val text) PARTITION BY RANGE (id);
CREATE TABLE part_toast_p1 PARTITION OF part_toast FOR VALUES FROM (1) TO (100) WITH (toast_flavour = 'direct');
CREATE TABLE part_toast_p2 PARTITION OF part_toast FOR VALUES FROM (100) TO (200) WITH (toast_flavour = 'plain');

INSERT INTO part_toast SELECT 1, string_agg(md5(i::text), '') FROM generate_series(1, 200) i;
INSERT INTO part_toast SELECT 101, string_agg(md5(i::text), '') FROM generate_series(1, 200) i;

SELECT id, length(val), substring(val, 1, 10), pg_column_toast_chunk_id(val) IS NOT NULL AS has_chunk_id
FROM part_toast
ORDER BY id;

-- Move row from direct partition to plain partition
UPDATE part_toast SET id = 102 WHERE id = 1;
SELECT id, length(val), substring(val, 1, 10), pg_column_toast_chunk_id(val) IS NOT NULL AS has_chunk_id
FROM part_toast
ORDER BY id;

-- Move row from plain partition to direct partition
UPDATE part_toast SET id = 2 WHERE id = 101;
SELECT id, length(val), substring(val, 1, 10), pg_column_toast_chunk_id(val) IS NOT NULL AS has_chunk_id
FROM part_toast
ORDER BY id;

DROP TABLE part_toast;

--
-- Test Expression / Functional Indexes on Direct TOAST Columns
--
CREATE TABLE tab_expr_idx(id int primary key, payload text) WITH (toast_flavour = 'direct');
CREATE INDEX idx_tab_expr_md5 ON tab_expr_idx (md5(payload));
CREATE INDEX idx_tab_expr_substr ON tab_expr_idx (substring(payload, 1, 20));

INSERT INTO tab_expr_idx VALUES (1, repeat('expr-index-test-payload-', 500));
INSERT INTO tab_expr_idx VALUES (2, repeat('other-index-test-payload-', 500));

SET enable_seqscan = off;

SELECT id, length(payload) FROM tab_expr_idx WHERE md5(payload) = md5(repeat('expr-index-test-payload-', 500));
SELECT id, length(payload) FROM tab_expr_idx WHERE substring(payload, 1, 20) = 'expr-index-test-payl';

RESET enable_seqscan;

--
-- Test Table Maintenance and Rewrites (VACUUM FULL, CLUSTER, ALTER TYPE, TRUNCATE)
--
VACUUM FULL tab_expr_idx;
SELECT id, length(payload), substring(payload, 1, 24) FROM tab_expr_idx ORDER BY id;

CLUSTER tab_expr_idx USING tab_expr_idx_pkey;
SELECT id, length(payload), substring(payload, 1, 24) FROM tab_expr_idx ORDER BY id;

ALTER TABLE tab_expr_idx ALTER COLUMN payload TYPE varchar(20000);
SELECT id, length(payload), substring(payload, 1, 24) FROM tab_expr_idx ORDER BY id;

TRUNCATE tab_expr_idx;
SELECT count(*) FROM tab_expr_idx;

DROP TABLE tab_expr_idx;

--
-- Test VACUUM FULL / CLUSTER restrictions on direct TOAST tables
--
CREATE TABLE tab_toast_maint(id int, val text) WITH (toast_flavour = 'direct');
INSERT INTO tab_toast_maint VALUES (1, repeat('maint-test-', 500));

-- VACUUM FULL on the parent table succeeds and rebuilds direct toast safely
VACUUM FULL tab_toast_maint;
SELECT id, length(val) FROM tab_toast_maint;

-- CLUSTER and REPACK on direct TOAST table directly are rejected
DO $$
DECLARE
    toast_relname text;
    toast_idxname text;
BEGIN
    SELECT c2.relname, c3.relname INTO toast_relname, toast_idxname
    FROM pg_class c1
    JOIN pg_class c2 ON c1.reltoastrelid = c2.oid
    JOIN pg_index i ON c2.oid = i.indrelid
    JOIN pg_class c3 ON i.indexrelid = c3.oid
    WHERE c1.relname = 'tab_toast_maint';

    -- CLUSTER directly on direct TOAST table should be rejected
    BEGIN
        EXECUTE 'CLUSTER pg_toast.' || toast_relname || ' USING ' || toast_idxname;
        RAISE EXCEPTION 'CLUSTER on direct TOAST table should have failed';
    EXCEPTION WHEN feature_not_supported THEN
        RAISE NOTICE 'expected error caught for CLUSTER on direct TOAST table: %', regexp_replace(SQLERRM, 'pg_toast_[0-9]+_index', 'pg_toast_xxx_index');
    END;

    -- REPACK directly on direct TOAST table should be rejected
    BEGIN
        EXECUTE 'REPACK pg_toast.' || toast_relname;
        RAISE EXCEPTION 'REPACK on direct TOAST table should have failed';
    EXCEPTION WHEN feature_not_supported THEN
        RAISE NOTICE 'expected error caught for REPACK on direct TOAST table: %', SQLERRM;
    END;
END$$;

-- VACUUM FULL targeting only the direct TOAST table is also rejected
VACUUM (PROCESS_MAIN FALSE, FULL) tab_toast_maint;

DROP TABLE tab_toast_maint;

--
-- Test pg_ensure_direct_toast and legacy TOAST table in-place upgrade
--
CREATE TABLE tab_legacy_test(id int, val text);
ALTER TABLE tab_legacy_test ALTER COLUMN val SET STORAGE EXTERNAL;
INSERT INTO tab_legacy_test VALUES (1, repeat('legacy-plain-payload-', 300));

-- Set toast_flavour = 'direct' first, then simulate an un-upgraded 3-column TOAST table
-- to test the write-time guard in toast_save_datum_direct()
ALTER TABLE tab_legacy_test SET (toast_flavour = 'direct');

DO $$
DECLARE
    toast_relid oid;
    toast_idxid oid;
BEGIN
    SELECT c1.reltoastrelid INTO toast_relid
    FROM pg_class c1
    WHERE c1.relname = 'tab_legacy_test';

    SELECT indexrelid INTO toast_idxid
    FROM pg_index
    WHERE indrelid = toast_relid;

    -- Delete attributes 4 and 5 from pg_attribute
    DELETE FROM pg_attribute WHERE attrelid = toast_relid AND attnum IN (4, 5);
    UPDATE pg_class SET relnatts = 3 WHERE oid = toast_relid;

    -- Clear index predicate from pg_index
    UPDATE pg_index SET indpred = NULL WHERE indexrelid = toast_idxid;
END$$;

\c -

-- Attempting direct write to legacy TOAST table should fail with descriptive error & hint
DO $$
BEGIN
    INSERT INTO tab_legacy_test VALUES (2, repeat('direct-write-attempt-', 300));
    RAISE EXCEPTION 'direct write to legacy table should have failed';
EXCEPTION WHEN feature_not_supported THEN
    RAISE NOTICE 'expected error caught: %', regexp_replace(SQLERRM, 'pg_toast_[0-9]+', 'pg_toast_xxx');
END$$;

-- Read legacy plain data still works
SELECT id, length(val), substring(val, 1, 20) FROM tab_legacy_test WHERE id = 1;

-- Unprivileged role cannot call pg_ensure_direct_toast
CREATE ROLE regress_dtoast_user;
SET ROLE regress_dtoast_user;
SELECT pg_ensure_direct_toast('tab_legacy_test'::regclass);
RESET ROLE;
DROP ROLE regress_dtoast_user;

-- Upgrade using pg_ensure_direct_toast
SELECT pg_ensure_direct_toast('tab_legacy_test'::regclass);

-- Direct write now succeeds!
INSERT INTO tab_legacy_test VALUES (2, repeat('direct-write-success-', 300));

-- Read both plain and direct rows
SELECT id, length(val), substring(val, 1, 20) FROM tab_legacy_test ORDER BY id;

-- Test ALTER TABLE SET (toast_flavour = 'direct') on a naturally created 3-column TOAST table
CREATE TABLE tab_legacy_alter(id int, val text);
ALTER TABLE tab_legacy_alter ALTER COLUMN val SET STORAGE EXTERNAL;
INSERT INTO tab_legacy_alter VALUES (1, repeat('legacy-alter-payload-', 300));

-- Verify initial 3-column format and that REPACK and VACUUM FULL directly on the 3-column TOAST table are allowed
SELECT t.relnatts, i.indisprimary, i.indpred IS NOT NULL AS has_pred
FROM pg_class c
JOIN pg_class t ON t.oid = c.reltoastrelid
JOIN pg_index i ON i.indrelid = t.oid
WHERE c.relname = 'tab_legacy_alter';

VACUUM (PROCESS_MAIN FALSE, FULL) tab_legacy_alter;

DO $$
DECLARE
    toast_relname text;
BEGIN
    SELECT t.relname INTO toast_relname
    FROM pg_class c JOIN pg_class t ON t.oid = c.reltoastrelid
    WHERE c.relname = 'tab_legacy_alter';
    EXECUTE 'REPACK pg_toast.' || toast_relname;
END$$;

-- Verify that VACUUM FULL on the parent table while toast_default_flavour = 'direct'
-- preserves the existing 3-column TOAST table format
SET toast_default_flavour = 'direct';
VACUUM FULL tab_legacy_alter;
RESET toast_default_flavour;

SELECT t.relnatts, i.indisprimary, i.indpred IS NOT NULL AS has_pred
FROM pg_class c
JOIN pg_class t ON t.oid = c.reltoastrelid
JOIN pg_index i ON i.indrelid = t.oid
WHERE c.relname = 'tab_legacy_alter';

-- Alter table SET toast_flavour = 'direct' automatically calls ensure_direct_toast
ALTER TABLE tab_legacy_alter SET (toast_flavour = 'direct');

-- Verify upgraded 5-column format and partial non-primary index
SELECT t.relnatts, i.indisprimary, i.indpred IS NOT NULL AS has_pred
FROM pg_class c
JOIN pg_class t ON t.oid = c.reltoastrelid
JOIN pg_index i ON i.indrelid = t.oid
WHERE c.relname = 'tab_legacy_alter';

-- Direct write now succeeds
INSERT INTO tab_legacy_alter VALUES (2, repeat('alter-direct-success-', 300));

-- Read both rows
SELECT id, length(val), substring(val, 1, 20) FROM tab_legacy_alter ORDER BY id;

DROP TABLE tab_legacy_test;
DROP TABLE tab_legacy_alter;

-- Test that toast_default_flavour = 'direct' creates a 5-column TOAST table,
-- while INSERT and UPDATE still use the table's toast_flavour reloption ('plain' by default),
-- and VACUUM (PROCESS_MAIN FALSE, FULL) refuses based on the 5-column TOAST table format
SET toast_default_flavour = 'direct';
CREATE TABLE tab_default_direct_guc(id int, val text STORAGE EXTERNAL);
RESET toast_default_flavour;

SELECT t.relnatts, i.indisprimary, i.indpred IS NOT NULL AS has_pred
FROM pg_class c
JOIN pg_class t ON t.oid = c.reltoastrelid
JOIN pg_index i ON i.indrelid = t.oid
WHERE c.relname = 'tab_default_direct_guc';

INSERT INTO tab_default_direct_guc VALUES (1, repeat('guc-only-plain-insert-', 300));
SELECT pg_column_toast_chunk_id(val) IS NOT NULL AS is_plain_after_insert FROM tab_default_direct_guc WHERE id = 1;

UPDATE tab_default_direct_guc SET val = repeat('guc-only-plain-update-', 300) WHERE id = 1;
SELECT pg_column_toast_chunk_id(val) IS NOT NULL AS is_plain_after_update FROM tab_default_direct_guc WHERE id = 1;

VACUUM (PROCESS_MAIN FALSE, FULL) tab_default_direct_guc;

DROP TABLE tab_default_direct_guc;

-- Test ALTER TABLE ADD COLUMN adding the first toastable column to a table with toast_flavour = 'direct'
CREATE TABLE tab_addcol_direct(id int) WITH (toast_flavour = 'direct');
ALTER TABLE tab_addcol_direct ADD COLUMN val text STORAGE EXTERNAL;

SELECT t.relnatts, i.indisprimary, i.indpred IS NOT NULL AS has_pred
FROM pg_class c
JOIN pg_class t ON t.oid = c.reltoastrelid
JOIN pg_index i ON i.indrelid = t.oid
WHERE c.relname = 'tab_addcol_direct';

INSERT INTO tab_addcol_direct VALUES (1, repeat('addcol-direct-payload-', 300));
SELECT pg_column_toast_chunk_id(val) IS NULL AS is_direct_toast, length(val) FROM tab_addcol_direct WHERE id = 1;

DROP TABLE tab_addcol_direct;

--
-- Test direct_toast_self_prune GUC and reloption propagation
--
SHOW direct_toast_self_prune;

CREATE TABLE tab_self_prune_opt(id int, val text)
  WITH (toast_flavour = 'direct', direct_toast_self_prune = off);

SELECT c1.reloptions AS heap_opts, c2.reloptions AS toast_opts
FROM pg_class c1
JOIN pg_class c2 ON c1.reltoastrelid = c2.oid
WHERE c1.relname = 'tab_self_prune_opt';

ALTER TABLE tab_self_prune_opt SET (direct_toast_self_prune = on);

SELECT c1.reloptions AS heap_opts, c2.reloptions AS toast_opts
FROM pg_class c1
JOIN pg_class c2 ON c1.reltoastrelid = c2.oid
WHERE c1.relname = 'tab_self_prune_opt';

ALTER TABLE tab_self_prune_opt RESET (direct_toast_self_prune);
DROP TABLE tab_self_prune_opt;

--
-- Performance Improvement Test 1: Write Amplification, TOAST Index Size, and WAL Volume
-- Compare Plain (oid), Plain (oid8), and Direct TOAST on identical bulk inserts
--
CREATE TABLE tab_sup_oid(id int, val text)
  WITH (toast_flavour = 'plain', toast_value_type = 'oid', autovacuum_enabled = off);
ALTER TABLE tab_sup_oid ALTER COLUMN val SET STORAGE EXTERNAL;

CREATE TABLE tab_sup_oid8(id int, val text)
  WITH (toast_flavour = 'plain', toast_value_type = 'oid8', autovacuum_enabled = off);
ALTER TABLE tab_sup_oid8 ALTER COLUMN val SET STORAGE EXTERNAL;

CREATE TABLE tab_sup_direct(id int, val text)
  WITH (toast_flavour = 'direct', autovacuum_enabled = off);
ALTER TABLE tab_sup_direct ALTER COLUMN val SET STORAGE EXTERNAL;

DO $$
DECLARE
    j json;
    wal_oid bigint;
    wal_oid8 bigint;
    wal_direct bigint;
    rec_oid bigint;
    rec_oid8 bigint;
    rec_direct bigint;
    idx_oid_sz bigint;
    idx_oid8_sz bigint;
    idx_direct_sz bigint;
BEGIN
    -- Measure backend-local non-FPI WAL bytes and records via EXPLAIN (ANALYZE,
    -- WAL, FORMAT JSON) so results are 100% deterministic under parallel_schedule
    -- and across background checkpoints.
    EXECUTE 'EXPLAIN (ANALYZE, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) '
        'INSERT INTO tab_sup_oid SELECT g, repeat(md5(g::text), 300) FROM generate_series(1, 100) g'
        INTO j;
    rec_oid := (j->0->'Plan'->>'WAL Records')::bigint;
    wal_oid := (j->0->'Plan'->>'WAL Bytes')::bigint - (j->0->'Plan'->>'WAL FPI Bytes')::bigint;

    EXECUTE 'EXPLAIN (ANALYZE, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) '
        'INSERT INTO tab_sup_oid8 SELECT g, repeat(md5(g::text), 300) FROM generate_series(1, 100) g'
        INTO j;
    rec_oid8 := (j->0->'Plan'->>'WAL Records')::bigint;
    wal_oid8 := (j->0->'Plan'->>'WAL Bytes')::bigint - (j->0->'Plan'->>'WAL FPI Bytes')::bigint;

    EXECUTE 'EXPLAIN (ANALYZE, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) '
        'INSERT INTO tab_sup_direct SELECT g, repeat(md5(g::text), 300) FROM generate_series(1, 100) g'
        INTO j;
    rec_direct := (j->0->'Plan'->>'WAL Records')::bigint;
    wal_direct := (j->0->'Plan'->>'WAL Bytes')::bigint - (j->0->'Plan'->>'WAL FPI Bytes')::bigint;

    SELECT pg_relation_size(i.indexrelid) INTO idx_oid_sz
    FROM pg_class c JOIN pg_index i ON c.reltoastrelid = i.indrelid
    WHERE c.relname = 'tab_sup_oid';

    SELECT pg_relation_size(i.indexrelid) INTO idx_oid8_sz
    FROM pg_class c JOIN pg_index i ON c.reltoastrelid = i.indrelid
    WHERE c.relname = 'tab_sup_oid8';

    SELECT pg_relation_size(i.indexrelid) INTO idx_direct_sz
    FROM pg_class c JOIN pg_index i ON c.reltoastrelid = i.indrelid
    WHERE c.relname = 'tab_sup_direct';

    RAISE NOTICE 'index_size_checks: direct_is_1page=%, oid_grew=%, oid8_ge_oid=%',
        (idx_direct_sz = 8192),
        (idx_oid_sz > 8192),
        (idx_oid8_sz >= idx_oid_sz);

    RAISE NOTICE 'wal_checks: direct_lt_oid=%, direct_lt_oid8=%, oid8_gt_oid=%',
        (wal_direct < wal_oid AND rec_direct < rec_oid),
        (wal_direct < wal_oid8 AND rec_direct < rec_oid8),
        (wal_oid8 > wal_oid);
END$$;

DROP TABLE tab_sup_oid, tab_sup_oid8, tab_sup_direct;

--
-- Performance Improvement Test 2: Read & Slice Buffer Access Efficiency Across All 3 Tiers
-- Verify 0 TOAST index buffer accesses for Direct TOAST and minimal heap block
-- accesses when slicing flat multi-chunk (Tier 2) and tree DAG (Tier 3) values.
--
CREATE TABLE tab_tier_oid(id int PRIMARY KEY, pad text, val bytea)
  WITH (toast_flavour = 'plain', toast_value_type = 'oid', toast_tuple_target = 128, autovacuum_enabled = off);
ALTER TABLE tab_tier_oid ALTER COLUMN pad SET STORAGE PLAIN;
ALTER TABLE tab_tier_oid ALTER COLUMN val SET STORAGE EXTERNAL;

CREATE TABLE tab_tier_oid8(id int PRIMARY KEY, pad text, val bytea)
  WITH (toast_flavour = 'plain', toast_value_type = 'oid8', toast_tuple_target = 128, autovacuum_enabled = off);
ALTER TABLE tab_tier_oid8 ALTER COLUMN pad SET STORAGE PLAIN;
ALTER TABLE tab_tier_oid8 ALTER COLUMN val SET STORAGE EXTERNAL;

CREATE TABLE tab_tier_direct(id int PRIMARY KEY, pad text, val bytea)
  WITH (toast_flavour = 'direct', toast_tuple_target = 128, autovacuum_enabled = off);
ALTER TABLE tab_tier_direct ALTER COLUMN pad SET STORAGE PLAIN;
ALTER TABLE tab_tier_direct ALTER COLUMN val SET STORAGE EXTERNAL;

CREATE FUNCTION measure_toast_io(tbl regclass, query_sql text,
                                 OUT heap_io bigint, OUT idx_io bigint)
LANGUAGE plpgsql AS $$
DECLARE
    toast_rel oid;
    toast_idx oid;
    h0 bigint; i0 bigint;
    h1 bigint; i1 bigint;
    dummy text;
BEGIN
    SELECT c.reltoastrelid, i.indexrelid
      INTO toast_rel, toast_idx
      FROM pg_class c
      JOIN pg_index i ON c.reltoastrelid = i.indrelid
     WHERE c.oid = tbl;

    h0 := pg_stat_get_xact_blocks_fetched(toast_rel);
    i0 := pg_stat_get_xact_idx_blocks_fetched(toast_idx);

    EXECUTE query_sql INTO dummy;

    h1 := pg_stat_get_xact_blocks_fetched(toast_rel);
    i1 := pg_stat_get_xact_idx_blocks_fetched(toast_idx);

    heap_io := h1 - h0;
    idx_io := i1 - i0;
END$$;

-- Run INSERTs and reads within a single transaction so pd_prune_xid set on
-- insert is in-progress and heap_page_prune_opt never non-deterministically
-- pins a visibility map page depending on concurrent parallel_schedule xacts.
BEGIN;
SET LOCAL debug_parallel_query = off;

-- id=1: Tier 1 single-chunk (1600 bytes < 1996 max chunk size; 600B PLAIN pad pushes tuple > 2KB threshold)
-- id=2: Tier 2 flat multi-chunk (22400 bytes, 11 leaf chunks + 1 root chunk)
-- id=3: Tier 3 tree DAG (320000 bytes, 161 leaf chunks + internal/root chunks)
INSERT INTO tab_tier_oid VALUES
  (1, repeat('p', 600), decode(repeat(md5('tier1'), 100), 'hex')),
  (2, repeat('p', 600), decode(repeat(md5('tier2'), 1400), 'hex')),
  (3, repeat('p', 600), decode(repeat(md5('tier3'), 20000), 'hex'));

INSERT INTO tab_tier_oid8 VALUES
  (1, repeat('p', 600), decode(repeat(md5('tier1'), 100), 'hex')),
  (2, repeat('p', 600), decode(repeat(md5('tier2'), 1400), 'hex')),
  (3, repeat('p', 600), decode(repeat(md5('tier3'), 20000), 'hex'));

INSERT INTO tab_tier_direct VALUES
  (1, repeat('p', 600), decode(repeat(md5('tier1'), 100), 'hex')),
  (2, repeat('p', 600), decode(repeat(md5('tier2'), 1400), 'hex')),
  (3, repeat('p', 600), decode(repeat(md5('tier3'), 20000), 'hex'));

-- Tier 1 (1600B single-chunk) full read:
-- Direct pins 1 heap block + 0 index blocks; Plain pins 1 heap + >=1 index blocks
SELECT d.heap_io AS d_h, d.idx_io AS d_i, o.heap_io AS o_h, (o.idx_io > 0) AS o_idx, o8.heap_io AS o8_h, (o8.idx_io > 0) AS o8_idx
FROM measure_toast_io('tab_tier_direct', 'SELECT md5(val) FROM tab_tier_direct WHERE id = 1') d,
     measure_toast_io('tab_tier_oid',    'SELECT md5(val) FROM tab_tier_oid WHERE id = 1') o,
     measure_toast_io('tab_tier_oid8',   'SELECT md5(val) FROM tab_tier_oid8 WHERE id = 1') o8;

-- Tier 2 (22.4KB flat multi-chunk) 500B prefix slice:
-- Direct pins 1 directory chunk + 1 leaf chunk = 2 heap blocks + 0 index blocks
SELECT d.heap_io AS d_h, d.idx_io AS d_i, o.heap_io AS o_h, (o.idx_io > 0) AS o_idx, o8.heap_io AS o8_h, (o8.idx_io > 0) AS o8_idx
FROM measure_toast_io('tab_tier_direct', 'SELECT encode(substring(val, 1, 500), ''hex'') FROM tab_tier_direct WHERE id = 2') d,
     measure_toast_io('tab_tier_oid',    'SELECT encode(substring(val, 1, 500), ''hex'') FROM tab_tier_oid WHERE id = 2') o,
     measure_toast_io('tab_tier_oid8',   'SELECT encode(substring(val, 1, 500), ''hex'') FROM tab_tier_oid8 WHERE id = 2') o8;

-- Tier 3 (320KB tree DAG, 161 chunks) 100B middle slice at offset 150,100:
-- Direct traverses root -> internal -> 1 leaf chunk = 3 heap blocks + 0 index blocks
SELECT d.heap_io AS d_h, d.idx_io AS d_i, o.heap_io AS o_h, (o.idx_io > 0) AS o_idx, o8.heap_io AS o8_h, (o8.idx_io > 0) AS o8_idx
FROM measure_toast_io('tab_tier_direct', 'SELECT encode(substring(val, 150100, 100), ''hex'') FROM tab_tier_direct WHERE id = 3') d,
     measure_toast_io('tab_tier_oid',    'SELECT encode(substring(val, 150100, 100), ''hex'') FROM tab_tier_oid WHERE id = 3') o,
     measure_toast_io('tab_tier_oid8',   'SELECT encode(substring(val, 150100, 100), ''hex'') FROM tab_tier_oid8 WHERE id = 3') o8;

COMMIT;

DROP FUNCTION measure_toast_io(regclass, text);
DROP TABLE tab_tier_oid, tab_tier_oid8, tab_tier_direct;

--
-- Performance Improvement Test 3: On-Access LP_UNUSED Self-Pruning Under High-Churn UPDATEs
-- Without VACUUM (autovacuum_enabled = off), Direct TOAST with direct_toast_self_prune=on
-- reclaims dead TOAST chunks directly to LP_UNUSED and reuses space at steady state,
-- whereas Plain TOAST (oid/oid8) and direct_toast_self_prune=off grow linearly.
--
CREATE TEMP TABLE tab_churn_direct_on(id int PRIMARY KEY, val text)
  WITH (toast_flavour = 'direct', direct_toast_self_prune = on, autovacuum_enabled = off);
ALTER TABLE tab_churn_direct_on ALTER COLUMN val SET STORAGE EXTERNAL;

CREATE TEMP TABLE tab_churn_direct_off(id int PRIMARY KEY, val text)
  WITH (toast_flavour = 'direct', direct_toast_self_prune = off, autovacuum_enabled = off);
ALTER TABLE tab_churn_direct_off ALTER COLUMN val SET STORAGE EXTERNAL;

CREATE TEMP TABLE tab_churn_oid(id int PRIMARY KEY, val text)
  WITH (toast_flavour = 'plain', toast_value_type = 'oid', autovacuum_enabled = off);
ALTER TABLE tab_churn_oid ALTER COLUMN val SET STORAGE EXTERNAL;

CREATE TEMP TABLE tab_churn_oid8(id int PRIMARY KEY, val text)
  WITH (toast_flavour = 'plain', toast_value_type = 'oid8', autovacuum_enabled = off);
ALTER TABLE tab_churn_oid8 ALTER COLUMN val SET STORAGE EXTERNAL;

INSERT INTO tab_churn_direct_on  SELECT g, repeat(md5(g::text), 300) FROM generate_series(1, 20) g;
INSERT INTO tab_churn_direct_off SELECT g, repeat(md5(g::text), 300) FROM generate_series(1, 20) g;
INSERT INTO tab_churn_oid        SELECT g, repeat(md5(g::text), 300) FROM generate_series(1, 20) g;
INSERT INTO tab_churn_oid8       SELECT g, repeat(md5(g::text), 300) FROM generate_series(1, 20) g;

-- Run 10 full-table UPDATE churn passes in separate transactions (no VACUUM)
DO $$
DECLARE
    iter int;
    r int;
BEGIN
    FOR iter IN 1..10 LOOP
        FOR r IN 1..20 LOOP
            UPDATE tab_churn_direct_on  SET val = repeat(md5(iter::text || '-' || r::text), 300) WHERE id = r;
            UPDATE tab_churn_direct_off SET val = repeat(md5(iter::text || '-' || r::text), 300) WHERE id = r;
            UPDATE tab_churn_oid        SET val = repeat(md5(iter::text || '-' || r::text), 300) WHERE id = r;
            UPDATE tab_churn_oid8       SET val = repeat(md5(iter::text || '-' || r::text), 300) WHERE id = r;
            COMMIT;
        END LOOP;
    END LOOP;
END$$;

SELECT
    pg_total_relation_size(c_on.reltoastrelid) < pg_total_relation_size(c_off.reltoastrelid) / 4 AS self_prune_beats_off_4x,
    pg_total_relation_size(c_on.reltoastrelid) < pg_total_relation_size(c_oid.reltoastrelid) / 4 AS self_prune_beats_oid_4x,
    pg_total_relation_size(c_on.reltoastrelid) < pg_total_relation_size(c_oid8.reltoastrelid) / 4 AS self_prune_beats_oid8_4x,
    pg_relation_size(i_on.indexrelid) = 8192 AS direct_idx_still_1page,
    pg_relation_size(i_oid.indexrelid) > 32768 AS oid_idx_bloated,
    pg_relation_size(i_oid8.indexrelid) > pg_relation_size(i_oid.indexrelid) AS oid8_idx_larger_than_oid
FROM pg_class c_on
JOIN pg_index i_on ON c_on.reltoastrelid = i_on.indrelid,
     pg_class c_off,
     pg_class c_oid
JOIN pg_index i_oid ON c_oid.reltoastrelid = i_oid.indrelid,
     pg_class c_oid8
JOIN pg_index i_oid8 ON c_oid8.reltoastrelid = i_oid8.indrelid
WHERE c_on.relname = 'tab_churn_direct_on'
  AND c_off.relname = 'tab_churn_direct_off'
  AND c_oid.relname = 'tab_churn_oid'
  AND c_oid8.relname = 'tab_churn_oid8';

-- Also test multi-row single-transaction UPDATE churn with direct_toast_self_prune = on:
-- First full-table batch UPDATE establishes the 2x MVCC working set (old + new rows in-flight),
-- and subsequent full-table batch UPDATEs reuse those exact pages with 0 steady-state growth!
DO $$
DECLARE
    sz_batch1 bigint;
    sz_batch6 bigint;
    iter int;
BEGIN
    UPDATE tab_churn_direct_on SET val = repeat(md5('batch-1-' || id::text), 300);
    COMMIT;

    SELECT pg_relation_size(reltoastrelid) INTO sz_batch1
    FROM pg_class WHERE relname = 'tab_churn_direct_on';

    FOR iter IN 2..6 LOOP
        UPDATE tab_churn_direct_on SET val = repeat(md5('batch-' || iter::text || '-' || id::text), 300);
        COMMIT;
    END LOOP;

    SELECT pg_relation_size(reltoastrelid) INTO sz_batch6
    FROM pg_class WHERE relname = 'tab_churn_direct_on';

    RAISE NOTICE 'batch_churn_steady_state: zero_growth=%',
        (sz_batch6 <= sz_batch1);
END$$;

-- Verify data integrity after heavy churn
SELECT count(*), min(length(val)), max(length(val)) FROM tab_churn_direct_on;

-- Verify that Plain TOAST tables (oid and oid8) do not incur 256-block
-- clock-hand probe amplification on extension when direct_toast_self_prune = on
DO $$
DECLARE
    toast_oid oid;
    toast_oid8 oid;
    h0_oid bigint;
    h1_oid bigint;
    h0_oid8 bigint;
    h1_oid8 bigint;
BEGIN
    SELECT reltoastrelid INTO toast_oid FROM pg_class WHERE relname = 'tab_churn_oid';
    SELECT reltoastrelid INTO toast_oid8 FROM pg_class WHERE relname = 'tab_churn_oid8';

    h0_oid := pg_stat_get_xact_blocks_fetched(toast_oid);
    UPDATE tab_churn_oid SET val = repeat(md5('plain-probe-check-oid'), 300) WHERE id = 1;
    h1_oid := pg_stat_get_xact_blocks_fetched(toast_oid);

    h0_oid8 := pg_stat_get_xact_blocks_fetched(toast_oid8);
    UPDATE tab_churn_oid8 SET val = repeat(md5('plain-probe-check-oid8'), 300) WHERE id = 1;
    h1_oid8 := pg_stat_get_xact_blocks_fetched(toast_oid8);

    RAISE NOTICE 'plain_toast_no_probe_amplification: oid_bounded=%, oid8_bounded=%',
        ((h1_oid - h0_oid) <= 20),
        ((h1_oid8 - h0_oid8) <= 20);
END$$;

--
-- Performance Improvement Test 4: Single-Pass VACUUM Without Index Scans
-- Unindexed Direct TOAST dead tuples are reclaimed directly to LP_UNUSED during
-- the first heap pass of VACUUM, so even with INDEX_CLEANUP OFF (which skips
-- index scans and the second heap pass), Direct TOAST reclaims all space and
-- truncates to 0 bytes, whereas Plain TOAST (oid/oid8) can only mark chunks
-- LP_DEAD in the first pass and cannot reclaim or truncate without index scans.
--
DELETE FROM tab_churn_direct_off;
DELETE FROM tab_churn_oid;
DELETE FROM tab_churn_oid8;

VACUUM (INDEX_CLEANUP OFF) tab_churn_direct_off;
VACUUM (INDEX_CLEANUP OFF) tab_churn_oid;
VACUUM (INDEX_CLEANUP OFF) tab_churn_oid8;

SELECT
    pg_relation_size(c_off.reltoastrelid) = 0 AS direct_single_pass_reclaimed_all,
    pg_relation_size(c_oid.reltoastrelid) > 500000 AS oid_needs_index_pass,
    pg_relation_size(c_oid8.reltoastrelid) > 500000 AS oid8_needs_index_pass
FROM pg_class c_off,
     pg_class c_oid,
     pg_class c_oid8
WHERE c_off.relname = 'tab_churn_direct_off'
  AND c_oid.relname = 'tab_churn_oid'
  AND c_oid8.relname = 'tab_churn_oid8';

DROP TABLE tab_churn_direct_on, tab_churn_direct_off, tab_churn_oid, tab_churn_oid8;
