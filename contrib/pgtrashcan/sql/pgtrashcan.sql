-- pgtrashcan regression test suite
-- Tests: DROP TABLE interception, restore, purge, CASCADE, metadata, protections
--
-- NOTE: This test relies on shared_preload_libraries = 'pgtrashcan'
-- being set in postgresql.conf. The pg_regress framework creates a
-- temporary database for testing.
--
-- TEST SECTIONS:
--   Section 1: Core - Basic DROP and Restore
--   Section 2: Indexes and Sequences
--   Section 3: CASCADE Behavior
--   Section 4: Metadata Capture
--   Section 5: Restore Advanced (versions, schemas, conflicts)
--   Section 6: Purge Operations
--   Section 7: DROP TABLE Variants (IF EXISTS, multi-table, schemas)
--   Section 8: Protection Mechanisms
--   Section 9: Listing Functions
--   Section 10: Final State

-- Setup: create the extension (installs schema + catalog + functions)
CREATE EXTENSION IF NOT EXISTS pgtrashcan;

-- Suppress NOTICE messages from restore/purge functions (they contain
-- dynamic content like OIDs and timestamps that would make output non-deterministic)
SET client_min_messages = warning;

-- ########################################
-- SECTION 1: Core - Basic DROP and Restore
-- ########################################

-- ========================================
-- TEST 1: Basic DROP TABLE moves to pgtrashcan
-- ========================================
CREATE TABLE test_basic (id int, val text);
INSERT INTO test_basic VALUES (1, 'hello');

DROP TABLE test_basic;

-- Verify table is in trash catalog
SELECT orig_name, orig_schema, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_basic' ORDER BY orig_name;

-- ========================================
-- TEST 2: Restore by name + data survives
-- ========================================
SELECT pgtrashcan_restore('test_basic') IS NOT NULL AS restored;

-- Verify data survived
SELECT * FROM test_basic;

-- Verify catalog entry removed
SELECT count(*) AS remaining FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_basic';

-- ========================================
-- TEST 3: TOAST data survives drop/restore
-- ========================================
DROP TABLE test_basic;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

CREATE TABLE toast_test (id int PRIMARY KEY, large_text text);
INSERT INTO toast_test VALUES (1, repeat('x', 10000));

DROP TABLE toast_test;

SELECT pgtrashcan_restore('toast_test') IS NOT NULL AS restored;

-- Verify large data survived via TOAST
SELECT id, length(large_text) AS text_length FROM toast_test;

DROP TABLE toast_test;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 4: DROP from pgtrashcan is permanent
-- ========================================
CREATE TABLE test_perm (id int);
DROP TABLE test_perm;

-- Verify in trash first
SELECT count(*) AS in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_perm';

-- Get trash_name and drop it permanently
DO $$
DECLARE v_tname text;
BEGIN
    SELECT trash_name INTO v_tname FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'test_perm' LIMIT 1;
    EXECUTE format('DROP TABLE pgtrashcan.%I', v_tname);
END $$;

-- Verify gone from catalog
SELECT count(*) AS remaining FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_perm';

-- ########################################
-- SECTION 2: Indexes and Sequences
-- ########################################

-- ========================================
-- TEST 5: Indexes are preserved on drop/restore
-- ========================================
CREATE TABLE test_idx (id int PRIMARY KEY, email text UNIQUE, name text);
CREATE INDEX idx_test_name ON test_idx(name);
INSERT INTO test_idx VALUES (1, 'a@b.com', 'Alice');

DROP TABLE test_idx;

-- Check indexes tracked in catalog
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('test_idx', 'test_idx_pkey', 'test_idx_email_key', 'idx_test_name')
ORDER BY orig_obj_type, orig_name;

-- Restore
SELECT pgtrashcan_restore('test_idx') IS NOT NULL AS restored;

-- Verify indexes restored
SELECT indexname FROM pg_indexes
WHERE tablename = 'test_idx' AND schemaname = 'public'
ORDER BY indexname;

-- Verify data
SELECT * FROM test_idx;

-- Cleanup
DROP TABLE test_idx;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 6: Index name conflict detection + resolution via rename
-- ========================================
CREATE TABLE idx_tbl1 (id int, value text);
CREATE INDEX shared_idx ON idx_tbl1(value);
DROP TABLE idx_tbl1;

-- Create another table reusing the same index name
CREATE TABLE idx_tbl2 (id int, value text);
CREATE INDEX shared_idx ON idx_tbl2(value);

-- Restore should fail due to index name conflict
DO $$
BEGIN
    PERFORM pgtrashcan_restore('idx_tbl1');
    RAISE WARNING 'UNEXPECTED: restore should have failed but succeeded';
EXCEPTION
    WHEN OTHERS THEN
        RAISE WARNING 'OK: index conflict correctly detected';
END $$;

-- Rename the conflicting index in trash to resolve
DO $$
DECLARE v_idx_tname text;
BEGIN
    SELECT trash_name INTO v_idx_tname FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'shared_idx' LIMIT 1;
    PERFORM pgtrashcan_rename_in_trash(v_idx_tname, 'shared_idx_old');
END $$;

-- Now restore should succeed
SELECT pgtrashcan_restore('idx_tbl1') IS NOT NULL AS restored;

-- Verify both tables have their indexes
SELECT tablename, indexname FROM pg_indexes
WHERE schemaname = 'public' AND tablename IN ('idx_tbl1', 'idx_tbl2')
ORDER BY tablename, indexname;

DROP TABLE idx_tbl1, idx_tbl2;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 7: SERIAL sequence moves with table
-- ========================================
CREATE TABLE serial_test (id SERIAL PRIMARY KEY, val text);
INSERT INTO serial_test (val) VALUES ('row1');

DROP TABLE serial_test;

-- Sequence should be tracked
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('serial_test', 'serial_test_id_seq')
ORDER BY orig_obj_type, orig_name;

-- Restore and verify sequence works
SELECT pgtrashcan_restore('serial_test') IS NOT NULL AS restored;
INSERT INTO serial_test (val) VALUES ('row2');
SELECT id, val FROM serial_test ORDER BY id;

DROP TABLE serial_test;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ########################################
-- SECTION 3: CASCADE Behavior
-- ########################################

