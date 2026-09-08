--
-- Test for row pattern recognition: WINDOW clause integration and
-- scenario tests using synthetic stock data.
--
-- Parser/planner tests: rpr_base.sql
-- NFA engine tests: rpr_nfa.sql
-- EXPLAIN statistics tests: rpr_explain.sql
--

\getenv abs_srcdir PG_ABS_SRCDIR

-- Synthetic stock data for RPR pattern matching tests
CREATE TABLE rpr_stock (
       part_id integer,
       rn      integer,
       price   numeric(10,3),
       volume  bigint,
       open    numeric(10,3),
       low     numeric(10,3),
       high    numeric(10,3)
);

\set filename :abs_srcdir '/data/stock.data'
COPY rpr_stock FROM :'filename';
ANALYZE rpr_stock;

CREATE TEMP TABLE stock (company TEXT, tdate DATE, price INTEGER);
INSERT INTO stock VALUES
('company1', '2023-07-01', 100), ('company1', '2023-07-02', 200),
('company1', '2023-07-03', 150), ('company1', '2023-07-04', 140),
('company1', '2023-07-05', 150), ('company1', '2023-07-06', 90),
('company1', '2023-07-07', 110), ('company1', '2023-07-08', 130),
('company1', '2023-07-09', 120), ('company1', '2023-07-10', 130),
('company2', '2023-07-01', 50), ('company2', '2023-07-02', 2000),
('company2', '2023-07-03', 1500), ('company2', '2023-07-04', 1400),
('company2', '2023-07-05', 1500), ('company2', '2023-07-06', 60),
('company2', '2023-07-07', 1100), ('company2', '2023-07-08', 1300),
('company2', '2023-07-09', 1200), ('company2', '2023-07-10', 1300);

SELECT * FROM stock;

--
-- Basic pattern matching with PREV/NEXT
--

-- basic test using PREV
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w,
 nth_value(tdate, 2) OVER w AS nth_second
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UP+ DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- basic test using PREV. UP appears twice
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w,
 nth_value(tdate, 2) OVER w AS nth_second
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UP+ DOWN+ UP+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- basic test using PREV. Use '*'
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w,
 nth_value(tdate, 2) OVER w AS nth_second
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UP* DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- basic test using PREV. Use '?'
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w,
 nth_value(tdate, 2) OVER w AS nth_second
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UP? DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- test using alternation (|) with sequence
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START (UP | DOWN))
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- test using alternation (|) with group quantifier
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START (UP | DOWN)+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- test using nested alternation
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START ((UP DOWN) | FLAT)+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price),
  FLAT AS price = PREV(price)
);

-- test using group with quantifier
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN ((UP DOWN)+)
 DEFINE
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- test using absolute threshold values (not relative PREV)
-- HIGH: price > 150, LOW: price < 100, MID: neutral range
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (LOW MID* HIGH)
 DEFINE
  LOW AS price < 100,
  MID AS price >= 100 AND price <= 150,
  HIGH AS price > 150
);

-- test threshold-based pattern with alternation
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (LOW (MID | HIGH)+)
 DEFINE
  LOW AS price < 100,
  MID AS price >= 100 AND price <= 150,
  HIGH AS price > 150
);

-- basic test with fixed-length pattern (A A A = exactly 3)
SELECT company, tdate, price, count(*) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (A A A)
 DEFINE
  A AS price >= 140 AND price <= 150
);

-- test using {n} quantifier (A A A should be optimized to A{3})
SELECT company, tdate, price, count(*) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (A{3})
 DEFINE
  A AS price >= 140 AND price <= 150
);

-- test using {n,} quantifier (2 or more)
SELECT company, tdate, price, count(*) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (A{2,})
 DEFINE
  A AS price > 100
);

-- test using {n,m} quantifier (2 to 4)
SELECT company, tdate, price, count(*) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (A{2,4})
 DEFINE
  A AS price > 100
);

-- test prefix/suffix merge optimization with bounded quantifier
-- Pattern A B (A B){1,2} A B should be optimized to (A B){3,4}
CREATE TEMP TABLE rpr_t (id int, val text);
INSERT INTO rpr_t VALUES
  (1,'A'),(2,'B'),
  (3,'A'),(4,'B'),
  (5,'A'),(6,'B'),
  (7,'A'),(8,'B'),
  (9,'X');
SELECT id, val, count(*) OVER w AS match_count
FROM rpr_t
WINDOW w AS (
  ORDER BY id
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  AFTER MATCH SKIP TO NEXT ROW
  INITIAL
  PATTERN (A B (A B){1,2} A B)
  DEFINE
    A AS val = 'A',
    B AS val = 'B'
);
DROP TABLE rpr_t;

-- last_value() should remain consistent
SELECT company, tdate, price, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UP+ DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- omit "START" in DEFINE but it is ok because "START AS TRUE" is
-- implicitly defined. per spec.
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w,
 nth_value(tdate, 2) OVER w AS nth_second
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UP+ DOWN+)
 DEFINE
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- the first row start with less than or equal to 100
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (LOWPRICE UP+ DOWN+)
 DEFINE
  LOWPRICE AS price <= 100,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- second row raises 120%
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (LOWPRICE UP+ DOWN+)
 DEFINE
  LOWPRICE AS price <= 100,
  UP AS price > PREV(price) * 1.2,
  DOWN AS price < PREV(price)
);

-- using NEXT
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UPDOWN)
 DEFINE
  START AS TRUE,
  UPDOWN AS price > PREV(price) AND price > NEXT(price)
);

-- using AFTER MATCH SKIP TO NEXT ROW (same pattern as above;
-- match length is always 2, so result is identical to SKIP PAST LAST ROW.
-- SKIP TO NEXT ROW's distinct effect is tested in backtracking section.)
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP TO NEXT ROW
 INITIAL
 PATTERN (START UPDOWN)
 DEFINE
  START AS TRUE,
  UPDOWN AS price > PREV(price) AND price > NEXT(price)
);

-- PREV returns NULL at the partition's first row (no earlier row to fetch)
SELECT company, tdate, price, count(*) OVER w
FROM stock
WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 PATTERN (BOUNDARY REST+)
 DEFINE
  BOUNDARY AS PREV(price) IS NULL,
  REST AS PREV(price) IS NOT NULL
);

