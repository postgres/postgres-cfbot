--
-- int16_funcs.sql
--
-- Regression tests for additional int16 functions: gcd, lcm, factorial,
-- generate_series, in_range (window support), and btree support functions.
--

--
-- gcd
--

-- Basic cases
SELECT gcd('12'::int16, '8'::int16);   -- 4
SELECT gcd('17'::int16, '5'::int16);   -- 1 (coprime)
SELECT gcd('100'::int16, '10'::int16); -- 10
SELECT gcd('48'::int16, '36'::int16);  -- 12

-- gcd with zero
SELECT gcd('0'::int16, '5'::int16);    -- 5
SELECT gcd('5'::int16, '0'::int16);    -- 5
SELECT gcd('0'::int16, '0'::int16);    -- 0

-- gcd with negatives
SELECT gcd('-12'::int16, '8'::int16);  -- 4
SELECT gcd('12'::int16, '-8'::int16);  -- 4
SELECT gcd('-12'::int16, '-8'::int16); -- 4

-- gcd with 1
SELECT gcd('1'::int16, '1'::int16);    -- 1
SELECT gcd('100'::int16, '1'::int16);  -- 1

-- gcd with same value
SELECT gcd('42'::int16, '42'::int16);  -- 42

-- gcd(INT128_MIN, -1) = 1 (special case)
SELECT gcd('-170141183460469231731687303715884105728'::int16, '-1'::int16); -- 1

-- gcd(INT128_MIN, 0) should error (abs overflow)
SELECT gcd('-170141183460469231731687303715884105728'::int16, '0'::int16);

-- gcd(INT128_MIN, INT128_MIN) should error (abs overflow)
SELECT gcd('-170141183460469231731687303715884105728'::int16,
           '-170141183460469231731687303715884105728'::int16);

--
-- lcm
--

-- Basic cases
SELECT lcm('4'::int16, '6'::int16);    -- 12
SELECT lcm('3'::int16, '5'::int16);    -- 15 (coprime)
SELECT lcm('12'::int16, '8'::int16);   -- 24
SELECT lcm('10'::int16, '10'::int16);  -- 10

-- lcm with zero
SELECT lcm('0'::int16, '5'::int16);    -- 0
SELECT lcm('5'::int16, '0'::int16);    -- 0
SELECT lcm('0'::int16, '0'::int16);    -- 0

-- lcm with negatives
SELECT lcm('-4'::int16, '6'::int16);   -- 12
SELECT lcm('4'::int16, '-6'::int16);   -- 12
SELECT lcm('-4'::int16, '-6'::int16);  -- 12

-- lcm with 1
SELECT lcm('1'::int16, '7'::int16);    -- 7

-- lcm overflow
SELECT lcm('170141183460469231731687303715884105727'::int16, '2'::int16);

--
-- factorial
--

SELECT factorial('0'::int16);  -- 1
SELECT factorial('1'::int16);  -- 1
SELECT factorial('2'::int16);  -- 2
SELECT factorial('5'::int16);  -- 120
SELECT factorial('10'::int16); -- 3628800
SELECT factorial('20'::int16); -- 2432902008176640000

-- factorial of negative number (error)
SELECT factorial('-1'::int16);

--
-- generate_series
--

-- Default step (1)
SELECT * FROM generate_series('1'::int16, '5'::int16);

-- Positive step
SELECT * FROM generate_series('1'::int16, '10'::int16, '2'::int16);

-- Negative step (counting down)
SELECT * FROM generate_series('5'::int16, '1'::int16, '-1'::int16);

-- Step of 0 (error)
SELECT * FROM generate_series('1'::int16, '5'::int16, '0'::int16);

-- Empty range (start > stop with positive step)
SELECT * FROM generate_series('5'::int16, '1'::int16);

-- Single element
SELECT * FROM generate_series('3'::int16, '3'::int16);

-- Large step
SELECT * FROM generate_series('0'::int16, '100000000000000000000'::int16,
                              '100000000000000000000'::int16);

