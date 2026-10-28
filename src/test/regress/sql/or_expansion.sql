--
-- Tests for OR-clause expansion into UNION ALL Append paths
--

CREATE TABLE orexp_a (id int, val int);
CREATE TABLE orexp_b (x int, y int, val int);

INSERT INTO orexp_a VALUES
  (1, 10),
  (2, 20),
  (3, 30),
  (4, NULL),
  (5, 50);

INSERT INTO orexp_b VALUES
  (1, 99, 100),
  (99, 2, 200),
  (3, 3, 300),
  (4, 99, 400),
  (99, 99, 500),
  (NULL, NULL, 600);

CREATE TYPE orexp_c_type AS (num int, str text);
CREATE TABLE orexp_phys (
    a int,
    b date,
    c text,
    d numeric,
    arr int[],
    comp orexp_c_type
);
CREATE TABLE orexp_phys2 (
    x int,
    y text,
    z date,
    arr int[]
);

INSERT INTO orexp_phys VALUES
  (1, '2026-01-01', 'foo', 10.5, ARRAY[10, 20], ROW(1, 'c1')),
  (2, '2026-02-01', 'bar', 20.0, ARRAY[30, 40], ROW(2, 'c2')),
  (3, '2026-03-01', 'baz', 30.25, ARRAY[50, 60], ROW(3, 'c3')),
  (4, '2026-04-01', 'qux', NULL, ARRAY[70, 80], ROW(4, 'c4'));

INSERT INTO orexp_phys SELECT g, '2026-01-01'::date + g, 'txt' || g, g * 1.5, ARRAY[g, g*2], (g, 'str' || g)::orexp_c_type FROM generate_series(5, 50) g;

INSERT INTO orexp_phys2 VALUES
  (1, 'bar', '2026-01-01', ARRAY[10, 99]),
  (99, 'foo', '2026-02-01', ARRAY[20, 88]),
  (3, 'other', '2026-03-01', ARRAY[50, 77]),
  (4, 'baz', '2026-05-01', ARRAY[99, 11]);

INSERT INTO orexp_phys2 SELECT g, 'txt' || g, '2026-01-01'::date + g, ARRAY[g, g*3] FROM generate_series(5, 50) g;

-- check GUC parameter
SHOW or_expansion_limit;

--
-- Disjunctive join conditions
--

-- disjunction of join keys
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

-- ensure results match when disabled
SET or_expansion_limit = 0;
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

RESET or_expansion_limit;

--
-- Target list and projection variants
--

-- physical column order differs from target list order
EXPLAIN (COSTS OFF)
SELECT p2.y, p1.d, p1.c, p2.z, p1.b, p1.a, p2.x
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY p1.a, p2.x, p2.y;

SELECT p2.y, p1.d, p1.c, p2.z, p1.b, p1.a, p2.x
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY p1.a, p2.x, p2.y
LIMIT 6;

-- complex expressions combining columns across relations
EXPLAIN (COSTS OFF)
SELECT
    p1.a + p2.x AS sum_ax,
    p1.c || '-' || p2.y AS str_concat,
    p2.z - p1.b AS date_diff,
    COALESCE(p1.d, 0) * 2.5 AS scaled_d,
    CASE WHEN p1.a > 10 THEN 'high' ELSE 'low' END AS a_level,
    p2.y,
    p1.a
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY sum_ax, str_concat;

SELECT
    p1.a + p2.x AS sum_ax,
    p1.c || '-' || p2.y AS str_concat,
    p2.z - p1.b AS date_diff,
    COALESCE(p1.d, 0) * 2.5 AS scaled_d,
    CASE WHEN p1.a > 10 THEN 'high' ELSE 'low' END AS a_level,
    p2.y,
    p1.a
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY sum_ax, str_concat
LIMIT 6;

-- array types, subscripting, and constructors
EXPLAIN (COSTS OFF)
SELECT
    p1.arr[1] AS first_elem,
    p2.arr[2] AS second_elem,
    p1.arr || p2.arr AS concatenated_arr,
    ARRAY[p1.a, p2.x] AS constructed_arr,
    p1.c,
    p2.y
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY first_elem, second_elem;

