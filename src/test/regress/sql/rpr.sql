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

CREATE TEMP TABLE rpr_price (company TEXT, tdate DATE, price INTEGER);
INSERT INTO rpr_price VALUES
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

SELECT * FROM rpr_price;

--
-- Basic pattern matching with PREV/NEXT
--

-- basic test using PREV
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w,
 nth_value(tdate, 2) OVER w AS nth_second
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
CREATE TEMP TABLE rpr_ab_pairs (id int, val text);
INSERT INTO rpr_ab_pairs VALUES
  (1,'A'),(2,'B'),
  (3,'A'),(4,'B'),
  (5,'A'),(6,'B'),
  (7,'A'),(8,'B'),
  (9,'X');
SELECT id, val, count(*) OVER w AS match_count
FROM rpr_ab_pairs
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
DROP TABLE rpr_ab_pairs;

-- last_value() should remain consistent
SELECT company, tdate, price, last_value(price) OVER w
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
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
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > PREV(PREV(price))
);

-- Nested NEXT
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > NEXT(NEXT(price))
);

-- PREV nested inside NEXT
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > NEXT(PREV(price))
);

-- PREV nested inside expression inside NEXT
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > NEXT(price * PREV(price))
);

-- Triple nesting: error reported at outermost PREV
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS price > PREV(PREV(PREV(price)))
);

-- No column reference in PREV/NEXT argument
-- PREV(1): constant only, no column reference
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(1) > 0
);

-- NEXT(1 + 2): constant expression, no column reference
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS NEXT(1 + 2) > 0
);

-- 2-arg form: PREV(1, 1): constant expression as first arg
SELECT price FROM rpr_price
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
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(FIRST(1)) > 0
);

-- NEXT(LAST(1 + 2)): compound, constant expression, no column reference
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS NEXT(LAST(1 + 2)) > 0
);

-- PREV(FIRST(1, 2)): compound, two-arg inner, no column reference
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(FIRST(1, 2)) > 0
);

-- PREV(FIRST(1), 2): compound, outer offset only, no column reference
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(FIRST(1), 2) > 0
);

-- PREV(FIRST(1, 2), 3): compound, inner and outer offsets, no column reference
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(FIRST(1, 2), 3) > 0
);

-- Non-constant offset: column reference as offset
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(price, price) > 0
);

-- Non-constant offset: column reference in compound inner offset
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(LAST(price, price), 2) > 0
);

-- Non-constant offset: column reference in compound outer offset
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(LAST(price, 1), price) > 0
);

-- Non-constant offset: volatile function as offset
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(price, random()::int) > 0
);

-- Non-constant offset: volatile function as compound outer offset
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(LAST(price, 1), random()::int) > 0
);

-- Non-constant offset: subquery as offset
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS PREV(price, (SELECT 1)) > 0
);

-- First arg: subquery (caught by DEFINE-level subquery restriction)
SELECT price FROM rpr_price
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
FROM rpr_price
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price + random() * 0) >= 0
);

-- nextval is volatile, so a DEFINE that calls it is rejected
CREATE SEQUENCE rpr_seq;
SELECT price FROM rpr_price
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
FROM rpr_price
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
    SELECT price FROM rpr_price
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
    SELECT price FROM rpr_price
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
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company ORDER BY tdate
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS PREV(o.threshold, 1) > 0
    )
) s;

-- An outer range variable is subject to the same two rules as a local one: a
-- whole-row reference is rejected as one, and a name that does not resolve
-- keeps its own diagnosis rather than being reported as a qualifier problem.
SELECT * FROM (VALUES (95)) AS o(threshold),
LATERAL (
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS (o.*) IS NOT NULL
    )
) s;
SELECT * FROM (VALUES (95)) AS o(threshold),
LATERAL (
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS o.threshhold > 0
    )
) s;

-- A two-part name is not always a range variable qualifier: a SQL function's
-- parameter and a PL/pgSQL variable both resolve through
-- p_post_columnref_hook.  The qualifier slot is reserved all the same, so
-- these are rejected for the spelling, not for what they name.
CREATE FUNCTION rpr_sqlfn(threshold int) RETURNS SETOF int
LANGUAGE sql AS $$
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS price > rpr_sqlfn.threshold)
$$;

