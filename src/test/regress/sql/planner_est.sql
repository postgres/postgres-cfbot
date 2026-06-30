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

--
-- Multi column unique index row estimates
--

-- Function to assist with verifying EXPLAIN row estimation.
-- Row estimation will be replaced by >1 if row estimation is greater than 1
CREATE FUNCTION explain_one_or_more_row(query text) RETURNS setof text
LANGUAGE plpgsql AS
$$
DECLARE
    ln text;
BEGIN
    -- avoid jit related output by disabling it
    SET LOCAL jit = 0;

    FOR ln IN
        EXECUTE format('explain (costs on, summary off, timing off, buffers off) %s', query)
    LOOP
        ln := regexp_replace(ln, 'cost=\d+\.\d\d\.\.\d+\.\d\d', 'cost=N..N');
        ln := regexp_replace(ln, 'rows=([2-9]|[1-9][0-9]+)', 'rows=>1');
        ln := regexp_replace(ln, 'width=\d+', 'width=N');
        RETURN NEXT ln;
    END LOOP;
END;
$$;


CREATE TABLE multi_column_unique (a int, b int, c int) WITH (autovacuum_enabled=false);
CREATE UNIQUE INDEX multi_column_unique_idx ON multi_column_unique (a, b);
INSERT INTO multi_column_unique(a, b, c) SELECT 1, i, 3 FROM generate_series(1,10) as g(i);
INSERT INTO multi_column_unique(a, b, c) SELECT i, 1, 3 FROM generate_series(2,10) as g(i);
ANALYZE multi_column_unique;

CREATE TABLE multi_column_unique_null (a int, b int) WITH (autovacuum_enabled=false);
CREATE UNIQUE INDEX multi_column_unique_null_idx ON multi_column_unique_null (a, b);
INSERT INTO multi_column_unique_null(a, b) SELECT 1, NULL FROM generate_series(1,20);
ANALYZE multi_column_unique_null;

CREATE TABLE multi_column_unique_null_not_distinct (a int, b int) WITH (autovacuum_enabled=false);
CREATE UNIQUE INDEX multi_column_unique_null_not_distinct_idx ON multi_column_unique_null_not_distinct (a, b) NULLS NOT DISTINCT;
INSERT INTO multi_column_unique_null_not_distinct(a, b) SELECT i, NULL FROM generate_series(1,10) AS g(i);
INSERT INTO multi_column_unique_null_not_distinct(a, b) SELECT 1, i FROM generate_series(1,10) as g(i);
ANALYZE multi_column_unique_null_not_distinct;

CREATE TABLE multi_column_unique_deferred (a int, b int) WITH (autovacuum_enabled=false);
ALTER TABLE multi_column_unique_deferred
  ADD CONSTRAINT multi_column_unique_deferred_idx UNIQUE (a, b)
  DEFERRABLE INITIALLY DEFERRED;
INSERT INTO multi_column_unique_deferred(a, b) SELECT 1, i FROM generate_series(1,10) as g(i);
INSERT INTO multi_column_unique_deferred(a, b) SELECT i, 1 FROM generate_series(2,10) as g(i);
ANALYZE multi_column_unique_deferred;

CREATE TABLE multi_column_unique_partial (a int, b int) WITH (autovacuum_enabled=false);
CREATE UNIQUE INDEX multi_column_unique_partial_idx  ON multi_column_unique_partial (a, b) WHERE b > 10;
INSERT INTO multi_column_unique_partial(a, b) SELECT 1, 1 FROM generate_series(1,10);
INSERT INTO multi_column_unique_partial(a, b) SELECT i, 11 FROM generate_series(1,10) as g(i);
ANALYZE multi_column_unique_partial;

set enable_seqscan to false;

-- Matching a unique index should yield 1 row
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique WHERE a=1 AND b=1;
$$);

-- An array shouldn't invalidate the unique path and still yield 1 row
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique WHERE a=1 AND b=1 AND b=ANY('{1,2,3}');
$$);

-- Missing a unique key column should yield >1 rows
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique WHERE a=1;
$$);

SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique WHERE a=1 AND c=3;
$$);

-- A missing equal op on one of the key columns should invalidate path
-- uniqueness and yield >1 rows

SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique WHERE a=1 AND b=ANY('{1,2,3}');
$$);
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique WHERE a=1 AND b>1;
$$);
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique WHERE a=1 AND b IS NOT NULL;
$$);

-- IS NULL + index with NULLS DISTINCT should yield >1 rows
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique_null WHERE a=1 AND b IS NULL;
$$);

-- IS NULL + Unique index with NULLS NOT DISTINCT should yield 1 row
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique_null_not_distinct WHERE a=1 AND b IS NULL;
$$);

-- While a deferrable unique constraint is not a planner proof, it's
-- probably closer to the truth than using statistics. So, expect one
-- estimated row here
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique_deferred WHERE a=1 AND b=1;
$$);

-- Matching a unique partial index should yield 1 row
SELECT explain_one_or_more_row($$
SELECT * FROM multi_column_unique_partial WHERE a=1 AND b=11;
$$);

reset enable_seqscan;
DROP FUNCTION explain_one_or_more_row(text);
DROP FUNCTION explain_mask_costs(text, bool, bool, bool, bool);
