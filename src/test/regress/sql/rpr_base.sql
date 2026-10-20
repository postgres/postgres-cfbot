-- ============================================================
-- RPR Base Tests
-- Tests for Row Pattern Recognition (ISO/IEC 19075-5)
-- ============================================================
--
-- Parser Layer:
--   Keyword Usage Tests
--   DEFINE Clause Tests
--   FRAME Options Tests
--   PARTITION BY + FRAME Tests
--   PATTERN Syntax Tests
--   Quantifiers Tests
--   Navigation Functions Tests
--   SKIP TO / INITIAL Tests
--   Serialization/Deserialization Tests
--   Glued Quantifier / Alternation Tests
--   Error Cases Tests
--
-- Planner Layer:
--   Pattern Optimization Tests
--   Absorption Flag Display Tests
--   Absorption Analysis Tests
--   Edge Case Tests
--   Optimization Fallback Tests
--   Planner Integration Tests
--   Subquery and CTE Tests
--   JOIN Tests
--   Complex Expression Tests
--   Set Operations Tests
--   Sorting and Grouping Tests
--   SQL Function Inlining Tests
--   Stress Tests
--   Error Limit Tests
--
-- Contributed Tests:
--   Basic Pattern Matching
--   Pathological Patterns
-- ============================================================

SET client_min_messages = WARNING;

-- ============================================================
-- Keyword Usage Tests
-- ============================================================

-- RPR keywords as column names
-- Keywords: define, initial, past, pattern, permute, seek

CREATE TABLE rpr_keywords (
    id INT,
    define INT,      -- DEFINE keyword
    initial INT,     -- INITIAL keyword
    past INT,        -- PAST keyword
    pattern INT,     -- PATTERN keyword
    permute INT,     -- PERMUTE keyword
    seek INT,        -- SEEK keyword
    skip INT         -- SKIP keyword (pre-existing)
);

INSERT INTO rpr_keywords VALUES (1, 10, 20, 30, 40, 45, 50, 60);

SELECT id, define, initial, past, pattern, permute, seek, skip
FROM rpr_keywords;

DROP TABLE rpr_keywords;

-- ============================================================
-- DEFINE Clause Tests
-- ============================================================

-- Simple column references
CREATE TABLE rpr_stock_price (
    dt DATE,
    symbol TEXT,
    price NUMERIC,
    volume INT
);

INSERT INTO rpr_stock_price VALUES
    ('2024-01-01', 'AAPL', 150, 1000),
    ('2024-01-02', 'AAPL', 155, 1200),
    ('2024-01-03', 'AAPL', 152, 900),
    ('2024-01-04', 'AAPL', 160, 1500),
    ('2024-01-05', 'AAPL', 158, 1100);

-- Simple column reference
SELECT dt, price, COUNT(*) OVER w as cnt
FROM rpr_stock_price
WINDOW w AS (
    PARTITION BY symbol
    ORDER BY dt
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (UP+)
    DEFINE UP AS price > 150
);

-- Multiple column references
SELECT dt, price, volume, COUNT(*) OVER w as cnt
FROM rpr_stock_price
WINDOW w AS (
    PARTITION BY symbol
    ORDER BY dt
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (GOOD+)
    DEFINE GOOD AS price > 150 AND volume > 1000
);

-- Expression in DEFINE
SELECT dt, price, COUNT(*) OVER w as cnt
FROM rpr_stock_price
WINDOW w AS (
    PARTITION BY symbol
    ORDER BY dt
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (HIGH+)
    DEFINE HIGH AS price * 1.1 > 165
);

-- Arithmetic and functions
SELECT dt, price, volume, COUNT(*) OVER w as cnt
FROM rpr_stock_price
WINDOW w AS (
    PARTITION BY symbol
    ORDER BY dt
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (CALC+)
    DEFINE CALC AS (price + volume / 100) > 160
);

DROP TABLE rpr_stock_price;

-- Pattern variables with no DEFINE entry
CREATE TABLE rpr_auto (id INT, val INT);
INSERT INTO rpr_auto VALUES (1, 10), (2, 20), (3, 30), (4, 15);

-- B has no DEFINE entry, so it matches every row
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_auto
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ B*)
    DEFINE A AS val > 15
);

-- Multiple undefined variables
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_auto
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B C)
    DEFINE A AS val > 0
    -- B and C have no DEFINE entry, so they match every row
);

-- All variables defined explicitly
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_auto
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (X Y Z)
    DEFINE
        X AS val > 10,
        Y AS val > 20,
        Z AS val < 20
);

DROP TABLE rpr_auto;

-- Duplicate variable names
CREATE TABLE rpr_dup (id INT);
INSERT INTO rpr_dup VALUES (1), (2);

-- Duplicate DEFINE variable name is not allowed
SELECT COUNT(*) OVER w
FROM rpr_dup
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS id > 0, A AS id < 10
);

DROP TABLE rpr_dup;

-- Boolean coercion
CREATE TABLE rpr_bool (id INT, flag BOOLEAN);
INSERT INTO rpr_bool VALUES (1, true), (2, false);

-- DEFINE clause must be a boolean expression
SELECT COUNT(*) OVER w
FROM rpr_bool
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS id
);

-- Boolean column reference
SELECT id, flag, COUNT(*) OVER w as cnt
FROM rpr_bool
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (T+)
    DEFINE T AS flag
);

-- NULL::boolean
SELECT id, COUNT(*) OVER w as cnt
FROM rpr_bool
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (N+)
    DEFINE N AS NULL::boolean
);

-- Implicit cast to boolean via custom type
CREATE TYPE rpr_truthyint AS (v int);
CREATE FUNCTION rpr_truthyint_to_bool(rpr_truthyint) RETURNS boolean AS $$
  SELECT ($1).v <> 0;
$$ LANGUAGE SQL IMMUTABLE STRICT;
CREATE CAST (rpr_truthyint AS boolean)
  WITH FUNCTION rpr_truthyint_to_bool(rpr_truthyint)
  AS ASSIGNMENT;

CREATE TABLE rpr_coerce (id int, val rpr_truthyint);
INSERT INTO rpr_coerce VALUES (1, ROW(1)), (2, ROW(0)), (3, ROW(5)), (4, ROW(0));

SELECT id, val, cnt
FROM (SELECT id, val,
             COUNT(*) OVER w AS cnt
      FROM rpr_coerce
      WINDOW w AS (
          ORDER BY id
          ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
          PATTERN (A+)
          DEFINE A AS val
      )
) s ORDER BY id;

DROP TABLE rpr_coerce;
DROP CAST (rpr_truthyint AS boolean);
DROP FUNCTION rpr_truthyint_to_bool(rpr_truthyint);
DROP TYPE rpr_truthyint;

DROP TABLE rpr_bool;

-- Coercion over a boolean domain is not a no-op; the wrapped Var must still
-- propagate when referenced only in DEFINE (flag is not in the select list)
CREATE DOMAIN rpr_boolish AS boolean;
CREATE TABLE rpr_domain (id int, flag rpr_boolish);
INSERT INTO rpr_domain VALUES (1, true), (2, false), (3, true);
SELECT id, COUNT(*) OVER w AS cnt
FROM rpr_domain
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS flag
);
DROP TABLE rpr_domain;
DROP DOMAIN rpr_boolish;

-- A Var referenced only inside a navigation operation must still propagate
-- (val appears only inside PREV(), not as a bare operand or in the select list)
CREATE TABLE rpr_nav (id int, val int);
INSERT INTO rpr_nav VALUES (1, 0), (2, 1), (3, 0), (4, 2);
SELECT id, COUNT(*) OVER w AS cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (UP+)
    DEFINE UP AS id > PREV(val)
);
DROP TABLE rpr_nav;

-- A non-boolean DEFINE expression is rejected
CREATE TABLE rpr_noncoerce (id int, n int);
INSERT INTO rpr_noncoerce VALUES (1, 1);
SELECT id, COUNT(*) OVER w AS cnt
FROM rpr_noncoerce
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS n
);
DROP TABLE rpr_noncoerce;

-- A non-boolean later DEFINE is rejected at its own definition even when an
-- earlier DEFINE variable is valid
CREATE TABLE rpr_noncoerce2 (id int, n int);
INSERT INTO rpr_noncoerce2 VALUES (1, 1);
SELECT id, COUNT(*) OVER w AS cnt
FROM rpr_noncoerce2
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS id > 0, B AS n
);
DROP TABLE rpr_noncoerce2;

-- Complex expressions
CREATE TABLE rpr_complex (id INT, val1 INT, val2 INT);
INSERT INTO rpr_complex VALUES (1, 10, 20), (2, 15, 25), (3, 20, 30);

-- CASE expression
SELECT id, val1, val2, COUNT(*) OVER w as cnt
FROM rpr_complex
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C+)
    DEFINE C AS CASE WHEN val1 > 10 THEN val2 > 20 ELSE false END
);

DROP TABLE rpr_complex;

-- Extra DEFINE variable not present in PATTERN
CREATE TABLE rpr_unused (id INT);
INSERT INTO rpr_unused VALUES (1), (2);

-- Extra DEFINE variable
SELECT id, COUNT(*) OVER w as cnt
FROM rpr_unused
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS id > 0, B AS id > 5  -- B not in pattern
);

DROP TABLE rpr_unused;

-- A DEFINE predicate is evaluated only when its variable is tentatively
-- mapped.  A is false at every row, so B is never reached; B's condition
-- (which would divide by zero) must never run, and every row is unmatched.
CREATE TABLE rpr_lazy (id INT, v INT);
INSERT INTO rpr_lazy VALUES (1, 1), (2, 2), (3, 3);
SELECT id, v, count(*) OVER w AS cnt
FROM rpr_lazy
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B)
    DEFINE A AS v < 0, B AS 1 / (v - v) > 0
);

DROP TABLE rpr_lazy;

-- A navigation result carrying a collation.  Both forms make the parser ask
-- for the collation of the navigation node itself rather than of its argument.
CREATE TABLE rpr_navcoll (id INT, s TEXT);
INSERT INTO rpr_navcoll VALUES (1, 'a'), (2, 'B'), (3, 'c');

-- COLLATE applied to the navigation result
SELECT id, s, count(*) OVER w AS cnt
FROM rpr_navcoll
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(s) COLLATE "C" < s
);

-- Simple CASE over the navigation result: the placeholder takes its collation
-- from the tested expression
SELECT id, s, count(*) OVER w AS cnt
FROM rpr_navcoll
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS CASE PREV(s) WHEN 'a' THEN true ELSE false END
);

DROP TABLE rpr_navcoll;

-- A system column in a DEFINE expression.  Only the scan reads it as a system
-- attribute; it reaches the expression as an outer Var, and navigation still
-- applies to it: PREV(ctid) is the previous row of the match, not this row.
CREATE TABLE rpr_navsys (i INT);
INSERT INTO rpr_navsys SELECT generate_series(1, 5);
SELECT i, count(*) OVER w AS cnt
FROM rpr_navsys
WINDOW w AS (
    ORDER BY i
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(ctid) IS DISTINCT FROM ctid
);

DROP TABLE rpr_navsys;

-- ============================================================
-- FRAME Options Tests
-- ============================================================

CREATE TABLE rpr_frame (id INT, val INT);
INSERT INTO rpr_frame VALUES
    (1, 10), (2, 10), (3, 10),  -- Same val: 10
    (4, 20), (5, 20),           -- Same val: 20
    (6, 30);

-- Valid frame options

-- ROWS: counts physical rows (1 FOLLOWING = next 1 physical row)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY val
    ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B?)
    DEFINE A AS val >= 0, B AS val >= 0
)
ORDER BY id;

-- frame must start at CURRENT ROW, not UNBOUNDED PRECEDING
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- EXCLUDE options

-- EXCLUDE not permitted
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    EXCLUDE CURRENT ROW
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- EXCLUDE GROUP not permitted
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    EXCLUDE GROUP
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- EXCLUDE TIES not permitted
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    EXCLUDE TIES
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Both rules broken at once.  The frame shape is settled first, so the
-- report names the shape; the EXCLUDE clause may not survive the rewrite.
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
    EXCLUDE TIES
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- range frame is not allowed with RPR
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- GROUPS frame is not allowed with RPR
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    GROUPS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- omitting the frame clause leaves the standard default, RANGE BETWEEN
-- UNBOUNDED PRECEDING AND CURRENT ROW, which breaks three of the rules at
-- once.  One report, stating what the frame has to be.
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- frame must start at CURRENT ROW, not offset PRECEDING
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN 1 PRECEDING AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- frame must start at CURRENT ROW, not offset FOLLOWING
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- ERROR: end before start: CURRENT ROW AND 1 PRECEDING
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 1 PRECEDING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- ERROR: end before start: CURRENT ROW AND UNBOUNDED PRECEDING
SELECT COUNT(*) OVER w
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED PRECEDING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Single row frame: CURRENT ROW AND CURRENT ROW is rejected (the standard
-- allows only UNBOUNDED FOLLOWING or a positive offset FOLLOWING).
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND CURRENT ROW
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A)
    DEFINE A AS val > 0
);

-- Zero offset: CURRENT ROW AND 0 FOLLOWING denotes the same one-row frame
-- and is likewise rejected (caught at execution time).
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 0 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A)
    DEFINE A AS val > 0
);

-- A non-constant frame end offset is allowed; a zero value is rejected by the
-- same execution-time check the literal 0 above reaches.
PREPARE rpr_end_offset(int8) AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND $1 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A)
    DEFINE A AS val > 0
);
EXECUTE rpr_end_offset(2);
EXECUTE rpr_end_offset(0);
DEALLOCATE rpr_end_offset;

-- Large offset: CURRENT ROW AND 1000 FOLLOWING
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 1000 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Maximum offset: CURRENT ROW AND 2147483646 FOLLOWING (INT_MAX - 1)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 2147483646 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- int64 frame-end overflow: a huge FOLLOWING offset must clamp to the
-- partition end (matchStartRow + offset + 1 overflows int64; the clamp makes
-- it behave like UNBOUNDED FOLLOWING).  Guards against signed-integer overflow
-- in the "frameOffset + 1" subexpression (undefined behavior).  The cnt values
-- must match the UNBOUNDED FOLLOWING result for the same data.
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 9223372036854775806 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- range frame is not allowed with RPR
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY val
    RANGE BETWEEN CURRENT ROW AND 10 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B?)
    DEFINE A AS val >= 0, B AS val >= 0
);

-- GROUPS frame with RPR (not permitted)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_frame
WINDOW w AS (
    ORDER BY val
    GROUPS BETWEEN CURRENT ROW AND 1 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B?)
    DEFINE A AS val >= 0, B AS val >= 0
);

DROP TABLE rpr_frame;

-- ============================================================
-- PARTITION BY + FRAME Tests
-- ============================================================

-- Test PARTITION BY with RPR to ensure proper partitioning behavior
CREATE TABLE rpr_partition (id INT, grp INT, val INT);
INSERT INTO rpr_partition VALUES
    (1, 1, 10), (2, 1, 20), (3, 1, 30),
    (4, 2, 15), (5, 2, 25), (6, 2, 35);

-- PARTITION BY with ROWS frame
SELECT id, grp, val, COUNT(*) OVER w as cnt
FROM rpr_partition
WINDOW w AS (
    PARTITION BY grp
    ORDER BY val
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B+)
    DEFINE A AS val >= 10, B AS val > 15
);

-- PARTITION BY with RANGE frame
SELECT id, grp, val, COUNT(*) OVER w as cnt
FROM rpr_partition
WINDOW w AS (
    PARTITION BY grp
    ORDER BY val
    RANGE BETWEEN CURRENT ROW AND 10 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B?)
    DEFINE A AS val >= 10, B AS val >= 20
);

DROP TABLE rpr_partition;

-- ============================================================
-- PATTERN Syntax Tests
-- ============================================================

CREATE TABLE rpr_pattern (id INT, val INT);
INSERT INTO rpr_pattern VALUES
    (1, 5), (2, 10), (3, 15), (4, 20), (5, 25),
    (6, 30), (7, 35), (8, 40), (9, 45), (10, 50);

-- Alternation (|)

-- Multiple alternatives
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_pattern
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ | B+ | C+)
    DEFINE A AS val > 35, B AS val BETWEEN 15 AND 35, C AS val < 15
);

-- Grouping

-- Nested grouping with quantifier
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_pattern
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A B) C)+)
    DEFINE A AS val > 10, B AS val > 20, C AS val > 30
);

-- Sequence

-- Multi-element sequence
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_pattern
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B C D E)
    DEFINE
        A AS val < 15,
        B AS val BETWEEN 15 AND 25,
        C AS val BETWEEN 25 AND 35,
        D AS val BETWEEN 35 AND 45,
        E AS val >= 45
);

-- Complex combinations

-- Alternation with grouping
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_pattern
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B) | (C D))
    DEFINE A AS val < 20, B AS val >= 20, C AS val < 30, D AS val >= 30
);

-- Alternation + sequence + grouping
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_pattern
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (START (UP{2,} DOWN? | FLAT+) FINISH)
    DEFINE
        START AS val >= 0,
        UP AS val > 20,
        DOWN AS val <= 30,
        FLAT AS val BETWEEN 25 AND 35,
        FINISH AS val > 40
);

-- Nested alternation in groups
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_pattern
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B) (C | D))
    DEFINE A AS val < 15, B AS val BETWEEN 15 AND 25, C AS val BETWEEN 25 AND 35, D AS val > 35
);

DROP TABLE rpr_pattern;

-- ============================================================
-- Quantifiers Tests
-- ============================================================

CREATE TABLE rpr_quant (id INT, val INT);
INSERT INTO rpr_quant VALUES
    (1, 10), (2, 20), (3, 30), (4, 40), (5, 50),
    (6, 60), (7, 70), (8, 80), (9, 90), (10, 100);

-- Basic greedy quantifiers

-- * (zero or more)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A*)
    DEFINE A AS val > 0
);

-- + (one or more)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 50
);

-- ? (zero or one)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A?)
    DEFINE A AS val = 50
);

-- Edge case quantifiers

-- {0} is not allowed (min must be >= 1)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{0} B)
    DEFINE A AS val > 1000, B AS val > 0
);

-- {0,0} is not allowed (max must be >= 1)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{0,0} B)
    DEFINE A AS val > 1000, B AS val > 0
);

-- {0,1} (equivalent to ?)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{0,1})
    DEFINE A AS val = 50
);

-- Exact quantifiers {n}

-- {3} (representative exact quantifier)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{3})
    DEFINE A AS val > 0
);

-- Range quantifiers {n,}

-- {2,} (representative n or more)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2,})
    DEFINE A AS val > 40
);

-- Upper bound quantifiers {,m}

-- {,3} (representative up to m)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{,3})
    DEFINE A AS val > 0
);

-- Range quantifiers {n,m}

-- {3,7} (representative range)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_quant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{3,7})
    DEFINE A AS val > 0
);

DROP TABLE rpr_quant;

-- Reluctant quantifiers
CREATE TABLE rpr_reluctant (id INT, val INT);
INSERT INTO rpr_reluctant VALUES (1, 10), (2, 20), (3, 30);

-- A greedy quantifier followed by a reluctant one over the same variable must
-- not be merged: the merged spellings A{2,3} and A{1,4} match all three rows
-- where these stop at two, so merging would change the preferred match.
SELECT id, count(*) OVER w FROM rpr_reluctant
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2} A??) DEFINE A AS TRUE);

SELECT id, count(*) OVER w FROM rpr_reluctant
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{1,2} A{0,2}?) DEFINE A AS TRUE);

-- cascade: the reluctant middle VAR must stop the merge on both sides, where
-- the merged A{1,3} would match all three rows
SELECT id, count(*) OVER w FROM rpr_reluctant
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A? A?? A) DEFINE A AS TRUE);

-- *? (zero or more, reluctant)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A*?)
    DEFINE A AS val > 0
);

-- +? (one or more, reluctant)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+?)
    DEFINE A AS val > 0
);

-- ?? (zero or one, reluctant)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A??)
    DEFINE A AS val > 0
);

-- {n,}? (n or more, reluctant)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2,}?)
    DEFINE A AS val > 0
);

-- {n,m}? (n to m, reluctant)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1,3}?)
    DEFINE A AS val > 0
);

-- {n}? (exactly n): min == max, so the reluctant flag is cleared and the
-- plan is indistinguishable from A{2}.  A fixed count has no shorter match,
-- so the result is the same either way.
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2}?)
    DEFINE A AS val > 0
);

-- {,m}? (up to m, reluctant)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{,3}?)
    DEFINE A AS val > 0
);

-- {2}+ (should be {2}? not {2}+)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2}+)
    DEFINE A AS val > 0
);

-- {2,}* (should be {2,}? not {2,}*)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2,}*)
    DEFINE A AS val > 0
);

-- {,3}* (should be {,3}? not {,3}*)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{,3}*)
    DEFINE A AS val > 0
);

-- {1,3}+ (should be {1,3}? not {1,3}+)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1,3}+)
    DEFINE A AS val > 0
);

-- Boundary errors in reluctant quantifiers

-- negative bound is not allowed
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{-1}?)
    DEFINE A AS val > 0
);

-- ERROR: quantifier bound exceeds limits
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2147483647}?)
    DEFINE A AS val > 0
);

-- negative lower bound is not allowed
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{-1,}?)
    DEFINE A AS val > 0
);

-- ERROR: quantifier lower bound exceeds limits
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2147483647,}?)
    DEFINE A AS val > 0
);

-- zero upper bound is not allowed
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{,0}?)
    DEFINE A AS val > 0
);

-- ERROR: {,2147483647}? (upper bound in range exceeds limits)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{,2147483647}?)
    DEFINE A AS val > 0
);

-- ERROR: {-1,3}? (negative lower bound in range is not allowed)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{-1,3}?)
    DEFINE A AS val > 0
);

-- ERROR: {1,2147483647}? (upper bound in range exceeds limits)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1,2147483647}?)
    DEFINE A AS val > 0
);

-- ERROR: {5,3}? (min > max is not allowed)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{5,3}?)
    DEFINE A AS val > 0
);

-- Token-separated reluctant quantifiers (space between quantifier and ?)
-- Whitespace between the quantifier and "?" is insignificant: each form below
-- returns exactly what its unseparated counterpart above returns.

-- * ? (token separated)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A* ?)
    DEFINE A AS val > 0
);

-- + ? (token separated)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ ?)
    DEFINE A AS val > 0
);

-- {2,} ? (token separated)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2,} ?)
    DEFINE A AS val > 0
);

-- * + (invalid combination)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A* +)
    DEFINE A AS val > 0
);

-- + * (invalid combination)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ *)
    DEFINE A AS val > 0
);

-- ? ? (parsed as ?? reluctant quantifier)
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A? ?)
    DEFINE A AS val > 0
);

-- Two operator tokens where the first is not "?": the offending one is the
-- second, so that is what gets named and what the cursor points at
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A *? ?)
    DEFINE A AS val > 0
);

-- A first token ending in "|" is a quantifier plus the alternation operator,
-- so what follows it has to be a pattern and an Op never is one.  The report
-- names the alternation rather than the pair: "A* ?" is accepted, so naming
-- that pair would describe something the grammar takes.
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A *| ?)
    DEFINE A AS val > 0
);

-- the same for a glued two-character quantifier, where naming the pair would
-- have to invent "*?" out of "*?|"
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A *?| ??)
    DEFINE A AS val > 0
);

-- A first token that is no quantifier at all is itself the offending one, so
-- it is reported the same way as when it stands alone
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A ?+ ?)
    DEFINE A AS val > 0
);

-- The two tokens are reported separately, so the report cannot glue them into
-- a spelling that was never typed
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A ?? || B)
    DEFINE A AS val > 0, B AS val > 1
);

-- The trailing "|" belongs to the alternation, not to the quantifier, so the
-- report drops it
SELECT COUNT(*) OVER w
FROM rpr_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A ?? ?|B)
    DEFINE A AS val > 0, B AS val > 1
);

DROP TABLE rpr_reluctant;

-- Quantifier boundary conditions

CREATE TABLE rpr_bounds (id INT);
INSERT INTO rpr_bounds VALUES (1), (2);

-- ERROR: quantifier lower bound must not exceed upper bound
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{5,3})
    DEFINE A AS id > 0
);

-- Large bounds
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1000,2000})
    DEFINE A AS id > 0
);

-- Very large bound
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{100000})
    DEFINE A AS id > 0
);

-- INT_MAX - 1 = 2147483646 (at limit)
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2147483646})
    DEFINE A AS id > 0
);

-- ERROR: quantifier bound exceeds limits
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2147483647})
    DEFINE A AS id > 0
);

-- {n,} boundary errors

-- ERROR: negative lower bound in {n,} is not allowed
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{-1,})
    DEFINE A AS id > 0
);

-- ERROR: quantifier lower bound exceeds limits
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2147483647,})
    DEFINE A AS id > 0
);

-- {,m} boundary errors

-- Zero upper bound in {,m}
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{,0})
    DEFINE A AS id > 0
);

-- ERROR: quantifier upper bound exceeds limits
SELECT COUNT(*) OVER w
FROM rpr_bounds
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{,2147483647})
    DEFINE A AS id > 0
);

DROP TABLE rpr_bounds;