-- NEXT returns NULL at the partition's last row (no later row to fetch)
SELECT company, tdate, price, count(*) OVER w
FROM stock
WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (A+ BOUNDARY)
 DEFINE
  A AS NEXT(price) IS NOT NULL,
  BOUNDARY AS NEXT(price) IS NULL
);

-- DESC order: PREV refers to the row with later date
SELECT company, tdate, price, count(*) OVER w
FROM stock
WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate DESC
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (START DOWN+ UP+)
 DEFINE
  START AS TRUE,
  DOWN AS price < PREV(price),
  UP AS price > PREV(price)
);

-- Multiple partitions with unequal sizes
WITH multi_part AS (
 SELECT * FROM (VALUES
  ('a', 1, 10), ('a', 2, 20), ('a', 3, 15),
  ('b', 1, 5),
  ('c', 1, 100), ('c', 2, 200), ('c', 3, 150), ('c', 4, 140), ('c', 5, 300)
 ) AS t(grp, id, val)
)
SELECT grp, id, val, count(*) OVER w
FROM multi_part
WINDOW w AS (
 PARTITION BY grp
 ORDER BY id
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (A B+)
 DEFINE
  A AS val <= NEXT(val),
  B AS val > PREV(val) OR val < PREV(val)
);

-- FLOAT/NUMERIC DEFINE conditions
WITH float_data AS (
 SELECT * FROM (VALUES
  (1, 1.0::float8), (2, 1.5), (3, 1.4999), (4, 1.50001), (5, 0.1)
 ) AS t(id, val)
)
SELECT id, val, count(*) OVER w
FROM float_data
WINDOW w AS (
 ORDER BY id
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (A B+)
 DEFINE
  A AS TRUE,
  B AS val > PREV(val) * 0.99
);

--
-- Error cases: PREV/NEXT usage restrictions
--

-- Nested PREV
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > PREV(PREV(price))
);

-- Nested NEXT
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > NEXT(NEXT(price))
);

-- PREV nested inside NEXT
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > NEXT(PREV(price))
);

-- PREV nested inside expression inside NEXT
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > NEXT(price * PREV(price))
);

-- Triple nesting: error reported at outermost PREV
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > PREV(PREV(PREV(price)))
);

-- No column reference in PREV/NEXT argument
-- PREV(1): constant only, no column reference
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(1) > 0
);

-- NEXT(1 + 2): constant expression, no column reference
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS NEXT(1 + 2) > 0
);

-- 2-arg form: PREV(1, 1): constant expression as first arg
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(1, 1) > 0
);

-- Compound navigation without a column reference must be rejected too,
-- consistent with the simple forms above.
-- PREV(FIRST(1)): compound, constant only, no column reference
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(FIRST(1)) > 0
);

-- NEXT(LAST(1 + 2)): compound, constant expression, no column reference
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS NEXT(LAST(1 + 2)) > 0
);

-- PREV(FIRST(1, 2)): compound, two-arg inner, no column reference
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(FIRST(1, 2)) > 0
);

-- PREV(FIRST(1), 2): compound, outer offset only, no column reference
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(FIRST(1), 2) > 0
);

-- PREV(FIRST(1, 2), 3): compound, inner and outer offsets, no column reference
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(FIRST(1, 2), 3) > 0
);

-- Non-constant offset: column reference as offset
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(price, price) > 0
);

-- Non-constant offset: column reference in compound inner offset
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(LAST(price, price), 2) > 0
);

-- Non-constant offset: column reference in compound outer offset
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(LAST(price, 1), price) > 0
);

-- Non-constant offset: volatile function as offset
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(price, random()::int) > 0
);

-- Non-constant offset: subquery as offset
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(price, (SELECT 1)) > 0
);

-- First arg: subquery (caught by DEFINE-level subquery restriction)
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(price + (SELECT 1)) > 0
);

-- Volatile function inside nav.arg is rejected in the planner
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price + random() * 0) >= 0
);

-- nextval is volatile, so a DEFINE that calls it is rejected
CREATE SEQUENCE rpr_seq;
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > nextval('rpr_seq')
);
DROP SEQUENCE rpr_seq;

-- A volatile DEFINE is rejected in the planner, so a view that hides one is
-- created successfully and errors only when read.
CREATE TEMP VIEW rpr_volatile_view AS
SELECT company, tdate, price, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A+)
    DEFINE A AS price > random() * 0
);
SELECT * FROM rpr_volatile_view;
DROP VIEW rpr_volatile_view;

-- DEFINE cannot reference an outer query's column.  A correlated outer
-- reference must produce a clean error, not the internal "Upper-level Var"
-- elog that pull_var_clause would otherwise raise.
-- Qualified outer reference (o.threshold):
SELECT * FROM (VALUES (95)) AS o(threshold),
LATERAL (
    SELECT price FROM stock
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS price > o.threshold
    )
) s;
-- Unqualified name resolving to the outer column (threshold):
SELECT * FROM (VALUES (95)) AS o(threshold),
LATERAL (
    SELECT price FROM stock
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS price > threshold
    )
) s;
-- Outer reference inside a navigation argument is rejected too:
SELECT * FROM (VALUES (95)) AS o(threshold),
LATERAL (
    SELECT price FROM stock
    WINDOW w AS (
        PARTITION BY company ORDER BY tdate
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS PREV(o.threshold, 1) > 0
    )
) s;

-- DEFINE rejects a schema-qualified column reference (three or more name
-- parts) once it resolves; the qualified form itself is not allowed.  (stock
-- is a temp table, so it is qualified with pg_temp here.)
-- 3-part (schema.table.column):
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS pg_temp.stock.price > 0
);
-- whole-row variant (schema.table.*):
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS (pg_temp.stock.*) IS NOT NULL
);
-- A two-part table-qualified whole-row reference is rejected as well, through
-- a separate range-variable check (a bare relation name is instead accepted
-- as a whole-row Var).
-- 2-part (table.*):
SELECT price FROM stock
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS (stock.*) IS NOT NULL
);

--
-- 2-arg PREV/NEXT: functional tests
--

-- PREV(price, 2): with A=any, B matches where the price beats the one two rows
-- back.  On company1 (100, 200, 150, 140, 150, 90, 110, 130, 120, 130) that is
-- 200 -> 150, then 110 -> 130 -> 120, which stops where 130 only ties 130.
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS price > PREV(price, 2)
);

