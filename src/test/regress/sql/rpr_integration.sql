-- ============================================================
-- RPR Integration Tests
-- Planner optimization interaction tests for Row Pattern Recognition
-- ============================================================
--
-- Verifies that each planner optimization correctly handles RPR windows.
-- Even if individual optimizations are tested elsewhere, this file
-- provides a single checkpoint for all planner/RPR interactions.
--
-- The file drops what it creates, with one exception: A3 leaves the view
-- rpr_ev_opt_mixed behind, so that pg_upgrade and pg_dump exercise an RPR
-- window serialized together with a non-RPR one.
--
-- A. Planner Optimization Protection Tests
--    A1. Frame optimization bypass
--    A2. Run condition pushdown bypass
--    A3. Window dedup prevention (RPR vs non-RPR)
--    A4. Window dedup prevention (same PATTERN, different DEFINE)
--    A5. Unused window removal prevention
--    A6. Inverse transition bypass
--    A7. Cost estimation RPR awareness
--    A8. Subquery flattening prevention
--    A9. DEFINE expression non-propagation
--    A10. RPR + LIMIT
--
-- B. Integration Scenario Tests
--    B1. RPR + CTE
--    B2. RPR + JOIN
--    B3. RPR + Set operations
--    B4. RPR + Prepared statements
--    B5. RPR + Partitioned table
--    B6. RPR + LATERAL
--    B7. RPR + Recursive CTE
--    B8. RPR + Incremental sort
--    B9. RPR + Volatile function in DEFINE
--    B10. RPR + Correlated subquery in WHERE
--    B11. RPR + Junk targetlist pruning
--    B12. RPR + Correlated navigation offsets
--    B13. RPR + DEFINE-only parameter caching
--    B14. RPR + Multiple window definitions
--

CREATE TABLE rpr_integ (id INT, val INT);
INSERT INTO rpr_integ VALUES
    (1, 10), (2, 20), (3, 15), (4, 25), (5, 5),
    (6, 30), (7, 35), (8, 20), (9, 40), (10, 45);

-- ============================================================
-- A1. Frame optimization bypass
-- ============================================================
-- Verify that optimize_window_clauses() does not apply frame
-- optimization to RPR windows.  Both queries below use the same input
-- frame (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) with
-- row_number(), whose prosupport handles
-- SupportRequestOptimizeWindowClause and triggers frame rewriting.
-- In the non-RPR baseline the planner rewrites the frame to ROWS
-- UNBOUNDED PRECEDING, while in the RPR case the guard in
-- optimize_window_clauses() blocks the rewrite and the frame is
-- preserved as specified.
-- Affected functions: row_number, rank, dense_rank, percent_rank,
-- cume_dist, ntile.  All would change the frame to ROWS UNBOUNDED
-- PRECEDING, breaking RPR's required ROWS BETWEEN CURRENT ROW AND
-- UNBOUNDED FOLLOWING.

-- Non-RPR baseline: the planner rewrites the frame to ROWS UNBOUNDED PRECEDING.
EXPLAIN (COSTS OFF)
SELECT row_number() OVER w FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING);

-- RPR case: the frame is preserved as specified.
EXPLAIN (COSTS OFF)
SELECT row_number() OVER w FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val));

-- ============================================================
-- A2. Run condition pushdown bypass
-- ============================================================
-- Verify that find_window_run_conditions() does not push a monotonic
-- filter down as a Run Condition on RPR windows.  RPR match counts are
-- determined by pattern matching rather than by a monotonic
-- accumulation over the frame, so a filter such as "cnt > 0" cannot be
-- used to stop evaluating the window function early.
-- With RPR's required frame (ROWS BETWEEN CURRENT ROW AND UNBOUNDED
-- FOLLOWING), the monotonic direction of the window function determines
-- which comparison operators allow pushdown:
--   INCREASING (<=): row_number, rank, dense_rank, percent_rank,
--                    cume_dist, ntile
--   DECREASING (>):  count(*).  As the current row advances, the frame
--                    (which ends at UNBOUNDED FOLLOWING) shrinks, so the
--                    count decreases.
-- RPR window function results are match-dependent, not monotonic, so this
-- pushdown does not apply.

-- Non-RPR baseline: the filter is expected to appear as a Run Condition.
EXPLAIN (COSTS OFF)
SELECT * FROM (
    SELECT count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
) t WHERE cnt > 0;

-- RPR case: the filter must appear as a Filter above the WindowAgg,
-- not as a Run Condition.
EXPLAIN (COSTS OFF)
SELECT * FROM (
    SELECT count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) t WHERE cnt > 0;

-- Verify that the RPR query still returns every row whose match count is
-- greater than zero, confirming the filter is evaluated above the
-- WindowAgg rather than cutting off pattern matching prematurely.
SELECT * FROM (
    SELECT id, val, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) t WHERE cnt > 0
ORDER BY id;

-- ============================================================
-- A3. Window dedup prevention (RPR vs non-RPR)
-- ============================================================
-- Verify that PostgreSQL does not merge an RPR window with a non-RPR
-- window even when both share the same ORDER BY and frame
-- specification.  RPR pattern matching produces results that are
-- semantically different from a plain frame-based aggregate, so the
-- two windows must remain as separate WindowAgg nodes.  Inline window
-- specs are used throughout this section because only inline windows
-- are subject to the dedup path; distinct named windows are always
-- kept separate regardless of equivalence.