CREATE FUNCTION rpr_plfn(threshold int) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    n bigint;
BEGIN
    SELECT count(*) INTO n FROM (
        SELECT price FROM rpr_price
        WINDOW w AS (
            PARTITION BY company
            ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
            INITIAL
            PATTERN (A)
            DEFINE A AS price > rpr_plfn.threshold)
    ) s;
    RETURN n;
END
$$;
SELECT rpr_plfn(0);
DROP FUNCTION rpr_plfn(int);

-- Unqualified, the same parameter is readable.
CREATE FUNCTION rpr_sqlfn(threshold int) RETURNS SETOF int
LANGUAGE sql AS $$
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS price > threshold)
$$;
SELECT count(*) FROM rpr_sqlfn(0);
DROP FUNCTION rpr_sqlfn(int);

-- The qualifier slot is decided on the qualifier alone, before resolution, so
-- a pattern variable takes the slot even from the routine that contains the
-- query.  Naming a pattern variable after the function makes rpr_pv.threshold
-- the pattern variable's, and the reservation is reported; that the function
-- has a parameter of that name, and the query has no such column, does not
-- enter into it.
CREATE FUNCTION rpr_pv(threshold int) RETURNS SETOF int
LANGUAGE sql AS $$
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (rpr_pv)
        DEFINE rpr_pv AS price > rpr_pv.threshold)
$$;

-- The collision is in the qualifier, not in the DEFINE variable being
-- defined: any pattern variable of that name reserves it.
CREATE FUNCTION rpr_pv(threshold int) RETURNS SETOF int
LANGUAGE sql AS $$
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (rpr_pv A)
        DEFINE A AS price > rpr_pv.threshold)
$$;

-- A field of a composite parameter has no unqualified spelling, so it is
-- reached by parenthesizing the value: "(p).lo" selects a field rather than
-- qualifying a name, and occupies no qualifier slot.
CREATE TYPE rpr_pair AS (lo int, hi int);
CREATE FUNCTION rpr_compfn(p rpr_pair) RETURNS SETOF int
LANGUAGE sql AS $$
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS price > p.lo)
$$;
CREATE FUNCTION rpr_compfn(p rpr_pair) RETURNS SETOF int
LANGUAGE sql AS $$
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS price > (p).lo)
$$;
SELECT count(*) FROM rpr_compfn(ROW(0, 0)::rpr_pair);
DROP FUNCTION rpr_compfn(rpr_pair);
DROP TYPE rpr_pair;

-- The DEFINE rules apply to the names the ref hooks leave to the query
-- parser.  Under use_variable resolution PL/pgSQL answers first and keeps
-- any name one of its variables owns, so the qualified spelling rejected
-- above is resolved by PL/pgSQL here and never reaches the rule.
CREATE FUNCTION rpr_plfn_var(threshold int) RETURNS bigint
LANGUAGE plpgsql AS $$
#variable_conflict use_variable
DECLARE
    n bigint;
BEGIN
    SELECT count(*) INTO n FROM (
        SELECT price FROM rpr_price
        WINDOW w AS (
            PARTITION BY company
            ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
            INITIAL
            PATTERN (A)
            DEFINE A AS price > rpr_plfn_var.threshold)
    ) s;
    RETURN n;
END
$$;
SELECT rpr_plfn_var(0);
DROP FUNCTION rpr_plfn_var(int);

-- The same applies to a pattern variable's name.  Under the default
-- resolution PL/pgSQL declines the name, so the reservation is reached and
-- the collision is reported rather than resolved.
CREATE FUNCTION rpr_conflictfn_err() RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    a rpr_price%ROWTYPE;
    n bigint;
BEGIN
    a.price := 95;
    SELECT count(*) INTO n FROM (
        SELECT price FROM rpr_price
        WINDOW w AS (
            PARTITION BY company
            ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
            INITIAL
            PATTERN (A)
            DEFINE A AS price > a.price)
    ) s;
    RETURN n;
END
$$;
SELECT rpr_conflictfn_err();
DROP FUNCTION rpr_conflictfn_err();

