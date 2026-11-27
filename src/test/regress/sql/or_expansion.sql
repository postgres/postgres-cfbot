--
-- Tests for OR-clause expansion into UNION ALL Append paths
--

-- Initial GUC check
SHOW or_expansion_limit;

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

--
-- Basic OR-clause expansion
--

-- Disjunction on join keys across two tables
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

-- Compare with expansion disabled to confirm semantic equivalence
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
-- Three-valued logic and duplicate elimination
--
-- Row (3, 30) from orexp_a and (3, 3, 300) from orexp_b satisfies both arms:
-- a.id = b.x (3 = 3) AND a.id = b.y (3 = 3).
-- The (a.id = b.x) IS NOT TRUE exclusion condition in the second arm ensures
-- it is emitted exactly once. Rows with NULLs are correctly handled.
EXPLAIN (COSTS OFF)
SELECT a.id, a.val, b.x, b.y, b.val
FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y) AND (a.val IS NOT NULL OR b.val > 200)
ORDER BY a.id, b.x, b.y;

SELECT a.id, a.val, b.x, b.y, b.val
FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y) AND (a.val IS NOT NULL OR b.val > 200)
ORDER BY a.id, b.x, b.y;

--
-- Multi-arm expansion (3 arms)
--
-- Arm 2 excludes Arm 0; Arm 3 excludes both Arm 0 and Arm 1.
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y, b.val FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY a.id, b.x, b.y, b.val;

SELECT a.id, b.x, b.y, b.val FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY a.id, b.x, b.y, b.val;

--
-- Target list projection and expressions
--

-- Column order differs from scan order and includes cross-table expressions
EXPLAIN (COSTS OFF)
SELECT b.y, a.id + b.x AS sum_id_x, a.val, 'tag' AS literal
FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY sum_id_x, b.y;

SELECT b.y, a.id + b.x AS sum_id_x, a.val, 'tag' AS literal
FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY sum_id_x, b.y;

-- Whole-row references
EXPLAIN (COSTS OFF)
SELECT a.*, b.* FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

--
-- Upper planner nodes and cost propagation
--

-- Fast-start with LIMIT: tuple_fraction and limit_tuples passed to arms
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
LIMIT 2;

-- GROUP BY and aggregate atop expanded join
EXPLAIN (COSTS OFF)
SELECT a.id, count(*), sum(b.val) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
ORDER BY a.id;

SELECT a.id, count(*), sum(b.val) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
GROUP BY a.id
ORDER BY a.id;

-- Zero-column target (count(*))
EXPLAIN (COSTS OFF)
SELECT count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y;

SELECT count(*) FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y;

--
-- Outer joins
--

-- LEFT JOIN where WHERE clause contains cross-relation disjunction
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a LEFT JOIN orexp_b b ON a.id = b.x
WHERE b.val = 100 OR a.id = 2
ORDER BY 1, 2, 3;

SELECT a.id, b.x, b.y FROM orexp_a a LEFT JOIN orexp_b b ON a.id = b.x
WHERE b.val = 100 OR a.id = 2
ORDER BY 1, 2, 3;

-- FULL JOIN reduced to INNER join by strict WHERE quals
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.id = b.y OR b.x = a.id
ORDER BY 1, 2;

SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.id = b.y OR b.x = a.id
ORDER BY 1, 2;

--
-- Subqueries and derived tables
--

-- Pulled-up subquery participating in disjunction
EXPLAIN (COSTS OFF)
SELECT sub.id, b.y FROM (SELECT id, val FROM orexp_a) sub, orexp_b b
WHERE sub.id = b.x OR sub.val = b.y
ORDER BY 1, 2;

SELECT sub.id, b.y FROM (SELECT id, val FROM orexp_a) sub, orexp_b b
WHERE sub.id = b.x OR sub.val = b.y
ORDER BY 1, 2;

-- Derived table from VALUES list
EXPLAIN (COSTS OFF)
SELECT v.a, b.y FROM (VALUES (1, 99), (2, NULL), (3, 3)) v(a, b), orexp_b b
WHERE v.a = b.x OR v.b = b.y
ORDER BY 1, 2;

SELECT v.a, b.y FROM (VALUES (1, 99), (2, NULL), (3, 3)) v(a, b), orexp_b b
WHERE v.a = b.x OR v.b = b.y
ORDER BY 1, 2;

--
-- Partitioned and inheritance tables
--

