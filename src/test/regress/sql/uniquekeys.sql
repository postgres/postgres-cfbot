--
-- UNIQUEKEYS
--
-- Tests for the planner's inference of the expression sets a relation is
-- distinct over.  UniqueKeys are not printed anywhere, so each test observes
-- them through one of the consumers that acts on them:
--
--	* a redundant DISTINCT step is dropped
--	* a redundant GROUP BY step is dropped
--	* a join is marked "Inner Unique: true"
--	* a semijoin's RHS is not unique-ified
--

-- one-column and two-column primary keys
create table uk_p (id int primary key, v int);
create table uk_q (id int primary key, v int);
create table uk_r (id int primary key, v int);
create table uk_pk2 (a int, b int, c int, primary key (a, b));
-- a unique column that admits NULLs, and one that does not
create table uk_nul (u int unique, w int);
create table uk_nnd (u int, w int);
create unique index uk_nnd_u on uk_nnd (u) nulls not distinct;
-- text, whose '=' belongs to both text_ops and text_pattern_ops
create table uk_txt (t text primary key, v int);
-- indexes that cannot prove anything
create table uk_defer (id int, primary key (id) deferrable);
create table uk_part (a int, b int);
create unique index uk_part_a on uk_part (a) where b > 0;

--
-- Base relation keys
--
-- the whole primary key
explain (costs off) select distinct a, b from uk_pk2;
-- a superset of it
explain (costs off) select distinct a, b, c from uk_pk2;
-- a subset is not enough
explain (costs off) select distinct a from uk_pk2;
-- a key column equated to a constant drops out of the key
explain (costs off) select distinct b from uk_pk2 where a = 5;
-- the key of a text column, whose '=' belongs to more than one opfamily
explain (costs off) select distinct t from uk_txt;
-- a deferrable constraint is not enforced row by row
explain (costs off) select distinct id from uk_defer;
-- a partial index does not cover the whole relation
explain (costs off) select distinct a from uk_part where b > 0;

--
-- NULL-awareness of base relation keys
--
-- two NULLs are distinct to a unique index but equal to DISTINCT, so a
-- nullable unique column is not a NULL-aware key on its own
explain (costs off) select distinct u from uk_nul;
-- a strict restriction clause rules the NULLs out
explain (costs off) select distinct u from uk_nul where u > 0;
-- so does declaring the index NULLS NOT DISTINCT
explain (costs off) select distinct u from uk_nnd;

--
-- Subquery-in-FROM keys
--
-- a DISTINCT inside is translated into the outer query
explain (costs off) select distinct a, b from (select distinct a, b from uk_pk2) s;
-- likewise a GROUP BY
explain (costs off) select distinct a, b from (select a, b from uk_pk2 group by a, b) s;
-- a plain aggregate emits a single row
explain (costs off) select distinct m from (select max(a) as m from uk_pk2) s;
-- a key deduced inside the subquery's own join search
explain (costs off)
select distinct x from
  (select uk_p.id as x from uk_p join uk_q on uk_p.id = uk_q.id offset 0) s;
-- sorting and LIMIT do not add rows, so the key survives them
explain (costs off)
select distinct x from (select distinct id as x from uk_p order by 1 limit 3) s;
-- neither does window function evaluation
explain (costs off)
select distinct x from
  (select distinct id as x, row_number() over (order by v) as rn from uk_p) s;
-- but a target-list SRF multiplies rows, and voids the key
explain (costs off)
select distinct x from (select id as x, generate_series(1, 2) from uk_p) s;

--
-- Set operation keys
--
-- a non-ALL set operation is distinct over its whole output row
explain (costs off)
select distinct s.a, s.b from
  (select a, b from uk_pk2 union select a, b from uk_pk2) s join uk_p on s.a = uk_p.id;
-- which is not true of UNION ALL
explain (costs off)
select distinct s.a, s.b from
  (select a, b from uk_pk2 union all select a, b from uk_pk2) s join uk_p on s.a = uk_p.id;
-- INTERSECT and EXCEPT deduplicate too
explain (costs off)
select distinct s.a, s.b from
  (select a, b from uk_pk2 intersect select a, b from uk_pk2) s join uk_p on s.a = uk_p.id;
explain (costs off)
select distinct s.a, s.b from
  (select a, b from uk_pk2 except select a, b from uk_pk2) s join uk_p on s.a = uk_p.id;