-- NEXT(price, 2): A matches while the price beats the one two rows ahead, so
-- company1 gives 200 on its own, since 150 only ties the 150 ahead of it, and
-- then 140, 150 up to where 90 falls short of 130.
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS price > NEXT(price, 2)
);

-- Expressions inside PREV/NEXT arg: expr is evaluated on target row
-- PREV(price - 50, 1): fetches (price - 50) from 1 row back
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS price > PREV(price - 50, 1)
);

-- NEXT(price * 2, 1): fetches (price * 2) from 1 row ahead
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS price < NEXT(price * 2, 1)
);

-- Large offset: PREV(val, 999) on 1000-row series matches only last row
-- NEXT(val, 999) matches only first row
SELECT val, first_value(val) OVER w, last_value(val) OVER w, count(*) OVER w
FROM generate_series(1, 1000) AS t(val)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(val, 999) = 1
)
ORDER BY val DESC LIMIT 3;

SELECT val, first_value(val) OVER w, last_value(val) OVER w, count(*) OVER w
FROM generate_series(1, 1000) AS t(val)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(val, 999) = 1000
)
LIMIT 3;

-- PREV(price, 0): offset 0 means current row, always equal to price
-- A+ matches entire partition as one group; count = partition size
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price, 0) = price
);

-- 2-arg PREV/NEXT: negative offset
SELECT company, tdate, price, first_value(price) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price, -1) IS NOT NULL
);

-- 2-arg PREV/NEXT: NULL offset (typed)
SELECT company, tdate, price, first_value(price) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price, NULL::int8) IS NOT NULL
);

-- 2-arg PREV/NEXT: NULL offset (untyped)
SELECT company, tdate, price, first_value(price) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price, NULL) IS NOT NULL
);

-- 2-arg PREV/NEXT: host variable negative and NULL
PREPARE test_prev_offset(int8) AS
SELECT company, tdate, price, first_value(price) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS price > PREV(price, $1)
);
EXECUTE test_prev_offset(-1);
EXECUTE test_prev_offset(NULL);
DEALLOCATE test_prev_offset;

-- 2-arg PREV/NEXT: host variable with expression (0 + $1)
PREPARE test_prev_offset(int8) AS
SELECT company, tdate, price, first_value(price) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS price > PREV(price, 0 + $1)
);
EXECUTE test_prev_offset(-1);
EXECUTE test_prev_offset(NULL);
DEALLOCATE test_prev_offset;

-- 2-arg PREV/NEXT: host variable with positive value.  A generic plan keeps
-- the parameter as a Param, so the offset is resolved at run time; a custom
-- plan would fold it to a constant and settle the reach at init.
SET plan_cache_mode = force_generic_plan;
PREPARE test_prev_offset(int8) AS
SELECT company, tdate, price, first_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS price > PREV(price, $1)
);
EXECUTE test_prev_offset(1);
EXECUTE test_prev_offset(2);
DEALLOCATE test_prev_offset;
RESET plan_cache_mode;

-- 2-arg: two PREV with different offsets in same DEFINE clause
-- B: price exceeds both 1-back and 2-back values
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS price > PREV(price, 1) AND price > PREV(price, 2)
);

-- 2-arg: PREV and NEXT with explicit offsets in same DEFINE clause
-- A: price exceeds 1-back and is below 1-ahead (ascending interior point)
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS price > PREV(price, 1) AND price < NEXT(price, 1)
);

-- Pass-by-ref types: two PREV calls targeting different positions, so the
-- first navigation result must survive the second fetch.  B compares the
-- 1-back and 2-back tdate text.
SELECT company, tdate, tdate::text AS tdate_text,
       first_value(tdate::text) OVER w, last_value(tdate::text) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS PREV(tdate::text, 1) > PREV(tdate::text, 2)
);

-- numeric: PREV(price::numeric, 1) > PREV(price::numeric, 2)
-- B matches when price 1-back > price 2-back (ascending pair).
SELECT company, tdate, price::numeric AS nprice,
       first_value(price::numeric) OVER w, last_value(price::numeric) OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS PREV(price::numeric, 1) > PREV(price::numeric, 2)
);

-- Typmod coercion over a navigation result: casting PREV(p) (a numeric(10,3)
-- column) to a narrower numeric(8,2) inside DEFINE forces coerce_type_typmod,
-- which calls exprTypmod() on the RPRNavExpr.
CREATE TEMP TABLE rpr_typmod (id int, p numeric(10,3));
INSERT INTO rpr_typmod VALUES (1, 1.5), (2, 2.5), (3, 3.5);
SELECT id, count(*) OVER w AS cnt
FROM rpr_typmod
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS CAST(PREV(p) AS numeric(8,2)) > 0
);
DROP TABLE rpr_typmod;

--
-- FIRST/LAST navigation
--

-- Test data for FIRST/LAST: values cycle back so FIRST(val) = LAST(val)
-- at specific positions.
CREATE TEMP TABLE rpr_nav (id int, val int);
INSERT INTO rpr_nav VALUES (1,10),(2,20),(3,30),(4,10),(5,50),(6,10);

-- FIRST(val) = constant: B matches when match_start has val=10
-- match_start=1(10): A=id1, B=id2, FIRST(val)=10 -> match {1,2}
-- match_start=3(30): A=id3, B=id4, FIRST(val)=30!=10 -> no match
-- match_start=4(10): A=id4, B=id5, FIRST(val)=10 -> match {4,5}
SELECT id, val, first_value(id) OVER w AS mf, last_value(id) OVER w AS ml
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS TRUE, B AS FIRST(val) = 10
);

-- LAST(val): always equals current row's val (offset 0 default)
-- Equivalent to: B AS val > 15
SELECT id, val, first_value(id) OVER w AS mf, last_value(id) OVER w AS ml
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS TRUE, B AS LAST(val) > 15
);

-- Reluctant A+? with FIRST(val) = LAST(val): find shortest match where
-- first and last rows have the same val.
-- match_start=1(10): reluctant tries B early:
--   id2(20!=10), id3(30!=10), id4(10=10) -> match {1,2,3,4}
-- match_start=5(50): id6(10!=50) -> no match
SELECT id, val, first_value(id) OVER w AS mf, last_value(id) OVER w AS ml
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+? B)
    DEFINE A AS TRUE, B AS FIRST(val) = LAST(val)
);

