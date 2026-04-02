--
-- LIMIT
-- Check the LIMIT/OFFSET feature of SELECT
--

SELECT ''::text AS two, unique1, unique2, stringu1
		FROM onek WHERE unique1 > 50
		ORDER BY unique1 LIMIT 2;
SELECT ''::text AS five, unique1, unique2, stringu1
		FROM onek WHERE unique1 > 60
		ORDER BY unique1 LIMIT 5;
SELECT ''::text AS two, unique1, unique2, stringu1
		FROM onek WHERE unique1 > 60 AND unique1 < 63
		ORDER BY unique1 LIMIT 5;
SELECT ''::text AS three, unique1, unique2, stringu1
		FROM onek WHERE unique1 > 100
		ORDER BY unique1 LIMIT 3 OFFSET 20;
SELECT ''::text AS zero, unique1, unique2, stringu1
		FROM onek WHERE unique1 < 50
		ORDER BY unique1 DESC LIMIT 8 OFFSET 99;
SELECT ''::text AS eleven, unique1, unique2, stringu1
		FROM onek WHERE unique1 < 50
		ORDER BY unique1 DESC LIMIT 20 OFFSET 39;
SELECT ''::text AS ten, unique1, unique2, stringu1
		FROM onek
		ORDER BY unique1 OFFSET 990;
SELECT ''::text AS five, unique1, unique2, stringu1
		FROM onek
		ORDER BY unique1 OFFSET 990 LIMIT 5;
SELECT ''::text AS five, unique1, unique2, stringu1
		FROM onek
		ORDER BY unique1 LIMIT 5 OFFSET 900;

-- Test null limit and offset.  The planner would discard a simple null
-- constant, so to ensure executor is exercised, do this:
select * from int8_tbl limit (case when random() < 0.5 then null::bigint end);
select * from int8_tbl offset (case when random() < 0.5 then null::bigint end);

-- Test assorted cases involving backwards fetch from a LIMIT plan node
begin;

declare c1 cursor for select * from int8_tbl limit 10;
fetch all in c1;
fetch 1 in c1;
fetch backward 1 in c1;
fetch backward all in c1;
fetch backward 1 in c1;
fetch all in c1;

declare c2 cursor for select * from int8_tbl limit 3;
fetch all in c2;
fetch 1 in c2;
fetch backward 1 in c2;
fetch backward all in c2;
fetch backward 1 in c2;
fetch all in c2;

declare c3 cursor for select * from int8_tbl offset 3;
fetch all in c3;
fetch 1 in c3;
fetch backward 1 in c3;
fetch backward all in c3;
fetch backward 1 in c3;
fetch all in c3;

declare c4 cursor for select * from int8_tbl offset 10;
fetch all in c4;
fetch 1 in c4;
fetch backward 1 in c4;
fetch backward all in c4;
fetch backward 1 in c4;
fetch all in c4;

declare c5 cursor for select * from int8_tbl order by q1 fetch first 2 rows with ties;
fetch all in c5;
fetch 1 in c5;
fetch backward 1 in c5;
fetch backward 1 in c5;
fetch all in c5;
fetch backward all in c5;
fetch all in c5;
fetch backward all in c5;

rollback;

-- Stress test for variable LIMIT in conjunction with bounded-heap sorting

SELECT
  (SELECT n
     FROM (VALUES (1)) AS x,
          (SELECT n FROM generate_series(1,10) AS n
             ORDER BY n LIMIT 1 OFFSET s-1) AS y) AS z
  FROM generate_series(1,10) AS s;

--
-- Test that the LIMIT bound is pushed through the outer side of a left or
-- full join down to a Sort node.
--
create function explain_limit_bound(query text) returns setof text
language plpgsql as
$$
declare ln text;
begin
    for ln in
        execute 'explain (analyze, costs off, timing off, summary off, buffers off) '
                || query
    loop
        return next regexp_replace(ln, '\d+kB', 'NNkB', 'g');
    end loop;
end;
$$;

begin;

-- Force merge join to keep plans stable.
set local enable_hashjoin = off;
set local enable_nestloop = off;

