--
-- UNION (also INTERSECT, EXCEPT)
--

-- Simple UNION constructs

SELECT 1 AS two UNION SELECT 2 ORDER BY 1;

SELECT 1 AS one UNION SELECT 1 ORDER BY 1;

SELECT 1 AS two UNION ALL SELECT 2;

SELECT 1 AS two UNION ALL SELECT 1;

SELECT 1 AS three UNION SELECT 2 UNION SELECT 3 ORDER BY 1;

SELECT 1 AS two UNION SELECT 2 UNION SELECT 2 ORDER BY 1;

SELECT 1 AS three UNION SELECT 2 UNION ALL SELECT 2 ORDER BY 1;

SELECT 1.1 AS two UNION SELECT 2.2 ORDER BY 1;

-- Mixed types

SELECT 1.1 AS two UNION SELECT 2 ORDER BY 1;

SELECT 1 AS two UNION SELECT 2.2 ORDER BY 1;

SELECT 1 AS one UNION SELECT 1.0::float8 ORDER BY 1;

SELECT 1.1 AS two UNION ALL SELECT 2 ORDER BY 1;

SELECT 1.0::float8 AS two UNION ALL SELECT 1 ORDER BY 1;

SELECT 1.1 AS three UNION SELECT 2 UNION SELECT 3 ORDER BY 1;

SELECT 1.1::float8 AS two UNION SELECT 2 UNION SELECT 2.0::float8 ORDER BY 1;

SELECT 1.1 AS three UNION SELECT 2 UNION ALL SELECT 2 ORDER BY 1;

SELECT 1.1 AS two UNION (SELECT 2 UNION ALL SELECT 2) ORDER BY 1;

--
-- Try testing from tables...
--

SELECT f1 AS five FROM FLOAT8_TBL
UNION
SELECT f1 FROM FLOAT8_TBL
ORDER BY 1;

SELECT f1 AS ten FROM FLOAT8_TBL
UNION ALL
SELECT f1 FROM FLOAT8_TBL;

SELECT f1 AS nine FROM FLOAT8_TBL
UNION
SELECT f1 FROM INT4_TBL
ORDER BY 1;

SELECT f1 AS ten FROM FLOAT8_TBL
UNION ALL
SELECT f1 FROM INT4_TBL;

SELECT f1 AS five FROM FLOAT8_TBL
  WHERE f1 BETWEEN -1e6 AND 1e6
UNION
SELECT f1 FROM INT4_TBL
  WHERE f1 BETWEEN 0 AND 1000000
ORDER BY 1;

SELECT CAST(f1 AS char(4)) AS three FROM VARCHAR_TBL
UNION
SELECT f1 FROM CHAR_TBL
ORDER BY 1;

SELECT f1 AS three FROM VARCHAR_TBL
UNION
SELECT CAST(f1 AS varchar) FROM CHAR_TBL
ORDER BY 1;

SELECT f1 AS eight FROM VARCHAR_TBL
UNION ALL
SELECT f1 FROM CHAR_TBL;

SELECT f1 AS five FROM TEXT_TBL
UNION
SELECT f1 FROM VARCHAR_TBL
UNION
SELECT TRIM(TRAILING FROM f1) FROM CHAR_TBL
ORDER BY 1;

--
-- INTERSECT and EXCEPT
--

SELECT q2 FROM int8_tbl INTERSECT SELECT q1 FROM int8_tbl ORDER BY 1;

SELECT q2 FROM int8_tbl INTERSECT ALL SELECT q1 FROM int8_tbl ORDER BY 1;

SELECT q2 FROM int8_tbl EXCEPT SELECT q1 FROM int8_tbl ORDER BY 1;

SELECT q2 FROM int8_tbl EXCEPT ALL SELECT q1 FROM int8_tbl ORDER BY 1;

SELECT q2 FROM int8_tbl EXCEPT ALL SELECT DISTINCT q1 FROM int8_tbl ORDER BY 1;

SELECT q1 FROM int8_tbl EXCEPT SELECT q2 FROM int8_tbl ORDER BY 1;

SELECT q1 FROM int8_tbl EXCEPT ALL SELECT q2 FROM int8_tbl ORDER BY 1;

SELECT q1 FROM int8_tbl EXCEPT ALL SELECT DISTINCT q2 FROM int8_tbl ORDER BY 1;

SELECT q1 FROM int8_tbl EXCEPT ALL SELECT q1 FROM int8_tbl FOR NO KEY UPDATE;

-- nested cases
(SELECT 1,2,3 UNION SELECT 4,5,6) INTERSECT SELECT 4,5,6;
(SELECT 1,2,3 UNION SELECT 4,5,6 ORDER BY 1,2) INTERSECT SELECT 4,5,6;
(SELECT 1,2,3 UNION SELECT 4,5,6) EXCEPT SELECT 4,5,6;
(SELECT 1,2,3 UNION SELECT 4,5,6 ORDER BY 1,2) EXCEPT SELECT 4,5,6;

-- exercise both hashed and sorted implementations of UNION/INTERSECT/EXCEPT

set enable_hashagg to on;

explain (costs off)
select count(*) from
  ( select unique1 from tenk1 union select fivethous from tenk1 ) ss;
select count(*) from
  ( select unique1 from tenk1 union select fivethous from tenk1 ) ss;

explain (costs off)
select count(*) from
  ( select unique1 from tenk1 intersect select fivethous from tenk1 ) ss;