-- generate_series in a subquery
SELECT count(*) FROM generate_series('1'::int16, '100'::int16);

-- generate_series with large values near INT128_MAX
SELECT * FROM generate_series(
    '170141183460469231731687303715884105725'::int16,
    '170141183460469231731687303715884105727'::int16
);

--
-- in_range (window function support)
--

-- Basic in_range tests (direct function call)
SELECT in_range('5'::int16, '0'::int16, '10'::int16, false, true);  -- true (5 <= 0+10)
SELECT in_range('15'::int16, '0'::int16, '10'::int16, false, true); -- false (15 > 10)
SELECT in_range('5'::int16, '0'::int16, '10'::int16, false, false); -- true (5 >= 0+10? no) false

-- in_range with sub=true (preceding)
SELECT in_range('5'::int16, '10'::int16, '3'::int16, true, true);   -- true (5 <= 10-3=7)

-- Negative offset (error)
SELECT in_range('5'::int16, '0'::int16, '-1'::int16, false, true);

-- Window function using RANGE with int16
CREATE TABLE int16_window_test (id int4, val int16);
INSERT INTO int16_window_test VALUES
    (1, '10'::int16),
    (2, '20'::int16),
    (3, '30'::int16),
    (4, '40'::int16),
    (5, '50'::int16);

-- RANGE BETWEEN 10 PRECEDING AND 10 FOLLOWING
SELECT id, val, count(*) OVER (
    ORDER BY val
    RANGE BETWEEN '10'::int16 PRECEDING AND '10'::int16 FOLLOWING
) AS cnt
FROM int16_window_test
ORDER BY id;

-- RANGE BETWEEN 15 PRECEDING AND 5 FOLLOWING
SELECT id, val, sum(val) OVER (
    ORDER BY val
    RANGE BETWEEN '15'::int16 PRECEDING AND '5'::int16 FOLLOWING
) AS range_sum
FROM int16_window_test
ORDER BY id;

DROP TABLE int16_window_test;

--
-- B-tree operator class support functions (sort support, equalimage, skip support)
--

-- Verify btree index creation and queries work
CREATE TABLE int16_btree_test (val int16);
INSERT INTO int16_btree_test VALUES
    ('5'::int16), ('3'::int16), ('8'::int16), ('1'::int16), ('9'::int16);

CREATE INDEX idx_int16_btree ON int16_btree_test USING btree (val);

-- Query using the index
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM int16_btree_test WHERE val = '5'::int16;
SELECT * FROM int16_btree_test WHERE val = '5'::int16;

-- Ordered query
SELECT * FROM int16_btree_test ORDER BY val;

-- Deduplication (btequalimage) - create table with duplicates and verify
CREATE TABLE int16_dedup_test (val int16);
INSERT INTO int16_dedup_test VALUES
    ('5'::int16), ('5'::int16), ('5'::int16), ('3'::int16), ('3'::int16);

CREATE INDEX idx_int16_dedup ON int16_dedup_test USING btree (val);

-- Verify deduplication is working (index should have fewer entries)
SELECT count(*) FROM int16_dedup_test;

SET enable_seqscan = off;
SELECT * FROM int16_dedup_test WHERE val = '5'::int16;
SET enable_seqscan = on;

DROP TABLE int16_dedup_test;

-- Skip scan test: query on a column with a leading skipped column
CREATE TABLE int16_skip_test (grp int16, val int4);
INSERT INTO int16_skip_test VALUES
    ('1'::int16, 10),
    ('1'::int16, 20),
    ('2'::int16, 30),
    ('2'::int16, 40),
    ('3'::int16, 50);

CREATE INDEX idx_int16_skip ON int16_skip_test (grp, val);

-- DISTINCT on grp should be able to use skip scan
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT DISTINCT grp FROM int16_skip_test ORDER BY grp;
SELECT DISTINCT grp FROM int16_skip_test ORDER BY grp;
SET enable_seqscan = on;

DROP TABLE int16_skip_test;

DROP TABLE int16_btree_test;
