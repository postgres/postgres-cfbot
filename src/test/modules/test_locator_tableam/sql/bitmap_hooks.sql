CREATE EXTENSION IF NOT EXISTS test_locator_tableam;

--
-- A table AM's bitmap_or_inexact callback marks the groups a BitmapOr
-- returns for recheck.  heap_orrecheck flags every group; the plan is the
-- same as on heap and so are the rows.  tbm_recheck_inexact_unions() is
-- reached only through such a callback, so this runs it.
--
CREATE TABLE orr (a int, b int) USING heap_orrecheck;
INSERT INTO orr SELECT g, g FROM generate_series(1, 2000) g;
CREATE INDEX orr_a ON orr (a);
CREATE INDEX orr_b ON orr (b);
VACUUM ANALYZE orr;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF) SELECT * FROM orr WHERE a = 7 OR b = 1500;
SELECT a, b FROM orr WHERE a = 7 OR b = 1500 ORDER BY a;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
RESET max_parallel_workers_per_gather;
DROP TABLE orr;

--
-- An index AM states whether it can store a variable-width locator.  No
-- in-core index AM can, so each is refused for a table such as heap_wide.
--
SELECT amname, pg_indexam_has_property(oid, 'can_var_locator')
  FROM pg_am WHERE amtype = 'i' ORDER BY amname;
