-- Tests for extensible node and custom scan registration
-- (src/backend/nodes/extensible.c)

CREATE EXTENSION test_extensible;

-- ----------------------------------------------------------------
-- GetExtensibleNodeMethods() and GetCustomScanMethods() lookup tests
-- ----------------------------------------------------------------

-- GetExtensibleNodeMethods: known name returns the registered extnodename.
SELECT test_get_extensible_node_methods('TestExtNode', false);
-- GetExtensibleNodeMethods: unknown name with missing_ok=true returns NULL.
SELECT test_get_extensible_node_methods('NoSuchExtNode', true);
-- GetExtensibleNodeMethods: unknown name with missing_ok=false raises ERROR.
SELECT test_get_extensible_node_methods('NoSuchExtNode', false);

-- GetCustomScanMethods: known name returns the registered CustomName.
SELECT test_get_custom_scan_methods('TestCustomScan', false);
-- GetCustomScanMethods: unknown name with missing_ok=true returns NULL.
SELECT test_get_custom_scan_methods('NoSuchCustomScan', true);
-- GetCustomScanMethods: unknown name with missing_ok=false raises ERROR.
SELECT test_get_custom_scan_methods('NoSuchCustomScan', false);

-- ----------------------------------------------------------------
-- ExtensibleNodeMethods callbacks: nodeCopy, nodeEqual, nodeOut, nodeRead
-- ----------------------------------------------------------------

-- Copy the node, compare it with the original, serialize it and read it back.
-- The returned string is the output of the nodeOut callback.
SELECT test_ext_node_callbacks('1234'::oid, 2);

-- ----------------------------------------------------------------
-- End-to-end CustomScan test
-- ----------------------------------------------------------------

CREATE TABLE test_extensible_tbl (id integer, val text);
INSERT INTO test_extensible_tbl VALUES (1, 'one'), (2, 'two'), (3, 'three');

-- Verify the planner chose our CustomScan (not a SeqScan).
EXPLAIN (COSTS OFF) SELECT id, val FROM test_extensible_tbl ORDER BY id;

-- Execute through the CustomScan; each of the 3 inserted rows appears
-- twice (6 rows total), proving the custom scan logic is in effect.
SELECT id, val FROM test_extensible_tbl ORDER BY id;

-- The restriction clauses are passed to the CustomScan as its qual.
EXPLAIN (COSTS OFF) SELECT val FROM test_extensible_tbl WHERE id > 1;
SELECT val FROM test_extensible_tbl WHERE id > 1 ORDER BY id;

DROP TABLE test_extensible_tbl;

DROP EXTENSION test_extensible;
