--
-- QUALIFY clause tests
--

CREATE TEMPORARY TABLE empsalary (
    depname varchar,
    empno bigint,
    salary int,
    enroll_date date
);

INSERT INTO empsalary VALUES
('develop', 10, 5200, '2007-08-01'),
('sales', 1, 5000, '2006-10-01'),
('personnel', 5, 3500, '2007-12-10'),
('sales', 4, 4800, '2007-08-08'),
('personnel', 2, 3900, '2006-12-23'),
('develop', 7, 4200, '2008-01-01'),
('develop', 9, 4500, '2008-01-01'),
('sales', 3, 4800, '2007-08-01'),
('develop', 8, 6000, '2006-10-01'),
('develop', 11, 5200, '2007-08-15');

--
-- Basic QUALIFY functionality
--

-- Simple QUALIFY with inline window function
EXPLAIN (COSTS OFF)
SELECT depname, empno, salary,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC)
FROM empsalary
QUALIFY rank() OVER (PARTITION BY depname ORDER BY salary DESC) <= 2;

SELECT depname, empno, salary,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC)
FROM empsalary
QUALIFY rank() OVER (PARTITION BY depname ORDER BY salary DESC) <= 2
ORDER BY depname, salary DESC, empno;

-- QUALIFY with named window
EXPLAIN (COSTS OFF)
SELECT depname, empno, salary,
       rank() OVER w
FROM empsalary
WINDOW w AS (PARTITION BY depname ORDER BY salary DESC)
QUALIFY rank() OVER w <= 2;

SELECT depname, empno, salary,
       rank() OVER w
FROM empsalary
WINDOW w AS (PARTITION BY depname ORDER BY salary DESC)
QUALIFY rank() OVER w <= 2
ORDER BY depname, salary DESC, empno;

--
-- Alias resolution tests
--

-- Reference alias defined in SELECT
EXPLAIN (COSTS OFF)
SELECT depname, empno, salary,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY rnk <= 2;

SELECT depname, empno, salary,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY rnk <= 2
ORDER BY depname, salary DESC, empno;

-- Reference alias with row_number
EXPLAIN (COSTS OFF)
SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn < 3;

SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn < 3;

-- Multiple aliases
EXPLAIN (COSTS OFF)
SELECT depname, empno, salary,
       row_number() OVER (PARTITION BY depname ORDER BY salary DESC) AS rn,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY rn = 1 OR rnk <= 2;

SELECT depname, empno, salary,
       row_number() OVER (PARTITION BY depname ORDER BY salary DESC) AS rn,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY rn = 1 OR rnk <= 2
ORDER BY depname, salary DESC;

--
-- Run condition optimization tests
--

-- row_number with < operator (should use run condition)
EXPLAIN (COSTS OFF)
SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn < 3;

SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn < 3;

-- row_number with <= operator
EXPLAIN (COSTS OFF)
SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn <= 2;

SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn <= 2;

-- rank with <= operator
EXPLAIN (COSTS OFF)
SELECT empno, salary, rank() OVER (ORDER BY salary DESC) AS r
FROM empsalary
QUALIFY r <= 3;

SELECT empno, salary, rank() OVER (ORDER BY salary DESC) AS r
FROM empsalary
QUALIFY r <= 3;

-- dense_rank with = operator (should convert to <= for run condition)
EXPLAIN (COSTS OFF)
SELECT empno, salary, dense_rank() OVER (ORDER BY salary DESC) AS dr
FROM empsalary
QUALIFY dr = 1;

SELECT empno, salary, dense_rank() OVER (ORDER BY salary DESC) AS dr
FROM empsalary
QUALIFY dr = 1;

-- count(*) with <= operator
EXPLAIN (COSTS OFF)
SELECT empno, salary, count(*) OVER (ORDER BY salary DESC) AS c
FROM empsalary
QUALIFY c <= 3;

SELECT empno, salary, count(*) OVER (ORDER BY salary DESC) AS c
FROM empsalary
QUALIFY c <= 3;