select count(*) from
  ( select unique1 from tenk1 intersect select fivethous from tenk1 ) ss;

-- this query will prefer a sorted setop unless we force it.
set enable_groupagg to off;

explain (costs off)
select unique1 from tenk1 except select unique2 from tenk1 where unique2 != 10;
select unique1 from tenk1 except select unique2 from tenk1 where unique2 != 10;

reset enable_groupagg;

-- the hashed implementation is sensitive to child plans' tuple slot types
explain (costs off)
select * from int8_tbl intersect select q2, q1 from int8_tbl order by 1, 2;
select * from int8_tbl intersect select q2, q1 from int8_tbl order by 1, 2;
select q2, q1 from int8_tbl intersect select * from int8_tbl order by 1, 2;

set enable_hashagg to off;

explain (costs off)
select count(*) from
  ( select unique1 from tenk1 union select fivethous from tenk1 ) ss;
select count(*) from
  ( select unique1 from tenk1 union select fivethous from tenk1 ) ss;

explain (costs off)
select count(*) from
  ( select unique1 from tenk1 intersect select fivethous from tenk1 ) ss;
select count(*) from
  ( select unique1 from tenk1 intersect select fivethous from tenk1 ) ss;

explain (costs off)
select unique1 from tenk1 except select unique2 from tenk1 where unique2 != 10;
select unique1 from tenk1 except select unique2 from tenk1 where unique2 != 10;

explain (costs off)
select f1 from int4_tbl union all
  (select unique1 from tenk1 union select unique2 from tenk1);

reset enable_hashagg;

-- non-hashable type
set enable_hashagg to on;

explain (costs off)
select x from (values ('11'::varbit), ('10'::varbit)) _(x) union select x from (values ('11'::varbit), ('10'::varbit)) _(x);

set enable_hashagg to off;

explain (costs off)
select x from (values ('11'::varbit), ('10'::varbit)) _(x) union select x from (values ('11'::varbit), ('10'::varbit)) _(x);

reset enable_hashagg;

-- arrays
set enable_hashagg to on;

explain (costs off)
select x from (values (array[1, 2]), (array[1, 3])) _(x) union select x from (values (array[1, 2]), (array[1, 4])) _(x);
select x from (values (array[1, 2]), (array[1, 3])) _(x) union select x from (values (array[1, 2]), (array[1, 4])) _(x);
explain (costs off)
select x from (values (array[1, 2]), (array[1, 3])) _(x) intersect select x from (values (array[1, 2]), (array[1, 4])) _(x);
select x from (values (array[1, 2]), (array[1, 3])) _(x) intersect select x from (values (array[1, 2]), (array[1, 4])) _(x);
explain (costs off)
select x from (values (array[1, 2]), (array[1, 3])) _(x) except select x from (values (array[1, 2]), (array[1, 4])) _(x);
select x from (values (array[1, 2]), (array[1, 3])) _(x) except select x from (values (array[1, 2]), (array[1, 4])) _(x);

-- non-hashable type
explain (costs off)
select x from (values (array['10'::varbit]), (array['11'::varbit])) _(x) union select x from (values (array['10'::varbit]), (array['01'::varbit])) _(x);
select x from (values (array['10'::varbit]), (array['11'::varbit])) _(x) union select x from (values (array['10'::varbit]), (array['01'::varbit])) _(x);

set enable_hashagg to off;

explain (costs off)
select x from (values (array[1, 2]), (array[1, 3])) _(x) union select x from (values (array[1, 2]), (array[1, 4])) _(x);
select x from (values (array[1, 2]), (array[1, 3])) _(x) union select x from (values (array[1, 2]), (array[1, 4])) _(x);
explain (costs off)
select x from (values (array[1, 2]), (array[1, 3])) _(x) intersect select x from (values (array[1, 2]), (array[1, 4])) _(x);
select x from (values (array[1, 2]), (array[1, 3])) _(x) intersect select x from (values (array[1, 2]), (array[1, 4])) _(x);
explain (costs off)
select x from (values (array[1, 2]), (array[1, 3])) _(x) except select x from (values (array[1, 2]), (array[1, 4])) _(x);
select x from (values (array[1, 2]), (array[1, 3])) _(x) except select x from (values (array[1, 2]), (array[1, 4])) _(x);

reset enable_hashagg;

-- records
set enable_hashagg to on;

explain (costs off)
select x from (values (row(1, 2)), (row(1, 3))) _(x) union select x from (values (row(1, 2)), (row(1, 4))) _(x);
select x from (values (row(1, 2)), (row(1, 3))) _(x) union select x from (values (row(1, 2)), (row(1, 4))) _(x);
explain (costs off)
select x from (values (row(1, 2)), (row(1, 3))) _(x) intersect select x from (values (row(1, 2)), (row(1, 4))) _(x);
select x from (values (row(1, 2)), (row(1, 3))) _(x) intersect select x from (values (row(1, 2)), (row(1, 4))) _(x);
explain (costs off)
select x from (values (row(1, 2)), (row(1, 3))) _(x) except select x from (values (row(1, 2)), (row(1, 4))) _(x);
select x from (values (row(1, 2)), (row(1, 3))) _(x) except select x from (values (row(1, 2)), (row(1, 4))) _(x);

-- non-hashable type