-- Greedy A+ with FIRST(val) = LAST(val): find longest match where
-- first and last rows have the same val.
-- match_start=1(10): greedy A eats all, B tries last:
--   id6(10=10) -> match {1,2,3,4,5,6}
SELECT id, val, first_value(id) OVER w AS mf, last_value(id) OVER w AS ml
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS TRUE, B AS FIRST(val) = LAST(val)
);

-- SKIP TO NEXT ROW with FIRST(val) = LAST(val): overlapping match attempts.
-- Each row reports only the match that starts at it.
SELECT id, val, first_value(id) OVER w AS mf, last_value(id) OVER w AS ml
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+? B)
    DEFINE A AS TRUE, B AS FIRST(val) = LAST(val)
);

-- FIRST/LAST 2-arg offset form
--
-- FIRST(val, 0) = FIRST(val): match_start row
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS FIRST(val, 0) = 10
);

-- FIRST(val, 1): match_start + 1 row (second row of match)
-- match_start=1(10): FIRST(val,1)=20, B needs val=20 -> id2(20) match, id3(30) no
-- match_start=3(30): FIRST(val,1)=10, B needs val=10 -> id4(10) match
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS val = FIRST(val, 1)
);

-- FIRST(val, 99): offset beyond match range -> NULL, no match
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS FIRST(val, 99) IS NOT NULL
);

-- LAST(val, 0) = LAST(val): current row
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS LAST(val, 0) > 15
);

-- LAST(val, 1): one row back from current (previous match row)
-- At B evaluation on id2: LAST(val,1) = val at id1 = 10
-- B matches when previous row val < 30
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS LAST(val, 1) < 30
);

-- LAST(val, 99): offset before match_start -> NULL
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS LAST(val, 99) IS NOT NULL
);

-- Error: NULL offset
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS FIRST(val, NULL::int8) IS NULL
);

-- Error: negative offset
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS LAST(val, -1) IS NULL
);

-- Functional notation: should access column, not RPR navigation
CREATE TEMP TABLE rpr_names (prev int, next int, first text, last text);
INSERT INTO rpr_names VALUES (1, 2, 'Joe', 'Blow');
SELECT prev(f), next(f), first(f), last(f) FROM rpr_names f;
DROP TABLE rpr_names;

-- Compound navigation: PREV(FIRST(val), M)
-- rpr_nav: (1,10),(2,20),(3,30),(4,10),(5,50),(6,10)
-- PREV(FIRST(val), 1): target = match_start + 0 - 1 = match_start - 1
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val), 1) > 0
);

-- NEXT(FIRST(val, 1), 1): target = match_start + 1 + 1 = match_start + 2
-- At match_start=1, B on id2: target=1+1+1=3(val=30), 30>0 -> true
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val, 1), 1) > 0
);

-- PREV(LAST(val), 2): LAST(val) is the current row (inner offset 0), so
-- target = currentpos - 0 - 2 = currentpos - 2.  Same backward reach as PREV(val, 2).
-- At currentpos=2 (start id=1): target=0 -> out of range -> NULL -> B fails.
-- At currentpos=3 (start id=2): target=1(val=10) -> in range -> B runs on id3..id6,
-- so the match is id2..id6.
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(LAST(val), 2) IS NOT NULL
);

-- NEXT(LAST(val, 1), 2): LAST(val, 1) is one row back (inner offset 1), then
-- NEXT adds 2, so target = currentpos - 1 + 2 = currentpos + 1.  Looks one row
-- ahead: same as NEXT(val, 1).
-- At currentpos=2 (start id=1): target=3(val=30) -> in range -> B true.
-- B stays true through id5 (target=6); at id6 target=7 -> out of range -> NULL,
-- so the match is id1..id5.
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(LAST(val, 1), 2) IS NOT NULL
);

-- Compound: outer offset beyond partition (PREV far back)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val), 99) IS NOT NULL
);

-- Compound: outer offset beyond partition (NEXT far forward)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val), 99) IS NOT NULL
);

-- Compound: inner offset beyond match range (FIRST offset too large)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val, 99), 1) IS NOT NULL
);

-- Compound: inner offset beyond match range (LAST offset too large)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(LAST(val, 99), 1) IS NOT NULL
);

-- Compound: NULL outer offset (runtime error)
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(val), NULL::int8) IS NULL
);

-- Compound: negative outer offset (runtime error)
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(val), -1) IS NULL
);

-- Compound: an out-of-range inner offset must not skip validation of the outer
-- one.  All four arms resolve their outer offset through the same call, so each
-- appears once, and the negative and the null case take two arms apiece.
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val, 99), -1) IS NULL
);
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(LAST(val, 99), NULL::int8) IS NULL
);
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val, 99), NULL::int8) IS NULL
);
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(LAST(val, 99), -1) IS NULL
);

-- Same with a host variable, where the offset is not a Const the planner can
-- fold: one prepared statement, and only the outer offset decides the outcome.
-- The reach reads "runtime" here; a custom plan would fold it to 99 - 1 = 98.
SET plan_cache_mode = force_generic_plan;
PREPARE test_compound_illegal(int8, int8) AS
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val, $1), $2) IS NULL
);
EXPLAIN (COSTS OFF) EXECUTE test_compound_illegal(99, 1);
EXECUTE test_compound_illegal(99, 1);
EXECUTE test_compound_illegal(99, -1);
EXECUTE test_compound_illegal(99, NULL);
EXECUTE test_compound_illegal(0, -1);
DEALLOCATE test_compound_illegal;
RESET plan_cache_mode;

-- An offset is settled before the first row is fetched, so a partition with no
-- rows at all rejects an illegal one just the same, and a legal one returns no
-- rows rather than failing.
CREATE TABLE rpr_nav_empty (id int, val int);
SELECT id, count(*) OVER w FROM rpr_nav_empty WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(val, -1) IS NULL
);
SELECT id, count(*) OVER w FROM rpr_nav_empty WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(val, 1) IS NULL
);
SET plan_cache_mode = force_generic_plan;
PREPARE test_empty_offset(int8) AS
SELECT id, count(*) OVER w FROM rpr_nav_empty WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(val, $1) IS NULL
);
EXECUTE test_empty_offset(-1);
EXECUTE test_empty_offset(NULL);
EXECUTE test_empty_offset(1);
DEALLOCATE test_empty_offset;
RESET plan_cache_mode;
DROP TABLE rpr_nav_empty;

