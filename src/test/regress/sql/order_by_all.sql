--
-- Test ORDER BY ALL
--

CREATE TEMP TABLE test_order (
  a int,
  b text,
  c float
);

INSERT INTO test_order VALUES
  (3, 'foo', 1.5),
  (1, 'bar', 2.5),
  (2, 'baz', 1.5),
  (1, 'qux', 1.5);

-- Basic ORDER BY ALL test (default ASC)
SELECT * FROM test_order ORDER BY ALL;

-- ORDER BY ALL ASC (explicit)
SELECT * FROM test_order ORDER BY ALL ASC;

-- ORDER BY ALL DESC
SELECT * FROM test_order ORDER BY ALL DESC;

-- ORDER BY ALL with NULLS FIRST
SELECT * FROM test_order ORDER BY ALL NULLS FIRST;

-- ORDER BY ALL DESC NULLS LAST
SELECT * FROM test_order ORDER BY ALL DESC NULLS LAST;

-- ORDER BY ALL with specific columns in SELECT
SELECT a, b FROM test_order ORDER BY ALL;

-- ORDER BY ALL with expressions
SELECT a, b, a + c AS sum FROM test_order ORDER BY ALL;

-- ORDER BY ALL with aggregates (should only order by non-aggregated columns)
SELECT a, COUNT(*) FROM test_order GROUP BY a ORDER BY ALL;

-- ORDER BY ALL with WHERE clause
SELECT * FROM test_order WHERE a > 1 ORDER BY ALL;

-- Verify that ORDER BY ALL is equivalent to listing all columns
SELECT * FROM test_order ORDER BY ALL;
SELECT * FROM test_order ORDER BY a, b, c;

-- ORDER BY ALL with LIMIT
SELECT * FROM test_order ORDER BY ALL LIMIT 2;

-- Test deparsing of ORDER BY ALL (ruleutils.c)
-- Verify that ORDER BY ALL with modifiers is correctly preserved in view definitions

-- View with ORDER BY ALL (default)
CREATE VIEW view_order_all AS
  SELECT * FROM test_order ORDER BY ALL;

SELECT pg_get_viewdef('view_order_all'::regclass, true);

-- View with ORDER BY ALL ASC
CREATE VIEW view_order_all_asc AS
  SELECT * FROM test_order ORDER BY ALL ASC;

SELECT pg_get_viewdef('view_order_all_asc'::regclass, true);

-- View with ORDER BY ALL DESC
CREATE VIEW view_order_all_desc AS
  SELECT * FROM test_order ORDER BY ALL DESC;

SELECT pg_get_viewdef('view_order_all_desc'::regclass, true);

-- View with ORDER BY ALL NULLS FIRST
CREATE VIEW view_order_all_nulls_first AS
  SELECT * FROM test_order ORDER BY ALL NULLS FIRST;

SELECT pg_get_viewdef('view_order_all_nulls_first'::regclass, true);

-- View with ORDER BY ALL NULLS LAST
CREATE VIEW view_order_all_nulls_last AS
  SELECT * FROM test_order ORDER BY ALL NULLS LAST;

SELECT pg_get_viewdef('view_order_all_nulls_last'::regclass, true);

-- View with ORDER BY ALL DESC NULLS FIRST
CREATE VIEW view_order_all_desc_nulls_first AS
  SELECT * FROM test_order ORDER BY ALL DESC NULLS FIRST;

SELECT pg_get_viewdef('view_order_all_desc_nulls_first'::regclass, true);

-- View with ORDER BY ALL ASC NULLS LAST
CREATE VIEW view_order_all_asc_nulls_last AS
  SELECT * FROM test_order ORDER BY ALL ASC NULLS LAST;

SELECT pg_get_viewdef('view_order_all_asc_nulls_last'::regclass, true);

-- Verify the views actually work
SELECT * FROM view_order_all;
SELECT * FROM view_order_all_desc;

-- Clean up views
DROP VIEW view_order_all;
DROP VIEW view_order_all_asc;
DROP VIEW view_order_all_desc;
DROP VIEW view_order_all_nulls_first;
DROP VIEW view_order_all_nulls_last;
DROP VIEW view_order_all_desc_nulls_first;
DROP VIEW view_order_all_asc_nulls_last;

-- Test with NULL values
CREATE TEMP TABLE test_order_nulls (
  a int,
  b text,
  c float
);

INSERT INTO test_order_nulls VALUES
  (3, 'foo', 1.5),
  (1, 'bar', 2.5),
  (2, 'baz', 1.5),
  (1, 'qux', 1.5),
  (NULL, 'null_a', 3.0),
  (2, NULL, 2.0),
  (3, 'foo', NULL);

-- ORDER BY ALL with NULLs (default ASC)
SELECT * FROM test_order_nulls ORDER BY ALL;

-- ORDER BY ALL ASC (explicit) with NULLs
SELECT * FROM test_order_nulls ORDER BY ALL ASC;

