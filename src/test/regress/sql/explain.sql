--
-- EXPLAIN
--
-- There are many test cases elsewhere that use EXPLAIN as a vehicle for
-- checking something else (usually planner behavior).  This file is
-- concerned with testing EXPLAIN in its own right.
--

-- To produce stable regression test output, it's usually necessary to
-- ignore details such as exact costs or row counts.  These filter
-- functions replace changeable output details with fixed strings.

create function explain_filter(text) returns setof text
language plpgsql as
$$
declare
    ln text;
begin
    for ln in execute $1
    loop
        -- Replace any numeric word with just 'N'
        ln := regexp_replace(ln, '-?\m\d+\M', 'N', 'g');
        -- In sort output, the above won't match units-suffixed numbers
        ln := regexp_replace(ln, '\m\d+kB', 'NkB', 'g');
        -- Ignore text-mode buffers output because it varies depending
        -- on the system state
        CONTINUE WHEN (ln ~ ' +Buffers: .*');
        -- Ignore text-mode "Planning:" line because whether it's output
        -- varies depending on the system state
        CONTINUE WHEN (ln = 'Planning:');
        return next ln;
    end loop;
end;
$$;

-- To produce valid JSON output, replace numbers with "0" or "0.0" not "N"
create function explain_filter_to_json(text) returns jsonb
language plpgsql as
$$
declare
    data text := '';
    ln text;
begin
    for ln in execute $1
    loop
        -- Replace any numeric word with just '0'
        ln := regexp_replace(ln, '\m\d+\M', '0', 'g');
        data := data || ln;
    end loop;
    return data::jsonb;
end;
$$;

-- Disable JIT, or we'll get different output on machines where that's been
-- forced on
set jit = off;

-- Similarly, disable track_io_timing, to avoid output differences when
-- enabled.
set track_io_timing = off;

-- Simple cases

explain (costs off) select 1 as a, 2 as b having false;
select explain_filter('explain select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers off) select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers off, verbose) select * from int8_tbl i8');
select explain_filter('explain (analyze, buffers, format text) select * from int8_tbl i8');
select explain_filter('explain (buffers, format text) select * from int8_tbl i8');

\a
select explain_filter('explain (analyze, buffers, io, format xml) select * from int8_tbl i8');
select explain_filter('explain (analyze, serialize, buffers, io, format yaml) select * from int8_tbl i8');
select explain_filter('explain (buffers, format json) select * from int8_tbl i8');
\a

-- Check expansion of window definitions

select explain_filter('explain verbose select sum(unique1) over w, sum(unique2) over (w order by hundred), sum(tenthous) over (w order by hundred) from tenk1 window w as (partition by ten)');
select explain_filter('explain verbose select sum(unique1) over w1, sum(unique2) over (w1 order by hundred), sum(tenthous) over (w1 order by hundred rows 10 preceding) from tenk1 window w1 as (partition by ten)');

-- Check output including I/O timings.  These fields are conditional
-- but always set in JSON format, so check them only in this case.
set track_io_timing = on;
select explain_filter('explain (analyze, buffers, format json) select * from int8_tbl i8');
set track_io_timing = off;

-- SETTINGS option
-- We have to ignore other settings that might be imposed by the environment,
-- so printing the whole Settings field unfortunately won't do.

begin;
set local plan_cache_mode = force_generic_plan;
select true as "OK"
  from explain_filter('explain (settings) select * from int8_tbl i8') ln
  where ln ~ '^ *Settings: .*plan_cache_mode = ''force_generic_plan''';
select explain_filter_to_json('explain (settings, format json) select * from int8_tbl i8') #> '{0,Settings,plan_cache_mode}';
rollback;

-- GENERIC_PLAN option

select explain_filter('explain (generic_plan) select unique1 from tenk1 where thousand = $1');
-- should fail
select explain_filter('explain (analyze, generic_plan) select unique1 from tenk1 where thousand = $1');

-- MEMORY option
select explain_filter('explain (memory) select * from int8_tbl i8');
select explain_filter('explain (memory, analyze, buffers off) select * from int8_tbl i8');
select explain_filter('explain (memory, summary, format yaml) select * from int8_tbl i8');
select explain_filter('explain (memory, analyze, format json) select * from int8_tbl i8');
prepare int8_query as select * from int8_tbl i8;
select explain_filter('explain (memory) execute int8_query');

-- Test EXPLAIN (GENERIC_PLAN) with partition pruning
-- partitions should be pruned at plan time, based on constants,
-- but there should be no pruning based on parameter placeholders
create table gen_part (
  key1 integer not null,
  key2 integer not null
) partition by list (key1);
create table gen_part_1
  partition of gen_part for values in (1)
  partition by range (key2);