-- Outer offset overflows int64: target position out of range -> NULL.
-- Plain NEXT(val, INT64_MAX): currentpos + INT64_MAX overflows.
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(val, 9223372036854775807) IS NULL
);

-- Compound NEXT(FIRST()): outer offset overflow.  Inner offset 1 forces
-- inner_pos >= 1, so inner_pos + INT64_MAX overflows at every match.
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val, 1), 9223372036854775807) IS NULL
);

-- Compound NEXT(LAST()): outer offset overflow.
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(val), 9223372036854775807) IS NULL
);

-- Compound: default offsets on both sides
-- PREV(FIRST(val)): inner=0 (match_start), outer=1 -> target = match_start - 1
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val)) IS NOT NULL
);

-- NEXT(LAST(val)): inner=0 (currentpos), outer=1 -> target = currentpos + 1
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(LAST(val)) IS NOT NULL
);

-- Compound: inner NULL offset (runtime error)
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(val, NULL::int8), 1) IS NULL
);

-- Compound: inner negative offset (runtime error)
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(val, -1), 1) IS NULL
);

-- Offset argument whose type has no implicit cast to bigint (parse error)
SELECT id, val, count(*) OVER w FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS val > PREV(val, 1.5)
);

-- Compound + host variable offsets
PREPARE test_compound_offset(int8, int8) AS
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val, $1), $2) IS NOT NULL
);
EXECUTE test_compound_offset(0, 1);
EXECUTE test_compound_offset(1, 1);
DEALLOCATE test_compound_offset;

-- Compound + SKIP TO NEXT ROW: overlapping matches with PREV(FIRST())
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val), 1) > 0
);

-- Compound + multiple partitions
CREATE TEMP TABLE rpr_nav_part (gid int, id int, val int);
INSERT INTO rpr_nav_part VALUES
    (1,1,10),(1,2,20),(1,3,30),
    (2,1,40),(2,2,50),(2,3,60);
SELECT gid, id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_part WINDOW w AS (
    PARTITION BY gid ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val), 1) > 0
);
DROP TABLE rpr_nav_part;

-- Reverse nesting: FIRST wrapping PREV is prohibited
SELECT id, val FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B)
    DEFINE A AS TRUE, B AS FIRST(PREV(val)) > 0
);

-- Reverse nesting: LAST wrapping NEXT is prohibited
SELECT id, val FROM rpr_nav WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B)
    DEFINE A AS TRUE, B AS LAST(NEXT(val)) > 0
);

DROP TABLE rpr_nav;

--
-- SKIP TO / Backtracking / Frame boundary
--

-- match everything
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 INITIAL
 PATTERN (A+)
 DEFINE
  A AS TRUE
);

-- nth_value beyond reduced frame (no IGNORE NULLS)
SELECT company, tdate, price,
 nth_value(price, 5) OVER w AS nth_5
FROM stock
WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (START UP+ DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- backtracking with reclassification of rows
-- using AFTER MATCH SKIP PAST LAST ROW
SELECT company, tdate, price, first_value(tdate) OVER w, last_value(tdate) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 INITIAL
 PATTERN (A+ B+)
 DEFINE
  A AS price > 100,
  B AS price > 100
);

-- backtracking with reclassification of rows
-- using AFTER MATCH SKIP TO NEXT ROW
SELECT company, tdate, price, first_value(tdate) OVER w, last_value(tdate) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP TO NEXT ROW
 INITIAL
 PATTERN (A+ B+)
 DEFINE
  A AS price > 100,
  B AS price > 100
);

-- SKIP TO NEXT ROW with limited frame
-- Each row should produce its own match within its frame
WITH data AS (
 SELECT * FROM (VALUES
  ('A', 1), ('A', 2),
  ('B', 3), ('B', 4)
 ) AS t(gid, id)
)
SELECT gid, id, array_agg(id) OVER w
FROM data
WINDOW w AS (
 PARTITION BY gid
 ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING
 AFTER MATCH SKIP TO NEXT ROW
 PATTERN (A+)
 DEFINE A AS id < 10
);

-- Limited frame with absorption test
WITH frame_absorb_test AS (
 SELECT * FROM (VALUES
  (0, 'A'), (1, 'A'), (2, 'A'), (3, 'B')
 ) AS t(id, flag)
)
SELECT id, flag, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM frame_absorb_test
WINDOW w AS (
 ORDER BY id
 ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (A+ B)
 DEFINE
  A AS flag = 'A',
  B AS flag = 'B'
);