-- ORDER BY ALL DESC with NULLs
SELECT * FROM test_order_nulls ORDER BY ALL DESC;

-- ORDER BY ALL NULLS FIRST (with default ASC)
SELECT * FROM test_order_nulls ORDER BY ALL NULLS FIRST;

-- ORDER BY ALL NULLS LAST (with default ASC)
SELECT * FROM test_order_nulls ORDER BY ALL NULLS LAST;

-- ORDER BY ALL ASC NULLS FIRST
SELECT * FROM test_order_nulls ORDER BY ALL ASC NULLS FIRST;

-- ORDER BY ALL DESC NULLS LAST
SELECT * FROM test_order_nulls ORDER BY ALL DESC NULLS LAST;

-- ORDER BY ALL DESC NULLS FIRST
SELECT * FROM test_order_nulls ORDER BY ALL DESC NULLS FIRST;

-- Verify ORDER BY ALL DESC is equivalent to listing all columns DESC
SELECT * FROM test_order_nulls ORDER BY ALL DESC;
SELECT * FROM test_order_nulls ORDER BY a DESC, b DESC, c DESC;

-- Test with WHERE clause and NULLs
SELECT * FROM test_order_nulls WHERE a IS NOT NULL ORDER BY ALL DESC;

-- Test with LIMIT and NULLs
SELECT * FROM test_order_nulls ORDER BY ALL DESC LIMIT 3;

-- Test with subset of columns and NULLs
SELECT a, b FROM test_order_nulls ORDER BY ALL DESC;

-- Clean up
DROP TABLE test_order_nulls;

-- Negative tests: invalid ORDER BY ALL syntax

-- ORDER BY ALL cannot be mixed with explicit column specifications
SELECT * FROM test_order ORDER BY ALL, a;

-- ORDER BY ALL cannot use USING operator
SELECT * FROM test_order ORDER BY ALL USING <;

-- ORDER BY ALL with multiple ALL keywords (should fail)
SELECT * FROM test_order ORDER BY ALL, ALL;

-- ORDER BY ALL with set operations (UNION)
SELECT a, b FROM test_order UNION SELECT a, b FROM test_order ORDER BY ALL;

-- ORDER BY ALL with column number reference throw an error
SELECT a, b FROM test_order ORDER BY ALL, 1;

-- Additional test coverage for edge cases

-- Test ORDER BY ALL in subquery
SELECT * FROM (SELECT a, b FROM test_order ORDER BY ALL) sq;

-- Test ORDER BY ALL with nested subquery
SELECT * FROM (
  SELECT a, b FROM (
    SELECT * FROM test_order ORDER BY ALL DESC
  ) sub1 ORDER BY ALL
) sub2;

-- Test ORDER BY ALL with DISTINCT
SELECT DISTINCT a, b FROM test_order ORDER BY ALL;

-- Test ORDER BY ALL with JOIN
CREATE TEMP TABLE test_order2 (
  x int,
  y text
);

INSERT INTO test_order2 VALUES (1, 'alpha'), (2, 'beta'), (3, 'gamma');

SELECT t1.a, t1.b, t2.x, t2.y
FROM test_order t1
JOIN test_order2 t2 ON t1.a = t2.x
ORDER BY ALL;

DROP TABLE test_order2;

-- Test ORDER BY ALL with CTE
WITH cte AS (
  SELECT a, b FROM test_order WHERE a > 1
)
SELECT * FROM cte ORDER BY ALL;

-- Test ORDER BY ALL with window function (ORDER BY ALL in outer query)
SELECT a, b, ROW_NUMBER() OVER (PARTITION BY a ORDER BY b) as rn
FROM test_order
ORDER BY ALL;

-- Test ORDER BY ALL with INTERSECT (should fail like UNION)
SELECT a, b FROM test_order INTERSECT SELECT a, b FROM test_order ORDER BY ALL;

-- Test ORDER BY ALL with EXCEPT (should fail like UNION)
SELECT a, b FROM test_order EXCEPT SELECT a, b FROM test_order ORDER BY ALL;

-- Test ORDER BY ALL with VALUES
SELECT * FROM (VALUES (3, 'foo'), (1, 'bar'), (2, 'baz')) AS t(x, y) ORDER BY ALL;

-- Test ORDER BY ALL with only computed columns (no junk)
SELECT a + 1 AS col1, b || '_test' AS col2 FROM test_order ORDER BY ALL;

-- Test behavior when all selected columns would be junk (using ctid/system columns)
-- This should still work as system columns aren't marked as junk in target list when explicitly selected
SELECT ctid FROM test_order ORDER BY ALL;

-- Test ORDER BY ALL with UNION ALL in subquery (ORDER BY on outer query should work)
SELECT * FROM (
  SELECT a, b FROM test_order
  UNION ALL
  SELECT a, b FROM test_order
) sub ORDER BY ALL;

-- Clean up
DROP TABLE test_order;