--
-- Join relation keys
--
-- Preservation: uk_q is unique for the clause, so each uk_p row yields one row
explain (costs off) select distinct uk_p.id from uk_p join uk_q on uk_p.id = uk_q.id;
-- Combination: the union of a key from each side
explain (costs off) select distinct uk_p.id, uk_q.id from uk_p cross join uk_q;
-- one side's key alone is not a key of a cross join
explain (costs off) select distinct uk_p.id from uk_p cross join uk_q;
-- a left join preserves the LHS key
explain (costs off)
select distinct uk_p.id, uk_q.v from uk_p left join uk_q on uk_p.id = uk_q.id;
-- but the RHS key loses its NULL-awareness across one
explain (costs off)
select distinct uk_q.id from uk_p left join uk_q on uk_p.id = uk_q.id;
-- a semijoin's output is a subset of its LHS
explain (costs off)
select distinct a, b from uk_pk2 where exists (select 1 from uk_p where uk_p.id = uk_pk2.c);
-- and so is an antijoin's
explain (costs off)
select distinct a, b from uk_pk2 where not exists (select 1 from uk_p where uk_p.id = uk_pk2.c);
-- a full join null-extends both sides, so nothing survives
explain (costs off)
select distinct uk_p.id from uk_p full join uk_q on uk_p.id = uk_q.id;
-- an inner join's strict clause restores NULL-awareness to a key
explain (costs off) select distinct u from uk_nul join uk_p on uk_nul.u = uk_p.id;
-- but only when the clause covers the key column
explain (costs off) select distinct u from uk_nul join uk_p on uk_nul.w = uk_p.id;
-- a semijoin's clauses hold of its output too, so they restore it as well
explain (costs off)
select distinct u from uk_nul where exists (select 1 from uk_pk2 where uk_pk2.c = uk_nul.u);
-- an antijoin's do not: its output is the rows they found no match for, which
-- is where the NULL u values are
explain (costs off)
select distinct u from uk_nul where not exists (select 1 from uk_pk2 where uk_pk2.c = uk_nul.u);
-- a key survives stacked outer joins
explain (costs off)
select distinct uk_p.id, y.v from uk_p
  left join uk_q x on uk_p.id = x.id left join uk_r y on x.id = y.id;

--
-- Upper relation keys
--
-- grouping by a superset of a key, with no aggregates, is a no-op
explain (costs off) select a, b from uk_pk2 group by a, b, c;
-- grouping by a non-key set is not
explain (costs off) select a, c from uk_pk2 group by a, c;
-- nor is it when there is aggregation to do
explain (costs off) select a, b, count(*) from uk_pk2 group by a, b;
-- grouping sets emit one row per set
explain (costs off) select a, b from uk_pk2 group by grouping sets ((a, b), (a));

--
-- Consumers other than DISTINCT
--
-- inner_unique proven from a joinrel's key rather than a base relation's
explain (verbose, costs off)
select uk_p.v from uk_p left join (uk_q join uk_r on uk_q.id = uk_r.id) on uk_p.v = uk_q.id;
-- the RHS of this semijoin is already distinct, so it is not unique-ified
explain (costs off)
select * from uk_p, uk_q where (uk_p.id, uk_q.id) in (select a, b from uk_pk2);
-- whereas this one is not, so it is
explain (costs off)
select * from uk_p, uk_q where (uk_p.id, uk_q.id) in (select c, c from uk_pk2);

--
-- Appendrel children
--
-- A child emits a subset of its parent's rows, so it inherits the parent's
-- keys.  A partitioned table's unique index must include the partition key,
-- so the parent has keys to inherit; an inheritance parent has none.
--
create table uk_pt1 (a int, b int, primary key (a)) partition by range (a);
create table uk_pt1a partition of uk_pt1 for values from (0) to (10);
create table uk_pt1b partition of uk_pt1 for values from (10) to (20);
create table uk_pt2 (a int, b int, primary key (a)) partition by range (a);
create table uk_pt2a partition of uk_pt2 for values from (0) to (10);
create table uk_pt2b partition of uk_pt2 for values from (10) to (20);
create table uk_pt3 (a int, b int, primary key (a)) partition by range (a);
create table uk_pt3a partition of uk_pt3 for values from (0) to (10);
create table uk_pt3b partition of uk_pt3 for values from (10) to (20);
create table uk_inh (id int primary key, v int);
create table uk_inh_c () inherits (uk_inh);

-- the partitioned parent's own key removes the DISTINCT
explain (costs off) select distinct a from uk_pt1;
-- an inheritance parent has no key: a child row may duplicate a parent one
explain (costs off) select distinct id from uk_inh;

set enable_partitionwise_join to on;
set enable_hashjoin to off;
-- each child join is inner-unique, proven from the child base rel's key
explain (verbose, costs off)
select uk_pt1.b from uk_pt1 join uk_pt2 on uk_pt1.a = uk_pt2.a;
-- here the outer join's inner side is itself a child joinrel, whose key is
-- inherited from the parent joinrel
explain (verbose, costs off)
select uk_pt1.b from uk_pt1 left join (uk_pt2 join uk_pt3 on uk_pt2.a = uk_pt3.a)
  on uk_pt1.a = uk_pt2.a;
reset enable_hashjoin;
reset enable_partitionwise_join;