-- Non-RPR baseline: two inline windows with identical spec are
-- deduped by the parser into a single WindowAgg node, confirming
-- that the dedup path is active for non-RPR windows.
EXPLAIN (COSTS OFF)
SELECT
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) AS cnt,
    sum(val)  OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) AS total
FROM rpr_integ;

-- An inline RPR window and an inline non-RPR window share the same
-- ORDER BY and frame but must remain as distinct WindowAgg nodes.
EXPLAIN (COSTS OFF)
SELECT
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val)) AS rpr_cnt,
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) AS normal_cnt
FROM rpr_integ;

-- Verify that the two windows return independent counts per row,
-- confirming they were not merged into a single WindowAgg.
SELECT
    id, val,
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val)) AS rpr_cnt,
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) AS normal_cnt
FROM rpr_integ
ORDER BY id;

-- Result level: if the two windows had been merged, fv_normal and fv_rpr
-- would agree on every row.  They do not, so the windows stayed separate.
SELECT id, val,
    first_value(id) OVER (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    ) AS fv_normal,
    first_value(id) OVER w1 AS fv_rpr
FROM (VALUES (1, 10), (2, 20), (3, 30), (4, 40)) AS t(id, val)
WINDOW w1 AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS val > 10
);

-- The two windows above start from the same frame.  These two do not:
-- they converge only after frame optimization rewrites the non-RPR one,
-- and the RPR window is preserved, so they still must not be merged.
-- The view is deliberately left undropped: it is the only one in the
-- tree that serializes an RPR window and a non-RPR window together, so
-- pg_upgrade/pg_dump needs it to exercise that round trip.
CREATE VIEW rpr_ev_opt_mixed AS
SELECT
    row_number() OVER w_normal AS rn_normal,
    row_number() OVER w_rpr AS rn_rpr
FROM generate_series(1, 5) AS s(v)
WINDOW
    w_normal AS (ORDER BY v RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW),
    w_rpr AS (
        ORDER BY v
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS v > 1
    );

EXPLAIN (COSTS OFF) SELECT * FROM rpr_ev_opt_mixed;

-- ============================================================
-- A4. Window dedup prevention (same PATTERN, different DEFINE)
-- ============================================================
-- Verify that inline-window dedup does not merge two RPR windows
-- that share the same PATTERN structure but have different DEFINE
-- conditions.  Even though the ORDER BY, frame, and PATTERN coincide,
-- the differing DEFINE expressions classify rows differently and
-- must therefore yield two separate WindowAgg nodes.  Inline specs
-- are used here because dedup only applies to inline windows.

-- Baseline: two inline RPR windows that are structurally identical
-- (same PARTITION BY, ORDER BY, frame, PATTERN, and DEFINE) are deduped by the
-- parser into a single WindowAgg node, confirming that parser-level dedup is
-- active for RPR windows whose DEFINE matches.
EXPLAIN (COSTS OFF)
SELECT
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val)) AS cnt,
    sum(val)  OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val)) AS total
FROM rpr_integ;

-- Two inline RPR windows with the same PATTERN but opposite DEFINE
-- conditions must remain as separate WindowAgg nodes.
EXPLAIN (COSTS OFF)
SELECT
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val)) AS cnt_up,
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val < PREV(val)) AS cnt_down
FROM rpr_integ;

-- Verify that the two windows return different counts per row,
-- confirming the DEFINE conditions were not collapsed by dedup.
SELECT
    id, val,
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val)) AS cnt_up,
    count(*) OVER (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val < PREV(val)) AS cnt_down
FROM rpr_integ
ORDER BY id;

-- ============================================================
-- A5. Unused window removal prevention
-- ============================================================
-- Verify that remove_unused_subquery_outputs() does not drop an RPR
-- window function when the outer query does not reference its result.
-- The WindowAgg node performs the pattern match itself; without it,
-- the match would be silently skipped.  The plan must contain a
-- WindowAgg node beneath the outer Aggregate.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM (
    SELECT count(*) OVER w FROM rpr_integ
    WINDOW w AS (
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > PREV(val))
) t;

SELECT count(*) FROM (
    SELECT count(*) OVER w FROM rpr_integ
    WINDOW w AS (
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > PREV(val))
) t;

-- The DEFINE expression references PREV(val), so the window must be
-- preserved even if the outer query only aggregates over the count.
-- The plan must still contain a WindowAgg with the PATTERN/DEFINE
-- intact.
EXPLAIN (COSTS OFF)
SELECT count(*), sum(c) FROM (
    SELECT count(*) OVER w AS c FROM rpr_integ
    WINDOW w AS (
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > PREV(val))
) t;

SELECT count(*), sum(c) FROM (
    SELECT count(*) OVER w AS c FROM rpr_integ
    WINDOW w AS (
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS val > PREV(val))
) t;

-- The DEFINE expression contains no navigation, but the RPR window
-- must still be preserved because the match structure itself affects
-- the count.  The plan must retain the WindowAgg.
EXPLAIN (COSTS OFF)
SELECT count(*), sum(c) FROM (
    SELECT count(*) OVER w AS c FROM rpr_integ
    WINDOW w AS (
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS TRUE)
) t;