-- With an anonymous row type, the typcache does not report that the
-- type is hashable.  (Otherwise, this would fail at execution time.)
explain (costs off)
select x from (values (row('10'::varbit)), (row('11'::varbit))) _(x) union select x from (values (row('10'::varbit)), (row('01'::varbit))) _(x);
select x from (values (row('10'::varbit)), (row('11'::varbit))) _(x) union select x from (values (row('10'::varbit)), (row('01'::varbit))) _(x);

-- With a defined row type, the typcache can inspect the type's fields
-- for hashability.
create type ct1 as (f1 varbit);
explain (costs off)
select x from (values (row('10'::varbit)::ct1), (row('11'::varbit)::ct1)) _(x) union select x from (values (row('10'::varbit)::ct1), (row('01'::varbit)::ct1)) _(x);
select x from (values (row('10'::varbit)::ct1), (row('11'::varbit)::ct1)) _(x) union select x from (values (row('10'::varbit)::ct1), (row('01'::varbit)::ct1)) _(x);
drop type ct1;

set enable_hashagg to off;

explain (costs off)
select x from (values (row(1, 2)), (row(1, 3))) _(x) union select x from (values (row(1, 2)), (row(1, 4))) _(x);
select x from (values (row(1, 2)), (row(1, 3))) _(x) union select x from (values (row(1, 2)), (row(1, 4))) _(x);
explain (costs off)
select x from (values (row(1, 2)), (row(1, 3))) _(x) intersect select x from (values (row(1, 2)), (row(1, 4))) _(x);
select x from (values (row(1, 2)), (row(1, 3))) _(x) intersect select x from (values (row(1, 2)), (row(1, 4))) _(x);
explain (costs off)
select x from (values (row(1, 2)), (row(1, 3))) _(x) except select x from (values (row(1, 2)), (row(1, 4))) _(x);
select x from (values (row(1, 2)), (row(1, 3))) _(x) except select x from (values (row(1, 2)), (row(1, 4))) _(x);

-- non-sortable type

-- Ensure we get a HashAggregate plan.  Keep enable_hashagg=off to ensure
-- there's no chance of a sort.
explain (costs off) select '123'::xid union select '123'::xid;

reset enable_hashagg;

--
-- Mixed types
--

SELECT f1 FROM float8_tbl INTERSECT SELECT f1 FROM int4_tbl ORDER BY 1;

SELECT f1 FROM float8_tbl EXCEPT SELECT f1 FROM int4_tbl ORDER BY 1;

--
-- Operator precedence and (((((extra))))) parentheses
--

SELECT q1 FROM int8_tbl INTERSECT SELECT q2 FROM int8_tbl UNION ALL SELECT q2 FROM int8_tbl  ORDER BY 1;

SELECT q1 FROM int8_tbl INTERSECT (((SELECT q2 FROM int8_tbl UNION ALL SELECT q2 FROM int8_tbl))) ORDER BY 1;

(((SELECT q1 FROM int8_tbl INTERSECT SELECT q2 FROM int8_tbl ORDER BY 1))) UNION ALL SELECT q2 FROM int8_tbl;

SELECT q1 FROM int8_tbl UNION ALL SELECT q2 FROM int8_tbl EXCEPT SELECT q1 FROM int8_tbl ORDER BY 1;

SELECT q1 FROM int8_tbl UNION ALL (((SELECT q2 FROM int8_tbl EXCEPT SELECT q1 FROM int8_tbl ORDER BY 1)));

(((SELECT q1 FROM int8_tbl UNION ALL SELECT q2 FROM int8_tbl))) EXCEPT SELECT q1 FROM int8_tbl ORDER BY 1;

--
-- Subqueries with ORDER BY & LIMIT clauses
--

-- In this syntax, ORDER BY/LIMIT apply to the result of the EXCEPT
SELECT q1,q2 FROM int8_tbl EXCEPT SELECT q2,q1 FROM int8_tbl
ORDER BY q2,q1;

-- This should fail, because q2 isn't a name of an EXCEPT output column
SELECT q1 FROM int8_tbl EXCEPT SELECT q2 FROM int8_tbl ORDER BY q2 LIMIT 1;

-- But this should work:
SELECT q1 FROM int8_tbl EXCEPT (((SELECT q2 FROM int8_tbl ORDER BY q2 LIMIT 1))) ORDER BY 1;

--
-- New syntaxes (7.1) permit new tests
--

(((((select * from int8_tbl)))));

--
-- Check behavior with empty select list (allowed since 9.4)
--

select union select;
select intersect select;
select except select;

-- check hashed implementation
set enable_hashagg = true;
set enable_groupagg = false;

explain (costs off)
select from generate_series(1,5) union select from generate_series(1,3);
explain (costs off)
select from generate_series(1,5) intersect select from generate_series(1,3);

select from generate_series(1,5) union select from generate_series(1,3);
select from generate_series(1,5) union all select from generate_series(1,3);
select from generate_series(1,5) intersect select from generate_series(1,3);
select from generate_series(1,5) intersect all select from generate_series(1,3);
select from generate_series(1,5) except select from generate_series(1,3);
select from generate_series(1,5) except all select from generate_series(1,3);

-- check sorted implementation
set enable_hashagg = false;
set enable_groupagg = true;

explain (costs off)
select from generate_series(1,5) union select from generate_series(1,3);
explain (costs off)
select from generate_series(1,5) intersect select from generate_series(1,3);

