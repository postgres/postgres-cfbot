-- Non-MVCC index scans must return every visible member of a HOT chain.
-- Verify that we get the details right in a variety of cases.

CREATE EXTENSION test_indexscan;

-- All TIDs below are deterministic: a fresh table gets block 0, and both
-- heap_insert and heap_update assign line pointers sequentially
CREATE TEMP TABLE hot_chain_tab (id int, filler text);
CREATE INDEX hot_chain_idx ON hot_chain_tab (id);
INSERT INTO hot_chain_tab VALUES (1, 'r1v1'), (2, 'r2v1');  -- (0,1) (0,2)

BEGIN;

-- Create HOT chains
UPDATE hot_chain_tab SET filler = 'r1v2' WHERE id = 1;   -- (0,3)
UPDATE hot_chain_tab SET filler = 'r1v3' WHERE id = 1;   -- (0,4)
UPDATE hot_chain_tab SET filler = 'r2v2' WHERE id = 2;   -- (0,5)

-- Verify that all three updates were HOT
SELECT pg_stat_get_xact_tuples_hot_updated('hot_chain_tab'::regclass) AS hot_updated;

-- SnapshotAny: every chain member
SELECT * FROM index_scan_tids('hot_chain_idx', 'any');
-- SnapshotAny, backward: index entries in reverse key order, chains still in ASC chain order
SELECT * FROM index_scan_tids('hot_chain_idx', 'any', false);

-- MVCC, for contrast: only the newest member of each chain
SELECT * FROM index_scan_tids('hot_chain_idx', 'mvcc');
-- MVCC, backward: index entries in reverse key order
SELECT * FROM index_scan_tids('hot_chain_idx', 'mvcc', false);

-- SnapshotDirty: show only the newest member of each chain, even though our
-- updating xact is still in progress
SELECT * FROM index_scan_tids('hot_chain_idx', 'dirty');

-- SnapshotNonVacuumable: show every chain member, since our updates are still
-- in progress
SELECT * FROM index_scan_tids('hot_chain_idx', 'nonvacuumable');

COMMIT;

-- SnapshotNonVacuumable: now that the updates have committed, older chain
-- members are shown as dead
SELECT * FROM index_scan_tids('hot_chain_idx', 'nonvacuumable');

DROP TABLE hot_chain_tab;
DROP EXTENSION test_indexscan;

-- Re-pruning must retain every producing hop between an index anchor and
-- the visible tuple, including hops in stubs left by an earlier prune.
CREATE TABLE hot_prune (id int PRIMARY KEY, k int, v int, u int, payload text)
  WITH (fillfactor = 40, autovacuum_enabled = false);
INSERT INTO hot_prune SELECT g, g % 101, (g * 17) % 103, g, repeat('x', 256)
  FROM generate_series(1, 20) g;
CREATE INDEX hot_prune_k ON hot_prune(k);
CREATE INDEX hot_prune_v ON hot_prune(v);
CREATE UNIQUE INDEX hot_prune_u ON hot_prune(u);
ANALYZE hot_prune;
VACUUM (FREEZE) hot_prune;
UPDATE hot_prune SET u = -u WHERE id BETWEEN 1 AND 10;
UPDATE hot_prune SET v = v + 1 WHERE id BETWEEN 1 AND 10;
VACUUM (INDEX_CLEANUP OFF) hot_prune;
VACUUM (INDEX_CLEANUP OFF) hot_prune;
UPDATE hot_prune SET k = k + 1;
UPDATE hot_prune SET v = v + 1;
BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;
SET LOCAL enable_seqscan = off;
SET LOCAL enable_bitmapscan = off;
SET LOCAL enable_indexonlyscan = off;
EXPLAIN (COSTS OFF)
SELECT id, u FROM hot_prune WHERE u BETWEEN -250 AND 250 ORDER BY u;
-- Check scan order and multiplicity directly, without aggregate reordering.
SELECT id, u FROM hot_prune WHERE u BETWEEN -250 AND 250 ORDER BY u;
SELECT id, u FROM hot_prune WHERE u = 1;
-- A direct mid-chain anchor excludes its own producing u hop.
SELECT id, u FROM hot_prune WHERE u = -1;
COMMIT;
-- The old key can be reused, but the current key remains unique.
INSERT INTO hot_prune VALUES (21, 0, 0, 1, 'old key');
INSERT INTO hot_prune VALUES (22, 0, 0, -1, 'duplicate');
DROP TABLE hot_prune;
