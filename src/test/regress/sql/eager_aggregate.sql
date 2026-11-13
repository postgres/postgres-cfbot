--
-- EAGER AGGREGATION
-- Test we can push aggregation down below join
--

CREATE TABLE eager_agg_t1 (a int, b int, c double precision);
CREATE TABLE eager_agg_t2 (a int, b int, c double precision);
CREATE TABLE eager_agg_t3 (a int, b int, c double precision);

INSERT INTO eager_agg_t1 SELECT i, i, i FROM generate_series(1, 1000) i;
INSERT INTO eager_agg_t2 SELECT i, i%10, i FROM generate_series(1, 1000) i;
INSERT INTO eager_agg_t3 SELECT i%10, i%10, i FROM generate_series(1, 1000) i;

ANALYZE eager_agg_t1;
ANALYZE eager_agg_t2;
ANALYZE eager_agg_t3;


--
-- Test eager aggregation over base rel
--

-- Perform scan of a table, aggregate the result, join it to the other table
-- and finalize the aggregation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

-- Produce results with sorting aggregation
SET enable_hashagg TO off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

RESET enable_hashagg;


--
-- Test eager aggregation over join rel
--

-- Perform join of tables, aggregate the result, join it to the other table
-- and finalize the aggregation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c + t3.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
  JOIN eager_agg_t3 t3 ON t2.a = t3.a
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c + t3.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
  JOIN eager_agg_t3 t3 ON t2.a = t3.a
GROUP BY t1.a ORDER BY t1.a;

-- Produce results with sorting aggregation
SET enable_hashagg TO off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c + t3.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
  JOIN eager_agg_t3 t3 ON t2.a = t3.a
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c + t3.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
  JOIN eager_agg_t3 t3 ON t2.a = t3.a
GROUP BY t1.a ORDER BY t1.a;

RESET enable_hashagg;


--
-- Test that eager aggregation works for outer join
--

-- Ensure aggregation can be pushed down to the non-nullable side
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  RIGHT JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  RIGHT JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

-- Ensure aggregation cannot be pushed down to the nullable side
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t2.b, avg(t2.c)
  FROM eager_agg_t1 t1
  LEFT JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t2.b ORDER BY t2.b;

SELECT t2.b, avg(t2.c)
  FROM eager_agg_t1 t1
  LEFT JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t2.b ORDER BY t2.b;


--
-- Test that eager aggregation works for parallel plans
--

SET parallel_setup_cost=0;
SET parallel_tuple_cost=0;
SET min_parallel_table_scan_size=0;
SET max_parallel_workers_per_gather=4;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;

--
-- Test eager aggregation with GEQO
--

SET geqo = on;
SET geqo_threshold = 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

RESET geqo;
RESET geqo_threshold;

-- Ensure eager aggregation is not applied because random() is a volatile
-- function
EXPLAIN (COSTS OFF)
SELECT t1.a, avg(t2.c + random())
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

EXPLAIN (COSTS OFF)
SELECT t1.a, avg(t2.c) FILTER (WHERE random() > 0.5)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b
GROUP BY t1.a ORDER BY t1.a;

-- Ensure eager aggregation is not applied below a volatile join clause
EXPLAIN (COSTS OFF)
SELECT t1.a, avg(t2.c)
  FROM eager_agg_t1 t1
  JOIN eager_agg_t2 t2 ON t1.b = t2.b AND t1.c > t2.b * random()
GROUP BY t1.a ORDER BY t1.a;

-- Eager aggregation must not push a partial aggregate onto the inner side of a
-- SEMI or ANTI join
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t2.b, count(*)
  FROM eager_agg_t2 t2
  WHERE NOT EXISTS (SELECT 1 FROM eager_agg_t3 t3 WHERE t3.a = t2.a)
GROUP BY t2.b ORDER BY t2.b;

SELECT t2.b, count(*)
  FROM eager_agg_t2 t2
  WHERE NOT EXISTS (SELECT 1 FROM eager_agg_t3 t3 WHERE t3.a = t2.a)
