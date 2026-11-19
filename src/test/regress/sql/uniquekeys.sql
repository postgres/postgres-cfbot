--
-- UNIQUEKEYS
--

create table uk_a (id int primary key, x int, y int);
create table uk_b (id int primary key, x int, y int);
create table uk_c (id int primary key, x int, y int);
create table uk_d (a int, b int, x int, primary key (a, b));

--
-- Test proving a join relation unique from the UniqueKeys of its base
-- relations.  We use a left join to make the join relation of interest the
-- inner side, and look at whether that join is marked as "Inner Unique".
--

set enable_hashjoin = off;
set enable_mergejoin = off;

-- the clause leaves one row of uk_b, which leaves one row of uk_c
explain (verbose, costs off)
select * from uk_a a left join
  (uk_b b join uk_c c on b.x = c.id) on a.x = b.id;

-- the clause leaves one row of uk_c, but that doesn't limit uk_b
explain (verbose, costs off)
select * from uk_a a left join
  (uk_b b join uk_c c on b.x = c.id) on a.x = c.id;

-- each relation has a clause of its own
explain (verbose, costs off)
select * from uk_a a left join
  (uk_b b cross join uk_c c) on a.x = b.id and a.y = c.id;

-- but a clause for only one of them is not enough
explain (verbose, costs off)
select * from uk_a a left join
  (uk_b b cross join uk_c c) on a.x = b.id;

-- a key column that is equal to a constant is not needed
explain (verbose, costs off)
select * from uk_a a left join
  (uk_d d join uk_c c on d.x = c.id) on a.x = d.b and d.a = 1;

-- a chain of three relations, the last one with a two-column key
explain (verbose, costs off)
select * from uk_a a left join
  (uk_b b join uk_c c on b.x = c.id join uk_d d on c.x = d.a and c.y = d.b)
  on a.x = b.id;

-- a left join's clause leaves one row of its nullable side.  Keep the
-- planner from rearranging the left joins, so that the join relation of
-- interest remains the inner side.
set join_collapse_limit = 1;

explain (verbose, costs off)
select * from uk_a a left join
  (uk_b b left join uk_c c on b.x = c.id) on a.x = b.id;

-- but not if it doesn't cover a key of the nullable side
explain (verbose, costs off)
select * from uk_a a left join
  (uk_b b left join uk_c c on b.x = c.x) on a.x = b.id;

reset join_collapse_limit;

-- the right-hand side of a semijoin or antijoin need not be unique
explain (verbose, costs off)
select * from uk_a a left join
  (select * from uk_b b
   where exists (select 1 from uk_c c where c.x = b.x)) s on a.x = s.id;

explain (verbose, costs off)
select * from uk_a a left join
  (select * from uk_b b
   where not exists (select 1 from uk_c c where c.x = b.x)) s on a.x = s.id;

-- but its left-hand side must be
explain (verbose, costs off)
select * from uk_a a left join
  (select * from uk_b b
   where exists (select 1 from uk_c c where c.x = b.x)) s on a.x = s.x;

reset enable_hashjoin;
reset enable_mergejoin;

-- A clause on a column that is nulled by an outer join works like one on the
-- column itself.  The join of interest is put under a full join, because a
-- left join's clause would let the planner reduce the outer join below it.
explain (verbose, costs off)
select * from uk_a a full join
  (uk_b b left join uk_c c on b.x = c.x) on a.x = b.id and a.y = c.id;

-- but not if the expression can be non-NULL in a null-extended row
explain (verbose, costs off)
select * from uk_a a full join
  (uk_b b left join uk_c c on b.x = c.x)
  on a.x = b.id and a.y = coalesce(c.id, 0);

-- a full join's clauses do not limit either of its sides
explain (verbose, costs off)
select * from uk_a a full join
  (uk_b b full join uk_c c on b.x = c.x) on a.x = b.id;

explain (verbose, costs off)
select * from uk_a a full join
  (uk_b b full join uk_c c on b.x = c.x) on a.x = b.id and a.y = c.id;

-- a relation nulled by a full join needs a clause of its own, even if it
-- has at most one row
explain (verbose, costs off)
select * from uk_c c left join
  ((select * from uk_a where id = 1) a full join
   (select * from uk_b where id = 1) b on a.x = b.x)
  on c.x = coalesce(a.y, b.y);

--
-- Test skipping DISTINCT and GROUP BY steps when the input is already
-- distinct
--

create table uk_n (id int primary key, u int unique, v int, w int);
create unique index on uk_n (v) nulls not distinct;

-- a single relation with its key among the DISTINCT columns
explain (costs off)
select distinct id, x from uk_a;

-- but not without the key
explain (costs off)
select distinct x, y from uk_a;

-- a key column that is equal to a constant is not needed
explain (costs off)
select distinct b, x from uk_d where a = 1;

-- a join needs the keys of all its relations
explain (costs off)
select distinct a.id, b.id from uk_a a join uk_b b on a.x = b.x;

explain (costs off)
select distinct a.id, b.x from uk_a a join uk_b b on a.x = b.x;

-- unless the join clauses supply them
explain (costs off)
select distinct a.id, a.y from uk_a a join uk_b b on a.x = b.id;

-- a unique index on a nullable column allows duplicate NULLs
explain (costs off)
select distinct u from uk_n;

-- unless a strict restriction clause filters them out
explain (costs off)
select distinct u from uk_n where u > 0;

-- or the index is NULLS NOT DISTINCT
explain (costs off)
select distinct v from uk_n;

-- a column that a join clause equates to something cannot be NULL
explain (costs off)
select distinct a.id, n.w from uk_a a join uk_n n on a.x = n.u;

