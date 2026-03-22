-- Non-MVCC index scans must return every visible member of a HOT chain.
-- Verify that table_index_getnext_slot gets that right in a variety of cases.

CREATE EXTENSION test_indexscan;

-- Single-page table; all TIDs below are deterministic: a fresh table gets
-- block 0, heap_insert/heap_update assign line pointers sequentially, and the
-- rows are tiny to avoid any toasting.
CREATE TABLE hot_chain_tab (id int, filler text) WITH (autovacuum_enabled = off);
CREATE INDEX hot_chain_idx ON hot_chain_tab (id);
INSERT INTO hot_chain_tab VALUES (1, 'r1v1'), (2, 'r2v1');  -- (0,1) (0,2)

BEGIN;

-- Create HOT chains.  These updates touch only the non-indexed column
-- "filler", and every chain member stays visible to SnapshotAny because the
-- deleting transaction (us) is still open.
UPDATE hot_chain_tab SET filler = 'r1v2' WHERE id = 1;   -- (0,3)
UPDATE hot_chain_tab SET filler = 'r1v3' WHERE id = 1;   -- (0,4)
UPDATE hot_chain_tab SET filler = 'r2v2' WHERE id = 2;   -- (0,5)

-- Verify that all three updates were HOT using backend-local xact counter:
SELECT pg_stat_get_xact_tuples_hot_updated('hot_chain_tab'::regclass) AS hot_updated;

-- SnapshotAny: every chain member
SELECT * FROM index_scan_tids('hot_chain_idx', 'any');

-- MVCC scan, for contrast: only the newest member of each chain
SELECT * FROM index_scan_tids('hot_chain_idx', 'mvcc');

-- backward: index entries in reverse key order, chains still in ASC chain order
SELECT * FROM index_scan_tids('hot_chain_idx', 'any', 'backward');

COMMIT;

DROP TABLE hot_chain_tab;
DROP EXTENSION test_indexscan;