GROUP BY t2.b ORDER BY t2.b;

-- Eager aggregation may still push a partial aggregate onto the outer side of
-- a SEMI or ANTI join
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t2.b, count(*)
  FROM eager_agg_t2 t2
  WHERE EXISTS (SELECT 1 FROM eager_agg_t1 t1 WHERE t1.b = t2.b)
GROUP BY t2.b ORDER BY t2.b;

SELECT t2.b, count(*)
  FROM eager_agg_t2 t2
  WHERE EXISTS (SELECT 1 FROM eager_agg_t1 t1 WHERE t1.b = t2.b)
GROUP BY t2.b ORDER BY t2.b;

-- Ensure a join key of a different type than the grouping key is not grouped
-- using the grouping key's operators
CREATE TABLE eager_agg_s1 (a int2);
CREATE TABLE eager_agg_s2 (b int4, c double precision);
INSERT INTO eager_agg_s1 VALUES (5);
INSERT INTO eager_agg_s2 SELECT 5, 1 FROM generate_series(1, 100);
INSERT INTO eager_agg_s2 SELECT 65541, 1 FROM generate_series(1, 100);
ANALYZE eager_agg_s1, eager_agg_s2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT s1.a, sum(s2.c)
  FROM eager_agg_s1 s1
  JOIN eager_agg_s2 s2 ON s1.a = s2.b
GROUP BY s1.a;

SELECT s1.a, sum(s2.c)
  FROM eager_agg_s1 s1
  JOIN eager_agg_s2 s2 ON s1.a = s2.b
GROUP BY s1.a;

DROP TABLE eager_agg_s1;
DROP TABLE eager_agg_s2;

-- Ensure the partial aggregation can use an index scan that differs in sort
-- direction from the ordering that the grouping keys took from ORDER BY
CREATE TABLE eager_agg_d1 (a int, b int, c int);
CREATE TABLE eager_agg_d2 (a int);
INSERT INTO eager_agg_d1
  SELECT i % 10, (i / 10) % 100, i FROM generate_series(1, 10000) i;
INSERT INTO eager_agg_d2 SELECT i FROM generate_series(0, 9) i;
CREATE INDEX eager_agg_d1_a_b_idx ON eager_agg_d1 (a, b);
ANALYZE eager_agg_d1, eager_agg_d2;

SET enable_hashagg TO off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT d1.a, d1.b, sum(d1.c)
  FROM eager_agg_d1 d1
  JOIN eager_agg_d2 d2 ON d1.a = d2.a
GROUP BY d1.a, d1.b ORDER BY d1.a, d1.b DESC LIMIT 5;

SELECT d1.a, d1.b, sum(d1.c)
  FROM eager_agg_d1 d1
  JOIN eager_agg_d2 d2 ON d1.a = d2.a
GROUP BY d1.a, d1.b ORDER BY d1.a, d1.b DESC LIMIT 5;

-- ... but not with enable_group_by_reordering off
SET enable_group_by_reordering TO off;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT d1.a, d1.b, sum(d1.c)
  FROM eager_agg_d1 d1
  JOIN eager_agg_d2 d2 ON d1.a = d2.a
GROUP BY d1.a, d1.b ORDER BY d1.a, d1.b DESC LIMIT 5;

RESET enable_group_by_reordering;

-- Likewise for partial aggregation below a parallel join, with an index
-- that differs from the grouping key in both sort direction and NULLS
-- ordering
DROP INDEX eager_agg_d1_a_b_idx;
CREATE INDEX eager_agg_d1_a_idx ON eager_agg_d1 (a DESC NULLS LAST);

SET enable_nestloop TO off;
SET enable_hashjoin TO off;
SET parallel_setup_cost TO 0;
SET parallel_tuple_cost TO 0;
SET min_parallel_table_scan_size TO 0;
SET min_parallel_index_scan_size TO 0;
SET max_parallel_workers_per_gather TO 2;