-- ========================================
-- TEST 8: RESTRICT fails with FK dependents
-- TEST 9: CASCADE drops FK constraint, child table survives
-- (paired: test 8 leaves test_parent + test_child for test 9)
-- ========================================
CREATE TABLE test_parent (id int PRIMARY KEY);
CREATE TABLE test_child (id int, pid int REFERENCES test_parent(id));

-- Should fail: child depends on parent
DROP TABLE test_parent;

-- Verify parent not in trash (drop failed)
SELECT count(*) AS parent_in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_parent';

-- Now CASCADE: only parent goes to trash, child survives with FK dropped
DROP TABLE test_parent CASCADE;

-- Only parent should be in trash (child table stays, only FK constraint dropped)
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('test_parent', 'test_child') AND orig_obj_type = 'table'
ORDER BY orig_name;

-- Child table should still exist in public schema
SELECT tablename FROM pg_tables WHERE schemaname = 'public' AND tablename = 'test_child';

-- FK constraint should be gone from child
SELECT count(*) AS fk_count FROM pg_constraint
WHERE conrelid = 'test_child'::regclass AND contype = 'f';

-- FK metadata should be captured on parent
SELECT orig_name, metadata ? 'foreign_keys_dropped' AS has_fk_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_parent';

DROP TABLE test_child;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 10: CASCADE with multiple FK children - children survive
-- ========================================
CREATE TABLE cascade_parent (id int PRIMARY KEY, name text);
CREATE TABLE cascade_child1 (id int PRIMARY KEY, parent_id int REFERENCES cascade_parent(id));
CREATE TABLE cascade_child2 (id int PRIMARY KEY, parent_id int REFERENCES cascade_parent(id));

INSERT INTO cascade_parent VALUES (1, 'Parent');
INSERT INTO cascade_child1 VALUES (1, 1);
INSERT INTO cascade_child2 VALUES (1, 1);

DROP TABLE cascade_parent CASCADE;

-- Only parent should be in trash
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('cascade_parent', 'cascade_child1', 'cascade_child2') AND orig_obj_type = 'table'
ORDER BY orig_name;

-- Both children should still exist with their data intact
SELECT * FROM cascade_child1;
SELECT * FROM cascade_child2;

-- FK constraints should be gone from both children
SELECT count(*) AS fk_count FROM pg_constraint
WHERE conrelid IN ('cascade_child1'::regclass, 'cascade_child2'::regclass) AND contype = 'f';

DROP TABLE cascade_child1, cascade_child2;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 11: RESTRICT fails with view dependents
-- TEST 12: CASCADE - view permanently dropped, metadata captured
-- (paired: test 11 leaves view_base + view_dep for test 12)
-- ========================================
CREATE TABLE view_base (id int PRIMARY KEY, val text);
CREATE VIEW view_dep AS SELECT * FROM view_base WHERE id > 0;

-- RESTRICT should fail (view depends on table)
DROP TABLE view_base;

-- CASCADE drops view, moves table
DROP TABLE view_base CASCADE;

SELECT orig_name, metadata ? 'views_dropped' AS has_view_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'view_base';

-- New: ddl field is a runnable CREATE statement
SELECT (metadata->'views_dropped'->0->>'ddl' LIKE 'CREATE OR REPLACE VIEW %AS%') AS view_ddl_looks_runnable
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'view_base';

-- View should be gone
SELECT count(*) AS views_remaining FROM pg_views
WHERE viewname = 'view_dep' AND schemaname = 'public';

SELECT pgtrashcan_restore('view_base') IS NOT NULL AS restored;

-- View NOT auto-restored
SELECT count(*) AS views_after_restore FROM pg_views
WHERE viewname = 'view_dep' AND schemaname = 'public';

DROP TABLE view_base;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 13: CASCADE - materialized views permanently dropped
-- ========================================
CREATE TABLE matview_base (id int PRIMARY KEY, name text, price numeric);
CREATE VIEW matview_regular AS SELECT * FROM matview_base WHERE price > 0;
CREATE MATERIALIZED VIEW matview_expensive AS SELECT * FROM matview_base WHERE price > 100;

INSERT INTO matview_base VALUES (1, 'Item 1', 50), (2, 'Item 2', 150);

DROP TABLE matview_base CASCADE;

-- Metadata should capture both regular and materialized views
SELECT orig_name, metadata ? 'views_dropped' AS has_view_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'matview_base';

-- New: every view's ddl field is a runnable CREATE statement
SELECT bool_and(
    (v->>'ddl' LIKE 'CREATE MATERIALIZED VIEW %') OR
    (v->>'ddl' LIKE 'CREATE OR REPLACE VIEW %')
) AS all_view_ddls_runnable
FROM pgtrashcan._trash_catalog tc,
     jsonb_array_elements(tc.metadata->'views_dropped') AS v
WHERE tc.orig_name = 'matview_base';

-- Both views gone
SELECT count(*) AS regular_views FROM pg_views
WHERE viewname = 'matview_regular' AND schemaname = 'public';

SELECT count(*) AS mat_views FROM pg_matviews
WHERE matviewname = 'matview_expensive' AND schemaname = 'public';

SELECT pgtrashcan_restore('matview_base') IS NOT NULL AS restored;

-- Verify data survived
SELECT * FROM matview_base ORDER BY id;

DROP TABLE matview_base;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 14: CASCADE - nested 3-level view chain
-- ========================================
CREATE TABLE chain_base (id int PRIMARY KEY, val text);
CREATE VIEW chain_v1 AS SELECT id, val FROM chain_base;
CREATE VIEW chain_v2 AS SELECT id FROM chain_v1;
CREATE VIEW chain_v3 AS SELECT id FROM chain_v2;

DROP TABLE chain_base CASCADE;

-- Table should be in trash
SELECT count(*) AS chain_base_in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'chain_base';

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 15: CASCADE - multi-table view (drop one table, other survives)
-- ========================================
CREATE TABLE multi_base_a (id int PRIMARY KEY, a_val text);
CREATE TABLE multi_base_b (id int PRIMARY KEY, b_val text);

INSERT INTO multi_base_a VALUES (1, 'a_test');
INSERT INTO multi_base_b VALUES (1, 'b_test');

CREATE VIEW multi_table_view AS
    SELECT a.id, a.a_val, b.b_val
    FROM multi_base_a a JOIN multi_base_b b ON a.id = b.id;

-- Drop ONLY table_a with CASCADE
DROP TABLE multi_base_a CASCADE;