create table gen_part_1_1
  partition of gen_part_1 for values from (1) to (2);
create table gen_part_1_2
  partition of gen_part_1 for values from (2) to (3);
create table gen_part_2
  partition of gen_part for values in (2);
-- should scan gen_part_1_1 and gen_part_1_2, but not gen_part_2
select explain_filter('explain (generic_plan) select key1, key2 from gen_part where key1 = 1 and key2 = $1');
drop table gen_part;

--
-- Test production of per-worker data
--
-- Unfortunately, because we don't know how many worker processes we'll
-- actually get (maybe none at all), we can't examine the "Workers" output
-- in any detail.  We can check that it parses correctly as JSON, and then
-- remove it from the displayed results.

begin;
-- encourage use of parallel plans
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_table_scan_size=0;
set max_parallel_workers_per_gather=4;

select jsonb_pretty(
  explain_filter_to_json('explain (analyze, verbose, buffers, format json)
                         select * from tenk1 order by tenthous')
  -- remove "Workers" node of the Seq Scan plan node
  #- '{0,Plan,Plans,0,Plans,0,Workers}'
  -- remove "Workers" node of the Sort plan node
  #- '{0,Plan,Plans,0,Workers}'
  -- Also remove its sort-type fields, as those aren't 100% stable
  #- '{0,Plan,Plans,0,Sort Method}'
  #- '{0,Plan,Plans,0,Sort Space Type}'
);

rollback;

-- Test display of temporary objects
create temp table t1(f1 float8);

create function pg_temp.mysin(float8) returns float8 language plpgsql
as 'begin return sin($1); end';

select explain_filter('explain (verbose) select * from t1 where pg_temp.mysin(f1) < 0.5');

-- Test compute_query_id
set compute_query_id = on;
select explain_filter('explain (verbose) select * from int8_tbl i8');

-- Test compute_query_id with utility statements containing plannable query
select explain_filter('explain (verbose) declare test_cur cursor for select * from int8_tbl');
select explain_filter('explain (verbose) create table test_ctas as select 1');

-- Test SERIALIZE option
select explain_filter('explain (analyze,buffers off,serialize) select * from int8_tbl i8');
select explain_filter('explain (analyze,serialize text,buffers,timing off) select * from int8_tbl i8');
select explain_filter('explain (analyze,serialize binary,buffers,timing) select * from int8_tbl i8');
-- this tests an edge case where we have no data to return
select explain_filter('explain (analyze,buffers off,serialize) create temp table explain_temp as select * from int8_tbl i8');

-- Test tuplestore storage usage in Window aggregate (memory case)
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over() from generate_series(1,10) a(n)');
-- Test tuplestore storage usage in Window aggregate (disk case)
set work_mem to 64;
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over() from generate_series(1,2500) a(n)');
-- Test tuplestore storage usage in Window aggregate (memory and disk case, final result is disk)
select explain_filter('explain (analyze,buffers off,costs off) select sum(n) over(partition by m) from (SELECT n < 3 as m, n from generate_series(1,2500) a(n))');
reset work_mem;

-- EXPLAIN (ANALYZE, BUFFERS) should report buffer usage from PL/pgSQL
-- EXCEPTION blocks, even after subtransaction rollback.
CREATE TEMP TABLE explain_exc_tab (a int, b char(20));
INSERT INTO explain_exc_tab VALUES (0, 'zzz');

CREATE FUNCTION explain_exc_func() RETURNS void AS $$
DECLARE
    v int;
BEGIN
    WITH ins AS (INSERT INTO explain_exc_tab VALUES (1, 'aaa') RETURNING a)
    SELECT a / 0 INTO v FROM ins;
EXCEPTION WHEN division_by_zero THEN
    NULL;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION check_explain_exception_buffers() RETURNS boolean AS $$
DECLARE
    plan_json json;
    node json;
    total_buffers int;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT explain_exc_func()' INTO plan_json;
    node := plan_json->0->'Plan';
    total_buffers :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);
    RETURN total_buffers > 0;
END;
$$ LANGUAGE plpgsql;

SELECT check_explain_exception_buffers() AS exception_buffers_visible;

-- Also test with nested EXPLAIN ANALYZE (two levels of instrumentation)
CREATE FUNCTION check_explain_exception_buffers_nested() RETURNS boolean AS $$
DECLARE
    plan_json json;
    node json;
    total_buffers int;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT check_explain_exception_buffers()' INTO plan_json;
    node := plan_json->0->'Plan';
    total_buffers :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);
    RETURN total_buffers > 0;
END;
$$ LANGUAGE plpgsql;

SELECT check_explain_exception_buffers_nested() AS exception_buffers_nested_visible;