EXPLAIN (COSTS OFF)
SELECT d1.a, sum(d1.c)
  FROM eager_agg_d1 d1
  JOIN eager_agg_d2 d2 ON d1.a = d2.a
GROUP BY d1.a;

SELECT d1.a, sum(d1.c)
  FROM eager_agg_d1 d1
  JOIN eager_agg_d2 d2 ON d1.a = d2.a
GROUP BY d1.a ORDER BY d1.a;

-- ... but not with enable_group_by_reordering off
SET enable_group_by_reordering TO off;

EXPLAIN (COSTS OFF)
SELECT d1.a, sum(d1.c)
  FROM eager_agg_d1 d1
  JOIN eager_agg_d2 d2 ON d1.a = d2.a
GROUP BY d1.a;

RESET enable_group_by_reordering;
RESET enable_nestloop;
RESET enable_hashjoin;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET max_parallel_workers_per_gather;

RESET enable_hashagg;

DROP TABLE eager_agg_d1;
DROP TABLE eager_agg_d2;

-- An index scan in the direction that the operator class has no operator for
-- must not cause an error in the partial aggregation, though such an operator
-- class is not valid
CREATE TYPE eager_agg_color AS ENUM ('r', 'g', 'b');
CREATE FUNCTION eager_agg_color_lt(eager_agg_color, eager_agg_color)
  RETURNS bool LANGUAGE internal IMMUTABLE STRICT AS 'enum_lt';
CREATE FUNCTION eager_agg_color_eq(eager_agg_color, eager_agg_color)
  RETURNS bool LANGUAGE internal IMMUTABLE STRICT AS 'enum_eq';
CREATE FUNCTION eager_agg_color_cmp(eager_agg_color, eager_agg_color)
  RETURNS int LANGUAGE internal IMMUTABLE STRICT AS 'enum_cmp';
CREATE OPERATOR <<< (LEFTARG = eager_agg_color, RIGHTARG = eager_agg_color,
  FUNCTION = eager_agg_color_lt);
CREATE OPERATOR === (LEFTARG = eager_agg_color, RIGHTARG = eager_agg_color,
  FUNCTION = eager_agg_color_eq);
-- FUNCTION 4 is needed for the grouping key to be usable in eager aggregation
CREATE OPERATOR CLASS eager_agg_color_ops DEFAULT FOR TYPE eager_agg_color
  USING btree AS
  OPERATOR 1 <<<, OPERATOR 3 ===,
  FUNCTION 1 eager_agg_color_cmp(eager_agg_color, eager_agg_color),
  FUNCTION 4 btequalimage(oid);
CREATE TABLE eager_agg_c1 (c eager_agg_color, d int, x int);
CREATE TABLE eager_agg_c2 (k int);
INSERT INTO eager_agg_c1
  SELECT (ARRAY['r', 'g', 'b'])[1 + i % 3]::eager_agg_color, i % 50, i
  FROM generate_series(1, 3000) i;
INSERT INTO eager_agg_c2 SELECT generate_series(0, 49);
CREATE INDEX ON eager_agg_c1 (c DESC) INCLUDE (d, x);
VACUUM ANALYZE eager_agg_c1, eager_agg_c2;


SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SET enable_hashagg = off;

EXPLAIN (COSTS OFF)
SELECT c1.c, sum(c1.x)
  FROM eager_agg_c1 c1
  JOIN eager_agg_c2 c2 ON c1.d = c2.k
GROUP BY c1.c;

SELECT c1.c, sum(c1.x)
  FROM eager_agg_c1 c1
  JOIN eager_agg_c2 c2 ON c1.d = c2.k
GROUP BY c1.c;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_index_scan_size;
RESET enable_hashagg;

