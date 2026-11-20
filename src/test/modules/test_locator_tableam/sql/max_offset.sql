CREATE EXTENSION IF NOT EXISTS test_locator_tableam;

--
-- A TID bitmap holds offsets only up to MaxHeapTuplesPerPage.  heap_bigoffset
-- is heap with a LocatorDesc that declares max_offset = MaxOffsetNumber, so
-- the planner must not consider bitmap scans of it, and GIN, which is scanned
-- only through bitmaps, must refuse to index it.
--
-- The error details name block-size-dependent limits, so they are not
-- shown.
--
\set VERBOSITY terse
CREATE TABLE bigoff_t (a int, b int, t int[]) USING heap_bigoffset;
INSERT INTO bigoff_t SELECT g, g % 10, ARRAY[g] FROM generate_series(1, 1000) g;
CREATE INDEX bigoff_t_a ON bigoff_t (a);
CREATE INDEX bigoff_t_b ON bigoff_t USING hash (b);
CREATE INDEX bigoff_t_t ON bigoff_t USING gin (t);
VACUUM ANALYZE bigoff_t;
SET enable_seqscan = off;
SET enable_indexscan = off;
-- no bitmap scan: the planner falls back to a disabled scan type
EXPLAIN (COSTS OFF) SELECT * FROM bigoff_t WHERE a = 1 OR a = 2;
SELECT a FROM bigoff_t WHERE a = 1 OR a = 2 ORDER BY a;
EXPLAIN (COSTS OFF) SELECT * FROM bigoff_t WHERE a < 10 AND b = 3;
-- the same table under heap does use one
ALTER TABLE bigoff_t SET ACCESS METHOD heap;
EXPLAIN (COSTS OFF) SELECT * FROM bigoff_t WHERE a = 1 OR a = 2;
CREATE INDEX bigoff_t_t ON bigoff_t USING gin (t);
-- and GIN prevents switching back
ALTER TABLE bigoff_t SET ACCESS METHOD heap_bigoffset;
RESET enable_seqscan;
RESET enable_indexscan;
DROP TABLE bigoff_t;
\set VERBOSITY default