-- Pattern element-count boundary (maximum 32767 elements, the FIN marker
-- included).  Alternating distinct variables stop the optimizer from merging
-- consecutive elements, so each "A B" pair contributes two elements.
-- ECHO is silenced so the generated multi-thousand-token patterns do not flood
-- the expected output.
--   16383 pairs         -> 32766 + 1 FIN = 32767 = maximum, accepted.
--   16383 pairs + one A -> 32767 + 1 FIN = 32768 > maximum, rejected.
\set ECHO none
SELECT format($$SELECT count(*) OVER w FROM (SELECT 1 i) t
  WINDOW w AS (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  INITIAL PATTERN (%s) DEFINE A AS TRUE, B AS TRUE)$$,
  repeat('A B ', 16383)) \gexec
SELECT format($$SELECT count(*) OVER w FROM (SELECT 1 i) t
  WINDOW w AS (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  INITIAL PATTERN (%s A) DEFINE A AS TRUE, B AS TRUE)$$,
  repeat('A B ', 16383)) \gexec
\set ECHO all

-- ============================================================
-- Navigation Functions Tests (PREV / NEXT / FIRST / LAST)
-- ============================================================
CREATE TEMP TABLE rpr_nav0 (id int, v int);
INSERT INTO rpr_nav0 SELECT g, g*10 FROM generate_series(1, 5) g;

-- Two concurrently open portals of the SAME cached generic plan, with
-- different offset parameters.
--
-- The parameterized cursor 'c' compiles to one plpgsql statement -> one SPI
-- cached plan.  The recursive call OPENs a second portal of that same plan
-- (with a different offset) while the outer portal is already started but has
-- not yet FETCHed.
CREATE OR REPLACE FUNCTION rpr_nested(p_off int, depth int)
RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
  c CURSOR (o int) FOR
    SELECT id, count(*) OVER w AS cnt
    FROM rpr_nav0
    WINDOW w AS (ORDER BY id
                 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                 PATTERN (A)
                 DEFINE A AS PREV(v, o) IS NULL);
  r record;
BEGIN
  OPEN c(p_off);
  IF depth > 0 THEN
    RETURN QUERY SELECT * FROM rpr_nested(p_off + 2, depth - 1);
  END IF;

  LOOP
    FETCH c INTO r;
    EXIT WHEN NOT FOUND;
    RETURN NEXT format('off=%s id=%s cnt=%s', p_off, r.id, r.cnt);
  END LOOP;
  CLOSE c;
END $$;

SET plan_cache_mode = force_generic_plan;
SELECT * FROM rpr_nested(1, 1);
RESET plan_cache_mode;

DROP FUNCTION rpr_nested(int, int);

CREATE TABLE rpr_nav (id INT, val INT);
INSERT INTO rpr_nav VALUES
    (1, 10), (2, 20), (3, 15), (4, 25), (5, 30);

-- Avoid evaluating the inner argument expression when the target row is out
-- of range or does not exist.  PREV misses on the first row of the partition
-- and NEXT on the last, so the two directions are checked separately.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(val is not null) is null);

SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS NEXT(val is not null) is null);

-- FIRST and LAST cannot miss under PATTERN (A): the match is one row long, so
-- the target is the current row and the slot swap is elided.  These two run
-- that path; the query further down is what pins what it writes.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS LAST(val is not null) is null);

SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS FIRST(val is not null) is null);

-- The elided path has to report the target row as present, since resnull may
-- still be null from a scan whose navigation missed.  Here the navigation is
-- the whole DEFINE, so nothing overwrites resnull in between, and inlining the
-- function makes the offset a parameter: 1 misses, then 0 elides.
CREATE FUNCTION rpr_nav_off(k int) RETURNS SETOF bigint LANGUAGE sql STABLE AS $$
  SELECT count(*) OVER w FROM rpr_nav WHERE id = 1
  WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
               PATTERN (A) DEFINE A AS PREV(val > 0, k)) $$;
SELECT o.k, f FROM (VALUES (1), (0)) o(k), LATERAL rpr_nav_off(o.k) f;
DROP FUNCTION rpr_nav_off(int);

-- Under a longer pattern FIRST and LAST can miss.  On the first row of a
-- match currentpos equals match_start, so an offset of one falls outside the
-- match in either direction, and the compound forms miss on their inner
-- navigation, which is a separate bound from the one the plain forms cross.
-- The argument does not propagate null, so a missing row is told apart from
-- a row of nulls.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS LAST(val is not null, 1) is null);

SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS FIRST(val is not null, 1) is null);

SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS PREV(FIRST(val is not null, 1), 2) is null);

SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS PREV(LAST(val is not null, 1), 2) is null);

-- Not a duplicate of the PREV(val is not null) case: the argument here is
-- true where that one is false, and a missing row still has to win over both.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(val is null) is null);

-- Skipping the argument is data-dependent: restricted to the row PREV misses
-- on, a division by zero in it never runs; over the whole table NEXT reaches
-- a row for all but the last and it does.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t WHERE id = 1
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(val / 0) > 0);

SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS NEXT(val / 0) > 0);

-- A constant subexpression of the argument is folded away, so the pattern
-- does not recompute it per row.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS PREV(val + 2 * 3) > 0);

-- Folding a constant subexpression can raise where the whole argument would
-- not have: unlike val / 0 above, 1 / 0 does not depend on the row, so it is
-- reached at plan time even on the row PREV misses on.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav t
WHERE id = 1
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(val + 1 / 0) > 0);

-- Here the null reaches the DEFINE predicate itself instead of an IS NULL
-- An all-NULL target row would have made v IS NULL true and matched the
-- first row, so this pins the predicate side of the same behaviour.
WITH t(id, v) AS (VALUES (1, 10), (2, 20))
SELECT id, count(*) OVER w AS cnt
FROM t
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(v IS NULL));

-- Pulling up the VALUES substitutes 10 for v, which is not what the column
-- stood for under a navigation: the argument reads the row the navigation
-- lands on, not this one.  The replacement is wrapped in a PlaceHolderVar
-- rather than folded through.
WITH t(id, v) AS (VALUES (1, 10))
SELECT id, count(*) OVER w AS cnt
FROM t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(v IS NULL));

-- That wrapping is what keeps this one from raising: the divisor is constant
-- but the dividend is not folded through, so the division stands until
-- execution, where PREV has no row to navigate to and never reaches it.
WITH t(id, v) AS (VALUES (1, 10))
SELECT id, count(*) OVER w AS cnt
FROM t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(v / 0) > 0);

-- A pulled-up subquery and a function RTE that folded to a constant reach a
-- navigation argument the same way, so both are wrapped as well.
SELECT id, count(*) OVER w AS cnt
FROM (SELECT 1 AS id, 10 AS v) t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(v / 0) > 0);

SELECT count(*) OVER w AS cnt
FROM abs(-10) AS v
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS PREV(v / 0) > 0);

-- Only the argument is protected.  One level outside the navigation the same
-- column is replaced and folded as it is anywhere else, and the division
-- raises at plan time -- as it does for the same WHERE clause over the same
-- one-row VALUES, with no pattern in sight.
WITH t(id, v) AS (VALUES (1, 10))
SELECT id, count(*) OVER w AS cnt
FROM t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A) DEFINE A AS v / 0 > 0);

-- A replacement that still depends on the row is left unwrapped, because it
-- is what the column meant at whichever row the navigation lands on.
SELECT id, count(*) OVER w AS cnt
FROM (SELECT id, val + 1 AS v FROM rpr_nav) t
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS PREV(v) > 0);

-- Nesting: the inner navigation's argument is below the outer one's, so the
-- column there is wrapped too.
WITH t(id, v) AS (VALUES (1, 10))
SELECT id, count(*) OVER w AS cnt
FROM t
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A) DEFINE A AS PREV(LAST(v / 0, 1), 2) > 0);

-- eval_const_expressions() must perform a few rewrites on every expression it
-- is handed -- a CollateExpr becomes a RelabelType, named arguments become
-- positional, omitted defaults are inserted -- and the executor depends on
-- all three, so they are mandatory, not optimizations.  Each of the three
-- below reaches the executor only if those rewrites reach inside a navigation
-- argument, and each returns what the same expression one level outside the
-- navigation returns.
CREATE TABLE rpr_nav_txt (id int, s text);
INSERT INTO rpr_nav_txt VALUES (1, 'b'), (2, 'c'), (3, 'a');
CREATE FUNCTION rpr_nav_named(a int, b int) RETURNS int
    LANGUAGE sql IMMUTABLE AS 'SELECT $1 * 10 + $2';
CREATE FUNCTION rpr_nav_dflt(a int, b int DEFAULT 100) RETURNS int
    LANGUAGE sql IMMUTABLE AS 'SELECT $2';

-- COLLATE under a navigation: the executor has no CollateExpr step, so the
-- RelabelType rewrite has to reach here.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav_txt
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS PREV(s COLLATE "C") > 'a');

-- Named arguments under a navigation: the executor has no NamedArgExpr step.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS PREV(rpr_nav_named(b => 7, a => val)) > 0);

-- An omitted default under a navigation: without the insertion the call is
-- initialized with one fewer argument than the callee reads.
SELECT id, count(*) OVER w AS cnt
FROM rpr_nav
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+) DEFINE A AS PREV(rpr_nav_dflt(val)) = 100);

DROP FUNCTION rpr_nav_dflt(int, int);
DROP FUNCTION rpr_nav_named(int, int);
DROP TABLE rpr_nav_txt;

-- A navigation offset is resolved once at the top of the scan, before any
-- input row has been read, so it must not be matched to the window input the
-- way the navigated argument is.  These two spell the offset the same as a
-- window ORDER BY key and as a GROUP BY expression, which is what makes the
-- match available.
CREATE TABLE rpr_navoff (id int, val int);
INSERT INTO rpr_navoff VALUES (1, 10), (2, 20), (3, 15), (4, 30), (5, 5);

SELECT id, val, count(*) OVER w AS cnt
FROM rpr_navoff
WINDOW w AS (ORDER BY (extract(hour from localtimestamp)::int * 0 + 1), id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE B AS val > PREV(val, (extract(hour from localtimestamp)::int * 0 + 1)));

-- Control: an offset that matches nothing in the window input.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_navoff
WINDOW w AS (ORDER BY (extract(hour from localtimestamp)::int * 0 + 1), id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE B AS val > PREV(val, (extract(hour from localtimestamp)::int * 0 + 2)));

SELECT id, val, count(*) OVER w AS cnt
FROM rpr_navoff
GROUP BY GROUPING SETS ((id, val, ((random() * 0)::bigint + 1)),
                        (id,      ((random() * 0)::bigint + 1)))
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE B AS val > PREV(val, (random() * 0)::bigint + 1))
ORDER BY id, val;

DROP TABLE rpr_navoff;

-- PREV function - reference previous row in pattern
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE
        A AS val > 0,
        B AS val > PREV(val)
);

-- NEXT function - reference next row in pattern
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ B)
    DEFINE
        A AS val < NEXT(val),
        B AS val > 0
);

-- Combined PREV and NEXT
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B C)
    DEFINE
        A AS val > 0,
        B AS val > PREV(val) AND val < NEXT(val),
        C AS val > PREV(val)
);

-- PREV function cannot be used other than in DEFINE
SELECT PREV(id), id, val, COUNT(*) OVER w as cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE
        A AS val > 0,
        B AS val > PREV(val)
);

-- NEXT function cannot be used other than in DEFINE
SELECT NEXT(id), id, val, COUNT(*) OVER w as cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE
        A AS val > 0,
        B AS val > PREV(val)
);

-- FIRST function - reference match_start row
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE
        A AS val > 0,
        B AS val > FIRST(val)
);

-- LAST function without offset - equivalent to current row's value
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE
        A AS val > 0,
        B AS LAST(val) > PREV(val)
);

-- FIRST and LAST combined
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_nav
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE
        A AS val > 0,
        B AS val > FIRST(val) AND LAST(val) > PREV(val)
);

-- FIRST function cannot be used other than in DEFINE
SELECT FIRST(id), id, val FROM rpr_nav;

-- LAST function cannot be used other than in DEFINE
SELECT LAST(id), id, val FROM rpr_nav;

DROP TABLE rpr_nav;

-- Name-space: prev/next/first/last are navigation functions,
-- not ordinary functions
CREATE SCHEMA rpr_navns;
SET search_path TO rpr_navns, public;
CREATE TABLE rpr_nav_rows (g text, id int, val int);
INSERT INTO rpr_nav_rows VALUES ('x', 1, 100), ('x', 2, 200), ('x', 3, 150),
                      ('x', 4, 140), ('x', 5, 150);

-- Outside DEFINE these are ordinary identifiers and resolve to nothing
SELECT prev(val) FROM rpr_nav_rows;
SELECT next(val) FROM rpr_nav_rows;
SELECT prev(val, 2) FROM rpr_nav_rows;
SELECT next(val, 2) FROM rpr_nav_rows;
SELECT first(val) FROM rpr_nav_rows;
SELECT last(val) FROM rpr_nav_rows;
SELECT first(val, 1) FROM rpr_nav_rows;
-- A schema-qualified call is also a plain (failing) function lookup
SELECT pg_catalog.prev(val) FROM rpr_nav_rows;

-- Outside DEFINE, a user-defined function of that name is callable
CREATE FUNCTION next(numeric) RETURNS numeric AS 'SELECT -999::numeric'
  LANGUAGE sql IMMUTABLE;
SELECT next(10);

-- Inside DEFINE, unqualified PREV is nav whether or not a user prev() exists
SELECT id, val, count(*) OVER w AS cnt, last_value(id) OVER w AS last_id
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (START UP+)
    DEFINE START AS TRUE, UP AS val > PREV(val))
  ORDER BY id;

-- A qualified call invokes the function, so its volatility still matters
-- VOLATILE: unqualified is nav; qualified is rejected unless it folds away
CREATE FUNCTION prev(integer) RETURNS integer
  LANGUAGE plpgsql VOLATILE AS 'BEGIN RETURN -999; END';
SELECT id, val, count(*) OVER w AS cnt, last_value(id) OVER w AS last_id
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (START UP+)
    DEFINE START AS TRUE, UP AS val > PREV(val))
  ORDER BY id;
SELECT id, val, count(*) OVER w AS cnt, last_value(id) OVER w AS last_id
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+)
    DEFINE A AS rpr_navns.prev(val) = -999)
  ORDER BY id;
-- The SQL body inlines and folds to a constant, so no volatile call
-- is left for the check to find
CREATE OR REPLACE FUNCTION prev(integer) RETURNS integer AS 'SELECT -999'
  LANGUAGE sql VOLATILE;
SELECT id, val, count(*) OVER w AS cnt, last_value(id) OVER w AS last_id
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+)
    DEFINE A AS rpr_navns.prev(val) = -999)
  ORDER BY id;

-- No OVER references the window, so flattening the subquery drops it
-- before the check runs, the same way an unreferenced CTE is never planned
SELECT id FROM (
 SELECT id FROM rpr_nav_rows
 WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS random() > 0.5)) s
ORDER BY id;

-- ERROR: OFFSET 0 keeps the subquery, so the subquery is planned and the
-- check reaches its DEFINE before anything settles that no OVER references
-- the window, just as for an unreferenced window at the top level
SELECT id FROM (
 SELECT id FROM rpr_nav_rows
 WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS random() > 0.5) OFFSET 0) sub;

-- ERROR: an OVER referencing the window keeps the subquery without OFFSET 0,
-- and its DEFINE is checked the same way
SELECT id, c FROM (
 SELECT id, count(*) OVER w AS c FROM rpr_nav_rows
 WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS random() > 0.5)) sub;

-- The same query with WHERE false makes the subquery rel dummy, so the
-- planner never plans it and nothing looks at its DEFINE
SELECT id, c FROM (
 SELECT id, count(*) OVER w AS c FROM rpr_nav_rows
 WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS random() > 0.5)) sub
WHERE false;

-- The window runs, but the volatile is in a dead CASE arm that folds away
-- before the check, so nothing volatile is left for the check to find
SELECT id, count(*) OVER w AS c FROM rpr_nav_rows
 WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS CASE WHEN false THEN random()::int > 0
                                  ELSE val > 5 END)
ORDER BY id;

-- ERROR: folding can splice in a volatile that parse analysis never saw -- a
-- STABLE function whose default argument is volatile -- and the check runs
-- late enough to catch it
CREATE FUNCTION rpr_off_leak(n bigint DEFAULT (random() * 5)::bigint)
  RETURNS bigint LANGUAGE sql STABLE AS 'SELECT n';
SELECT count(*) OVER w FROM generate_series(1, 100) g(v)
  WINDOW w AS (ORDER BY v ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS v > PREV(v, rpr_off_leak()));
DROP FUNCTION rpr_off_leak(bigint);


-- A UNION ALL leaf is flattened like any other subquery, so its
-- unreferenced window goes the same way
SELECT id FROM (
 SELECT id FROM rpr_nav_rows
 WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS random() > 0.5)
 UNION ALL
 SELECT id FROM rpr_nav_rows) s;

-- An unreferenced CTE is never planned, so nothing looks at its
-- DEFINE
WITH unused AS (
 SELECT id FROM rpr_nav_rows
 WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS random() > 0.5))
SELECT 1;

-- ERROR: referencing it plans the CTE, and the check reaches the DEFINE there
WITH used AS (
 SELECT id FROM rpr_nav_rows
 WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS random() > 0.5))
SELECT count(*) FROM used;

DROP FUNCTION prev(integer);
-- IMMUTABLE: unqualified is nav; qualified is the escape hatch and succeeds
CREATE FUNCTION prev(integer) RETURNS integer AS 'SELECT -999'
  LANGUAGE sql IMMUTABLE;
SELECT id, val, count(*) OVER w AS cnt, last_value(id) OVER w AS last_id
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (START UP+)
    DEFINE START AS TRUE, UP AS val > PREV(val))
  ORDER BY id;
-- (val).prev is attribute notation,
-- so it calls the ordinary function prev(val)
-- (the IMMUTABLE user prev here), the same as the schema-qualified call below
SELECT id, val, count(*) OVER w AS cnt, last_value(id) OVER w AS last_id
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+)
    DEFINE A AS (val).prev = -999)
  ORDER BY id;
SELECT id, val, count(*) OVER w AS cnt, last_value(id) OVER w AS last_id
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+)
    DEFINE A AS rpr_navns.prev(val) = -999)
  ORDER BY id;

-- Zero or more than two arguments is an error, with no function fallback
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV() IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(val, 1, 2) IS NULL);
-- the error stands even when a user function of that exact arity exists
CREATE FUNCTION prev(integer, integer, integer) RETURNS integer
  AS 'SELECT -999' LANGUAGE sql IMMUTABLE;
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(val, 1, 2) IS NULL);
DROP FUNCTION prev(integer, integer, integer);

-- Syntactic decoration is rejected
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(*) IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(DISTINCT val) IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(val ORDER BY val) IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(val) FILTER (WHERE true) IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(val) WITHIN GROUP (ORDER BY val) IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(val) OVER () IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(VARIADIC ARRAY[val]) IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS prev(x => val) IS NULL);
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS PREV(val) IGNORE NULLS IS NULL);

-- Quoting does not escape: "prev" is nav, "PREV" is an ordinary name
SELECT id, val, count(*) OVER w AS cnt
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (START UP+)
    DEFINE START AS TRUE, UP AS val > "prev"(val))
  ORDER BY id;
SELECT count(*) OVER w FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS "PREV"(val) IS NULL);

-- A view round-trips: bare PREV stays a navigation function, and a qualified
-- user prev() stays schema-qualified so it does not reparse as navigation
CREATE VIEW rpr_navns_nav AS
  SELECT id, count(*) OVER w AS cnt FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (START UP+) DEFINE START AS TRUE, UP AS val > PREV(val));
CREATE VIEW rpr_navns_fn AS
  SELECT id, count(*) OVER w AS cnt FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS rpr_navns.prev(val) = -999);
SELECT pg_get_viewdef('rpr_navns_nav');
SELECT pg_get_viewdef('rpr_navns_fn');
DROP VIEW rpr_navns_nav, rpr_navns_fn;

-- A qualified last() in DEFINE must stay schema-qualified on deparse so that
-- it does not reparse as the LAST navigation function (force-qualify path)
CREATE FUNCTION rpr_navns.last(integer) RETURNS integer AS 'SELECT -999' LANGUAGE sql IMMUTABLE;
CREATE VIEW rpr_navns_fn_last AS
  SELECT id, count(*) OVER w AS cnt FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS rpr_navns.last(val) = -999);
SELECT pg_get_viewdef('rpr_navns_fn_last');
DROP VIEW rpr_navns_fn_last;
DROP FUNCTION rpr_navns.last(integer);

-- Attribute notation is never a navigation call; it resolves to a field or
-- to an ordinary function
CREATE TYPE rpr_navns_pair AS (first int, last int);
CREATE TABLE rpr_composite_rows (id int, p rpr_navns_pair);
INSERT INTO rpr_composite_rows VALUES (1, (10, 20)), (2, (30, 40));
SELECT (p).last FROM rpr_composite_rows ORDER BY id;
SELECT count(*) OVER w FROM rpr_composite_rows
  WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS (p).last > 0);
SELECT count(*) OVER w FROM rpr_composite_rows
  WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+) DEFINE A AS (p).prev > 0);

-- Navigation offset must not contain a navigation operation
SELECT id, val
  FROM rpr_nav_rows
  WINDOW w AS (PARTITION BY g ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING INITIAL
    PATTERN (A+)
    DEFINE A AS PREV(val, FIRST(1)) > 0)
  ORDER BY id;

DROP SCHEMA rpr_navns CASCADE;
RESET search_path;

-- ============================================================
-- SKIP TO / INITIAL Tests
-- ============================================================

CREATE TABLE rpr_skip (id INT, val INT);
INSERT INTO rpr_skip VALUES
    (1, 1), (2, 2), (3, 3), (4, 4), (5, 5),
    (6, 6), (7, 7), (8, 8);

-- SKIP TO NEXT ROW

-- SKIP TO NEXT ROW
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_skip
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C)
    DEFINE A AS val > 0, B AS val > 2, C AS val > 4
);

-- SKIP PAST LAST ROW

-- SKIP PAST LAST ROW
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_skip
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C)
    DEFINE A AS val > 0, B AS val > 2, C AS val > 4
);

-- Default behavior (should be SKIP PAST LAST ROW)

-- No SKIP TO clause (default)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_skip
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B)
    DEFINE A AS val > 0, B AS val > 1
);

-- Compare default with explicit PAST LAST ROW
-- Results should be identical
WITH default_skip AS (
    SELECT id, val, COUNT(*) OVER w as cnt
    FROM rpr_skip
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B C)
        DEFINE A AS val > 0, B AS val > 2, C AS val > 4
    )
),
explicit_skip AS (
    SELECT id, val, COUNT(*) OVER w as cnt
    FROM rpr_skip
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (A B C)
        DEFINE A AS val > 0, B AS val > 2, C AS val > 4
    )
)
SELECT 'default' as type, * FROM default_skip
UNION ALL
SELECT 'explicit' as type, * FROM explicit_skip
ORDER BY type, id;

DROP TABLE rpr_skip;

CREATE TABLE rpr_init (id INT, val INT);
INSERT INTO rpr_init VALUES (1, 10), (2, 20), (3, 30), (4, 40);

-- Explicit INITIAL
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_init
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Implicit INITIAL (default)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_init
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

DROP TABLE rpr_init;

-- SEEK

CREATE TABLE rpr_seek (id INT, val INT);
INSERT INTO rpr_seek VALUES (1, 10);

-- SEEK keyword, SEEK mode is not supported
SELECT COUNT(*) OVER w
FROM rpr_seek
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    SEEK
    PATTERN (A+)
    DEFINE A AS val > 0
);

DROP TABLE rpr_seek;

-- PERMUTE

CREATE TABLE rpr_permute (id INT, val INT);
INSERT INTO rpr_permute VALUES (1, 10);

-- PERMUTE syntax is recognized, but the feature is not supported
SELECT COUNT(*) OVER w
FROM rpr_permute
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (PERMUTE(A))
    DEFINE A AS val > 0
);

-- rejected the same way for a list, and for sub-patterns of any shape
SELECT COUNT(*) OVER w
FROM rpr_permute
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (PERMUTE(A+ B, C | D))
    DEFINE A AS val > 0, B AS val > 1, C AS val > 2, D AS val > 3
);

-- PERMUTE stays unreserved, so it is still usable as a pattern variable
SELECT COUNT(*) OVER w
FROM rpr_permute
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (PERMUTE A)
    DEFINE PERMUTE AS val > 5, A AS val > 0
);

-- Except immediately before a group: PERMUTE shifts on "(" whether or not a
-- comma follows, so such a variable lands on the not-supported error, and for
-- that user the alternations advice is beside the point.  The hint has to name
-- the way out too.
SELECT COUNT(*) OVER w
FROM rpr_permute
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (PERMUTE (A | B))
    DEFINE PERMUTE AS val > 5, A AS val > 0, B AS val > 9
);

-- quoted, the same query runs
SELECT COUNT(*) OVER w
FROM rpr_permute
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ("permute" (A | B))
    DEFINE PERMUTE AS val > 5, A AS val > 0, B AS val > 9
);

-- Deparse must quote such a variable, or a view holding one would reparse as
-- the PERMUTE syntax; other variables stay unquoted
CREATE VIEW rpr_permute_v AS
  SELECT COUNT(*) OVER w AS cnt FROM rpr_permute
  WINDOW w AS (
      ORDER BY id
      ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      PATTERN ("permute" (A))
      DEFINE PERMUTE AS val > 5, A AS val > 0
  );
SELECT pg_get_viewdef('rpr_permute_v'::regclass);