SELECT count(*), sum(c) FROM (
    SELECT count(*) OVER w AS c FROM rpr_integ
    WINDOW w AS (
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS TRUE)
) t;

-- XXX: "val" is non-resjunk in the subquery output and is not
-- referenced by the outer query.  Without a guard,
-- remove_unused_subquery_outputs() would replace it with NULL in
-- the subquery output, and that replacement propagates to the
-- scan's targetlist -- DEFINE would then evaluate with NULL
-- inputs.  The targetlist has no way to distinguish "exposed to
-- the outer query" from "referenced only by DEFINE", so the
-- optimization cannot be applied selectively.  The column guard
-- in allpaths.c blocks this replacement for any column referenced
-- by an RPR DEFINE clause, keeping the WindowAgg with DEFINE
-- active in the plan.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM (
    SELECT val, count(*) OVER w FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) t;

SELECT count(*) FROM (
    SELECT val, count(*) OVER w FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) t;

-- The same guard must also cover a whole-row Var.  Writing the bare
-- relation name (rpr_integ) in DEFINE resolves to a whole-row Var, whose
-- attribute number is 0.  remove_unused_subquery_outputs() matches the
-- guard on attribute number, so the whole-row Var is retained as a single
-- entry while the unused scalar "val" output is still replaced with NULL;
-- DEFINE evaluates against the intact row, and the match is unchanged.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT sum(c) FROM (
    SELECT val, count(*) OVER w AS c FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS rpr_integ IS NOT NULL)
) t;

SELECT sum(c) FROM (
    SELECT val, count(*) OVER w AS c FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS rpr_integ IS NOT NULL)
) t;

-- ============================================================
-- A6. Inverse transition bypass
-- ============================================================
-- Verify that RPR windows do not use the moving aggregate (inverse
-- transition) optimization.  Moving aggregates maintain state by
-- adding arriving rows and subtracting leaving rows, but an RPR
-- reduced frame is not a sliding window; the set of rows included in
-- the frame is determined by pattern matching and cannot be derived
-- incrementally from the previous frame.

-- sum() is inverse-transition eligible in a plain window; over an RPR reduced
-- frame rpr_is_defined() forces peraggstate->restart, so the aggregate is
-- recomputed from scratch.  The logging aggregate below asserts the bypass
-- directly.
SELECT id, val,
    sum(val) OVER w AS pattern_sum
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
ORDER BY id;

-- A custom aggregate logs how its transition functions are called: msfunc
-- appends '+' for a forward transition, minvfunc appends '-' for an inverse
-- transition.  Over an RPR reduced frame only '+' appears, confirming the
-- inverse-transition path is not used, and the logged values show which rows
-- each frame feeds the aggregate.
CREATE FUNCTION rpr_logging_sfunc(text, anyelement) RETURNS text AS
$$ SELECT COALESCE($1, '') || '*' || quote_nullable($2) $$ LANGUAGE SQL IMMUTABLE;
CREATE FUNCTION rpr_logging_msfunc(text, anyelement) RETURNS text AS
$$ SELECT COALESCE($1, '') || '+' || quote_nullable($2) $$ LANGUAGE SQL IMMUTABLE;
CREATE FUNCTION rpr_logging_minvfunc(text, anyelement) RETURNS text AS
$$ SELECT $1 || '-' || quote_nullable($2) $$ LANGUAGE SQL IMMUTABLE;
CREATE AGGREGATE rpr_logging_agg (anyelement)
(
    stype = text,
    sfunc = rpr_logging_sfunc,
    mstype = text,
    msfunc = rpr_logging_msfunc,
    minvfunc = rpr_logging_minvfunc
);

-- Match 1 is id1..id3 (v = NULL,'a','b') and match 2 is id4..id6
-- (v = NULL,'c','d'); B extends the match while k increases.  Every frame
-- shows '+' only (no '-'), and the two matches never share state across their
-- boundary.
SELECT id, v,
    rpr_logging_agg(v) OVER w AS logged
FROM (VALUES
    (1, 1, NULL),
    (2, 3, 'a'),
    (3, 5, 'b'),
    (4, 2, NULL),
    (5, 4, 'c'),
    (6, 6, 'd')
) AS t(id, k, v)
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE A AS TRUE, B AS k > PREV(k))
ORDER BY id;

DROP AGGREGATE rpr_logging_agg(anyelement);
DROP FUNCTION rpr_logging_sfunc(text, anyelement);
DROP FUNCTION rpr_logging_msfunc(text, anyelement);
DROP FUNCTION rpr_logging_minvfunc(text, anyelement);

-- ============================================================
-- A7. Cost estimation RPR awareness
-- ============================================================
-- cost_windowagg() must account for DEFINE expression evaluation cost.
-- Verify RPR WindowAgg cost > non-RPR WindowAgg cost.

CREATE FUNCTION get_windowagg_cost(query text) RETURNS numeric AS $$
DECLARE
    plan json;
    cost numeric;
BEGIN
    EXECUTE 'EXPLAIN (FORMAT JSON) ' || query INTO plan;
    cost := (plan->0->'Plan'->>'Total Cost')::numeric;
    RETURN cost;
END;
$$ LANGUAGE plpgsql;