CREATE FUNCTION rpr_conflictfn() RETURNS bigint
LANGUAGE plpgsql AS $$
#variable_conflict use_variable
DECLARE
    a rpr_price%ROWTYPE;
    n bigint;
BEGIN
    a.price := 95;
    SELECT count(*) INTO n FROM (
        SELECT price FROM rpr_price
        WINDOW w AS (
            PARTITION BY company
            ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
            INITIAL
            PATTERN (A)
            DEFINE A AS price > a.price)
    ) s;
    RETURN n;
END
$$;
SELECT rpr_conflictfn();
DROP FUNCTION rpr_conflictfn();

-- A star on a name a ref hook owns is not a reference to a FROM-clause
-- relation, so it expands in a DEFINE condition as it does anywhere else.
-- Withholding the expansion would not reject such a name; it would read
-- "rec.*" as the single whole value "rec", which is a different condition,
-- and one that differs silently wherever a row constructor is not counting
-- its entries.  Each function below matches every row when the record holds
-- (1,2) and none when it does not, so a reading that is off returns the
-- wrong count rather than an error.
CREATE TYPE rpr_pair AS (a int, b int);
CREATE TEMP TABLE rpr_rec (id int);
INSERT INTO rpr_rec VALUES (1), (2), (3);

-- under the default resolution the post hook answers the name:
CREATE FUNCTION rpr_recstar(x int, y int) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    rec rpr_pair;
    n bigint;
BEGIN
    rec := ROW(x, y);
    SELECT count(*) OVER w INTO n FROM rpr_rec
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS ROW(rec.*)::text = '(1,2)')
    LIMIT 1;
    RETURN n;
END
$$;
SELECT rpr_recstar(1, 2);
SELECT rpr_recstar(1, 3);
DROP FUNCTION rpr_recstar(int, int);

-- under use_variable the pre hook answers it instead:
CREATE FUNCTION rpr_recstar_var(x int, y int) RETURNS bigint
LANGUAGE plpgsql AS $$
#variable_conflict use_variable
DECLARE
    rec rpr_pair;
    n bigint;
BEGIN
    rec := ROW(x, y);
    SELECT count(*) OVER w INTO n FROM rpr_rec
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS ROW(rec.*)::text = '(1,2)')
    LIMIT 1;
    RETURN n;
END
$$;
SELECT rpr_recstar_var(1, 2);
SELECT rpr_recstar_var(1, 3);
DROP FUNCTION rpr_recstar_var(int, int);

-- the same spelling outside a DEFINE condition, which is what the two above
-- have to agree with:
CREATE FUNCTION rpr_recstar_plain(x int, y int) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    rec rpr_pair;
BEGIN
    rec := ROW(x, y);
    RETURN (SELECT ROW(rec.*)::text FROM rpr_rec LIMIT 1);
END
$$;
SELECT rpr_recstar_plain(1, 2);
DROP FUNCTION rpr_recstar_plain(int, int);

-- A FROM-clause relation is not such a name, inside such a function or out.
CREATE FUNCTION rpr_relstar() RETURNS bigint
LANGUAGE plpgsql AS $$
#variable_conflict use_variable
DECLARE
    rec rpr_pair := ROW(1, 2);
    n bigint;
BEGIN
    SELECT count(*) OVER w INTO n FROM rpr_rec
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS ROW(rpr_rec.*)::text IS NOT NULL)
    LIMIT 1;
    RETURN n;
END
$$;
SELECT rpr_relstar();
DROP FUNCTION rpr_relstar();
DROP TABLE rpr_rec;
DROP TYPE rpr_pair;

-- An outer range variable used as a function-call qualifier reaches DEFINE as
-- a FuncExpr rather than a Var, so the level the qualifier resolved at, not
-- the shape of the resulting node, is what identifies the outer reference.
CREATE TABLE rpr_outer (threshold int);
INSERT INTO rpr_outer VALUES (95);
CREATE FUNCTION rpr_rowfn(rpr_outer) RETURNS int LANGUAGE sql AS 'SELECT 1';
SELECT * FROM rpr_outer AS o,
LATERAL (
    SELECT price FROM rpr_price
    WINDOW w AS (
        PARTITION BY company
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        INITIAL
        PATTERN (A)
        DEFINE A AS o.rpr_rowfn > 0
    )
) s;
DROP FUNCTION rpr_rowfn(rpr_outer);
DROP TABLE rpr_outer;