-- Quoted even where no group follows: the deparser quotes the name wherever
-- it appears in PATTERN rather than looking ahead for the "(" that would
-- make it ambiguous
CREATE VIEW rpr_permute_v2 AS
  SELECT COUNT(*) OVER w AS cnt FROM rpr_permute
  WINDOW w AS (
      ORDER BY id
      ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      PATTERN (PERMUTE A)
      DEFINE PERMUTE AS val > 5, A AS val > 0
  );
SELECT pg_get_viewdef('rpr_permute_v2'::regclass);

-- EXPLAIN deparses the compiled pattern with a printer of its own, so it has
-- to quote the same names ruleutils does.  The alternation keeps the group
-- from being flattened away, which is what puts a "(" after the variable.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w
FROM rpr_permute
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ("permute" (A | B))
    DEFINE PERMUTE AS val > 5, A AS val > 0, B AS val > 9
);

DROP VIEW rpr_permute_v, rpr_permute_v2;
DROP TABLE rpr_permute;

-- ============================================================
-- Serialization/Deserialization Tests
-- ============================================================
-- RPR-defining views and tables here that are not dropped explicitly are
-- intentionally left in place so that pg_dump/pg_upgrade exercise the
-- deparse-then-re-parse round-trip of the RPR window clause.

-- View creation and deparsing

CREATE TABLE rpr_serial (id INT, val INT);
INSERT INTO rpr_serial VALUES
    (1, 10), (2, 20), (3, 15), (4, 25), (5, 30);

-- Simple pattern
CREATE VIEW rpr_serial_v1 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_serial
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Verify view works (tests deserialization)
SELECT * FROM rpr_serial_v1 ORDER BY id;

-- Verify deparsing
SELECT pg_get_viewdef('rpr_serial_v1'::regclass);

-- Complex pattern with alternation
CREATE VIEW rpr_serial_v2 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_serial
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ | B*)
    DEFINE A AS val > 20, B AS val <= 20
);

SELECT * FROM rpr_serial_v2 ORDER BY id;
SELECT pg_get_viewdef('rpr_serial_v2'::regclass);

-- Pattern with grouping and quantifiers
CREATE VIEW rpr_serial_v3 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_serial
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B){2,5} | C*)
    DEFINE
        A AS val > 10,
        B AS val > 20,
        C AS val <= 10
);

SELECT * FROM rpr_serial_v3 ORDER BY id;
SELECT pg_get_viewdef('rpr_serial_v3'::regclass);

-- All features combined
CREATE VIEW rpr_serial_v4 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_serial
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    INITIAL
    PATTERN (START (MID{1,3} | ALT+) FINISH)
    DEFINE
        START AS val > 5,
        MID AS val BETWEEN 10 AND 25,
        ALT AS val > 25,
        FINISH AS val > 15
);

SELECT * FROM rpr_serial_v4 ORDER BY id;
SELECT pg_get_viewdef('rpr_serial_v4'::regclass);

-- Additional quantifiers for deparsing coverage

-- ? quantifier (zero or one)
CREATE VIEW rpr_serial_v5 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_serial
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B?)
    DEFINE A AS val > 10, B AS val > 20
);

SELECT * FROM rpr_serial_v5 ORDER BY id;
SELECT pg_get_viewdef('rpr_serial_v5'::regclass);

-- {n,} quantifier (n or more)
CREATE VIEW rpr_serial_v6 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_serial
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2,})
    DEFINE A AS val > 15
);

SELECT * FROM rpr_serial_v6 ORDER BY id;
SELECT pg_get_viewdef('rpr_serial_v6'::regclass);

-- {n} quantifier (exactly n)
CREATE VIEW rpr_serial_v7 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_serial
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{3})
    DEFINE A AS val > 0
);

SELECT * FROM rpr_serial_v7 ORDER BY id;
SELECT pg_get_viewdef('rpr_serial_v7'::regclass);

-- Nested ALT pattern (tests deparse of complex nested structure)
CREATE VIEW rpr_serial_v8 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_serial
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A+ B) | C) D | A B C)
    DEFINE A AS val <= 15, B AS val <= 25, C AS val <= 30, D AS val > 30
);

SELECT * FROM rpr_serial_v8 ORDER BY id;
SELECT pg_get_viewdef('rpr_serial_v8'::regclass);

-- Navigation function serialization: PREV with offset
CREATE VIEW rpr_serial_nav1 AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE A AS TRUE, B AS val > PREV(val, 2));
SELECT pg_get_viewdef('rpr_serial_nav1'::regclass);

-- Navigation function serialization: FIRST and LAST
CREATE VIEW rpr_serial_nav2 AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE A AS TRUE, B AS FIRST(val) < LAST(val, 1));
SELECT pg_get_viewdef('rpr_serial_nav2'::regclass);

-- Navigation function serialization: compound PREV(FIRST())
CREATE VIEW rpr_serial_nav3 AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE A AS TRUE, B AS PREV(FIRST(val, 1), 2) > 0);
SELECT pg_get_viewdef('rpr_serial_nav3'::regclass);

-- Navigation function serialization: compound NEXT(LAST())
CREATE VIEW rpr_serial_nav4 AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE A AS TRUE, B AS NEXT(LAST(val), 2) IS NOT NULL);
SELECT pg_get_viewdef('rpr_serial_nav4'::regclass);

-- Navigation function serialization: compound PREV(LAST())
CREATE VIEW rpr_serial_nav5 AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE A AS TRUE, B AS PREV(LAST(val, 1), 2) > 0);
SELECT pg_get_viewdef('rpr_serial_nav5'::regclass);

-- Navigation function serialization: compound NEXT(FIRST())
CREATE VIEW rpr_serial_nav6 AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE A AS TRUE, B AS NEXT(FIRST(val), 3) > 0);
SELECT pg_get_viewdef('rpr_serial_nav6'::regclass);

-- Pretty deparse: navigation calls are function-like and take no extra parens
CREATE VIEW rpr_nav_pretty_v AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+)
             DEFINE A AS TRUE,
                    B AS val > PREV(val) AND PREV(val) IS NOT NULL
                         AND NEXT(val) > FIRST(val)
                         AND PREV(FIRST(val)) > 0);
SELECT pg_get_viewdef('rpr_nav_pretty_v'::regclass, true);

-- Reluctant {1}? quantifier deparse through ruleutils
CREATE VIEW rpr_quant_reluctant_v AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL
             PATTERN (A{1}? B)
             DEFINE A AS val > 0, B AS val > 0);
SELECT pg_get_viewdef('rpr_quant_reluctant_v'::regclass);

-- Quoted identifier round-trip: mixed-case names need quoting
CREATE VIEW rpr_serial_quoted AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ("Start" "Up"+)
             DEFINE "Start" AS TRUE, "Up" AS val > PREV(val));
SELECT pg_get_viewdef('rpr_serial_quoted'::regclass);

-- Quoting the deparser adds on its own: permute is unreserved, so the stored
-- rule holds a plain name and only the deparser knows it has to come back
-- quoted.  Restoring this view is what proves it does.
CREATE VIEW rpr_serial_permute AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ("permute" (A | B))
             DEFINE PERMUTE AS val > 0, A AS val > 10, B AS val > 20);
SELECT pg_get_viewdef('rpr_serial_permute'::regclass);

-- Inline OVER round-trip: inline window spec (no WINDOW alias) deparses
-- inside OVER (...)
CREATE VIEW rpr_serial_inline_over AS
SELECT id, val,
       count(*) OVER (ORDER BY id
                      ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                      PATTERN (A B+)
                      DEFINE A AS val > 10, B AS val > PREV(val)) AS cnt
FROM rpr_serial;
SELECT pg_get_viewdef('rpr_serial_inline_over'::regclass);

-- Multi-relation view: a DEFINE column is deparsed with no qualifier, so it
-- must stay unambiguous across the join for the view to re-parse.  This one
-- is left in place like the rpr_serial views above, which is what puts an
-- unqualified DEFINE column through the pg_dump round trip at all.
CREATE TABLE rpr_serial_j (id INT, qty INT);
INSERT INTO rpr_serial_j VALUES (1, 5), (2, 7), (3, 9), (4, 11), (5, 13);

CREATE VIEW rpr_serial_join AS
SELECT s.id, count(*) OVER w AS cnt
FROM rpr_serial s JOIN rpr_serial_j j ON s.id = j.id
WINDOW w AS (ORDER BY s.id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (UP+) DEFINE UP AS val > 0);
SELECT pg_get_viewdef('rpr_serial_join'::regclass);

-- A DEFINE clause can only name a column without a qualifier, so the name has
-- to resolve exactly as printed.  When another relation of the query acquires
-- a column of that name, the deparser pushes the newcomer aside with a column
-- alias list, the same way it protects a column merged by USING.

CREATE TABLE rpr_pin (id INT, val INT);
CREATE TABLE rpr_pin_other (id INT);
INSERT INTO rpr_pin VALUES (1, 10), (2, 20), (3, 15);
INSERT INTO rpr_pin_other VALUES (1), (2), (3);

CREATE VIEW rpr_pin_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_pin, rpr_pin_other
WHERE rpr_pin.id = rpr_pin_other.id
WINDOW w AS (ORDER BY rpr_pin.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS val > 0);

-- names reached through a navigation operation are pinned too
CREATE VIEW rpr_pin_nav_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_pin, rpr_pin_other
WHERE rpr_pin.id = rpr_pin_other.id
WINDOW w AS (ORDER BY rpr_pin.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS PREV(val) < val);

-- no collision yet, so no column alias list
SELECT pg_get_viewdef('rpr_pin_v'::regclass, true);

ALTER TABLE rpr_pin_other ADD COLUMN val INT;

SELECT pg_get_viewdef('rpr_pin_v'::regclass, true);
SELECT pg_get_viewdef('rpr_pin_nav_v'::regclass, true);

-- and the deparsed text builds an identical view
CREATE VIEW rpr_pin_v2 AS
 SELECT count(*) OVER w AS cnt
   FROM rpr_pin,
    rpr_pin_other rpr_pin_other(id, val_1)
  WHERE rpr_pin.id = rpr_pin_other.id
  WINDOW w AS (ORDER BY rpr_pin.id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  AFTER MATCH SKIP PAST LAST ROW
  INITIAL
  PATTERN (a+)
  DEFINE
  a AS val > 0);

SELECT pg_get_viewdef('rpr_pin_v'::regclass, true)
     = pg_get_viewdef('rpr_pin_v2'::regclass, true) AS identical;

-- The hazard this section guards against cannot be written in the first
-- place: a whole-row reference through a row constructor is rejected in
-- DEFINE, so no view can carry one as far as the deparser.
CREATE VIEW rpr_pin_row_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_pin, rpr_pin_other
WHERE rpr_pin.id = rpr_pin_other.id
WINDOW w AS (ORDER BY rpr_pin.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS ROW(rpr_pin.*) IS NOT NULL);

-- a column merged by USING is pinned the same way
CREATE TABLE rpr_pin_l (x INT, y INT);
CREATE TABLE rpr_pin_r (x INT, z INT);
CREATE TABLE rpr_pin_x (id INT);

CREATE VIEW rpr_pin_using_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_pin_l JOIN rpr_pin_r USING (x), rpr_pin_x
WINDOW w AS (ORDER BY rpr_pin_l.y
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS x > 0);

ALTER TABLE rpr_pin_x ADD COLUMN x INT;

SELECT pg_get_viewdef('rpr_pin_using_v'::regclass, true);

-- a JOIN ... ON behaves the same, and the view keeps returning its rows
CREATE TABLE rpr_pin_j1 (id INT, price INT);
CREATE TABLE rpr_pin_j2 (id INT, qty INT);
INSERT INTO rpr_pin_j1 VALUES (1, 10), (2, 20);
INSERT INTO rpr_pin_j2 VALUES (1, 5), (2, 7);

CREATE VIEW rpr_pin_on_v AS
SELECT j1.id, count(*) OVER w AS cnt
FROM rpr_pin_j1 j1 JOIN rpr_pin_j2 j2 ON j1.id = j2.id
WINDOW w AS (ORDER BY j1.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS price > 0);

ALTER TABLE rpr_pin_j2 ADD COLUMN price INT;

SELECT pg_get_viewdef('rpr_pin_on_v'::regclass, true);
SELECT * FROM rpr_pin_on_v ORDER BY id;

-- The same query written fresh is rejected, since nothing pins the name for
-- it.  Pinning is what lets the stored rpr_pin_on_v definition still reparse.
SELECT j1.id, count(*) OVER w AS cnt
FROM rpr_pin_j1 j1 JOIN rpr_pin_j2 j2 ON j1.id = j2.id
WINDOW w AS (ORDER BY j1.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS price > 0);

-- An aliased join hides its inputs, so the name that gets printed is
-- the join's own, taken from varnosyn, not the child column the Var
-- carries in varno.
-- Pinning the child instead would reserve a name that never reaches the output
-- and leave the printed one free for a later column to collide with.
CREATE TABLE rpr_pin_a (i INT, x INT);
CREATE TABLE rpr_pin_b (k INT, y INT);
CREATE TABLE rpr_pin_c (m INT);
INSERT INTO rpr_pin_a VALUES (1, 10), (2, 20);
INSERT INTO rpr_pin_b VALUES (1, 5), (2, 7);
INSERT INTO rpr_pin_c VALUES (100), (200);

CREATE VIEW rpr_pin_alias_v AS
SELECT count(*) OVER w AS cnt
FROM (rpr_pin_a JOIN rpr_pin_b ON rpr_pin_a.i = rpr_pin_b.k) j(p, q, r, s),
     rpr_pin_c
WINDOW w AS (ORDER BY rpr_pin_c.m
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A)
             DEFINE A AS q > 0);

ALTER TABLE rpr_pin_c ADD COLUMN q INT;

SELECT pg_get_viewdef('rpr_pin_alias_v'::regclass, true);
SELECT * FROM rpr_pin_alias_v;

-- and the deparsed text builds a view that returns the same rows
CREATE VIEW rpr_pin_alias_v2 AS
SELECT count(*) OVER w AS cnt
FROM (rpr_pin_a JOIN rpr_pin_b ON rpr_pin_a.i = rpr_pin_b.k) j(p, q, r, s),
     rpr_pin_c rpr_pin_c(m, q_1)
WINDOW w AS (ORDER BY rpr_pin_c.m
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW
             INITIAL
             PATTERN (a)
             DEFINE a AS q > 0);
SELECT * FROM rpr_pin_alias_v2;
DROP VIEW rpr_pin_alias_v2;

-- Without a user column alias list the join still keeps the printed name, and
-- the input relation is the one that moves aside.
CREATE VIEW rpr_pin_alias_v3 AS
SELECT count(*) OVER w AS cnt
FROM (rpr_pin_a JOIN rpr_pin_b ON rpr_pin_a.i = rpr_pin_b.k) j, rpr_pin_c
WINDOW w AS (ORDER BY rpr_pin_c.m
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A)
             DEFINE A AS x > 0);
SELECT pg_get_viewdef('rpr_pin_alias_v3'::regclass, true);

DROP VIEW rpr_pin_alias_v3;
DROP VIEW rpr_pin_alias_v;
DROP TABLE rpr_pin_a, rpr_pin_b, rpr_pin_c;

-- A relation RTE prints the column alias the user wrote, not the catalog name,
-- so the alias is the name to reserve.  Pinning the catalog name would replace
-- the alias in the printed text and push aside an unrelated column that
-- collides only with a name nobody prints.
CREATE TABLE rpr_pin_d (i INT, k INT);
CREATE TABLE rpr_pin_e (m INT);
INSERT INTO rpr_pin_d VALUES (1, 10), (2, 20);
INSERT INTO rpr_pin_e VALUES (100), (200);

CREATE VIEW rpr_pin_rel_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_pin_d d(p, q), rpr_pin_e
WINDOW w AS (ORDER BY rpr_pin_e.m
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A)
             DEFINE A AS q > 0);

-- a column named after the catalog name is no collision: q is what is printed
ALTER TABLE rpr_pin_e ADD COLUMN k INT;

SELECT pg_get_viewdef('rpr_pin_rel_v'::regclass, true);

-- one named after the alias is, and moves aside
ALTER TABLE rpr_pin_e ADD COLUMN q INT;

SELECT pg_get_viewdef('rpr_pin_rel_v'::regclass, true);
SELECT * FROM rpr_pin_rel_v;

DROP VIEW rpr_pin_rel_v;
DROP TABLE rpr_pin_d, rpr_pin_e;

-- A name a DEFINE clause reads is the one name in the query that cannot be
-- spelled any other way, the qualifier slot being reserved for a pattern
-- variable.  set_using_names() picks the name of every column merged by USING
-- before it, and is free to pick any name at all, renaming the merged inputs
-- to match.  So the DEFINE names that are settled already are reserved first
-- and the merged names are chosen around them.  Here the second USING would
-- otherwise reach for x_1, the very column the DEFINE clause reads: the
-- anonymous FULL JOIN forces USING names to be unique query-wide, which takes
-- plain x, and x_1 is what the next one counts up to.
CREATE TABLE rpr_res_t (x_1 INT, id INT);
CREATE TABLE rpr_res_l1 (x INT);
CREATE TABLE rpr_res_r1 (x INT);
CREATE TABLE rpr_res_l2 (x INT);
CREATE TABLE rpr_res_r2 (x INT);
INSERT INTO rpr_res_t VALUES (1, 1), (2, 2);
INSERT INTO rpr_res_l1 VALUES (1);
INSERT INTO rpr_res_r1 VALUES (1);
INSERT INTO rpr_res_l2 VALUES (1);
INSERT INTO rpr_res_r2 VALUES (1);

CREATE VIEW rpr_res_using_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_t,
     (rpr_res_l1 FULL JOIN rpr_res_r1 USING (x)),
     (rpr_res_l2 JOIN rpr_res_r2 USING (x))
WINDOW w AS (ORDER BY rpr_res_t.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS x_1 > 0);

