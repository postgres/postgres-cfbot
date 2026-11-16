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

drop table uk_a, uk_b, uk_c, uk_d;
