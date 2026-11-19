--
-- SELECT
--

-- btree index
-- awk '{if($1<10){print;}else{next;}}' onek.data | sort +0n -1
--
SELECT * FROM onek
   WHERE onek.unique1 < 10
   ORDER BY onek.unique1;

--
-- awk '{if($1<20){print $1,$14;}else{next;}}' onek.data | sort +0nr -1
--
SELECT onek.unique1, onek.stringu1 FROM onek
   WHERE onek.unique1 < 20
   ORDER BY unique1 using >;

--
-- awk '{if($1>980){print $1,$14;}else{next;}}' onek.data | sort +1d -2
--
SELECT onek.unique1, onek.stringu1 FROM onek
   WHERE onek.unique1 > 980
   ORDER BY stringu1 using <;

--
-- awk '{if($1>980){print $1,$16;}else{next;}}' onek.data |
-- sort +1d -2 +0nr -1
--
SELECT onek.unique1, onek.string4 FROM onek
   WHERE onek.unique1 > 980
   ORDER BY string4 using <, unique1 using >;

--
-- awk '{if($1>980){print $1,$16;}else{next;}}' onek.data |
-- sort +1dr -2 +0n -1
--
SELECT onek.unique1, onek.string4 FROM onek
   WHERE onek.unique1 > 980
   ORDER BY string4 using >, unique1 using <;

--
-- awk '{if($1<20){print $1,$16;}else{next;}}' onek.data |
-- sort +0nr -1 +1d -2
--
SELECT onek.unique1, onek.string4 FROM onek
   WHERE onek.unique1 < 20
   ORDER BY unique1 using >, string4 using <;

--
-- awk '{if($1<20){print $1,$16;}else{next;}}' onek.data |
-- sort +0n -1 +1dr -2
--
SELECT onek.unique1, onek.string4 FROM onek
   WHERE onek.unique1 < 20
   ORDER BY unique1 using <, string4 using >;

--
-- test partial btree indexes
--
-- As of 7.2, planner probably won't pick an indexscan without stats,
-- so ANALYZE first.  Also, we want to prevent it from picking a bitmapscan
-- followed by sort, because that could hide index ordering problems.
--
ANALYZE onek2;

SET enable_seqscan TO off;
SET enable_bitmapscan TO off;
SET enable_sort TO off;

--
-- awk '{if($1<10){print $0;}else{next;}}' onek.data | sort +0n -1
--
SELECT onek2.* FROM onek2 WHERE onek2.unique1 < 10;

--
-- awk '{if($1<20){print $1,$14;}else{next;}}' onek.data | sort +0nr -1
--
SELECT onek2.unique1, onek2.stringu1 FROM onek2
    WHERE onek2.unique1 < 20
    ORDER BY unique1 using >;

--
-- awk '{if($1>980){print $1,$14;}else{next;}}' onek.data | sort +1d -2
--
SELECT onek2.unique1, onek2.stringu1 FROM onek2
   WHERE onek2.unique1 > 980;

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_sort;

--
-- awk '{print $1,$2;}' person.data |
-- awk '{if(NF!=2){print $3,$2;}else{print;}}' - emp.data |
-- awk '{if(NF!=2){print $3,$2;}else{print;}}' - student.data |
-- awk 'BEGIN{FS="      ";}{if(NF!=2){print $4,$5;}else{print;}}' - stud_emp.data
--
-- SELECT name, age FROM person*; ??? check if different
SELECT p.name, p.age FROM person* p;

--
-- awk '{print $1,$2;}' person.data |
-- awk '{if(NF!=2){print $3,$2;}else{print;}}' - emp.data |
-- awk '{if(NF!=2){print $3,$2;}else{print;}}' - student.data |
-- awk 'BEGIN{FS="      ";}{if(NF!=1){print $4,$5;}else{print;}}' - stud_emp.data |
-- sort +1nr -2
--
SELECT p.name, p.age FROM person* p ORDER BY age using >, name;

--
-- Test some cases involving whole-row Var referencing a subquery
--
select foo from (select 1 offset 0) as foo;
select foo from (select null offset 0) as foo;
select foo from (select 'xyzzy',1,null offset 0) as foo;

--
-- Test VALUES lists
--
select * from onek, (values(147, 'RFAAAA'), (931, 'VJAAAA')) as v (i, j)
    WHERE onek.unique1 = v.i and onek.stringu1 = v.j;

-- a more complex case
-- looks like we're coding lisp :-)
select * from onek,
  (values ((select i from
    (values(10000), (2), (389), (1000), (2000), ((select 10029))) as foo(i)
    order by i asc limit 1))) bar (i)
  where onek.unique1 = bar.i;