select from generate_series(1,5) union select from generate_series(1,3);
select from generate_series(1,5) union all select from generate_series(1,3);
select from generate_series(1,5) intersect select from generate_series(1,3);
select from generate_series(1,5) intersect all select from generate_series(1,3);
select from generate_series(1,5) except select from generate_series(1,3);
select from generate_series(1,5) except all select from generate_series(1,3);

-- Try a variation of the above but with a CTE which contains a column, again
-- with an empty final select list.

-- Ensure we get the expected 1 row with 0 columns
with cte as materialized (select s from generate_series(1,5) s)
select from cte union select from cte;

-- Ensure we get the same result as the above.
with cte as not materialized (select s from generate_series(1,5) s)
select from cte union select from cte;

reset enable_hashagg;
reset enable_groupagg;

--
-- Check handling of a case with unknown constants.  We don't guarantee
-- an undecorated constant will work in all cases, but historically this
-- usage has worked, so test we don't break it.
--

SELECT a.f1 FROM (SELECT 'test' AS f1 FROM varchar_tbl) a
UNION
SELECT b.f1 FROM (SELECT f1 FROM varchar_tbl) b
ORDER BY 1;

-- This should fail, but it should produce an error cursor
SELECT '3.4'::numeric UNION SELECT 'foo';

--
-- Test that expression-index constraints can be pushed down through
-- UNION or UNION ALL
--

CREATE TEMP TABLE t1 (a text, b text);
CREATE INDEX t1_ab_idx on t1 ((a || b));
CREATE TEMP TABLE t2 (ab text primary key);
INSERT INTO t1 VALUES ('a', 'b'), ('x', 'y');
INSERT INTO t2 VALUES ('ab'), ('xy');

set enable_seqscan = off;
set enable_indexscan = on;
set enable_bitmapscan = off;
set enable_sort = off;

explain (costs off)
 SELECT * FROM
 (SELECT a || b AS ab FROM t1
  UNION ALL
  SELECT * FROM t2) t
 WHERE ab = 'ab';

explain (costs off)
 SELECT * FROM
 (SELECT a || b AS ab FROM t1
  UNION
  SELECT * FROM t2) t
 WHERE ab = 'ab';

--
-- Test that ORDER BY for UNION ALL can be pushed down to inheritance
-- children.
--

CREATE TEMP TABLE t1c (b text, a text);
ALTER TABLE t1c INHERIT t1;
CREATE TEMP TABLE t2c (primary key (ab)) INHERITS (t2);
INSERT INTO t1c VALUES ('v', 'w'), ('c', 'd'), ('m', 'n'), ('e', 'f');
INSERT INTO t2c VALUES ('vw'), ('cd'), ('mn'), ('ef');
CREATE INDEX t1c_ab_idx on t1c ((a || b));

set enable_seqscan = on;
set enable_indexonlyscan = off;

explain (costs off)
  SELECT * FROM
  (SELECT a || b AS ab FROM t1
   UNION ALL
   SELECT ab FROM t2) t
  ORDER BY 1 LIMIT 8;

  SELECT * FROM
  (SELECT a || b AS ab FROM t1
   UNION ALL
   SELECT ab FROM t2) t
  ORDER BY 1 LIMIT 8;

reset enable_seqscan;
reset enable_indexscan;
reset enable_bitmapscan;
reset enable_sort;

-- This simpler variant of the above test has been observed to fail differently

create table events (event_id int primary key);
create table other_events (event_id int primary key);
create table events_child () inherits (events);

explain (costs off)
select event_id
 from (select event_id from events
       union all
       select event_id from other_events) ss
 order by event_id;

drop table events_child, events, other_events;

reset enable_indexonlyscan;

--
-- Test handling of UNION / EXCEPT / INTERSECT with provably empty inputs
--

-- Ensure the empty UNION input is pruned and de-duplication is done for the
-- remaining relation.
EXPLAIN (COSTS OFF, VERBOSE)
SELECT two FROM tenk1 WHERE 1=2
UNION
SELECT four FROM tenk1
ORDER BY 1;

-- Validate that the results of the above are correct
SELECT two FROM tenk1 WHERE 1=2
UNION
SELECT four FROM tenk1
ORDER BY 1;

-- All UNION inputs are proven empty.  Ensure the planner provides a
-- const-false Result node
EXPLAIN (COSTS OFF, VERBOSE)
SELECT two FROM tenk1 WHERE 1=2
UNION
SELECT four FROM tenk1 WHERE 1=2
UNION
SELECT ten FROM tenk1 WHERE 1=2
ORDER BY 1;

-- Ensure the planner provides a const-false Result node
EXPLAIN (COSTS OFF, VERBOSE)
SELECT two FROM tenk1 WHERE 1=2
INTERSECT
SELECT four FROM tenk1
ORDER BY 1;

-- As above, with the inputs swapped
EXPLAIN (COSTS OFF, VERBOSE)
SELECT four FROM tenk1
INTERSECT
SELECT two FROM tenk1 WHERE 1=2
ORDER BY 1;

-- Try with both inputs dummy
EXPLAIN (COSTS OFF, VERBOSE)
SELECT four FROM tenk1 WHERE 1=2
INTERSECT
SELECT two FROM tenk1 WHERE 1=2
ORDER BY 1;

