--
-- Tests for testing query planner selectivity and width estimates
--
-- Most selectivity and width estimations rely too heavily on statistics
-- gathered by ANALYZE, or could vary depending on hardware.  However, there
-- are a few cases where we can have more certainty about the expected number
-- of rows, or width of rows.  This is a good home for such tests.
--

-- Function to assist with verifying EXPLAIN which includes costs.  A series
-- of bool flags allows control over which portions are masked out
CREATE FUNCTION explain_mask_costs(query text, do_analyze bool,
    hide_costs bool, hide_row_est bool, hide_width bool) RETURNS setof text
LANGUAGE plpgsql AS
$$
DECLARE
    ln text;
    analyze_str text;
BEGIN
    IF do_analyze = true THEN
        analyze_str := 'on';
    ELSE
        analyze_str := 'off';
    END IF;

    -- avoid jit related output by disabling it
    SET LOCAL jit = 0;

    FOR ln IN
        EXECUTE format('explain (analyze %s, costs on, summary off, timing off, buffers off) %s',
            analyze_str, query)
    LOOP
        IF hide_costs = true THEN
            ln := regexp_replace(ln, 'cost=\d+\.\d\d\.\.\d+\.\d\d', 'cost=N..N');
        END IF;

        IF hide_row_est = true THEN
            -- don't use 'g' so that we leave the actual rows intact
            ln := regexp_replace(ln, 'rows=\d+', 'rows=N');
        END IF;

        IF hide_width = true THEN
            ln := regexp_replace(ln, 'width=\d+', 'width=N');
        END IF;

        RETURN NEXT ln;
    END LOOP;
END;
$$;

--
-- Test the SupportRequestRows support function for generate_series_timestamp()
--

-- Ensure the row estimate matches the actual rows
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- As above but with generate_series_timestamp
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMP '2024-02-01', TIMESTAMP '2024-03-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- As above but with generate_series_timestamptz_at_zone()
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '1 day', 'UTC') g(s);$$,
true, true, false, true);

-- Ensure the estimated and actual row counts match when the range isn't
-- evenly divisible by the step
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '7 day') g(s);$$,
true, true, false, true);

-- Ensure the estimates match when step is decreasing
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-03-01', TIMESTAMPTZ '2024-02-01', INTERVAL '-1 day') g(s);$$,
true, true, false, true);

-- Ensure an empty range estimates 1 row
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '2024-03-01', TIMESTAMPTZ '2024-02-01', INTERVAL '1 day') g(s);$$,
true, true, false, true);

-- Ensure we get the default row estimate for infinity values
SELECT explain_mask_costs($$
SELECT * FROM generate_series(TIMESTAMPTZ '-infinity', TIMESTAMPTZ 'infinity', INTERVAL '1 day') g(s);$$,
false, true, false, true);

-- Ensure the row estimate behaves correctly when step size is zero.
-- We expect generate_series_timestamp() to throw the error rather than in
-- the support function.
SELECT * FROM generate_series(TIMESTAMPTZ '2024-02-01', TIMESTAMPTZ '2024-03-01', INTERVAL '0 day') g(s);

--
-- Test the SupportRequestRows support function for generate_series_numeric()
--

-- Ensure the row estimate matches the actual rows
SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0) g(s);$$,
true, true, false, true);

-- As above but with non-default step
SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0, 2.0) g(s);$$,
true, true, false, true);

-- Ensure the estimates match when step is decreasing
SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 1.0, -1.0) g(s);$$,
true, true, false, true);

-- Ensure an empty range estimates 1 row
SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 1.0, 1.0) g(s);$$,
true, true, false, true);

-- Ensure we get the default row estimate for error cases (infinity/NaN values
-- and zero step size)
SELECT explain_mask_costs($$
SELECT * FROM generate_series('-infinity'::NUMERIC, 'infinity'::NUMERIC, 1.0) g(s);$$,
false, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_series(1.0, 25.0, 'NaN'::NUMERIC) g(s);$$,
false, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_series(25.0, 2.0, 0.0) g(s);$$,
false, true, false, true);

--
-- Test the SupportRequestRows support function for generate_subscripts()
--

