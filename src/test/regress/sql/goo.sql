--
-- GOO (Greedy Operator Ordering) Join Search Tests
--
-- This test suite validates the GOO join ordering algorithm and verifies
-- correct behavior for various query patterns.
--

-- Create test tables with various sizes and join patterns
CREATE TEMP TABLE t1 (a int, b int);
CREATE TEMP TABLE t2 (a int, c int);
CREATE TEMP TABLE t3 (b int, d int);
CREATE TEMP TABLE t4 (c int, e int);
CREATE TEMP TABLE t5 (d int, f int);
CREATE TEMP TABLE t6 (e int, g int);
CREATE TEMP TABLE t7 (f int, h int);
CREATE TEMP TABLE t8 (g int, i int);
CREATE TEMP TABLE t9 (h int, j int);
CREATE TEMP TABLE t10 (i int, k int);
CREATE TEMP TABLE t11 (j int, l int);
CREATE TEMP TABLE t12 (k int, m int);
CREATE TEMP TABLE t13 (l int, n int);
CREATE TEMP TABLE t14 (m int, o int);
CREATE TEMP TABLE t15 (n int, p int);
CREATE TEMP TABLE t16 (o int, q int);
CREATE TEMP TABLE t17 (p int, r int);
CREATE TEMP TABLE t18 (q int, s int);

-- Populate with small amount of data
INSERT INTO t1 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t2 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t3 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t4 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t5 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t6 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t7 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t8 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t9 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t10 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t11 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t12 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t13 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t14 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t15 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t16 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t17 SELECT i, i FROM generate_series(1,10) i;
INSERT INTO t18 SELECT i, i FROM generate_series(1,10) i;

ANALYZE;

-- Verify combined is the default strategy.
SHOW goo_greedy_strategy;

--
-- Basic 3-way join (sanity check)
--
SET enable_goo_join_search = on;
SET geqo_threshold = 2;

EXPLAIN (COSTS OFF)
SELECT count(*)
FROM (VALUES (1),(2)) AS a(x)
JOIN (VALUES (1),(2)) AS b(x) USING (x)
JOIN (VALUES (1),(3)) AS c(x) USING (x);

SELECT count(*)
FROM (VALUES (1),(2)) AS a(x)
JOIN (VALUES (1),(2)) AS b(x) USING (x)
JOIN (VALUES (1),(3)) AS c(x) USING (x);

--
-- Disconnected graph (Cartesian products required)
--
-- This tests GOO's ability to handle queries where some relations
-- have no join clauses connecting them. GOO should allow Cartesian
-- products when no clause-connected joins are available.
--
EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1, t2, t5
WHERE t1.a = 1 AND t2.c = 2 AND t5.f = 3;

SELECT count(*)
FROM t1, t2, t5
WHERE t1.a = 1 AND t2.c = 2 AND t5.f = 3;

--
-- Star schema (fact table with multiple dimension tables)
--
-- Test GOO with a typical star schema join pattern.
--
CREATE TEMP TABLE fact (id int, dim1_id int, dim2_id int, dim3_id int, dim4_id int, value int);
CREATE TEMP TABLE dim1 (id int, name text);
CREATE TEMP TABLE dim2 (id int, name text);
CREATE TEMP TABLE dim3 (id int, name text);
CREATE TEMP TABLE dim4 (id int, name text);

INSERT INTO fact SELECT i, i, i, i, i, i FROM generate_series(1,100) i;
INSERT INTO dim1 SELECT i, 'dim1_'||i FROM generate_series(1,10) i;
INSERT INTO dim2 SELECT i, 'dim2_'||i FROM generate_series(1,10) i;
INSERT INTO dim3 SELECT i, 'dim3_'||i FROM generate_series(1,10) i;
INSERT INTO dim4 SELECT i, 'dim4_'||i FROM generate_series(1,10) i;

ANALYZE;

EXPLAIN (COSTS OFF)
SELECT count(*)
FROM fact
JOIN dim1 ON fact.dim1_id = dim1.id
JOIN dim2 ON fact.dim2_id = dim2.id
JOIN dim3 ON fact.dim3_id = dim3.id
JOIN dim4 ON fact.dim4_id = dim4.id
WHERE dim1.id < 5;