-- table_b should still exist with data intact
SELECT count(*) AS b_exists FROM pg_tables
WHERE tablename = 'multi_base_b' AND schemaname = 'public';

SELECT * FROM multi_base_b;

-- View should be gone
SELECT count(*) AS view_remaining FROM pg_views
WHERE viewname = 'multi_table_view' AND schemaname = 'public';

-- Metadata captured on the dropped table
SELECT orig_name, metadata ? 'views_dropped' AS has_view_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'multi_base_a';

-- Restore table_a
SELECT pgtrashcan_restore('multi_base_a') IS NOT NULL AS restored;
SELECT * FROM multi_base_a;

DROP TABLE multi_base_a, multi_base_b;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 16: CASCADE - mixed objects (SERIAL + FK child + view)
-- ========================================
CREATE TABLE mixed_parent (id SERIAL PRIMARY KEY, name text);
CREATE TABLE mixed_child (id int PRIMARY KEY, parent_id int REFERENCES mixed_parent(id));
CREATE VIEW mixed_view AS SELECT * FROM mixed_parent WHERE id > 0;

INSERT INTO mixed_parent (name) VALUES ('Test');
INSERT INTO mixed_child VALUES (1, 1);

DROP TABLE mixed_parent CASCADE;

-- Only parent table and its sequence should be in trash; child survives, view permanently dropped
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name LIKE 'mixed_%'
ORDER BY orig_obj_type, orig_name;

-- Child table should still exist with data intact
SELECT * FROM mixed_child;

-- FK constraint should be gone from child
SELECT count(*) AS fk_count FROM pg_constraint
WHERE conrelid = 'mixed_child'::regclass AND contype = 'f';

-- View metadata should be captured on parent
SELECT orig_name, metadata ? 'views_dropped' AS has_view_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'mixed_parent';

-- Restore parent and verify SERIAL sequence ownership is restored
SELECT pgtrashcan_restore('mixed_parent') IS NOT NULL AS restored;

SELECT pg_get_serial_sequence('mixed_parent', 'id') IS NOT NULL AS seq_ownership_restored;

-- Sequence should work after restore
INSERT INTO mixed_parent (name) VALUES ('After Restore');
SELECT id, name FROM mixed_parent ORDER BY id;

DROP TABLE mixed_child;
DROP TABLE mixed_parent;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 17: Multi-table DROP with FK between them (RESTRICT mode)
-- Both in same DROP statement - should succeed without CASCADE
-- ========================================
CREATE TABLE test_multi_fk_parent (id int PRIMARY KEY);
CREATE TABLE test_multi_fk_child (id int, pid int REFERENCES test_multi_fk_parent(id));
INSERT INTO test_multi_fk_parent VALUES (1), (2);
INSERT INTO test_multi_fk_child VALUES (1, 1), (2, 2);

-- This should NOT error even without CASCADE, because both tables are in the same DROP
DROP TABLE test_multi_fk_parent, test_multi_fk_child;

-- Both should be in trash
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('test_multi_fk_parent', 'test_multi_fk_child')
  AND orig_obj_type = 'table'
ORDER BY orig_name;

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 18: Multi-table DROP CASCADE with FK between them + external table
-- ========================================
CREATE TABLE test_cascade_multi_p (id int PRIMARY KEY, name text);
CREATE TABLE test_cascade_multi_c (id int PRIMARY KEY, pid int REFERENCES test_cascade_multi_p(id));
CREATE TABLE test_cascade_multi_ext (id int, pid int REFERENCES test_cascade_multi_p(id));
INSERT INTO test_cascade_multi_p VALUES (1, 'parent');
INSERT INTO test_cascade_multi_c VALUES (1, 1);
INSERT INTO test_cascade_multi_ext VALUES (1, 1);

-- CASCADE: parent and child in same DROP, ext_table has FK to parent but not in DROP
DROP TABLE test_cascade_multi_p, test_cascade_multi_c CASCADE;

-- Both parent and child should be in trash
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('test_cascade_multi_p', 'test_cascade_multi_c', 'test_cascade_multi_ext')
  AND orig_obj_type = 'table'
ORDER BY orig_name;

-- External table should survive with FK dropped
SELECT * FROM test_cascade_multi_ext;

-- FK on external table should be gone
SELECT count(*) AS fk_count FROM pg_constraint
WHERE conrelid = 'test_cascade_multi_ext'::regclass AND contype = 'f';

DROP TABLE test_cascade_multi_ext;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ########################################
-- SECTION 4: Metadata Capture
-- ########################################

-- ========================================
-- TEST 19: FK metadata captured (outgoing FK on dropped table)
-- ========================================
CREATE TABLE fk_ref (id int PRIMARY KEY);
CREATE TABLE fk_test (id int PRIMARY KEY, ref_id int,
    CONSTRAINT fk_to_ref FOREIGN KEY (ref_id) REFERENCES fk_ref(id));
INSERT INTO fk_ref VALUES (1);
INSERT INTO fk_test VALUES (1, 1);

-- Drop child (has outgoing FK)
DROP TABLE fk_test;

-- FK metadata stored
SELECT orig_name, metadata ? 'foreign_keys_dropped' AS has_fk_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'fk_test';

-- New FK metadata fields: columns list, ddl, human-readable on_delete
SELECT
    (metadata->'foreign_keys_dropped'->0->>'ddl' LIKE 'ALTER TABLE %ADD CONSTRAINT %FOREIGN KEY%') AS ddl_looks_runnable,
    jsonb_array_length(metadata->'foreign_keys_dropped'->0->'columns') AS col_count,
    metadata->'foreign_keys_dropped'->0->>'on_delete' AS on_delete_action
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'fk_test';

-- Restore and verify FK is gone
SELECT pgtrashcan_restore('fk_test') IS NOT NULL AS restored;
SELECT conname FROM pg_constraint
WHERE conrelid = 'fk_test'::regclass AND contype = 'f';

DROP TABLE fk_test, fk_ref;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 20: Trigger metadata captured
-- ========================================
CREATE TABLE trig_test (id int PRIMARY KEY, val text, updated_at timestamptz);

CREATE OR REPLACE FUNCTION test_update_ts() RETURNS TRIGGER AS $$
BEGIN NEW.updated_at = now(); RETURN NEW; END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_update BEFORE UPDATE ON trig_test
    FOR EACH ROW EXECUTE FUNCTION test_update_ts();