--
-- WHERE pushdown tests
-- Quals on PARTITION BY columns without window functions can be pushed to WHERE
--

-- depname is in PARTITION BY, should be pushed to WHERE (SeqScan filter)
EXPLAIN (COSTS OFF)
SELECT depname, empno, salary,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY rnk <= 2 AND depname = 'develop';

SELECT depname, empno, salary,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY rnk <= 2 AND depname = 'develop'
ORDER BY salary DESC;

-- Mixed: depname pushed to WHERE, rank condition stays as run condition
EXPLAIN (COSTS OFF)
SELECT depname, empno, salary,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY depname = 'sales' AND rnk = 1;

SELECT depname, empno, salary,
       rank() OVER (PARTITION BY depname ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY depname = 'sales' AND rnk = 1;

--
-- QUALIFY without window function in SELECT list
-- The window function appears only in QUALIFY
--

EXPLAIN (COSTS OFF)
SELECT depname, empno, salary
FROM empsalary
QUALIFY row_number() OVER (PARTITION BY depname ORDER BY salary DESC) = 1;

SELECT depname, empno, salary
FROM empsalary
QUALIFY row_number() OVER (PARTITION BY depname ORDER BY salary DESC) = 1
ORDER BY depname;

-- With PARTITION BY column filter pushed down
EXPLAIN (COSTS OFF)
SELECT depname, empno, salary
FROM empsalary
QUALIFY row_number() OVER (PARTITION BY depname ORDER BY salary DESC) = 1
        AND depname = 'develop';

SELECT depname, empno, salary
FROM empsalary
QUALIFY row_number() OVER (PARTITION BY depname ORDER BY salary DESC) = 1
        AND depname = 'develop';

--
-- Multiple window functions with different windows
--

EXPLAIN (COSTS OFF)
SELECT depname, empno, salary,
       row_number() OVER (PARTITION BY depname ORDER BY salary) AS rn_asc,
       row_number() OVER (PARTITION BY depname ORDER BY salary DESC) AS rn_desc
FROM empsalary
QUALIFY rn_asc = 1 OR rn_desc = 1;

SELECT depname, empno, salary,
       row_number() OVER (PARTITION BY depname ORDER BY salary) AS rn_asc,
       row_number() OVER (PARTITION BY depname ORDER BY salary DESC) AS rn_desc
FROM empsalary
QUALIFY rn_asc = 1 OR rn_desc = 1
ORDER BY depname, empno;

--
-- QUALIFY with aggregate and window functions
--

EXPLAIN (COSTS OFF)
SELECT depname, sum(salary) AS total,
       rank() OVER (ORDER BY sum(salary) DESC) AS rnk
FROM empsalary
GROUP BY depname
QUALIFY rnk <= 2;

SELECT depname, sum(salary) AS total,
       rank() OVER (ORDER BY sum(salary) DESC) AS rnk
FROM empsalary
GROUP BY depname
QUALIFY rnk <= 2;

--
-- QUALIFY in a grouped query without window functions.
-- The condition is validated like a SELECT-list item, so aggregates and
-- grouping columns are allowed (and behave as HAVING would).
--

-- Aggregate used directly in QUALIFY filters groups like HAVING
SELECT depname, count(*) AS c
FROM empsalary
GROUP BY depname
QUALIFY count(*) > 2
ORDER BY depname;

-- A grouping column may be referenced directly in QUALIFY
SELECT depname, count(*) AS c
FROM empsalary
GROUP BY depname
QUALIFY depname <> 'sales'
ORDER BY depname;

--
-- Comparison with equivalent subquery (results should match)
--

-- Subquery version
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno, salary, rank() OVER (ORDER BY salary DESC) AS r
   FROM empsalary) emp
WHERE r <= 3;

SELECT * FROM
  (SELECT empno, salary, rank() OVER (ORDER BY salary DESC) AS r
   FROM empsalary) emp
WHERE r <= 3
ORDER BY salary DESC, empno;

-- QUALIFY version (should produce same results)
EXPLAIN (COSTS OFF)
SELECT empno, salary, rank() OVER (ORDER BY salary DESC) AS r
FROM empsalary
QUALIFY r <= 3;