-- A constant array gives an exact estimate for the requested dimension
SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{10,20,30,40,50}'::int[], 1) g(s);$$,
true, true, false, true);

-- As above but for the 3-argument form; "reverse" cannot change the row count
SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{10,20,30,40,50}'::int[], 1, true) g(s);$$,
true, true, false, true);

-- Ensure a non-zero lower bound does not affect the estimate
SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('[5:9]={10,20,30,40,50}'::int[], 1) g(s);$$,
true, true, false, true);

-- Ensure each dimension of a multi-dimensional array is estimated separately
SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{{1,2,3},{4,5,6}}'::int[], 1) g(s);$$,
true, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{{1,2,3},{4,5,6}}'::int[], 2) g(s);$$,
true, true, false, true);

-- Ensure cases which return no rows estimate 1 row after clamping.  Try an
-- out-of-range dimension, a dimension below the first, and an empty array.
SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{{1,2,3},{4,5,6}}'::int[], 3) g(s);$$,
true, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{10,20,30}'::int[], 0) g(s);$$,
true, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{}'::int[], 1) g(s);$$,
true, true, false, true);

-- Ensure a constant NULL in any argument position estimates no rows, since
-- generate_subscripts() is strict
SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts(NULL::int[], 1) g(s);$$,
true, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{10,20,30}'::int[], NULL) g(s);$$,
true, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts('{10,20,30}'::int[], 1, NULL) g(s);$$,
true, true, false, true);

-- An ArrayExpr is estimated by way of estimate_array_length()
SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts(ARRAY[1, 2, (SELECT 3)], 1) g(s);$$,
true, true, false, true);

-- Ensure we get the default row estimate when the dimension number isn't a
-- constant, and when a dimension above the first is requested for an array
-- which isn't a constant.  Neither case can be estimated.
SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts(ARRAY[1, 2, (SELECT 3)], (SELECT 1)) g(s);$$,
false, true, false, true);

SELECT explain_mask_costs($$
SELECT * FROM generate_subscripts(ARRAY[1, 2, (SELECT 3)], 2) g(s);$$,
false, true, false, true);

-- Ensure a Var array is estimated from the DECHIST statistics.  Every array
-- below holds exactly 7 distinct elements, so the average distinct element
-- count does not depend on which rows ANALYZE happens to sample.
CREATE TEMP TABLE subscript_table_1 AS
  SELECT array_agg(g) AS a FROM generate_series(1, 100) i,
    LATERAL generate_series(1, 7) g GROUP BY i;
ANALYZE subscript_table_1;

-- Memoize is disabled here only to keep the plan shape stable; the node's
-- capacity and hit percentage are not masked by explain_mask_costs().
SET enable_memoize = off;

SELECT explain_mask_costs($$
SELECT * FROM subscript_table_1 t, LATERAL generate_subscripts(t.a, 1) g(s);$$,
true, true, false, true);

-- As above, but a dimension above the first still falls back on prorows
SELECT explain_mask_costs($$
SELECT * FROM subscript_table_1 t, LATERAL generate_subscripts(t.a, 2) g(s);$$,
false, true, false, true);

RESET enable_memoize;

--
-- Test ScalarArrayOpExpr row estimates for <> ALL for arrays with NULLs.  We
-- expect the planner to estimate 1 row will match in both of the following
-- tests.
--

-- Try a const array containing a NULL
SELECT explain_mask_costs($$
SELECT * FROM tenk1 WHERE unique1 <> ALL (ARRAY[1, 2, 99, NULL]);$$,
false, true, false, true);

-- Try a non-const array containing a NULL
SELECT explain_mask_costs($$
SELECT * FROM tenk1 WHERE unique1 <> ALL (ARRAY[1, 2, 98, (SELECT 99), NULL]);$$,
false, true, false, true);

-- Verify that scalarineqsel() works on "char" columns
CREATE TEMP TABLE char_table_1 AS
  SELECT i::"char" AS c FROM generate_series(64,96) i;
ANALYZE char_table_1;
EXPLAIN (COSTS OFF) SELECT * FROM char_table_1 WHERE c < 'Q';

DROP FUNCTION explain_mask_costs(text, bool, bool, bool, bool);
