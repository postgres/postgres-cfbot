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

reset enable_hashjoin;
reset enable_mergejoin;

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

-- but that takes a join clause on each column of the key
explain (costs off)
select distinct a.id, n.u from uk_a a join uk_n n on a.x = n.w;

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

drop table uk_a, uk_b, uk_c, uk_d, uk_n;
