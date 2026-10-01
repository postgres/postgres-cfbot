--
-- Test GiST indexes.
--
-- There are other tests to test different GiST opclasses. This is for
-- testing GiST code itself. Vacuuming in particular.

create table gist_point_tbl(id int4, p point);
create index gist_pointidx on gist_point_tbl using gist(p);

-- Verify the fillfactor and buffering options
create index gist_pointidx2 on gist_point_tbl using gist(p) with (buffering = on, fillfactor=50);
create index gist_pointidx3 on gist_point_tbl using gist(p) with (buffering = off);
create index gist_pointidx4 on gist_point_tbl using gist(p) with (buffering = auto);
drop index gist_pointidx2, gist_pointidx3, gist_pointidx4;

-- Make sure bad values are refused
create index gist_pointidx5 on gist_point_tbl using gist(p) with (buffering = invalid_value);
create index gist_pointidx5 on gist_point_tbl using gist(p) with (fillfactor=9);
create index gist_pointidx5 on gist_point_tbl using gist(p) with (fillfactor=101);

-- Insert enough data to create a tree that's a couple of levels deep.
insert into gist_point_tbl (id, p)
select g,        point(g*10, g*10) from generate_series(1, 10000) g;

insert into gist_point_tbl (id, p)
select g+100000, point(g*10+1, g*10+1) from generate_series(1, 10000) g;

-- To test vacuum, delete some entries from all over the index.
delete from gist_point_tbl where id % 2 = 1;

-- And also delete some concentration of values.
delete from gist_point_tbl where id > 5000;

vacuum analyze gist_point_tbl;

-- rebuild the index with a different fillfactor
alter index gist_pointidx SET (fillfactor = 40);
reindex index gist_pointidx;

--
-- Test Index-only plans on GiST indexes
--

create table gist_tbl (b box, p point, c circle);

insert into gist_tbl
select box(point(0.05*i, 0.05*i), point(0.05*i, 0.05*i)),
       point(0.05*i, 0.05*i),
       circle(point(0.05*i, 0.05*i), 1.0)
from generate_series(0,10000) as i;

vacuum analyze gist_tbl;

set enable_seqscan=off;
set enable_bitmapscan=off;
set enable_indexonlyscan=on;

-- Test index-only scan with point opclass
create index gist_tbl_point_index on gist_tbl using gist (p);

-- check that the planner chooses an index-only scan
explain (costs off)
select p from gist_tbl where p <@ box(point(0,0), point(0.5, 0.5));

-- execute the same
select p from gist_tbl where p <@ box(point(0,0), point(0.5, 0.5));

-- Also test an index-only knn-search
explain (costs off)
select p from gist_tbl where p <@ box(point(0,0), point(0.5, 0.5))
order by p <-> point(0.201, 0.201);

select p from gist_tbl where p <@ box(point(0,0), point(0.5, 0.5))
order by p <-> point(0.201, 0.201);

-- Check commuted case as well
explain (costs off)
select p from gist_tbl where p <@ box(point(0,0), point(0.5, 0.5))
order by point(0.101, 0.101) <-> p;

select p from gist_tbl where p <@ box(point(0,0), point(0.5, 0.5))
order by point(0.101, 0.101) <-> p;