explain (costs off)
select distinct a.id, n.w from uk_a a left join uk_n n on a.x = n.u;

-- but that takes a join clause on each column of the key
explain (costs off)
select distinct a.id, n.u from uk_a a join uk_n n on a.x = n.w;

-- outer joins
explain (costs off)
select distinct a.id, b.y from uk_a a left join uk_b b on a.x = b.id;

explain (costs off)
select distinct a.id, b.y from uk_a a left join uk_b b on a.x = b.x;

explain (costs off)
select distinct a.id, b.id from uk_a a left join uk_b b on a.x = b.x;

explain (costs off)
select distinct a.id, b.id from uk_a a full join uk_b b on a.x = b.x;

-- a left join's null-extended row cannot be mistaken for a row that has a
-- NULL in its key
explain (costs off)
select distinct a.id, n.v from uk_a a left join uk_n n on a.x = n.w;

-- but a full join's can
explain (costs off)
select distinct n1.v, n2.v from uk_n n1 full join uk_n n2 on n1.w = n2.w;

-- unless a NOT NULL column tells them apart
explain (costs off)
select distinct n1.v, n1.id, n2.v, n2.id
from uk_n n1 full join uk_n n2 on n1.w = n2.w;

-- or a strict restriction clause filters out the NULLs
explain (costs off)
select distinct n1.v, n2.v
from (select * from uk_n where v > 0) n1 full join
  (select * from uk_n where v > 0) n2 on n1.w = n2.w;

-- semijoins and antijoins
explain (costs off)
select distinct b.id from uk_b b
where exists (select 1 from uk_c c where c.x = b.x);

explain (costs off)
select distinct b.id from uk_b b
where not exists (select 1 from uk_c c where c.x = b.x);

explain (costs off)
select distinct b.x from uk_b b
where exists (select 1 from uk_c c where c.x = b.x);

-- GROUP BY without aggregates is handled likewise
explain (costs off)
select a.id, a.y from uk_a a join uk_b b on a.x = b.id group by a.id, a.y;

-- but aggregates need the grouping step
explain (costs off)
select id, count(*) from uk_a group by id;

-- as does DISTINCT ON, and set-returning functions in the targetlist
explain (costs off)
select distinct on (id) id, x from uk_a;

explain (costs off)
select distinct id, generate_series(1, 2) from uk_a;

-- DISTINCT over grouped input needs all the grouping columns
explain (costs off)
select distinct x, count(*) from uk_a group by x;

explain (costs off)
select distinct x, count(*) from uk_a group by x, y;

-- or grouping columns that the scan/join relation is distinct for
explain (costs off)
select distinct a.id, count(*) from uk_a a join uk_b b on a.x = b.id
group by a.id, b.y;

explain (costs off)
select distinct b.id, count(*) from uk_a a join uk_b b on a.x = b.id
group by a.id, b.id;

-- unless there are grouping sets
explain (costs off)
select distinct x, count(*) from uk_a group by grouping sets ((x), ());

-- aggregation without GROUP BY returns one row
explain (costs off)
select distinct count(*) from uk_a;

-- window functions do not change the rows of their input
explain (costs off)
select distinct id, sum(x) over (partition by y) from uk_a;

explain (costs off)
select distinct x, sum(x) over (partition by y) from uk_a;

--
-- Test child relations under partitionwise join.
--

create table uk_p (id int primary key, x int, y int) partition by range (id);
create table uk_p1 partition of uk_p for values from (0) to (100);
create table uk_p2 partition of uk_p for values from (100) to (200);
create table uk_q (id int primary key, x int, y int) partition by range (id);
create table uk_q1 partition of uk_q for values from (0) to (100);
create table uk_q2 partition of uk_q for values from (100) to (200);
create table uk_r (id int, x int, y int) partition by range (id);
create table uk_r1 partition of uk_r for values from (0) to (100);
create table uk_r2 partition of uk_r for values from (100) to (200);

-- Disable hash and merge joins, so that the nullable side of a left join is
-- the inner side.
set enable_partitionwise_join = on;
set max_parallel_workers_per_gather = 0;
set enable_hashjoin = off;
set enable_mergejoin = off;

-- a child relation is unique through its own index
explain (verbose, costs off)
select * from uk_p p join uk_q q on p.id = q.id;

-- a child join is unique if its parent is
explain (verbose, costs off)
select * from uk_p p left join
  (uk_q q join uk_p p2 on q.id = p2.id) on p.id = q.id;

-- but not if its parent is not
explain (verbose, costs off)
select * from uk_p p left join
  (uk_q q join uk_r r on q.id = r.id) on p.id = q.id;

reset enable_partitionwise_join;
reset max_parallel_workers_per_gather;
reset enable_hashjoin;
reset enable_mergejoin;

--
-- Test relations that are partially aggregated by eager aggregation.
--

create table uk_ea1 (id int primary key, x int);
create table uk_ea2 (id int, y int, z int);
insert into uk_ea1 select i, i % 10 from generate_series(1, 1000) i;
insert into uk_ea2 select i % 100, i, i % 3 from generate_series(1, 1000) i;
analyze uk_ea1, uk_ea2;

-- uk_ea2 is grouped by the join column
explain (verbose, costs off)
select a.x, sum(b.y) from uk_ea1 a join uk_ea2 b on a.id = b.id
group by a.x;

-- uk_ea2 is grouped by more than the join column
explain (verbose, costs off)
select a.x, b.z, sum(b.y) from uk_ea1 a join uk_ea2 b on a.id = b.id
group by a.x, b.z;

drop table uk_ea1, uk_ea2;

drop table uk_a, uk_b, uk_c, uk_d, uk_n, uk_p, uk_q, uk_r;