SELECT empno, salary, rank() OVER (ORDER BY salary DESC) AS r
FROM empsalary
QUALIFY r <= 3
ORDER BY salary DESC, empno;

--
-- QUALIFY with different comparison operators
--

-- Greater than (no run condition optimization for increasing functions)
EXPLAIN (COSTS OFF)
SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn > 7;

SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn > 7
ORDER BY empno;

-- Equality
EXPLAIN (COSTS OFF)
SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn = 5;

SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn = 5;

-- BETWEEN (should keep as filter)
EXPLAIN (COSTS OFF)
SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn BETWEEN 3 AND 5;

SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn BETWEEN 3 AND 5
ORDER BY empno;

-- IN list
EXPLAIN (COSTS OFF)
SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn IN (1, 3, 5);

SELECT empno, row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY rn IN (1, 3, 5)
ORDER BY empno;

--
-- QUALIFY with expressions
--

EXPLAIN (COSTS OFF)
SELECT empno, salary,
       row_number() OVER (ORDER BY salary DESC) AS rn
FROM empsalary
QUALIFY rn <= 10 / 2;

SELECT empno, salary,
       row_number() OVER (ORDER BY salary DESC) AS rn
FROM empsalary
QUALIFY rn <= 10 / 2;

--
-- Error cases
--

-- Aggregate in QUALIFY without GROUP BY: just as in the SELECT list, the
-- non-aggregated output columns make this an error (must appear in GROUP BY).
SELECT depname, empno, salary
FROM empsalary
QUALIFY sum(salary) > 5000;

-- QUALIFY in a grouped query referencing an ungrouped column (should error,
-- just as such a reference would in the SELECT list)
SELECT depname, count(*) AS c
FROM empsalary
GROUP BY depname
QUALIFY salary > 100;

-- QUALIFY with set-returning function (should error)
SELECT empno, salary
FROM empsalary
QUALIFY generate_series(1, 3) > 1;

--
-- QUALIFY in different query structures
--

-- With DISTINCT
EXPLAIN (COSTS OFF)
SELECT DISTINCT depname,
       first_value(empno) OVER (PARTITION BY depname ORDER BY salary DESC) AS top_emp
FROM empsalary
QUALIFY row_number() OVER (PARTITION BY depname ORDER BY salary DESC) = 1;

SELECT DISTINCT depname,
       first_value(empno) OVER (PARTITION BY depname ORDER BY salary DESC) AS top_emp
FROM empsalary
QUALIFY row_number() OVER (PARTITION BY depname ORDER BY salary DESC) = 1
ORDER BY depname;

