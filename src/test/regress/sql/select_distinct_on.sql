--
-- SELECT_DISTINCT_ON
--

SELECT DISTINCT ON (string4) string4, two, ten
   FROM onek
   ORDER BY string4 using <, two using >, ten using <;

-- this will fail due to conflict of ordering requirements
SELECT DISTINCT ON (string4, ten) string4, two, ten
   FROM onek
   ORDER BY string4 using <, two using <, ten using <;

SELECT DISTINCT ON (string4, ten) string4, ten, two
   FROM onek
   ORDER BY string4 using <, ten using >, two using <;

-- bug #5049: early 8.4.x chokes on volatile DISTINCT ON clauses
select distinct on (1) floor(random()) as r, f1 from int4_tbl order by 1,2;

--
-- Test the planner's ability to use a LIMIT 1 instead of a Unique node when
-- all of the distinct_pathkeys have been marked as redundant
--

-- Ensure we also get a LIMIT plan with DISTINCT ON
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (four) four,two
   FROM tenk1 WHERE four = 0 ORDER BY 1;

-- and check the result of the above query is correct
SELECT DISTINCT ON (four) four,two
   FROM tenk1 WHERE four = 0 ORDER BY 1;

-- Ensure a Sort -> Limit is used when the ORDER BY contains additional cols
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (four) four,two
   FROM tenk1 WHERE four = 0 ORDER BY 1,2;

-- Same again but use a column that is indexed so that we get an index scan
-- then a limit
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (four) four,hundred
   FROM tenk1 WHERE four = 0 ORDER BY 1,2;

--
-- Test the planner's ability to reorder the distinctClause Pathkeys to match
-- the input path's ordering
--

CREATE TABLE distinct_on_tbl (x int, y int, z int);
INSERT INTO distinct_on_tbl SELECT i%10, i%10, i%10 FROM generate_series(1, 1000) AS i;
CREATE INDEX distinct_on_tbl_x_y_idx ON distinct_on_tbl (x, y);
ANALYZE distinct_on_tbl;

-- Produce results with sorting.
SET enable_hashagg TO OFF;

-- Ensure we avoid the need to re-sort by reordering the distinctClause
-- Pathkeys to match the ordering of the input path
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (y, x) x, y FROM distinct_on_tbl;
SELECT DISTINCT ON (y, x) x, y FROM distinct_on_tbl;

-- Ensure we leverage incremental-sort by reordering the distinctClause
-- Pathkeys to partially match the ordering of the input path
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (y, x) x, y FROM (SELECT * FROM distinct_on_tbl ORDER BY x) s;
SELECT DISTINCT ON (y, x) x, y FROM (SELECT * FROM distinct_on_tbl ORDER BY x) s;

-- Ensure we reorder the distinctClause Pathkeys to match the ordering of the
-- input path even if there is ORDER BY clause
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (y, x) x, y FROM distinct_on_tbl ORDER BY y;
SELECT DISTINCT ON (y, x) x, y FROM distinct_on_tbl ORDER BY y;

-- Ensure the resulting pathkey list matches the initial distinctClause Pathkeys
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (y, x) x, y FROM (select * from distinct_on_tbl order by x, z, y) s ORDER BY y, x, z;
SELECT DISTINCT ON (y, x) x, y FROM (select * from distinct_on_tbl order by x, z, y) s ORDER BY y, x, z;

RESET enable_hashagg;

DROP TABLE distinct_on_tbl;

--
-- Test SELECT DISTINCT ON with inline ORDER BY
--
CREATE TABLE distinct_inline_tbl (a int, b int);
INSERT INTO distinct_inline_tbl VALUES (1, 10), (1, 20), (2, 5), (2, 15);

SET enable_hashagg TO OFF;
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (a ORDER BY b DESC) a, b FROM distinct_inline_tbl ORDER BY a;
SELECT DISTINCT ON (a ORDER BY b DESC) a, b FROM distinct_inline_tbl ORDER BY a;
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (a ORDER BY b ASC) a, b FROM distinct_inline_tbl ORDER BY a;
SELECT DISTINCT ON (a ORDER BY b ASC) a, b FROM distinct_inline_tbl ORDER BY a;

-- Test with HashAgg
RESET enable_hashagg;
SET enable_sort TO OFF;
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (a ORDER BY b DESC) a, b FROM distinct_inline_tbl;
SELECT * FROM (SELECT DISTINCT ON (a ORDER BY b DESC) a, b FROM distinct_inline_tbl) s ORDER BY a;
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (a ORDER BY b ASC) a, b FROM distinct_inline_tbl;
SELECT * FROM (SELECT DISTINCT ON (a ORDER BY b ASC) a, b FROM distinct_inline_tbl) s ORDER BY a;
RESET enable_sort;

DROP TABLE distinct_inline_tbl;