DROP TABLE trig_test;

SELECT orig_name, metadata ? 'triggers_dropped' AS has_trigger_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'trig_test';

SELECT pgtrashcan_restore('trig_test') IS NOT NULL AS restored;

-- Trigger should be gone
SELECT tgname FROM pg_trigger
WHERE tgrelid = 'trig_test'::regclass AND NOT tgisinternal;

DROP TABLE trig_test;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 21: Rule metadata captured
-- ========================================
CREATE TABLE rule_test (id int PRIMARY KEY, val text);
CREATE RULE notify_rule AS ON INSERT TO rule_test DO ALSO NOTIFY rule_channel;

DROP TABLE rule_test;

SELECT orig_name, metadata ? 'rules_dropped' AS has_rule_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'rule_test';

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 22: RLS policy metadata captured
-- ========================================
CREATE TABLE policy_test (id int PRIMARY KEY, user_id int);
ALTER TABLE policy_test ENABLE ROW LEVEL SECURITY;
CREATE POLICY user_pol ON policy_test FOR SELECT USING (user_id = 1);

DROP TABLE policy_test;

SELECT orig_name, metadata ? 'policies_dropped' AS has_policy_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'policy_test';

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 23: Complete scenario - all 5 metadata types on one table
-- ========================================
CREATE TABLE ref_for_complete (id int PRIMARY KEY);
INSERT INTO ref_for_complete VALUES (1);

CREATE TABLE complete_test (id SERIAL PRIMARY KEY, ref_id int, val text, updated_at timestamptz);

ALTER TABLE complete_test ADD CONSTRAINT fk_complete
    FOREIGN KEY (ref_id) REFERENCES ref_for_complete(id);

CREATE OR REPLACE FUNCTION complete_update_ts() RETURNS TRIGGER AS $$
BEGIN NEW.updated_at = now(); RETURN NEW; END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trg_complete BEFORE UPDATE ON complete_test
    FOR EACH ROW EXECUTE FUNCTION complete_update_ts();

CREATE VIEW complete_view AS SELECT * FROM complete_test WHERE val IS NOT NULL;

CREATE RULE complete_rule AS ON DELETE TO complete_test
    WHERE val = 'protected' DO INSTEAD NOTHING;

ALTER TABLE complete_test ENABLE ROW LEVEL SECURITY;
CREATE POLICY complete_pol ON complete_test FOR SELECT USING (ref_id = 1);

INSERT INTO complete_test (ref_id, val) VALUES (1, 'test');

DROP TABLE complete_test CASCADE;

-- All 5 metadata keys should exist
SELECT orig_name,
    metadata ? 'foreign_keys_dropped' AS has_fk,
    metadata ? 'triggers_dropped' AS has_triggers,
    metadata ? 'views_dropped' AS has_views,
    metadata ? 'rules_dropped' AS has_rules,
    metadata ? 'policies_dropped' AS has_policies
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'complete_test';

-- ========================================
-- TEST 24: Post-restore verification - all 5 types absent from system catalogs
-- ========================================
SELECT pgtrashcan_restore('complete_test') IS NOT NULL AS restored;

-- FK absent
SELECT count(*) AS fk_count FROM pg_constraint
WHERE conrelid = 'complete_test'::regclass AND contype = 'f';

-- Trigger absent
SELECT count(*) AS trigger_count FROM pg_trigger
WHERE tgrelid = 'complete_test'::regclass AND NOT tgisinternal;

-- View absent
SELECT count(*) AS view_count FROM pg_views
WHERE viewname = 'complete_view' AND schemaname = 'public';

-- Rule absent (excluding _RETURN)
SELECT count(*) AS rule_count FROM pg_rewrite r
JOIN pg_class c ON r.ev_class = c.oid
WHERE c.relname = 'complete_test'
AND c.relnamespace = 'public'::regnamespace
AND r.rulename != '_RETURN';

-- Policy absent
SELECT count(*) AS policy_count FROM pg_policy
WHERE polrelid = 'complete_test'::regclass;

DROP TABLE complete_test, ref_for_complete;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ########################################
-- SECTION 5: Restore Advanced
-- ########################################

-- ========================================
-- TEST 25: Multiple drops of same table name (versioning)
-- ========================================
CREATE TABLE test_basic (id int, val text);
INSERT INTO test_basic VALUES (1, 'hello');
DROP TABLE test_basic;

CREATE TABLE test_basic (id int, val text, extra text);
INSERT INTO test_basic VALUES (2, 'world', 'v2');
DROP TABLE test_basic;

-- Both versions in trash
SELECT count(*) AS versions FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_basic' AND orig_obj_type = 'table';

-- Restore most recent (has extra column)
SELECT pgtrashcan_restore('test_basic') IS NOT NULL AS restored;
SELECT * FROM test_basic;

DROP TABLE test_basic;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 26: Conflict detection on restore
-- ========================================
CREATE TABLE conflict_t (id int, val text);
INSERT INTO conflict_t VALUES (1, 'original');
DROP TABLE conflict_t;

-- Create new table with same name
CREATE TABLE conflict_t (id int, val text);

-- Restore should fail - name conflict (use DO block to get deterministic output)
DO $$
BEGIN
    PERFORM pgtrashcan_restore('conflict_t');
    RAISE WARNING 'UNEXPECTED: restore should have failed but succeeded';
EXCEPTION
    WHEN OTHERS THEN
        RAISE WARNING 'OK: conflict correctly detected';
END $$;

-- Clean up
DROP TABLE conflict_t;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 27: Rename in trash for conflict resolution
-- ========================================
CREATE TABLE rename_test (id int, val text);
INSERT INTO rename_test VALUES (1, 'original');
DROP TABLE rename_test;

CREATE TABLE rename_test (id int, val text);
INSERT INTO rename_test VALUES (2, 'new');

-- Rename in trash
DO $$
DECLARE v_tname text;
BEGIN
    SELECT trash_name INTO v_tname FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'rename_test' LIMIT 1;
    PERFORM pgtrashcan_rename_in_trash(v_tname, 'rename_test_old');
END $$;

-- Now restore should work
SELECT pgtrashcan_restore('rename_test_old') IS NOT NULL AS restored;

SELECT * FROM rename_test ORDER BY id;
SELECT * FROM rename_test_old ORDER BY id;