SELECT pg_get_viewdef('rpr_res_using_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_using_rt AS '
       || pg_get_viewdef('rpr_res_using_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_using_v'::regclass, true)
       = pg_get_viewdef('rpr_res_using_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_using_v;
SELECT * FROM rpr_res_using_rt;

DROP VIEW rpr_res_using_rt, rpr_res_using_v;
DROP TABLE rpr_res_t, rpr_res_l1, rpr_res_r1, rpr_res_l2, rpr_res_r2;

-- A merged column keeps its natural name and collides all the same.  This one
-- is the column set_relation_column_names() cannot push aside afterwards: its
-- name was settled and handed to both inputs before that function ran, so the
-- loop there passes over it.  An ordinary column in its place does move aside,
-- which is what the rpr_pin views above cover.
CREATE TABLE rpr_res_a (j INT, p INT);
CREATE TABLE rpr_res_b (j INT, q INT);
CREATE TABLE rpr_res_c (r INT, s INT);
INSERT INTO rpr_res_a VALUES (1, 10);
INSERT INTO rpr_res_b VALUES (1, 30);
INSERT INTO rpr_res_c VALUES (1, 10), (2, 20), (3, 15);

CREATE VIEW rpr_res_merged_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_a JOIN rpr_res_b USING (j) CROSS JOIN rpr_res_c
WINDOW w AS (ORDER BY rpr_res_c.s
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL PATTERN (X Y+)
             DEFINE X AS true, Y AS s > PREV(s));

-- the collision arrives only now: the merged column has been named j all along
ALTER TABLE rpr_res_c RENAME COLUMN s TO j;

SELECT pg_get_viewdef('rpr_res_merged_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_merged_rt AS '
       || pg_get_viewdef('rpr_res_merged_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_merged_v'::regclass, true)
       = pg_get_viewdef('rpr_res_merged_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_merged_v;
SELECT * FROM rpr_res_merged_rt;

DROP VIEW rpr_res_merged_rt, rpr_res_merged_v;
DROP TABLE rpr_res_a, rpr_res_b, rpr_res_c;

-- Naming a merged column renames the columns it merges, and the name can land
-- in an RTE that has a real column of that name already.  The column the
-- DEFINE clause reads is the one that cannot move, so the merge counts past it
-- instead and the RTE prints two distinct aliases.
CREATE TABLE rpr_res_fa (k INT);
CREATE TABLE rpr_res_fb (k INT);
CREATE TABLE rpr_res_ga (k INT, k_1 INT);
CREATE TABLE rpr_res_gb (k INT);
CREATE TABLE rpr_res_ord (id INT);
INSERT INTO rpr_res_fa VALUES (1);
INSERT INTO rpr_res_fb VALUES (1);
INSERT INTO rpr_res_ga VALUES (1, 5);
INSERT INTO rpr_res_gb VALUES (1);
INSERT INTO rpr_res_ord VALUES (1), (2);

CREATE VIEW rpr_res_dup_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_fa FULL JOIN rpr_res_fb USING (k),
     rpr_res_ga JOIN rpr_res_gb USING (k),
     rpr_res_ord
WINDOW w AS (ORDER BY rpr_res_ord.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS k_1 > 0);

SELECT pg_get_viewdef('rpr_res_dup_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_dup_rt AS '
       || pg_get_viewdef('rpr_res_dup_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_dup_v'::regclass, true)
       = pg_get_viewdef('rpr_res_dup_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_dup_v;
SELECT * FROM rpr_res_dup_rt;

DROP VIEW rpr_res_dup_rt, rpr_res_dup_v;
DROP TABLE rpr_res_fa, rpr_res_fb, rpr_res_ga, rpr_res_gb, rpr_res_ord;

-- A merged column a DEFINE clause reads is named by the DEFINE clause: its
-- name is settled before set_using_names() runs, and the USING clause adopts
-- it rather than inventing one.  The merged name therefore stays id here,
-- and the newcomer is the one that moves aside.
CREATE TABLE rpr_res_p (id INT, v INT);
CREATE TABLE rpr_res_q (id INT, w INT);
CREATE TABLE rpr_res_s (n INT);
INSERT INTO rpr_res_p VALUES (1, 10), (2, 20);
INSERT INTO rpr_res_q VALUES (1, 30), (3, 40);
INSERT INTO rpr_res_s VALUES (7);

CREATE VIEW rpr_res_full_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_p FULL JOIN rpr_res_q USING (id), rpr_res_s
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS id > 0);

ALTER TABLE rpr_res_s ADD COLUMN id INT;

SELECT pg_get_viewdef('rpr_res_full_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_full_rt AS '
       || pg_get_viewdef('rpr_res_full_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_full_v'::regclass, true)
       = pg_get_viewdef('rpr_res_full_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_full_v;
SELECT * FROM rpr_res_full_rt;

DROP VIEW rpr_res_full_rt, rpr_res_full_v;
DROP TABLE rpr_res_p, rpr_res_q, rpr_res_s;

-- An aliased join answers for its inputs.  The parser gives every column of
-- such a join the join's own varnosyn, so a DEFINE clause reading a column the
-- inputs brought in names the join and not the relation it came from.  The
-- name is settled all the same, the join's USING clause not being the thing
-- that produced it, and so it is reserved: a column of a join that its own
-- USING clause does not name is no more merged than a relation's would be.
-- Left unreserved, the merged name counts up onto it and the join prints the
-- one alias twice, which reparses as an ambiguous column.
CREATE TABLE rpr_res_ja (x INT, x_1 INT);
CREATE TABLE rpr_res_jb (x INT, z INT);
CREATE TABLE rpr_res_m1 (x INT);
CREATE TABLE rpr_res_m2 (x INT);
CREATE TABLE rpr_res_ordj (id INT);
INSERT INTO rpr_res_ja VALUES (1, 7);
INSERT INTO rpr_res_jb VALUES (1, 9);
INSERT INTO rpr_res_m1 VALUES (1);
INSERT INTO rpr_res_m2 VALUES (1);
INSERT INTO rpr_res_ordj VALUES (1), (2);

CREATE VIEW rpr_res_alias_v AS
SELECT count(*) OVER w AS cnt
FROM (rpr_res_m1 FULL JOIN rpr_res_m2 USING (x)),
     (rpr_res_ja JOIN rpr_res_jb USING (x)) AS jx,
     rpr_res_ordj
WINDOW w AS (ORDER BY rpr_res_ordj.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS x_1 > 0);

SELECT pg_get_viewdef('rpr_res_alias_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_alias_rt AS '
       || pg_get_viewdef('rpr_res_alias_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_alias_v'::regclass, true)
       = pg_get_viewdef('rpr_res_alias_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_alias_v;
SELECT * FROM rpr_res_alias_rt;


-- The same through NATURAL JOIN, which names no column in the query text.  The
-- test above reads a join's USING clause to tell a merged column from one that
-- only passes through, and the analyzed tree is where it reads it: the parser
-- works out which columns NATURAL merges and files them there.
CREATE VIEW rpr_res_nat_v AS
SELECT count(*) OVER w AS cnt
FROM (rpr_res_m1 FULL JOIN rpr_res_m2 USING (x)),
     (rpr_res_ja NATURAL JOIN rpr_res_jb) AS jx,
     rpr_res_ordj
WINDOW w AS (ORDER BY rpr_res_ordj.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS x_1 > 0);

SELECT pg_get_viewdef('rpr_res_nat_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_nat_rt AS '
       || pg_get_viewdef('rpr_res_nat_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_nat_v'::regclass, true)
       = pg_get_viewdef('rpr_res_nat_rt'::regclass, true) AS round_trips;

DROP VIEW rpr_res_nat_rt, rpr_res_nat_v;
DROP VIEW rpr_res_alias_rt, rpr_res_alias_v;
DROP TABLE rpr_res_ja, rpr_res_jb, rpr_res_m1, rpr_res_m2, rpr_res_ordj;

-- A column a function's result type grows after the view is made is one the
-- deparser does not see at all: expandRTE() stops at the column count the
-- query was parsed with.  A column it cannot see is one it cannot rename out
-- of the way, and the name it collides with here is the one a DEFINE clause
-- has to resolve to as printed.  So the grown columns are looked up and named
-- too, and printed in full, the alias list being positional.
CREATE TABLE rpr_res_fn (id INT, val INT);
INSERT INTO rpr_res_fn VALUES (1, 1), (2, 2), (3, 3);
CREATE TABLE rpr_res_cfg (a INT);
INSERT INTO rpr_res_cfg VALUES (1);
CREATE FUNCTION rpr_res_fcfg() RETURNS SETOF rpr_res_cfg LANGUAGE sql
  AS $$ SELECT * FROM rpr_res_cfg $$;

CREATE VIEW rpr_res_fn_v AS
SELECT rpr_res_fn.id, count(*) OVER w AS cnt
FROM rpr_res_fn, rpr_res_fcfg() f
WINDOW w AS (ORDER BY rpr_res_fn.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS val > 0);

-- nothing to keep off yet
SELECT pg_get_viewdef('rpr_res_fn_v'::regclass, true);

ALTER TABLE rpr_res_cfg ADD COLUMN val INT;

SELECT pg_get_viewdef('rpr_res_fn_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_fn_rt AS '
       || pg_get_viewdef('rpr_res_fn_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_fn_v'::regclass, true)
       = pg_get_viewdef('rpr_res_fn_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_fn_v;
SELECT * FROM rpr_res_fn_rt;

-- a grown column that collides with nothing is still on the list
ALTER TABLE rpr_res_cfg ADD COLUMN spare INT;
SELECT pg_get_viewdef('rpr_res_fn_v'::regclass, true);

DROP VIEW rpr_res_fn_rt, rpr_res_fn_v;
DROP FUNCTION rpr_res_fcfg();
DROP TABLE rpr_res_fn, rpr_res_cfg;

-- A system column is named from the catalog, not from the deparser's own
-- choice, so there is no alias to pick for it and nothing to exempt from
-- renaming.  Its name still has to be held against the rest of the query, or a
-- column that turns up later answers to it as well.  The function builds its
-- row from the type as it stands when called, so it still returns one after
-- the type grows, with NULL in the grown column; a DEFINE clause that read
-- that column instead of rpr_res_sys.ctid would match no row.
CREATE TABLE rpr_res_sys (id INT, v INT);
INSERT INTO rpr_res_sys VALUES (1, 1), (2, 2);
CREATE TYPE rpr_res_ct AS (a INT);
CREATE FUNCTION rpr_res_fct() RETURNS SETOF rpr_res_ct LANGUAGE sql
  AS $$ SELECT * FROM json_populate_record(NULL::rpr_res_ct, '{"a": 1}') $$;

CREATE VIEW rpr_res_sys_v AS
SELECT rpr_res_sys.id, count(*) OVER w AS cnt
FROM rpr_res_sys, rpr_res_fct() f
WINDOW w AS (ORDER BY rpr_res_sys.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS ctid IS NOT NULL);

ALTER TYPE rpr_res_ct ADD ATTRIBUTE ctid INT;

SELECT pg_get_viewdef('rpr_res_sys_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_sys_rt AS '
       || pg_get_viewdef('rpr_res_sys_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_sys_v'::regclass, true)
       = pg_get_viewdef('rpr_res_sys_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_sys_v;
SELECT * FROM rpr_res_sys_rt;

DROP VIEW rpr_res_sys_rt, rpr_res_sys_v;
DROP FUNCTION rpr_res_fct();
DROP TYPE rpr_res_ct;
DROP TABLE rpr_res_sys;

-- Four more corners of the same deparse handling.
--
-- An INNER JOIN USING merges to a plain Var of the left input, not to a
-- COALESCE, so there is no merge expression for the DEFINE clause to be
-- collapsed onto; the grouping still makes the deparser look for one.
CREATE TABLE rpr_cov_l (id INT, v INT);
CREATE TABLE rpr_cov_r (id INT, w INT);
INSERT INTO rpr_cov_l VALUES (1, 1), (2, 2), (3, 3);
INSERT INTO rpr_cov_r VALUES (1, 1), (2, 2), (3, 3);

CREATE VIEW rpr_cov_inner_v AS
SELECT id + 1 AS idp1, count(*) OVER w AS cnt
FROM rpr_cov_l JOIN rpr_cov_r USING (id)
GROUP BY id + 1
WINDOW w AS (ORDER BY id + 1
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS id + 1 > 0);

SELECT pg_get_viewdef('rpr_cov_inner_v'::regclass, true);
SELECT 'CREATE VIEW rpr_cov_inner_rt AS '
       || pg_get_viewdef('rpr_cov_inner_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_cov_inner_v'::regclass, true)
       = pg_get_viewdef('rpr_cov_inner_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_cov_inner_v ORDER BY idp1;
SELECT * FROM rpr_cov_inner_rt ORDER BY idp1;

DROP VIEW rpr_cov_inner_rt, rpr_cov_inner_v;

-- A DEFINE clause that reads the same system column in two variables holds its
-- name once; the second reference finds it held already.
CREATE VIEW rpr_cov_sys2_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_cov_l
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B)
             DEFINE A AS tableoid > 0, B AS tableoid > 0);

SELECT pg_get_viewdef('rpr_cov_sys2_v'::regclass, true);
SELECT 'CREATE VIEW rpr_cov_sys2_rt AS '
       || pg_get_viewdef('rpr_cov_sys2_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_cov_sys2_v'::regclass, true)
       = pg_get_viewdef('rpr_cov_sys2_rt'::regclass, true) AS round_trips;

DROP VIEW rpr_cov_sys2_rt, rpr_cov_sys2_v;

-- A function with a column definition list has its column set fixed by the
-- list, so no column can have grown since the view was made.
CREATE VIEW rpr_cov_coldef_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_cov_l, json_to_record('{"a": 1}') AS j(a int)
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS a > 0);

SELECT pg_get_viewdef('rpr_cov_coldef_v'::regclass, true);
SELECT 'CREATE VIEW rpr_cov_coldef_rt AS '
       || pg_get_viewdef('rpr_cov_coldef_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_cov_coldef_v'::regclass, true)
       = pg_get_viewdef('rpr_cov_coldef_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_cov_coldef_v;
SELECT * FROM rpr_cov_coldef_rt;

DROP VIEW rpr_cov_coldef_rt, rpr_cov_coldef_v;

-- Once a FULL JOIN USING has a merge expression to collapse, every node of the
-- DEFINE clause is looked at, whatever it is.  A function call that is no
-- merge is left as it is, and a navigation without an offset has an empty
-- offset argument to pass over.
CREATE VIEW rpr_cov_merge_v AS
SELECT id, count(*) OVER w AS cnt
FROM rpr_cov_l FULL JOIN rpr_cov_r USING (id)
GROUP BY id
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS abs(id) > 0 AND PREV(id) IS NULL OR id > 1);

SELECT pg_get_viewdef('rpr_cov_merge_v'::regclass, true);
SELECT 'CREATE VIEW rpr_cov_merge_rt AS '
       || pg_get_viewdef('rpr_cov_merge_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_cov_merge_v'::regclass, true)
       = pg_get_viewdef('rpr_cov_merge_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_cov_merge_v ORDER BY id;
SELECT * FROM rpr_cov_merge_rt ORDER BY id;

DROP VIEW rpr_cov_merge_rt, rpr_cov_merge_v;
DROP TABLE rpr_cov_l, rpr_cov_r;

-- A TABLEFUNC RTE writes its column names into the clause that produces them,
-- but it accepts a column alias list like any other RTE, and a rename of one
-- of its columns is printed there.  So a TABLEFUNC column that comes to
-- answer to the name a DEFINE clause reads is renamed like any other column
-- would be, and the DEFINE clause keeps its spelling.
CREATE TABLE rpr_res_tf (id INT, s INT);
INSERT INTO rpr_res_tf VALUES (1, 1), (2, 2);

CREATE VIEW rpr_res_tf_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_tf,
     JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (c1 int PATH '$')) AS jx
WINDOW w AS (ORDER BY rpr_res_tf.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS s > 0);

-- the collision arrives only now
ALTER TABLE rpr_res_tf RENAME COLUMN s TO c1;

SELECT pg_get_viewdef('rpr_res_tf_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_tf_rt AS '
       || pg_get_viewdef('rpr_res_tf_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_tf_v'::regclass, true)
       = pg_get_viewdef('rpr_res_tf_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_tf_v;
SELECT * FROM rpr_res_tf_rt;

DROP VIEW rpr_res_tf_rt, rpr_res_tf_v;
DROP TABLE rpr_res_tf;

-- An aliased join answers for its inputs and hides them, so the DEFINE
-- clause here reads the join's own column x.  That name is reserved across
-- the query level, which reaches the TABLEFUNC column underneath as well:
-- it is renamed on its own alias list, and the join prints x on its list to
-- keep the name the query sees.  Out of reach of an unqualified reference,
-- the TABLEFUNC column would not have collided, but a reserved name is kept
-- off every RTE of the level, as a globally unique USING name is.
CREATE TABLE rpr_res_hid (id INT, v INT);
CREATE TABLE rpr_res_hu (m INT);
INSERT INTO rpr_res_hid VALUES (1, 1), (2, 2), (3, 3);
INSERT INTO rpr_res_hu VALUES (9);

CREATE VIEW rpr_res_hid_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_hid,
     (JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (x int PATH '$')) AS jt
      JOIN rpr_res_hu ON jt.x > 0) j
WINDOW w AS (ORDER BY rpr_res_hid.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS x > 0);

SELECT pg_get_viewdef('rpr_res_hid_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_hid_rt AS '
       || pg_get_viewdef('rpr_res_hid_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_hid_v'::regclass, true)
       = pg_get_viewdef('rpr_res_hid_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_hid_v;
SELECT * FROM rpr_res_hid_rt;

DROP VIEW rpr_res_hid_rt, rpr_res_hid_v;
DROP TABLE rpr_res_hid, rpr_res_hu;

-- A column can come to carry the name a DEFINE clause reads only after the
-- view is made, by being renamed, and from then on the two have to be told
-- apart in the printed text.  The DEFINE column keeps its spelling, being
-- settled first, and the newcomer gets name_N -- whether it sits in the same
-- RTE or in another one where a DEFINE clause reads it too, is the one a
-- USING clause merges, or is not merged but carries the USING clause's
-- spelling, which is no business of the deparser's to guess from.  A USING
-- clause elsewhere that spells the newcomer's new name moves out of the way
-- as well.
CREATE TABLE rpr_res_ren (c INT, d INT);
INSERT INTO rpr_res_ren VALUES (1, 1), (2, 2);
CREATE TABLE rpr_res_ren2 (a INT, e INT);
INSERT INTO rpr_res_ren2 VALUES (1, 0), (2, 0);
CREATE TABLE rpr_res_renl (a_1 INT);
CREATE TABLE rpr_res_renr (a_1 INT);
INSERT INTO rpr_res_renl VALUES (1);
INSERT INTO rpr_res_renr VALUES (1);
CREATE TABLE rpr_res_rs (x INT);
CREATE TABLE rpr_res_rr (x INT, y INT);
INSERT INTO rpr_res_rs VALUES (1), (2);
INSERT INTO rpr_res_rr VALUES (1, 5), (2, 6);
CREATE TABLE rpr_res_rena (id INT, a INT);
CREATE TABLE rpr_res_renb (id INT, b INT);
INSERT INTO rpr_res_rena VALUES (1, 1), (2, 2);
INSERT INTO rpr_res_renb VALUES (1, 5), (2, 0);

-- the same RTE, its alias list shorter than the table
CREATE VIEW rpr_res_ren_v AS
SELECT x.a, x.d, count(*) OVER w AS cnt
FROM rpr_res_ren AS x(a)
WINDOW w AS (ORDER BY x.a
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (P Q*)
             DEFINE P AS a > 0, Q AS d > 0);

-- the column a USING clause merges
CREATE VIEW rpr_res_renu_v AS
SELECT a, x.d, count(*) OVER w AS cnt
FROM rpr_res_ren AS x(a) JOIN rpr_res_ren2 USING (a)
WINDOW w AS (ORDER BY a
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (P Q*)
             DEFINE P AS d > 0);

-- a USING clause elsewhere that spells the name the newcomer is going to get
CREATE VIEW rpr_res_renx_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_ren AS x(a), rpr_res_renl JOIN rpr_res_renr USING (a_1)
WINDOW w AS (ORDER BY x.a
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (P Q*)
             DEFINE P AS a > 0, Q AS d > 0);

-- a merged column that is renamed away, and a DEFINE column renamed onto
-- the USING clause's spelling
CREATE VIEW rpr_res_renm_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_rs JOIN rpr_res_rr USING (x)
WINDOW w AS (ORDER BY y
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS y > 0);

-- another RTE, both of its columns read by the DEFINE clause
CREATE VIEW rpr_res_reno_v AS
SELECT rpr_res_rena.id, count(*) OVER w AS cnt
FROM rpr_res_rena JOIN rpr_res_renb ON rpr_res_rena.id = rpr_res_renb.id
WINDOW w AS (ORDER BY rpr_res_rena.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (P Q*)
             DEFINE P AS a > 0, Q AS b > 0);

-- the collisions arrive only now
ALTER TABLE rpr_res_ren RENAME d TO a;
ALTER TABLE rpr_res_rr RENAME x TO z;
ALTER TABLE rpr_res_rr RENAME y TO x;
ALTER TABLE rpr_res_renb RENAME b TO a;

SELECT pg_get_viewdef('rpr_res_ren_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_ren_rt AS '
       || pg_get_viewdef('rpr_res_ren_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_ren_v'::regclass, true)
       = pg_get_viewdef('rpr_res_ren_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_ren_v;
SELECT * FROM rpr_res_ren_rt;

SELECT pg_get_viewdef('rpr_res_renu_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_renu_rt AS '
       || pg_get_viewdef('rpr_res_renu_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_renu_v'::regclass, true)
       = pg_get_viewdef('rpr_res_renu_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_renu_v;
SELECT * FROM rpr_res_renu_rt;

SELECT pg_get_viewdef('rpr_res_renx_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_renx_rt AS '
       || pg_get_viewdef('rpr_res_renx_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_renx_v'::regclass, true)
       = pg_get_viewdef('rpr_res_renx_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_renx_v;
SELECT * FROM rpr_res_renx_rt;

SELECT pg_get_viewdef('rpr_res_renm_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_renm_rt AS '
       || pg_get_viewdef('rpr_res_renm_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_renm_v'::regclass, true)
       = pg_get_viewdef('rpr_res_renm_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_renm_v;
SELECT * FROM rpr_res_renm_rt;

SELECT pg_get_viewdef('rpr_res_reno_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_reno_rt AS '
       || pg_get_viewdef('rpr_res_reno_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_reno_v'::regclass, true)
       = pg_get_viewdef('rpr_res_reno_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_reno_v;
SELECT * FROM rpr_res_reno_rt;

DROP VIEW rpr_res_reno_rt, rpr_res_reno_v;
DROP VIEW rpr_res_renm_rt, rpr_res_renm_v;
DROP VIEW rpr_res_renx_rt, rpr_res_renx_v;
DROP VIEW rpr_res_renu_rt, rpr_res_renu_v;
DROP VIEW rpr_res_ren_rt, rpr_res_ren_v;
DROP TABLE rpr_res_ren, rpr_res_ren2, rpr_res_renl, rpr_res_renr;
DROP TABLE rpr_res_rs, rpr_res_rr;
DROP TABLE rpr_res_rena, rpr_res_renb;

-- A merged column an aliased join carries can spell the DEFINE name from the
-- start, and a TABLEFUNC among the join's inputs changes nothing about that:
-- the join's own alias list names the merged column, the inputs answer to
-- it, and the DEFINE column is left alone.  Nor does the order of the FROM
-- list matter.
CREATE TABLE rpr_res_tj (id INT, c1 INT);
INSERT INTO rpr_res_tj VALUES (1, 1), (2, 2);
CREATE TABLE rpr_res_tb (c1 INT, y INT);
INSERT INTO rpr_res_tb VALUES (1, 10), (2, 20);
CREATE TABLE rpr_res_ti (id INT);
INSERT INTO rpr_res_ti VALUES (1), (2);

CREATE VIEW rpr_res_tj_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_tj,
     (JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (c1 int PATH '$')) AS jt
      JOIN rpr_res_tb USING (c1)) AS j(k, m)
WINDOW w AS (ORDER BY rpr_res_tj.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS c1 > 0);

CREATE VIEW rpr_res_tjr_v AS
SELECT count(*) OVER w AS cnt
FROM (JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (c1 int PATH '$')) AS jt
      JOIN rpr_res_tb USING (c1)) AS j(k, m),
     rpr_res_tj
WINDOW w AS (ORDER BY rpr_res_tj.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS c1 > 0);

-- the join's alias list puts the USING spelling on a column it does not
-- merge, and that column is the one the DEFINE clause reads
CREATE VIEW rpr_res_tja_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_ti,
     (JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (c1 int PATH '$')) AS jt
      JOIN rpr_res_tb USING (c1)) AS j(k, c1)
WINDOW w AS (ORDER BY rpr_res_ti.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS c1 > 0);

-- the same with the TABLEFUNC on the right
CREATE VIEW rpr_res_tjb_v AS
SELECT count(*) OVER w AS cnt
FROM (rpr_res_tb JOIN JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (c1 int PATH '$')) AS jt
      USING (c1)) AS j(a, c1)
WINDOW w AS (ORDER BY j.a
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS c1 > 0);

SELECT pg_get_viewdef('rpr_res_tj_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_tj_rt AS '
       || pg_get_viewdef('rpr_res_tj_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_tj_v'::regclass, true)
       = pg_get_viewdef('rpr_res_tj_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_tj_v;
SELECT * FROM rpr_res_tj_rt;

SELECT pg_get_viewdef('rpr_res_tjr_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_tjr_rt AS '
       || pg_get_viewdef('rpr_res_tjr_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_tjr_v'::regclass, true)
       = pg_get_viewdef('rpr_res_tjr_rt'::regclass, true) AS round_trips;

SELECT pg_get_viewdef('rpr_res_tja_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_tja_rt AS '
       || pg_get_viewdef('rpr_res_tja_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_tja_v'::regclass, true)
       = pg_get_viewdef('rpr_res_tja_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_tja_v;
SELECT * FROM rpr_res_tja_rt;

SELECT pg_get_viewdef('rpr_res_tjb_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_tjb_rt AS '
       || pg_get_viewdef('rpr_res_tjb_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_tjb_v'::regclass, true)
       = pg_get_viewdef('rpr_res_tjb_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_tjb_v;
SELECT * FROM rpr_res_tjb_rt;

DROP VIEW rpr_res_tjb_rt, rpr_res_tjb_v;
DROP VIEW rpr_res_tja_rt, rpr_res_tja_v;
DROP VIEW rpr_res_tjr_rt, rpr_res_tjr_v;
DROP VIEW rpr_res_tj_rt, rpr_res_tj_v;
DROP TABLE rpr_res_tj, rpr_res_tb, rpr_res_ti;

-- The name a USING clause is given for an aliased join has to stay clear of
-- the names the join's alias list already carries, and of what a DEFINE
-- clause reads among them.  The anonymous FULL JOIN takes the plain spelling
-- first, so the second USING has to move past both.
CREATE TABLE rpr_res_fa (x INT);
CREATE TABLE rpr_res_fb (x INT);
CREATE TABLE rpr_res_fc (x INT, y INT);
INSERT INTO rpr_res_fa VALUES (1);
INSERT INTO rpr_res_fb VALUES (1);
INSERT INTO rpr_res_fc VALUES (1, 3);

CREATE VIEW rpr_res_fa_v AS
SELECT count(*) OVER w AS cnt
FROM (rpr_res_fa FULL JOIN rpr_res_fb USING (x)),
     ((rpr_res_fc JOIN rpr_res_fa t4 USING (x)) AS j(x, x_1)
      JOIN rpr_res_fb t5 USING (x))
WINDOW w AS (ORDER BY j.x
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS x_1 > 0);

SELECT pg_get_viewdef('rpr_res_fa_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_fa_rt AS '
       || pg_get_viewdef('rpr_res_fa_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_fa_v'::regclass, true)
       = pg_get_viewdef('rpr_res_fa_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_fa_v;
SELECT * FROM rpr_res_fa_rt;

DROP VIEW rpr_res_fa_rt, rpr_res_fa_v;
DROP TABLE rpr_res_fa, rpr_res_fb, rpr_res_fc;

-- A TABLEFUNC that merges through an aliased join, or that carries a column
-- alias list of its own, is no different: when a relation column is renamed
-- onto the TABLEFUNC's name, it is the TABLEFUNC column that moves, and a
-- third RTE that already spells the name it moves to is left alone, that
-- name being read nowhere unqualified.
CREATE TABLE rpr_res_tk (id INT, y INT);
INSERT INTO rpr_res_tk VALUES (1, 1), (2, 2);
CREATE TABLE rpr_res_th (x INT, z INT);
INSERT INTO rpr_res_th VALUES (1, 1), (2, 2);
CREATE TABLE rpr_res_ta (id INT, c1 INT);
INSERT INTO rpr_res_ta VALUES (1, 1), (2, 2);
CREATE TABLE rpr_res_ts (id INT, s INT);
INSERT INTO rpr_res_ts VALUES (1, 1), (2, 2);
CREATE TABLE rpr_res_to (c1_1 INT);
INSERT INTO rpr_res_to VALUES (9);

CREATE VIEW rpr_res_tk_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_tk,
     (JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (x int PATH '$')) AS jt
      JOIN rpr_res_th USING (x)) j
WINDOW w AS (ORDER BY rpr_res_tk.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS y > 0);

CREATE VIEW rpr_res_ta_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_ta,
     JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (c1 int PATH '$')) AS jx(a)
WINDOW w AS (ORDER BY rpr_res_ta.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS c1 > 0);

CREATE VIEW rpr_res_ts_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_ts, rpr_res_to,
     JSON_TABLE(jsonb '[1,2]', '$[*]' COLUMNS (c1 int PATH '$')) AS jx
WINDOW w AS (ORDER BY rpr_res_ts.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS s > 0);

ALTER TABLE rpr_res_tk RENAME y TO x;
ALTER TABLE rpr_res_ts RENAME s TO c1;

SELECT pg_get_viewdef('rpr_res_tk_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_tk_rt AS '
       || pg_get_viewdef('rpr_res_tk_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_tk_v'::regclass, true)
       = pg_get_viewdef('rpr_res_tk_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_tk_v;
SELECT * FROM rpr_res_tk_rt;

SELECT pg_get_viewdef('rpr_res_ta_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_ta_rt AS '
       || pg_get_viewdef('rpr_res_ta_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_ta_v'::regclass, true)
       = pg_get_viewdef('rpr_res_ta_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_ta_v;
SELECT * FROM rpr_res_ta_rt;

SELECT pg_get_viewdef('rpr_res_ts_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_ts_rt AS '
       || pg_get_viewdef('rpr_res_ts_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_ts_v'::regclass, true)
       = pg_get_viewdef('rpr_res_ts_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_ts_v;
SELECT * FROM rpr_res_ts_rt;

DROP VIEW rpr_res_ts_rt, rpr_res_ts_v;
DROP VIEW rpr_res_ta_rt, rpr_res_ta_v;
DROP VIEW rpr_res_tk_rt, rpr_res_tk_v;
DROP TABLE rpr_res_tk, rpr_res_th, rpr_res_ta, rpr_res_ts, rpr_res_to;

-- A merged column the DEFINE clause reads is named by the DEFINE clause, not
-- by the USING clause: the name settled for it is the one the merge adopts.
-- So a join whose alias list renames the merged column prints the same text
-- with the DEFINE clause as without it, and a merge two joins deep is reached
-- through the input the reference resolves to.  A column that turns up later
-- under the same name elsewhere is the one that moves.
CREATE TABLE rpr_res_ma (x INT, y INT);
CREATE TABLE rpr_res_mb (x INT, z INT);
CREATE TABLE rpr_res_mc (x INT, r INT);
CREATE TABLE rpr_res_mo (xx INT);
INSERT INTO rpr_res_ma VALUES (1, 1), (2, 2);
INSERT INTO rpr_res_mb VALUES (1, 1), (2, 2);
INSERT INTO rpr_res_mc VALUES (1, 1), (2, 2);
INSERT INTO rpr_res_mo VALUES (1);

CREATE VIEW rpr_res_ma_v AS
SELECT count(*) OVER w AS cnt
FROM (rpr_res_ma JOIN rpr_res_mb USING (x)) AS j(x1, y, z)
WINDOW w AS (ORDER BY j.y
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS x1 > 0);

CREATE VIEW rpr_res_ma_nodef AS
SELECT count(*) OVER w AS cnt
FROM (rpr_res_ma JOIN rpr_res_mb USING (x)) AS j(x1, y, z)
WINDOW w AS (ORDER BY j.y
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING);

CREATE VIEW rpr_res_mm_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_res_mo, (rpr_res_ma JOIN rpr_res_mb USING (x)) JOIN rpr_res_mc USING (x)
WINDOW w AS (ORDER BY rpr_res_ma.y
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS x > 0);

SELECT pg_get_viewdef('rpr_res_ma_v'::regclass, true);
SELECT pg_get_viewdef('rpr_res_ma_nodef'::regclass, true);
SELECT 'CREATE VIEW rpr_res_ma_rt AS '
       || pg_get_viewdef('rpr_res_ma_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_ma_v'::regclass, true)
       = pg_get_viewdef('rpr_res_ma_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_ma_v;
SELECT * FROM rpr_res_ma_rt;

SELECT pg_get_viewdef('rpr_res_mm_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_mm_rt AS '
       || pg_get_viewdef('rpr_res_mm_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_mm_v'::regclass, true)
       = pg_get_viewdef('rpr_res_mm_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_mm_v;
SELECT * FROM rpr_res_mm_rt;

-- the name turns up elsewhere only now
DROP VIEW rpr_res_mm_rt;
ALTER TABLE rpr_res_mo RENAME xx TO x;

SELECT pg_get_viewdef('rpr_res_mm_v'::regclass, true);
SELECT 'CREATE VIEW rpr_res_mm_rt AS '
       || pg_get_viewdef('rpr_res_mm_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_res_mm_v'::regclass, true)
       = pg_get_viewdef('rpr_res_mm_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_res_mm_v;
SELECT * FROM rpr_res_mm_rt;

DROP VIEW rpr_res_mm_rt, rpr_res_mm_v;
DROP VIEW rpr_res_ma_rt, rpr_res_ma_nodef, rpr_res_ma_v;
DROP TABLE rpr_res_ma, rpr_res_mb, rpr_res_mc, rpr_res_mo;

-- Deparsing a query whose DEFINE clause reads the grouping step expands the
-- clause's GROUP Vars into the grouping expressions, and over a column merged
-- by a FULL JOIN USING that expands to the COALESCE the parser built for the
-- merge.  Printed, both arms come out spelled the same -- a DEFINE clause
-- carries no qualifier -- so COALESCE(id, id) says nothing about where either
-- came from, and re-parsing nests one merged column inside another.  The join
-- RTE still holds what it built, so the expansion is folded back into the
-- merged column itself.
CREATE TABLE rpr_cds_l (id INT PRIMARY KEY, val INT);
CREATE TABLE rpr_cds_r (id INT, val INT);
CREATE TABLE rpr_cds_o (k INT);
INSERT INTO rpr_cds_l VALUES (1, 1), (2, 2);
INSERT INTO rpr_cds_r VALUES (1, 1), (3, 3);
INSERT INTO rpr_cds_o VALUES (1), (2);

CREATE VIEW rpr_cds_v AS
SELECT COALESCE(rpr_cds_l.id, rpr_cds_r.id) + 1 AS idp1, count(*) OVER w AS cnt
FROM rpr_cds_l FULL JOIN rpr_cds_r USING (id)
GROUP BY COALESCE(rpr_cds_l.id, rpr_cds_r.id) + 1
WINDOW w AS (ORDER BY COALESCE(rpr_cds_l.id, rpr_cds_r.id) + 1
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS id + 1 > 0);

SELECT pg_get_viewdef('rpr_cds_v'::regclass, true);
SELECT 'CREATE VIEW rpr_cds_rt AS '
       || pg_get_viewdef('rpr_cds_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_cds_v'::regclass, true)
       = pg_get_viewdef('rpr_cds_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_cds_v ORDER BY idp1;
SELECT * FROM rpr_cds_rt ORDER BY idp1;

-- An outer join above the merge marks the copy the grouping expression
-- carries and not the copy the join RTE keeps.  Neither mark reaches the
-- printed text, so the two still name one column.
CREATE VIEW rpr_cds_null_v AS
SELECT COALESCE(l.id, r.id) + 1 AS idp1, count(*) OVER w AS cnt
FROM rpr_cds_o LEFT JOIN (rpr_cds_l l FULL JOIN rpr_cds_r r USING (id)) ON true
GROUP BY COALESCE(l.id, r.id) + 1
WINDOW w AS (ORDER BY COALESCE(l.id, r.id) + 1
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS id + 1 > 0);

SELECT pg_get_viewdef('rpr_cds_null_v'::regclass, true);
SELECT 'CREATE VIEW rpr_cds_null_rt AS '
       || pg_get_viewdef('rpr_cds_null_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_cds_null_v'::regclass, true)
       = pg_get_viewdef('rpr_cds_null_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_cds_null_v ORDER BY idp1;
SELECT * FROM rpr_cds_null_rt ORDER BY idp1;

-- The same nesting under a join that nulls nothing leaves both copies
-- unmarked, and they still name one column.
CREATE VIEW rpr_cds_inner_v AS
SELECT COALESCE(l.id, r.id) + 1 AS idp1, count(*) OVER w AS cnt
FROM (rpr_cds_l l FULL JOIN rpr_cds_r r USING (id)) JOIN rpr_cds_o ON true
GROUP BY COALESCE(l.id, r.id) + 1
WINDOW w AS (ORDER BY COALESCE(l.id, r.id) + 1
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS id + 1 > 0);

SELECT pg_get_viewdef('rpr_cds_inner_v'::regclass, true);
SELECT 'CREATE VIEW rpr_cds_inner_rt AS '
       || pg_get_viewdef('rpr_cds_inner_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_cds_inner_v'::regclass, true)
       = pg_get_viewdef('rpr_cds_inner_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_cds_inner_v ORDER BY idp1;
SELECT * FROM rpr_cds_inner_rt ORDER BY idp1;

-- A merge built over another merge -- a FULL JOIN USING above a FULL JOIN
-- USING -- expands to a COALESCE over a COALESCE.  The inner one is folded
-- first, so that the outer one is seen whole and folds in turn; folded
-- inside out, it re-parses to itself rather than to twice the nesting.
CREATE VIEW rpr_cds_nest_v AS
SELECT count(*) OVER w AS cnt
FROM (rpr_cds_l FULL JOIN rpr_cds_r USING (id)) FULL JOIN rpr_cds_r t USING (id)
GROUP BY rpr_cds_l.id, rpr_cds_r.id, t.id
WINDOW w AS (ORDER BY rpr_cds_l.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+)
             DEFINE A AS id > 0);

SELECT pg_get_viewdef('rpr_cds_nest_v'::regclass, true);
SELECT 'CREATE VIEW rpr_cds_nest_rt AS '
       || pg_get_viewdef('rpr_cds_nest_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_cds_nest_v'::regclass, true)
       = pg_get_viewdef('rpr_cds_nest_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_cds_nest_v;
SELECT * FROM rpr_cds_nest_rt;

DROP VIEW rpr_cds_nest_rt, rpr_cds_nest_v;
DROP VIEW rpr_cds_inner_rt, rpr_cds_inner_v;
DROP VIEW rpr_cds_null_rt, rpr_cds_null_v;
DROP VIEW rpr_cds_rt, rpr_cds_v;
DROP TABLE rpr_cds_l, rpr_cds_r, rpr_cds_o;

-- A rule deparses with varprefix on no matter what its action query looks
-- like, the range table always holding *OLD* and *NEW*, so a DEFINE clause in
-- one would be printed with qualifiers that the parser rejects outright.
-- get_rule_define() turns the prefix off for the clause, and without that a
-- rule holding a row pattern query could not be restored at all -- not even a
-- single-table one, which is what makes this its own case and not the view's.
CREATE TABLE rpr_rule_t (id INT, val INT);
CREATE TABLE rpr_rule_log (id INT, cnt BIGINT);

CREATE RULE rpr_rule_r AS ON INSERT TO rpr_rule_t DO ALSO
  INSERT INTO rpr_rule_log
    SELECT id, count(*) OVER w FROM rpr_rule_t
    WINDOW w AS (ORDER BY id
                 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                 PATTERN (A+)
                 DEFINE A AS val > 0);

SELECT pg_get_ruledef(oid, true) FROM pg_rewrite WHERE rulename = 'rpr_rule_r';

-- and that text is what has to reparse
CREATE TABLE rpr_rule_saved AS
  SELECT pg_get_ruledef(oid, true) AS def
    FROM pg_rewrite WHERE rulename = 'rpr_rule_r';
DROP RULE rpr_rule_r ON rpr_rule_t;
SELECT def FROM rpr_rule_saved \gexec
SELECT (SELECT def FROM rpr_rule_saved) = pg_get_ruledef(oid, true) AS round_trips
  FROM pg_rewrite WHERE rulename = 'rpr_rule_r';

-- the restored rule still fires.  A rule action is run against the rows the
-- statement supplies as well as the table, so a two-row INSERT gives the
-- window four rows to order by two distinct ids; sort the result on both
-- columns, the pairs within an id being interchangeable.
INSERT INTO rpr_rule_t VALUES (1, 1), (2, 2);
SELECT * FROM rpr_rule_log ORDER BY id, cnt;

DROP TABLE rpr_rule_saved;
DROP TABLE rpr_rule_t, rpr_rule_log;

-- A relation alias that happens to spell a pattern variable is the other way
-- the prefix goes wrong.  Printed as up.price it does not come back as a
-- range variable qualifier, which is merely rejected, but as a pattern
-- variable one, which is rejected by a different rule and with a different
-- message.  Two RTEs are what turns the prefix on.
CREATE TABLE rpr_pvar_a (id INT, price INT);
CREATE TABLE rpr_pvar_b (id INT);
INSERT INTO rpr_pvar_a VALUES (1, 10), (2, 20), (3, 5);
INSERT INTO rpr_pvar_b VALUES (1), (2), (3);

CREATE VIEW rpr_pvar_v AS
SELECT count(*) OVER w AS cnt
FROM rpr_pvar_a up, rpr_pvar_b
WHERE up.id = rpr_pvar_b.id
WINDOW w AS (ORDER BY up.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (up+)
             DEFINE up AS price > 0);

SELECT pg_get_viewdef('rpr_pvar_v'::regclass, true);
SELECT 'CREATE VIEW rpr_pvar_rt AS '
       || pg_get_viewdef('rpr_pvar_v'::regclass, true) \gexec
SELECT pg_get_viewdef('rpr_pvar_v'::regclass, true)
       = pg_get_viewdef('rpr_pvar_rt'::regclass, true) AS round_trips;
SELECT * FROM rpr_pvar_v;
SELECT * FROM rpr_pvar_rt;

DROP VIEW rpr_pvar_rt, rpr_pvar_v;
DROP TABLE rpr_pvar_a, rpr_pvar_b;


-- Materialized view (if supported)

CREATE TABLE rpr_mview (id INT, val INT);
INSERT INTO rpr_mview VALUES (1, 10), (2, 20), (3, 30);

CREATE MATERIALIZED VIEW rpr_mview_v1 AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_mview
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

SELECT * FROM rpr_mview_v1 ORDER BY id;
SELECT pg_get_viewdef('rpr_mview_v1'::regclass);

-- Refresh test
REFRESH MATERIALIZED VIEW rpr_mview_v1;
SELECT * FROM rpr_mview_v1 ORDER BY id;

-- CREATE TABLE AS SELECT with RPR
CREATE TABLE rpr_ctas (id INT, val INT);
INSERT INTO rpr_ctas VALUES (1, 10), (2, 20), (3, 15), (4, 25);

CREATE TABLE rpr_ctas_result AS
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_ctas
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS val > PREV(val)
);
SELECT * FROM rpr_ctas_result ORDER BY id;

-- INSERT INTO ... SELECT with RPR
CREATE TABLE rpr_insert_target (id INT, val INT, cnt BIGINT);
INSERT INTO rpr_insert_target
SELECT id, val, count(*) OVER w
FROM rpr_ctas
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS val > PREV(val)
);
SELECT * FROM rpr_insert_target ORDER BY id;

DROP TABLE rpr_ctas_result;
DROP TABLE rpr_insert_target;
DROP TABLE rpr_ctas;

-- Prepared statements (tests copyfuncs.c via the plan cache)

CREATE TABLE rpr_prep (id INT, val INT);
INSERT INTO rpr_prep VALUES (1, 10), (2, 20), (3, 30);

-- Simple prepared statement
PREPARE rpr_prep_simple AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_prep
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

EXECUTE rpr_prep_simple;
EXECUTE rpr_prep_simple;

DEALLOCATE rpr_prep_simple;

-- Prepared statement with parameters
PREPARE rpr_prep_param(int) AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_prep
WHERE id <= $1
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 10
);

EXECUTE rpr_prep_param(2);
EXECUTE rpr_prep_param(3);

DEALLOCATE rpr_prep_param;

-- Complex prepared statement
PREPARE rpr_prep_complex AS
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_prep
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A B){1,2} | C+)
    DEFINE
        A AS val > 5,
        B AS val > 15,
        C AS val <= 15
);

EXECUTE rpr_prep_complex;
EXECUTE rpr_prep_complex;

DEALLOCATE rpr_prep_complex;

DROP TABLE rpr_prep;

-- CTE and Subquery (tests copyfuncs.c)

CREATE TABLE rpr_copy (id INT, val INT);
INSERT INTO rpr_copy VALUES (1, 10), (2, 20), (3, 30), (4, 40);

-- Simple CTE
WITH rpr_cte AS (
    SELECT id, val, COUNT(*) OVER w as cnt
    FROM rpr_copy
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
)
SELECT * FROM rpr_cte ORDER BY id;

-- CTE with multiple references (not inlined; planned as a CTE scan)
WITH rpr_cte AS (
    SELECT id, val, COUNT(*) OVER w as cnt
    FROM rpr_copy
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 15
    )
)
SELECT c1.id, c1.cnt as cnt1, c2.cnt as cnt2
FROM rpr_cte c1
JOIN rpr_cte c2 ON c1.id = c2.id
ORDER BY c1.id;

-- Subquery in FROM clause
SELECT *
FROM (
    SELECT id, val, COUNT(*) OVER w as cnt
    FROM rpr_copy
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B?)
        DEFINE A AS val > 10, B AS val > 20
    )
) sub
WHERE cnt > 0;

-- Nested subqueries
SELECT *
FROM (
    SELECT *
    FROM (
        SELECT id, val, COUNT(*) OVER w as cnt
        FROM rpr_copy
        WINDOW w AS (
            ORDER BY id
            ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
            PATTERN (A+)
            DEFINE A AS val >= 10
        )
    ) inner_sub
    WHERE cnt > 0
) outer_sub;

DROP TABLE rpr_copy;

-- DISTINCT and set operations (tests equalfuncs.c)

CREATE TABLE rpr_equal (id INT, val INT);
INSERT INTO rpr_equal VALUES (1, 10), (2, 20), (3, 10), (4, 20);

-- DISTINCT with RPR
SELECT DISTINCT cnt
FROM (
    SELECT id, val, COUNT(*) OVER w as cnt
    FROM rpr_equal
    WINDOW w AS (
        ORDER BY val
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP TO NEXT ROW
        PATTERN (A+)
        DEFINE A AS val > 0
    )
) sub
ORDER BY cnt;

-- UNION with RPR in both sides
SELECT id, val, cnt FROM (
    SELECT id, val, COUNT(*) OVER w as cnt
    FROM rpr_equal
    WHERE val = 10
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
) sub1
UNION
SELECT id, val, cnt FROM (
    SELECT id, val, COUNT(*) OVER w as cnt
    FROM rpr_equal
    WHERE val = 20
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
) sub2
ORDER BY id;

-- UNION ALL
SELECT id, cnt FROM (
    SELECT id, COUNT(*) OVER w as cnt
    FROM rpr_equal
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 10
    )
) sub
UNION ALL
SELECT id, cnt FROM (
    SELECT id, COUNT(*) OVER w as cnt
    FROM rpr_equal
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (B+)
        DEFINE B AS val <= 10
    )
) sub
ORDER BY id, cnt;

-- INTERSECT
SELECT id, cnt FROM (
    SELECT id, COUNT(*) OVER w as cnt
    FROM rpr_equal
    WHERE id <= 3
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
) sub1
INTERSECT
SELECT id, cnt FROM (
    SELECT id, COUNT(*) OVER w as cnt
    FROM rpr_equal
    WHERE id >= 2
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
) sub2
ORDER BY id;

DROP TABLE rpr_equal;

-- View with multiple window definitions

CREATE TABLE rpr_multiwin (id INT, val INT);
INSERT INTO rpr_multiwin VALUES (1, 10), (2, 20), (3, 30);

CREATE VIEW rpr_multiwin_v AS
SELECT
    id,
    val,
    COUNT(*) OVER w1 as cnt1,
    COUNT(*) OVER w2 as cnt2
FROM rpr_multiwin
WINDOW
    w1 AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 15
    ),
    w2 AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (B*)
        DEFINE B AS val <= 15
    );

SELECT * FROM rpr_multiwin_v ORDER BY id;
SELECT pg_get_viewdef('rpr_multiwin_v'::regclass);

-- {n} quantifier display in view
CREATE VIEW rpr_quant_n_v AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL
             PATTERN (A{3})
             DEFINE A AS val > 0);
SELECT pg_get_viewdef('rpr_quant_n_v'::regclass);

-- {n,} quantifier display in view
CREATE VIEW rpr_quant_n_plus_v AS
SELECT id, val, count(*) OVER w
FROM rpr_serial
WINDOW w AS (ORDER BY id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL
             PATTERN (A{2,})
             DEFINE A AS val > 0);
SELECT pg_get_viewdef('rpr_quant_n_plus_v'::regclass);

-- ============================================================
-- Glued Quantifier / Alternation Tests
-- ============================================================
CREATE TABLE rpr_glue (id INT, val INT);
INSERT INTO rpr_glue VALUES (1, 5), (2, 8), (3, 9), (4, -1), (5, 6), (6, -2);
-- Quantifier glued to the alternation operator '|' without a space.  The
-- lexer glues the trailing '|' into one Op token; the grammar reattaches it as
-- the lowest-precedence alternation once the surrounding sequence is built.

-- Op-char quantifiers (*, +, ?, *?, +?, ??) glued to '|'.
CREATE VIEW rpr_dp_op AS SELECT
    count(*) OVER w1 AS w1, count(*) OVER w2 AS w2, count(*) OVER w3 AS w3,
    count(*) OVER w4 AS w4, count(*) OVER w5 AS w5, count(*) OVER w6 AS w6
FROM rpr_glue
WINDOW w1 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*|B) DEFINE A AS val > 0, B AS val <= 0),
       w2 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A+|B) DEFINE A AS val > 0, B AS val <= 0),
       w3 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A?|B) DEFINE A AS val > 0, B AS val <= 0),
       w4 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*?|B) DEFINE A AS val > 0, B AS val <= 0),
       w5 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A+?|B) DEFINE A AS val > 0, B AS val <= 0),
       w6 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A??|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_dp_op'), E'\n')) AS line WHERE line ~ 'PATTERN';
DROP VIEW rpr_dp_op;
-- Spaced reference: the fully-spaced canonical forms.  Identical deparse to
-- the glued rpr_dp_op w1/w4 above completes the
-- glued = spaced = mixed equivalence.
CREATE VIEW rpr_dp_spc AS SELECT count(*) OVER w1 AS w1, count(*) OVER w2 AS w2
FROM rpr_glue
WINDOW w1 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A* | B) DEFINE A AS val > 0, B AS val <= 0),
       w2 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*? | B) DEFINE A AS val > 0, B AS val <= 0);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_dp_spc'), E'\n')) AS line WHERE line ~ 'PATTERN';
DROP VIEW rpr_dp_spc;
-- Range quantifiers glued to '|': non-reluctant {n}| (} + char '|') and
-- reluctant {n}?| (} + Op "?|").
CREATE VIEW rpr_dp_rng AS SELECT
    count(*) OVER w1 AS w1, count(*) OVER w2 AS w2, count(*) OVER w3 AS w3, count(*) OVER w4 AS w4,
    count(*) OVER w5 AS w5, count(*) OVER w6 AS w6, count(*) OVER w7 AS w7, count(*) OVER w8 AS w8
FROM rpr_glue
WINDOW w1 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2}|B) DEFINE A AS val > 0, B AS val <= 0),
       w2 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2,}|B) DEFINE A AS val > 0, B AS val <= 0),
       w3 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{,3}|B) DEFINE A AS val > 0, B AS val <= 0),
       w4 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2,3}|B) DEFINE A AS val > 0, B AS val <= 0),
       w5 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2}?|B) DEFINE A AS val > 0, B AS val <= 0),
       w6 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2,}?|B) DEFINE A AS val > 0, B AS val <= 0),
       w7 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{,3}?|B) DEFINE A AS val > 0, B AS val <= 0),
       w8 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2,3}?|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_dp_rng'), E'\n')) AS line WHERE line ~ 'PATTERN';