-- With ORDER BY
EXPLAIN (COSTS OFF)
SELECT empno, salary,
       rank() OVER (ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY rnk <= 3;

SELECT empno, salary,
       rank() OVER (ORDER BY salary DESC) AS rnk
FROM empsalary
QUALIFY rnk <= 3
ORDER BY salary DESC, empno;

-- With LIMIT (QUALIFY is evaluated before LIMIT)
EXPLAIN (COSTS OFF)
SELECT empno, salary,
       row_number() OVER (ORDER BY salary DESC) AS rn
FROM empsalary
QUALIFY rn <= 5
LIMIT 3;

SELECT empno, salary,
       row_number() OVER (ORDER BY salary DESC) AS rn
FROM empsalary
QUALIFY rn <= 5
ORDER BY salary DESC
LIMIT 3;

--
-- Verify evaluation order: QUALIFY happens after window functions, before DISTINCT
--

-- This should first compute row_number, then filter, then apply DISTINCT
EXPLAIN (COSTS OFF)
SELECT DISTINCT salary,
       row_number() OVER (ORDER BY salary DESC) AS rn
FROM empsalary
QUALIFY rn <= 3;

SELECT DISTINCT salary,
       row_number() OVER (ORDER BY salary DESC) AS rn
FROM empsalary
QUALIFY rn <= 3;

-- QUALIFY with FILTERS over window function
EXPLAIN(COSTS OFF) SELECT sum(salary), row_number() OVER (ORDER BY depname) as rank, sum(
    sum(salary) FILTER (WHERE enroll_date > '2007-01-01')
) FILTER (WHERE depname <> 'sales') OVER (ORDER BY depname DESC) AS "filtered_sum",
    depname
FROM empsalary GROUP BY depname QUALIFY rank = 1;

SELECT sum(salary), row_number() OVER (ORDER BY depname) as rank, sum(
    sum(salary) FILTER (WHERE enroll_date > '2007-01-01')
) FILTER (WHERE depname <> 'sales') OVER (ORDER BY depname DESC) AS "filtered_sum",
    depname
FROM empsalary GROUP BY depname QUALIFY rank = 1;

--
-- Aggregate functions directly in QUALIFY
-- (allowed in a grouped query, the same as in the SELECT list / HAVING)
--

-- Aggregate condition over the group
EXPLAIN (COSTS OFF)
SELECT depname, sum(salary) AS total
FROM empsalary
GROUP BY depname
QUALIFY sum(salary) > 15000;

SELECT depname, sum(salary) AS total
FROM empsalary
GROUP BY depname
QUALIFY sum(salary) > 15000
ORDER BY depname;

-- Aggregate and window function combined in QUALIFY
SELECT depname, sum(salary) AS total
FROM empsalary
GROUP BY depname
QUALIFY rank() OVER (ORDER BY sum(salary) DESC) = 1
ORDER BY depname;

-- Whole-table aggregate (single group)
SELECT sum(salary) AS s
FROM empsalary
QUALIFY sum(salary) > 1000;

--
-- Name resolution tests
--

-- An input column takes precedence over a same-named select-list alias
-- "salary" in QUALIFY is the input column (> 4000 keeps most rows), not the
-- alias (which is 0 and would keep none)
SELECT empno, (empno * 0) AS salary
FROM empsalary
QUALIFY salary > 4000
ORDER BY empno;

-- QUALIFY referecing columns that not on output result

-- Same column
SELECT depname FROM empsalary QUALIFY salary > 0 ORDER BY salary;

-- Different column
SELECT depname FROM empsalary QUALIFY salary > 0 ORDER BY enroll_date;

--
-- Name resolution error cases
--

-- A non-deterministic alias expression cannot be referenced from QUALIFY
SELECT empno, (random() < 2)::int AS r
FROM empsalary
QUALIFY r = 1;

-- An ambiguous alias reference is rejected
SELECT empno AS x, salary AS x,
       row_number() OVER (ORDER BY empno) AS rn
FROM empsalary
QUALIFY x > 0;

-- A name that is neither an input column nor a select-list alias
SELECT empno
FROM empsalary
QUALIFY nosuchcol > row_number() OVER (ORDER BY empno);

--
-- QUALIFY without any window function: a general post-SELECT filter
--

EXPLAIN (COSTS OFF)
SELECT empno, salary, salary * 2 as new_salary
FROM empsalary
QUALIFY new_salary > 10000;

SELECT empno, salary, salary * 2 as new_salary
FROM empsalary
QUALIFY new_salary > 10000
ORDER BY empno;

--
-- QUALIFY in other query structures
--

-- In the arms of a set operation
SELECT empno FROM empsalary
QUALIFY row_number() OVER (ORDER BY salary) = 1
UNION ALL
SELECT empno FROM empsalary
QUALIFY row_number() OVER (ORDER BY salary DESC) = 1
ORDER BY empno;

-- In a subquery that carries an outer reference
SELECT depname,
       (SELECT empno FROM empsalary e2
        WHERE e2.depname = e1.depname
        QUALIFY row_number() OVER (ORDER BY salary DESC) = 1) AS top_emp
FROM (SELECT DISTINCT depname FROM empsalary) e1
ORDER BY depname;

-- Combined with row-level locking (the lock applies to the base table)
SELECT empno FROM empsalary
QUALIFY salary > 5000
ORDER BY empno
FOR UPDATE;

--
-- Cleanup
--
DROP TABLE empsalary;