-- try VALUES in a subquery
select * from onek
    where (unique1,ten) in (values (1,1), (20,0), (99,9), (17,99))
    order by unique1;

-- VALUES is also legal as a standalone query or a set-operation member
VALUES (1,2), (3,4+4), (7,77.7);

VALUES (1,2), (3,4+4), (7,77.7)
UNION ALL
SELECT 2+2, 57
UNION ALL
TABLE int8_tbl;

-- corner case: VALUES with no columns
CREATE TEMP TABLE nocols();
INSERT INTO nocols DEFAULT VALUES;
SELECT * FROM nocols n, LATERAL (VALUES(n.*)) v;

--
-- Test ORDER BY options
--

CREATE TEMP TABLE foo (f1 int);

INSERT INTO foo VALUES (42),(3),(10),(7),(null),(null),(1);

SELECT * FROM foo ORDER BY f1;
SELECT * FROM foo ORDER BY f1 ASC;	-- same thing
SELECT * FROM foo ORDER BY f1 NULLS FIRST;
SELECT * FROM foo ORDER BY f1 DESC;
SELECT * FROM foo ORDER BY f1 DESC NULLS LAST;

-- check if indexscans do the right things
CREATE INDEX fooi ON foo (f1);
SET enable_sort = false;

SELECT * FROM foo ORDER BY f1;
SELECT * FROM foo ORDER BY f1 NULLS FIRST;
SELECT * FROM foo ORDER BY f1 DESC;
SELECT * FROM foo ORDER BY f1 DESC NULLS LAST;

DROP INDEX fooi;
CREATE INDEX fooi ON foo (f1 DESC);

SELECT * FROM foo ORDER BY f1;
SELECT * FROM foo ORDER BY f1 NULLS FIRST;
SELECT * FROM foo ORDER BY f1 DESC;
SELECT * FROM foo ORDER BY f1 DESC NULLS LAST;

DROP INDEX fooi;
CREATE INDEX fooi ON foo (f1 DESC NULLS LAST);

SELECT * FROM foo ORDER BY f1;
SELECT * FROM foo ORDER BY f1 NULLS FIRST;
SELECT * FROM foo ORDER BY f1 DESC;
SELECT * FROM foo ORDER BY f1 DESC NULLS LAST;

--
-- Test planning of some cases with partial indexes
--

-- partial index is usable
explain (costs off)
select * from onek2 where unique2 = 11 and stringu1 = 'ATAAAA';
select * from onek2 where unique2 = 11 and stringu1 = 'ATAAAA';
-- actually run the query with an analyze to use the partial index
explain (costs off, analyze on, timing off, summary off, buffers off)
select * from onek2 where unique2 = 11 and stringu1 = 'ATAAAA';
explain (costs off)
select unique2 from onek2 where unique2 = 11 and stringu1 = 'ATAAAA';
select unique2 from onek2 where unique2 = 11 and stringu1 = 'ATAAAA';
-- partial index predicate implies clause, so no need for retest
explain (costs off)
select * from onek2 where unique2 = 11 and stringu1 < 'B';
select * from onek2 where unique2 = 11 and stringu1 < 'B';
explain (costs off)
select unique2 from onek2 where unique2 = 11 and stringu1 < 'B';
select unique2 from onek2 where unique2 = 11 and stringu1 < 'B';
-- but if it's an update target, must retest anyway
explain (costs off)
select unique2 from onek2 where unique2 = 11 and stringu1 < 'B' for update;
select unique2 from onek2 where unique2 = 11 and stringu1 < 'B' for update;
-- partial index is not applicable
explain (costs off)
select unique2 from onek2 where unique2 = 11 and stringu1 < 'C';
select unique2 from onek2 where unique2 = 11 and stringu1 < 'C';
-- partial index implies clause, but bitmap scan must recheck predicate anyway
SET enable_indexscan TO off;
explain (costs off)
select unique2 from onek2 where unique2 = 11 and stringu1 < 'B';
select unique2 from onek2 where unique2 = 11 and stringu1 < 'B';
-- check multi-index cases too
explain (costs off)
select unique1, unique2 from onek2
  where (unique2 = 11 or unique1 = 0) and stringu1 < 'B';
select unique1, unique2 from onek2
  where (unique2 = 11 or unique1 = 0) and stringu1 < 'B';
explain (costs off)
select unique1, unique2 from onek2
  where (unique2 = 11 and stringu1 < 'B') or unique1 = 0;
select unique1, unique2 from onek2
  where (unique2 = 11 and stringu1 < 'B') or unique1 = 0;
RESET enable_indexscan;