SELECT
    p1.arr[1] AS first_elem,
    p2.arr[2] AS second_elem,
    p1.arr || p2.arr AS concatenated_arr,
    ARRAY[p1.a, p2.x] AS constructed_arr,
    p1.c,
    p2.y
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY first_elem, second_elem
LIMIT 5;

-- composite types and row constructors
EXPLAIN (COSTS OFF)
SELECT
    (p1.comp).num AS comp_num,
    (p1.comp).str AS comp_str,
    ROW(p1.c, p2.x, p1.d) AS constructed_row,
    p2.y,
    p1.a
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY comp_num NULLS LAST, p2.y;

SELECT
    (p1.comp).num AS comp_num,
    (p1.comp).str AS comp_str,
    ROW(p1.c, p2.x, p1.d) AS constructed_row,
    p2.y,
    p1.a
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY comp_num NULLS LAST, p2.y
LIMIT 6;

-- mixed target list with window functions atop Append
EXPLAIN (COSTS OFF)
SELECT
    p2.y AS col_text,
    p1.a + p2.x AS sum_ax,
    (p1.comp).str AS comp_str,
    p1.arr[1] AS first_arr_elem,
    ROW(p1.a, p2.y) AS row_ctor,
    'constant_str' AS literal_val,
    NULL::int AS null_literal,
    p1.a AS dup_col1,
    p1.a AS dup_col2,
    p2.z AS col_date,
    count(*) OVER (PARTITION BY p1.a ORDER BY p2.x) AS win_cnt
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY sum_ax, p2.y, win_cnt;

SELECT
    p2.y AS col_text,
    p1.a + p2.x AS sum_ax,
    (p1.comp).str AS comp_str,
    p1.arr[1] AS first_arr_elem,
    ROW(p1.a, p2.y) AS row_ctor,
    'constant_str' AS literal_val,
    NULL::int AS null_literal,
    p1.a AS dup_col1,
    p1.a AS dup_col2,
    p2.z AS col_date,
    count(*) OVER (PARTITION BY p1.a ORDER BY p2.x) AS win_cnt
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY sum_ax, p2.y, win_cnt
LIMIT 5;

-- grouping and aggregation atop cross-relation expressions
SET enable_nestloop = off;
EXPLAIN (COSTS OFF)
SELECT p1.c, count(*), sum(p1.a + p2.x)
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
GROUP BY p1.c
ORDER BY p1.c
LIMIT 5;

SELECT p1.c, count(*), sum(p1.a + p2.x)
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
GROUP BY p1.c
ORDER BY p1.c
LIMIT 5;
RESET enable_nestloop;

-- whole-row references
SET enable_nestloop = off;
EXPLAIN (COSTS OFF)
SELECT p1.*, p2.* FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY p1.a, p2.x, p2.y
LIMIT 2;

SELECT p1.*, p2.* FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY p1.a, p2.x, p2.y
LIMIT 2;

-- system columns
EXPLAIN (COSTS OFF)
SELECT p1.tableoid::regclass, p1.ctid, p2.ctid
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY p1.ctid, p2.ctid
LIMIT 2;

SELECT p1.tableoid::regclass, p1.ctid, p2.ctid
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY p1.ctid, p2.ctid
LIMIT 2;

-- constants-only targetlist
EXPLAIN (COSTS OFF)
SELECT 1 AS one, 'const'::text AS txt, NULL::int AS nl
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY 1, 2
LIMIT 2;

SELECT 1 AS one, 'const'::text AS txt, NULL::int AS nl
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY 1, 2
LIMIT 2;

-- DISTINCT ON with unprojected expressions
EXPLAIN (COSTS OFF)
SELECT DISTINCT ON (p1.a + p2.x) p1.c, p2.y
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY p1.a + p2.x, p1.c
LIMIT 3;

SELECT DISTINCT ON (p1.a + p2.x) p1.c, p2.y
FROM orexp_phys p1, orexp_phys2 p2
WHERE p1.a = p2.x OR p1.c = p2.y
ORDER BY p1.a + p2.x, p1.c
LIMIT 3;