-- Check case with multiple rescans (bug #14641)
explain (costs off)
select p from
  (values (box(point(0,0), point(0.5,0.5))),
          (box(point(0.5,0.5), point(0.75,0.75))),
          (box(point(0.8,0.8), point(1.0,1.0)))) as v(bb)
cross join lateral
  (select p from gist_tbl where p <@ bb order by p <-> bb[0] limit 2) ss;

select p from
  (values (box(point(0,0), point(0.5,0.5))),
          (box(point(0.5,0.5), point(0.75,0.75))),
          (box(point(0.8,0.8), point(1.0,1.0)))) as v(bb)
cross join lateral
  (select p from gist_tbl where p <@ bb order by p <-> bb[0] limit 2) ss;

drop index gist_tbl_point_index;

-- Test index-only scan with box opclass
create index gist_tbl_box_index on gist_tbl using gist (b);

-- check that the planner chooses an index-only scan
explain (costs off)
select b from gist_tbl where b <@ box(point(5,5), point(6,6));

-- execute the same
select b from gist_tbl where b <@ box(point(5,5), point(6,6));

-- Also test an index-only knn-search
explain (costs off)
select b from gist_tbl where b <@ box(point(5,5), point(6,6))
order by b <-> point(5.2, 5.91);

select b from gist_tbl where b <@ box(point(5,5), point(6,6))
order by b <-> point(5.2, 5.91);

-- Check commuted case as well
explain (costs off)
select b from gist_tbl where b <@ box(point(5,5), point(6,6))
order by point(5.2, 5.91) <-> b;

select b from gist_tbl where b <@ box(point(5,5), point(6,6))
order by point(5.2, 5.91) <-> b;

drop index gist_tbl_box_index;

-- Test that an index-only scan is not chosen, when the query involves the
-- circle column (the circle opclass does not support index-only scans).
create index gist_tbl_multi_index on gist_tbl using gist (p, c);

explain (costs off)
select p, c from gist_tbl
where p <@ box(point(5,5), point(6, 6));

-- execute the same
select b, p from gist_tbl
where b <@ box(point(4.5, 4.5), point(5.5, 5.5))
and p <@ box(point(5,5), point(6, 6));

drop index gist_tbl_multi_index;

-- Test that we don't try to return the value of a non-returnable
-- column in an index-only scan.  (This isn't GIST-specific, but
-- it only applies to index AMs that can return some columns and not
-- others, so GIST with appropriate opclasses is a convenient test case.)
create index gist_tbl_multi_index on gist_tbl using gist (circle(p,1), p);
explain (verbose, costs off)
select circle(p,1) from gist_tbl
where p <@ box(point(5, 5), point(5.3, 5.3));
select circle(p,1) from gist_tbl
where p <@ box(point(5, 5), point(5.3, 5.3));

-- Similarly, test that index rechecks involving a non-returnable column
-- are done correctly.
explain (verbose, costs off)
select p from gist_tbl where circle(p,1) @> circle(point(0,0),0.95);
select p from gist_tbl where circle(p,1) @> circle(point(0,0),0.95);

-- Also check that use_physical_tlist doesn't trigger in such cases.
explain (verbose, costs off)
select count(*) from gist_tbl;
select count(*) from gist_tbl;

-- This case isn't supported, but it should at least EXPLAIN correctly.
explain (verbose, costs off)
select p from gist_tbl order by circle(p,1) <-> point(0,0) limit 1;
select p from gist_tbl order by circle(p,1) <-> point(0,0) limit 1;

drop index gist_tbl_multi_index;

-- Test that an ordering Index Scan returns the AM's ORDER BY values to a
-- targetlist entry equal to the ORDER BY expression, instead of evaluating
-- the expression again, for an opclass that passes amcanreturnorderby.
set enable_indexonlyscan = off;

-- An expression index over a counting function proves the operator is not
-- re-evaluated: the count stays at zero for the rows returned.
create sequence gist_cnt_seq;
create function gist_cnt_pt(point) returns point language plpgsql immutable
  as $$ begin perform nextval('public.gist_cnt_seq'); return $1; end $$;
create index gist_tbl_cnt_index on gist_tbl using gist (gist_cnt_pt(p));

explain (verbose, costs off)
select p, gist_cnt_pt(p) <-> point(0.201, 0.201) as dist
  from gist_tbl order by gist_cnt_pt(p) <-> point(0.201, 0.201) limit 3;

select setval('gist_cnt_seq', 1, false);
select p, gist_cnt_pt(p) <-> point(0.201, 0.201) as dist
  from gist_tbl order by gist_cnt_pt(p) <-> point(0.201, 0.201) limit 3;
select nextval('gist_cnt_seq') - 1 as calls_during_scan;

-- Every copy of the ORDER BY expression is served from the index; an
-- expression that merely contains it is not.
explain (verbose, costs off)
select gist_cnt_pt(p) <-> point(0,0) as d1,
       (gist_cnt_pt(p) <-> point(0,0)) * 2 as d2,
       gist_cnt_pt(p) <-> point(0,0) as d3
  from gist_tbl order by gist_cnt_pt(p) <-> point(0,0) limit 2;

drop index gist_tbl_cnt_index;
drop function gist_cnt_pt(point);
drop sequence gist_cnt_seq;

-- The box opclass computes its exact leaf distance with the operator's own
-- code, so it qualifies too.  The values must equal the operator's.
create index gist_tbl_box_index on gist_tbl using gist (b);

explain (verbose, costs off)
select b, b <-> point(5.2, 5.91) as dist from gist_tbl
  where b <@ box(point(5,5), point(6,6)) order by b <-> point(5.2, 5.91);

-- the distance numbers are not exactly the same across platforms
set extra_float_digits = 0;
select b, b <-> point(5.2, 5.91) as dist from gist_tbl
  where b <@ box(point(5,5), point(6,6)) order by b <-> point(5.2, 5.91);
reset extra_float_digits;

select count(*) filter (where dist = b <-> point(5.2, 5.91)) as same,
       count(*) as total
  from (select b, b <-> point(5.2, 5.91) as dist from gist_tbl
          order by b <-> point(5.2, 5.91) limit 200) ss;

-- Same, under row locking (the tuple is re-fetched by LockRows).
set extra_float_digits = 0;
select b, b <-> point(5.2, 5.91) as dist from gist_tbl
  where b <@ box(point(5,5), point(6,6)) order by b <-> point(5.2, 5.91)
  limit 3 for update;
reset extra_float_digits;

drop index gist_tbl_box_index;

-- The circle opclass's distance is a lower bound that is always rechecked,
-- and its opclass does not pass amcanreturnorderby: the targetlist is left
-- alone and the operator is evaluated as before.
create index gist_tbl_circle_index on gist_tbl using gist (c);

explain (verbose, costs off)
select c <-> point(5.2, 5.91) as dist from gist_tbl
  order by c <-> point(5.2, 5.91) limit 3;

select count(*) filter (where dist = c <-> point(5.2, 5.91)) as same,
       count(*) as total
  from (select c, c <-> point(5.2, 5.91) as dist from gist_tbl
          order by c <-> point(5.2, 5.91) limit 200) ss;

drop index gist_tbl_circle_index;
reset enable_indexonlyscan;

-- Test that an index-only scan deforms the tuple it reconstructs with the
-- descriptor the AM formed it with, not the scan slot's descriptor.
create temp table gist_ios_tupdesc (a inet, r numrange);

-- range_ops forms its tuples using the opclass input type, the polymorphic
-- anyrange (alignment 'd'), while the scan slot uses the actual range type
-- numrange (alignment 'i').  A buggy implementation will incorrectly access
-- the r/numrange column at the wrong offset.
--
-- The range bounds are made long so the value needs a four-byte varlena
-- header; shorter values get a one-byte header and are stored without
-- alignment padding, which would mask the problem.
insert into gist_ios_tupdesc
values (
        '::1', -- shifts "r" datum value to differing offset
        numrange(repeat('7', 200)::numeric, repeat('8', 200)::numeric));
create index on gist_ios_tupdesc using gist (a inet_ops, r);
vacuum analyze gist_ios_tupdesc;

explain (costs off)
select lower(r) = repeat('7', 200)::numeric as lower_ok,
       upper(r) = repeat('8', 200)::numeric as upper_ok
  from gist_ios_tupdesc where r && numrange(null, null);
select lower(r) = repeat('7', 200)::numeric as lower_ok,
       upper(r) = repeat('8', 200)::numeric as upper_ok
  from gist_ios_tupdesc where r && numrange(null, null);

drop table gist_ios_tupdesc;

-- test deletion of LP_DEAD-marked index tuples
create table gist_prune_tbl (k int, p point);
create index gist_prune_tbl_p_index on gist_prune_tbl using gist (p);

begin;
insert into gist_prune_tbl select i, point(1, i) from generate_series(1, 600) i;
rollback;

set enable_bitmapscan = off;
set enable_indexonlyscan = off;
set enable_seqscan = off;

select count(*) from gist_prune_tbl where p <@ box(point(0,0), point(2,1000));
insert into gist_prune_tbl select i, point(1, i) from generate_series(1, 600) i;
select count(*) from gist_prune_tbl where p <@ box(point(0,0), point(2,1000));

reset enable_bitmapscan;
reset enable_indexonlyscan;
reset enable_seqscan;
drop table gist_prune_tbl;

-- Force an index build using buffering.
create index gist_tbl_box_index_forcing_buffering on gist_tbl using gist (p)
  with (buffering=on, fillfactor=50);

-- Clean up
reset enable_seqscan;
reset enable_bitmapscan;
reset enable_indexonlyscan;

drop table gist_tbl;

-- test an unlogged table, mostly to get coverage of gistbuildempty
create unlogged table gist_tbl (b box);
create index gist_tbl_box_index on gist_tbl using gist (b);
insert into gist_tbl
  select box(point(0.05*i, 0.05*i)) from generate_series(0,10) as i;
drop table gist_tbl;