DROP VIEW rpr_dp_rng;
-- Mixed spacing: a space inside the quantifier with '|' still glued.
-- "A* ?|B" = '*' + Op"?|" = reluctant "A*?" plus alternation.
CREATE VIEW rpr_dp_mix AS SELECT count(*) OVER w1 AS w1, count(*) OVER w2 AS w2, count(*) OVER w3 AS w3
FROM rpr_glue
WINDOW w1 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A* ?|B) DEFINE A AS val > 0, B AS val <= 0),
       w2 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A+ ?|B) DEFINE A AS val > 0, B AS val <= 0),
       w3 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A? ?|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_dp_mix'), E'\n')) AS line WHERE line ~ 'PATTERN';
DROP VIEW rpr_dp_mix;
-- Structure: precedence (| is lowest, so its right operand is the whole
-- following sequence), chaining, concatenation, and grouping.
CREATE VIEW rpr_dp_struct AS SELECT
    count(*) OVER w1 AS w1, count(*) OVER w2 AS w2, count(*) OVER w3 AS w3,
    count(*) OVER w4 AS w4, count(*) OVER w5 AS w5, count(*) OVER w6 AS w6
FROM rpr_glue
WINDOW w1 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*|B C) DEFINE A AS val > 0, B AS val <= 0, C AS val < 100),
       w2 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*|B*|C) DEFINE A AS val > 0, B AS val <= 0, C AS val < 100),
       w3 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A B*|C D) DEFINE A AS val > 0, B AS val <= 0, C AS val < 100, D AS val > 5),
       w4 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A*|B)) DEFINE A AS val > 0, B AS val <= 0),
       w5 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*|(B|C)) DEFINE A AS val > 0, B AS val <= 0, C AS val < 100),
       w6 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A*|B)+) DEFINE A AS val > 0, B AS val <= 0);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_dp_struct'), E'\n')) AS line WHERE line ~ 'PATTERN';
DROP VIEW rpr_dp_struct;
-- Execution semantics (deparse cannot show reluctant shortest-match).  The
-- rpr_glue rows -- A rows 1-3 and 5, B rows 4 and 6 -- show when the '|B'
-- alternative is reachable.  With "*" the first branch always succeeds, so B
-- never fires: the greedy form matches the whole run and the reluctant form
-- matches empty, and on a B row the empty match still outranks B.  With "+"
-- the first branch fails on a B row, so there the B alternative fires; on an
-- A row the greedy form matches the run and the reluctant form one row.
SELECT id, val,
       count(*) OVER gs AS gstar, count(*) OVER rs AS rstar,
       count(*) OVER gp AS gplus, count(*) OVER rp AS rplus
FROM rpr_glue
WINDOW gs AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*|B) DEFINE A AS val > 0, B AS val <= 0),
       rs AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*?|B) DEFINE A AS val > 0, B AS val <= 0),
       gp AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A+|B) DEFINE A AS val > 0, B AS val <= 0),
       rp AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A+?|B) DEFINE A AS val > 0, B AS val <= 0);
-- Patterns that must stay rejected.  "&" is an invalid op; a '|' with an empty
-- side (leading, trailing, doubled, or alone in a group) has no operand; "||"
-- and "*||" are doubled pipes; "A* *|B"/"A* *?|B"/"A{2}*?|B" are doubled
-- quantifiers.
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A&B) DEFINE A AS val > 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*|) DEFINE A AS val > 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*| |B) DEFINE A AS val > 0, B AS val <= 0);
-- the dangling operator is blamed on the element it hangs off,
-- not on the first
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A B*|) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*||B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A||B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*|B|) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (|A) DEFINE A AS val > 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A*|)) DEFINE A AS val > 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A* *|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A* *?|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A? *?|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2}*?|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A{2} *?|B) DEFINE A AS val > 0, B AS val <= 0);
-- Doubled op-char quantifiers lex as one Op token and are unsupported, whether
-- glued to '|' ("**|", "*+|", "???|") or on their own ("**").
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A**|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*+|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A???|B) DEFINE A AS val > 0, B AS val <= 0);
SELECT count(*) OVER w FROM rpr_glue WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A**B) DEFINE A AS val > 0);
DROP TABLE rpr_glue;

-- ============================================================
-- Error Cases Tests
-- ============================================================

DROP TABLE IF EXISTS rpr_err;
CREATE TABLE rpr_err (id INT, val INT);
INSERT INTO rpr_err VALUES (1, 10), (2, 20);

-- Invalid quantifier syntax
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+!)
    DEFINE A AS val > 0
);

-- none of the following queries should be accepted
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A+ !) DEFINE A AS TRUE);
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A+ ?+) DEFINE A AS TRUE);
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A* ?+) DEFINE A AS TRUE);
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A? ??) DEFINE A AS TRUE);
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A {1,2}??) DEFINE A AS TRUE);

-- none of the following 4 range-quantifier queries should be accepted
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A{2} !) DEFINE A AS TRUE);
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A{2,} !) DEFINE A AS TRUE);
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A{,3} !) DEFINE A AS TRUE);
SELECT FROM rpr_err WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A{2,3} !) DEFINE A AS TRUE);

-- Unmatched parentheses
SET client_min_messages = NOTICE;
DO $$
BEGIN
    EXECUTE 'SELECT COUNT(*) OVER w FROM rpr_err WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A B) DEFINE A AS val > 0, B AS val > 10)';
    RAISE NOTICE 'Unmatched parentheses: UNEXPECTED SUCCESS';