-- tables with dropped columns
CREATE TABLE orexp_drop1 (c1 int, to_drop text, c2 int, to_drop2 date, c3 text);
CREATE TABLE orexp_drop2 (x1 int, to_drop text, x2 int, to_drop2 text, x3 text);
INSERT INTO orexp_drop1 SELECT g, 'drop', g*2, current_date, 'txt'||g FROM generate_series(1, 50) g;
INSERT INTO orexp_drop2 SELECT g, 'drop', g*2, 'drop', 'txt'||g FROM generate_series(1, 50) g;
ALTER TABLE orexp_drop1 DROP COLUMN to_drop;
ALTER TABLE orexp_drop1 DROP COLUMN to_drop2;
ALTER TABLE orexp_drop2 DROP COLUMN to_drop;
ALTER TABLE orexp_drop2 DROP COLUMN to_drop2;

EXPLAIN (COSTS OFF)
SELECT d1.c3, d2.x1, d1.c1, d2.x3
FROM orexp_drop1 d1, orexp_drop2 d2
WHERE d1.c1 = d2.x1 OR d1.c3 = d2.x3
ORDER BY d1.c1, d2.x1
LIMIT 3;

SELECT d1.c3, d2.x1, d1.c1, d2.x3
FROM orexp_drop1 d1, orexp_drop2 d2
WHERE d1.c1 = d2.x1 OR d1.c3 = d2.x3
ORDER BY d1.c1, d2.x1
LIMIT 3;

DROP TABLE orexp_drop1, orexp_drop2;
RESET enable_nestloop;

--
-- Cross-table filter predicates
--

-- cross-table disjunction with separate join condition
EXPLAIN (COSTS OFF)
SELECT a.id, a.val, b.val FROM orexp_a a JOIN orexp_b b ON a.id = b.x
WHERE a.val = 10 OR b.val = 300
ORDER BY a.id, a.val, b.val;

SELECT a.id, a.val, b.val FROM orexp_a a JOIN orexp_b b ON a.id = b.x
WHERE a.val = 10 OR b.val = 300
ORDER BY a.id, a.val, b.val;

--
-- Aggregation and grouping
--

-- GROUP BY over an expanded join
EXPLAIN (COSTS OFF)
SELECT a.id, count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
ORDER BY a.id;

SELECT a.id, count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
ORDER BY a.id;

-- zero-column target
EXPLAIN (COSTS OFF)
SELECT count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y;

SELECT count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y;

-- non-aggregate HAVING clause
EXPLAIN (COSTS OFF)
SELECT a.id, count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
HAVING a.id > 1
ORDER BY a.id;

-- aggregate HAVING clause
EXPLAIN (COSTS OFF)
SELECT a.id, count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
HAVING count(*) > 1
ORDER BY a.id;

-- grouping sets
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, grouping (a.id), count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY GROUPING SETS ((a.id), (b.x), ())
ORDER BY 1, 2, 3, 4;

SELECT a.id, b.x, grouping (a.id), count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY GROUPING SETS ((a.id), (b.x), ())
ORDER BY 1, 2, 3, 4;

--
-- Subqueries, views, and derived tables
--

-- pulled-up simple subquery
EXPLAIN (COSTS OFF)
SELECT x.id, b.y FROM (SELECT id, val FROM orexp_a) x, orexp_b b
WHERE x.id = b.x OR x.val = b.y
ORDER BY 1, 2;

SELECT x.id, b.y FROM (SELECT id, val FROM orexp_a) x, orexp_b b
WHERE x.id = b.x OR x.val = b.y
ORDER BY 1, 2;

-- pulled-up subquery containing outer join
CREATE TABLE orexp_c (id int, val int);
CREATE TABLE orexp_d (id int, val int);
INSERT INTO orexp_c VALUES (1, 10), (2, 20), (3, NULL);
INSERT INTO orexp_d VALUES (1, 100), (3, 300), (5, 500);

EXPLAIN (COSTS OFF)
SELECT x.cid, b.y FROM (SELECT c.id AS cid, c.val AS cval, d.val + 1 AS dval
                        FROM orexp_c c LEFT JOIN orexp_d d ON c.id = d.id) x,
                      orexp_b b
WHERE x.dval = b.y OR x.cid = b.x
ORDER BY 1, 2;

SELECT x.cid, b.y FROM (SELECT c.id AS cid, c.val AS cval, d.val + 1 AS dval
                        FROM orexp_c c LEFT JOIN orexp_d d ON c.id = d.id) x,
                      orexp_b b