DROP TABLE rename_test, rename_test_old;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 28: Restore with explicit schema hint
-- ========================================
CREATE TABLE schema_restore_test (id int, val text);
INSERT INTO schema_restore_test VALUES (1, 'data');
DROP TABLE schema_restore_test;

-- Restore using 2-arg form with explicit schema
SELECT pgtrashcan_restore('schema_restore_test', 'public') IS NOT NULL AS restored;

-- Verify data
SELECT * FROM schema_restore_test;

DROP TABLE schema_restore_test;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 29: Restore with OID hint (restoring older version)
-- ========================================
CREATE TABLE versioned_test (id int);
INSERT INTO versioned_test VALUES (1);
DROP TABLE versioned_test;  -- version 1

CREATE TABLE versioned_test (id int, extra text);
INSERT INTO versioned_test VALUES (2, 'v2');
DROP TABLE versioned_test;  -- version 2

-- Both versions in trash
SELECT count(*) AS versions FROM pgtrashcan._trash_catalog
WHERE orig_name = 'versioned_test' AND orig_obj_type = 'table';

-- Rename older version so both can be restored without conflict
DO $$
DECLARE v_old_oid oid;
BEGIN
    SELECT orig_oid INTO v_old_oid
    FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'versioned_test' AND orig_obj_type = 'table'
    ORDER BY drop_time ASC
    LIMIT 1;

    PERFORM pgtrashcan_rename_in_trash(
        (SELECT trash_name FROM pgtrashcan._trash_catalog WHERE orig_oid = v_old_oid),
        'versioned_test_old'
    );
END $$;

-- Restore most recent (v2 with extra column)
SELECT pgtrashcan_restore('versioned_test') IS NOT NULL AS restored_v2;
SELECT * FROM versioned_test;

-- Restore old version (v1, single column)
SELECT pgtrashcan_restore('versioned_test_old') IS NOT NULL AS restored_v1;
SELECT * FROM versioned_test_old;

DROP TABLE versioned_test, versioned_test_old;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 30: Restore from non-public schema
-- ========================================
CREATE SCHEMA myapp;
CREATE TABLE myapp.myapp_table (id int PRIMARY KEY, val text);
INSERT INTO myapp.myapp_table VALUES (1, 'from myapp');

DROP TABLE myapp.myapp_table;

-- Should be in trash with orig_schema = myapp
SELECT orig_name, orig_schema, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name = 'myapp_table';

-- Restore back to original schema using schema hint
SELECT pgtrashcan_restore('myapp_table', 'myapp') IS NOT NULL AS restored;

-- Verify back in myapp schema with data
SELECT * FROM myapp.myapp_table;

DROP TABLE myapp.myapp_table;
DROP SCHEMA myapp;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 31: Multiple schemas - same table name disambiguation
-- ========================================
CREATE SCHEMA schema_a;
CREATE SCHEMA schema_b;
CREATE TABLE schema_a.shared_name (id int);
CREATE TABLE schema_b.shared_name (id int);
INSERT INTO schema_a.shared_name VALUES (1);
INSERT INTO schema_b.shared_name VALUES (2);

DROP TABLE schema_a.shared_name;
DROP TABLE schema_b.shared_name;

-- Both in trash
SELECT orig_name, orig_schema FROM pgtrashcan._trash_catalog
WHERE orig_name = 'shared_name' AND orig_obj_type = 'table'
ORDER BY orig_schema;

-- Restore from specific schema
SELECT pgtrashcan_restore('shared_name', 'schema_a') IS NOT NULL AS restored_a;
SELECT * FROM schema_a.shared_name;

SELECT pgtrashcan_restore('shared_name', 'schema_b') IS NOT NULL AS restored_b;
SELECT * FROM schema_b.shared_name;

DROP TABLE schema_a.shared_name, schema_b.shared_name;
DROP SCHEMA schema_a;
DROP SCHEMA schema_b;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ########################################
-- SECTION 6: Purge Operations
-- ########################################

-- ========================================
-- TEST 32: Purge by name
-- ========================================
CREATE TABLE purge_test (id int);
DROP TABLE purge_test;

SELECT count(*) AS in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'purge_test';

SELECT pgtrashcan_purge('purge_test') IS NOT NULL AS purged;

SELECT count(*) AS after_purge FROM pgtrashcan._trash_catalog
WHERE orig_name = 'purge_test';

-- ========================================
-- TEST 33: Purge by OID
-- ========================================
CREATE TABLE purge_oid_test (id int, val text);
INSERT INTO purge_oid_test VALUES (1, 'test');
DROP TABLE purge_oid_test;

-- Get OID and purge by it
DO $$
DECLARE v_oid oid;
BEGIN
    SELECT orig_oid INTO v_oid FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'purge_oid_test' AND orig_obj_type = 'table';
    PERFORM pgtrashcan_purge_by_oid(v_oid);
END $$;

-- Should be gone
SELECT count(*) AS remaining FROM pgtrashcan._trash_catalog
WHERE orig_name = 'purge_oid_test';

-- ========================================
-- TEST 34: Purge with schema hint
-- ========================================
CREATE SCHEMA purge_ns;
CREATE TABLE purge_ns.purge_schema_test (id int);
DROP TABLE purge_ns.purge_schema_test;

-- Purge using 2-arg form with explicit schema
SELECT pgtrashcan_purge('purge_schema_test', 'purge_ns') IS NOT NULL AS purged;

SELECT count(*) AS remaining FROM pgtrashcan._trash_catalog
WHERE orig_name = 'purge_schema_test';

DROP SCHEMA purge_ns;

-- ========================================
-- TEST 35: Purge all
-- ========================================
CREATE TABLE purge_all_t1 (id int);
CREATE TABLE purge_all_t2 (id int);
DROP TABLE purge_all_t1;
DROP TABLE purge_all_t2;

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- Verify empty
SELECT count(*) AS remaining FROM pgtrashcan._trash_catalog;

-- ########################################
-- SECTION 7: DROP TABLE Variants
-- ########################################

-- ========================================
-- TEST 36: DROP TABLE in non-public schema
-- ========================================
CREATE SCHEMA testns;
CREATE TABLE testns.ns_table (id int, val text);
CREATE INDEX ns_idx ON testns.ns_table(val);
INSERT INTO testns.ns_table VALUES (1, 'ns_data');

DROP TABLE testns.ns_table;