SELECT get_windowagg_cost(
    'SELECT count(*) OVER w FROM rpr_integ
     WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
                  PATTERN (A B+ C+) DEFINE B AS val > PREV(val), C AS val < PREV(val))')
    >
    get_windowagg_cost(
    'SELECT count(*) OVER w FROM rpr_integ
     WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)')
    AS rpr_cost_is_higher;

DROP FUNCTION get_windowagg_cost(text);

-- ============================================================
-- A8. Subquery flattening prevention
-- ============================================================
-- Verify that a subquery containing an RPR window is not flattened
-- into the outer query.  is_simple_subquery() already blocks pullup
-- for subqueries with window functions in general; this test confirms
-- the rule continues to apply to RPR windows, so EXPLAIN must still
-- show a Subquery Scan above the RPR WindowAgg.

EXPLAIN (COSTS OFF)
SELECT * FROM (
    SELECT id, val, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) sub
WHERE cnt > 0;

-- ============================================================
-- A9. DEFINE expression non-propagation
-- ============================================================
-- Verify that DEFINE expressions are not propagated into the
-- targetlist of any upper WindowAgg node.  Only the column references
-- consumed by DEFINE should be passed up; the full DEFINE expression
-- is meaningful only inside the RPR WindowAgg that owns it.
-- EXPLAIN VERBOSE is therefore expected to show a clean targetlist on
-- the outer WindowAgg, with no DEFINE-derived expression leaking in.
-- Note: columns referenced by DEFINE (e.g., "val") may appear as
-- resjunk entries in upper WindowAgg targetlists -- but that is harmless.
-- The claim here is limited to the full DEFINE boolean expression.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT
    count(*) OVER w_rpr AS rpr_cnt,
    count(*) OVER w_normal AS normal_cnt
FROM rpr_integ
WINDOW
    w_rpr AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val)),
    w_normal AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING);

SELECT
    count(*) OVER w_rpr AS rpr_cnt,
    count(*) OVER w_normal AS normal_cnt
FROM rpr_integ
WINDOW
    w_rpr AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val)),
    w_normal AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
ORDER BY rpr_cnt DESC, normal_cnt DESC;

-- ============================================================
-- A10. RPR + LIMIT
-- ============================================================
-- LIMIT must not interfere with RPR pattern matching.  The Limit
-- node must sit above the WindowAgg so that pattern matching runs
-- on the full partition first; the result is then a prefix of the
-- un-LIMITed output.  Pushing Limit below the WindowAgg would
-- truncate input before matching and silently drop valid matches.
EXPLAIN (COSTS OFF)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
LIMIT 5;

-- Reference: un-LIMITed result against which the LIMIT 5 result is
-- compared.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
ORDER BY id;

-- LIMIT 5 case; the first five rows must match the reference above.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
LIMIT 5;

-- ============================================================
-- B1. RPR + CTE
-- ============================================================
-- Verify that an RPR window embedded inside a CTE behaves the same as
-- a direct RPR query:
--   (1) A single-reference CTE is inlined by the planner and yields
--       per-row results identical to the direct RPR query.
--   (2) A multi-reference CTE is materialized (CTE Scan appears in
--       the plan) so pattern matching runs once, and every reference
--       observes the same match results.

-- Baseline: direct RPR produces the per-row reference output.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
ORDER BY id;

-- Single-reference CTE: plan has no "CTE rpr_result" scope, showing
-- the CTE was inlined into the surrounding query.
EXPLAIN (COSTS OFF)
WITH rpr_result AS (
    SELECT id, val, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
)
SELECT id, val, cnt FROM rpr_result ORDER BY id;

-- Result must match the baseline row-for-row.
WITH rpr_result AS (
    SELECT id, val, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
)
SELECT id, val, cnt FROM rpr_result ORDER BY id;

-- Multi-reference CTE (self-join): plan has a "CTE rpr_result" scope
-- and CTE Scan nodes on both sides, showing the CTE was materialized
-- and pattern matching ran only once.
EXPLAIN (COSTS OFF)
WITH rpr_result AS (
    SELECT id, val, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
)
SELECT r1.id, r1.cnt
FROM rpr_result r1
JOIN rpr_result r2 ON r1.id = r2.id AND r1.cnt = r2.cnt
WHERE r1.cnt > 0
ORDER BY r1.id;

-- Result: both references see the same match counts, so the self-join
-- preserves all matched rows from the baseline.
WITH rpr_result AS (
    SELECT id, val, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
)
SELECT r1.id, r1.cnt
FROM rpr_result r1
JOIN rpr_result r2 ON r1.id = r2.id AND r1.cnt = r2.cnt
WHERE r1.cnt > 0
ORDER BY r1.id;

-- ============================================================
-- B2. RPR + JOIN
-- ============================================================
-- Verify that an RPR subquery can be joined with another relation.
-- Two aspects are checked against a non-RPR baseline:
--   (1) Flattening: a non-RPR subquery is pulled up by the planner
--       (no Subquery Scan in the plan); an RPR subquery is kept
--       un-flattened (Subquery Scan above WindowAgg).
--   (2) Join correctness: the join aligns each RPR match row with
--       the dimension-table row on the same key.