-- DEFINE rejects a schema-qualified column reference (three or more name
-- parts) once it resolves; the qualified form itself is not allowed.
-- (rpr_price is a temp table, so it is qualified with pg_temp here.)
-- 3-part (schema.table.column):
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS pg_temp.rpr_price.price > 0
);
-- whole-row variant (schema.table.*):
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS (pg_temp.rpr_price.*) IS NOT NULL
);
-- A two-part table-qualified whole-row reference is rejected as well, and by
-- the whole-row check rather than by a qualifier rule: the error names the
-- whole-row reference, not the qualifier.
-- 2-part (table.*):
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS (rpr_price.*) IS NOT NULL
);
-- The form decides before the qualifier is looked up, so a misspelled table
-- name is reported as the whole-row reference it is written as, not as a
-- missing FROM-clause entry:
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS (stok.*) IS NOT NULL
);
-- and the same through a row constructor:
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS ROW(stok.*) IS NOT NULL
);

-- A row constructor reaches the same references through
-- transformExpressionList(), whose star expansion binds them by RTE into
-- individual column Vars, past every check.  DEFINE skips it.
-- ROW(schema.table.*):
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS ROW(pg_temp.rpr_price.*) IS NOT NULL
);
-- ROW(table.*):
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS ROW(rpr_price.*) IS NOT NULL
);
-- the ROW keyword is optional, so the bare constructor needs the same
-- treatment:
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS (rpr_price.*, 1) IS NOT NULL
);
-- redundant parentheses are not a way around it:
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS ROW((rpr_price.*)) IS NOT NULL
);
-- a pattern variable qualifier is a separate class of rejection:
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS ROW(A.*) IS NOT NULL
);
-- The plain two-part form is the one the standard writes its DEFINE examples
-- with, and it is decided on the qualifier alone, before resolution.
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS A.price > 100
);
-- Deciding on the qualifier alone means a pattern variable takes a name a
-- range variable would otherwise answer to: the rejection names the pattern
-- variable, not the alias, even though "a" is a live alias here.
SELECT price FROM rpr_price AS a
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS a.price > 100
);
-- Unlike the pattern variable and whole-row rejections, the range variable
-- and schema-qualified rejections classify the reference only after it
-- resolves, so a misspelled column keeps the diagnosis and the suggestion it
-- gets anywhere else.  Firing on the qualifier alone would report a range
-- variable problem before the rest of the name was looked at.
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS rpr_price.pric > 0
);
SELECT price FROM rpr_price
WINDOW w AS (
    PARTITION BY company
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS pg_temp.rpr_price.pric > 0
);
-- the same typo outside a DEFINE clause, for comparison:
SELECT price FROM rpr_price WHERE rpr_price.pric > 0;

-- Retrying an unresolved column as a function call on the whole row builds a
-- whole-row reference the query does not contain.  That must not be reported
-- as one, and must not let the reference through either: rpr_tag(rpr_stock)
-- below resolves, so the retry succeeds and the result is rejected by the
-- qualifier rules rather than by the whole-row check.
CREATE FUNCTION rpr_tag(rpr_stock) RETURNS int
    LANGUAGE sql IMMUTABLE AS $$SELECT 1$$;
SELECT price FROM rpr_stock
WINDOW w AS (
    PARTITION BY part_id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS rpr_stock.rpr_tag > 0
);
SELECT price FROM rpr_stock
WINDOW w AS (
    PARTITION BY part_id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A)
    DEFINE A AS public.rpr_stock.rpr_tag > 0
);
DROP FUNCTION rpr_tag(rpr_stock);