-- Should be in trash with orig_schema = testns
SELECT orig_name, orig_schema, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('ns_table', 'ns_idx')
ORDER BY orig_obj_type, orig_name;

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;
DROP SCHEMA testns;

-- ========================================
-- TEST 37: DROP TABLE IF EXISTS - single nonexistent table
-- ========================================
-- Should emit a NOTICE and do nothing (no crash, no error)
DROP TABLE IF EXISTS this_table_does_not_exist;

-- Verify trash catalog unchanged
SELECT count(*) AS count_after_if_exists FROM pgtrashcan._trash_catalog;

-- ========================================
-- TEST 38: DROP TABLE IF EXISTS - single existing table
-- ========================================
CREATE TABLE test_if_exists_single (id int, val text);
INSERT INTO test_if_exists_single VALUES (1, 'exists');

DROP TABLE IF EXISTS test_if_exists_single;

-- Verify table was moved to trash (not permanently dropped)
SELECT orig_name, orig_schema, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_if_exists_single';

-- Clean up
SELECT pgtrashcan_purge('test_if_exists_single') IS NOT NULL AS purged;

-- ========================================
-- TEST 39: DROP TABLE IF EXISTS - multi-table, first doesn't exist
-- ========================================
CREATE TABLE test_multi_real (id int, val text);
INSERT INTO test_multi_real VALUES (1, 'should be in trash');

DROP TABLE IF EXISTS nonexistent_table_xyz, test_multi_real;

-- Verify real table was moved to trash (NOT permanently dropped)
SELECT orig_name, orig_schema, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_multi_real';

-- Clean up
SELECT pgtrashcan_purge('test_multi_real') IS NOT NULL AS purged;

-- ========================================
-- TEST 40: DROP TABLE IF EXISTS - multi-table, second doesn't exist
-- ========================================
CREATE TABLE test_multi_real2 (id int, val text);
INSERT INTO test_multi_real2 VALUES (2, 'also should be in trash');

DROP TABLE IF EXISTS test_multi_real2, another_nonexistent_xyz;

-- Verify real table was moved to trash
SELECT orig_name, orig_schema, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name = 'test_multi_real2';

-- Clean up
SELECT pgtrashcan_purge('test_multi_real2') IS NOT NULL AS purged;

-- ========================================
-- TEST 41: DROP TABLE IF EXISTS - multi-table, all exist
-- ========================================
CREATE TABLE test_multi_all1 (id int);
CREATE TABLE test_multi_all2 (id int);
CREATE TABLE test_multi_all3 (id int);

DROP TABLE IF EXISTS test_multi_all1, test_multi_all2, test_multi_all3;

-- Verify all three were moved to trash
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('test_multi_all1', 'test_multi_all2', 'test_multi_all3')
  AND orig_obj_type = 'table'
ORDER BY orig_name;

-- Clean up
SELECT pgtrashcan_purge('test_multi_all1') IS NOT NULL AS purged1;
SELECT pgtrashcan_purge('test_multi_all2') IS NOT NULL AS purged2;
SELECT pgtrashcan_purge('test_multi_all3') IS NOT NULL AS purged3;

-- ========================================
-- TEST 42: DROP TABLE IF EXISTS - multi-table, none exist
-- ========================================
DROP TABLE IF EXISTS ghost1, ghost2, ghost3;

-- Verify trash catalog unchanged
SELECT count(*) AS count_after_all_ghosts FROM pgtrashcan._trash_catalog;

-- ========================================
-- TEST 43: DROP TABLE IF EXISTS - multi-table, middle missing
-- ========================================
CREATE TABLE test_multi_a (id int);
CREATE TABLE test_multi_c (id int);

DROP TABLE IF EXISTS test_multi_a, nonexistent_middle_xyz, test_multi_c;

-- Verify both existing tables were moved to trash
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('test_multi_a', 'test_multi_c')
  AND orig_obj_type = 'table'
ORDER BY orig_name;

-- Clean up
SELECT pgtrashcan_purge('test_multi_a') IS NOT NULL AS purged_a;
SELECT pgtrashcan_purge('test_multi_c') IS NOT NULL AS purged_c;

-- ========================================
-- TEST 44: DROP TABLE IF EXISTS with CASCADE
-- ========================================
CREATE TABLE if_exists_cascade_base (id int PRIMARY KEY, val text);
CREATE VIEW if_exists_cascade_view AS SELECT * FROM if_exists_cascade_base WHERE id > 0;
INSERT INTO if_exists_cascade_base VALUES (1, 'test');

-- IF EXISTS + CASCADE: base moves to trash, view permanently dropped
DROP TABLE IF EXISTS if_exists_cascade_base CASCADE;

-- Base should be in trash
SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name = 'if_exists_cascade_base';

-- View should be gone
SELECT count(*) AS view_count FROM pg_views
WHERE viewname = 'if_exists_cascade_view' AND schemaname = 'public';

-- View definition should be in metadata
SELECT orig_name, metadata ? 'views_dropped' AS has_view_metadata
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'if_exists_cascade_base';

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 45: DROP TABLE multi-table without IF EXISTS
-- ========================================
CREATE TABLE test_multi_no_ie_1 (id int);
CREATE TABLE test_multi_no_ie_2 (id int);

DROP TABLE test_multi_no_ie_1, test_multi_no_ie_2;

SELECT orig_name, orig_obj_type FROM pgtrashcan._trash_catalog
WHERE orig_name IN ('test_multi_no_ie_1', 'test_multi_no_ie_2')
  AND orig_obj_type = 'table'
ORDER BY orig_name;

-- Clean up
SELECT pgtrashcan_purge('test_multi_no_ie_1') IS NOT NULL AS purged1;
SELECT pgtrashcan_purge('test_multi_no_ie_2') IS NOT NULL AS purged2;

-- ########################################
-- SECTION 8: Protection Mechanisms
-- ########################################

-- ========================================
-- TEST 46: pgtrashcan schema protection (DML + DDL + schema drop)
-- ========================================

-- Cannot create tables in pgtrashcan schema
CREATE TABLE pgtrashcan.user_junk (id int);

-- Cannot insert into catalog directly (blocked by trigger)
INSERT INTO pgtrashcan._trash_catalog (trash_name, orig_name, orig_schema, orig_oid, orig_obj_type)
VALUES ('fake', 'fake', 'public', 0, 'table');