CREATE TABLE rpr_integ2 (id INT, label TEXT);
INSERT INTO rpr_integ2 VALUES
    (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e'),
    (6, 'f'), (7, 'g'), (8, 'h'), (9, 'i'), (10, 'j');

-- Baseline: a non-RPR subquery is flattened by the planner.  No
-- Subquery Scan node appears; the inner SELECT is merged into the
-- outer join.
EXPLAIN (COSTS OFF)
SELECT r.id, r.val, j.label
FROM (SELECT id, val FROM rpr_integ) r
JOIN rpr_integ2 j ON r.id = j.id
ORDER BY r.id;

-- RPR subquery JOIN: the Subquery Scan is preserved above the
-- WindowAgg, confirming the RPR subquery is not flattened.
EXPLAIN (COSTS OFF)
SELECT r.id, r.cnt, j.label
FROM (
    SELECT id, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) r
JOIN rpr_integ2 j ON r.id = j.id
WHERE r.cnt > 0
ORDER BY r.id;

-- Result: matched RPR rows align with dimension rows on id, showing
-- the join correctly pairs per-row match counts with their labels.
SELECT r.id, r.cnt, j.label
FROM (
    SELECT id, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) r
JOIN rpr_integ2 j ON r.id = j.id
WHERE r.cnt > 0
ORDER BY r.id;

-- ============================================================
-- B3. RPR + Set operations
-- ============================================================
-- Verify that RPR results combine correctly with non-RPR results
-- under a UNION ALL.  The plan must show an Append node with two
-- independent child plans: the RPR branch with Pattern/DEFINE active,
-- and the non-RPR branch with a plain WindowAgg.  Each child scans
-- the base relation on its own and contributes its rows to the
-- unioned output.

-- Plan: Append with two independent children.  The RPR branch has a
-- WindowAgg carrying Pattern/Nav Mark Lookback; the non-RPR branch
-- has a plain WindowAgg with no pattern metadata.
EXPLAIN (COSTS OFF)
SELECT id, cnt, 'rpr' AS source FROM (
    SELECT id, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) t WHERE cnt > 0
UNION ALL
SELECT id, count(*) OVER (ORDER BY id) AS cnt, 'normal' AS source
FROM rpr_integ
ORDER BY source, id;

-- Result: rows from both branches are present in the unioned output.
-- The RPR branch emits only matched rows (cnt > 0), while the
-- non-RPR branch emits all rows with its own count values.
SELECT id, cnt, 'rpr' AS source FROM (
    SELECT id, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) t WHERE cnt > 0
UNION ALL
SELECT id, count(*) OVER (ORDER BY id) AS cnt, 'normal' AS source
FROM rpr_integ
ORDER BY source, id;

-- ============================================================
-- B4. RPR + Prepared statements
-- ============================================================
-- Verify that RPR queries survive the prepared-statement path by
-- exercising both plancache modes with a parameter that feeds into
-- RPR's navigation offset (PREV(val, $1)).  The parameter surfaces
-- the RPR-specific plancache difference:
--   - custom plan: "Nav Mark Lookback" is resolved to the literal
--     parameter value at plan time (e.g., "Nav Mark Lookback: 1").
--   - generic plan: "Nav Mark Lookback" is deferred to execution and
--     appears as "Nav Mark Lookback: runtime" in the plan.
-- The result must be identical under both modes.

-- Register the prepared statement; DEFINE uses PREV(val, $1) so the
-- parameter reaches RPR's navigation machinery.
PREPARE rpr_prev(int) AS
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val, $1))
ORDER BY id;

-- Custom plan: Nav Mark Lookback resolved to the literal 1.
SET plan_cache_mode = force_custom_plan;
EXPLAIN (COSTS OFF) EXECUTE rpr_prev(1);
EXECUTE rpr_prev(1);

-- Generic plan: Nav Mark Lookback deferred to execution, shown as
-- "runtime" in the plan.  Result must match the custom-plan result
-- exactly.
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE rpr_prev(1);
EXECUTE rpr_prev(1);

-- Negative runtime nav offset under the generic plan: init clamps it to 0 for
-- trim sizing, but the per-row navigation rejects the negative offset.
EXECUTE rpr_prev(-1);

RESET plan_cache_mode;
DEALLOCATE rpr_prev;

-- ============================================================
-- B5. RPR + Partitioned table
-- ============================================================
-- Verify that RPR pattern matching works correctly when the source
-- relation is partitioned.  The planner must gather rows from every
-- partition into a single ordered stream before RPR can see them,
-- because pattern matching is sequential across the entire
-- partition-by group and cannot be performed independently on each
-- table partition.

CREATE TABLE rpr_part (id INT, val INT) PARTITION BY RANGE (id);
CREATE TABLE rpr_part_1 PARTITION OF rpr_part FOR VALUES FROM (1) TO (6);
CREATE TABLE rpr_part_2 PARTITION OF rpr_part FOR VALUES FROM (6) TO (11);
INSERT INTO rpr_part SELECT id, val FROM rpr_integ;

-- Plan: partition scans are combined with Append (or Merge Append),
-- sorted into a single ordered stream, and fed into one WindowAgg
-- that performs RPR pattern matching across the combined stream.
EXPLAIN (COSTS OFF)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_part
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
ORDER BY id;

-- Baseline: the same query against the non-partitioned rpr_integ
-- produces the per-row reference output.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
ORDER BY id;