EXCEPTION
    WHEN syntax_error THEN
        RAISE NOTICE 'Unmatched parentheses: EXPECTED ERROR - %', SQLERRM;
    WHEN OTHERS THEN
        RAISE NOTICE 'Unmatched parentheses: UNEXPECTED ERROR - %', SQLERRM;
END $$;
SET client_min_messages = WARNING;

-- ERROR: empty DEFINE not allowed
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE
);

-- ERROR: empty PATTERN not allowed
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ()
    DEFINE A AS val > 0
);

-- ERROR: DEFINE without PATTERN (PATTERN and DEFINE must be used together)
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    DEFINE A AS val > 0
);

-- Qualified column references (NOT SUPPORTED)

-- Pattern variable qualified name: not supported
-- (valid per ISO/IEC 19075-5 6.15 / 4.16, not yet implemented)
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS A.val > 0
);

-- PATTERN-only variable qualified name:
-- not supported even without DEFINE entry
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ B+)
    DEFINE A AS B.val > 0
);

-- DEFINE-only variable used as a qualifier
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0, B AS B.val > 0
);

-- FROM-clause range variable qualified name: not allowed
-- (prohibited by ISO/IEC 19075-5 6.5)
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS rpr_err.val > 0
);

-- Unknown qualifier (neither pattern var nor range var): rejected like any
-- other qualified name, not reported as a missing FROM-clause entry, since
-- adding one would only lead to the range variable error above
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS nosuch.val > 0
);

-- A three-part name is schema-qualified even when its first part spells a
-- pattern variable, and is rejected as any other qualified name
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (public+)
    DEFINE public AS public.rpr_err.val > 0
);

-- Unqualified composite field access in DEFINE works: no qualifier means no
-- pattern/range-var navigation, so the pre-check skips and normal resolution
-- handles "(items).amount" via A_Indirection on the current row.
CREATE TYPE rpr_item AS (name TEXT, amount INT);
CREATE TEMP TABLE rpr_composite (id int, items rpr_item);
INSERT INTO rpr_composite VALUES (1, ROW('a',5)), (2, ROW('b',15)), (3, ROW('c',25));
SELECT id, (items).amount, COUNT(*) OVER w AS cnt
FROM rpr_composite
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE A AS (items).amount > 10
);

-- Composite type field selection (qualified forms):
-- the ColumnRef portion ("A.items" or
-- "rpr_composite.items") is what gets quoted; the trailing ".amount" lives in
-- the surrounding A_Indirection node and is not visible to the pre-check.
SELECT COUNT(*) OVER w
FROM rpr_composite
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS (A.items).amount > 10
);
SELECT COUNT(*) OVER w
FROM rpr_composite
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS (rpr_composite.items).amount > 10
);

-- A trailing star on a composite column is a different thing from a trailing
-- star on a relation: it names no relation, so the row constructor keeps
-- expanding it and the DEFINE restrictions do not apply.
SELECT COUNT(*) OVER w
FROM rpr_composite
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS ROW((items).*) IS NOT NULL
);

DROP TABLE rpr_composite;
DROP TYPE rpr_item;

-- A composite value that reaches DEFINE by way of a subquery Var only takes
-- its ROW(...) shape after pullup, and the ORDER BY copy's sortgroupref
-- keeps it from being flattened.  make_window_input_target() adds the fields
-- the split leaves behind.
CREATE TABLE rpr_ordrow (a int, b int);
INSERT INTO rpr_ordrow SELECT g, g % 4 FROM generate_series(1, 10) g;
SELECT count(*) OVER w AS c
FROM (SELECT ROW(a, b) AS x FROM rpr_ordrow) s
WINDOW w AS (ORDER BY x
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL PATTERN (P Q+) DEFINE P AS TRUE, Q AS x IS NOT NULL);
-- Control: without ORDER BY, x is flattened normally and this succeeds too.
SELECT count(*) OVER w AS c
FROM (SELECT ROW(a, b) AS x FROM rpr_ordrow) s
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL PATTERN (P Q+) DEFINE P AS TRUE, Q AS x IS NOT NULL);
DROP TABLE rpr_ordrow;

-- The same split by way of a pulled-up composite target, both as a plain
-- subquery and as a view.  Rows with equal a share a partition, so q is
-- tested and DEFINE really reads k.
CREATE TABLE rpr_partrow (a int, b int);
INSERT INTO rpr_partrow VALUES (1, 1), (1, 2), (1, 3), (2, 4);
SELECT count(*) OVER w
FROM (SELECT b, row(a, 1) AS k FROM rpr_partrow) s
WINDOW w AS (PARTITION BY k ORDER BY b
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  PATTERN (p q+) DEFINE q AS k IS NOT NULL);
CREATE TYPE rpr_partrow_t AS (x int, y int);
CREATE VIEW rpr_partrow_v AS SELECT b, row(a, 1)::rpr_partrow_t AS k FROM rpr_partrow;
SELECT count(*) OVER w FROM rpr_partrow_v
WINDOW w AS (PARTITION BY k ORDER BY b
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  PATTERN (p q+) DEFINE q AS k IS NOT NULL);
-- Control: PATTERN/DEFINE aside, the same window clause runs fine.
SELECT count(*) OVER w
FROM (SELECT b, row(a, 1) AS k FROM rpr_partrow) s
WINDOW w AS (PARTITION BY k ORDER BY b
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING);
DROP VIEW rpr_partrow_v;
DROP TYPE rpr_partrow_t;
DROP TABLE rpr_partrow;

-- ERROR: undefined column in DEFINE
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS nonexistent_column > 0
);

-- ERROR: type mismatch
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 'string'
);

-- ERROR: aggregate function in DEFINE is not supported
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS COUNT(*) > 0
);

-- ERROR: grouping operation in DEFINE is not supported.  This shares the
-- EXPR_KIND_RPR_DEFINE arm with the aggregate case above, but takes the
-- GroupingFunc half of it.  parseCheckAggregates() relies on this rejection
-- to leave DEFINE out of finalize_grouping_exprs().
SELECT COUNT(*) OVER w
FROM rpr_err
GROUP BY id
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS GROUPING(id) = 0
);

-- ERROR: set-returning function in DEFINE is not supported
SELECT FROM rpr_err
WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A+) DEFINE A AS 1 > generate_series(1 ,2));

-- ERROR: window function in DEFINE is not supported
SELECT FROM rpr_err
WINDOW w AS ( ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING PATTERN (A+) DEFINE A AS 1 > row_number() OVER ());

-- Subquery in DEFINE is not supported
SELECT COUNT(*) OVER w
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > (SELECT max(val) FROM rpr_err)
);

-- DEFINE variables that do not appear in PATTERN
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_err
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0, B AS val > 5, C AS val > 10
);

DROP TABLE rpr_err;

-- NULL handling

CREATE TABLE rpr_null (id INT, val INT);
INSERT INTO rpr_null VALUES (1, 10), (2, NULL), (3, 30);

-- NULL in DEFINE expression
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_null
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 15
);

-- IS NULL in DEFINE
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_null
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (N+)
    DEFINE N AS val IS NULL
);

-- IS NOT NULL in DEFINE
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_null
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (NN+)
    DEFINE NN AS val IS NOT NULL
);

DROP TABLE rpr_null;

-- Compound navigation: inner nav must be direct arg (not nested in expression)
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(v + FIRST(v)) > 0
);

-- FIRST/LAST wrapping FIRST/LAST: prohibited
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS FIRST(FIRST(v)) > 0
);

-- Triple nesting: prohibited (3-level deep navigation)
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(PREV(v))) > 0
);

-- Sibling navigations: prohibited, but they are not a deeper nesting,
-- so the inner navigation must be reported as not being the direct
-- argument rather than as a third level.
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(v) + LAST(v)) > 0
);

-- Three navigations, but the inner one is again not the whole argument, so
-- that is what gets reported and the depth is not reached
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(PREV(v)) + 1) > 0
);

-- A navigation offset must be a run-time constant, not a navigation operation
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(v, FIRST(1)) > 0);
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(v, FIRST(1) + 1) > 0);
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(v, NEXT(1, 0)) > 0);
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(FIRST(v), LAST(1)) > 0);
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(v, FIRST(v)) > 0);
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS NEXT(v, PREV(v, 1)) > 0);
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(FIRST(v, LAST(1)), 2) > 0);
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(v, FIRST(1::bigint)) > 0);

-- An unknown literal argument resolves to text;
-- it must still reference a column
SELECT count(*) OVER w
FROM generate_series(1,5) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV('foo') = 'bar');
SELECT count(*) OVER w
FROM generate_series(1,5) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV('foo'));
SELECT count(*) OVER w
FROM generate_series(1,5) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(NULL) IS NULL);
PREPARE rpr_navarg AS SELECT count(*) OVER w
FROM generate_series(1,5) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV($1) IS NULL);

-- An int2 offset is coerced to int8 like any implicit cast (same as plain 0)
SELECT count(*) OVER w
FROM generate_series(1,5) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(v, 0::smallint) = v);
SELECT count(*) OVER w
FROM generate_series(1,5) s(v)
WINDOW w AS (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+) DEFINE A AS PREV(v, 0) = v);

-- ============================================================
-- Pattern Optimization Tests
-- ============================================================
-- Tests for pattern optimization
-- Use EXPLAIN to verify optimized pattern (shown as "Pattern: ...")

CREATE TABLE rpr_plan (id INT, val INT);
INSERT INTO rpr_plan VALUES
    (1, 10), (2, 20), (3, 30), (4, 40), (5, 50),
    (6, 60), (7, 70), (8, 80), (9, 90), (10, 100);

-- Consecutive VAR merge: A A A -> a{3}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A A A) DEFINE A AS val > 0);

-- Consecutive VAR merge: A{2} A{3} -> a{5}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A{2} A{3}) DEFINE A AS val > 0);

-- Consecutive VAR merge: A+ A* -> a+
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+ A*) DEFINE A AS val > 0);

-- Consecutive VAR merge: A A+ -> a{2,}
-- where a finite prev (A{1,1}) meets an infinite child (A+).
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A A+) DEFINE A AS val > 0);

-- Consecutive VAR merge at the boundary: A{1073741823,} A{1073741823,} ->
-- a{2147483646,}.  The min sum 2147483646 = INT32_MAX - 1 is the largest
-- still-finite bound, so the merge proceeds; a sum of exactly INF instead
-- falls back (see the Optimization Fallback Tests).
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A{1073741823,} A{1073741823,}) DEFINE A AS val > 0);

-- Consecutive GROUP merge with finite quantifiers:
-- ((A B){5}) ((A B){10}) -> merged
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (((A B){5}) ((A B){10})) DEFINE A AS val <= 50, B AS val > 50);

-- Consecutive GROUP merge with unbounded: (A B)+ (A B)+ -> (a b){2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B)+ (A B)+) DEFINE A AS val <= 50, B AS val > 50);

-- Consecutive GROUP merge: (A B){2} (A B)+ -> (a b){3,}
-- Where a finite prev ((A B){2,2}) meets an infinite child ((A B)+).
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B){2} (A B)+) DEFINE A AS val <= 50, B AS val > 50);

-- Consecutive GROUP merge at the boundary:
-- (A B){1073741823,} (A B){1073741823,}
-- -> (a b){2147483646,}.  The min sum INT32_MAX - 1 is still finite, so the
-- merge proceeds; a sum of exactly INF instead falls back (see the
-- Optimization Fallback Tests).
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B){1073741823,} (A B){1073741823,}) DEFINE A AS val <= 50, B AS val > 50);

-- PREFIX merge: A B (A B)+ -> (a b){2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B (A B)+) DEFINE A AS val <= 50, B AS val > 50);

-- PREFIX and SUFFIX merge: A B (A B)+ A B -> (a b){3,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B (A B)+ A B) DEFINE A AS val <= 40, B AS val > 40);

-- Flatten nested: A ((B) (C)) -> a b c
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A ((B) (C))) DEFINE A AS val <= 30, B AS val <= 60, C AS val > 60);

-- Data execution: SEQ flatten produces correct results
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP TO NEXT ROW
             PATTERN (A ((B) (C))) DEFINE A AS val <= 30, B AS val <= 60, C AS val > 60);

-- ALT flatten: (A | (B | C))+ -> (a | b | c)+
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A | (B | C))+) DEFINE A AS val <= 30, B AS val <= 60, C AS val > 60);

-- ALT deduplicate: (A | B | A) -> (a | b)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A | B | A)+) DEFINE A AS val <= 50, B AS val > 50);

-- Data execution: ALT dedup produces correct results
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW
             PATTERN ((A | B | A)+) DEFINE A AS val <= 50, B AS val > 50);

-- Quantifier multiply: (A{2}){3} -> a{6}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2}){3}) DEFINE A AS val > 0);

-- Quantifier multiply: (((A B){2}?){3}) -> (a b){6}
-- {2}? has a fixed count, so reluctance is normalized away and the
-- multiplication that (((A B){2}){3}) gets applies here too
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (((A B){2}?){3}) DEFINE A AS val > 0, B AS val > 0);

-- Quantifier multiply control: greedy GROUP (((A B){2}){3}) -> (a b){6}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (((A B){2}){3}) DEFINE A AS val > 0, B AS val > 0);

-- Quantifier multiply with child range: (A{2,3}){3} -> a{6,9}
-- outer exact, child range - optimization applies
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2,3}){3}) DEFINE A AS val > 0);

-- Quantifier NO multiply: (A{2}){2,3} stays as (a{2}){2,3}
-- outer range - gaps would occur (4,6 not 4,5,6), no optimization
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2}){2,3}) DEFINE A AS val > 0);

-- Quantifier NO multiply: (A{2}){2,} stays as (a{2}){2,}
-- outer unbounded - gaps would occur
-- (4,6,8,... not 4,5,6,...), no optimization
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2}){2,}) DEFINE A AS val > 0);

-- Quantifier multiply: (A){2,} -> a{2,}
-- child exact 1 - no gaps, optimization applies
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A){2,}) DEFINE A AS val > 0);

-- Quantifier multiply: (A)+ -> a+
-- child exact 1 - no gaps, optimization applies
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A)+) DEFINE A AS val > 0);

-- Quantifier NO multiply: (A{2}){3,5} stays as (a{2}){3,5}
-- outer range, child exact > 1 - gaps would occur (6,8,10 not 6,7,8,9,10)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2}){3,5}) DEFINE A AS val > 0);

-- Quantifier multiply refused: (A{2,3}){2,3} stays nested.
-- The counts [4,6] U [6,9] = [4,9] are contiguous, but a bounded child with a
-- lower bound to fall short of makes the nested form prefer a shorter match
-- than a{4,9} would.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2,3}){2,3}) DEFINE A AS val > 0);

-- Quantifier NO multiply: (A{4,5}){2,3} stays as (a{4,5}){2,3}
-- outer range, child range with a gap: [8,10] U [12,15] misses 11
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{4,5}){2,3}) DEFINE A AS val > 0);

-- Nested unbounded: (A*)* -> a*
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A*)*) DEFINE A AS val > 0);

-- Nested unbounded: (A+)* -> a*
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A+)*) DEFINE A AS val > 0);

-- Nested unbounded: (A+)+ -> a+
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A+)+) DEFINE A AS val > 0);

-- Quantifier multiply with an unbounded child: an exact outer count (m == n)
-- always folds regardless of the child's max - (A+){3} -> a{3,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A+){3}) DEFINE A AS val > 0);

-- (A{2,}){3} -> a{6,}  (m == n, unbounded child with min 2)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2,}){3}) DEFINE A AS val > 0);

-- (A+){2,4} -> a{2,}  (outer range, unbounded child: every interval
-- reaches INF, so they always touch)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A+){2,4}) DEFINE A AS val > 0);

-- (A{2,3}){2,4} stays nested like (A{2,3}){2,3} above, even though the counts
-- [4,6] U [6,9] U [8,12] = [4,12] are contiguous.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2,3}){2,4}) DEFINE A AS val > 0);

-- Skippable outer (min 0) folds only when the zero case connects to the child
-- range: (A{1,3})? -> a{0,3}
-- (child min <= 1, so {0} U [1,3] = [0,3] is contiguous)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{1,3})?) DEFINE A AS val > 0);

-- Quantifier NO multiply: (A{2,3})? stays as (a{2,3})?
-- min 0 with child min >= 2: {0} U [2,3] leaves 1 unreachable
-- (intervals touch but the zero case does not connect)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2,3})?) DEFINE A AS val > 0);

-- Quantifier NO multiply: (A{3,4})? stays as (a{3,4})?
-- min 0 with child min >= 2: {0} U [3,4] leaves 1,2 unreachable
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{3,4})?) DEFINE A AS val > 0);

-- Unwrap GROUP{1,1}: (A) -> a
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A)) DEFINE A AS val > 0);

-- Unwrap GROUP{1,1}: (A B) -> a b
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B)) DEFINE A AS val <= 50, B AS val > 50);

-- Combined optimization: A A (B B)+ B B C C C -> a{2} (b{2}){2,} c{3}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A A (B B)+ B B C C C)
             DEFINE A AS val <= 20, B AS val > 20 AND val <= 70, C AS val > 70);

-- Unwrapped GROUPs then VAR merge: (A+) (A+) -> a{2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A+) (A+)) DEFINE A AS val > 0);

-- Quantifier multiply finite: (A{10}){20} -> a{200}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{10}){20}) DEFINE A AS val > 0);

-- Different GROUP prevents merge: (A B){2} (C D){3}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B){2} (C D){3})
             DEFINE A AS val <= 25, B AS val > 25 AND val <= 50,
                    C AS val > 50 AND val <= 75, D AS val > 75);

-- Different children count prevents merge: (A B)+ (A B C)+
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B)+ (A B C)+)
             DEFINE A AS val <= 33, B AS val > 33 AND val <= 66, C AS val > 66);

-- PREFIX only merge: A B (A B)+ -> (a b){2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B (A B)+) DEFINE A AS val <= 50, B AS val > 50);

-- SUFFIX only merge: (A B)+ A B -> (a b){2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B)+ A B) DEFINE A AS val <= 50, B AS val > 50);

-- Multiple SUFFIX absorption: (A B)+ A B A B C
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B)+ A B A B C)
             DEFINE A AS val <= 50, B AS val > 50 AND val <= 75, C AS val > 75);

-- PREFIX merge with remaining prefix: A B C D (C D)+  -> A B (C D) {2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B C D (C D)+)
             DEFINE A AS val <= 25, B AS val > 25 AND val <= 50,
                    C AS val > 50 AND val <= 75, D AS val > 75);

-- cannot merge, prefix is different
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w
FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
PATTERN (A B C D C (C D) +)
DEFINE A AS val <= 25, B AS val > 25,
       C AS val > 50, D AS val > 75);

-- PREFIX merge with quantifiers: A B* (A B*)+ -> (a b*){2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B* (A B*)+)
             DEFINE A AS val <= 50, B AS val > 50);

-- PREFIX merge with multiple quantifiers:
-- A+ B* C? (A+ B* C?)+ -> (a+ b* c?){2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+ B* C? (A+ B* C?)+)
             DEFINE A AS val <= 30, B AS val > 30 AND val <= 60, C AS val > 60);

-- SUFFIX merge refused: (A B*)+ A B* stays as written.  The body A B* has no
-- fixed row count, so folding the trailing copy into the group would move the
-- group's stop decision ahead of that copy's own choices.  The PREFIX merge
-- just above keeps working on the same kind of body -- a leading copy is
-- mandatory, so it merges without reordering anything.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B*)+ A B*)
             DEFINE A AS val <= 50, B AS val > 50);

-- Unwrap GROUP{1,1}: ((A | B | C)) -> (a | b | c)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A | B | C)) DEFINE A AS val <= 30, B AS val <= 60, C AS val > 60);

-- Data execution: GROUP unwrap produces correct results
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP TO NEXT ROW
             PATTERN ((A | B | C)) DEFINE A AS val <= 30, B AS val <= 60, C AS val > 60);

-- Reluctant optimization bypass: VAR merge
-- A+? A stays as a+? a (greedy A+ A merges to a{2,})
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A+? A) DEFINE A AS val > 0);

-- Reluctant optimization bypass: SUFFIX merge after GROUP{1,1} unwrap
-- (A B)+? (A B) stays as (a b)+? a b (greedy merges to (a b){2,})
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B)+? (A B)) DEFINE A AS val <= 50, B AS val > 50);

-- Reluctant optimization bypass: quantifier multiply (outer reluctant)
-- (A+){2,4}? stays nested where the greedy (A+){2,4} multiplies to a{2,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A+){2,4}?) DEFINE A AS val > 0);

-- Reluctant optimization bypass: quantifier multiply (inner reluctant)
-- (A{2,3}?){3} stays as (a{2,3}?){3} (greedy (A{2,3}){3} merges to a{6,9})
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2,3}?){3}) DEFINE A AS val > 0);

-- Fixed count normalizes away: (A{2}?){3} -> a{6}, same as (A{2}){3}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A{2}?){3}) DEFINE A AS val > 0);

-- {0,} is the only brace spelling that reaches an unbounded min 0
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A{0,}) DEFINE A AS val > 0);

-- Bare {1} and {1,1} carry no quantifier, on a VAR or on a GROUP
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A{1} B{1,1} (C){1})
             DEFINE A AS val <= 30, B AS val <= 60, C AS val > 60);

-- A reluctant {1,1} normalizes to a bare variable
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A{1,1}? (B C){1})
             DEFINE A AS val <= 30, B AS val <= 60, C AS val > 60);

-- Reluctant optimization bypass: PREFIX merge
-- A B (A B)+? stays separate (greedy merges to (a b){2,})
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B (A B)+?) DEFINE A AS val <= 50, B AS val > 50);

-- Reluctant optimization bypass: SUFFIX merge
-- (A B)+? A B stays separate (greedy merges to (a b){2,})
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A B)+? A B) DEFINE A AS val <= 50, B AS val > 50);

-- GROUP unwrap with quantifier propagation: (A)?? B -> a?? b
-- Single VAR child {1,1} receives GROUP's quantifier and reluctant
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A)?? B) DEFINE A AS val <= 50, B AS val > 50);

-- Reluctant preserved through ALT flatten
-- (A | (B | C))+? flattens to (a | b | c)+? - inner
-- ALT flattened, reluctant kept
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A | (B | C))+?) DEFINE A AS val <= 30, B AS val <= 60, C AS val > 60);

-- Reluctant optimization bypass: absorption flags
-- A+? with SKIP PAST LAST ROW - no absorption markers (greedy A+ gets a+#)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (A+?) DEFINE A AS val > 0);

-- Duplicate GROUP removal: ((A | B)+ | (A | B)+) -> (a | b)+
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A | B)+ | (A | B)+) DEFINE A AS val <= 50, B AS val > 50);

-- Consecutive VAR merge with zero-min: A* A+ -> a+
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A* A+) DEFINE A AS val > 0);

-- Consecutive VAR merge (4-element): A A{2} A+ A{3} -> a{7,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A A{2} A+ A{3}) DEFINE A AS val > 0);

-- PREFIX+SUFFIX merge (5-way): A B A B (A B)+ A B A B -> (a b){5,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B A B (A B)+ A B A B)
             DEFINE A AS val <= 50, B AS val > 50);

-- PREFIX+SUFFIX merge (5-way): B A B (A B)+ A B A B -> b (a b){4,}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (B A B (A B)+ A B A B)
             DEFINE A AS val <= 50, B AS val > 50);

-- Unwrap single-item ALT after dedup: (A | A)+ -> a+
-- ALT dedup reduces to single-item, then quantifier multiply folds the GROUP
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN ((A | A)+) DEFINE A AS val > 0);

-- GROUP{1,1} to SEQ with flatten: ((A B)(C D)) -> a b c d
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (((A B)(C D)))
             DEFINE A AS val <= 25, B AS val > 25 AND val <= 50,
                    C AS val > 50 AND val <= 75, D AS val > 75);

-- Nested ALT pattern: ((A B) | C) D | A B C
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (((A B) | C) D | A B C)
             DEFINE A AS val <= 25, B AS val > 25 AND val <= 50,
                    C AS val > 50 AND val <= 75, D AS val > 75);

-- Nested ALT with unbounded: ((A+ B) | C) D | A B C
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (((A+ B) | C) D | A B C)
             DEFINE A AS val <= 25, B AS val > 25 AND val <= 50,
                    C AS val > 50 AND val <= 75, D AS val > 75);

-- ============================================================
-- Absorption Flag Display Tests
-- ============================================================
-- Tests absorption marker display in EXPLAIN output
-- Markers: ~ = branch element, # = comparison point

-- Simple VAR: A+ -> a+# (comparison point)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (A+) DEFINE A AS val > 0);

-- GROUP unbounded: (A B)+ -> (a~ b~)+# (branch + comparison)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ((A B)+) DEFINE A AS val <= 50, B AS val > 50);

-- ALT both absorbable: A+ | B+ -> (a+# | b+#)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (A+ | B+) DEFINE A AS val <= 50, B AS val > 50);

-- ALT one absorbable: A+ | B -> (a+# | b)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (A+ | B) DEFINE A AS val <= 50, B AS val > 50);

-- Sequence with absorbable start: A+ B -> a+# b
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (A+ B) DEFINE A AS val <= 50, B AS val > 50);

-- Complex nested: ((A+ B) | C) D | A B C - deeply nested ALT
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (((A+ B) | C) D | A B C)
             DEFINE A AS val <= 30, B AS val <= 60, C AS val <= 80, D AS val > 80);