-- A JOIN USING alias has no whole-row Var of its own, so the same retry
-- expands it to a row constructor instead.  The retry carries no star, so
-- DEFINE lets it through to that arm.
CREATE TEMP TABLE rpr_j_l (x int, y int);
CREATE TEMP TABLE rpr_j_r (x int, z int);
SELECT count(*) OVER w FROM (rpr_j_l JOIN rpr_j_r USING (x)) j
WINDOW w AS (
    ORDER BY x
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS j.yy > 0
);
SELECT count(*) OVER w FROM (rpr_j_l JOIN rpr_j_r USING (x)) j
WINDOW w AS (
    ORDER BY x
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS j.y > 0
);
SELECT count(*) OVER w FROM (rpr_j_l JOIN rpr_j_r USING (x)) j
WINDOW w AS (
    ORDER BY x
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS (j.*) IS NOT NULL
);
DROP TABLE rpr_j_l, rpr_j_r;

-- A row constructor over plain columns is unaffected.
SELECT company, tdate, count(*) OVER w AS cnt
FROM rpr_price
WHERE company = 'company2' AND tdate <= '2023-07-03'
WINDOW w AS (
    PARTITION BY company
    ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    INITIAL
    PATTERN (A+)
    DEFINE A AS ROW(price, price) IS NOT NULL
);

-- A restriction on a DEFINE condition covers the whole condition, including a
-- clause nested inside it that carries an expression kind of its own.  FILTER
-- and an aggregate's ORDER BY are the two such clauses a condition can reach,
-- and each case below is preceded by the same reference written directly in
-- the condition, which is the rejection the nested one has to keep.
CREATE TEMP TABLE rpr_nest_i (i int, v int);
CREATE TEMP TABLE rpr_nest_o (v int);
-- an outer query column:
SELECT o.v, (SELECT count(*) OVER w FROM rpr_nest_i inn
             WINDOW w AS (
                 ORDER BY inn.i
                 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                 PATTERN (A+)
                 DEFINE A AS o.v > 0)
             LIMIT 1)
FROM rpr_nest_o o GROUP BY o.v;
SELECT o.v, (SELECT count(*) OVER w FROM rpr_nest_i inn
             WINDOW w AS (
                 ORDER BY inn.i
                 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                 PATTERN (A+)
                 DEFINE A AS count(*) FILTER (WHERE o.v > 0) > 0)
             LIMIT 1)
FROM rpr_nest_o o GROUP BY o.v;
SELECT o.v, (SELECT count(*) OVER w FROM rpr_nest_i inn
             WINDOW w AS (
                 ORDER BY inn.i
                 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                 PATTERN (A+)
                 DEFINE A AS v > count(1 ORDER BY o.v))
             LIMIT 1)
FROM rpr_nest_o o GROUP BY o.v;
-- a pattern variable qualifier, where an outer alias answers to the same
-- name, so letting it through would silently read the outer column instead:
SELECT count(*) OVER w FROM rpr_nest_i A
WINDOW w AS (
    ORDER BY A.i
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS A.v > 0
);
SELECT A.v, (SELECT count(*) OVER w FROM rpr_nest_i inn
             WINDOW w AS (
                 ORDER BY inn.i
                 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                 PATTERN (A+)
                 DEFINE A AS percentile_disc(0.5)
                             WITHIN GROUP (ORDER BY A.v) > 0)
             LIMIT 1)
FROM rpr_nest_i A GROUP BY A.v;
-- a whole-row reference, which a row constructor would expand by RTE:
SELECT (SELECT count(*) OVER w FROM rpr_nest_i inn
        WINDOW w AS (
            ORDER BY inn.i
            ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
            PATTERN (A+)
            DEFINE A AS ROW(o.*)::text IS NOT NULL)
        LIMIT 1)
FROM rpr_nest_o o;
SELECT (SELECT count(*) OVER w FROM rpr_nest_i inn
        WINDOW w AS (
            ORDER BY inn.i
            ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
            PATTERN (A+)
            DEFINE A AS percentile_disc(0.5)
                        WITHIN GROUP (ORDER BY ROW(o.*)::text) IS NOT NULL)
        LIMIT 1)