DROP TABLE eager_agg_c1;
DROP TABLE eager_agg_c2;
DROP OPERATOR CLASS eager_agg_color_ops USING btree;
DROP OPERATOR FAMILY eager_agg_color_ops USING btree;
DROP OPERATOR <<< (eager_agg_color, eager_agg_color);
DROP OPERATOR === (eager_agg_color, eager_agg_color);
DROP FUNCTION eager_agg_color_lt(eager_agg_color, eager_agg_color);
DROP FUNCTION eager_agg_color_eq(eager_agg_color, eager_agg_color);
DROP FUNCTION eager_agg_color_cmp(eager_agg_color, eager_agg_color);
DROP TYPE eager_agg_color;

DROP TABLE eager_agg_t1;
DROP TABLE eager_agg_t2;
DROP TABLE eager_agg_t3;


--
-- Test eager aggregation for partitionwise join
--

-- Enable partitionwise aggregate, which by default is disabled.
SET enable_partitionwise_aggregate TO true;
-- Enable partitionwise join, which by default is disabled.
SET enable_partitionwise_join TO true;

CREATE TABLE eager_agg_tab1(x int, y int) PARTITION BY RANGE(x);
CREATE TABLE eager_agg_tab1_p1 PARTITION OF eager_agg_tab1 FOR VALUES FROM (0) TO (5);
CREATE TABLE eager_agg_tab1_p2 PARTITION OF eager_agg_tab1 FOR VALUES FROM (5) TO (10);
CREATE TABLE eager_agg_tab1_p3 PARTITION OF eager_agg_tab1 FOR VALUES FROM (10) TO (15);
CREATE TABLE eager_agg_tab2(x int, y int) PARTITION BY RANGE(y);
CREATE TABLE eager_agg_tab2_p1 PARTITION OF eager_agg_tab2 FOR VALUES FROM (0) TO (5);
CREATE TABLE eager_agg_tab2_p2 PARTITION OF eager_agg_tab2 FOR VALUES FROM (5) TO (10);
CREATE TABLE eager_agg_tab2_p3 PARTITION OF eager_agg_tab2 FOR VALUES FROM (10) TO (15);
INSERT INTO eager_agg_tab1 SELECT i % 15, i % 10 FROM generate_series(1, 1000) i;
INSERT INTO eager_agg_tab2 SELECT i % 10, i % 15 FROM generate_series(1, 1000) i;

ANALYZE eager_agg_tab1;
ANALYZE eager_agg_tab2;

-- When GROUP BY clause matches; full aggregation is performed for each
-- partition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t1.x ORDER BY t1.x;

-- GROUP BY having other matching key
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t2.y, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t2.y ORDER BY t2.y;

SELECT t2.y, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t2.y ORDER BY t2.y;

-- When GROUP BY clause does not match; partial aggregation is performed for
-- each partition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t2.x, sum(t1.x), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t2.x HAVING avg(t1.x) > 5 ORDER BY t2.x;

SELECT t2.x, sum(t1.x), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t2.x HAVING avg(t1.x) > 5 ORDER BY t2.x;

-- Check with eager aggregation over join rel
-- full aggregation
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y + t3.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x
  JOIN eager_agg_tab1 t3 ON t2.x = t3.x
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y + t3.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x
  JOIN eager_agg_tab1 t3 ON t2.x = t3.x
GROUP BY t1.x ORDER BY t1.x;

-- partial aggregation
SET enable_hashagg TO off;
SET max_parallel_workers_per_gather TO 0;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t3.y, sum(t2.y + t3.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x
  JOIN eager_agg_tab1 t3 ON t2.x = t3.x
GROUP BY t3.y ORDER BY t3.y;

SELECT t3.y, sum(t2.y + t3.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x
  JOIN eager_agg_tab1 t3 ON t2.x = t3.x
GROUP BY t3.y ORDER BY t3.y;

-- partial aggregation with an extra grouping key needed by a non-equality
-- join clause
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t1.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x AND t1.y < t2.y
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t1.y)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab1 t2 ON t1.x = t2.x AND t1.y < t2.y
GROUP BY t1.x ORDER BY t1.x;

-- same, with the extra grouping key nullable by an outer join below
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y)
  FROM eager_agg_tab1 t1
  LEFT JOIN eager_agg_tab1 t2 ON t1.x = t2.x AND t1.y = t2.y
  JOIN eager_agg_tab1 t3 ON t1.x = t3.x AND COALESCE(t2.y, 0) < t3.y
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y)
  FROM eager_agg_tab1 t1
  LEFT JOIN eager_agg_tab1 t2 ON t1.x = t2.x AND t1.y = t2.y
  JOIN eager_agg_tab1 t3 ON t1.x = t3.x AND COALESCE(t2.y, 0) < t3.y
