--
-- Tests for direct TOAST flavour
--

SET toast_flavour = 'direct';

-- Check GUC
SHOW toast_flavour;

CREATE TABLE dirtoasttest(descr text, f1 text);
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
    r record;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'dirtoasttest');
    IF toast_relname IS NOT NULL THEN
        FOR r IN EXECUTE 'SELECT chunk_id IS NULL as id_isnull, chunk_seq, chunk_tids IS NULL as tids_isnull FROM ' || toast_relname || ' ORDER BY id_isnull desc, chunk_id, chunk_seq' LOOP
            RAISE NOTICE 'chunk: id_isnull=%, seq=%, tids_isnull=%', r.id_isnull, r.chunk_seq, r.tids_isnull;
        END LOOP;
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
SET toast_flavour = 'plain';
SHOW toast_flavour;

SELECT descr, length(f1), substring(f1, 1, 10), substring(f1, length(f1)-9, 10) FROM dirtoasttest;

-- Test slice reading
SELECT descr, substring(f1, 500, 20) FROM dirtoasttest WHERE descr = 'toasted-multi';
SELECT descr, substring(f1, 45000, 20) FROM dirtoasttest WHERE descr = 'toasted-multi';

-- Test update (should write as 'plain' now because GUC is 'plain')
-- We use a smaller value to avoid too many chunks, but still toasted.
-- Actually, updated value will also be toasted if it's large.
-- 'toasted-1' is 10KB. f1 || 'edited' is 10006 bytes. It will be toasted.
UPDATE dirtoasttest SET f1 = f1 || 'edited' WHERE descr = 'toasted-1';
SELECT descr, length(f1), substring(f1, length(f1)-9, 10) FROM dirtoasttest WHERE descr = 'toasted-1';

-- Toast table should now contain some 'plain' toast (no tid array) and some 'direct' toast.
-- The updated 'toasted-1' should be plain.
-- Let's check toast table again.
DO $$
DECLARE
    toast_relname text;
    r record;
BEGIN
    SELECT 'pg_toast.' || relname INTO toast_relname FROM pg_class WHERE oid = (SELECT reltoastrelid FROM pg_class WHERE relname = 'dirtoasttest');
    IF toast_relname IS NOT NULL THEN
        FOR r IN EXECUTE 'SELECT chunk_id IS NULL as id_isnull, chunk_seq, chunk_tids IS NULL as tids_isnull FROM ' || toast_relname || ' ORDER BY id_isnull desc, chunk_id, chunk_seq' LOOP
            RAISE NOTICE 'chunk: id_isnull=%, seq=%, tids_isnull=%', r.id_isnull, r.chunk_seq, r.tids_isnull;
        END LOOP;
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
SET toast_flavour = 'direct';
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

-- Test Table Storage Parameter 'toast_flavour'
SET toast_flavour = 'plain'; -- GUC is plain

-- 1. Table option 'direct'
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

-- 2. Table option 'plain', GUC is 'direct'
SET toast_flavour = 'direct';
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

-- 3. Default (no option), follows GUC
CREATE TABLE tab_default(descr text, f1 text);
ALTER TABLE tab_default ALTER COLUMN f1 SET STORAGE EXTERNAL;

-- GUC is direct -> writes direct (chunk_id = NULL)
INSERT INTO tab_default VALUES ('default-direct', repeat('g', 3000));

-- GUC is plain -> writes plain (chunk_id <> NULL)
SET toast_flavour = 'plain';
INSERT INTO tab_default VALUES ('default-plain', repeat('h', 3000));

-- Verify contents
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
-- GUC is plain -> should write plain (chunk_id <> NULL)
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

-- CLUSTER on direct TOAST table directly is rejected
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
        RAISE NOTICE 'expected error caught for CLUSTER on direct TOAST table: %', SQLERRM;
    END;
END$$;

DROP TABLE tab_toast_maint;

--
-- Test pg_ensure_direct_toast and legacy TOAST table in-place upgrade
--
CREATE TABLE tab_legacy_test(id int, val text);
ALTER TABLE tab_legacy_test ALTER COLUMN val SET STORAGE EXTERNAL;
INSERT INTO tab_legacy_test VALUES (1, repeat('legacy-plain-payload-', 300));

-- Simulate a legacy 3-column TOAST table by removing trailing attributes and index predicate
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
SET toast_flavour = 'direct';
INSERT INTO tab_legacy_test VALUES (2, repeat('direct-write-attempt-', 300));
RESET toast_flavour;

-- Read legacy plain data still works
SELECT id, length(val), substring(val, 1, 20) FROM tab_legacy_test WHERE id = 1;

-- Upgrade using pg_ensure_direct_toast
SELECT pg_ensure_direct_toast('tab_legacy_test'::regclass);

-- Direct write now succeeds!
SET toast_flavour = 'direct';
INSERT INTO tab_legacy_test VALUES (2, repeat('direct-write-success-', 300));
RESET toast_flavour;

-- Read both plain and direct rows
SELECT id, length(val), substring(val, 1, 20) FROM tab_legacy_test ORDER BY id;

-- Test ALTER TABLE SET (toast_flavour = 'direct') on a simulated legacy table
CREATE TABLE tab_legacy_alter(id int, val text);
ALTER TABLE tab_legacy_alter ALTER COLUMN val SET STORAGE EXTERNAL;
INSERT INTO tab_legacy_alter VALUES (1, repeat('legacy-alter-payload-', 300));

DO $$
DECLARE
    toast_relid oid;
    toast_idxid oid;
BEGIN
    SELECT c1.reltoastrelid INTO toast_relid
    FROM pg_class c1
    WHERE c1.relname = 'tab_legacy_alter';

    SELECT indexrelid INTO toast_idxid
    FROM pg_index
    WHERE indrelid = toast_relid;

    DELETE FROM pg_attribute WHERE attrelid = toast_relid AND attnum IN (4, 5);
    UPDATE pg_class SET relnatts = 3 WHERE oid = toast_relid;
    UPDATE pg_index SET indpred = NULL WHERE indexrelid = toast_idxid;
END$$;

\c -

-- Alter table SET toast_flavour = 'direct' automatically calls ensure_direct_toast
ALTER TABLE tab_legacy_alter SET (toast_flavour = 'direct');

-- Direct write now succeeds
INSERT INTO tab_legacy_alter VALUES (2, repeat('alter-direct-success-', 300));

-- Read both rows
SELECT id, length(val), substring(val, 1, 20) FROM tab_legacy_alter ORDER BY id;

DROP TABLE tab_legacy_test;
DROP TABLE tab_legacy_alter;