-- ALT branch tail not over-marked: A | (B C)+ (D E)+ -> (a | (b~ c~)+# (d e)+)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (A | (B C)+ (D E)+)
             DEFINE A AS val <= 20, B AS val <= 40, C AS val <= 60, D AS val <= 80, E AS val > 80);

-- Nested unbounded: (A+ | B)+ -> (a+# | b)+ (first iteration absorbable)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ((A+ | B)+)
             DEFINE A AS val <= 50, B AS val > 50);

-- ALT inside unbounded GROUP: (A+ B | A B)* -> (a+# b | a b)*
-- (first iteration absorbable)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ((A+ B | A B)*)
             DEFINE A AS val <= 50, B AS val > 50);

-- Fixed-length group absorbable: (A{2} B{3})+ -> (a{2}~ b{3}~)+#
-- All children have min == max, equivalent to unrolling to {1,1}
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ((A{2} B{3})+)
             DEFINE A AS val <= 50, B AS val > 50);

-- Nested fixed-length group: (A (B C){2} D)+ -> absorbable
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ((A (B C){2} D)+)
             DEFINE A AS val <= 20, B AS val <= 40, C AS val <= 60, D AS val > 60);

-- Nested fixed-length with inner quantifier: ((A{2} B{3}){2})+ -> absorbable
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (((A{2} B{3}){2})+)
             DEFINE A AS val <= 50, B AS val > 50);

-- Non-absorbable fixed-length: (A B{2,5})+ -> no markers (min != max)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ((A B{2,5})+)
             DEFINE A AS val <= 50, B AS val > 50);

-- Non-absorbable fixed-length: (A B?)+ -> no markers (min != max)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ((A B?)+)
             DEFINE A AS val <= 50, B AS val > 50);

-- Non-absorbable (unbounded not at start): A B+ -> a b+ (no markers)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (A B+) DEFINE A AS val <= 50, B AS val > 50);

-- Non-absorbable (no unbounded branch):
-- (A | B){2,} -> (a | b){2,} (no markers)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ((A | B){2,}) DEFINE A AS val <= 50, B AS val > 50);

-- Non-absorbable (SKIP TO NEXT ROW): A+ -> a+ (no markers)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP TO NEXT ROW PATTERN (A+) DEFINE A AS val > 0);

-- Non-absorbable (limited frame): A+ -> a+ (no markers)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND 10 FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (A+) DEFINE A AS val > 0);

-- A marker on a quoted variable name lands outside the closing quote: a
-- keyword name takes the comparison point, "select"+ -> "select"+#
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN ("select"+ B)
             DEFINE "select" AS val <= 50, B AS val > 50);

-- The same for the branch marker, on a name quoted for its space:
-- ("My Var" A)+ -> ("My Var"~ a~)+#
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_plan
WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             AFTER MATCH SKIP PAST LAST ROW PATTERN (("My Var" A)+ B)
             DEFINE "My Var" AS val <= 25, A AS val <= 50, B AS val > 50);

-- Reluctant {1}? quantifier: min == max, so the plan normalizes it away
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM rpr_plan
WINDOW w AS (
    ORDER BY val
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1}? B)
    DEFINE A AS val > 0, B AS val > 0
);

-- ============================================================
-- Absorption Analysis Tests
-- ============================================================
-- Tests context absorption optimization (O(n^2) -> O(n))

-- Simple Absorbable Pattern: A+ B
-- Pattern starts with unbounded VAR

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS val <= 50, B AS val > 50
);

-- Absorbable GROUP Pattern: (A B)+ C
-- Pattern starts with unbounded GROUP

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+ C)
    DEFINE A AS val <= 30, B AS val > 30 AND val <= 60, C AS val > 60
);

-- Non-Absorbable: Unbounded Not at Start
-- Pattern: A B+ (unbounded not at start)

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS val <= 50, B AS val > 50
);

-- ALT with Absorbable Branches
-- Pattern: (A+ | B+) C - both branches absorbable

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ | B+) C)
    DEFINE A AS val <= 30, B AS val > 30 AND val <= 60, C AS val > 60
);

-- ALT with Mixed Branches
-- Pattern: (A+ | B C)+ - only first branch absorbable

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ | B C)+)
    DEFINE A AS val <= 30, B AS val > 30 AND val <= 60, C AS val > 60
);

-- Non-Absorbable: ALT Inside GROUP
-- Pattern: (A | B){2,} - ALT inside unbounded GROUP

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B){2,})
    DEFINE A AS val <= 50, B AS val > 50
);

-- Nested Unbounded: only the inner GROUP is absorbable
-- Pattern: ((A B)+ C)+ - inner (A B)+ absorbable on the first iteration

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A B)+ C)+)
    DEFINE A AS val <= 30, B AS val > 30 AND val <= 60, C AS val > 60
);

-- Non-Absorbable: Unbounded Element Inside GROUP
-- Pattern: (A B+){2,} - unbounded inside GROUP

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B+){2,})
    DEFINE A AS val <= 50, B AS val > 50
);

-- Runtime Conditions: SKIP TO NEXT ROW
-- Absorption disabled with SKIP TO NEXT ROW

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE A AS val <= 50, B AS val > 50
);

-- Runtime Conditions: Limited Frame
-- Absorption disabled with limited frame end

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 5 FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS val <= 50, B AS val > 50
);

-- ALT Non-Absorbable Branch Match: A+ B | C
-- C match on the non-absorbable branch (id=2, id=5) must survive absorption of
-- the dominating A+ run, which never completes a match (B is never present)

WITH test_nonabsorbable_branch AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A', 'C']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['A', 'C']),
        (6, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nonabsorbable_branch
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B | C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Measuring a group body for a fixed row count.  The suffix merge measures
-- the body of a group followed by more of the sequence, so each pattern below
-- puts the shape under test inside such a group.  Each measurement must
-- report "not a fixed length": the first because its repetition count is a
-- range, the other two instead of overflowing.  Only the first pattern can
-- match real rows.

-- A nested group whose repetition count is a range has no fixed length
SELECT id, val, COUNT(*) OVER w AS cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A B){1,2} C)+ D)
    DEFINE A AS val <= 30, B AS val > 30, C AS val > 0, D AS val > 0
);

-- A fixed repetition whose body length times its count reaches the ceiling
SELECT id, val, COUNT(*) OVER w AS cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A B C){1000000000})+ D)
    DEFINE A AS val <= 30, B AS val > 30, C AS val > 0, D AS val > 0
);

-- A body whose members sum past the ceiling
SELECT id, val, COUNT(*) OVER w AS cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A{2000000000} B{2000000000})+ C)
    DEFINE A AS val <= 30, B AS val > 30, C AS val > 0
);

-- ALT Both Branches Absorbable: A+ C | B+
-- A+ C never completes (C absent) so its A+ run keeps expanding and dominates;
-- a finalized B+ match on the other branch
-- (id=1, id=6) must survive absorption

WITH test_absorbable_branches AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A', 'B']),
        (2, ARRAY['A', 'B']),
        (3, ARRAY['A', 'B']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['A', 'B']),
        (7, ARRAY['A', 'B']),
        (8, ARRAY['A', 'B']),
        (9, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_absorbable_branches
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ C | B+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- ============================================================
-- Edge Case Tests
-- ============================================================
-- Tests boundary conditions and complex scenarios

-- Empty Match Prevention
-- Pattern that could match empty: A*

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A*)
    DEFINE A AS val > 1000  -- Never matches
);

-- All Rows Match
-- Pattern where every row matches

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val >= 0  -- Always true
);

-- Large Quantifiers
-- Pattern: A{100} (large exact quantifier)

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{100})
    DEFINE A AS val > 0
);

-- Pattern: A{10,20} (large range quantifier)
SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{10,20})
    DEFINE A AS val > 0
);

-- Complex Multi-Level Nesting
-- Pattern: (((A B) | C)+ D)+

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((((A B) | C)+ D)+)
    DEFINE A AS val <= 20, B AS val > 20 AND val <= 40,
           C AS val > 40 AND val <= 60, D AS val > 60
);

-- Long Alternation Chain
-- Pattern: A | B | C | D | E (5-way ALT)

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A | B | C | D | E)
    DEFINE A AS val = 10, B AS val = 30, C AS val = 50,
           D AS val = 70, E AS val = 90
);

-- Long Sequence
-- Pattern: A B C D E F G H (8-element SEQ)

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B C D E F G H)
    DEFINE A AS val >= 10, B AS val >= 20, C AS val >= 30,
           D AS val >= 40, E AS val >= 50, F AS val >= 60,
           G AS val >= 70, H AS val >= 80
);

-- Interleaved Quantifiers
-- Pattern: A{2} B+ C{3,5} D* E{1,}

SELECT id, val, COUNT(*) OVER w as cnt
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{2} B+ C{3,5} D* E{1,})
    DEFINE A AS val > 0, B AS val > 0, C AS val > 0,
           D AS val > 0, E AS val > 0
);

-- ============================================================
-- Optimization Fallback Tests
-- ============================================================
-- Tests for optimization edge cases and fallback behavior

CREATE TABLE rpr_fallback (id INT, val INT);
INSERT INTO rpr_fallback VALUES (1, 10), (2, 20);

-- Min quantifier overflow causes optimization fallback (min == max case)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A{2000000000}){2})
    DEFINE A AS val > 0
);

-- Max-only quantifier overflow causes optimization fallback
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A{1,2000000000}){2})
    DEFINE A AS val > 0
);

-- Max quantifier exceeds valid range
-- (2147483647 = INT_MAX, limit is 2147483646)
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A{2000000000,2147483647}){2})
    DEFINE A AS val > 0
);

-- Nested unbounded with large min causes overflow fallback
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A{2000000000,}){2000000000,})
    DEFINE A AS val > 0
);

-- Prefix mismatch causes optimization fallback
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B (C D)+)
    DEFINE A AS val > 0, B AS val > 5, C AS val > 10, D AS val > 15
);

-- Consecutive VAR merge whose min sum is exactly INF causes fallback.
-- 1073741824 + 1073741823 = 2147483647 = INT32_MAX = RPR_QUANTITY_INF.
-- Merging would yield a VAR with min == INF, so the merge must fall back and
-- leave the two VARs unmerged (mirrors the multiply path's >= INF guard).
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1073741824,} A{1073741823,})
    DEFINE A AS val > 0
);

-- One more than that sum does not fit in int32.  The fallback looks the
-- same as the case above; this one reaches the overflow check instead of the
-- >= INF comparison.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1073741825,} A{1073741823,})
    DEFINE A AS val > 0
);

-- VAR merge falls back when the max sum lands exactly on INF.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1,1073741823} A{1,1073741824})
    DEFINE A AS val > 0
);
-- One below that sum is the largest max the merge may keep.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1,1073741822} A{1,1073741824})
    DEFINE A AS val > 0
);
-- One above that does not fit in int32; the overflow check rejects it.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1,1073741824} A{1,1073741824})
    DEFINE A AS val > 0
);
-- An operand that is already unbounded still merges.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A{1,1073741823} A{1,})
    DEFINE A AS val > 0
);
-- Consecutive GROUP merge whose min sum is exactly INF causes fallback.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B){1073741824,} (A B){1073741823,})
    DEFINE A AS val > 0, B AS val > 5
);

-- Consecutive GROUP merge whose max sum is exactly INF causes fallback,
-- where one less merges.  Without the guard the merged max would alias INF and
-- a bounded pattern would become unbounded.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B){1,1073741823} (A B){1,1073741824})
    DEFINE A AS val > 0, B AS val > 5
);
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B){1,1073741822} (A B){1,1073741824})
    DEFINE A AS val > 0, B AS val > 5
);

-- The prefix merge adds one iteration, so it declines a min already at
-- INF - 1 and a max already at INF - 1; one less than either merges.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B (A B){2147483646,})
    DEFINE A AS val > 0, B AS val > 5
);
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B (A B){2147483645,})
    DEFINE A AS val > 0, B AS val > 5
);
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B (A B){1,2147483646})
    DEFINE A AS val > 0, B AS val > 5
);
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B (A B){1,2147483645})
    DEFINE A AS val > 0, B AS val > 5
);

-- The suffix merge has the same boundary as the prefix merge.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B){2147483646,} A B)
    DEFINE A AS val > 0, B AS val > 5
);
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER w FROM rpr_fallback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B){2147483645,} A B)
    DEFINE A AS val > 0, B AS val > 5
);

DROP TABLE rpr_fallback;

-- ============================================================
-- Planner Integration Tests
-- ============================================================
-- Tests full planning pipeline and WindowAgg plan node creation

CREATE TABLE rpr_planner (id INT, category VARCHAR(10), val INT);
INSERT INTO rpr_planner VALUES
    (1, 'A', 10), (2, 'A', 20), (3, 'A', 30),
    (4, 'B', 40), (5, 'B', 50), (6, 'B', 60),
    (7, 'C', 70), (8, 'C', 80), (9, 'C', 90);

-- Multiple Window Functions in Same Query
SELECT id, category, val,
       COUNT(*) OVER w1 as cnt1,
       COUNT(*) OVER w2 as cnt2
FROM rpr_planner
WINDOW w1 AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
),
w2 AS (
    PARTITION BY category
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (B+)
    DEFINE B AS val >= 40
);

-- Window Function with PARTITION BY

SELECT id, category, val,
       COUNT(*) OVER w as cnt
FROM rpr_planner
WINDOW w AS (
    PARTITION BY category
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Window Function with Complex ORDER BY

SELECT id, category, val,
       COUNT(*) OVER w as cnt
FROM rpr_planner
WINDOW w AS (
    ORDER BY category DESC, val ASC
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Named Window Reference

SELECT id, category, val,
       COUNT(*) OVER w as cnt
FROM rpr_planner
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Inline Window Definition

SELECT id, category, val,
       COUNT(*) OVER (
           ORDER BY id
           ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
           PATTERN (A+)
           DEFINE A AS val > 0
       ) as cnt
FROM rpr_planner;

-- ============================================================
-- Subquery and CTE Tests
-- ============================================================
-- Tests RPR with subqueries and CTEs

-- RPR in Subquery (FROM clause)

SELECT * FROM (
    SELECT id, category, val,
           COUNT(*) OVER w as cnt
    FROM rpr_planner
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
) sub
WHERE cnt > 5;

-- RPR with Subquery in WHERE

SELECT id, category, val,
       COUNT(*) OVER w as cnt
FROM rpr_planner
WHERE val > (SELECT AVG(val) FROM rpr_planner)
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 50
);

-- CTE with RPR

WITH rpr_cte AS (
    SELECT id, category, val,
           COUNT(*) OVER w as cnt
    FROM rpr_planner
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
)
SELECT * FROM rpr_cte WHERE cnt > 5 ORDER BY id;

-- Multiple CTE References

WITH rpr_cte AS (
    SELECT id, category, val,
           COUNT(*) OVER w as cnt
    FROM rpr_planner
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
)
SELECT c1.id, c1.cnt, c2.cnt as cnt2
FROM rpr_cte c1
JOIN rpr_cte c2 ON c1.id = c2.id
ORDER BY c1.id;

-- Nested CTEs

WITH cte1 AS (
    SELECT id, category, val FROM rpr_planner WHERE val > 30
),
cte2 AS (
    SELECT id, category, val,
           COUNT(*) OVER w as cnt
    FROM cte1
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > 0
    )
)
SELECT * FROM cte2 ORDER BY id;

-- ============================================================
-- JOIN Tests
-- ============================================================
-- Tests RPR with JOINs and multiple table references

CREATE TABLE rpr_join1 (id INT, val1 INT);
CREATE TABLE rpr_join2 (id INT, val2 INT);

INSERT INTO rpr_join1 VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);
INSERT INTO rpr_join2 VALUES (1, 100), (2, 200), (3, 300), (4, 400), (5, 500);

-- RPR After INNER JOIN

SELECT t1.id, t1.val1, t2.val2,
       COUNT(*) OVER w as cnt
FROM rpr_join1 t1
INNER JOIN rpr_join2 t2 ON t1.id = t2.id
WINDOW w AS (
    ORDER BY t1.id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val1 + val2 > 100
);

-- RPR After LEFT JOIN

SELECT t1.id, t1.val1, t2.val2,
       COUNT(*) OVER w as cnt
FROM rpr_join1 t1
LEFT JOIN rpr_join2 t2 ON t1.id = t2.id
WINDOW w AS (
    ORDER BY t1.id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val1 > 0
);

-- RPR with Multiple Tables in DEFINE

SELECT t1.id, t1.val1, t2.val2,
       COUNT(*) OVER w as cnt
FROM rpr_join1 t1
INNER JOIN rpr_join2 t2 ON t1.id = t2.id
WINDOW w AS (
    ORDER BY t1.id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ B)
    DEFINE A AS val1 > 20,
           B AS val2 > 200
);

-- RPR After Cross Join

SELECT t1.id as id1, t2.id as id2, t1.val1, t2.val2,
       COUNT(*) OVER w as cnt
FROM rpr_join1 t1
CROSS JOIN rpr_join2 t2
WHERE t1.id <= 2 AND t2.id <= 2
WINDOW w AS (
    ORDER BY t1.id, t2.id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val1 + val2 > 0
);

-- Self-Join with RPR

SELECT id, val1, val1_next,
       COUNT(*) OVER w as cnt
FROM (SELECT a.id, a.val1, b.val1 as val1_next
      FROM rpr_join1 a
      INNER JOIN rpr_join1 b ON a.id + 1 = b.id) sub
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (X+)
    DEFINE X AS val1 < val1_next
);

DROP TABLE rpr_join1, rpr_join2;

-- A mismatched-type USING merge column used to let the other side's
-- pulled-up constant fold into a navigation argument at plan time.
CREATE TABLE rpr_join4 (k bigint);
INSERT INTO rpr_join4 VALUES (10);

SELECT count(*) OVER w AS cnt
FROM (SELECT 10 AS k) a LEFT JOIN rpr_join4 USING (k)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS PREV(k / 0) > 0
);

-- Same, but RIGHT JOIN with the constant subquery on its preserved
-- (right) side.
SELECT count(*) OVER w AS cnt
FROM rpr_join4 RIGHT JOIN (SELECT 10 AS k) a USING (k)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS PREV(k / 0) > 0
);

DROP TABLE rpr_join4;

-- Same shape with real per-row data, confirming navigation still sees
-- each row's own value.
CREATE TABLE rpr_join5 (k int, v int);
INSERT INTO rpr_join5 VALUES (1, 10), (2, 0), (3, 5);
CREATE TABLE rpr_join6 (k bigint, w int);
INSERT INTO rpr_join6 VALUES (1, 1), (2, 1), (3, 1);

SELECT k, v, cnt
FROM (SELECT k, v, count(*) OVER win AS cnt
      FROM rpr_join5 LEFT JOIN rpr_join6 USING (k)
      WINDOW win AS (
          ORDER BY k
          ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
          PATTERN (A+)
          DEFINE A AS PREV(v) IS NOT NULL
      )) s
ORDER BY k;

DROP TABLE rpr_join5, rpr_join6;

-- A DEFINE clause reading a USING column whose two sides differ in typmod.
-- The merged column stays a join alias Var, pullup leaves its joinaliasvars
-- entry a non-trivial expression, and the outer join's nullingrels wrap that
-- in a PlaceHolderVar.  The target list copy and the DEFINE copy are wrapped
-- by separate calls, so their phids differ and equal() does not match them --
-- the window input has to carry the DEFINE clause's own PlaceHolderVar.
CREATE TABLE rpr_phv_src (n int);
CREATE TABLE rpr_phv_dim (c varchar(10), tdate date);
CREATE TABLE rpr_phv_out (k varchar);
INSERT INTO rpr_phv_src VALUES (2), (4);
INSERT INTO rpr_phv_dim VALUES ('zz', '2024-01-01'), ('zzzz', '2024-01-02');
INSERT INTO rpr_phv_out VALUES ('zz'), ('zzzz');

SELECT j.c, j.tdate, count(*) OVER w AS cnt
FROM rpr_phv_out o1
     LEFT JOIN ( (SELECT n, repeat('z', n)::varchar(5) AS c FROM rpr_phv_src) s
                 JOIN rpr_phv_dim USING (c) ) j
     ON o1.k = j.c
WINDOW w AS (ORDER BY j.tdate
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL PATTERN (p q+)
             DEFINE p AS TRUE, q AS c > '');

-- The same with one more join level above it.  Reaching the window input is
-- not enough on its own: an intermediate join emits only what something above
-- has declared a need for, so what a DEFINE clause reads is marked needed at
-- relation 0 the way the target list's own columns are.
SELECT j.c, j.tdate, count(*) OVER w AS cnt
FROM rpr_phv_out o1
     LEFT JOIN rpr_phv_out o2 ON o1.k = o2.k
     LEFT JOIN ( (SELECT n, repeat('z', n)::varchar(5) AS c FROM rpr_phv_src) s
                 JOIN rpr_phv_dim USING (c) ) j
     ON o2.k = j.c
WINDOW w AS (ORDER BY j.tdate
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL PATTERN (p q+)
             DEFINE p AS TRUE, q AS c > '');

-- Control: with both sides of USING at the same typmod the merged column is a
-- plain Var of one side, no PlaceHolderVar is built, and neither shape above
-- needs any of this.
SELECT j.c, j.tdate, count(*) OVER w AS cnt
FROM rpr_phv_out o1
     LEFT JOIN rpr_phv_out o2 ON o1.k = o2.k
     LEFT JOIN ( (SELECT n, repeat('z', n)::varchar(10) AS c
                  FROM rpr_phv_src) s
                 JOIN rpr_phv_dim USING (c) ) j
     ON o2.k = j.c
WINDOW w AS (ORDER BY j.tdate
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             INITIAL PATTERN (p q+)
             DEFINE p AS TRUE, q AS c > '');

DROP TABLE rpr_phv_src, rpr_phv_dim, rpr_phv_out;

-- A WINDOW clause no window function names is never executed, so its DEFINE
-- clause is emptied before build_base_rel_tlists() could mark what it reads
-- as needed at relation 0, which would keep the outer join from being
-- removed.  The three plans below are the assertion: no WINDOW clause, a
-- plain one and a row pattern one all lose the join alike.
CREATE TABLE rpr_jr (id int, v int);
CREATE TABLE rpr_jr_u (id int PRIMARY KEY, uval int);
INSERT INTO rpr_jr SELECT g, g * 10 FROM generate_series(1, 5) g;
INSERT INTO rpr_jr_u SELECT g, g * 100 FROM generate_series(1, 5) g;

EXPLAIN (COSTS OFF)
SELECT t.id FROM rpr_jr t LEFT JOIN rpr_jr_u u ON t.id = u.id;

EXPLAIN (COSTS OFF)
SELECT t.id FROM rpr_jr t LEFT JOIN rpr_jr_u u ON t.id = u.id
WINDOW w AS (ORDER BY t.id);

EXPLAIN (COSTS OFF)
SELECT t.id FROM rpr_jr t LEFT JOIN rpr_jr_u u ON t.id = u.id
WINDOW w AS (ORDER BY t.id
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (A B+) DEFINE B AS uval > PREV(uval));

DROP TABLE rpr_jr, rpr_jr_u;

-- ============================================================
-- Complex Expression Tests
-- ============================================================
-- Tests complex target list expressions

CREATE TABLE rpr_target (id INT, val INT);
INSERT INTO rpr_target VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);

-- Expressions in Target List

SELECT id,
       val * 2 as doubled,
       val + 10 as added,
       COUNT(*) OVER w as cnt
FROM rpr_target
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- CASE Expression in Target List

SELECT id, val,
       CASE
           WHEN val < 30 THEN 'low'
           WHEN val < 50 THEN 'medium'
           ELSE 'high'
       END as category,
       COUNT(*) OVER w as cnt
FROM rpr_target
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Subquery in Target List

SELECT id, val,
       (SELECT MAX(val) FROM rpr_target) as max_val,
       COUNT(*) OVER w as cnt
FROM rpr_target
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Function Calls in Target List

SELECT id, val,
       COALESCE(val, 0) as coalesced,
       ABS(val - 30) as distance,
       COUNT(*) OVER w as cnt
FROM rpr_target
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Column Aliases and References

SELECT id as row_id,
       val as value,
       COUNT(*) OVER w as cnt
FROM rpr_target
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

DROP TABLE rpr_target;

-- ============================================================
-- Set Operations Tests
-- ============================================================
-- Tests RPR with UNION, INTERSECT, EXCEPT

CREATE TABLE rpr_set1 (id INT, val INT);
CREATE TABLE rpr_set2 (id INT, val INT);

INSERT INTO rpr_set1 VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO rpr_set2 VALUES (2, 20), (3, 30), (4, 40);

-- UNION with RPR

(SELECT id, val, COUNT(*) OVER w as cnt
 FROM rpr_set1
 WINDOW w AS (
     ORDER BY id
     ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
     PATTERN (A+)
     DEFINE A AS val > 0
 ))
UNION
(SELECT id, val, COUNT(*) OVER w as cnt
 FROM rpr_set2
 WINDOW w AS (
     ORDER BY id
     ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
     PATTERN (A+)
     DEFINE A AS val > 0
 ))
ORDER BY id;

-- UNION ALL with RPR

(SELECT id, val, COUNT(*) OVER w as cnt
 FROM rpr_set1
 WINDOW w AS (
     ORDER BY id
     ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
     PATTERN (A+)
     DEFINE A AS val > 0
 ))