-- Ensure the planner provides a const-false Result node when the left input
-- is empty
EXPLAIN (COSTS OFF, VERBOSE)
SELECT two FROM tenk1 WHERE 1=2
EXCEPT
SELECT four FROM tenk1
ORDER BY 1;

-- Ensure the planner only scans the left input when right input is empty
EXPLAIN (COSTS OFF, VERBOSE)
SELECT two FROM tenk1
EXCEPT ALL
SELECT four FROM tenk1 WHERE 1=2
ORDER BY 1;

-- Try mixed setop cases.  Ensure the right-hand UNION child gets removed.
EXPLAIN (COSTS OFF, VERBOSE)
SELECT two FROM tenk1 t1
EXCEPT
SELECT four FROM tenk1 t2
UNION
SELECT ten FROM tenk1 dummy WHERE 1=2;

EXPLAIN (COSTS OFF, VERBOSE)
SELECT two FROM tenk1 t1 WHERE two = 1
INTERSECT ALL
SELECT four FROM tenk1 t2 WHERE four = 1
UNION
SELECT ten FROM tenk1 dummy WHERE 1=2;

-- As above, but with sorting: ensure we do not get confused by child pathkeys
SET enable_hashagg = off;

EXPLAIN (COSTS OFF, VERBOSE)
SELECT two FROM tenk1 t1 WHERE two = 1
INTERSECT ALL
SELECT four FROM tenk1 t2 WHERE four = 1
UNION
SELECT ten FROM tenk1 dummy WHERE 1=2;

-- also check case where the upper op is EXCEPT
EXPLAIN (COSTS OFF, VERBOSE)
(SELECT two FROM tenk1 t1 WHERE two = 1
 INTERSECT
 SELECT four FROM tenk1 t2 WHERE four = 1)
EXCEPT ALL
SELECT ten FROM tenk1 dummy WHERE 1=2;

RESET enable_hashagg;

-- Ensure EXPLAIN can show a dummy set operation whose output is coerced
-- to another type by the parent set operation.
EXPLAIN (COSTS OFF, VERBOSE)
SELECT two::numeric FROM tenk1 t1
EXCEPT
(SELECT four FROM tenk1 dummy WHERE 1=2
 EXCEPT ALL
 SELECT ten FROM tenk1 t2);

-- Test constraint exclusion of UNION ALL subqueries
explain (costs off)
 SELECT * FROM
  (SELECT 1 AS t, * FROM tenk1 a
   UNION ALL
   SELECT 2 AS t, * FROM tenk1 b) c
 WHERE t = 2;