FROM rpr_nest_o o GROUP BY o.v;
-- a subquery:
SELECT count(*) OVER w FROM rpr_nest_i inn
WINDOW w AS (
    ORDER BY inn.i
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > (SELECT 1)
);
SELECT count(*) OVER w FROM rpr_nest_i inn
WINDOW w AS (
    ORDER BY inn.i
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS count(*) FILTER (WHERE (SELECT 1) = 1) > 0
);
-- The nesting itself is not what is rejected: with nothing forbidden inside
-- it, the aggregate carrying the FILTER is what the condition trips over.
SELECT count(*) OVER w FROM rpr_nest_i inn
WINDOW w AS (
    ORDER BY inn.i
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS count(*) FILTER (WHERE v > 0) > 0
);
DROP TABLE rpr_nest_i, rpr_nest_o;

--
-- 2-arg PREV/NEXT: functional tests
--

-- PREV(price, 2): with A=any, B matches where the price beats the one two rows
-- back.  On company1 (100, 200, 150, 140, 150, 90, 110, 130, 120, 130) that is
-- 200 -> 150, then 110 -> 130 -> 120, which stops where 130 only ties 130.
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS price > PREV(price - 50, 1)
);

-- NEXT(price * 2, 1): fetches (price * 2) from 1 row ahead
SELECT company, tdate, price,
       first_value(price) OVER w, last_value(price) OVER w, count(*) OVER w
FROM rpr_price
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
FROM rpr_price
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price, 0) = price
);

-- 2-arg PREV/NEXT: negative offset
SELECT company, tdate, price, first_value(price) OVER w
FROM rpr_price
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price, -1) IS NOT NULL
);

-- 2-arg PREV/NEXT: NULL offset (typed)
SELECT company, tdate, price, first_value(price) OVER w
FROM rpr_price
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price, NULL::int8) IS NOT NULL
);

-- 2-arg PREV/NEXT: NULL offset (untyped)
SELECT company, tdate, price, first_value(price) OVER w
FROM rpr_price
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(price, NULL) IS NOT NULL
);

-- 2-arg PREV/NEXT: host variable negative and NULL
PREPARE test_prev_offset(int8) AS
SELECT company, tdate, price, first_value(price) OVER w
FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
WINDOW w AS (
    PARTITION BY company ORDER BY tdate
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS PREV(price::numeric, 1) > PREV(price::numeric, 2)
);

-- Bare pass-by-reference column rather than a cast: the two navigations land
-- on different rows, so the second fetch frees the tuple the first result
-- points into.  The casts above allocate a fresh datum and never reach that;
-- only EEOP_RPR_NAV_RESTORE's datumCopy keeps this one alive.
CREATE TEMP TABLE rpr_byref (id int, s text);
INSERT INTO rpr_byref VALUES
  (1, 'aaa'), (2, 'bbb'), (3, 'ccc'), (4, 'bbb'), (5, 'ddd'), (6, 'aaa');
SELECT id, s, first_value(s) OVER w AS fs, last_value(s) OVER w AS ls,
       count(*) OVER w AS cnt
FROM rpr_byref
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(s, 1) > PREV(s, 2)
);
DROP TABLE rpr_byref;

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
CREATE TEMP TABLE rpr_nav_cycle (id int, val int);
INSERT INTO rpr_nav_cycle VALUES (1,10),(2,20),(3,30),(4,10),(5,50),(6,10);

-- FIRST(val) = constant: B matches when match_start has val=10
-- match_start=1(10): A=id1, B=id2, FIRST(val)=10 -> match {1,2}
-- match_start=3(30): A=id3, B=id4, FIRST(val)=30!=10 -> no match
-- match_start=4(10): A=id4, B=id5, FIRST(val)=10 -> match {4,5}
SELECT id, val, first_value(id) OVER w AS mf, last_value(id) OVER w AS ml
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS TRUE, B AS FIRST(val) = 10
);

-- LAST(val): always equals current row's val (offset 0 default)
-- Equivalent to: B AS val > 15
SELECT id, val, first_value(id) OVER w AS mf, last_value(id) OVER w AS ml
FROM rpr_nav_cycle WINDOW w AS (
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
FROM rpr_nav_cycle WINDOW w AS (
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
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS TRUE, B AS FIRST(val) = LAST(val)
);

-- SKIP TO NEXT ROW with FIRST(val) = LAST(val): overlapping match attempts.
-- Each row reports only the match that starts at it.
SELECT id, val, first_value(id) OVER w AS mf, last_value(id) OVER w AS ml
FROM rpr_nav_cycle WINDOW w AS (
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
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS FIRST(val, 0) = 10
);

-- FIRST(val, 1): match_start + 1 row (second row of match)
-- match_start=1(10): FIRST(val,1)=20, B needs val=20
-- -> id2(20) match, id3(30) no
-- match_start=3(30): FIRST(val,1)=10, B needs val=10 -> id4(10) match
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS val = FIRST(val, 1)
);