-- the bound is pushed to the Sort: expect a top-N heapsort
select * from explain_limit_bound($$
  select o.tenthous, i.f1
  from (select * from onek order by tenthous) o
    left join int4_tbl i on o.tenthous = i.f1
  order by o.tenthous
  limit 3
$$);
select o.tenthous, i.f1
from (select * from onek order by tenthous) o
  left join int4_tbl i on o.tenthous = i.f1
order by o.tenthous
limit 3;

-- a join qual and a qual pushed down below the Sort don't prevent that
select * from explain_limit_bound($$
  select o.tenthous, i.f1
  from (select * from onek order by tenthous) o
    left join int4_tbl i on o.tenthous = i.f1 and i.f1 > 0
  where o.two = 0
  order by o.tenthous
  limit 3
$$);
select o.tenthous, i.f1
from (select * from onek order by tenthous) o
  left join int4_tbl i on o.tenthous = i.f1 and i.f1 > 0
where o.two = 0
order by o.tenthous
limit 3;

-- a qual applied to the join output prevents the top-N heapsort
select * from explain_limit_bound($$
  select o.tenthous, i.f1
  from (select * from onek order by tenthous) o
    left join int4_tbl i on o.tenthous = i.f1
  where i.f1 is null or i.f1 > 0
  limit 3
$$);

-- no pushdown for a left join reduced to an anti join
select * from explain_limit_bound($$
  select o.tenthous, i.f1
  from (select * from onek order by tenthous) o
    left join int4_tbl i on o.tenthous = i.f1
  where i.f1 is null
  limit 3
$$);

-- WITH TIES has no fixed bound to push
select * from explain_limit_bound($$
  select o.tenthous, i.f1
  from (select * from onek order by tenthous) o
    left join int4_tbl i on o.tenthous = i.f1
  order by o.tenthous fetch first 3 rows with ties
$$);

-- rescans with a changing bound must re-sort with the new one
select v.n, s.tenthous, s.f1
from (values (1), (3), (2)) v(n),
     lateral (select o.tenthous, i.f1
              from (select * from onek order by tenthous) o
                left join int4_tbl i on o.tenthous = i.f1
              order by o.tenthous
              limit v.n) s
order by v.n, s.tenthous;

-- a full join keeps every outer row too, so the bound reaches the outer Sort
select * from explain_limit_bound($$
  select o.tenthous, i.f1
  from (select * from onek order by tenthous) o
    full join int4_tbl i on o.tenthous = i.f1
  limit 3
$$);
select o.tenthous, i.f1
from (select * from onek order by tenthous) o
  full join int4_tbl i on o.tenthous = i.f1
limit 3;

-- the bound passes through a chain of left joins
select * from explain_limit_bound($$
  select o.tenthous, i.f1, j.f1
  from (select * from onek order by tenthous) o
    left join int4_tbl i on o.tenthous = i.f1
    left join int4_tbl j on o.tenthous = j.f1
  order by o.tenthous
  limit 3
$$);
select o.tenthous, i.f1, j.f1
from (select * from onek order by tenthous) o
  left join int4_tbl i on o.tenthous = i.f1
  left join int4_tbl j on o.tenthous = j.f1
order by o.tenthous
limit 3;

-- but stops at an inner join in the chain
select * from explain_limit_bound($$
  select o.tenthous, i.f1, j.f1
  from (select * from onek order by tenthous) o
    join int4_tbl i on o.tenthous = i.f1
    left join int4_tbl j on o.tenthous = j.f1
  limit 3
$$);

rollback;

drop function explain_limit_bound(text);

--
-- Test behavior of volatile and set-returning functions in conjunction
-- with ORDER BY and LIMIT.
--

create temp sequence testseq;

explain (verbose, costs off)
select unique1, unique2, nextval('testseq')
  from tenk1 order by unique2 limit 10;

select unique1, unique2, nextval('testseq')
  from tenk1 order by unique2 limit 10;

select currval('testseq');