-- To test UPDATE/DELETE blocking, we need a row in the catalog first.
-- Drop a table to get a real row, then try to modify it directly.
CREATE TABLE dml_protect_test (id int);
DROP TABLE dml_protect_test;

-- Cannot update catalog directly (blocked by trigger)
UPDATE pgtrashcan._trash_catalog SET orig_name = 'hacked' WHERE orig_name = 'dml_protect_test';

-- Cannot delete from catalog directly (blocked by trigger)
DELETE FROM pgtrashcan._trash_catalog WHERE orig_name = 'dml_protect_test';

-- Cannot truncate catalog (blocked by ProcessUtility hook)
TRUNCATE pgtrashcan._trash_catalog;

-- Clean up the test row via purge
SELECT pgtrashcan_purge('dml_protect_test') IS NOT NULL AS purged;

-- Cannot drop pgtrashcan schema while extension is installed
DROP SCHEMA pgtrashcan CASCADE;

-- ########################################
-- SECTION 9: Listing Functions
-- ########################################

-- ========================================
-- TEST 47: pgtrashcan_list_dropped() - basic listing
-- ========================================
CREATE TABLE list_test_a (id int, val text);
CREATE TABLE list_test_b (id int);
INSERT INTO list_test_a VALUES (1, 'hello');
DROP TABLE list_test_a;
DROP TABLE list_test_b;

-- Both should appear in list_dropped
SELECT orig_name, orig_schema, obj_type
FROM pgtrashcan_list_dropped()
ORDER BY orig_name;

-- is_dependent should be false for both
SELECT orig_name, is_dependent
FROM pgtrashcan_list_dropped()
ORDER BY orig_name;

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 48: pgtrashcan_list_dropped_grouped() - standalone tables
-- ========================================
CREATE TABLE grouped_t1 (id int);
CREATE TABLE grouped_t2 (id int);
DROP TABLE grouped_t1;
DROP TABLE grouped_t2;

-- Both should show as standalone (separate drops, no CASCADE relationship)
SELECT drop_group, relationship, orig_name
FROM pgtrashcan_list_dropped_grouped()
ORDER BY orig_name;

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 49: pgtrashcan_list_dropped_grouped() - CASCADE parent with FK child
-- ========================================
CREATE TABLE grouped_parent (id int PRIMARY KEY);
CREATE TABLE grouped_child (id int, pid int REFERENCES grouped_parent(id));
INSERT INTO grouped_parent VALUES (1);
INSERT INTO grouped_child VALUES (1, 1);

DROP TABLE grouped_parent CASCADE;

-- Only parent in trash, child survives - parent should show as standalone
SELECT drop_group, relationship, orig_name
FROM pgtrashcan_list_dropped_grouped()
ORDER BY orig_name;

DROP TABLE grouped_child;
SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ========================================
-- TEST 50: pgtrashcan_list_dropped_grouped() - multi-table DROP with FK
-- Both tables in same DROP statement, no cascade_parent_oid
-- ========================================
CREATE TABLE grouped_mp (id int PRIMARY KEY);
CREATE TABLE grouped_mc (id int, pid int REFERENCES grouped_mp(id));
INSERT INTO grouped_mp VALUES (1);
INSERT INTO grouped_mc VALUES (1, 1);

DROP TABLE grouped_mp, grouped_mc;

-- Both in trash as standalone (same DROP statement, no CASCADE relationship)
SELECT drop_group, relationship, orig_name
FROM pgtrashcan_list_dropped_grouped()
ORDER BY orig_name;

SELECT pgtrashcan_purge_all() IS NOT NULL AS purged;

-- ########################################
-- SECTION 10: Protection: ALTER/RENAME on pgtrashcan Objects
-- ########################################

-- ========================================
-- TEST 52: Block direct SET SCHEMA on trash object
-- ========================================
CREATE TABLE alter_protect_test (id int, val text);
INSERT INTO alter_protect_test VALUES (1, 'data');
DROP TABLE alter_protect_test;

-- Attempt direct SET SCHEMA - should be blocked (insufficient_privilege)
DO $$
DECLARE v_tname text;
BEGIN
    SELECT trash_name INTO v_tname FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'alter_protect_test' LIMIT 1;
    EXECUTE format('ALTER TABLE pgtrashcan.%I SET SCHEMA public', v_tname);
    RAISE WARNING 'FAIL: SET SCHEMA on trash object was NOT blocked';
EXCEPTION WHEN insufficient_privilege THEN
    RAISE WARNING 'PASS: SET SCHEMA correctly blocked: %', SQLERRM;
END $$;

-- Table must NOT have leaked to public (the definitive protection check)
SELECT count(*) AS leaked_to_public FROM pg_tables
WHERE schemaname = 'public' AND tablename = 'alter_protect_test';

-- Catalog entry must still exist
SELECT count(*) AS still_in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'alter_protect_test';

SELECT pgtrashcan_purge('alter_protect_test') IS NOT NULL AS purged;

-- ========================================
-- TEST 53: Block direct RENAME on trash object
-- ========================================
CREATE TABLE rename_protect_test (id int);
DROP TABLE rename_protect_test;

-- Attempt direct RENAME - should be blocked (insufficient_privilege)
DO $$
DECLARE v_tname text;
BEGIN
    SELECT trash_name INTO v_tname FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'rename_protect_test' LIMIT 1;
    EXECUTE format('ALTER TABLE pgtrashcan.%I RENAME TO something_else', v_tname);
    RAISE WARNING 'FAIL: RENAME on trash object was NOT blocked';
EXCEPTION WHEN insufficient_privilege THEN
    RAISE WARNING 'PASS: RENAME correctly blocked: %', SQLERRM;
END $$;

-- The rename target must NOT exist in pgtrashcan (protection definitive check)
SELECT count(*) AS rename_leaked FROM pg_tables
WHERE schemaname = 'pgtrashcan' AND tablename = 'something_else';

-- Catalog entry must still exist
SELECT count(*) AS still_in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'rename_protect_test';

SELECT pgtrashcan_purge('rename_protect_test') IS NOT NULL AS purged;

-- ========================================
-- TEST 54: pgtrashcan_move_to_schema() - simple table
-- ========================================
CREATE SCHEMA move_target_schema;
CREATE TABLE move_to_schema_test (id int, val text);
INSERT INTO move_to_schema_test VALUES (1, 'hello'), (2, 'world');
DROP TABLE move_to_schema_test;

