--
-- Planner extension state must not be shared between a RelOptInfo and the
-- RelOptInfos that the planner builds by flat-copying it.
--
CREATE EXTENSION test_extendplan;

-- The first call loads the module, which installs its planner hooks.
SELECT test_extendplan_reset();

CREATE TABLE extendplan_t1 (a int, b int, c double precision);
CREATE TABLE extendplan_t2 (a int, b int, c double precision);
INSERT INTO extendplan_t1 SELECT i, i, i FROM generate_series(1, 1000) i;
INSERT INTO extendplan_t2 SELECT i, i % 10, i FROM generate_series(1, 1000) i;
ANALYZE extendplan_t1, extendplan_t2;

-- Eager aggregation copies base and join rels to build grouped rels.
SET enable_eager_aggregate = on;
SELECT test_extendplan_reset();
SELECT t1.a, avg(t2.c)
  FROM extendplan_t1 t1 JOIN extendplan_t2 t2 ON t1.b = t2.b
 GROUP BY t1.a ORDER BY t1.a;
SELECT * FROM test_extendplan_summary();
RESET enable_eager_aggregate;

-- A semijoin can unique-ify its inner rel, which copies that rel.
SELECT test_extendplan_reset();
SELECT count(*) FROM extendplan_t1 t1
 WHERE t1.b IN (SELECT t2.b FROM extendplan_t2 t2);
SELECT * FROM test_extendplan_summary();

DROP TABLE extendplan_t1, extendplan_t2;
DROP EXTENSION test_extendplan;