WHERE x.dval = b.y OR x.cid = b.x
ORDER BY 1, 2;

-- view expanded into pulled-up subquery
CREATE VIEW orexp_v AS
  SELECT a.id AS aid, a.val AS aval, b.x AS bx FROM orexp_a a, orexp_b b;

EXPLAIN (COSTS OFF)
SELECT v.aid, b.y FROM orexp_v v, orexp_b b
WHERE v.aval = b.y OR v.bx = b.y
ORDER BY 1, 2;

SELECT v.aid, b.y FROM orexp_v v, orexp_b b
WHERE v.aval = b.y OR v.bx = b.y
ORDER BY 1, 2;

-- non-table RTE_RESULT derived table
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b, (SELECT 1 AS one) x
WHERE a.id = b.x OR a.id = b.y
ORDER BY 1, 2;

--
-- Non-table range table entries
--

-- VALUES RTE with nulls
EXPLAIN (COSTS OFF)
SELECT v.a, b.y FROM (VALUES (1, 99), (2, NULL), (3, 3), (NULL, 4)) v (a, b), orexp_b b
WHERE v.a = b.x OR v.b = b.y
ORDER BY 1, 2;

SELECT v.a, b.y FROM (VALUES (1, 99), (2, NULL), (3, 3), (NULL, 4)) v (a, b), orexp_b b
WHERE v.a = b.x OR v.b = b.y
ORDER BY 1, 2;

-- set-returning function RTEs
EXPLAIN (COSTS OFF)
SELECT g, b.y FROM generate_series(1, 4) g, orexp_b b
WHERE g = b.x OR g = b.y
ORDER BY 1, 2;

SELECT g, b.y FROM generate_series(1, 4) g, orexp_b b
WHERE g = b.x OR g = b.y
ORDER BY 1, 2;

-- table functions
EXPLAIN (COSTS OFF)
SELECT jt.a, b.y FROM json_table('{"a": 1}'::jsonb, '$' COLUMNS (a int PATH '$.a')) jt, orexp_b b
WHERE jt.a = b.x OR jt.a = b.y;

SELECT jt.a, b.y FROM json_table('{"a": 1}'::jsonb, '$' COLUMNS (a int PATH '$.a')) jt, orexp_b b
WHERE jt.a = b.x OR jt.a = b.y;

--
-- Pseudoconstant predicates
--

-- arm with pseudoconstant qual
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR (b.y = 2 AND now() > '2000-01-01 00:00:00'::timestamp);

SELECT count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR (b.y = 2 AND now() > '2000-01-01 00:00:00'::timestamp);

-- pseudoconstant arm alongside subquery arm
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (b.y = 2 AND now() > '2000-01-01 00:00:00'::timestamp)
   OR (a.id = b.x AND a.val > (SELECT count(*) FROM orexp_b));

SELECT count(*) FROM orexp_a a, orexp_b b
WHERE (b.y = 2 AND now() > '2000-01-01 00:00:00'::timestamp)
   OR (a.id = b.x AND a.val > (SELECT count(*) FROM orexp_b));

--
-- Outer joins
--

-- FULL JOIN with strict WHERE quals
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.id = b.y OR b.x = a.id
ORDER BY 1, 2;

SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.id = b.y OR b.x = a.id
ORDER BY 1, 2;

-- LEFT JOIN with cross-relation disjunction in WHERE
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a LEFT JOIN orexp_b b ON (a.id = b.x OR a.val = b.y)
WHERE b.val = 100 OR a.id = 1
ORDER BY 1, 2, 3;

SELECT a.id, b.x, b.y FROM orexp_a a LEFT JOIN orexp_b b ON (a.id = b.x OR a.val = b.y)
WHERE b.val = 100 OR a.id = 1
ORDER BY 1, 2, 3;

--
-- Queries ineligible for OR expansion
--

-- volatile set-returning function
EXPLAIN (COSTS OFF)
SELECT g, b.y FROM generate_series(1, (random() * 3)::int) g, orexp_b b
WHERE g = b.x OR g = b.y;

