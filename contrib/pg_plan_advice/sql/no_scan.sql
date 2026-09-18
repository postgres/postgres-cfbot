LOAD 'pg_plan_advice';
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 0.1;
SET random_page_cost = 0.1;
SET cpu_tuple_cost = 0;
SET cpu_index_tuple_cost = 0;

CREATE TABLE no_scan_table (a int primary key, b text)
	WITH (autovacuum_enabled = false);
INSERT INTO no_scan_table
	SELECT g, 'some text ' || g FROM generate_series(1, 100000) g;
VACUUM ANALYZE no_scan_table;

-- Baselines without advice: seq scan for a full read, index scan for a lookup.
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table;
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE a = 1;

-- NO_SEQ_SCAN with no indexable qual: no alternative, so the advice fails.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_SEQ_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table;
COMMIT;

-- NO_SEQ_SCAN with an indexable qual: falls back to index scan.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_SEQ_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE a = 1;
COMMIT;

-- NO_INDEX_SCAN: falls back to bitmap heap scan.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_INDEX_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE a = 1;
COMMIT;

-- NO_INDEX_ONLY_SCAN: falls back to a plain index scan.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_INDEX_ONLY_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT a FROM no_scan_table WHERE a = 1;
COMMIT;

-- NO_BITMAP_HEAP_SCAN: with only a BRIN index available, falls back to seq scan.
CREATE INDEX no_scan_table_b ON no_scan_table USING brin (b);
VACUUM ANALYZE no_scan_table;
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_BITMAP_HEAP_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE b > 'some text 8';
COMMIT;

-- NO_TID_SCAN: falls back to seq scan.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_TID_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE ctid = '(0,1)';
COMMIT;

-- Multiple NO_ scan tags stack without conflict.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_SEQ_SCAN(no_scan_table) NO_BITMAP_HEAP_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE a = 1;
COMMIT;

-- Conflict: same method required and forbidden; both conflicting, neither applied.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(no_scan_table) NO_SEQ_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table;
COMMIT;

-- No conflict: NO_SEQ_SCAN and a positive INDEX_SCAN are compatible.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'NO_SEQ_SCAN(no_scan_table) INDEX_SCAN(no_scan_table no_scan_table_pkey)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE a = 1;
COMMIT;

-- No conflict: BITMAP_HEAP_SCAN and NO_INDEX_ONLY_SCAN only share the
-- PGS_CONSIDER_INDEXONLY enforcement bit, not an actual scan method.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'BITMAP_HEAP_SCAN(no_scan_table) NO_INDEX_ONLY_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE b > 'some text 8';
COMMIT;

-- A conflict on one method must not cancel enforcement of an unrelated NO_
-- tag: NO_SEQ_SCAN conflicts with SEQ_SCAN here, but NO_BITMAP_HEAP_SCAN
-- does not, so it still applies.
BEGIN;
SET LOCAL pg_plan_advice.advice = 'SEQ_SCAN(no_scan_table) NO_SEQ_SCAN(no_scan_table) NO_BITMAP_HEAP_SCAN(no_scan_table)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_table WHERE b > 'some text 8';
COMMIT;

-- A conflicting INDEX_SCAN request must not have the side effect of
-- disabling every other index: with two equivalent indexes, an
-- INDEX_SCAN/NO_INDEX_SCAN conflict on the relation must leave the
-- planner's natural index choice alone.
CREATE TABLE no_scan_idx_conflict (a int)
	WITH (autovacuum_enabled = false);
INSERT INTO no_scan_idx_conflict SELECT g FROM generate_series(1, 100000) g;
CREATE INDEX no_scan_idx_conflict_a1 ON no_scan_idx_conflict (a);
CREATE INDEX no_scan_idx_conflict_a2 ON no_scan_idx_conflict (a);
VACUUM ANALYZE no_scan_idx_conflict;

-- Baseline: which index does the planner naturally pick?
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_idx_conflict ORDER BY a LIMIT 5;

-- INDEX_SCAN names the other index, and NO_INDEX_SCAN forbids index scans
-- entirely on the same relation, so the two conflict; neither should have
-- any effect, and in particular the un-named index must not be disabled.
BEGIN;
SET LOCAL pg_plan_advice.advice =
	'INDEX_SCAN(no_scan_idx_conflict no_scan_idx_conflict_a1) NO_INDEX_SCAN(no_scan_idx_conflict)';
EXPLAIN (COSTS OFF, PLAN_ADVICE)
	SELECT * FROM no_scan_idx_conflict ORDER BY a LIMIT 5;
COMMIT;

DROP TABLE no_scan_idx_conflict;

-- Scan tags forbid sublists (simple_target_list): this is a syntax error.
SET pg_plan_advice.advice = 'NO_SEQ_SCAN((no_scan_table no_scan_table))';

DROP TABLE no_scan_table;