-- Test HashAgg with unreferenced columns, resjunk tie-breaker expressions,
-- pass-by-reference text values, NULLS FIRST/LAST, duplicate keys, and views
CREATE TABLE distinct_hash_edge_tbl (
    unused1 int,
    grp text,
    unused2 text,
    tie1 int,
    tie2 text,
    payload text
);
INSERT INTO distinct_hash_edge_tbl VALUES
    (100, 'g1', 'u', 10, 'B', 'p1_10_b'),
    (101, 'g1', 'u', 20, 'c', 'p1_20_c_short'),
    (102, 'g1', 'u', 20, 'a', 'p1_20_a_much_longer_payload_string'),
    (103, 'g1', 'u', NULL, 'z', 'p1_null_z'),
    (104, 'g2', 'u', 5, 'x', 'p2_5_x_long_initial_string'),
    (105, 'g2', 'u', 15, 'y', 'p2_15_y');

SET enable_sort TO OFF;
SELECT * FROM (
    SELECT DISTINCT ON (grp ORDER BY tie1 DESC NULLS LAST, lower(tie2) ASC)
        grp, payload
    FROM distinct_hash_edge_tbl
) s ORDER BY grp;

SELECT * FROM (
    SELECT DISTINCT ON (grp ORDER BY tie1 DESC NULLS FIRST, lower(tie2) ASC)
        grp, payload
    FROM distinct_hash_edge_tbl
) s ORDER BY grp;
RESET enable_sort;

-- Outer ORDER BY different from inline ORDER BY
SELECT DISTINCT ON (grp ORDER BY tie1 DESC NULLS LAST, lower(tie2) ASC)
    grp, tie1, tie2, payload
FROM distinct_hash_edge_tbl
ORDER BY payload ASC;

-- Duplicate keys in DISTINCT ON and inline ORDER BY
SELECT DISTINCT ON (grp, grp ORDER BY tie1 DESC NULLS LAST, tie1 ASC, lower(tie2) ASC)
    grp, tie1, tie2, payload
FROM distinct_hash_edge_tbl
ORDER BY grp;

-- Redundant DISTINCT ON key with inline ORDER BY should use Limit 1
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (grp ORDER BY tie1 DESC NULLS LAST, lower(tie2) ASC)
    grp, tie1, tie2, payload
FROM distinct_hash_edge_tbl
WHERE grp = 'g1';

SELECT DISTINCT ON (grp ORDER BY tie1 DESC NULLS LAST, lower(tie2) ASC)
    grp, tie1, tie2, payload
FROM distinct_hash_edge_tbl
WHERE grp = 'g1';

-- View deparsing for SELECT DISTINCT ON (... ORDER BY ...)
CREATE VIEW distinct_inline_view AS
SELECT DISTINCT ON (grp ORDER BY tie1 DESC NULLS LAST, tie2 ASC)
    grp, tie1, tie2, payload
FROM distinct_hash_edge_tbl;

SELECT pg_get_viewdef('distinct_inline_view', true);
SELECT * FROM distinct_inline_view ORDER BY grp;

DROP VIEW distinct_inline_view;

-- Positional and table-qualified DISTINCT ON with inline ORDER BY, and
-- inline ORDER BY where DISTINCT ON key is not the first sort expression
SET enable_hashagg TO OFF;
SELECT DISTINCT ON (1 ORDER BY grp DESC, tie1 DESC NULLS LAST)
    grp, tie1, payload
FROM distinct_hash_edge_tbl;

SELECT DISTINCT ON (distinct_hash_edge_tbl.grp ORDER BY grp DESC, tie1 DESC NULLS LAST)
    grp, tie1, payload
FROM distinct_hash_edge_tbl;

SELECT DISTINCT ON (grp ORDER BY tie1 DESC NULLS LAST, grp ASC)
    grp, tie1, payload
FROM distinct_hash_edge_tbl
ORDER BY grp;
RESET enable_hashagg;

-- HashAggregate disk spill mode with DISTINCT ON (... ORDER BY ...) and
-- growing varlena replacements
CREATE TEMP TABLE distinct_spill_tbl (grp int, tie int, payload text);
INSERT INTO distinct_spill_tbl
SELECT i % 500, j, repeat('x', j * 20)
FROM generate_series(1, 2000) i, generate_series(1, 3) j;

SET enable_sort TO OFF;
SET work_mem = '64kB';
SELECT count(*), min(tie), max(tie), min(length(payload)), max(length(payload))
FROM (
    SELECT DISTINCT ON (grp ORDER BY tie DESC) grp, tie, payload
    FROM distinct_spill_tbl
) s;
RESET work_mem;
RESET enable_sort;
DROP TABLE distinct_spill_tbl;

DROP TABLE distinct_hash_edge_tbl;