-- volatile VALUES list
EXPLAIN (COSTS OFF)
SELECT v.a, b.y FROM (VALUES ((random() * 3)::int)) v (a), orexp_b b
WHERE v.a = b.x OR v.a = b.y;

-- volatile JSON_TABLE expressions
EXPLAIN (COSTS OFF)
SELECT jt.a, b.y FROM json_table(('{"a":' || (random() * 2)::int || '}')::jsonb,
                                 '$' COLUMNS (a int PATH '$.a')) jt, orexp_b b
WHERE jt.a = b.x OR jt.a = b.y;

-- volatile function in WHERE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR (a.id = b.y AND random() < 0.5);

-- correlated SubPlan in WHERE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y)
  AND a.val > (SELECT max(b2.val) FROM orexp_b b2 WHERE b2.x = a.id);

-- TABLESAMPLE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b TABLESAMPLE SYSTEM (50)
WHERE a.id = b.x OR a.id = b.y;

-- unreduced FULL JOIN
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.val IS NULL OR b.val IS NULL;

-- lateral reference
EXPLAIN (COSTS OFF)
SELECT a.id, l.ly FROM orexp_a a,
       LATERAL (SELECT b.y AS ly FROM orexp_b b WHERE b.x = a.id OR b.y = a.val) l
WHERE a.id = l.ly OR a.val = 1;

-- unflattened subquery with grouping
EXPLAIN (COSTS OFF)
SELECT a.id, s.n FROM orexp_a a,
       (SELECT x AS sx, count(*) AS n FROM orexp_b GROUP BY x) s
WHERE a.id = s.sx OR a.val = s.n;

-- append relation from UNION ALL
EXPLAIN (COSTS OFF)
SELECT u.id, a.val FROM (SELECT x AS id, val FROM orexp_b
                         UNION ALL
                         SELECT id, val FROM orexp_a) u, orexp_a a
WHERE u.val = a.val OR u.id = a.id;

-- partitioned table
CREATE TABLE orexp_part (id int, d int) PARTITION BY RANGE (d);
CREATE TABLE orexp_part1 PARTITION OF orexp_part FOR VALUES FROM (0) TO (10);
CREATE TABLE orexp_part2 PARTITION OF orexp_part FOR VALUES FROM (10) TO (20);
INSERT INTO orexp_part VALUES (1, 5), (2, 15);

EXPLAIN (COSTS OFF)
SELECT a.id, p.d FROM orexp_a a, orexp_part p
WHERE a.id = p.id OR a.val = p.d;

-- table inheritance
CREATE TABLE orexp_inh_parent (id int, val int);
CREATE TABLE orexp_inh_child () INHERITS (orexp_inh_parent);
INSERT INTO orexp_inh_parent VALUES (1, 10);
INSERT INTO orexp_inh_child VALUES (2, 20);

EXPLAIN (COSTS OFF)
SELECT a.id, p.val FROM orexp_a a, orexp_inh_parent p
WHERE a.id = p.id OR a.val = p.val;

--
-- Permitted sublinks
--

-- uncorrelated scalar sublink
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y)
  AND a.val > (SELECT min(val) FROM orexp_b WHERE val > 150)
ORDER BY 1, 2;

SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y)
  AND a.val > (SELECT min(val) FROM orexp_b WHERE val > 150)
ORDER BY 1, 2;

--
-- Multiple OR clauses in WHERE
--

EXPLAIN (COSTS OFF)
SELECT a.id, b.val FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y) AND (a.val = b.val OR a.val = b.x)
ORDER BY 1, 2;

SELECT a.id, b.val FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y) AND (a.val = b.val OR a.val = b.x)
ORDER BY 1, 2;

--
-- Control via or_expansion_limit
--

-- arm count exceeds limit
SET or_expansion_limit = 2;
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY 1, 2;

-- arm count within limit
SET or_expansion_limit = 3;
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY 1, 2;

SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY 1, 2;

--
-- Clean up
--
RESET or_expansion_limit;
DROP VIEW orexp_v;
DROP TABLE orexp_c, orexp_d;
DROP TABLE orexp_part;
DROP TABLE orexp_inh_parent CASCADE;
DROP TABLE orexp_phys, orexp_phys2;
DROP TYPE orexp_c_type;
DROP TABLE orexp_a, orexp_b;