explain (verbose, costs off)
select unique1, unique2, nextval('testseq')
  from tenk1 order by tenthous limit 10;

select unique1, unique2, nextval('testseq')
  from tenk1 order by tenthous limit 10;

select currval('testseq');

explain (verbose, costs off)
select unique1, unique2, generate_series(1,10)
  from tenk1 order by unique2 limit 7;

select unique1, unique2, generate_series(1,10)
  from tenk1 order by unique2 limit 7;

explain (verbose, costs off)
select unique1, unique2, generate_series(1,10)
  from tenk1 order by tenthous limit 7;

select unique1, unique2, generate_series(1,10)
  from tenk1 order by tenthous limit 7;

-- use of random() is to keep planner from folding the expressions together
explain (verbose, costs off)
select generate_series(0,2) as s1, generate_series((random()*.1)::int,2) as s2;

select generate_series(0,2) as s1, generate_series((random()*.1)::int,2) as s2;

explain (verbose, costs off)
select generate_series(0,2) as s1, generate_series((random()*.1)::int,2) as s2
order by s2 desc;

select generate_series(0,2) as s1, generate_series((random()*.1)::int,2) as s2
order by s2 desc;

-- test for failure to set all aggregates' aggtranstype
explain (verbose, costs off)
select sum(tenthous) as s1, sum(tenthous) + random()*0 as s2
  from tenk1 group by thousand order by thousand limit 3;

select sum(tenthous) as s1, sum(tenthous) + random()*0 as s2
  from tenk1 group by thousand order by thousand limit 3;

--
-- FETCH FIRST
-- Check the WITH TIES clause
--

SELECT  thousand
		FROM onek WHERE thousand < 5
		ORDER BY thousand FETCH FIRST 2 ROW WITH TIES;

SELECT  thousand
		FROM onek WHERE thousand < 5
		ORDER BY thousand FETCH FIRST ROWS WITH TIES;

SELECT  thousand
		FROM onek WHERE thousand < 5
		ORDER BY thousand FETCH FIRST 1 ROW WITH TIES;

SELECT  thousand
		FROM onek WHERE thousand < 5
		ORDER BY thousand FETCH FIRST 2 ROW ONLY;

-- SKIP LOCKED and WITH TIES are incompatible
SELECT  thousand
		FROM onek WHERE thousand < 5
		ORDER BY thousand FETCH FIRST 1 ROW WITH TIES FOR UPDATE SKIP LOCKED;

-- should fail
SELECT ''::text AS two, unique1, unique2, stringu1
		FROM onek WHERE unique1 > 50
		FETCH FIRST 2 ROW WITH TIES;

-- test ruleutils
CREATE VIEW limit_thousand_v_1 AS SELECT thousand FROM onek WHERE thousand < 995
		ORDER BY thousand FETCH FIRST 5 ROWS WITH TIES OFFSET 10;
\d+ limit_thousand_v_1
CREATE VIEW limit_thousand_v_2 AS SELECT thousand FROM onek WHERE thousand < 995
		ORDER BY thousand OFFSET 10 FETCH FIRST 5 ROWS ONLY;
\d+ limit_thousand_v_2
CREATE VIEW limit_thousand_v_3 AS SELECT thousand FROM onek WHERE thousand < 995
		ORDER BY thousand FETCH FIRST NULL ROWS WITH TIES;		-- fails
CREATE VIEW limit_thousand_v_3 AS SELECT thousand FROM onek WHERE thousand < 995
		ORDER BY thousand FETCH FIRST (NULL+1) ROWS WITH TIES;
\d+ limit_thousand_v_3
CREATE VIEW limit_thousand_v_4 AS SELECT thousand FROM onek WHERE thousand < 995
		ORDER BY thousand FETCH FIRST (5::bigint) ROWS WITH TIES;
\d+ limit_thousand_v_4
CREATE VIEW limit_thousand_v_5 AS SELECT thousand FROM onek WHERE thousand < 995
		ORDER BY thousand FETCH FIRST NULL ROWS ONLY;
\d+ limit_thousand_v_5
-- leave these views
