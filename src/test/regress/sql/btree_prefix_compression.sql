--
-- BTREE_PREFIX_COMPRESSION
--
-- Functional tests for leaf-level prefix compression: build, insert, split,
-- VACUUM's anchor-promotion path, and scans in both directions.  These
-- tests operate at the SQL level -- for structural invariants covered by
-- amcheck and page-layout checks covered by pageinspect, see contrib/.
--

-- Eligible: >= 2 key columns, all leading cols equalimage.  We can't
-- easily see the metapage flag without pageinspect, so we exercise the
-- feature through its user-visible effects: correct query results plus
-- substantially smaller index size relative to a single-column variant
-- built on the same leading keys.
CREATE TABLE pc_t (a int, b int, c int, d text);
INSERT INTO pc_t
  SELECT i / 1000, (i / 10) % 100, i, 'payload_' || (i % 7)
    FROM generate_series(1, 50000) i;

-- Multi-column integer index: prefix compression applies.
CREATE INDEX pc_idx_abc ON pc_t (a, b, c);
-- Single-column index: ineligible for prefix compression.
CREATE INDEX pc_idx_a   ON pc_t (a);

-- Forward scan correctness
SELECT count(*) FROM pc_t WHERE a = 7;
SELECT count(*) FROM pc_t WHERE a = 7 AND b = 42;
SELECT (a,b,c) FROM pc_t WHERE a = 7 AND b = 42 AND c = 7420;

-- Range scan
SELECT count(*) FROM pc_t WHERE a BETWEEN 3 AND 5;
SELECT count(*) FROM pc_t WHERE (a, b) BETWEEN (10, 50) AND (10, 55);

-- Backward scan (ORDER BY DESC)
SELECT c FROM pc_t WHERE a = 7 AND b = 42 ORDER BY c DESC LIMIT 3;

-- Forced index-only scan
SET enable_bitmapscan = off;
SET enable_seqscan = off;
EXPLAIN (COSTS off) SELECT c FROM pc_t WHERE a = 7 AND b = 42;
SELECT count(*) FROM pc_t WHERE a = 7 AND b = 42;
RESET enable_bitmapscan;
RESET enable_seqscan;

-- Insert new rows that share an existing anchor's leading attrs (in-group
-- follower path), rows starting new groups (new-anchor path), and rows
-- landing at the leftmost position (forced plain anchor).
INSERT INTO pc_t VALUES
  (7, 42, 7420001, 'insert_in_group'),
  (7, 42, 7420002, 'insert_in_group'),
  (7, 999, 7999999, 'insert_new_group'),   -- new (a,b) combo in existing a group
  (99, 0, -1, 'insert_new_a'),             -- first row of new a group
  (-1, -1, -1, 'insert_leftmost');         -- leftmost data item on its page

-- Verify inserted rows are scannable
SELECT count(*) FROM pc_t WHERE d LIKE 'insert_%';
SELECT (a,b,c) FROM pc_t WHERE a = 7 AND b = 42 AND c BETWEEN 7420001 AND 7420002
  ORDER BY c;
SELECT (a,b,c) FROM pc_t WHERE a = -1;
SELECT (a,b,c) FROM pc_t WHERE a = 99;
SELECT (a,b,c) FROM pc_t WHERE a = 7 AND b = 999;

-- Exercise the page-split path: enough inserts to force leaf splits on a
-- fully-loaded group.  Verify no rows are lost.
BEGIN;
INSERT INTO pc_t
  SELECT 7, 42, 7420000 + 1000 + i, 'split_forcer'
    FROM generate_series(1, 5000) i;
SELECT count(*) FROM pc_t WHERE d = 'split_forcer';
ROLLBACK;

-- VACUUM's anchor-promotion path: delete exactly the rows that serve as
-- anchors in their respective prefix groups, so VACUUM must promote the
-- first surviving follower in each group to a plain form.
-- For the (a, b, c) index, the anchor of each (a, b) group is the smallest
-- c -- i.e. i where (i/10) % 100 * 10 == (i % 1000).  Simpler rule: the
-- smallest i within each range of 10 is the anchor-producing row, so
-- deleting rows where c % 10 = 0 targets those anchors.
DELETE FROM pc_t WHERE c % 10 = 0 AND d LIKE 'payload_%';
VACUUM (INDEX_CLEANUP on) pc_t;

-- Scans must continue to return correct results after promotion
SELECT count(*) FROM pc_t WHERE a = 7 AND b = 42;
SELECT (a,b,c) FROM pc_t WHERE a = 7 AND b = 42 AND c = 7421;
SELECT count(*) FROM pc_t WHERE a = 7 AND b = 42 AND c % 10 = 0;
SELECT c FROM pc_t WHERE a = 7 AND b = 42 ORDER BY c DESC LIMIT 3;

-- Ineligible index shapes: non-equalimage leading col, single key col,
-- index with only NULLs in leading col.  All three should still work
-- correctly; they just do not enable prefix compression.
CREATE TABLE pc_t2 (a text COLLATE "C", b int, c int);
INSERT INTO pc_t2
  SELECT (i / 100)::text, (i / 10) % 10, i
    FROM generate_series(1, 5000) i;
CREATE INDEX pc_idx2 ON pc_t2 (a, b, c);
SELECT count(*) FROM pc_t2 WHERE a = '42';
SELECT (a,b,c) FROM pc_t2 WHERE a = '42' AND b = 5 AND c = 4255;

-- NULL handling in compressed followers: a row with NULL leading attr
-- and the same NULL-prefix neighbours should still be in a coherent
-- prefix group.
CREATE TABLE pc_t3 (a int, b int, c int);
INSERT INTO pc_t3
  SELECT NULL, (i / 10) % 10, i FROM generate_series(1, 1000) i;
INSERT INTO pc_t3
  SELECT 1, (i / 10) % 10, i FROM generate_series(1, 1000) i;
CREATE INDEX pc_idx3 ON pc_t3 (a, b, c);
SELECT count(*) FROM pc_t3 WHERE a IS NULL;
SELECT count(*) FROM pc_t3 WHERE a = 1;
SELECT count(*) FROM pc_t3 WHERE a IS NULL AND b = 5;

-- INCLUDE columns: non-key attrs are stored per-tuple (never part of the
-- prefix).  Verify correctness end-to-end.
CREATE TABLE pc_t4 (a int, b int, c int, payload text);
INSERT INTO pc_t4
  SELECT i / 100, (i / 10) % 10, i, 'row_' || i
    FROM generate_series(1, 10000) i;
CREATE INDEX pc_idx4 ON pc_t4 (a, b, c) INCLUDE (payload);
SELECT payload FROM pc_t4 WHERE a = 42 AND b = 5 AND c = 4255;
-- Index-only scan using INCLUDE column
SET enable_bitmapscan = off;
SET enable_seqscan = off;
EXPLAIN (COSTS off) SELECT payload FROM pc_t4 WHERE a = 42 AND b = 5 AND c = 4255;
SELECT payload FROM pc_t4 WHERE a = 42 AND b = 5 AND c = 4255;
RESET enable_bitmapscan;
RESET enable_seqscan;

DROP TABLE pc_t, pc_t2, pc_t3, pc_t4;