GROUP BY t1.x ORDER BY t1.x;

RESET enable_hashagg;
RESET max_parallel_workers_per_gather;

-- try that with GEQO too
SET geqo = on;
SET geqo_threshold = 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t1.y), count(*)
  FROM eager_agg_tab1 t1
  JOIN eager_agg_tab2 t2 ON t1.x = t2.y
GROUP BY t1.x ORDER BY t1.x;

RESET geqo;
RESET geqo_threshold;

DROP TABLE eager_agg_tab1;
DROP TABLE eager_agg_tab2;


--
-- Test with multi-level partitioning scheme
--
CREATE TABLE eager_agg_tab_ml(x int, y int) PARTITION BY RANGE(x);
CREATE TABLE eager_agg_tab_ml_p1 PARTITION OF eager_agg_tab_ml FOR VALUES FROM (0) TO (10);
CREATE TABLE eager_agg_tab_ml_p2 PARTITION OF eager_agg_tab_ml FOR VALUES FROM (10) TO (20) PARTITION BY RANGE(x);
CREATE TABLE eager_agg_tab_ml_p2_s1 PARTITION OF eager_agg_tab_ml_p2 FOR VALUES FROM (10) TO (15);
CREATE TABLE eager_agg_tab_ml_p2_s2 PARTITION OF eager_agg_tab_ml_p2 FOR VALUES FROM (15) TO (20);
CREATE TABLE eager_agg_tab_ml_p3 PARTITION OF eager_agg_tab_ml FOR VALUES FROM (20) TO (30) PARTITION BY RANGE(x);
CREATE TABLE eager_agg_tab_ml_p3_s1 PARTITION OF eager_agg_tab_ml_p3 FOR VALUES FROM (20) TO (25);
CREATE TABLE eager_agg_tab_ml_p3_s2 PARTITION OF eager_agg_tab_ml_p3 FOR VALUES FROM (25) TO (30);
INSERT INTO eager_agg_tab_ml SELECT i % 30, i % 30 FROM generate_series(1, 1000) i;

ANALYZE eager_agg_tab_ml;

-- When GROUP BY clause matches; full aggregation is performed for each
-- partition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.x ORDER BY t1.x;

-- When GROUP BY clause does not match; partial aggregation is performed for
-- each partition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.y, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.y ORDER BY t1.y;

SELECT t1.y, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.y ORDER BY t1.y;

-- Check with eager aggregation over join rel
-- full aggregation
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y + t3.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
  JOIN eager_agg_tab_ml t3 ON t2.x = t3.x
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y + t3.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
  JOIN eager_agg_tab_ml t3 ON t2.x = t3.x
GROUP BY t1.x ORDER BY t1.x;

-- partial aggregation
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t3.y, sum(t2.y + t3.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
  JOIN eager_agg_tab_ml t3 ON t2.x = t3.x
GROUP BY t3.y ORDER BY t3.y;

SELECT t3.y, sum(t2.y + t3.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
  JOIN eager_agg_tab_ml t3 ON t2.x = t3.x
GROUP BY t3.y ORDER BY t3.y;

-- try that with GEQO too
SET geqo = on;
SET geqo_threshold = 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.x, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.x ORDER BY t1.x;

SELECT t1.x, sum(t2.y), count(*)
  FROM eager_agg_tab_ml t1
  JOIN eager_agg_tab_ml t2 ON t1.x = t2.x
GROUP BY t1.x ORDER BY t1.x;

RESET geqo;
RESET geqo_threshold;

DROP TABLE eager_agg_tab_ml;