-- FIRST(val, 99): offset beyond match range -> NULL, no match
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS FIRST(val, 99) IS NOT NULL
);

-- LAST(val, 0) = LAST(val): current row
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
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
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS LAST(val, 1) < 30
);

-- LAST(val, 99): offset before match_start -> NULL
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS LAST(val, 99) IS NOT NULL
);

-- Error: NULL offset
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS FIRST(val, NULL::int8) IS NULL
);

-- Error: negative offset
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
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
-- rpr_nav_cycle: (1,10),(2,20),(3,30),(4,10),(5,50),(6,10)
-- PREV(FIRST(val), 1): target = match_start + 0 - 1 = match_start - 1
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val), 1) > 0
);

-- NEXT(FIRST(val, 1), 1): target = match_start + 1 + 1 = match_start + 2
-- At match_start=1, B on id2: target=1+1+1=3(val=30), 30>0 -> true
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val, 1), 1) > 0
);

-- PREV(LAST(val), 2): LAST(val) is the current row (inner offset 0), so
-- target = currentpos - 0 - 2 = currentpos - 2.
-- Same backward reach as PREV(val, 2).
-- At currentpos=2 (start id=1): target=0 -> out of range -> NULL -> B fails.
-- At currentpos=3 (start id=2): target=1(val=10) -> in range -> B runs on
-- id3..id6, so the match is id2..id6.
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
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
-- B stays true through id5 (target=6); at id6 target=7 -> out of range
-- -> NULL, so the match is id1..id5.
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(LAST(val, 1), 2) IS NOT NULL
);

-- Compound: outer offset beyond partition (PREV far back)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val), 99) IS NOT NULL
);

-- Compound: outer offset beyond partition (NEXT far forward)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val), 99) IS NOT NULL
);

-- Compound: inner offset beyond match range (FIRST offset too large)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val, 99), 1) IS NOT NULL
);

-- Compound: inner offset beyond match range (LAST offset too large)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(LAST(val, 99), 1) IS NOT NULL
);

-- Compound: NULL outer offset (runtime error)
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(val), NULL::int8) IS NULL
);

-- Compound: negative outer offset (runtime error)
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(val), -1) IS NULL
);

-- Compound: an out-of-range inner offset must not skip validation of the outer
-- one.  All four arms resolve their outer offset through the same call, so
-- each appears once, and the negative and the null case take two arms apiece.
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val, 99), -1) IS NULL
);
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(LAST(val, 99), NULL::int8) IS NULL
);
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val, 99), NULL::int8) IS NULL
);
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
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
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
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
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(val, 9223372036854775807) IS NULL
);

-- Compound NEXT(FIRST()): outer offset overflow.  Inner offset 1 forces
-- inner_pos >= 1, so inner_pos + INT64_MAX overflows at every match.
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(FIRST(val, 1), 9223372036854775807) IS NULL
);

-- Compound NEXT(LAST()): outer offset overflow.
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(val), 9223372036854775807) IS NULL
);

-- Inner offset overflows int64.  The cases above all overflow while applying
-- the outer offset; these two overflow while computing the inner position,
-- before any outer offset is applied.  A is false at the first row, so no
-- match starts there and match_start is at least 1 wherever B is evaluated;
-- match_start + INT64_MAX then overflows.  With match_start 0 the sum still
-- fits and the clamp below it answers instead.
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS val > 10, B AS FIRST(val, 9223372036854775807) IS NULL
);

-- The same overflow reached through a compound navigation, where it happens
-- before the outer offset is applied
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS val > 10, B AS NEXT(FIRST(val, 9223372036854775807), 1) IS NULL
);