-- Result against the partitioned table must match the baseline
-- row-for-row.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_part
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
ORDER BY id;

DROP TABLE rpr_part;

-- ============================================================
-- B6. RPR + LATERAL
-- ============================================================
-- RPR inside a LATERAL subquery.  Qualified column references from
-- the outer query are not yet supported in DEFINE, so this tests
-- the basic case where LATERAL provides the correlation filter
-- (WHERE id <= o.id) and DEFINE uses only local columns.  The plan
-- must show a Nested Loop driving the outer relation into the inner
-- subquery scan, with the RPR WindowAgg re-executed for each outer
-- row and the correlation surfacing as a scan-level Filter on
-- "id <= o.id".

-- Plan: Nested Loop with the RPR WindowAgg in the inner leg, driven
-- by the filtered outer rows (o.id IN (5, 10)).
EXPLAIN (COSTS OFF)
SELECT o.id AS outer_id, r.id, r.cnt
FROM rpr_integ o,
LATERAL (
    SELECT id, count(*) OVER w AS cnt
    FROM rpr_integ
    WHERE id <= o.id
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) r
WHERE r.cnt > 0 AND o.id IN (5, 10)
ORDER BY o.id, r.id;

-- Result: for each of the two outer ids (5 and 10), the LATERAL
-- subquery produces RPR match counts over the restricted input.
SELECT o.id AS outer_id, r.id, r.cnt
FROM rpr_integ o,
LATERAL (
    SELECT id, count(*) OVER w AS cnt
    FROM rpr_integ
    WHERE id <= o.id
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
) r
WHERE r.cnt > 0 AND o.id IN (5, 10)
ORDER BY o.id, r.id;

-- A lateral outer reference can share varno and varattno with a DEFINE-only
-- column: here o.b and y are both attribute 2 at their own query levels.
-- Only varlevelsup separates them, so the junk targetlist entry for y has to
-- be added even though a Var with the same varno and varattno is present.
CREATE TABLE rpr_lat_o (a int, b int);
CREATE TABLE rpr_lat_i (x int, y int);
INSERT INTO rpr_lat_o VALUES (1, 10);
INSERT INTO rpr_lat_i VALUES (1, 5), (2, 6);

-- The overlapping shape: the outer reference is o.b, attribute 2.
SELECT *
FROM rpr_lat_o o,
LATERAL (
    SELECT o.b AS lat, count(*) OVER w AS c
    FROM rpr_lat_i
    WINDOW w AS (ORDER BY x
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS y > 0)
) s;

-- Control: the outer reference is o.a, attribute 1, which cannot be mistaken
-- for y.  The counts must match the query above.
SELECT *
FROM rpr_lat_o o,
LATERAL (
    SELECT o.a AS lat, count(*) OVER w AS c
    FROM rpr_lat_i
    WINDOW w AS (ORDER BY x
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A+)
        DEFINE A AS y > 0)
) s;
DROP TABLE rpr_lat_o, rpr_lat_i;

-- ============================================================
-- B7. RPR + Recursive CTE
-- ============================================================
-- RPR is rejected in every leg of a recursive query, including the
-- non-recursive leg (see parse_cte.c for the standard citation).

-- WITH RECURSIVE: RPR in the base leg is rejected even though the
-- base leg never references the recursive CTE name.
WITH RECURSIVE seq AS (
    SELECT id, val, count(*) OVER w AS cnt
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val))
    UNION ALL
    SELECT id + 100, val, cnt FROM seq WHERE id < 3
)
SELECT id, val, cnt FROM seq ORDER BY id;

-- CREATE RECURSIVE VIEW: rewritten by makeRecursiveViewSelect()
-- into WITH RECURSIVE, so the same rejection applies.  This is
-- the form ISO/IEC 19075-5 6.17.5 cites verbatim.
CREATE RECURSIVE VIEW rpr_recv(id, val, cnt) AS
    SELECT id, val, count(*) OVER w
    FROM rpr_integ
    WINDOW w AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (A B+)
        DEFINE B AS val > PREV(val));

-- ============================================================
-- B8. RPR + Incremental sort
-- ============================================================
-- Verify that RPR pattern matching works correctly when the input
-- to WindowAgg arrives via an incremental sort.  The index on (id)
-- provides presorted input for the first ORDER BY key, so
-- "ORDER BY id, val" lets the planner use Incremental Sort to order
-- only on the second key.  The plan must show Incremental Sort
-- below the RPR WindowAgg, and RPR must produce the same per-row
-- match counts as it would with a plain Sort.

CREATE INDEX rpr_integ_id_idx ON rpr_integ (id);
SET enable_seqscan = off;

-- Plan: RPR WindowAgg above an Incremental Sort above an Index Scan.
-- The Incremental Sort declares "Presorted Key: id" and sorts only
-- on val within each id group.
EXPLAIN (COSTS OFF)
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id, val
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val));

-- Result: RPR over the incrementally sorted stream produces match
-- counts per row.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id, val
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val))
ORDER BY id, val;

RESET enable_seqscan;
DROP INDEX rpr_integ_id_idx;

-- ============================================================
-- B9. RPR + Volatile function in DEFINE
-- ============================================================
-- Volatile functions in DEFINE are rejected in the planner.  Under
-- RPR's NFA engine the same row's DEFINE predicate may be evaluated
-- multiple times (backtracking, PREV/NEXT navigation), so a volatile
-- result would make pattern matching non-deterministic.  STABLE and
-- IMMUTABLE callees are accepted.