-- Move to a different schema (not original)
DO $$
DECLARE v_tname text;
BEGIN
    SELECT trash_name INTO v_tname FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'move_to_schema_test' LIMIT 1;
    PERFORM pgtrashcan_move_to_schema(v_tname, 'move_target_schema');
END $$;

-- Table should now be in move_target_schema with original name
SELECT count(*) AS row_count FROM move_target_schema.move_to_schema_test;

-- Catalog entry should be gone
SELECT count(*) AS catalog_count FROM pgtrashcan._trash_catalog
WHERE orig_name = 'move_to_schema_test';

-- Clean up
DROP TABLE move_target_schema.move_to_schema_test;
SELECT pgtrashcan_purge('move_to_schema_test') IS NOT NULL AS purged;
DROP SCHEMA move_target_schema;

-- ========================================
-- TEST 55: pgtrashcan_move_to_schema() - table with index and sequence
-- ========================================
CREATE SCHEMA move_archive_schema;
CREATE TABLE move_with_deps_test (id SERIAL PRIMARY KEY, name text);
CREATE INDEX idx_move_with_deps ON move_with_deps_test(name);
INSERT INTO move_with_deps_test (name) VALUES ('alice'), ('bob');
DROP TABLE move_with_deps_test;

-- Move table (with its index and sequence) to archive schema
DO $$
DECLARE v_tname text;
BEGIN
    SELECT trash_name INTO v_tname FROM pgtrashcan._trash_catalog
    WHERE orig_name = 'move_with_deps_test' AND orig_obj_type = 'table' LIMIT 1;
    PERFORM pgtrashcan_move_to_schema(v_tname, 'move_archive_schema');
END $$;

-- Table should be in archive schema with original name and data intact
SELECT count(*) AS row_count FROM move_archive_schema.move_with_deps_test;

-- Catalog should have no entries for this table or its dependents
SELECT count(*) AS catalog_count FROM pgtrashcan._trash_catalog
WHERE orig_name = 'move_with_deps_test'
   OR orig_name IN ('idx_move_with_deps', 'move_with_deps_test_id_seq');

-- Index should exist in archive schema
SELECT count(*) AS idx_count FROM pg_indexes
WHERE schemaname = 'move_archive_schema' AND tablename = 'move_with_deps_test';

-- Clean up
DROP TABLE move_archive_schema.move_with_deps_test;
SELECT pgtrashcan_purge('move_with_deps_test') IS NOT NULL AS purged;
DROP SCHEMA move_archive_schema;

-- ########################################
-- SECTION 11: Temp Table Pass-Through
-- ########################################

-- ========================================
-- TEST 56: DROP on temp table passes through (not trashed)
-- ========================================
CREATE TEMP TABLE temp_drop_test (id int, val text);
INSERT INTO temp_drop_test VALUES (1, 'ephemeral');

-- Should emit NOTICE and permanently drop (not moved to trash)
DROP TABLE temp_drop_test;

-- Must NOT appear in trash catalog
SELECT count(*) AS temp_in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'temp_drop_test';

-- Must be gone entirely
SELECT count(*) AS temp_exists FROM pg_tables
WHERE tablename = 'temp_drop_test';

-- ========================================
-- TEST 57: Mixed DROP - regular + temp table
-- ========================================
CREATE TABLE regular_for_mix (id int);
CREATE TEMP TABLE temp_for_mix (id int);
INSERT INTO regular_for_mix VALUES (1);
INSERT INTO temp_for_mix VALUES (2);

-- Mixed drop: regular goes to trash, temp is permanently dropped
DROP TABLE regular_for_mix, temp_for_mix;

-- Regular table should be in trash
SELECT count(*) AS regular_in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'regular_for_mix';

-- Temp table must NOT be in trash
SELECT count(*) AS temp_in_trash FROM pgtrashcan._trash_catalog
WHERE orig_name = 'temp_for_mix';

-- Temp table must be gone entirely
SELECT count(*) AS temp_exists FROM pg_tables
WHERE tablename = 'temp_for_mix';

SELECT pgtrashcan_purge('regular_for_mix') IS NOT NULL AS purged;

-- ########################################
-- SECTION 12: GUC pgtrashcan.enabled (session toggle)
-- ########################################

-- ========================================
-- TEST 58a: GUC off -> DROP TABLE permanently drops (no pgtrashcan entry)
-- ========================================
CREATE TABLE guc_test (id int);
SET pgtrashcan.enabled = false;
DROP TABLE guc_test;
SELECT count(*) AS trash_entries
FROM pgtrashcan._trash_catalog
WHERE orig_name = 'guc_test';
SELECT count(*) AS still_in_pg_class
FROM pg_class
WHERE relname = 'guc_test' AND relnamespace = 'public'::regnamespace;
RESET pgtrashcan.enabled;

-- ========================================
-- TEST 58b: GUC off -> permanent purge from pgtrashcan still works
-- ========================================
CREATE TABLE guc_keep (id int);
DROP TABLE guc_keep;                                         -- normal: goes to pgtrashcan
SELECT count(*) AS before_purge
FROM pgtrashcan._trash_catalog WHERE orig_name = 'guc_keep';  -- 1

SET pgtrashcan.enabled = false;
SELECT pgtrashcan_purge('guc_keep') IS NOT NULL AS purged;   -- still works
SELECT count(*) AS after_purge
FROM pgtrashcan._trash_catalog WHERE orig_name = 'guc_keep';  -- 0
RESET pgtrashcan.enabled;

-- ========================================
-- TEST 58c: GUC off -> pgtrashcan protections still active
-- ========================================
SET pgtrashcan.enabled = false;
DO $$ BEGIN
    BEGIN
        DROP TABLE pgtrashcan._trash_catalog;
        RAISE WARNING 'pgtrashcan: catalog drop unexpectedly allowed';
    EXCEPTION WHEN insufficient_privilege THEN
        RAISE WARNING 'pgtrashcan: catalog drop blocked even when GUC is false';
    END;
END $$;
RESET pgtrashcan.enabled;

-- ########################################
-- SECTION 13: Final State
-- ########################################

-- ========================================
-- TEST 58: Final state check - trash must be empty
-- ========================================
SELECT count(*) AS final_count FROM pgtrashcan._trash_catalog;