-- ROWS BETWEEN CURRENT ROW AND offset FOLLOWING
SELECT company, tdate, price, first_value(tdate) OVER w, last_value(tdate) OVER w,
 count(*) OVER w
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (START UP+ DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

--
-- Aggregates
--

-- using AFTER MATCH SKIP PAST LAST ROW
SELECT company, tdate, price,
 first_value(price) OVER w,
 last_value(price) OVER w,
 max(price) OVER w,
 min(price) OVER w,
 sum(price) OVER w,
 avg(price) OVER w,
 count(price) OVER w
FROM stock
WINDOW w AS (
PARTITION BY company
ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
AFTER MATCH SKIP PAST LAST ROW
INITIAL
PATTERN (START UP+ DOWN+)
DEFINE
START AS TRUE,
UP AS price > PREV(price),
DOWN AS price < PREV(price)
);

-- using AFTER MATCH SKIP TO NEXT ROW
SELECT company, tdate, price,
 first_value(price) OVER w,
 last_value(price) OVER w,
 max(price) OVER w,
 min(price) OVER w,
 sum(price) OVER w,
 avg(price) OVER w,
 count(price) OVER w
FROM stock
WINDOW w AS (
PARTITION BY company
ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
AFTER MATCH SKIP TO NEXT ROW
INITIAL
PATTERN (START UP+ DOWN+)
DEFINE
START AS TRUE,
UP AS price > PREV(price),
DOWN AS price < PREV(price)
);

-- row_number() within RPR reduced frame
SELECT company, tdate, price, row_number() OVER w, count(*) OVER w
FROM stock
WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (START UP+ DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

--
-- SQL Integration: JOIN, CTE, LATERAL
--

-- JOIN case
CREATE TEMP TABLE t1 (i int, v1 int);
CREATE TEMP TABLE t2 (j int, v2 int);
INSERT INTO t1 VALUES(1,10);
INSERT INTO t1 VALUES(1,11);
INSERT INTO t1 VALUES(1,12);
INSERT INTO t2 VALUES(2,10);
INSERT INTO t2 VALUES(2,11);
INSERT INTO t2 VALUES(2,12);

SELECT * FROM t1, t2 WHERE t1.v1 <= 11 AND t2.v2 <= 11;

SELECT *, count(*) OVER w FROM t1, t2
WINDOW w AS (
 PARTITION BY t1.i
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (A)
 DEFINE
 A AS v1 <= 11 AND v2 <= 11
);

-- WITH case
WITH wstock AS (
  SELECT * FROM stock WHERE tdate < '2023-07-08'
)
SELECT tdate, price,
first_value(tdate) OVER w,
count(*) OVER w
 FROM wstock
 WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UP+ DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- ReScan test: LATERAL join forces WindowAgg rescan with RPR
SELECT g.x, sub.*
FROM generate_series(1, 2) g(x),
LATERAL (
  SELECT id, price, count(*) OVER w AS c
  FROM (VALUES (1, 100), (2, 200), (3, 150)) AS t(id, price)
  WHERE id <= g.x + 1
  WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (START UP+)
    DEFINE
      START AS TRUE,
      UP AS price > PREV(price)
  )
) sub
ORDER BY g.x, sub.id;

-- PREV has multiple column reference
CREATE TEMP TABLE rpr1 (id INTEGER, i SERIAL, j INTEGER);
INSERT INTO rpr1(id, j) SELECT 1, g*2 FROM generate_series(1, 10) AS g;
SELECT id, i, j, count(*) OVER w
 FROM rpr1
 WINDOW w AS (
 PARTITION BY id
 ORDER BY i
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 INITIAL
 PATTERN (START COND+)
 DEFINE
  START AS TRUE,
  COND AS PREV(i + j + 1) < 10
);

--
-- Large-scale / scalability tests
--

-- Smoke test for larger partitions.
WITH s AS (
 SELECT v, count(*) OVER w AS c
 FROM (SELECT generate_series(1, 5000) v)
 WINDOW w AS (
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  AFTER MATCH SKIP PAST LAST ROW
  INITIAL
  PATTERN ( r+ )
  DEFINE r AS TRUE
 )
)
-- Should be exactly one long match across all rows.
SELECT * FROM s WHERE c > 0;

WITH s AS (
 SELECT v, count(*) OVER w AS c
 FROM (SELECT generate_series(1, 5000) v)
 WINDOW w AS (
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  AFTER MATCH SKIP PAST LAST ROW
  INITIAL
  PATTERN ( r )
  DEFINE r AS TRUE
 )
)
-- Every row should be its own match.
SELECT count(*) FROM s WHERE c > 0;

-- Large partition test: 100K rows with A+ B* C{10000,} pattern
-- Tests that int32 count doesn't overflow with large repetitions
WITH data AS (
 SELECT generate_series(0, 100000) AS v
),
result AS (
 SELECT v,
        count(*) OVER w AS match_len,
        first_value(v) OVER w AS match_first,
        last_value(v) OVER w AS match_last
 FROM data
 WINDOW w AS (
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  AFTER MATCH SKIP PAST LAST ROW
  INITIAL
  PATTERN (A+ B* C{10000,})
  DEFINE
   A AS v < 33333,
   B AS v >= 33333 AND v < 66666,
   C AS v >= 66666 AND v < 99999
 )
)
-- Should match: A (33333 rows) + B (33333 rows) + C (33333 rows) = 99999 rows
SELECT match_first, match_last, match_len FROM result WHERE match_len > 0;

-- JIT PREV/NEXT navigation test: 100K rows with PREV in DEFINE.
-- Exercises EEOP_RPR_NAV_SET/RESTORE JIT code paths (has_rpr_nav reload)
-- at scale.  A single V: price falls to zero at the midpoint, then rises.
SET jit = on;
SET jit_above_cost = 0;
WITH data AS (
 SELECT i, abs(50000 - i) AS price
 FROM generate_series(1, 100000) i
),
result AS (
 SELECT i, price,
        count(*) OVER w AS match_len,
        first_value(price) OVER w AS match_first
 FROM data
 WINDOW w AS (
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  AFTER MATCH SKIP PAST LAST ROW
  INITIAL
  PATTERN (DOWN+ UP+)
  DEFINE
   DOWN AS price < PREV(price),
   UP AS price > PREV(price)
 )
)
SELECT count(*) AS matched_rows, max(match_len) AS longest_match
FROM result WHERE match_len > 0;
RESET jit_above_cost;
RESET jit;

-- JIT compound navigation test
SET jit = on;
SET jit_above_cost = 0;
SELECT count(*) AS matched_rows
FROM (
 SELECT v, count(*) OVER w AS match_len
 FROM generate_series(1, 1000) AS t(v)
 WINDOW w AS (
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  AFTER MATCH SKIP PAST LAST ROW
  PATTERN (A B+)
  DEFINE A AS TRUE, B AS PREV(FIRST(v), 1) > 0
 )
) sub WHERE match_len > 0;
RESET jit_above_cost;
RESET jit;

--
-- IGNORE NULLS
--

-- no NULL rows case. The result should be identical with "basic test using PREV"
SELECT company, tdate, price, first_value(price) IGNORE NULLS OVER w,
 last_value(price) IGNORE NULLS OVER w,
 nth_value(tdate, 2) IGNORE NULLS OVER w AS nth_second
 FROM stock
 WINDOW w AS (
 PARTITION BY company
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (START UP+ DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- nth_value with IGNORE NULLS option wants to find the second row but
-- due to a NULL in the middle, it returns the third row.
WITH data AS (
 SELECT * FROM (VALUES
  (10, 1), (11, NULL), (12, 3), (13, 4)
  ) AS t(gid, id))
  SELECT gid, id, nth_value(id, 2) IGNORE NULLS OVER w AS second_val,
  array_agg(id) OVER w
  FROM data
  WINDOW w AS (
   ORDER BY gid
   ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
   AFTER MATCH SKIP PAST LAST ROW
   PATTERN (A+)
   DEFINE A AS gid < 13
  );

-- nth_value with IGNORE NULLS option wants to find the third row but
-- due to a NULL in the middle, it reaches the end of reduced frame and
-- returns NULL
WITH data AS (
 SELECT * FROM (VALUES
  (10, 1), (11, NULL), (12, 3), (13, 4)
  ) AS t(gid, id))
  SELECT gid, id, nth_value(id, 3) IGNORE NULLS OVER w AS thrid_val,
  array_agg(id) OVER w
  FROM data
  WINDOW w AS (
   ORDER BY gid
   ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
   AFTER MATCH SKIP PAST LAST ROW
   PATTERN (A+)
   DEFINE A AS gid < 13
  );

-- nth_value beyond reduced frame with IGNORE NULLS
SELECT company, tdate, price,
 nth_value(price, 5) IGNORE NULLS OVER w AS nth_5_in
FROM stock
WINDOW w AS (
 PARTITION BY company
 ORDER BY tdate
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (START UP+ DOWN+)
 DEFINE
  START AS TRUE,
  UP AS price > PREV(price),
  DOWN AS price < PREV(price)
);

-- IGNORE NULLS + first_value where first value in reduced frame is NULL
WITH data AS (
 SELECT * FROM (VALUES
  (1, NULL), (2, NULL), (3, 30), (4, 40)
 ) AS t(id, val))
SELECT id, val,
 first_value(val) IGNORE NULLS OVER w AS fv_ignull,
 count(*) OVER w
FROM data
WINDOW w AS (
 ORDER BY id
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (A+)
 DEFINE A AS TRUE
);

-- IGNORE NULLS + all values NULL in reduced frame
WITH data AS (
 SELECT * FROM (VALUES
  (1, NULL), (2, NULL), (3, NULL)
 ) AS t(id, val))
SELECT id, val,
 first_value(val) IGNORE NULLS OVER w AS fv_ignull,
 last_value(val) IGNORE NULLS OVER w AS lv_ignull,
 count(*) OVER w
FROM data
WINDOW w AS (
 ORDER BY id
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (A+)
 DEFINE A AS TRUE
);

--
-- last_value IGNORE NULLS when the reduced frame ends with NULLs
-- The search for a non-NULL value runs past the end of the reduced frame.
--
CREATE TEMP TABLE rpr_nullval (id INT, val INT);
INSERT INTO rpr_nullval VALUES (1, 10), (2, NULL), (3, NULL), (4, 20);

SELECT id, val,
       last_value(val) IGNORE NULLS OVER w AS lv_ignull,
       count(*) OVER w AS cnt
FROM rpr_nullval
WINDOW w AS (
  ORDER BY id
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  AFTER MATCH SKIP PAST LAST ROW
  PATTERN (A B+)
  DEFINE
    A AS val IS NOT NULL,
    B AS val IS NULL
);

--
-- nth_value with a NULL offset
--

CREATE TABLE rpr_dormant (id int, price int);
INSERT INTO rpr_dormant SELECT g, g*10 FROM generate_series(1,60) g;

-- reference: first_value(id) is the start row of the match beginning at the
-- current row, count(*) is that match's length over the reduced frame
SELECT * FROM (
  SELECT id, first_value(id) OVER w AS match_start, count(*) OVER w AS match_len
  FROM rpr_dormant
  WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+) DEFINE A AS price > PREV(FIRST(price), 50))
) s WHERE id > 50 ORDER BY id;

-- nth_value with a NULL offset; FIRST navigation in DEFINE, SKIP PAST LAST ROW
SELECT * FROM (
  SELECT id, nv FROM (
    SELECT id, nth_value(price, CASE WHEN id < 50 THEN NULL ELSE 1 END) OVER w AS nv
    FROM rpr_dormant
    WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      AFTER MATCH SKIP PAST LAST ROW
      PATTERN (A+) DEFINE A AS price > PREV(FIRST(price), 50))
  ) s
) t WHERE id > 50 ORDER BY id;