-- Compound: default offsets on both sides
-- PREV(FIRST(val)): inner=0 (match_start), outer=1 -> target = match_start - 1
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS PREV(FIRST(val)) IS NOT NULL
);

-- NEXT(LAST(val)): inner=0 (currentpos), outer=1 -> target = currentpos + 1
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS NEXT(LAST(val)) IS NOT NULL
);

-- Compound: inner NULL offset (runtime error)
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(val, NULL::int8), 1) IS NULL
);

-- Compound: inner negative offset (runtime error)
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(val, -1), 1) IS NULL
);

-- Offset argument whose type has no implicit cast to bigint (parse error)
SELECT id, val, count(*) OVER w FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS val > PREV(val, 1.5)
);

-- Compound + host variable offsets
PREPARE test_compound_offset(int8, int8) AS
SELECT id, val, first_value(id) OVER w AS mf, count(*) OVER w AS cnt
FROM rpr_nav_cycle WINDOW w AS (
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
FROM rpr_nav_cycle WINDOW w AS (
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
SELECT id, val FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B)
    DEFINE A AS TRUE, B AS FIRST(PREV(val)) > 0
);

-- Reverse nesting: LAST wrapping NEXT is prohibited
SELECT id, val FROM rpr_nav_cycle WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B)
    DEFINE A AS TRUE, B AS LAST(NEXT(val)) > 0
);

DROP TABLE rpr_nav_cycle;

--
-- SKIP TO / Backtracking / Frame boundary
--

-- match everything
SELECT company, tdate, price, first_value(price) OVER w, last_value(price) OVER w
 FROM rpr_price
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
FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
 FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
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
FROM rpr_price
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
CREATE TEMP TABLE rpr_join_left (i int, v1 int);
CREATE TEMP TABLE rpr_join_right (j int, v2 int);
INSERT INTO rpr_join_left VALUES(1,10);
INSERT INTO rpr_join_left VALUES(1,11);
INSERT INTO rpr_join_left VALUES(1,12);
INSERT INTO rpr_join_right VALUES(2,10);
INSERT INTO rpr_join_right VALUES(2,11);
INSERT INTO rpr_join_right VALUES(2,12);

SELECT * FROM rpr_join_left, rpr_join_right WHERE rpr_join_left.v1 <= 11 AND rpr_join_right.v2 <= 11;

SELECT *, count(*) OVER w FROM rpr_join_left, rpr_join_right
WINDOW w AS (
 PARTITION BY rpr_join_left.i
 ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
 INITIAL
 PATTERN (A)
 DEFINE
 A AS v1 <= 11 AND v2 <= 11
);

-- WITH case
WITH wstock AS (
  SELECT * FROM rpr_price WHERE tdate < '2023-07-08'
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
CREATE TEMP TABLE rpr_prev_multicol (id INTEGER, i SERIAL, j INTEGER);
INSERT INTO rpr_prev_multicol(id, j) SELECT 1, g*2 FROM generate_series(1, 10) AS g;
SELECT id, i, j, count(*) OVER w
 FROM rpr_prev_multicol
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

-- no NULL rows case. The result should be identical with
-- "basic test using PREV"
SELECT company, tdate, price, first_value(price) IGNORE NULLS OVER w,
 last_value(price) IGNORE NULLS OVER w,
 nth_value(tdate, 2) IGNORE NULLS OVER w AS nth_second
 FROM rpr_price
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
FROM rpr_price
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
-- The search for a non-NULL value must start at the end of the reduced frame,
-- not the full frame, so the later non-NULL row 4 is not returned.
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

CREATE TEMP TABLE rpr_stock_null (company TEXT, tdate DATE, price INTEGER);
INSERT INTO rpr_stock_null VALUES ('c1', '2023-07-01', 100);
INSERT INTO rpr_stock_null VALUES ('c1', '2023-07-02', NULL);  -- NULL in middle
INSERT INTO rpr_stock_null VALUES ('c1', '2023-07-03', 200);
INSERT INTO rpr_stock_null VALUES ('c1', '2023-07-04', 150);

SELECT company, tdate, price, count(*) OVER w AS match_count
FROM rpr_stock_null
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

-- Price-volume divergence: price rising while volume declining
-- (bearish signal)
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