--
-- Long join chain
--
-- Tests GOO with a large join involving 15 relations.
--
SET geqo_threshold = 8;

EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1
JOIN t2 ON t1.a = t2.a
JOIN t3 ON t1.b = t3.b
JOIN t4 ON t2.c = t4.c
JOIN t5 ON t3.d = t5.d
JOIN t6 ON t4.e = t6.e
JOIN t7 ON t5.f = t7.f
JOIN t8 ON t6.g = t8.g
JOIN t9 ON t7.h = t9.h
JOIN t10 ON t8.i = t10.i
JOIN t11 ON t9.j = t11.j
JOIN t12 ON t10.k = t12.k
JOIN t13 ON t11.l = t13.l
JOIN t14 ON t12.m = t14.m
JOIN t15 ON t13.n = t15.n;

-- Execute to verify correctness
SELECT count(*)
FROM t1
JOIN t2 ON t1.a = t2.a
JOIN t3 ON t1.b = t3.b
JOIN t4 ON t2.c = t4.c
JOIN t5 ON t3.d = t5.d
JOIN t6 ON t4.e = t6.e
JOIN t7 ON t5.f = t7.f
JOIN t8 ON t6.g = t8.g
JOIN t9 ON t7.h = t9.h
JOIN t10 ON t8.i = t10.i
JOIN t11 ON t9.j = t11.j
JOIN t12 ON t10.k = t12.k
JOIN t13 ON t11.l = t13.l
JOIN t14 ON t12.m = t14.m
JOIN t15 ON t13.n = t15.n;

--
-- Bushy tree support
--
-- Verify that GOO can produce bushy join trees, not just left-deep or right-deep.
-- With appropriate cost model, GOO should join (t1,t2) and (t3,t4) first,
-- then join those results (bushy tree).
--
SET geqo_threshold = 4;

EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1, t2, t3, t4
WHERE t1.a = t2.a
  AND t3.b = t4.c
  AND t1.a = t3.b;

--
-- Compare GOO vs standard join search
--
-- Run the same query with GOO and standard join search to verify both
-- produce valid plans. Results should be identical even if plans differ.
--
SET enable_goo_join_search = on;
PREPARE goo_plan AS
SELECT count(*)
FROM t1 JOIN t2 ON t1.a = t2.a
JOIN t3 ON t1.b = t3.b
JOIN t4 ON t2.c = t4.c
JOIN t5 ON t3.d = t5.d
JOIN t6 ON t4.e = t6.e;

EXECUTE goo_plan;

SET enable_goo_join_search = off;
SET geqo_threshold = default;
PREPARE standard_plan AS
SELECT count(*)
FROM t1 JOIN t2 ON t1.a = t2.a
JOIN t3 ON t1.b = t3.b
JOIN t4 ON t2.c = t4.c
JOIN t5 ON t3.d = t5.d
JOIN t6 ON t4.e = t6.e;

EXECUTE standard_plan;

-- Results should match
EXECUTE goo_plan;
EXECUTE standard_plan;

--
-- Large join (18 relations)
--
-- Test GOO with a large number of relations.
--
SET enable_goo_join_search = on;
SET geqo_threshold = 10;

EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1
JOIN t2 ON t1.a = t2.a
JOIN t3 ON t1.b = t3.b
JOIN t4 ON t2.c = t4.c
JOIN t5 ON t3.d = t5.d
JOIN t6 ON t4.e = t6.e
JOIN t7 ON t5.f = t7.f
JOIN t8 ON t6.g = t8.g
JOIN t9 ON t7.h = t9.h
JOIN t10 ON t8.i = t10.i
JOIN t11 ON t9.j = t11.j
JOIN t12 ON t10.k = t12.k
JOIN t13 ON t11.l = t13.l
JOIN t14 ON t12.m = t14.m
JOIN t15 ON t13.n = t15.n
JOIN t16 ON t14.o = t16.o
JOIN t17 ON t15.p = t17.p
JOIN t18 ON t16.q = t18.q;

--
-- Mixed connected and disconnected components
--
-- Query with two connected components that need a Cartesian product between them.
--
EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1 JOIN t2 ON t1.a = t2.a,
     t5 JOIN t6 ON t5.f = t6.e