-- Test that we push quals into UNION sub-selects only when it's safe
explain (costs off)
SELECT * FROM
  (SELECT 1 AS t, 2 AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x < 4
ORDER BY x;

SELECT * FROM
  (SELECT 1 AS t, 2 AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x < 4
ORDER BY x;

explain (costs off)
SELECT * FROM
  (SELECT 1 AS t, generate_series(1,10) AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x < 4
ORDER BY x;

SELECT * FROM
  (SELECT 1 AS t, generate_series(1,10) AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x < 4
ORDER BY x;

explain (costs off)
SELECT * FROM
  (SELECT 1 AS t, (random()*3)::int AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x > 3
ORDER BY x;

SELECT * FROM
  (SELECT 1 AS t, (random()*3)::int AS x
   UNION
   SELECT 2 AS t, 4 AS x) ss
WHERE x > 3
ORDER BY x;

-- Test cases where the native ordering of a sub-select has more pathkeys
-- than the outer query cares about
explain (costs off)
select distinct q1 from
  (select distinct * from int8_tbl i81
   union all
   select distinct * from int8_tbl i82) ss
where q2 = q2;

select distinct q1 from
  (select distinct * from int8_tbl i81
   union all
   select distinct * from int8_tbl i82) ss
where q2 = q2;

explain (costs off)
select distinct q1 from
  (select distinct * from int8_tbl i81
   union all
   select distinct * from int8_tbl i82) ss
where -q1 = q2;

select distinct q1 from
  (select distinct * from int8_tbl i81
   union all
   select distinct * from int8_tbl i82) ss
where -q1 = q2;

-- Test proper handling of parameterized appendrel paths when the
-- potential join qual is expensive
create function expensivefunc(int) returns int
language plpgsql immutable strict cost 10000
as $$begin return $1; end$$;

create temp table t3 as select generate_series(-1000,1000) as x;
create index t3i on t3 (expensivefunc(x));
analyze t3;

explain (costs off)
select * from
  (select * from t3 a union all select * from t3 b) ss
  join int4_tbl on f1 = expensivefunc(x);
select * from
  (select * from t3 a union all select * from t3 b) ss
  join int4_tbl on f1 = expensivefunc(x);

drop table t3;
drop function expensivefunc(int);

-- Test handling of appendrel quals that const-simplify into an AND
explain (costs off)
select * from
  (select *, 0 as x from int8_tbl a
   union all
   select *, 1 as x from int8_tbl b) ss
where (x = 0) or (q1 >= q2 and q1 <= q2);
select * from
  (select *, 0 as x from int8_tbl a
   union all
   select *, 1 as x from int8_tbl b) ss
where (x = 0) or (q1 >= q2 and q1 <= q2);

--
-- Test the planner's ability to produce cheap startup plans with Append nodes
--

-- Ensure we get a Nested Loop join between tenk1 and tenk2
explain (costs off)
select t1.unique1 from tenk1 t1
inner join tenk2 t2 on t1.tenthous = t2.tenthous and t2.thousand = 0
   union all
(values(1)) limit 1;

-- Ensure there is no problem if cheapest_startup_path is NULL
explain (costs off)
select * from tenk1 t1
left join lateral
  (select t1.tenthous from tenk2 t2 union all (values(1)))
on true limit 1;

-- Test handling of Vars with varno 0 in estimate_array_length
explain (verbose, costs off)
select null::int[] union all select null::int[] union all select null::bigint[];

-- Estimate a UNION's output rows as the sum of its children's distinct-group
-- estimates, not the total input size.  Ensure we get an index nested loop
explain (costs off)
select * from tenk1 t
join (select ten from tenk1 union select ten from onek) s on s.ten = t.unique1;

--
-- Parallel partial deduplication for UNION
--
-- Force parallel paths to be cheap and available on the small regression
-- tables.
--
set parallel_setup_cost = 0;
set parallel_tuple_cost = 0;
set min_parallel_table_scan_size = 0;
set max_parallel_workers_per_gather = 2;

-- With an index-provided order, MergeAppend + Unique still wins.
explain (costs off)
select unique1 from tenk1 union select unique1 from tenk1;

-- The same holds for ORDER BY + LIMIT, which can terminate early.
explain (costs off)
select unique1 from tenk1 union select unique1 from tenk1
order by 1 limit 10;

create table union_par_nostat (a int, b int) with (autovacuum_enabled = false);
insert into union_par_nostat select g % 100, g % 100 from generate_series(1, 20000) g;

-- Grouped and DISTINCT children: the current planner does not expose
-- partial paths for these child shapes, so no union-level worker dedup path
-- is built.  Keep the plan and result coverage for them.
explain (costs off)
select a from (select a from union_par_nostat group by a) s
union select a from union_par_nostat;
select count(*) from
  (select a as x from (select a from union_par_nostat group by a) s
   union select a from union_par_nostat) s;

explain (costs off)
select distinct a from union_par_nostat
union select a from union_par_nostat;
select count(*) from
  (select distinct a as x from union_par_nostat
   union select a from union_par_nostat) s;

-- A child that is itself a set operation: likewise no partial path is
-- exposed, so this checks the current plan shape and result rather than the
-- worker-side dedup path.
explain (costs off)
select a from (select a from union_par_nostat
               intersect select a from union_par_nostat) s
union select a from union_par_nostat;
select count(*) from
  (select a as x from (select a from union_par_nostat
                       intersect select a from union_par_nostat) s
   union select a from union_par_nostat) s;

-- No statistics: the estimate is unreliable, but the worst-case hash table
-- (one group per input row) fits in hash_mem, so the worker-side path is
-- still considered.
explain (costs off)
select a, b from union_par_nostat union select a, b from union_par_nostat;

-- With a small work_mem the worst-case hash table does not fit, so the
-- worker-side path is rejected.
set work_mem = '64kB';
explain (costs off)
select a, b from union_par_nostat union select a, b from union_par_nostat;
reset work_mem;

create statistics union_par_stats (ndistinct) on a, b from union_par_nostat;
analyze union_par_nostat;

-- Extended statistics give a usable ndistinct estimate for the multi-column
-- key, so the worker-side dedup path is chosen.
explain (costs off)
select a, b from union_par_nostat union select a, b from union_par_nostat;

-- Execute the plan and check the result.
select count(*) from
  (select a, b from union_par_nostat union select a, b from union_par_nostat) s;

-- Multi-way UNION is flattened into a single partial-dedup path.
explain (costs off)
select a, b from union_par_nostat
union select a, b from union_par_nostat
union select a, b from union_par_nostat;

-- A nested UNION ALL child is flattened into the parent UNION.
explain (costs off)
select a, b from union_par_nostat
union (select a, b from union_par_nostat union all select a, b from union_par_nostat);

-- A near-unique multi-column key without extended statistics must not use
-- the worker-side dedup path: the 10% correlation clamp makes the estimate
-- unreliable.
create table union_par_unique (a int, b int);
insert into union_par_unique
select g / 1000, g % 1000 from generate_series(1, 200000) g;
analyze union_par_unique;
explain (costs off)
select a, b from union_par_unique union select a, b from union_par_unique;
drop table union_par_unique;

-- With enable_hashagg off, cost_agg's disabled_nodes penalty prevents the
-- hash-aggregate paths from being chosen.
set enable_hashagg = off;
explain (costs off)
select a, b from union_par_nostat union select a, b from union_par_nostat;
reset enable_hashagg;

-- Without parallel append, the path must still be correct.
set enable_parallel_append = off;
explain (costs off)
select a, b from union_par_nostat union select a, b from union_par_nostat;

-- Execute the plan and check the result.
select count(*) from
  (select a, b from union_par_nostat union select a, b from union_par_nostat) s;
reset enable_parallel_append;

-- With no workers, the serial path is used.
set max_parallel_workers_per_gather = 0;
explain (costs off)
select a, b from union_par_nostat union select a, b from union_par_nostat;

drop table union_par_nostat;
reset parallel_setup_cost;
reset parallel_tuple_cost;
reset min_parallel_table_scan_size;
reset max_parallel_workers_per_gather;

--
-- A de-duplicating set operation needs only one copy of each row, so a
-- branch whose output is a single constant tuple can stop after its first
-- row.  This applies to every child of UNION/INTERSECT/EXCEPT, but not to
-- their ALL variants, where duplicate counts are significant.
--
create table union_const_t (a int);
insert into union_const_t select g from generate_series(1, 1000) g;
analyze union_const_t;
create sequence union_const_seq;

-- Serial plan: only the constant child is limited.
set max_parallel_workers_per_gather = 0;
explain (costs off)
select 1 from union_const_t union select a from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t union select a from union_const_t) s;

-- Every constant child is limited.
explain (costs off)
select 1 from union_const_t union select 1 from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t union select 1 from union_const_t) s;

-- A locking clause hidden in a subquery or CTE is subject to the same
-- early-stop behavior as SELECT DISTINCT's Limit 1: only the first row is
-- fetched, so only that row is locked.  This is intentional.
explain (costs off)
select 1 from (select a from union_const_t for update) s union select 2;
select count(*), min(x), max(x) from
  (select 1 as x from (select a from union_const_t for update) s
   union select 2) t;

explain (costs off)
select 1 from (with c as (select a from union_const_t for update)
               select a from c) s union select 2;
select count(*), min(x), max(x) from
  (select 1 as x from (with c as (select a from union_const_t for update)
                       select a from c) s union select 2) t;

explain (costs off)
select 1 from (select a from union_const_t for share) s union select 2;

-- The zero-column form also qualifies; it is the case where row identity
-- targets do not incidentally prevent the optimization.
explain (costs off)
select * from (select from union_const_t for update) s
union select from union_const_t;

-- NULL is a pseudo-constant just like any other constant.
explain (costs off)
select null::int from union_const_t union select 1;
select count(*), count(x) from
  (select null::int as x from union_const_t union select 1) s;

-- A child with no FROM clause already produces at most one row, so the
-- planner deliberately does not add a Limit node.
explain (costs off)
select 1 union select 2;

-- An uncorrelated sublink in the target list becomes an InitPlan Param;
-- it is constant for the scan and is accepted.  (Correlated sublinks
-- become SubPlans and are rejected by setop_child_output_is_constant.)
explain (costs off)
select (select max(a) from union_const_t) from union_const_t
union select 999;
select count(*), min(x), max(x) from
  (select (select max(a) from union_const_t) as x from union_const_t
   union select 999) s;

-- Multi-way UNION: each constant branch gets its own Limit.
explain (costs off)
select 1 from union_const_t
union select 2 from union_const_t
union select a from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t
   union select 2 from union_const_t
   union select a from union_const_t) s;

-- INTERSECT and EXCEPT also remove duplicates from both inputs.
explain (costs off)
select 1 from union_const_t intersect select a from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t intersect select a from union_const_t) s;

explain (costs off)
select a from union_const_t intersect select 1 from union_const_t;
select count(*), min(x), max(x) from
  (select a as x from union_const_t intersect select 1 from union_const_t) s;

explain (costs off)
select a from union_const_t except select 1 from union_const_t;
select count(*), min(x), max(x) from
  (select a as x from union_const_t except select 1 from union_const_t) s;

explain (costs off)
select 1 from union_const_t except select a from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t except select a from union_const_t) s;