-- onek2_u2_prtl should be preferred over this index, but we have to
-- discount the metapage to arrive at that answer
begin;
create index onek2_index_full on onek2 (stringu1, unique2);
explain (costs off)
select unique2 from onek2
  where stringu1 < 'B'::name;
rollback;

--
-- Test some corner cases that have been known to confuse the planner
--

-- ORDER BY on a constant doesn't really need any sorting
SELECT 1 AS x ORDER BY x;

-- But ORDER BY on a set-valued expression does
create function sillysrf(int) returns setof int as
  'values (1),(10),(2),($1)' language sql immutable;

select sillysrf(42);
select sillysrf(-1) order by 1;

drop function sillysrf(int);

-- X = X isn't a no-op, it's effectively X IS NOT NULL assuming = is strict
-- (see bug #5084)
select * from (values (2),(null),(1)) v(k) where k = k order by k;
select * from (values (2),(null),(1)) v(k) where k = k;

-- Test partitioned tables with no partitions, which should be handled the
-- same as the non-inheritance case when expanding its RTE.
create table list_parted_tbl (a int,b int) partition by list (a);
create table list_parted_tbl1 partition of list_parted_tbl
  for values in (1) partition by list(b);
explain (costs off) select * from list_parted_tbl;
drop table list_parted_tbl;

--
-- ORDER BY ALL
--

create temp table order_all_tbl (a int, b text, c float8);
insert into order_all_tbl values
  (3, 'foo', 1.5), (1, 'bar', null), (2, 'baz', 1.5), (1, 'qux', 1.5),
  (null, 'nul', 3.0), (2, null, 2.0), (3, 'foo', null);

-- direction and NULLS variants
select * from order_all_tbl order by all;
select * from order_all_tbl order by all asc;
select * from order_all_tbl order by all desc;
select * from order_all_tbl order by all nulls first;
select * from order_all_tbl order by all nulls last;
select * from order_all_tbl order by all asc nulls first;
select * from order_all_tbl order by all asc nulls last;
select * from order_all_tbl order by all desc nulls first;
select * from order_all_tbl order by all desc nulls last;

-- equivalent to listing every select-list column
select * from order_all_tbl order by a desc, b desc, c desc;

-- subset of columns, expressions, duplicate columns, aggregates
select a, b from order_all_tbl order by all;
select a + 1 as x, b || '_t' as y from order_all_tbl order by all desc;
select a, a from order_all_tbl order by all;
select a, count(*) from order_all_tbl group by a order by all;

-- interaction with other clauses
select distinct a, b from order_all_tbl order by all;
select distinct on (a) a, b from order_all_tbl order by all;
select * from order_all_tbl order by all desc limit 3;
select * from order_all_tbl where a > 1 order by all;

-- subqueries, CTEs, joins, VALUES
select * from (select a, b from order_all_tbl order by all) sq;
with cte as (select a, b from order_all_tbl where a > 1)
  select * from cte order by all desc;
select t1.a, t1.b, t2.b as b2
  from order_all_tbl t1 join order_all_tbl t2 on t1.a = t2.a and t1.b < t2.b
  order by all;
select * from (values (3, 'foo'), (1, 'bar'), (2, 'baz')) as t(x, y) order by all;
values (3, 'foo'), (1, 'bar'), (2, 'baz') order by all desc;

-- not allowed: ALL combined with other sort items or a USING operator
select * from order_all_tbl order by all, a;
select * from order_all_tbl order by a, all;
select * from order_all_tbl order by all, all;
select * from order_all_tbl order by all using <;

-- ORDER BY ALL together with GROUP BY and DISTINCT must behave exactly like
-- the equivalent explicit ORDER BY (compare each pair of results)
create temp table order_all_num (n numeric, g int);
insert into order_all_num values
  (1.5, 1), (1.5, 1), (2.5, 2), (2.5, 2), (1.5, 3), (null, 4), (null, 4);

select n, count(*) from order_all_num group by n order by all desc;
select n, count(*) from order_all_num group by n order by n desc, count(*) desc;
select distinct n from order_all_num order by all;
select distinct n from order_all_num order by n;
select g, max(n), count(*) from order_all_num group by g
  order by all desc nulls last;
select g, max(n), count(*) from order_all_num group by g
  order by g desc nulls last, max(n) desc nulls last, count(*) desc nulls last;
select g, n, count(*) from order_all_num group by rollup (g, n)
  order by all nulls last;
select g, n, count(*) from order_all_num group by rollup (g, n)
  order by g nulls last, n nulls last, count(*) nulls last;

-- an ungrouped select-list column is still an error
select n, g from order_all_num group by n order by all;

-- a second ORDER BY on a parenthesized query is an error
(select n from order_all_num order by n) order by all;

drop table order_all_num;

drop table order_all_tbl;