UNION ALL
(SELECT id, val, COUNT(*) OVER w as cnt
 FROM rpr_set2
 WINDOW w AS (
     ORDER BY id
     ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
     PATTERN (A+)
     DEFINE A AS val > 0
 ))
ORDER BY id, val;

-- INTERSECT with RPR

(SELECT id, val, COUNT(*) OVER w as cnt
 FROM rpr_set1
 WINDOW w AS (
     ORDER BY id
     ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
     PATTERN (A+)
     DEFINE A AS val > 0
 ))
INTERSECT
(SELECT id, val, COUNT(*) OVER w as cnt
 FROM rpr_set2
 WINDOW w AS (
     ORDER BY id
     ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
     PATTERN (A+)
     DEFINE A AS val > 0
 ))
ORDER BY id;

-- EXCEPT with RPR

(SELECT id, val, COUNT(*) OVER w as cnt
 FROM rpr_set1
 WINDOW w AS (
     ORDER BY id
     ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
     PATTERN (A+)
     DEFINE A AS val > 0
 ))
EXCEPT
(SELECT id, val, COUNT(*) OVER w as cnt
 FROM rpr_set2
 WINDOW w AS (
     ORDER BY id
     ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
     PATTERN (A+)
     DEFINE A AS val > 0
 ))
ORDER BY id;

DROP TABLE rpr_set1, rpr_set2;

-- ============================================================
-- Sorting and Grouping Tests
-- ============================================================
-- Tests RPR interaction with sorting and grouping

CREATE TABLE rpr_sort (id INT, category VARCHAR(10), val INT);
INSERT INTO rpr_sort VALUES
    (1, 'A', 30), (2, 'B', 20), (3, 'A', 10),
    (4, 'B', 40), (5, 'A', 50), (6, 'B', 60);

-- RPR with GROUP BY (aggregate in DEFINE -> ERROR before GROUP BY interaction)

SELECT category,
       COUNT(*) as group_cnt,
       MAX(val) as max_val,
       COUNT(*) OVER w as window_cnt
FROM rpr_sort
GROUP BY category
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS COUNT(*) > 0
);

-- RPR with HAVING (same aggregate-in-DEFINE error)

SELECT category,
       COUNT(*) as group_cnt,
       COUNT(*) OVER w as window_cnt
FROM rpr_sort
GROUP BY category
HAVING COUNT(*) > 2
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS COUNT(*) > 0
);

-- RPR with DISTINCT

SELECT DISTINCT category,
       COUNT(*) OVER w as cnt
FROM rpr_sort
WINDOW w AS (
    PARTITION BY category
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
)
ORDER BY category;

-- RPR with ORDER BY (different from window ORDER BY)

SELECT id, category, val,
       COUNT(*) OVER w as cnt
FROM rpr_sort
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
)
ORDER BY val DESC;

-- RPR with LIMIT and OFFSET

SELECT id, category, val,
       COUNT(*) OVER w as cnt
FROM rpr_sort
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
)
ORDER BY id
LIMIT 3 OFFSET 1;

-- ------------------------------------------------------------
-- RPR over grouped input
-- ------------------------------------------------------------
-- A DEFINE clause is the only part of a WindowClause that holds an
-- expression tree of its own, so parseCheckAggregates() has to substitute
-- its grouped columns separately from the target list's.  These pin the
-- grouping shapes that reach that substitution, and what each returns once
-- a grouping set nulls a column the pattern reads.

CREATE TABLE rpr_grp (id int PRIMARY KEY, category text, val int);
INSERT INTO rpr_grp VALUES (1, 'A', 10), (2, 'B', 20);

-- Grouped input works; the pattern matches over the grouped rows
SELECT category, sum(val) AS total, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY category
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS category IS NOT NULL)
ORDER BY category;

-- Navigation over a grouping column
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY category
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B*)
    DEFINE B AS category > PREV(category))
ORDER BY category;

-- GROUP BY () builds no RTE_GROUP, so there is no grouped column for a
-- DEFINE clause to name and nothing that could diverge
SELECT count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY ()
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS true);

-- A single grouping set collapses to a plain GROUP BY and cannot null the
-- column
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY GROUPING SETS ((category))
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL)
ORDER BY category;

-- Duplicated sets leave the column in every set, so it is never nulled
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY GROUPING SETS ((category), (category))
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL)
ORDER BY category;

-- ROLLUP with a DEFINE clause that holds no column reference at all
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS true)
ORDER BY category NULLS LAST;

-- The DEFINE clause names only a column that every grouping set contains,
-- so gset_common covers it and no varnullingrels are attached
SELECT category, val, count(*) OVER w AS cnt
FROM rpr_sort
WHERE val < 30
GROUP BY category, ROLLUP(val)
WINDOW w AS (
    ORDER BY category, val
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL)
ORDER BY category, val NULLS LAST;

-- The window's own PARTITION BY and ORDER BY reference the target list, so
-- a nullable grouping column reaches them unharmed
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w AS (
    PARTITION BY category
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS true)
ORDER BY category NULLS LAST;

-- An aggregate query without GROUP BY produces one grouped row
SELECT count(*) OVER w AS cnt
FROM rpr_sort
HAVING count(*) > 0
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS true);

SELECT count(*) OVER w AS cnt, sum(val) AS total
FROM rpr_sort
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS true);

-- GROUP BY together with HAVING
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY category
HAVING count(*) > 1
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL)
ORDER BY category;

-- A grouping key that is not a plain Var does not stand in the way of a
-- DEFINE clause that names a plain-Var grouping key
SELECT category, val + 1 AS bumped, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY val + 1, category
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL)
ORDER BY category;

-- Grouping by the primary key exposes the dependent columns
SELECT id, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY id
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS val > 0)
ORDER BY id;

-- An aggregate is available to the window's ORDER BY, and it is the ordering
-- the pattern runs over: sum(val) descending puts B first, so the greedy
-- match starts there.  Ordering by category instead would start at A.
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY category
WINDOW w AS (
    ORDER BY sum(val) DESC
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS category IS NOT NULL)
ORDER BY category;

-- Grouping in a subquery leaves the outer RPR window alone
SELECT category, total, count(*) OVER w AS cnt
FROM (SELECT category, sum(val) AS total FROM rpr_sort GROUP BY category) s
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B*)
    DEFINE B AS total > PREV(total))
ORDER BY category;

-- A view over grouped input round-trips
CREATE VIEW rpr_grp_v AS
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY category
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS category IS NOT NULL);

SELECT pg_get_viewdef('rpr_grp_v'::regclass, true);
SELECT * FROM rpr_grp_v ORDER BY category;
DROP VIEW rpr_grp_v;

-- ROLLUP, with a DEFINE clause naming a column it can null.  The grouping
-- step's NULL reaches the predicate, which is then false, so the total row
-- is unmatched.
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL);

-- A match that spans several grouped rows, so the reduced frame is not just
-- the current row: A+ is greedy and stops at the row ROLLUP nulled.
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS category >= 'A')
ORDER BY category NULLS LAST;

-- The same for CUBE
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY CUBE(category)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL);

-- The same for an explicit set list containing the empty set
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY GROUPING SETS ((category), ())
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL);

-- The same when a second set simply omits the column
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY GROUPING SETS ((category), (val))
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL);

-- Naming val, which ROLLUP can null, works the same way as naming category
-- above: the rows where it is nulled do not match
SELECT category, val, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY category, ROLLUP(val)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS val > 0);

-- A navigation in the DEFINE clause reads the grouped column the same way
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B*)
    DEFINE B AS PREV(category) IS NOT NULL);

-- The same for a compound navigation
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w AS (
    ORDER BY category
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B*)
    DEFINE B AS PREV(LAST(category)) IS NOT NULL);

-- The same one query level down
SELECT * FROM (
    SELECT category, count(*) OVER w AS cnt
    FROM rpr_sort
    GROUP BY ROLLUP(category)
    WINDOW w AS (
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A)
        DEFINE A AS category IS NOT NULL)) s;

-- And in a view definition.  get_query_def() expands the DEFINE clause's
-- GROUP Vars like the target list's, so the deparsed text names the column
-- rather than the grouping step and the view re-parses.
CREATE VIEW rpr_grp_v2 AS
SELECT category, count(*) OVER w AS cnt
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL);

SELECT pg_get_viewdef('rpr_grp_v2'::regclass, true);
SELECT * FROM rpr_grp_v2 ORDER BY category NULLS LAST;
DROP VIEW rpr_grp_v2;

-- A DEFINE clause may spell a GROUP BY expression.  The planner stops at an
-- expression the window's input target already computes whole rather than
-- asking for the columns underneath it on their own, which grouping does not
-- make available; the DEFINE copy then resolves against that same column.
SELECT val + 1 AS bumped, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY val + 1
WINDOW w AS (
    ORDER BY val + 1
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS val + 1 > 0)
ORDER BY bumped;

-- A volatile expression that GROUP BY spells too is not rejected: the DEFINE
-- copy becomes a GROUP Var, and the pattern match reads the value the
-- grouping step computed, once per input row, not the expression.  The
-- sequence advancing by the number of input rows, not by the number of
-- DEFINE evaluations, shows that.
CREATE SEQUENCE rpr_grp_seq;
SELECT val, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY val, nextval('rpr_grp_seq')
WINDOW w AS (
    ORDER BY val
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS nextval('rpr_grp_seq') > 0)
ORDER BY val;
SELECT last_value = (SELECT count(*) FROM rpr_grp) AS once_per_row
FROM rpr_grp_seq;
DROP SEQUENCE rpr_grp_seq;

-- The same for a function call
SELECT upper(category) AS u, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY upper(category)
WINDOW w AS (
    ORDER BY upper(category)
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS upper(category) = 'A')
ORDER BY u;

-- The same for a cast
SELECT val::text AS t, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY val::text
WINDOW w AS (
    ORDER BY val::text
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS val::text > '0')
ORDER BY t;

-- And through a navigation operation, whose argument is read the same way
SELECT val + 1 AS bumped, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY val + 1
WINDOW w AS (
    ORDER BY val + 1
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B*)
    DEFINE B AS PREV(val + 1) > 0)
ORDER BY bumped;

-- The same under a grouping set, where the row the set nulls leaves the
-- predicate unknown and so unmatched.
SELECT val + 1 AS bumped, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY ROLLUP(val + 1)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS val + 1 > 0);

-- A DEFINE clause may repeat an expression the window itself orders by, with
-- no grouping in sight.  Adding the bare Vars a DEFINE clause reads to the
-- window's input target is what makes this hold: the DEFINE copy of
-- ROW(val, 1) IS NOT NULL is broken into per field tests before the plan is
-- built, and the bare val the break leaves behind is added to the input on
-- its own, next to the whole ROW(val, 1) the window orders by.
SELECT id, count(*) OVER w AS cnt
FROM rpr_grp
WINDOW w AS (
    ORDER BY ROW(val, 1)
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS ROW(val, 1) IS NOT NULL)
ORDER BY id;

-- ERROR: the bare column is another matter; grouping by an expression does
-- not make the columns inside it available on their own
SELECT val + 1 AS bumped, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY val + 1
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS val > 0);

-- ERROR: a column that was never grouped is still reported as one
SELECT category, count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY category
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS val > 0);

-- An inline OVER (...) carries a window clause of its own, and the
-- substitution reaches it the same way
SELECT category,
       count(*) OVER (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                      PATTERN (A)
                      DEFINE A AS category IS NOT NULL) AS cnt
FROM rpr_sort
GROUP BY ROLLUP(category);

-- The substitution visits every window clause, not just the first.  Here
-- the first window is a plain one and the row pattern is on the second.
SELECT category, count(*) OVER w1 AS plain, count(*) OVER w2 AS rpr
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w1 AS (ORDER BY category),
       w2 AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS category IS NOT NULL);

-- ERROR: an unreferenced window is substituted like any other, so its
-- DEFINE clause still cannot read a column that is not grouped, even
-- though the planner later drops the window
SELECT category
FROM rpr_sort
GROUP BY ROLLUP(category)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS val > 0);

-- An inner join's USING column of matching types is just the left input's
-- column, not a join alias Var, so plain grouping matches the DEFINE clause's
-- reference to it directly and the pattern reads it unharmed.
SELECT id, count(*) OVER w AS cnt
FROM rpr_grp JOIN rpr_sort USING (id)
GROUP BY id
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS id > 0)
ORDER BY id;

-- The same query through the join, with a grouping set that can null the
-- column the pattern reads
SELECT id, count(*) OVER w AS cnt
FROM rpr_grp JOIN rpr_sort USING (id)
GROUP BY ROLLUP(id)
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS id > 0)
ORDER BY id;

-- A FULL JOIN's USING column is a merged column -- a COALESCE over the two
-- sides rather than either one -- so a DEFINE clause naming it holds a join
-- alias Var, and only flatten_join_alias_for_parser() turns that into
-- something the grouped target list can be matched against.  The inner joins
-- above reach the pattern without that step.
SELECT id, count(*) OVER w AS cnt
FROM rpr_grp FULL JOIN rpr_sort USING (id)
GROUP BY ROLLUP(id)
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS id > 0)
ORDER BY id NULLS LAST;

-- A grouping set list holding only the empty set builds no RTE_GROUP
-- either, just like GROUP BY ()
SELECT count(*) OVER w AS cnt
FROM rpr_grp
GROUP BY GROUPING SETS (())
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE A AS true);

-- GROUP BY spelled as the merged column's COALESCE expansion, rather than the
-- join's own name, while DEFINE reads that same column: the two spellings
-- must compare equal despite the different tree shapes.
SELECT id + 1 AS b, count(*) OVER w AS cnt
FROM rpr_grp FULL JOIN rpr_sort USING (id)
GROUP BY COALESCE(rpr_grp.id, rpr_sort.id) + 1
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A) DEFINE A AS (id + 1) > 0)
ORDER BY 1;

-- Same construct as a view: the DEFINE clause must deparse to the plain
-- join column, not the two-sided COALESCE GROUP BY computed, or the printed
-- text would not re-parse.
CREATE VIEW rpr_fjcoal_v AS
SELECT COALESCE(rpr_grp.id, rpr_sort.id) + 1 AS idp1, count(*) OVER w AS cnt
FROM rpr_grp FULL JOIN rpr_sort USING (id)
GROUP BY COALESCE(rpr_grp.id, rpr_sort.id) + 1
WINDOW w AS (
    ORDER BY COALESCE(rpr_grp.id, rpr_sort.id) + 1
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS id + 1 > 0);

SELECT pg_get_viewdef('rpr_fjcoal_v'::regclass, true);
SELECT * FROM rpr_fjcoal_v ORDER BY 1;

-- The deparsed definition re-parses into an identical view.
CREATE VIEW rpr_fjcoal_v2 AS
 SELECT COALESCE(rpr_grp.id, rpr_sort.id) + 1 AS idp1,
    count(*) OVER w AS cnt
   FROM rpr_grp
     FULL JOIN rpr_sort USING (id)
   GROUP BY (COALESCE(rpr_grp.id, rpr_sort.id) + 1)
   WINDOW w AS (ORDER BY (COALESCE(rpr_grp.id, rpr_sort.id) + 1) ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
   AFTER MATCH SKIP PAST LAST ROW
   INITIAL
   PATTERN (a+)
   DEFINE
   a AS (id + 1) > 0);

SELECT pg_get_viewdef('rpr_fjcoal_v2'::regclass, true) =
      pg_get_viewdef('rpr_fjcoal_v'::regclass, true) AS same_definition;

DROP VIEW rpr_fjcoal_v2;
DROP VIEW rpr_fjcoal_v;

DROP TABLE rpr_grp;

DROP TABLE rpr_sort;

-- SQL function inlining: $1 in DEFINE must be substituted by
-- substitute_actual_parameters_in_from via query_tree_mutator.
CREATE TABLE rpr_srf_t (v int);
INSERT INTO rpr_srf_t SELECT generate_series(1, 5);

CREATE FUNCTION rpr_srf_inline(threshold int)
RETURNS TABLE (v int, cnt bigint)
LANGUAGE sql STABLE AS $$
    SELECT v::int, count(*) OVER w
    FROM rpr_srf_t
    WINDOW w AS (
        ORDER BY v
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS v > $1
    )
$$;

SELECT v, cnt FROM rpr_srf_inline(3) ORDER BY v;

DROP TABLE rpr_srf_t;
DROP FUNCTION rpr_srf_inline(int);

DROP TABLE rpr_planner;

-- A DEFINE clause reading a compound GROUP BY expression.  After grouping
-- only the expression itself exists, so make_window_input_target() has to
-- take it whole and stop: asking for the Vars underneath would ask the
-- grouping step for columns it cannot produce.  "((a + b))" alone on the
-- Output lines, with no bare a or b anywhere above the HashAggregate, is the
-- assertion.
CREATE TABLE rpr_gexp (a int, b int);
INSERT INTO rpr_gexp VALUES (1, 1), (2, 2), (3, 3), (4, 4);

SELECT a + b AS ab, count(*) OVER w AS c
FROM rpr_gexp
GROUP BY a + b
WINDOW w AS (ORDER BY a + b
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (X+) DEFINE X AS a + b > 2);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT a + b AS ab, count(*) OVER w AS c
FROM rpr_gexp
GROUP BY a + b
WINDOW w AS (ORDER BY a + b
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (X+) DEFINE X AS a + b > 2);

-- Reaching below the grouping expression is rejected, as it would be in any
-- other clause evaluated after grouping.
SELECT a + b AS ab, count(*) OVER w AS c
FROM rpr_gexp
GROUP BY a + b
WINDOW w AS (ORDER BY a + b
             ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
             PATTERN (X+) DEFINE X AS a > 2);

DROP TABLE rpr_gexp;

-- ============================================================
-- Stress Tests
-- ============================================================
-- Edge cases and stress scenarios

CREATE TABLE rpr_stress (id INT, val INT);
INSERT INTO rpr_stress SELECT i, i * 10 FROM generate_series(1, 20) i;

-- Very Long Query with Many Windows
SELECT id, val,
       COUNT(*) OVER w1 as cnt1,
       COUNT(*) OVER w2 as cnt2,
       COUNT(*) OVER w3 as cnt3
FROM rpr_stress
WINDOW w1 AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
),
w2 AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (B+)
    DEFINE B AS val > 50
),
w3 AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C+)
    DEFINE C AS val > 100
);

-- Deeply Nested Subqueries with RPR

SELECT * FROM (
    SELECT * FROM (
        SELECT * FROM (
            SELECT id, val,
                   COUNT(*) OVER w as cnt
            FROM rpr_stress
            WINDOW w AS (
                ORDER BY id
                ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                PATTERN (A+)
                DEFINE A AS val > 0
            )
        ) sub1
    ) sub2
) sub3
WHERE cnt > 10;

-- Complex Expression in DEFINE Clause

SELECT id, val,
       COUNT(*) OVER w as cnt
FROM rpr_stress
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ B)
    DEFINE A AS (val % 3 = 0 OR val % 5 = 0),
           B AS (val * 2 > 100 AND val / 2 < 100)
);

-- Window with No Matching Rows

SELECT id, val,
       COUNT(*) OVER w as cnt
FROM rpr_stress
WHERE val > 1000  -- No rows match
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

-- Window on Single Row

SELECT id, val,
       COUNT(*) OVER w as cnt
FROM rpr_stress
WHERE id = 10
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 0
);

DROP TABLE rpr_stress;

-- ============================================================
-- Error Limit Tests
-- ============================================================
-- Tests for error conditions in parse_rpr.c and rpr.c

CREATE TABLE rpr_errors (id INT, val INT);
INSERT INTO rpr_errors VALUES (1, 10), (2, 20);

-- DEFINE variable not in PATTERN (error)
SELECT id, val, COUNT(*) OVER w FROM rpr_errors
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A)
    DEFINE
      B AS TRUE
);

-- Row pattern variable-count boundary: 240 variables are accepted, 241
-- rejected.  A varId is one byte and the high nibble (0xF0-0xFF) is reserved
-- for control elements, so RPR_VARID_MAX is 0xEF and the 241st distinct
-- variable would fall into that reserved range.
-- The rejecting case names V241 in PATTERN only.  The limit counts distinct
-- PATTERN variables whether or not DEFINE names them, so V241 is still
-- counted, and that is what carries the total past the limit.
-- ECHO is silenced so the generated 240-variable clauses do not flood the
-- expected output.
--   240 variables -> maximum, accepted.
--   241 variables -> over maximum, rejected.
\set ECHO none
SELECT format($$SELECT COUNT(*) OVER w FROM rpr_errors
  WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  PATTERN (%s) DEFINE %s)$$,
  (SELECT string_agg('V' || i, ' ' ORDER BY i)
     FROM generate_series(1, 240) i),
  (SELECT string_agg('V' || i || ' AS val > 0', ', ' ORDER BY i)
     FROM generate_series(1, 240) i)) \gexec
SELECT format($$SELECT COUNT(*) OVER w FROM rpr_errors
  WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  PATTERN (%s) DEFINE %s)$$,
  (SELECT string_agg('V' || i, ' ' ORDER BY i)
     FROM generate_series(1, 241) i),
  (SELECT string_agg('V' || i || ' AS val > 0', ', ' ORDER BY i)
     FROM generate_series(1, 240) i)) \gexec
\set ECHO all

-- Pattern nesting-depth boundary: 254 levels are accepted, 255 rejected.
-- Reluctant quantifiers are not subject to quantifier multiplication, so the
-- nesting survives optimization and still reaches the depth check.
-- ECHO is silenced so the generated deeply nested patterns do not flood the
-- expected output.
--   254 nested GROUP{3,7}? -> depth 254 = maximum, accepted.
--   255 nested GROUP{3,7}? -> depth 255 > maximum, rejected.
\set ECHO none
SELECT format($$SELECT id, val, COUNT(*) OVER w FROM rpr_errors
  WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  PATTERN (%sA{3,7}?%s) DEFINE A AS val > 0)$$,
  repeat('(', 254), repeat('){3,7}?', 254)) \gexec
SELECT format($$SELECT id, val, COUNT(*) OVER w FROM rpr_errors
  WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  PATTERN (%sA{3,7}?%s) DEFINE A AS val > 0)$$,
  repeat('(', 255), repeat('){3,7}?', 255)) \gexec
\set ECHO all

DROP TABLE rpr_errors;

-- ============================================================
-- Basic Pattern Matching
-- ============================================================

-- A? (optional, greedy)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A?)
    DEFINE A AS val > 50
);

-- A{2} (exact count)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2})
    DEFINE A AS val <= 50
);

-- A{1,3} (bounded range, greedy)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{1,3})
    DEFINE A AS val <= 50
);

-- A | B (simple alternation)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A | B)
    DEFINE A AS val <= 30, B AS val > 70
);

-- A | B | C (three-way alternation)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A | B | C)
    DEFINE A AS val <= 20, B AS val BETWEEN 40 AND 60, C AS val > 80
);

-- A B C (concatenation)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C)
    DEFINE A AS val <= 30, B AS val BETWEEN 31 AND 60, C AS val > 60
);

-- A B? C (optional middle)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B? C)
    DEFINE A AS val <= 30, B AS val BETWEEN 31 AND 60, C AS val > 60
);

-- (A B)+ (grouped quantifier)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+)
    DEFINE A AS val <= 50, B AS val > 50
);

-- (A | B)+ C (alternation with quantifier)
SELECT id, val, count(*) OVER w AS c
FROM rpr_plan
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B)+ C)
    DEFINE A AS val <= 30, B AS val BETWEEN 31 AND 60, C AS val > 80
);

-- (A+ | (A | B)+)* - nested alternation inside quantified group
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM (VALUES
    (1, ARRAY['A', 'B']),
    (2, ARRAY['B']),
    (3, ARRAY['C'])
) AS t(id, flags)
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ | (A | B)+)*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Pathological Patterns
-- ============================================================
-- Nested unbounded quantifiers that the optimizer collapses.

-- (A*)* - nested unbounded (optimized to A*)
SELECT v, count(*) OVER w AS c
FROM (SELECT generate_series(1, 5) v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((A*)*)
    DEFINE A AS TRUE
);

-- (A*)+ - inner nullable (optimized to A*)
SELECT v, count(*) OVER w AS c
FROM (SELECT generate_series(1, 5) v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((A*)+)
    DEFINE A AS TRUE
);

-- (A+)* - outer nullable (optimized to A*)
SELECT v, count(*) OVER w AS c
FROM (SELECT generate_series(1, 5) v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((A+)*)
    DEFINE A AS TRUE
);

-- (A+)+ - both require match (optimized to A+)
SELECT v, count(*) OVER w AS c
FROM (SELECT generate_series(1, 5) v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((A+)+)
    DEFINE A AS TRUE
);

-- (((A)*)*)*  - triple nested (optimized to A*)
SELECT v, count(*) OVER w AS c
FROM (SELECT generate_series(1, 3) v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((((A)*)*)*)
    DEFINE A AS TRUE
);

-- Optional group with alternation: A ((B | C) (D | E))* F?
-- When only A matches, the * group matches 0 times and F? matches 0 times
SELECT id, val, match_len
FROM (SELECT id, val,
             COUNT(*) OVER w AS match_len
      FROM (VALUES (1, 1), (2, 99)) AS t(id, val)
      WINDOW w AS (
          ORDER BY id
          ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
          AFTER MATCH SKIP PAST LAST ROW
          PATTERN (A ((B | C) (D | E))* F?)
          DEFINE A AS val = 1,
                 B AS val = 2, C AS val = 3,
                 D AS val = 4, E AS val = 5,
                 F AS val = 6
      )
) s;

DROP TABLE rpr_plan;