DROP FUNCTION check_explain_exception_buffers_nested;
DROP FUNCTION check_explain_exception_buffers;
DROP FUNCTION explain_exc_func;
DROP TABLE explain_exc_tab;

-- Cursor instrumentation test.
-- Verify that buffer usage is correctly tracked through cursor execution paths.
-- Non-scrollable cursors exercise ExecShutdownNode after each ExecutorRun
-- (EXEC_FLAG_BACKWARD is not set), while scrollable cursors only shut down
-- nodes in ExecutorFinish. In both cases, buffer usage from the inner cursor
-- scan should be correctly reported.

CREATE TEMP TABLE cursor_buf_test AS SELECT * FROM tenk1;

CREATE FUNCTION cursor_noscroll_scan() RETURNS bigint AS $$
DECLARE
    cur NO SCROLL CURSOR FOR SELECT * FROM cursor_buf_test;
    rec RECORD;
    cnt bigint := 0;
BEGIN
    OPEN cur;
    LOOP
        FETCH NEXT FROM cur INTO rec;
        EXIT WHEN NOT FOUND;
        cnt := cnt + 1;
    END LOOP;
    CLOSE cur;
    RETURN cnt;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION cursor_scroll_scan() RETURNS bigint AS $$
DECLARE
    cur SCROLL CURSOR FOR SELECT * FROM cursor_buf_test;
    rec RECORD;
    cnt bigint := 0;
BEGIN
    OPEN cur;
    LOOP
        FETCH NEXT FROM cur INTO rec;
        EXIT WHEN NOT FOUND;
        cnt := cnt + 1;
    END LOOP;
    CLOSE cur;
    RETURN cnt;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION check_cursor_explain_buffers() RETURNS TABLE(noscroll_ok boolean, scroll_ok boolean) AS $$
DECLARE
    plan_json json;
    node json;
    direct_buf int;
    noscroll_buf int;
    scroll_buf int;
BEGIN
    -- Direct scan: get leaf Seq Scan node buffers as baseline
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT * FROM cursor_buf_test' INTO plan_json;
    node := plan_json->0->'Plan';
    WHILE node->'Plans' IS NOT NULL LOOP
        node := node->'Plans'->0;
    END LOOP;
    direct_buf :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);

    -- Non-scrollable cursor path: ExecShutdownNode runs after each ExecutorRun
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT cursor_noscroll_scan()' INTO plan_json;
    node := plan_json->0->'Plan';
    noscroll_buf :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);

    -- Scrollable cursor path: ExecShutdownNode is skipped
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        SELECT cursor_scroll_scan()' INTO plan_json;
    node := plan_json->0->'Plan';
    scroll_buf :=
        COALESCE((node->>'Local Hit Blocks')::int, 0) +
        COALESCE((node->>'Local Read Blocks')::int, 0);

    -- Both cursor paths should report buffer counts about as high as
    -- the direct scan (same data plus minor catalog overhead), and not
    -- double-counted (< 2x the direct scan)
    RETURN QUERY SELECT
        (noscroll_buf >= direct_buf * 0.5 AND noscroll_buf < direct_buf * 2),
        (scroll_buf >= direct_buf * 0.5 AND scroll_buf < direct_buf * 2);
END;
$$ LANGUAGE plpgsql;

SELECT * FROM check_cursor_explain_buffers();

DROP FUNCTION check_cursor_explain_buffers;
DROP FUNCTION cursor_noscroll_scan;
DROP FUNCTION cursor_scroll_scan;
DROP TABLE cursor_buf_test;

-- Test trigger instrumentation.
CREATE TEMP TABLE trig_test_tab (a int);
CREATE TEMP TABLE trig_work_tab (a int);
INSERT INTO trig_work_tab VALUES (1);

CREATE FUNCTION trig_test_func() RETURNS trigger AS $$
BEGIN
    PERFORM * FROM trig_work_tab;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE TRIGGER trig_test_trig
    BEFORE INSERT ON trig_test_tab
    FOR EACH ROW EXECUTE FUNCTION trig_test_func();

CREATE FUNCTION check_trigger_explain_buffers() RETURNS boolean AS $$
DECLARE
    plan_json json;
    trig json;
BEGIN
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, FORMAT JSON)
        INSERT INTO trig_test_tab VALUES (1)' INTO plan_json;
    trig := plan_json->0->'Triggers'->0;
    RETURN COALESCE((trig->>'Calls')::int, 0) > 0;
END;
$$ LANGUAGE plpgsql;

SELECT check_trigger_explain_buffers() AS trigger_buffers_visible;

DROP FUNCTION check_trigger_explain_buffers;
DROP TRIGGER trig_test_trig ON trig_test_tab;
DROP FUNCTION trig_test_func;
DROP TABLE trig_test_tab;
DROP TABLE trig_work_tab;