-- Baseline: STABLE (to_char) and IMMUTABLE (length) callees are accepted.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val)
                AND length('x') = 1
                AND to_char(date '2026-01-01', 'YYYY') = '2026')
ORDER BY id;

-- Volatile (random) is rejected.
SELECT id, val, count(*) OVER w AS cnt
FROM rpr_integ
WINDOW w AS (ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B+)
    DEFINE B AS val > PREV(val) AND random() >= 0.0)
ORDER BY id;

-- ============================================================
-- B10. RPR + Correlated subquery in WHERE
-- ============================================================
-- Verify that an RPR window placed inside a correlated scalar
-- subquery is executed once per outer row.  DEFINE still references
-- only local columns (qualified refs from the outer query are not
-- supported in DEFINE); the correlation lives in the subquery's
-- WHERE clause as "i.id <= o.id".  The plan must show a SubPlan
-- attached to the outer scan, with the RPR WindowAgg driven by a
-- per-row scan filter carrying the correlation predicate.

-- Plan: SubPlan attached to the outer Seq Scan; the inner scan
-- carries "Filter: (id <= o.id)", confirming the correlation is
-- evaluated per outer row.
EXPLAIN (COSTS OFF)
SELECT o.id, o.val,
    (SELECT count(*) OVER w
     FROM rpr_integ i
     WHERE i.id <= o.id
     WINDOW w AS (ORDER BY id
         ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
         PATTERN (A B+)
         DEFINE B AS val > PREV(val))
     ORDER BY id
     LIMIT 1) AS first_cnt
FROM rpr_integ o
ORDER BY o.id;

-- Result: each outer row receives the first_cnt from its own
-- correlated RPR subquery.
SELECT o.id, o.val,
    (SELECT count(*) OVER w
     FROM rpr_integ i
     WHERE i.id <= o.id
     WINDOW w AS (ORDER BY id
         ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
         PATTERN (A B+)
         DEFINE B AS val > PREV(val))
     ORDER BY id
     LIMIT 1) AS first_cnt
FROM rpr_integ o
ORDER BY o.id;

-- ============================================================
-- B11. RPR + Junk targetlist pruning
-- ============================================================
-- Verify that the junk targetlist entry planted for a DEFINE-only
-- column does not keep an unrelated column alive.  DEFINE references
-- a (rpr_over1); c (rpr_over2) carries the same attribute number but
-- is unused, so the plan must drop it.
CREATE TABLE rpr_over1 (a int);
CREATE TABLE rpr_over2 (c int);
INSERT INTO rpr_over1 VALUES (1),(2),(3);
INSERT INTO rpr_over2 VALUES (1),(2),(3);

-- Plan: only the DEFINE column survives in the subquery output.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT cnt FROM (
  SELECT a AS oa, c AS oc, count(*) OVER w AS cnt
  FROM rpr_over1 CROSS JOIN rpr_over2
  WINDOW w AS (ORDER BY a ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
               PATTERN (X+) DEFINE X AS a > 0)
) s;
DROP TABLE rpr_over1, rpr_over2;

-- ============================================================
-- B12. RPR + Correlated navigation offsets
-- ============================================================
-- A row pattern navigation offset that resolves to a correlated PARAM_EXEC
-- (here through SRF inlining of rpr_srf_prev(g.n)) must be re-resolved on every
-- rescan, not frozen at executor init.  The inlined WindowAgg is the inner
-- side of a nestloop and is rescanned once per outer row, so each row sees its
-- own PREV(v, n) offset; a frozen offset would report the same value for all.
CREATE TABLE rpr_srf (v int);
INSERT INTO rpr_srf SELECT generate_series(1, 10);
CREATE FUNCTION rpr_srf_prev(k int) RETURNS SETOF bigint AS $$
  SELECT count(*) OVER w
  FROM rpr_srf
  WINDOW w AS (ORDER BY v ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
               PATTERN (A+) DEFINE A AS v > PREV(v, k))
$$ LANGUAGE sql STABLE;
-- Plan: the offset reads "runtime"; the WindowAgg inlines into the
-- nestloop and is rescanned per outer row.
EXPLAIN (COSTS OFF)
SELECT g.n, max(s) FROM (VALUES (1), (2), (3)) g(n), LATERAL rpr_srf_prev(g.n) s
GROUP BY g.n ORDER BY g.n;
-- Each outer row yields its own offset (9, 8, 7), not one frozen value.
SELECT g.n, max(s) AS m FROM (VALUES (1), (2), (3)) g(n), LATERAL rpr_srf_prev(g.n) s
GROUP BY g.n ORDER BY g.n;

-- A forward FIRST-family offset must likewise be re-resolved per scan
-- (navFirstOffset / navFirstOffsetKind).  PATTERN (B A+) anchors the
-- match start at B so A can reference FIRST(v, k) k rows ahead, and
-- each outer k yields its own forward offset.
CREATE FUNCTION rpr_srf_first(k int) RETURNS SETOF bigint AS $$
  SELECT count(*) OVER w
  FROM rpr_srf
  WINDOW w AS (ORDER BY v ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
               PATTERN (B A+) DEFINE A AS v > FIRST(v, k))
