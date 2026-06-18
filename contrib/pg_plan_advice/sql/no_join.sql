LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;

CREATE TABLE no_join_dim (id serial primary key, dim text)
	WITH (autovacuum_enabled = false);
INSERT INTO no_join_dim (dim) SELECT random()::text FROM generate_series(1,100) g;
VACUUM ANALYZE no_join_dim;

CREATE TABLE no_join_fact (
	id int primary key,
	dim_id integer not null references no_join_dim (id)
) WITH (autovacuum_enabled = false);
INSERT INTO no_join_fact
	SELECT g, (g%3)+1 FROM generate_series(1,100000) g;
CREATE INDEX no_join_fact_dim_id ON no_join_fact (dim_id);
VACUUM ANALYZE no_join_fact;

-- Baseline: hash join with d on the inner side.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;

-- NO_HASH_JOIN(d): d stays inner, planner falls back to another method.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_HASH_JOIN(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

-- NO_HASH_JOIN(f): the join-order constraint forces f to the inner side.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_HASH_JOIN(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

-- NO_NESTED_LOOP(f): f forced inner, all NL variants forbidden -> merge join.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_NESTED_LOOP(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

-- NO_MERGE_JOIN(d): d already inner, hash join still allowed -> plan unchanged.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_MERGE_JOIN(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

-- Stacking: NO_HASH_JOIN(f) + NO_NESTED_LOOP(f) leaves only merge join.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_HASH_JOIN(f) NO_NESTED_LOOP(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

-- Specific NO_ variants targeting d, already the inner side: forbidding any one
-- NL or merge variant leaves hash join available, so the plan is unchanged.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_NESTED_LOOP_PLAIN(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NO_NESTED_LOOP_MEMOIZE(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NO_NESTED_LOOP_MATERIALIZE(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NO_MERGE_JOIN_PLAIN(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
SET LOCAL pg_plan_advice.advice = 'NO_MERGE_JOIN_MATERIALIZE(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

-- Conflict: same method required and forbidden; both marked conflicting.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'HASH_JOIN(f) NO_HASH_JOIN(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

-- No conflict: NO_HASH_JOIN(f) and a positive tag for a different method.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_HASH_JOIN(f) NESTED_LOOP_PLAIN(f)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

-- A conflict on one method must not cancel enforcement of an unrelated NO_
-- tag: MERGE_JOIN_PLAIN(d) conflicts with NO_MERGE_JOIN(d) here, but
-- NO_HASH_JOIN(d) does not, so it still applies.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'MERGE_JOIN_PLAIN(d) NO_MERGE_JOIN(d) NO_HASH_JOIN(d)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_join_fact f JOIN no_join_dim d ON f.dim_id = d.id;
COMMIT;

DROP TABLE no_join_fact;
DROP TABLE no_join_dim;