-- the same window with first_value and count alongside nth_value
SELECT * FROM (
  SELECT id, nv, fv, cnt FROM (
    SELECT id, nth_value(price, CASE WHEN id < 50 THEN NULL ELSE 1 END) OVER w AS nv,
               first_value(id) OVER w AS fv, count(*) OVER w AS cnt
    FROM rpr_dormant
    WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      AFTER MATCH SKIP PAST LAST ROW
      PATTERN (A+) DEFINE A AS price > PREV(FIRST(price), 50))
  ) s
) t WHERE id > 50 ORDER BY id;

-- the same nth_value with a non-navigation DEFINE
SELECT * FROM (
  SELECT id, nv FROM (
    SELECT id, nth_value(price, CASE WHEN id < 50 THEN NULL ELSE 1 END) OVER w AS nv
    FROM rpr_dormant
    WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      AFTER MATCH SKIP PAST LAST ROW
      PATTERN (A+) DEFINE A AS price > 0)
  ) s
) t WHERE id > 50 ORDER BY id;

-- the same nth_value with a PREV-only DEFINE (no FIRST navigation)
SELECT * FROM (
  SELECT id, nv FROM (
    SELECT id, nth_value(price, CASE WHEN id < 50 THEN NULL ELSE 1 END) OVER w AS nv
    FROM rpr_dormant
    WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      AFTER MATCH SKIP PAST LAST ROW
      PATTERN (A+) DEFINE A AS price > PREV(price, 50))
  ) s
) t WHERE id > 50 ORDER BY id;

-- nth_value with a NULL offset band in the middle of the partition
SELECT * FROM (
  SELECT id, nv FROM (
    SELECT id, nth_value(price, CASE WHEN id BETWEEN 20 AND 40 THEN NULL ELSE 1 END) OVER w AS nv
    FROM rpr_dormant
    WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      AFTER MATCH SKIP PAST LAST ROW
      PATTERN (A+) DEFINE A AS price > PREV(FIRST(price), 50))
  ) s
) t WHERE id BETWEEN 38 AND 46 ORDER BY id;

DROP TABLE rpr_dormant;

--
-- NULL handling
--

CREATE TEMP TABLE stock_null (company TEXT, tdate DATE, price INTEGER);
INSERT INTO stock_null VALUES ('c1', '2023-07-01', 100);
INSERT INTO stock_null VALUES ('c1', '2023-07-02', NULL);  -- NULL in middle
INSERT INTO stock_null VALUES ('c1', '2023-07-03', 200);
INSERT INTO stock_null VALUES ('c1', '2023-07-04', 150);

