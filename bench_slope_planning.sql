--
-- SLOPE Planning Time Benchmark (Single Connection)
--
-- Measures planning time for queries that benefit from SLOPE optimization.
-- Tests with varying numbers of indices to see scaling behavior.
-- Uses clock_timestamp() to measure time spent in EXPLAIN (without ANALYZE).
--

-- Force index usage
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_hashagg = off;

-- Create results table
DROP TABLE IF EXISTS bench_results;
CREATE TABLE bench_results (
    id serial PRIMARY KEY,
    num_indices int NOT NULL,
    planning_time_us numeric NOT NULL
);

-- Function to create test table with N indices
CREATE OR REPLACE FUNCTION setup_bench_table(p_num_indices int) RETURNS void AS $$
DECLARE
    v_i int;
BEGIN
    DROP TABLE IF EXISTS bench_slope CASCADE;

    CREATE TABLE bench_slope (
        id serial PRIMARY KEY,
        ts timestamp NOT NULL,
        v_int4 int4 NOT NULL,
        v_float8 float8 NOT NULL
    );

    -- Always create index on ts (the one we query)
    CREATE INDEX ON bench_slope (ts);

    -- Create additional indices to stress the planner
    FOR v_i IN 1..p_num_indices LOOP
        EXECUTE format('CREATE INDEX ON bench_slope ((v_int4 + %s))', v_i);
    END LOOP;

    INSERT INTO bench_slope (ts, v_int4, v_float8)
    SELECT
        '2020-01-01'::timestamp + (i || ' hours')::interval,
        i,
        i::float8
    FROM generate_series(1, 1000) i;

    ANALYZE bench_slope;
END;
$$ LANGUAGE plpgsql;

-- Benchmark function for a specific number of indices
CREATE OR REPLACE FUNCTION bench_slope_planning_n(
    p_num_indices int,
    p_iterations int,
    p_warmup int
) RETURNS void AS $$
DECLARE
    v_start timestamp;
    v_end timestamp;
    v_elapsed_us numeric;
    v_i int;
    v_plan text;
BEGIN
    -- Setup table
    PERFORM setup_bench_table(p_num_indices);

    -- Warmup phase
    FOR v_i IN 1..p_warmup LOOP
        FOR v_plan IN EXPLAIN SELECT date_trunc('month', ts), count(*) FROM bench_slope GROUP BY 1 LOOP
        END LOOP;
    END LOOP;

    -- Benchmark phase
    FOR v_i IN 1..p_iterations LOOP
        v_start := clock_timestamp();
        FOR v_plan IN EXPLAIN SELECT date_trunc('month', ts), count(*) FROM bench_slope GROUP BY 1 LOOP
        END LOOP;
        v_end := clock_timestamp();
        v_elapsed_us := extract(epoch from (v_end - v_start)) * 1000000;
        INSERT INTO bench_results (num_indices, planning_time_us)
        VALUES (p_num_indices, v_elapsed_us);
    END LOOP;
END;
$$ LANGUAGE plpgsql;

-- Run the benchmark
\echo '========================================================================'
\echo 'SLOPE Planning Time Benchmark (Single Connection, Varying Indices)'
\echo '========================================================================'
\echo ''

TRUNCATE bench_results;

\echo 'Testing with 0 additional indices...'
SELECT bench_slope_planning_n(0, 5000, 50);

\echo 'Testing with 5 additional indices...'
SELECT bench_slope_planning_n(5, 5000, 50);

\echo 'Testing with 10 additional indices...'
SELECT bench_slope_planning_n(10, 5000, 50);

\echo 'Testing with 20 additional indices...'
SELECT bench_slope_planning_n(20, 5000, 50);

\echo 'Testing with 50 additional indices...'
SELECT bench_slope_planning_n(50, 5000, 50);

-- Results by number of indices
\echo ''
\echo 'Results by Number of Indices:'
\echo ''

SELECT
    num_indices as "Indices",
    count(*) as "N",
    round(avg(planning_time_us)::numeric, 1) as "Mean (µs)",
    round((percentile_cont(0.5) WITHIN GROUP (ORDER BY planning_time_us))::numeric, 1) as "Median (µs)",
    round(stddev(planning_time_us)::numeric, 1) as "StdDev (µs)",
    round((percentile_cont(0.05) WITHIN GROUP (ORDER BY planning_time_us))::numeric, 1) as "P5 (µs)",
    round((percentile_cont(0.95) WITHIN GROUP (ORDER BY planning_time_us))::numeric, 1) as "P95 (µs)"
FROM bench_results
GROUP BY num_indices
ORDER BY num_indices;

-- Cleanup
DROP FUNCTION bench_slope_planning_n(int, int, int);
DROP FUNCTION setup_bench_table(int);
DROP TABLE bench_results;
DROP TABLE IF EXISTS bench_slope CASCADE;

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_hashagg;