WHERE t1.a < 5 AND t5.d < 3;

--
-- Outer joins
--
-- Verify GOO handles outer joins correctly (respects join order restrictions)
--
EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1
LEFT JOIN t2 ON t1.a = t2.a
LEFT JOIN t3 ON t2.a = t3.b
LEFT JOIN t4 ON t3.d = t4.c;

--
-- Complete Cartesian products (disconnected graphs)
--
-- Test GOO's ability to handle queries with no join clauses at all.
--
SET enable_goo_join_search = on;
SET geqo_threshold = 2;

EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1, t2;

SELECT count(*)
FROM t1, t2;

--
-- Join order restrictions with FULL OUTER JOIN
--
-- FULL OUTER JOIN creates strong ordering constraints that GOO must respect
--
EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1
FULL OUTER JOIN t2 ON t1.a = t2.a
FULL OUTER JOIN t3 ON t2.a = t3.b;

--
-- Self-join handling
--
-- Test GOO with the same table appearing multiple times. GOO must correctly
-- handle self-joins that were not removed by Self-Join Elimination.
--
EXPLAIN (COSTS OFF)
SELECT count(*)
FROM t1 a
JOIN t1 b ON a.a = b.a
JOIN t2 c ON b.b = c.c;

--
-- Complex bushy tree pattern
--
-- Create a query that naturally leads to bushy tree: multiple independent
-- join chains that need to be combined
--
CREATE TEMP TABLE chain1a (id int, val int);
CREATE TEMP TABLE chain1b (id int, val int);
CREATE TEMP TABLE chain1c (id int, val int);
CREATE TEMP TABLE chain2a (id int, val int);
CREATE TEMP TABLE chain2b (id int, val int);
CREATE TEMP TABLE chain2c (id int, val int);

INSERT INTO chain1a SELECT i, i FROM generate_series(1,100) i;
INSERT INTO chain1b SELECT i, i FROM generate_series(1,100) i;
INSERT INTO chain1c SELECT i, i FROM generate_series(1,100) i;
INSERT INTO chain2a SELECT i, i FROM generate_series(1,100) i;
INSERT INTO chain2b SELECT i, i FROM generate_series(1,100) i;
INSERT INTO chain2c SELECT i, i FROM generate_series(1,100) i;

ANALYZE;

EXPLAIN (COSTS OFF)
SELECT count(*)
FROM chain1a
JOIN chain1b ON chain1a.id = chain1b.id
JOIN chain1c ON chain1b.val = chain1c.id
JOIN chain2a ON chain1a.val = chain2a.id  -- Cross-chain join
JOIN chain2b ON chain2a.val = chain2b.id
JOIN chain2c ON chain2b.val = chain2c.id;

--
-- Eager aggregation with GOO join search
-- Ensure grouped_rel handling when eager aggregation is enabled.
--
SET enable_eager_aggregate = on;
SET min_eager_agg_group_size = 0;

CREATE TEMP TABLE center_tbl (id int PRIMARY KEY);
CREATE TEMP TABLE arm1_tbl (center_id int, payload int);
CREATE TEMP TABLE arm2_tbl (center_id int, payload int);

INSERT INTO center_tbl SELECT i FROM generate_series(1, 10) i;
INSERT INTO arm1_tbl SELECT i%10 + 1, i FROM generate_series(1, 1000) i;
INSERT INTO arm2_tbl SELECT i%10 + 1, i FROM generate_series(1, 1000) i;

ANALYZE center_tbl;
ANALYZE arm1_tbl;
ANALYZE arm2_tbl;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT c.id, count(*)
FROM center_tbl c
JOIN arm1_tbl a1 ON c.id = a1.center_id
JOIN arm2_tbl a2 ON c.id = a2.center_id
GROUP BY c.id;

SELECT c.id, count(*)
FROM center_tbl c
JOIN arm1_tbl a1 ON c.id = a1.center_id
JOIN arm2_tbl a2 ON c.id = a2.center_id
GROUP BY c.id;

RESET min_eager_agg_group_size;
RESET enable_eager_aggregate;

-- Cleanup
DEALLOCATE goo_plan;
DEALLOCATE standard_plan;

RESET geqo_threshold;
RESET enable_goo_join_search;
RESET goo_greedy_strategy;