SELECT company, tdate, price, count(*) OVER w AS match_count
FROM stock_null
WINDOW w AS (
  PARTITION BY company
  ORDER BY tdate
  ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
  PATTERN (START UP DOWN)
  DEFINE START AS TRUE, UP AS price > PREV(price), DOWN AS price <
PREV(price)
);

-- Consecutive NULLs: PREV navigates through NULL values
CREATE TEMP TABLE rpr_consec_null (id INT, val INT);
INSERT INTO rpr_consec_null VALUES
 (1, 100), (2, NULL), (3, NULL), (4, NULL), (5, 200), (6, 300);

-- PREV(val) IS NULL is true for a genuine NULL in the previous row
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_consec_null
WINDOW w AS (
 ORDER BY id
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (A B+ C)
 DEFINE
  A AS val IS NULL,
  B AS val IS NULL AND PREV(val) IS NULL,
  C AS val IS NOT NULL
);

-- NEXT(val) through consecutive NULLs
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_consec_null
WINDOW w AS (
 ORDER BY id
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 AFTER MATCH SKIP PAST LAST ROW
 PATTERN (A B+ C)
 DEFINE
  A AS val IS NOT NULL,
  B AS val IS NULL AND NEXT(val) IS NULL,
  C AS val IS NULL AND NEXT(val) IS NOT NULL
);

DROP TABLE rpr_consec_null;

-- ============================================================
-- Stock Scenario Tests (1632 rows, partitioned regions)
-- ============================================================

-- Consecutive rising days: find streaks of 7+ days
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (UP{7,})
        DEFINE UP AS price > PREV(price)
    )
) t WHERE days > 0 ORDER BY start_rn;

-- V-shape recovery: 4+ days decline followed by 4+ days rise
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(price) OVER w AS start_price,
           last_value(price) OVER w AS end_price,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (DECLINE{4,} RISE{4,})
        DEFINE
            DECLINE AS price < PREV(price),
            RISE AS price > PREV(price)
    )
) t WHERE days > 0 ORDER BY start_rn;

-- W-bottom: decline, bounce, re-decline, recovery
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(price) OVER w AS start_price,
           last_value(price) OVER w AS end_price,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (DECLINE{3,} BOUNCE{3,} DIP{3,} RECOVER{3,})
        DEFINE
            DECLINE AS price < PREV(price),
            BOUNCE AS price > PREV(price),
            DIP AS price < PREV(price),
            RECOVER AS price > PREV(price)
    )
) t WHERE days > 0 ORDER BY start_rn;

-- Volume surge streak: 6+ consecutive days of increasing volume
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(volume) OVER w AS start_vol,
           last_value(volume) OVER w AS end_vol,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (INIT SURGE{5,})
        DEFINE
            SURGE AS volume > PREV(volume)
    )
) t WHERE days > 0 ORDER BY start_rn;

-- Volatility squeeze: consecutive narrowing of daily price range
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(high - low) OVER w AS start_range,
           last_value(high - low) OVER w AS end_range,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (INIT NARROW{5,})
        DEFINE
            NARROW AS (high - low) < PREV(high) - PREV(low)
    )
) t WHERE days > 0 ORDER BY start_rn;

-- Gap up: open significantly higher than previous close (5%+)
SELECT * FROM (
    SELECT first_value(rn) OVER w AS gap_rn,
           first_value(price) OVER w AS prev_close,
           last_value(open) OVER w AS gap_open,
           count(*) OVER w AS cnt
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (PREV_DAY GAP_UP)
        DEFINE
            GAP_UP AS open > PREV(price) * 1.05
    )
) t WHERE cnt > 0 ORDER BY gap_rn;

-- Price-volume divergence: price rising while volume declining (bearish signal)
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(price) OVER w AS start_price,
           last_value(price) OVER w AS end_price,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (INIT DIVERGE{3,})
        DEFINE
            DIVERGE AS price > PREV(price) AND volume < PREV(volume)
    )
) t WHERE days > 0 ORDER BY start_rn;

-- Consolidation then breakout: sideways movement followed by sharp rise
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(price) OVER w AS start_price,
           last_value(price) OVER w AS end_price,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (FLAT{5,} BREAKOUT)
        DEFINE
            FLAT AS price BETWEEN PREV(price) * 0.98 AND PREV(price) * 1.02,
            BREAKOUT AS price > PREV(price) * 1.05
    )
) t WHERE days > 0 ORDER BY start_rn;

-- Dead cat bounce: decline followed by weak recovery (<1% per day)
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(price) OVER w AS start_price,
           last_value(price) OVER w AS end_price,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (DECLINE{4,} BOUNCE{3,})
        DEFINE
            DECLINE AS price < PREV(price),
            BOUNCE AS price > PREV(price) AND price < PREV(price) * 1.01
    )
) t WHERE days > 0 ORDER BY start_rn;

-- Uptrend: 7+ consecutive days of higher highs AND higher lows
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(price) OVER w AS start_price,
           last_value(price) OVER w AS end_price,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (UPTREND{7,})
        DEFINE
            UPTREND AS high > PREV(high) AND low > PREV(low)
    )
) t WHERE days > 0 ORDER BY start_rn;

-- Panic and snap-back: 3%+ daily drops followed by 2%+ rebound
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(price) OVER w AS start_price,
           last_value(price) OVER w AS end_price,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (PANIC{2,} SNAP)
        DEFINE
            PANIC AS price < PREV(price) * 0.97,
            SNAP AS price > PREV(price) * 1.02
    )
) t WHERE days > 0 ORDER BY start_rn;

-- Volume climax reversal: uptrend, volume spike (1.5x), then decline
SELECT * FROM (
    SELECT first_value(rn) OVER w AS start_rn,
           last_value(rn) OVER w AS end_rn,
           first_value(price) OVER w AS start_price,
           last_value(price) OVER w AS end_price,
           count(*) OVER w AS days
    FROM rpr_stock
    WINDOW w AS (
        PARTITION BY part_id
        ORDER BY rn
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP PAST LAST ROW
        PATTERN (RALLY{3,} CLIMAX SELLOFF{2,})
        DEFINE
            RALLY AS price > PREV(price),
            CLIMAX AS volume > PREV(volume) * 1.5,
            SELLOFF AS price < PREV(price)
    )
) t WHERE days > 0 ORDER BY start_rn;