--
-- Eager aggregation
--
-- Partial aggregation keeps one row per group, so the grouped relation is
-- distinct over its grouping expressions, which can prove a join above it
-- inner-unique.
--
create table uk_ea1 (id int primary key, x int, val int);
create table uk_ea2 (id int, y int);
insert into uk_ea1 select i, i % 10, i from generate_series(1, 1000) i;
insert into uk_ea2 select i % 100, i from generate_series(1, 1000) i;
analyze uk_ea1, uk_ea2;

-- uk_ea2 is grouped by the join column, so the join sees a unique inner side
explain (verbose, costs off)
select uk_ea1.x, sum(uk_ea2.y) from uk_ea1 join uk_ea2 on uk_ea1.id = uk_ea2.id
  group by uk_ea1.x order by 1;
select uk_ea1.x, sum(uk_ea2.y) from uk_ea1 join uk_ea2 on uk_ea1.id = uk_ea2.id
  group by uk_ea1.x order by 1;

drop table uk_ea1, uk_ea2;

--
-- Result correctness: steps that must not be removed
--
-- Cases where an over-strong key would drop a step that is really needed, and
-- so return duplicate rows.
--
create table uk_d1 (u int unique, w int);
create table uk_d2 (id int primary key, v int);
insert into uk_d1 values (null, 1), (null, 1), (1, 2);
insert into uk_d2 values (1, 10), (2, 20);

-- two NULL rows are distinct to the unique index but equal to DISTINCT
explain (costs off) select distinct u, w from uk_d1;
select distinct u, w from uk_d1 order by u, w;

-- across a left join the RHS key covers only the matched rows
explain (costs off) select distinct uk_d1.u from uk_d2 left join uk_d1 on uk_d2.id = uk_d1.u;
select distinct uk_d1.u from uk_d2 left join uk_d1 on uk_d2.id = uk_d1.u order by 1;

-- a target-list SRF duplicates the subquery's rows
explain (costs off)
select distinct x from (select id as x, generate_series(1, 2) from uk_d2) s;
select distinct x from (select id as x, generate_series(1, 2) from uk_d2) s order by 1;

-- UNION ALL does not deduplicate
explain (costs off)
select distinct s.id from (select id from uk_d2 union all select id from uk_d2) s;
select distinct s.id from (select id from uk_d2 union all select id from uk_d2) s order by 1;

--
-- Result correctness: steps that are removed
--
-- Removing a step replaces it with a bare projection, so these check that the
-- rewritten paths still emit the right rows and compute the right values.
--
create table uk_e1 (a int, b int, c int, primary key (a, b));
create table uk_e2 (id int primary key, v int);
create table uk_e3 (x int, y int);
insert into uk_e1 values (1, 1, 10), (1, 2, 20), (2, 1, 30);
insert into uk_e2 values (1, 1), (2, 1), (3, 300);
insert into uk_e3 values (1, 1), (1, 1), (2, 1), (9, 9);

-- DISTINCT over a superset of the key, projecting a computed column
explain (costs off) select distinct a, b, c * 10 from uk_e1;
select distinct a, b, c * 10 from uk_e1 order by a, b;

-- the remaining key columns still determine the row
explain (costs off) select distinct b, c from uk_e1 where a = 1;
select distinct b, c from uk_e1 where a = 1 order by b;

-- the same, for a grouping step that groups nothing
explain (costs off) select a, b, c * 10 from uk_e1 group by a, b, c;
select a, b, c * 10 from uk_e1 group by a, b, c order by a, b;

-- DISTINCT removed above a join, which must still not collapse uk_e2.v
explain (costs off)
select distinct uk_e1.a, uk_e1.b, uk_e2.v from uk_e2 join uk_e1 on uk_e2.id = uk_e1.a;
select distinct uk_e1.a, uk_e1.b, uk_e2.v from uk_e2 join uk_e1 on uk_e2.id = uk_e1.a
  order by 1, 2;

-- unique-ification skipped, but the semijoin must still keep both (1,1) rows
explain (costs off) select * from uk_e3 where (x, y) in (select a, b from uk_e1);
select * from uk_e3 where (x, y) in (select a, b from uk_e1) order by 1, 2;

-- a join marked inner-unique must not stop short of any matching row
explain (verbose, costs off)
select uk_e2.id, uk_e2.v from uk_e2 join uk_e1 on uk_e2.id = uk_e1.a and uk_e2.v = uk_e1.b;
select uk_e2.id, uk_e2.v from uk_e2 join uk_e1 on uk_e2.id = uk_e1.a and uk_e2.v = uk_e1.b
  order by 1;

drop table uk_p, uk_q, uk_r, uk_pk2, uk_nul, uk_nnd, uk_txt;
drop table uk_defer, uk_part, uk_d1, uk_d2, uk_e1, uk_e2, uk_e3;
drop table uk_pt1, uk_pt2, uk_pt3;
drop table uk_inh cascade;