$$ LANGUAGE sql STABLE;
-- Plan: the forward offset reads "runtime" and the WindowAgg inlines
-- into the nestloop.
EXPLAIN (COSTS OFF)
SELECT g.n, max(s) FROM (VALUES (0), (1), (2)) g(n), LATERAL rpr_srf_first(g.n) s
GROUP BY g.n ORDER BY g.n;
-- Result: k=0 matches all ten rows and k>=1 makes the first A fail,
-- so the forward offset is not frozen at ExecInit.
SELECT g.n, max(s) AS m FROM (VALUES (0), (1), (2)) g(n), LATERAL rpr_srf_first(g.n) s
GROUP BY g.n ORDER BY g.n;
DROP FUNCTION rpr_srf_first(int);

-- A compound navigation's OUTER offset must be re-resolved per scan
-- as well.  The last offset overflows int64, so that scan's navigation
-- has no target row at all.
CREATE FUNCTION rpr_srf_cmp(k int8) RETURNS SETOF bigint AS $$
  SELECT count(*) OVER w
  FROM rpr_srf
  WINDOW w AS (ORDER BY v ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
               PATTERN (A B+) DEFINE B AS v > PREV(LAST(v, 1), k))
$$ LANGUAGE sql STABLE;
-- Result: k=1 -> 9, k=3 -> 7, overflow -> 0.  Three answers from one plan
-- is what says each rescan resolved its own outer offset.  The trim kind a
-- scan settles on does not show in a count; rpr_explain reads it out of
-- EXPLAIN ANALYZE instead.
SELECT g.n, max(s) AS m
FROM (VALUES (1::int8), (3::int8), (9223372036854775807::int8)) g(n),
     LATERAL rpr_srf_cmp(g.n) s
GROUP BY g.n ORDER BY g.n;
DROP FUNCTION rpr_srf_cmp(int8);
DROP FUNCTION rpr_srf_prev(int);
DROP TABLE rpr_srf;

-- ============================================================
-- B13. RPR + DEFINE-only parameter caching
-- ============================================================
-- A correlated PARAM_EXEC used only inside DEFINE must reach the WindowAgg's
-- extParam.  Otherwise chgParam never gets to the HashAgg that DISTINCT plans
-- above it, and its hash table for the first outer row is re-served.
-- The SRF must be inlined as a lateral subquery for its argument to become a
-- PARAM_EXEC; a FunctionScan plan would pass either way.
CREATE TABLE rpr_hcache_thr (threshold int);
INSERT INTO rpr_hcache_thr VALUES (10), (200);
CREATE TABLE rpr_hcache_stock (price int);
INSERT INTO rpr_hcache_stock SELECT g FROM generate_series(1, 100) g;
CREATE FUNCTION rpr_hcache_fn(th int) RETURNS SETOF bigint LANGUAGE sql STABLE AS $$
  SELECT DISTINCT count(*) OVER w FROM rpr_hcache_stock
  WINDOW w AS (ORDER BY price ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
               AFTER MATCH SKIP PAST LAST ROW
               INITIAL PATTERN (a+) DEFINE a AS price > th) $$;
-- Result: each threshold gets its own answer set (10 -> {0, 90},
-- 200 -> {0}); a stale cache would add a spurious 200|90.
SELECT o.threshold, f FROM rpr_hcache_thr o, LATERAL rpr_hcache_fn(o.threshold) f
ORDER BY 1, 2;
DROP FUNCTION rpr_hcache_fn(int);
DROP TABLE rpr_hcache_thr, rpr_hcache_stock;

-- ============================================================
-- B14. RPR + Multiple window definitions
-- ============================================================
-- A DEFINE-only column and a later window's sort key both become junk
-- targetlist entries.  Each draws its resno from p_next_resno, which is what
-- keeps the two distinct: a targetlist that gives one resno to two entries is
-- not a valid Query, and the parser is the only place that can prevent it.
SELECT id, count(*) OVER w1 AS c1, count(*) OVER w2 AS c2
FROM (VALUES (1,1,10),(2,1,20)) t(id, grp, val)
WINDOW w1 AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (S U+)
        DEFINE U AS val > PREV(val)),
    w2 AS (PARTITION BY grp ORDER BY id)
ORDER BY id;

-- The same pair reached through a plain ORDER BY window.
SELECT id, count(*) OVER w1 AS c1, count(*) OVER w2 AS c2
FROM (VALUES (1,1,10),(2,1,20)) t(id, grp, val)
WINDOW w1 AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (S U+)
        DEFINE U AS val > PREV(val)),
    w2 AS (ORDER BY grp)
ORDER BY id;

-- Control: the opposite declaration order draws the sort key first, so the two
-- never compete for a resno.  It must return the same rows as the first query
-- above.
SELECT id, count(*) OVER w1 AS c1, count(*) OVER w2 AS c2
FROM (VALUES (1,1,10),(2,1,20)) t(id, grp, val)
WINDOW w2 AS (PARTITION BY grp ORDER BY id),
    w1 AS (ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        PATTERN (S U+)
        DEFINE U AS val > PREV(val))
ORDER BY id;

-- Cleanup
DROP TABLE rpr_integ;
DROP TABLE rpr_integ2;