-- Parallel plan: the limited partial path must remain available, otherwise
-- the whole set operation would fall back to a serial Append.
set parallel_setup_cost = 0;
set parallel_tuple_cost = 0;
set min_parallel_table_scan_size = 0;
set max_parallel_workers_per_gather = 2;
explain (costs off)
select 1 from union_const_t union select a from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t union select a from union_const_t) s;

-- Multi-way UNION keeps the partial path as well.
explain (costs off)
select 1 from union_const_t
union select 2 from union_const_t
union select a from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t
   union select 2 from union_const_t
   union select a from union_const_t) s;

-- One worker exercises the Gather row estimate with a single partial path.
set max_parallel_workers_per_gather = 1;
explain (costs off)
select 1 from union_const_t union select a from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t union select a from union_const_t) s;
set max_parallel_workers_per_gather = 2;

reset max_parallel_workers_per_gather;
reset min_parallel_table_scan_size;
reset parallel_tuple_cost;
reset parallel_setup_cost;

-- The sort-based strategies see the limit as well.
set enable_hashagg = off;
explain (costs off)
select 1 from union_const_t union select a from union_const_t;
select count(*), min(x), max(x) from
  (select 1 as x from union_const_t union select a from union_const_t) s;
explain (costs off)
select a from union_const_t except select 1 from union_const_t;
select count(*), min(x), max(x) from
  (select a as x from union_const_t except select 1 from union_const_t) s;
reset enable_hashagg;

-- UNION ALL preserves duplicates and must not be limited.
explain (costs off)
select 1 from union_const_t union all select a from union_const_t;
select count(*) from
  (select 1 from union_const_t union all select a from union_const_t) s;

-- INTERSECT ALL / EXCEPT ALL preserve duplicate counts and must not be
-- limited either.
create table union_const_dup (a int);
insert into union_const_dup values (1), (1), (1);
explain (costs off)
select 1 from union_const_dup intersect all select 1 from union_const_dup;
select count(*) from
  (select 1 from union_const_dup intersect all select 1 from union_const_dup) s;
explain (costs off)
select 1 from union_const_dup except all select 2 from union_const_dup;
select count(*) from
  (select 1 from union_const_dup except all select 2 from union_const_dup) s;