-- Partitioned table
CREATE TABLE orexp_part (id int, d int) PARTITION BY RANGE (d);
CREATE TABLE orexp_part1 PARTITION OF orexp_part FOR VALUES FROM (0) TO (10);
CREATE TABLE orexp_part2 PARTITION OF orexp_part FOR VALUES FROM (10) TO (20);
INSERT INTO orexp_part VALUES (1, 5), (2, 15);

EXPLAIN (COSTS OFF)
SELECT a.id, p.d FROM orexp_a a, orexp_part p
WHERE a.id = p.id OR a.val = p.d;

DROP TABLE orexp_part;

-- Table inheritance
CREATE TABLE orexp_inh_parent (id int, val int);
CREATE TABLE orexp_inh_child () INHERITS (orexp_inh_parent);
INSERT INTO orexp_inh_parent VALUES (1, 10);
INSERT INTO orexp_inh_child VALUES (2, 20);

EXPLAIN (COSTS OFF)
SELECT a.id, p.val FROM orexp_a a, orexp_inh_parent p
WHERE a.id = p.id OR a.val = p.val;

DROP TABLE orexp_inh_parent CASCADE;

--
-- Parallel Append
--

-- Encourage parallel query and parallel append
BEGIN;
SET LOCAL parallel_setup_cost = 0;
SET LOCAL parallel_tuple_cost = 0;
SET LOCAL min_parallel_table_scan_size = 0;
SET LOCAL max_parallel_workers_per_gather = 2;
SET LOCAL enable_parallel_append = on;

EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

SELECT a.id, b.x, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y
ORDER BY a.id, b.x, b.y;

COMMIT;

--
-- Pulled-up UNION ALL append relation
--

EXPLAIN (COSTS OFF)
SELECT u.id, a.val FROM (SELECT x AS id, val FROM orexp_b
                         UNION ALL
                         SELECT id, val FROM orexp_a) u, orexp_a a
WHERE u.val = a.val OR u.id = a.id
ORDER BY u.id, a.val;

SELECT u.id, a.val FROM (SELECT x AS id, val FROM orexp_b
                         UNION ALL
                         SELECT id, val FROM orexp_a) u, orexp_a a
WHERE u.val = a.val OR u.id = a.id
ORDER BY u.id, a.val;

--
-- LATERAL reference
--

EXPLAIN (COSTS OFF)
SELECT a.id, l.ly FROM orexp_a a,
       LATERAL (SELECT b.y AS ly FROM orexp_b b WHERE b.x = a.id OR b.y = a.val) l
WHERE a.id = l.ly OR a.val = 1
ORDER BY a.id, l.ly;

SELECT a.id, l.ly FROM orexp_a a,
       LATERAL (SELECT b.y AS ly FROM orexp_b b WHERE b.x = a.id OR b.y = a.val) l
WHERE a.id = l.ly OR a.val = 1
ORDER BY a.id, l.ly;

--
-- Ineligible queries (safety gates correctly bypass expansion)
--

-- Volatile function in WHERE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR (a.id = b.y AND random() < 0.5);

-- Correlated SubPlan in WHERE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b
WHERE (a.id = b.x OR a.id = b.y)
  AND a.val > (SELECT max(b2.val) FROM orexp_b b2 WHERE b2.x = a.id);

-- TABLESAMPLE
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a, orexp_b b TABLESAMPLE SYSTEM (50)
WHERE a.id = b.x OR a.id = b.y;

-- Unreduced FULL JOIN
EXPLAIN (COSTS OFF)
SELECT a.id, b.y FROM orexp_a a FULL JOIN orexp_b b ON a.id = b.x
WHERE a.val IS NULL OR b.val IS NULL;

-- Active unflattened subquery with grouping
EXPLAIN (COSTS OFF)
SELECT a.id, s.n FROM orexp_a a,
       (SELECT x AS sx, count(*) AS n FROM orexp_b GROUP BY x) s
WHERE a.id = s.sx OR a.val = s.n;

--
-- GUC limit control
--

-- Arm count exceeds limit: expansion skipped
SET or_expansion_limit = 2;
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y, b.val FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY a.id, b.x, b.y, b.val;

-- Arm count within limit: expansion applied
SET or_expansion_limit = 3;
EXPLAIN (COSTS OFF)
SELECT a.id, b.x, b.y, b.val FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY a.id, b.x, b.y, b.val;

SELECT a.id, b.x, b.y, b.val FROM orexp_a a, orexp_b b
WHERE a.id = b.x OR a.id = b.y OR a.val = b.val
ORDER BY a.id, b.x, b.y, b.val;

RESET or_expansion_limit;

--
-- Clean up
--
DROP TABLE orexp_a, orexp_b;