-- The multiplicity of either input matters, so neither side may be limited.
explain (costs off)
select a from union_const_dup intersect all select 1 from union_const_dup;
select count(*) from
  (select a from union_const_dup intersect all select 1 from union_const_dup) s;
explain (costs off)
select 1 from union_const_dup except all select 1 from union_const_dup;
select count(*) from
  (select 1 from union_const_dup except all select 1 from union_const_dup) s;

-- The set variants of the same queries do discard duplicates.
select count(*) from
  (select 1 from union_const_dup intersect select 1 from union_const_dup) s;
select count(*) from
  (select 1 from union_const_dup except select 2 from union_const_dup) s;

-- Empty inputs must stay empty.
select count(*) from
  (select 1 from union_const_dup where false
   union select 2 where false) s;

-- Outputs that can vary from row to row must not be limited.
set max_parallel_workers_per_gather = 0;
explain (costs off)
select nextval('union_const_seq') from union_const_t
union select a from union_const_t;
explain (costs off)
select row_number() over () from union_const_t union select a from union_const_t;
explain (costs off)
select generate_series(1, 3) from union_const_t union select a from union_const_t;

-- Equivalence classes can pin a Var to a constant, as in SELECT DISTINCT.
explain (costs off)
select a from union_const_t where a = 5 union select 9;
select count(*), min(x), max(x) from
  (select a as x from union_const_t where a = 5 union select 9) s;

explain (costs off)
select a from union_const_t group by a having a = 5 union select 9;
select count(*), min(x), max(x) from
  (select a as x from union_const_t group by a having a = 5 union select 9) s;

-- A join equality can carry the constant to another column.
explain (costs off)
select t.unique1 from tenk1 t join int8_tbl i on t.unique1 = i.q1
where i.q1 = 123 union select 9;
select count(*), min(x), max(x) from
  (select t.unique1 as x from tenk1 t join int8_tbl i on t.unique1 = i.q1
   where i.q1 = 123 union select 9) s;

-- Like DISTINCT, we do not chase constants through casts or expressions.
explain (costs off)
select a::bigint from union_const_t where a = 5 union select 9::bigint;
explain (costs off)
select a + 0 from union_const_t where a = 5 union select 9;

-- A constant from below the nullable side of an outer join must not be
-- used: that side can also contribute NULL.
explain (costs off)
select ss.two
from tenk1 a left join
     (select b.two from tenk1 b join tenk1 c on b.two = c.two
      where b.two = 10) ss on a.unique1 = ss.two
union select 9;

-- The pinned branch can be on either side.
explain (costs off)
select 9 union select a from union_const_t where a = 5;
select count(*), min(x), max(x) from
  (select 9 as x union select a from union_const_t where a = 5) s;
explain (costs off)
select 9 intersect select a from union_const_t where a = 5;
select count(*) from
  (select 9 as x intersect select a from union_const_t where a = 5) s;
explain (costs off)
select 9 except select a from union_const_t where a = 5;
select count(*), min(x), max(x) from
  (select 9 as x except select a from union_const_t where a = 5) s;

-- Only one of two output columns is pinned, so the tuple is not constant.
explain (costs off)
select q1, q2 from int8_tbl where q1 = 123 union select q1, q2 from int8_tbl;
select count(*) from
  (select q1, q2 from int8_tbl where q1 = 123
   union select q1, q2 from int8_tbl) s;

-- A generic parameter is an EC constant as well.
prepare union_ec_param(int) as
  select a from union_const_t where a = $1 union select 9;
set plan_cache_mode = force_generic_plan;
explain (costs off) execute union_ec_param(5);
execute union_ec_param(5);
deallocate union_ec_param;
reset plan_cache_mode;

-- A volatile qual, unlike a volatile output column, does not make the output
-- non-constant: the child still returns one distinct tuple.  As with the
-- DISTINCT Limit 1 optimization, the scan may stop early and evaluate the
-- qual fewer times.  EXPLAIN only; nextval() has side effects.
explain (costs off)
select 1 from union_const_t where nextval('union_const_seq') > 0
union select a from union_const_t;

-- A volatile function in FROM can be evaluated fewer times as well.  This
-- mirrors the DISTINCT Limit 1 behavior.  EXPLAIN only; nextval() has side
-- effects.
explain (costs off)
select 1 from (select nextval('union_const_seq') as v
               from generate_series(1, 10)) s
where v > 0
union select a from union_const_t;

-- A recursive CTE child follows the same rule.  Keep the example finite.
explain (costs off)
with recursive union_limit_r(n) as (
  select 1
  union all
  select n + 1 from union_limit_r where n < 10
)
select 1 from union_limit_r union select 2;
select count(*) from
  (with recursive union_limit_r(n) as (
     select 1
     union all
     select n + 1 from union_limit_r where n < 10
   )
   select 1 from union_limit_r union select 2) s;

-- An otherwise infinite recursive CTE is also stopped by the implicit
-- Limit.  This matches the DISTINCT Limit 1 behavior; EXPLAIN only, since
-- the finite case above covers execution.
explain (costs off)
with recursive union_limit_inf(n) as (
  select 1
  union all
  select n + 1 from union_limit_inf
)
select 1 from union_limit_inf union select 2;

reset max_parallel_workers_per_gather;

drop table union_const_t;
drop table union_const_dup;
drop sequence union_const_seq;
